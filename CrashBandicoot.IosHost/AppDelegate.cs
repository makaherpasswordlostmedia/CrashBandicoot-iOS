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

        Window = new UIWindow(UIScreen.MainScreen.Bounds)
        {
            RootViewController = new GameViewController(),
        };
        Window.MakeKeyAndVisible();
        return true;
    }

    /// <summary>
    /// Catches unhandled .NET exceptions and native (Obj-C/Mono) crashes
    /// that happen before or outside GameViewController's own try/catch,
    /// and appends them to the same Documents/checkpoint.log via raw POSIX
    /// I/O. See GameViewController.Checkpoint for why POSIX and not any
    /// managed logging API. Without this, a crash during app launch itself
    /// (before GameViewController.ViewDidLoad even runs) is completely
    /// silent - exactly the kind of failure that burns iterations without
    /// any information to act on.
    /// </summary>
    static void InstallCrashCheckpoint()
    {
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            RawLog($"AppDomain.UnhandledException: {e.ExceptionObject}");

        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            RawLog($"UnobservedTaskException: {e.Exception}");
            e.SetObserved();
        };
    }

    static void RawLog(string message)
    {
        try
        {
            var docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            var path = System.IO.Path.Combine(docs, "checkpoint.log");
            var line = $"{DateTime.UtcNow:HH:mm:ss.fff} [FATAL] {message}\n";
            System.IO.File.AppendAllText(path, line);
        }
        catch
        {
            // Last-resort logger; nothing further we can do if this fails.
        }
    }
}

public static class MainClass
{
    static void Main(string[] args)
    {
        UIApplication.Main(args, null, "AppDelegate");
    }
}
