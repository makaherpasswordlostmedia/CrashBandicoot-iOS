using Foundation;
using UIKit;

namespace CrashBandicoot.IosHost;

[Register("AppDelegate")]
public sealed class AppDelegate : UIApplicationDelegate
{
    public override UIWindow? Window { get; set; }

    public override bool FinishedLaunching(UIApplication app, NSDictionary options)
    {
        InstallCrashCheckpoint();
        DiskLog.Log("AppDelegate.FinishedLaunching: enter");

        Window = new UIWindow(UIScreen.MainScreen.Bounds)
        {
            RootViewController = new GameViewController(),
        };
        Window.MakeKeyAndVisible();
        DiskLog.Log("AppDelegate.FinishedLaunching: window made key and visible");
        return true;
    }

    // The app lifecycle transitions below (background/foreground/resign/
    // activate) are exactly when iOS can invalidate GPU/audio resources out
    // from under the app (e.g. the EAGL context, or the audio session route)
    // - a common source of crashes that have nothing to do with steady-state
    // gameplay. None of these were logged before, so if a field crash
    // report's last checkpoint.log line is one of these, that immediately
    // rules in/out "something during a background/foreground transition" as
    // the trigger, separate from anything happening mid-frame in Present().
    public override void DidEnterBackground(UIApplication application) =>
        DiskLog.Log("AppDelegate.DidEnterBackground");

    public override void WillEnterForeground(UIApplication application) =>
        DiskLog.Log("AppDelegate.WillEnterForeground");

    public override void OnResignActivation(UIApplication application) =>
        DiskLog.Log("AppDelegate.OnResignActivation");

    public override void OnActivated(UIApplication application) =>
        DiskLog.Log("AppDelegate.OnActivated");

    public override void WillTerminate(UIApplication application) =>
        DiskLog.Log("AppDelegate.WillTerminate");

    public override void ReceiveMemoryWarning(UIApplication application) =>
        DiskLog.Log("AppDelegate.ReceiveMemoryWarning: *** low memory ***");

    /// <summary>
    /// Catches unhandled .NET exceptions and native (Obj-C/Mono) crashes
    /// that happen before or outside GameViewController's own try/catch,
    /// and appends them to the same Documents/checkpoint.log via DiskLog
    /// (raw POSIX I/O - see DiskLog.cs for why not any managed logging
    /// API). Without this, a crash during app launch itself (before
    /// GameViewController.ViewDidLoad even runs) is completely silent -
    /// exactly the kind of failure that burns iterations without any
    /// information to act on.
    /// </summary>
    static void InstallCrashCheckpoint()
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            DiskLog.Log($"[FATAL] AppDomain.UnhandledException: {e.ExceptionObject}");

        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            DiskLog.Log($"[FATAL] UnobservedTaskException: {e.Exception}");
            e.SetObserved();
        };

        // AppDomain.UnhandledException only catches exceptions thrown from
        // managed code. It does NOT catch a raw abort() coming from a
        // native/objc-trampoline call (e.g. a managed override calling into
        // a UIKit base method through the binding layer) - which is exactly
        // what field crash reports show happening here (checkpoint.log
        // stops mid-ViewDidLoad, right at base.ViewDidLoad()). Installing an
        // NSUncaughtExceptionHandler catches genuine ObjC-level exceptions
        // that reach that layer, giving at least a checkpoint line before
        // the process dies in cases where it IS a catchable ObjC exception
        // rather than a hard trap/assert (which no handler can intercept -
        // that class of failure needs the MtouchUseLlvm=false fix instead).
        ObjCRuntime.Runtime.MarshalManagedException += (_, args) =>
            DiskLog.Log($"[FATAL] MarshalManagedException: {args.Exception}");
    }
}

public static class MainClass
{
    static void Main(string[] args)
    {
        UIApplication.Main(args, null, "AppDelegate");
    }
}
