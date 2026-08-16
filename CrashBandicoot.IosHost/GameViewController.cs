using CoreGraphics;
using Foundation;
using RecompOne.Runtime;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Memory;
using System.Threading;
using UIKit;

namespace CrashBandicoot.IosHost;

/// <summary>
/// iOS equivalent of the Android LauncherScreen/MainActivity combo, trimmed
/// to "just run the game" - the launcher's disc-picking / mod-management UI
/// (CrashBandicoot.Launcher/Ui) is a separate, larger port not attempted
/// here yet. This assumes a disc image already sitting in the app's
/// Documents folder (visible over Files.app thanks to
/// UIFileSharingEnabled/LSSupportsOpeningDocumentsInPlace in Info.plist),
/// and Recompiled/ already populated at build time by
/// scripts/prerecompile.sh + tools/CrashBandicoot.PreRecompiler.
///
/// Renders via IosEglContext (native EAGLContext/CAEAGLLayer, GLES2 - see
/// that file's doc comment for why this replaced an earlier ANGLE-based
/// approach: fewer third-party dependencies to get wrong).
/// </summary>
sealed class GameViewController : UIViewController, IStatusSink
{
    UILabel? _statusLabel;
    UIActivityIndicatorView? _spinner;
    TouchControllerView? _touchView;
    // volatile: written on crash-game-main (RunGame), read on the main
    // thread (ViewDidLayoutSubviews) with no lock between them. Without
    // this, ARM64's weak memory model does not guarantee the main thread
    // ever observes the write, or observes a fully-constructed
    // IosEglContext instance rather than a torn/partial one.
    volatile IosEglContext? _egl;
    Thread? _gameThread;

    /// <summary>Thin wrapper kept for call-site brevity; see DiskLog.cs for what this actually does.</summary>
    static void Checkpoint(string stage) => DiskLog.Log(stage);

    public override void ViewDidLoad()
    {
        Checkpoint("ViewDidLoad: enter");
        base.ViewDidLoad();
        Checkpoint("ViewDidLoad: base done");
        View!.BackgroundColor = UIColor.Black;
        Checkpoint("ViewDidLoad: bg color set");

        // No Metal/CAMetalLayer setup here: IosEglContext.Initialize(View, ...)
        // creates and attaches its own CAEAGLLayer once RunGame() starts.

        _statusLabel = new UILabel(View.Bounds)
        {
            TextColor = UIColor.White,
            TextAlignment = UITextAlignment.Center,
            Text = "Starting…",
        };
        View.AddSubview(_statusLabel);
        Checkpoint("ViewDidLoad: statusLabel added");

        _spinner = new UIActivityIndicatorView(UIActivityIndicatorViewStyle.Large)
        {
            Center = View.Center,
        };
        _spinner.StartAnimating();
        View.AddSubview(_spinner);
        Checkpoint("ViewDidLoad: spinner added");

        _touchView = new TouchControllerView(View.Bounds);
        Checkpoint("ViewDidLoad: TouchControllerView constructed");
        _touchView.ThreeFingerHold = () => InvokeOnMainThread(() =>
            SetStatus("Dev menu not ported yet (hold detected).", visible: true));
        View.AddSubview(_touchView);
        Checkpoint("ViewDidLoad: subviews attached");
    }

    public override void ViewDidLayoutSubviews()
    {
        base.ViewDidLayoutSubviews();
        if (_statusLabel != null) _statusLabel.Frame = View!.Bounds;
        if (_spinner != null) _spinner.Center = View!.Center;
        if (_touchView != null) _touchView.Frame = View!.Bounds;
        // Capture _egl into a local before the null check: this field is
        // written on crash-game-main (RunGame) and read here on the main
        // thread with no lock or volatile between them. Re-reading the
        // field a second time after the null check (the old code did
        // `_egl.SetExpectedSize(...)` as a separate statement) is a classic
        // TOCTOU - the field could still be non-null at the check and then
        // read again with no guarantee it's the same fully-published
        // reference, or SetExpectedSize could run against an _egl that
        // RunGame's catch block is simultaneously tearing down after an
        // exception. Capturing once removes the double-read; SetExpectedSize
        // itself already no-ops safely if this particular instance hasn't
        // finished Initialize() yet (_layer/_context still null there).
        var egl = _egl;
        if (egl != null && View != null)
        {
            var scale = UIScreen.MainScreen.Scale;
            egl.SetExpectedSize((int)(View.Bounds.Width * scale), (int)(View.Bounds.Height * scale));
        }
    }

    public override void ViewDidAppear(bool animated)
    {
        base.ViewDidAppear(animated);
        Checkpoint("ViewDidAppear: enter");
        if (_gameThread != null) return; // already started

        var scale = UIScreen.MainScreen.Scale;
        int pxWidth = (int)(View!.Bounds.Width * scale);
        int pxHeight = (int)(View.Bounds.Height * scale);

        _gameThread = new Thread(() => RunGame(pxWidth, pxHeight))
        {
            IsBackground = true,
            Name = "crash-game-main",
        };
        Checkpoint("ViewDidAppear: starting game thread");
        _gameThread.Start();
    }

    void RunGame(int width, int height)
    {
        Checkpoint("RunGame: enter");
        try
        {
            _egl = new IosEglContext();
            Checkpoint("RunGame: IosEglContext constructed");
            // EAGL/UIKit calls must happen on the main thread even though
            // the actual GL rendering afterward runs from this background
            // thread (this matches the standard EAGL pattern of "set up on
            // main thread, current-context-and-draw from a render thread").
            //
            // InvokeOnMainThread is ASYNCHRONOUS (dispatch_async under the
            // hood) - it queues the block and returns immediately on THIS
            // thread. The lambda below reads the `_egl` field from the main
            // thread while it was just written on THIS thread a few lines
            // up. On ARM64's weak memory model, a plain field write on one
            // thread is not guaranteed to be visible to a read on another
            // thread without an explicit memory barrier - there is nothing
            // here (no lock, no volatile, no Interlocked) forcing that
            // visibility, so the main thread could in principle still see a
            // stale/null `_egl` when the queued block runs, or - more
            // realistically given ManualResetEventSlim.Wait() below already
            // acts as a full fence once eglReady.Set() happens-before this
            // thread's eglReady.Wait() returns - see a *partially
            // constructed* IosEglContext object (the reference could become
            // visible before all of its constructor's field writes are).
            // Capturing the freshly constructed instance into a local and
            // passing that into the lambda closes this gap: the local is
            // definitely fully constructed by the time it's captured, and
            // ManualResetEventSlim's Set/Wait pair (which use Monitor
            // internally) already provides the happens-before edge back to
            // this thread for everything the lambda touches.
            var egl = _egl;
            using var eglReady = new ManualResetEventSlim(false);
            Exception? eglInitError = null;
            InvokeOnMainThread(() =>
            {
                try { egl.Initialize(View!, width, height); }
                catch (Exception ex) { eglInitError = ex; }
                finally { eglReady.Set(); }
            });
            eglReady.Wait();
            if (eglInitError != null)
                throw new InvalidOperationException("EAGL Initialize failed on main thread.", eglInitError);
            Checkpoint("RunGame: EAGL context/layer initialized");

            // The EAGLContext created above was made current on the MAIN
            // thread inside Initialize(), then explicitly released there.
            // Every GL call from this point on - Silk.NET's GL.GetApi
            // probing, GlBackend.InitGl, and the whole Present()/SwapBuffers
            // render loop - runs on THIS thread (crash-game-main), so the
            // context must be made current here too. EAGLContext is
            // thread-affine; skipping this is what previously caused a
            // native abort() on the main thread on first frame present.
            _egl.MakeCurrentOnCallingThread();
            Checkpoint("RunGame: EAGL context made current on render thread");

            var gl = Silk.NET.OpenGL.GL.GetApi(_egl);
            Checkpoint("RunGame: Silk.NET GL.GetApi resolved");
            var backend = new GlBackend(gl);
            backend.InitGl(gles: true);
            Checkpoint($"RunGame: GlBackend.InitGl done, Ready={backend.Ready}");
            if (!backend.Ready)
                throw new InvalidOperationException("GlBackend failed to initialize over EAGL.");

            var diagnostics = new IosGpuDiagnosticsSession();
            var host = new IosPlatformHost(this, _egl, backend, diagnostics);
            Runtime.SetPlatformHost(host);
            Checkpoint("RunGame: platform host attached");

            var cuePath = LocateDiscCue();
            if (cuePath == null)
            {
                Checkpoint("RunGame: no .cue found in Documents");
                SetStatus("No disc image found in Documents. Add a .cue/.bin pair via Files.app and relaunch.", visible: true);
                return;
            }

            Checkpoint($"RunGame: disc found at {cuePath}, calling Recompiled.Entry.Run");
            host.Initialize("Crash Bandicoot");

            // NOTE: no reflection, no AssemblyLoadContext - Recompiled.Entry
            // is an ordinary type statically compiled into this binary
            // (see Recompiled/ populated by scripts/prerecompile.sh before
            // `dotnet build`). This is the whole point of the iOS port:
            // everything the desktop/Android GameLoader.Run() does via
            // reflection at runtime, we do via a normal static call here,
            // because the code was already known at build time.
            Recompiled.Entry.Run(new PSMemory(), cuePath);
            Checkpoint("RunGame: Recompiled.Entry.Run returned (session ended)");
        }
        catch (Exception ex)
        {
            Checkpoint($"RunGame: EXCEPTION {ex.GetType().Name}: {ex.Message}\n{ex.StackTrace}");
            SessionLog.Exception("GameViewController.RunGame", ex);
            SetStatus($"Crashed: {ex.Message}", visible: true);
        }
    }

    static string? LocateDiscCue()
    {
        var docs = NSFileManager.DefaultManager
            .GetUrls(NSSearchPathDirectory.DocumentDirectory, NSSearchPathDomain.User)[0]
            .Path;
        if (docs == null) return null;
        var cue = Directory.GetFiles(docs, "*.cue").FirstOrDefault();
        return cue;
    }

    public void SetStatus(string text, bool visible)
    {
        InvokeOnMainThread(() =>
        {
            if (_statusLabel == null) return;
            _statusLabel.Text = text;
            _statusLabel.Hidden = !visible;
            _spinner!.Hidden = !visible;
        });
    }
}
