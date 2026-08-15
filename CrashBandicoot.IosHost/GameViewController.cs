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
    IosEglContext? _egl;
    Thread? _gameThread;

    /// <summary>
    /// Appends a line to Documents/checkpoint.log via raw POSIX
    /// open/write/close, deliberately bypassing every managed I/O and
    /// logging path (File.AppendAllText, NSLog, etc). The point: if the
    /// Mono runtime, GC, or any managed subsystem is what's crashing, a
    /// managed logger crashes right along with it and you're back to
    /// silent failures. A raw POSIX write from a P/Invoke has the fewest
    /// possible moving parts between "something went wrong" and "there is
    /// a line about it in a file you can pull off the device". This is the
    /// single highest-leverage lesson carried over from a previous
    /// (non-.NET) iOS port that took ~70 iterations to get building:
    /// unexplained crashes with no log cost far more time than any
    /// individual compile error does.
    /// </summary>
    static void Checkpoint(string stage)
    {
        try
        {
            var docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            var path = System.IO.Path.Combine(docs, "checkpoint.log");
            var line = $"{DateTime.UtcNow:HH:mm:ss.fff} {stage}\n";
            var bytes = System.Text.Encoding.UTF8.GetBytes(line);
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0x1A4 /* 0644 */);
            if (fd < 0) return;
            unsafe
            {
                fixed (byte* p = bytes)
                    write(fd, (IntPtr)p, (UIntPtr)bytes.Length);
            }
            close(fd);
        }
        catch
        {
            // Checkpointing must never itself be the thing that crashes.
        }
    }

    const int O_WRONLY = 0x0001;
    const int O_CREAT = 0x0200;
    const int O_APPEND = 0x0008;

    [System.Runtime.InteropServices.DllImport("libc", SetLastError = true)]
    static extern int open(string path, int flags, int mode);
    [System.Runtime.InteropServices.DllImport("libc")]
    static extern IntPtr write(int fd, IntPtr buf, UIntPtr count);
    [System.Runtime.InteropServices.DllImport("libc")]
    static extern int close(int fd);

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
        if (_egl != null && View != null)
        {
            var scale = UIScreen.MainScreen.Scale;
            _egl.SetExpectedSize((int)(View.Bounds.Width * scale), (int)(View.Bounds.Height * scale));
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
            // thread. The code here previously called
            // _egl.MakeCurrentOnCallingThread() and started issuing GL calls
            // right after this line, assuming Initialize() had already run
            // on the main thread by then. It hadn't, most of the time - this
            // was a race between Initialize() (main thread) and everything
            // below (render thread) that reads/writes the same IosEglContext
            // fields (_context, _layer, the framebuffer). That race, not the
            // resize/SwapBuffers path, was the actual cause of the abort():
            // the crash stack landed at different depths across runs because
            // a data race's exact failure point is non-deterministic, but it
            // reproduced on effectively every launch because this code path
            // runs unconditionally at startup, with no resize needed to
            // trigger it. A ManualResetEventSlim makes RunGame actually wait
            // for the main-thread Initialize() to finish before touching
            // _egl from this thread at all.
            using var eglReady = new ManualResetEventSlim(false);
            Exception? eglInitError = null;
            InvokeOnMainThread(() =>
            {
                try { _egl.Initialize(View!, width, height); }
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
