using System.Runtime.InteropServices;
using AVFoundation;
using Foundation;
using RecompOne.Runtime;

namespace CrashBandicoot.IosHost;

/// <summary>
/// iOS audio backend: SPU/XA mix -> AVAudioPlayerNode -> AVAudioEngine.
///
/// WHY THIS DESIGN (read before changing it):
/// The previous implementation used an AVAudioSourceNode render callback that
/// drained a ring buffer under a lock also taken by other threads. CoreAudio's
/// realtime thread has a hard per-buffer deadline; blocking on that lock (or
/// stalling in managed code, e.g. waiting for a GC) made the watchdog abort()
/// the whole process. So this version has NO managed code on the realtime
/// thread at all:
///
///   * A normal background thread ("spu-mixer-ios") calls spu.Mix() for
///     1024 frames (~23 ms @ 44.1 kHz), converts to Float32 and hands the
///     buffer to AVAudioPlayerNode.ScheduleBuffer() - a PUSH model, exactly
///     like AudioTrack.Write() on Android.
///   * CoreAudio pulls from the player node's internal queue on its own
///     realtime thread, in native code only.
///   * Pacing: a semaphore limits how many buffers are in flight. It is
///     released from ScheduleBuffer's completion handler (which runs on a
///     normal dispatch queue, NOT the realtime thread), so the mixer blocks
///     exactly like a blocking Write() would and never runs ahead.
///
/// Because nothing here runs on the realtime thread, ordinary locks are fine.
/// Any failure degrades to silence; it can never take the game down.
/// </summary>
sealed class IosAudioOutput : IDisposable
{
    const double SampleRate = 44100.0;
    const int FramesPerBuffer = 1024;   // ~23 ms, same chunk size as Android/desktop
    const int BufferCount = 4;          // ~93 ms of queued audio (latency vs. underrun safety)

    readonly object _sync = new();
    readonly short[] _sampleBuf = new short[FramesPerBuffer * 2];
    readonly float[] _left = new float[FramesPerBuffer];
    readonly float[] _right = new float[FramesPerBuffer];
    readonly SemaphoreSlim _inflight = new(BufferCount, BufferCount);
    readonly ManualResetEventSlim _resumeSignal = new(initialState: true);

    AVAudioEngine? _engine;
    AVAudioPlayerNode? _player;
    AVAudioFormat? _format;
    AVAudioPcmBuffer[]? _buffers;
    NSObject? _configObserver;
    NSObject? _interruptObserver;

    Thread? _mixerThread;
    volatile bool _running;
    volatile bool _paused;
    volatile bool _initFailed;
    Spu? _spu;
    float _masterVolume = 1f;

    // Called every game frame - keep the fast path trivial.
    public void Attach(Spu? spu)
    {
        if (spu == null || ReferenceEquals(_spu, spu)) return;
        _spu = spu;
        EnsureStarted();
    }

    public void SetMasterVolume(float volume)
    {
        _masterVolume = Math.Clamp(volume, 0f, 1f);
        lock (_sync)
        {
            try { if (_player != null) _player.Volume = _masterVolume; }
            catch { /* shutting down */ }
        }
    }

    public void PauseOutput()
    {
        _paused = true;
        _resumeSignal.Reset();
        lock (_sync)
        {
            try { _player?.Pause(); } catch { /* shutting down */ }
        }
    }

    public void ResumeOutput()
    {
        lock (_sync)
        {
            // After backgrounding / a phone call the engine may have been
            // stopped by the system - bring it back before playing.
            try { if (_running) RestartEngineLocked(); }
            catch { /* stays silent */ }
        }
        _paused = false;
        _resumeSignal.Set();
    }

    void EnsureStarted()
    {
        lock (_sync)
        {
            if (_running || _initFailed) return;
            _running = true;
            _mixerThread = new Thread(MixerLoop)
            {
                IsBackground = true,
                Name = "spu-mixer-ios",
                Priority = ThreadPriority.AboveNormal,
            };
            _mixerThread.Start();
        }
    }

    // ------------------------------------------------------------------
    // Engine setup / restart (never on the realtime thread)
    // ------------------------------------------------------------------

    bool InitEngine()
    {
        lock (_sync)
        {
            try
            {
                var session = AVAudioSession.SharedInstance();
                // Playback: audio also plays with the hardware silent switch on.
                session.SetCategory(AVAudioSessionCategory.Playback, out _);
                session.SetActive(true, out _);

                // Standard format = Float32, non-interleaved, 2 channels.
                _format = new AVAudioFormat(SampleRate, 2);

                _buffers = new AVAudioPcmBuffer[BufferCount];
                for (int i = 0; i < BufferCount; i++)
                    _buffers[i] = new AVAudioPcmBuffer(_format, (uint)FramesPerBuffer);

                _engine = new AVAudioEngine();
                _player = new AVAudioPlayerNode { Volume = _masterVolume };
                _engine.AttachNode(_player);
                _engine.Connect(_player, _engine.MainMixerNode, _format);
                _engine.Prepare();

                if (!_engine.StartAndReturnError(out var err))
                    throw new InvalidOperationException($"AVAudioEngine failed to start: {err?.LocalizedDescription}");

                _player.Play();

                // Route change / interruption (call, Siri, headphones unplugged...)
                _configObserver = NSNotificationCenter.DefaultCenter.AddObserver(
                    AVAudioEngine.ConfigurationChangeNotification, _ => OnSystemAudioChange());
                _interruptObserver = NSNotificationCenter.DefaultCenter.AddObserver(
                    AVAudioSession.InterruptionNotification, _ => OnSystemAudioChange());

                DiskLog.Log($"IosAudioOutput: started, {SampleRate} Hz stereo, {BufferCount}x{FramesPerBuffer} frames");
                return true;
            }
            catch (Exception ex)
            {
                DiskLog.Log($"IosAudioOutput EXCEPTION (init failed, game stays silent): {ex.Message}");
                _running = false;
                _engine = null;
                _player = null;
                return false;
            }
        }
    }

    void OnSystemAudioChange()
    {
        if (_paused) return; // ResumeOutput() will restart when the game resumes
        lock (_sync)
        {
            try { RestartEngineLocked(); } catch { /* stays silent */ }
        }
    }

    void RestartEngineLocked()
    {
        if (_engine == null || _player == null) return;
        var session = AVAudioSession.SharedInstance();
        session.SetActive(true, out _);
        if (!_engine.Running)
        {
            _engine.Prepare();
            _engine.StartAndReturnError(out _);
        }
        if (_engine.Running && !_player.Playing)
            _player.Play();
    }

    // ------------------------------------------------------------------
    // Mixer thread: SPU -> Float32 -> ScheduleBuffer (blocks via semaphore)
    // ------------------------------------------------------------------

    void MixerLoop()
    {
        if (!InitEngine())
        {
            _initFailed = true;
            return;
        }

        int idx = 0;
        while (_running)
        {
            _resumeSignal.Wait();                 // parked while paused
            if (!_running) break;

            var spu = _spu;
            var player = _player;
            var buffers = _buffers;
            if (spu == null || player == null || buffers == null)
            {
                Thread.Sleep(5);
                continue;
            }

            // Wait for a free slot (this is what paces the mixer, like a blocking Write()).
            if (!_inflight.Wait(20)) continue;
            if (!_running) break;
            if (_paused) { SafeRelease(); continue; }

            try
            {
                spu.Mix(_sampleBuf, FramesPerBuffer);

                for (int i = 0; i < FramesPerBuffer; i++)
                {
                    _left[i] = _sampleBuf[2 * i] * (1f / 32768f);
                    _right[i] = _sampleBuf[2 * i + 1] * (1f / 32768f);
                }

                var buf = buffers[idx];
                idx = (idx + 1) % BufferCount;

                // FloatChannelData is a float** (one pointer per channel).
                IntPtr channels = buf.FloatChannelData;
                Marshal.Copy(_left, 0, Marshal.ReadIntPtr(channels, 0), FramesPerBuffer);
                Marshal.Copy(_right, 0, Marshal.ReadIntPtr(channels, IntPtr.Size), FramesPerBuffer);
                buf.FrameLength = (uint)FramesPerBuffer;

                player.ScheduleBuffer(buf, SafeRelease);
            }
            catch (Exception ex)
            {
                DiskLog.Log($"IosAudioOutput: mixer error, skipping chunk: {ex.Message}");
                SafeRelease();
                Thread.Sleep(5);
            }
        }
    }

    void SafeRelease()
    {
        try { _inflight.Release(); }
        catch (SemaphoreFullException) { /* completion fired after a stop/restart flush */ }
        catch (ObjectDisposedException) { /* disposing */ }
    }

    public void Dispose()
    {
        _running = false;
        _resumeSignal.Set();
        lock (_sync)
        {
            try { _mixerThread?.Join(500); } catch { /* ignore */ }
            _mixerThread = null;

            if (_configObserver != null) NSNotificationCenter.DefaultCenter.RemoveObserver(_configObserver);
            if (_interruptObserver != null) NSNotificationCenter.DefaultCenter.RemoveObserver(_interruptObserver);
            _configObserver = null;
            _interruptObserver = null;

            try { _player?.Stop(); } catch { /* ignore */ }
            try { _engine?.Stop(); } catch { /* ignore */ }
            _player = null;
            _engine = null;
        }
    }
}
