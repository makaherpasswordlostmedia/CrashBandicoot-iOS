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
    UILabel? _debugOverlay;
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

        // Always-visible debug overlay, independent of _statusLabel (which
        // is hidden after the first Present() call - see SetStatus/
        // "first frame complete, hiding status"). The point is to answer,
        // just by looking at the screen with no log pull required, "is the
        // render loop alive and drawing frames with content, or stuck/
        // crashed?" - a black screen with a ticking frame counter and
        // hadRt=true means the game is genuinely rendering black content;
        // a frozen counter or hadRt=false the whole time points at a real
        // problem instead.
        _debugOverlay = new UILabel(new CGRect(4, 20, View.Bounds.Width - 8, 60))
        {
            TextColor = UIColor.Green,
            Font = UIFont.SystemFontOfSize(11),
            Lines = 3,
            Text = "debug: waiting for first frame…",
            BackgroundColor = UIColor.Black.ColorWithAlpha(0.5f),
        };
        View.AddSubview(_debugOverlay);
        Checkpoint("ViewDidLoad: debug overlay added");
    }

    public override void ViewDidLayoutSubviews()
    {
        base.ViewDidLayoutSubviews();
        if (_statusLabel != null) _statusLabel.Frame = View!.Bounds;
        if (_spinner != null) _spinner.Center = View!.Center;
        if (_touchView != null) _touchView.Frame = View!.Bounds;
        if (_debugOverlay != null) _debugOverlay.Frame = new CGRect(4, 20, View!.Bounds.Width - 8, 60);
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

        // DIAGNOSTIC: route RecompOne.Runtime.Log category logs (normally
        // discarded - see Log.Sink's own doc comment, Console.WriteLine is
        // invisible on a TrollStore-installed binary) into the same
        // checkpoint.log used everywhere else, and turn on the Gpu/Cd
        // categories specifically. This targets the observed symptom: CD
        // sectors keep getting read (sectorsRead climbs into the
        // thousands) but GlBackend.BeginCalls never leaves 0, i.e. no GP0
        // draw command is ever issued - these logs should show whether
        // WriteGp0 is ever reached at all, and what DriveStatus() bits are
        // actually being reported back to the game on every CD command.
        RecompOne.Runtime.Log.Sink = DiskLog.Log;
        RecompOne.Runtime.Log.GpuOn = true;
        RecompOne.Runtime.Log.CdOn = true;

        try
        {
            // AppPaths.Root defaults to Environment.ProcessPath's directory
            // (see AppPaths.SetRoot's own doc comment: "Android hosts must
            // call this before the runtime is initialized because the APK
            // install directory is read-only"). The exact same constraint
            // applies here - on iOS, ProcessPath/AppContext.BaseDirectory
            // point inside the app bundle, which is code-signed and
            // read-only after install. Any static state under
            // RecompOne.Runtime.Runtime that touches AppPaths at class-init
            // time (e.g. the MemoryCard fields opening/creating
            // AppPaths.CardAPath) throws there, which surfaces here as an
            // opaque TypeInitializationException the very first time
            // `Runtime` is touched. Point AppPaths at the app's actual
            // writable Documents directory - the same NSSearchPathDirectory
            // location LocateDiscCue() below already uses for the .cue/.bin
            // pair - before anything reaches RecompOne.Runtime.Runtime,
            // mirroring what AndroidRuntimeHost.MainActivity already does
            // with FilesDir before its own runtime init.
            var docsDir = NSFileManager.DefaultManager
                .GetUrls(NSSearchPathDirectory.DocumentDirectory, NSSearchPathDomain.User)[0]
                .Path;
            if (!string.IsNullOrEmpty(docsDir))
            {
                var dataRoot = System.IO.Path.Combine(docsDir, "runtime");
                RecompOne.Runtime.AppPaths.SetRoot(dataRoot);
                RecompOne.Runtime.AppPaths.EnsureCreated();
                Checkpoint($"RunGame: AppPaths.Root set to {dataRoot}");

                // Mirror AndroidRuntimeHost.MainActivity.StartGameAsync's
                // full pre-launch sequence, not just SetRoot/EnsureCreated -
                // ConfigManager.Load() reads settings.json/interface.ini
                // (safe no-ops on first run, since ConfigManager.Game/View
                // already default to `new()`), and
                // ApplyRuntimeGraphicsSettings mirrors ConfigManager.View
                // into RecompOne.Runtime.Hle.GpuHle's static fields
                // (TextureFilter, Dedither, PresentNearest, IntegerScale,
                // wide-aspect, etc.) that GlShaders' PrimFs (uFilterMode,
                // uDedither, ...) and GlBackend read at draw time. Skipping
                // this doesn't crash - those statics just stay at their C#
                // default values - but it silently produces a
                // visually-wrong render (no widescreen, no texture
                // filtering, no dedither) that would otherwise look like a
                // brand new, unrelated bug.
                RecompOne.Runtime.Config.ConfigManager.Load();
                ApplyRuntimeGraphicsSettings();
                Checkpoint("RunGame: ConfigManager.Load + ApplyRuntimeGraphicsSettings done");
            }
            else
            {
                Checkpoint("RunGame: WARNING could not resolve Documents dir, AppPaths.Root left at bundle default (read-only)");
            }

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

            // GlShaders.AdaptSource has three ways to express translucent
            // blending on GLES: EXT_shader_framebuffer_fetch,
            // ARM_shader_framebuffer_fetch, or - if neither is available -
            // dual-source blending via "#extension GL_EXT_blend_func_extended
            // : require". That third path is desktop/Android-GPU territory;
            // Apple's GPUs (via EAGL/Metal) do not expose
            // GL_EXT_blend_func_extended, so leaving framebufferFetch at its
            // GlesFramebufferFetchPath.None default here would compile
            // PrimFs against a "require" on an extension the driver doesn't
            // have - a second, separate compile failure right behind the
            // "#version 320 es" one that was already fixed. Apple's
            // tile-based GPUs do support EXT_shader_framebuffer_fetch (the
            // same extension AndroidGlesInfo already prefers first on
            // Android), so probe for it here the same way and request that
            // path explicitly instead of taking the None default.
            string glExtensions;
            unsafe
            {
                glExtensions = System.Runtime.InteropServices.Marshal.PtrToStringAnsi(
                    (nint)gl.GetString(Silk.NET.OpenGL.StringName.Extensions)) ?? string.Empty;
            }
            var fetchPath = glExtensions.Contains("GL_EXT_shader_framebuffer_fetch", StringComparison.Ordinal)
                ? RecompOne.Runtime.Hle.GlesFramebufferFetchPath.Ext
                : glExtensions.Contains("GL_ARM_shader_framebuffer_fetch", StringComparison.Ordinal)
                    ? RecompOne.Runtime.Hle.GlesFramebufferFetchPath.Arm
                    : RecompOne.Runtime.Hle.GlesFramebufferFetchPath.None;
            Checkpoint($"RunGame: GLES framebuffer fetch path = {fetchPath}");

            var backend = new GlBackend(gl);
            backend.InitGl(gles: true, framebufferFetch: fetchPath);
            Checkpoint($"RunGame: GlBackend.InitGl done, Ready={backend.Ready}");
            if (!backend.Ready)
                throw new InvalidOperationException($"GlBackend failed to initialize over EAGL: {backend.LastDiagnostic}");

            var diagnostics = new IosGpuDiagnosticsSession();
            var host = new IosPlatformHost(this, _egl, backend, diagnostics);
            Runtime.SetPlatformHost(host);
            Checkpoint("RunGame: platform host attached");

            var cuePath = LocateDiscCue();
            if (cuePath == null)
            {
                Checkpoint("RunGame: no .cue found in Documents");
                SetStatus("No disc image found in Documents. Add a .cue/.bin pair via Files.app, then tap to retry.", visible: true);
                // Allow ViewDidAppear to spin up a fresh game thread on the
                // next attempt (e.g. user adds the file and re-triggers via
                // foregrounding) instead of leaving _gameThread permanently
                // non-null with no way to retry without relaunching the app.
                _gameThread = null;
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
            // TypeInitializationException (and similar wrapper exceptions)
            // put the actually useful information in InnerException, not
            // Message - ex.Message alone was previously just the opaque
            // resource key "TypeInitialization_Type" with no indication of
            // which static field failed or why. Walk the chain so the real
            // cause (e.g. a static field's constructor failing to write to
            // a read-only path) actually reaches checkpoint.log.
            var chain = new System.Text.StringBuilder();
            for (var cur = ex; cur != null; cur = cur.InnerException)
                chain.Append($"{cur.GetType().Name}: {cur.Message}\n");
            Checkpoint($"RunGame: EXCEPTION chain:\n{chain}{ex.StackTrace}");
            SessionLog.Exception("GameViewController.RunGame", ex);
            SetStatus($"Crashed: {ex.Message}", visible: true);
        }
    }

    /// <summary>
    /// Mirrors AndroidRuntimeHost.MainActivity.ApplyRuntimeGraphicsSettings
    /// exactly - pushes ConfigManager.View into GpuHle's static fields.
    /// Called once, right after ConfigManager.Load(), before the game
    /// thread reaches any GL work that reads these.
    /// </summary>
    static void ApplyRuntimeGraphicsSettings()
    {
        var view = RecompOne.Runtime.Config.ConfigManager.View;
        RecompOne.Runtime.Hle.GpuHle.WideAspect = view.Widescreen ? 16f / 9f : 0f;
        RecompOne.Runtime.Hle.GpuHle.TextureFilter = view.TextureFilter;
        RecompOne.Runtime.Hle.GpuHle.TextureFilterStrength = view.TextureFilterStrength;
        RecompOne.Runtime.Hle.GpuHle.Dedither = view.Dedither;
        RecompOne.Runtime.Hle.GpuHle.Dejitter = view.Dejitter;
        RecompOne.Runtime.Hle.GpuHle.PresentNearest = view.PresentNearest;
        RecompOne.Runtime.Hle.GpuHle.IntegerScale = view.IntegerScale;
        RecompOne.Runtime.Host.FrameClock.SkipThrottle = false;
        RecompOne.Runtime.Hle.GpuHle.RefreshWideFov();
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

    /// <summary>
    /// Updates the always-visible debug overlay. Uses InvokeOnMainThread
    /// like every other UIKit mutation in this class - writing UILabel.Text
    /// from a background thread is not safe on this AOT/Mono UIKit binding
    /// (this exact class of background-thread-touches-UIKit bug is what
    /// several other crashes earlier in this file's history turned out to
    /// be). Callers on the render thread (IosPlatformHost.Present) are
    /// expected to throttle how often they call this - see the frame-
    /// interval check there - rather than relying on this method to
    /// coalesce anything.
    /// </summary>
    public void UpdateDebugOverlay(string text)
    {
        InvokeOnMainThread(() =>
        {
            if (_debugOverlay != null) _debugOverlay.Text = text;
        });
    }
}
