using RecompOne.Runtime;
using RecompOne.Runtime.Config;
using RecompOne.Runtime.Diagnostics;
using RecompOne.Runtime.Hle;
using RecompOne.Runtime.Host.Cheats;

namespace CrashBandicoot.IosHost;

/// <summary>
/// iOS port of AndroidRuntimeHost/AndroidPlatformHost.cs. Same shape,
/// same GlBackend (unmodified, reused as-is - see IosEglContext.cs for why
/// that's safe), swapped: Android Activity/TextView -> a thin delegate
/// (StatusSink) that forwards to the Swift UIViewController, and
/// AndroidAudioOutput -> IosAudioOutput (AVAudioEngine, see IosAudioOutput.cs).
/// </summary>
sealed class IosPlatformHost(
    IStatusSink status,
    IosEglContext egl,
    GlBackend backend,
    IosGpuDiagnosticsSession diagnostics) : IRuntimePlatformHost
{
    readonly IosAudioOutput _audio = new();
    bool _firstFrame = true;
    long _fpsWindow = System.Diagnostics.Stopwatch.GetTimestamp();
    int _fpsFrames;
    long _frameCounter;
    bool _lastLoggedHadRt;
    double _prepareMilliseconds;
    double _surfaceMilliseconds;
    double _swapMilliseconds;
    long _flushes;
    long _writebacks;
    long _vertices;
    long _lastPresentTimestamp;

    public static double LastFps { get; private set; }

    public void Initialize(string title)
    {
        DiskLog.Log($"IosPlatformHost.Initialize: {title}");
        status.SetStatus($"{title}: first frame incoming…", visible: true);
    }
    public void WaitForValidDisc() { }
    public void AttachAudio(Spu? spu)
    {
        DiskLog.Log("IosPlatformHost.AttachAudio");
        _audio.Attach(spu);
    }
    public void SetMasterVolume(float volume) => _audio.SetMasterVolume(volume);
    public void ShowNotice(string message)
    {
        DiskLog.Log($"IosPlatformHost.ShowNotice: {message}");
        status.SetStatus(message, visible: true);
    }

    public void PauseAudio()
    {
        DiskLog.Log("IosPlatformHost.PauseAudio: enter");
        RecompOne.Runtime.Host.FrameClock.PauseTiming();
        _audio.PauseOutput();
        DiskLog.Log("IosPlatformHost.PauseAudio: done");
    }

    public void ResumeAudio()
    {
        DiskLog.Log("IosPlatformHost.ResumeAudio: enter");
        _audio.ResumeOutput();
        RecompOne.Runtime.Host.FrameClock.ResumeTiming();
        DiskLog.Log("IosPlatformHost.ResumeAudio: done");
    }

    public void NotifySurfaceSize(int width, int height)
    {
        DiskLog.Log($"IosPlatformHost.NotifySurfaceSize: {width}x{height} (frame {_frameCounter})");
        egl.SetExpectedSize(width, height);
        DiskLog.Log($"IosPlatformHost.NotifySurfaceSize: SetExpectedSize returned (frame {_frameCounter})");
    }

    /// <summary>
    /// Exposes the same lock IosEglContext uses internally for
    /// SetExpectedSize/SwapBuffers/Initialize/Dispose, so a whole frame's
    /// worth of GL calls (PresentDisplay/PresentToDefaultFramebuffer, both
    /// straight Silk.NET GL calls that bypass IosEglContext's own locked
    /// methods) can be held atomic against a concurrent resize from
    /// ViewDidLayoutSubviews on the main thread. Locking only inside
    /// SwapBuffers was not sufficient: a resize could still land its own
    /// SetCurrentContext + framebuffer rebuild in between
    /// PresentToDefaultFramebuffer and SwapBuffers, i.e. mid-frame, pulling
    /// the context out from under the render thread's in-flight GL calls.
    /// </summary>
    public object GlLock => egl.GlLockObject;

    public void Present(Gpu? gpu)
    {
        CheatManager.Apply();
        Runtime.RamLog.Tick();

        if (gpu == null)
            return;

        if (gpu.DisplayToggledSinceLastCheck)
        {
            gpu.DisplayToggledSinceLastCheck = false;
            DiskLog.Log($"Present: GPU display toggled -> {(gpu.DisplayEnabled ? "ENABLED" : "DISABLED")} at frame {_frameCounter}, size {gpu.DisplayWidth}x{gpu.DisplayHeight}");
        }

        if (!gpu.DisplayEnabled || !backend.Ready)
            return;

        var nativeWidth = gpu.DisplayWidth;
        var nativeHeight = gpu.DisplayHeight;
        if (nativeWidth <= 0 || nativeHeight <= 0)
            return;

        _frameCounter++;
        // Heartbeat every 120 frames (~2s at 60fps) rather than every frame:
        // logging every single frame would itself perturb timing enough to
        // mask or shift a race, and 68KB-scale log files get awkward to
        // pull off-device. The goal here is narrowing "crashed somewhere in
        // Entry.Run, at some point, doing something" down to "crashed
        // between frame N and N+1, during phase X" - a heartbeat plus
        // per-phase logging on the frames immediately after a resize/
        // pause/resume (the known trigger candidates from earlier in this
        // investigation) covers that without flooding the log.
        bool verbose = _frameCounter <= 10 || _frameCounter % 300 == 0;
        if (verbose) DiskLog.Log($"Present: frame {_frameCounter} begin, gpu {nativeWidth}x{nativeHeight}");

        lock (GlLock)
        {
        var surfaceWidth = egl.SurfaceWidth;
        var surfaceHeight = egl.SurfaceHeight;
        if (verbose) DiskLog.Log($"Present: frame {_frameCounter} got GlLock, surface {surfaceWidth}x{surfaceHeight}");
        var phaseStart = System.Diagnostics.Stopwatch.GetTimestamp();
        var frameIntervalMilliseconds = _lastPresentTimestamp == 0
            ? 0
            : (phaseStart - _lastPresentTimestamp) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        _lastPresentTimestamp = phaseStart;

        var presented = backend.PresentDisplay(
            gpu.DisplayX, gpu.DisplayY, nativeWidth, nativeHeight, gpu.Display24Bit,
            surfaceWidth, surfaceHeight);
        if (backend.LastPresentHadRt != _lastLoggedHadRt)
        {
            _lastLoggedHadRt = backend.LastPresentHadRt;
            DiskLog.Log($"Present: frame {_frameCounter} render target availability changed -> {(backend.LastPresentHadRt ? "HAS RT (drawing real content)" : "NO RT (falling back to raw VRAM)")}");
        }
        if (verbose) DiskLog.Log($"Present: frame {_frameCounter} PresentDisplay done ({presented.w}x{presented.h}), hadRt={backend.LastPresentHadRt}");
        var prepared = System.Diagnostics.Stopwatch.GetTimestamp();
        backend.PresentToDefaultFramebuffer(surfaceWidth, surfaceHeight, presented.aspect);
        if (verbose) DiskLog.Log($"Present: frame {_frameCounter} PresentToDefaultFramebuffer done");
        var composited = System.Diagnostics.Stopwatch.GetTimestamp();
        egl.SwapBuffers();
        if (verbose) DiskLog.Log($"Present: frame {_frameCounter} SwapBuffers done");
        var swapped = System.Diagnostics.Stopwatch.GetTimestamp();

        // ~4x/sec at 60fps - frequent enough to visibly confirm the render
        // loop is alive without a per-frame InvokeOnMainThread flood.
        if (_frameCounter % 15 == 0)
        {
            status.UpdateDebugOverlay(
                $"frame {_frameCounter}  disp={gpu.DisplayWidth}x{gpu.DisplayHeight}  hadRt={backend.LastPresentHadRt}");
        }

        double ticksToMilliseconds = 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        _prepareMilliseconds += (prepared - phaseStart) * ticksToMilliseconds;
        _surfaceMilliseconds += (composited - prepared) * ticksToMilliseconds;
        _swapMilliseconds += (swapped - composited) * ticksToMilliseconds;
        _flushes += backend.LastFrameFlushes;
        _writebacks += backend.LastFrameWritebacks;
        _vertices += backend.LastFrameVertices;
        diagnostics.RecordFrame(
            frameIntervalMilliseconds,
            (prepared - phaseStart) * ticksToMilliseconds,
            (composited - prepared) * ticksToMilliseconds,
            (swapped - composited) * ticksToMilliseconds,
            backend.LastFrameFlushes,
            backend.LastFrameWritebacks,
            backend.LastFrameVertices);

        _fpsFrames++;
        var now = System.Diagnostics.Stopwatch.GetTimestamp();
        var elapsed = (now - _fpsWindow) / (double)System.Diagnostics.Stopwatch.Frequency;
        if (elapsed >= 2.0)
        {
            var frames = Math.Max(1, _fpsFrames);
            LastFps = _fpsFrames / elapsed;
            SessionLog.Info($"{_fpsFrames / elapsed:F1} FPS, surface {surfaceWidth}x{surfaceHeight}, " +
                     $"present {presented.w}x{presented.h}, CPU submit " +
                     $"{_prepareMilliseconds / frames:F2}+{_surfaceMilliseconds / frames:F2} ms, " +
                     $"swap {_swapMilliseconds / frames:F2} ms");
            _fpsFrames = 0;
            _fpsWindow = now;
            _prepareMilliseconds = _surfaceMilliseconds = _swapMilliseconds = 0;
            _flushes = _writebacks = _vertices = 0;
        }

        if (!_firstFrame) return;
        _firstFrame = false;
        DiskLog.Log("Present: first frame complete, hiding status");
        status.SetStatus($"Game running • {presented.w}×{presented.h}", visible: false);
        }
    }

    public void Shutdown()
    {
        DiskLog.Log("IosPlatformHost.Shutdown: enter");
        RecompOne.Runtime.Host.FrameClock.ResumeTiming();
        _audio.Dispose();
        RecompOne.Runtime.Hardware.Controller.SetVirtualPadState(0);
        status.SetStatus("Session ended.", visible: true);
        DiskLog.Log("IosPlatformHost.Shutdown: done");
    }
}

/// <summary>
/// Thin cross-boundary status callback. Implemented on the Swift/ObjC side
/// (or a small ObjC-bridge class) so this file has zero UIKit references and
/// can be unit-built/tested on any OS before wiring the actual view.
/// </summary>
public interface IStatusSink
{
    void SetStatus(string text, bool visible);
    void UpdateDebugOverlay(string text);
}

/// <summary>
/// AndroidRuntimeHost/GpuDiagnostics.cs's GameGpuDiagnosticsSession is tied
/// to Android.App.Activity and AndroidGlesInfo - not worth dragging its
/// telemetry-upload machinery onto iOS for a first pass. This is a
/// same-shaped, in-memory-only stand-in: it records frame timings so the
/// FPS/timing math in IosPlatformHost.Present keeps working unchanged, but
/// doesn't persist or upload anything. Swap for a real implementation
/// later if you want on-device GPU diagnostics reports for iOS too.
/// </summary>
public sealed class IosGpuDiagnosticsSession
{
    public void RecordFrame(
        double frameIntervalMs, double prepareMs, double surfaceMs, double swapMs,
        int flushes, int writebacks, int vertices)
    {
        // Intentionally empty - see class doc comment.
    }
}
