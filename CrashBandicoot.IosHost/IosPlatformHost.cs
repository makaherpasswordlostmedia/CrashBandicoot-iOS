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
    double _prepareMilliseconds;
    double _surfaceMilliseconds;
    double _swapMilliseconds;
    long _flushes;
    long _writebacks;
    long _vertices;
    long _lastPresentTimestamp;

    public static double LastFps { get; private set; }

    public void Initialize(string title) => status.SetStatus($"{title}: first frame incoming…", visible: true);
    public void WaitForValidDisc() { }
    public void AttachAudio(Spu? spu) => _audio.Attach(spu);
    public void SetMasterVolume(float volume) => _audio.SetMasterVolume(volume);
    public void ShowNotice(string message) => status.SetStatus(message, visible: true);

    public void PauseAudio()
    {
        RecompOne.Runtime.Host.FrameClock.PauseTiming();
        _audio.PauseOutput();
    }

    public void ResumeAudio()
    {
        _audio.ResumeOutput();
        RecompOne.Runtime.Host.FrameClock.ResumeTiming();
    }

    public void NotifySurfaceSize(int width, int height) => egl.SetExpectedSize(width, height);

    public void Present(Gpu? gpu)
    {
        CheatManager.Apply();
        Runtime.RamLog.Tick();

        if (gpu == null || !gpu.DisplayEnabled || !backend.Ready)
            return;

        var nativeWidth = gpu.DisplayWidth;
        var nativeHeight = gpu.DisplayHeight;
        if (nativeWidth <= 0 || nativeHeight <= 0)
            return;

        var surfaceWidth = egl.SurfaceWidth;
        var surfaceHeight = egl.SurfaceHeight;
        var phaseStart = System.Diagnostics.Stopwatch.GetTimestamp();
        var frameIntervalMilliseconds = _lastPresentTimestamp == 0
            ? 0
            : (phaseStart - _lastPresentTimestamp) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        _lastPresentTimestamp = phaseStart;

        var presented = backend.PresentDisplay(
            gpu.DisplayX, gpu.DisplayY, nativeWidth, nativeHeight, gpu.Display24Bit,
            surfaceWidth, surfaceHeight);
        var prepared = System.Diagnostics.Stopwatch.GetTimestamp();
        backend.PresentToDefaultFramebuffer(surfaceWidth, surfaceHeight, presented.aspect);
        var composited = System.Diagnostics.Stopwatch.GetTimestamp();
        egl.SwapBuffers();
        var swapped = System.Diagnostics.Stopwatch.GetTimestamp();

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
        status.SetStatus($"Game running • {presented.w}×{presented.h}", visible: false);
    }

    public void Shutdown()
    {
        RecompOne.Runtime.Host.FrameClock.ResumeTiming();
        _audio.Dispose();
        RecompOne.Runtime.Hardware.Controller.SetVirtualPadState(0);
        status.SetStatus("Session ended.", visible: true);
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
