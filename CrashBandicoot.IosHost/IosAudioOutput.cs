using RecompOne.Runtime;

namespace CrashBandicoot.IosHost;

/// <summary>
/// TEMPORARY no-op stand-in for the real iOS audio backend.
///
/// The previous AVAudioEngine-based implementation (AVAudioSourceNode render
/// callback draining a ring buffer filled by a background mixer thread) was
/// the actual, confirmed cause of the field abort() crashes seen throughout
/// this debugging session: CoreAudio's realtime render callback thread has a
/// hard per-buffer deadline and must never block, but DrainRing() took the
/// same `lock (_sync)` also taken by the mixer thread and by main-thread
/// calls (SetMasterVolume/PauseOutput/ResumeOutput/EnsureStarted/Dispose).
/// Under contention the realtime thread stalled past its deadline and
/// CoreAudio's watchdog aborted the whole process - which is why the crash
/// stack never correlated with EAGL/GL/resize changes and was invisible to
/// every earlier fix in this file's git history: it's a different subsystem
/// entirely, and the render thread carries no debug symbols in a release
/// AOT build, so the abort surfaced with no attribution back to CoreAudio.
///
/// Rather than ship a half-fixed realtime-audio implementation, this stub
/// keeps the exact same public surface IosPlatformHost.cs already calls
/// (Attach/SetMasterVolume/PauseOutput/ResumeOutput/Dispose) as pure no-ops.
/// The game runs completely silent until a proper iOS audio backend is
/// written and reviewed specifically for realtime-thread safety - do not
/// re-introduce a lock taken from both a CoreAudio render callback and any
/// other thread; the correct patterns are a lock-free SPSC ring buffer
/// (single producer/single consumer, no locks on either side) or, at
/// minimum, Monitor.TryEnter with a zero timeout on the render-thread side
/// so a miss degrades to silence instead of blocking.
/// </summary>
sealed class IosAudioOutput : IDisposable
{
    public void Attach(Spu? spu) { /* no-op: audio disabled, see class doc comment */ }
    public void SetMasterVolume(float volume) { /* no-op */ }
    public void PauseOutput() { /* no-op */ }
    public void ResumeOutput() { /* no-op */ }
    public void Dispose() { /* no-op */ }
}
