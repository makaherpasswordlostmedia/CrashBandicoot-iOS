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
    volatile IosPlatformHost? _host;
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
            Text = "debug: waiting for first frame…\n" + DiskLog.Status,
            BackgroundColor = UIColor.Black.ColorWithAlpha(0.5f),
        };
        _debugOverlay.Hidden = !DiskLog.Enabled;
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
        StartGameThreadIfNeeded();
    }

    void StartGameThreadIfNeeded()
    {
        if (_gameThread != null || View == null) return; // already started

        var scale = UIScreen.MainScreen.Scale;
        int pxWidth = (int)(View.Bounds.Width * scale);
        int pxHeight = (int)(View.Bounds.Height * scale);
        Checkpoint($"StartGameThread: view {View.Bounds.Width}x{View.Bounds.Height} pt, scale {scale}, {pxWidth}x{pxHeight} px");

        _gameThread = new Thread(() => RunGame(pxWidth, pxHeight))
        {
            IsBackground = true,
            Name = "crash-game-main",
        };
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
        if (DiskLog.Enabled)
        {
            RecompOne.Runtime.Log.Sink = DiskLog.Log;
            RecompOne.Runtime.Log.GpuOn = true;
            RecompOne.Runtime.Log.CdOn = true;
            RecompOne.Runtime.Log.SdkOn = true;
        }
        else
        {
            // Performance mode: every category log off, and any stray
            // Console.WriteLine in the runtime goes nowhere instead of
            // through the (slow) stdout pipe.
            RecompOne.Runtime.Log.Sink = null;
            RecompOne.Runtime.Log.GpuOn = false;
            RecompOne.Runtime.Log.CdOn = false;
            RecompOne.Runtime.Log.SdkOn = false;
            RecompOne.Runtime.Log.OverlayOn = false;
            RecompOne.Runtime.Log.OverlayOnDefaultTrue = false;
            try
            {
                Console.SetOut(System.IO.TextWriter.Null);
                Console.SetError(System.IO.TextWriter.Null);
            }
            catch { /* not fatal */ }
        }

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
                // iPhone 8 (A11) cannot sustain the runtime's default 4x
                // internal resolution - it is the biggest single source of
                // lag. Default to native 1x unless the user has explicitly
                // chosen a value in settings.json.
                var viewCfg = RecompOne.Runtime.Config.ConfigManager.View;
                if (!viewCfg.Values.ContainsKey("InternalResolution"))
                    viewCfg.InternalResolution = 1;
                ApplyRuntimeGraphicsSettings();
                Checkpoint("RunGame: ConfigManager.Load + ApplyRuntimeGraphicsSettings done");
            }
            else
            {
                Checkpoint("RunGame: WARNING could not resolve Documents dir, AppPaths.Root left at bundle default (read-only)");
            }

            // Enable the runtime's own SessionLog now that AppPaths points at
            // a writable directory. It was compiled-in but OFF, which made
            // SessionLog.Exception/Info silent no-ops (exceptions caught
            // inside the runtime never reached any log).
            try
            {
                RecompOne.Runtime.Diagnostics.SessionLog.Enabled = DiskLog.Enabled;
                if (DiskLog.Enabled)
                {
                    RecompOne.Runtime.Diagnostics.SessionLog.Start("iOS host");
                    Checkpoint($"RunGame: SessionLog -> {RecompOne.Runtime.Diagnostics.SessionLog.CurrentPath ?? "(failed to open)"}");
                }
            }
            catch (Exception logEx) { Checkpoint($"RunGame: SessionLog.Start FAILED: {logEx.Message}"); }

            // Check for the disc BEFORE spending time on GL setup, and log
            // exactly what is in Documents - "no disc found" and "disc found
            // but wrong name/location" look identical from the outside.
            var cuePath = LocateDiscCue();
            if (cuePath == null)
            {
                Checkpoint("RunGame: no .cue found in Documents");
                SetStatus("No disc image found in Documents. Add a .cue/.bin pair via Files.app, then reopen the app.", visible: true);
                _gameThread = null;
                return;
            }
            Checkpoint($"RunGame: disc found at {cuePath}");

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
                string Str(Silk.NET.OpenGL.StringName n) =>
                    System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)gl.GetString(n)) ?? "?";
                Checkpoint($"RunGame: GL_VENDOR={Str(Silk.NET.OpenGL.StringName.Vendor)} GL_RENDERER={Str(Silk.NET.OpenGL.StringName.Renderer)} " +
                           $"GL_VERSION={Str(Silk.NET.OpenGL.StringName.Version)} GLSL={Str(Silk.NET.OpenGL.StringName.ShadingLanguageVersion)}");
            }
            Checkpoint($"RunGame: GL extensions ({glExtensions.Length} chars): {glExtensions}");
            var fetchPath = glExtensions.Contains("GL_EXT_shader_framebuffer_fetch", StringComparison.Ordinal)
                ? RecompOne.Runtime.Hle.GlesFramebufferFetchPath.Ext
                : glExtensions.Contains("GL_ARM_shader_framebuffer_fetch", StringComparison.Ordinal)
                    ? RecompOne.Runtime.Hle.GlesFramebufferFetchPath.Arm
                    : RecompOne.Runtime.Hle.GlesFramebufferFetchPath.None;
            Checkpoint($"RunGame: GLES framebuffer fetch path = {fetchPath}");

            // Same order as AndroidRuntimeHost.MainActivity.RunGameCore: the
            // internal resolution must be set BEFORE the backend allocates
            // its VRAM textures.
            RecompOne.Runtime.Hle.GlVram.Scale = RecompOne.Runtime.Config.ConfigManager.View.InternalResolution;
            Checkpoint($"RunGame: GlVram.Scale = {RecompOne.Runtime.Hle.GlVram.Scale}");
            var backend = new GlBackend(gl);
            backend.InitGl(gles: true, framebufferFetch: fetchPath);
            Checkpoint($"RunGame: GlBackend.InitGl done, Ready={backend.Ready}");
            if (!backend.Ready)
                throw new InvalidOperationException($"GlBackend failed to initialize over EAGL: {backend.LastDiagnostic}");

            // ROOT CAUSE OF THE PERMANENT BLACK SCREEN (begin=0 in checkpoint.log):
            // Gpu.HleOn is `GpuHle.Active && GpuHle.Backend is { Ready: true }`,
            // and the iOS host never set either. Every GP0 draw / VRAM load /
            // fill therefore went to the CPU software rasteriser and the GL
            // backend never saw a single primitive, so PresentDisplay always
            // fell back to an empty VRAM texture. Android sets exactly these
            // three lines in MainActivity.RunGameCore.
            RecompOne.Runtime.Hle.GpuHle.Backend = backend;
            RecompOne.Runtime.Hle.GpuHle.Active = true;
            RecompOne.Runtime.Hle.GpuHle.NativeResolution =
                RecompOne.Runtime.Config.ConfigManager.View.InternalResolution <= 1;
            Checkpoint($"RunGame: GpuHle.Active={RecompOne.Runtime.Hle.GpuHle.Active} Backend.Ready={backend.Ready} NativeResolution={RecompOne.Runtime.Hle.GpuHle.NativeResolution}");

            var diagnostics = new IosGpuDiagnosticsSession();
            var host = new IosPlatformHost(this, _egl, backend, diagnostics);
            _host = host;
            Runtime.SetPlatformHost(host);
            Checkpoint("RunGame: platform host attached");

            Checkpoint($"RunGame: disc found at {cuePath}, calling Recompiled.Entry.Run");
            host.Initialize("Crash Bandicoot");

            if (DiskLog.Enabled) StartStallWatchdog();

            // NOTE: no reflection, no AssemblyLoadContext - Recompiled.Entry
            // is an ordinary type statically compiled into this binary
            // (see Recompiled/ populated by scripts/prerecompile.sh before
            // `dotnet build`). This is the whole point of the iOS port:
            // everything the desktop/Android GameLoader.Run() does via
            // reflection at runtime, we do via a normal static call here,
            // because the code was already known at build time.
            Recompiled.Entry.Run(new PSMemory(), cuePath);
            Checkpoint("RunGame: Recompiled.Entry.Run returned (session ended)");
            RecompOne.Runtime.Hle.GpuHle.Active = false;
            RecompOne.Runtime.Hle.GpuHle.Backend = null;
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
            RecompOne.Runtime.Hle.GpuHle.Active = false;
            RecompOne.Runtime.Hle.GpuHle.Backend = null;
            SessionLog.Exception("GameViewController.RunGame", ex);
            SetStatus($"Crashed: {ex.Message}", visible: true);
        }
    }

    /// <summary>
    /// Runs on its own background thread and logs a "STALL" line whenever
    /// Runtime.PresentFrameCalls hasn't advanced for over a second, then
    /// again as the stall gets longer (2s, 5s, 10s, then every 10s). This
    /// exists to turn "black screen for a while, then the user gave up and
    /// closed the app" into an unambiguous answer to "is this normal
    /// loading, or is execution actually stuck": PresentFrameCalls climbing
    /// steadily but slowly means real (if slow) progress through
    /// Recompiled.Entry.Run's main loop every frame; PresentFrameCalls
    /// frozen at some value while wall-clock time keeps advancing means
    /// gameplay code has genuinely stopped calling PresentFrame at all -
    /// almost certainly parked in a BIOS syscall busy-wait (the same shape
    /// of bug the CdController GetlocL/unhandled-cmd fixes addressed, just
    /// potentially in a different subsystem - GPU or SPU BIOS calls are the
    /// next most likely candidates, see Bios/BiosA.cs, BiosB.cs, BiosC.cs).
    /// A fixed, low-overhead 1s poll is cheap enough to leave running for
    /// the whole session and never needs its own separate on/off toggle.
    /// </summary>
    static void StartStallWatchdog()
    {
        var thread = new Thread(() =>
        {
            long lastSeenFrame = -1;
            long stallStartTicks = 0;
            int nextReportSeconds = 1;
            while (true)
            {
                Thread.Sleep(1000);
                long frame = RecompOne.Runtime.Runtime.PresentFrameCalls;
                long now = System.Diagnostics.Stopwatch.GetTimestamp();
                if (frame != lastSeenFrame)
                {
                    lastSeenFrame = frame;
                    stallStartTicks = now;
                    nextReportSeconds = 1;
                    continue;
                }
                double stalledSeconds = (now - stallStartTicks) / (double)System.Diagnostics.Stopwatch.Frequency;
                if (stalledSeconds < nextReportSeconds) continue;
                DiskLog.Log($"[STALL] PresentFrameCalls stuck at {frame} for {stalledSeconds:F0}s - " +
                            "gameplay code has not called PresentFrame in this long, almost certainly " +
                            "parked in a BIOS syscall busy-wait rather than slow-but-progressing loading.");
                nextReportSeconds = nextReportSeconds switch
                {
                    1 => 2,
                    2 => 5,
                    5 => 10,
                    _ => nextReportSeconds + 10,
                };
            }
            // ReSharper disable once FunctionNeverReturns - deliberate:
            // lives for the process lifetime, same as _gameThread itself.
        })
        { IsBackground = true, Name = "stall-watchdog" };
        thread.Start();
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
            .GetUrls(NSSearchPathDirectory.DocumentDirectory, NSSearchPathDomain.User)
            .FirstOrDefault()?.Path;
        if (string.IsNullOrEmpty(docs))
        {
            DiskLog.Log("LocateDiscCue: Documents directory could not be resolved");
            return null;
        }

        if (DiskLog.Enabled)
        try
        {
            var entries = Directory.GetFileSystemEntries(docs);
            DiskLog.Log($"LocateDiscCue: {docs} contains {entries.Length} entries:");
            foreach (var e in entries.Take(40))
            {
                long size = File.Exists(e) ? new FileInfo(e).Length : -1;
                DiskLog.Log($"LocateDiscCue:   {System.IO.Path.GetFileName(e)}{(size >= 0 ? $" ({size} bytes)" : "/")}");
            }
        }
        catch (Exception ex)
        {
            DiskLog.Log($"LocateDiscCue: listing {docs} FAILED: {ex.Message}");
        }

        // Case-insensitive: Files.app / iTunes can preserve ".CUE".
        var cue = Directory.GetFiles(docs)
            .FirstOrDefault(f => f.EndsWith(".cue", StringComparison.OrdinalIgnoreCase));
        if (cue == null) return null;

        // The .cue must reference a .bin that actually exists next to it,
        // otherwise the run dies deep inside CueBin with an opaque error.
        try
        {
            foreach (var line in File.ReadAllLines(cue))
            {
                var t = line.Trim();
                if (!t.StartsWith("FILE", StringComparison.OrdinalIgnoreCase)) continue;
                var a = t.IndexOf('"'); var b = t.LastIndexOf('"');
                if (a < 0 || b <= a) continue;
                var bin = t.Substring(a + 1, b - a - 1);
                var binPath = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(cue)!, bin);
                DiskLog.Log($"LocateDiscCue: cue references '{bin}' -> {(File.Exists(binPath) ? "exists" : "MISSING (fix the FILE line in the .cue)")}");
            }
        }
        catch (Exception ex)
        {
            DiskLog.Log($"LocateDiscCue: reading cue FAILED: {ex.Message}");
        }
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
        if (!DiskLog.Enabled) return;
        InvokeOnMainThread(() =>
        {
            if (_debugOverlay != null) _debugOverlay.Text = text;
        });
    }

    /// <summary>
    /// Called from AppDelegate.DidEnterBackground. Sets Suspended
    /// immediately (main thread) so the render thread's very next
    /// Present() call - which can land at any point, including mid-frame,
    /// since crash-game-main keeps running independently of app lifecycle
    /// events - stops touching the EAGL surface before iOS gets a chance
    /// to invalidate it out from under an in-flight GL call.
    /// </summary>
    public void OnEnteredBackground()
    {
        if (_host != null) _host.Suspended = true;
        Checkpoint("GameViewController.OnEnteredBackground: host.Suspended=true");
    }

    /// <summary>
    /// Called from AppDelegate.WillEnterForeground. Forces a full
    /// framebuffer/renderbuffer rebuild (same size or not - see
    /// IosEglContext.RecreateSurfaceAfterForeground's doc comment for why
    /// the normal size-comparison early-out in SetExpectedSize can't be
    /// relied on here) before clearing Suspended, so the render thread's
    /// next Present() draws into a fresh, valid surface instead of
    /// whatever iOS left behind after backgrounding.
    ///
    /// Reads View.Bounds fresh here (main thread, right now) instead of
    /// letting IosEglContext reuse its own cached SurfaceWidth/Height -
    /// those reflect the size from BEFORE backgrounding, which is wrong
    /// if a rotation, Split View change, or Stage Manager resize happened
    /// while suspended. Falls back to (0,0) - meaning "reuse whatever
    /// IosEglContext already has" - only if View is somehow gone, which
    /// shouldn't normally happen for a foregrounding root view controller.
    /// </summary>
    public void OnWillEnterForeground()
    {
        // RunGame bails out early (and clears _gameThread) when no disc is
        // present yet; coming back from Files.app after copying it in is the
        // natural moment to retry.
        if (_gameThread == null) StartGameThreadIfNeeded();
        Checkpoint("GameViewController.OnWillEnterForeground: rebuilding surface");
        var egl = _egl;
        if (egl != null)
        {
            int width = 0, height = 0;
            if (View != null)
            {
                var scale = UIScreen.MainScreen.Scale;
                width = (int)(View.Bounds.Width * scale);
                height = (int)(View.Bounds.Height * scale);
            }
            egl.RecreateSurfaceAfterForeground(width, height);
        }
        if (_host != null) _host.Suspended = false;
        Checkpoint("GameViewController.OnWillEnterForeground: host.Suspended=false");
    }
}
