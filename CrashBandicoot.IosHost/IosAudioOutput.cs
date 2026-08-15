using AVFoundation;
using AudioToolbox;
using RecompOne.Runtime;
using RecompOne.Runtime.Diagnostics;

namespace CrashBandicoot.IosHost;

/// <summary>
/// iOS port of AndroidRuntimeHost/AndroidAudioOutput.cs.
///
/// Android's AudioTrack is a push/blocking-write API: a dedicated mixer
/// thread calls spu.Mix(...) then blocks in track.Write(...) until the
/// hardware wants more. CoreAudio/AVAudioEngine is the opposite: a
/// PULL model - the OS calls our render callback from a realtime audio
/// thread whenever it needs samples. We bridge the two with a small lock-free
/// ring buffer: the same background "mixer" thread as Android keeps calling
/// spu.Mix(...) and pushing into the ring; the AVAudioSourceNode render
/// callback just drains it. This keeps RecompOne.Runtime's Spu.Mix contract
/// completely unchanged from Android/Windows.
/// </summary>
sealed class IosAudioOutput : IDisposable
{
    const int SampleRate = 44100;
    const int Channels = 2;
    const int FramesPerBuffer = 1024; // same chunk size as Android, ~23 ms
    const int RingCapacityFrames = FramesPerBuffer * 8;

    readonly short[] _mixBuf = new short[FramesPerBuffer * Channels];
    readonly short[] _ring = new short[RingCapacityFrames * Channels];
    readonly object _sync = new();
    readonly ManualResetEventSlim _resumeSignal = new(initialState: true);

    int _ringReadFrame, _ringWriteFrame, _ringCountFrames;

    AVAudioEngine? _engine;
    AVAudioSourceNode? _sourceNode;
    Thread? _mixerThread;
    volatile bool _running;
    volatile bool _paused;
    Spu? _spu;
    float _masterVolume = 1f;
    bool _initFailed;

    public void Attach(Spu? spu)
    {
        if (spu == null || ReferenceEquals(_spu, spu))
            return;
        _spu = spu;
        EnsureStarted();
    }

    public void SetMasterVolume(float volume)
    {
        _masterVolume = Math.Clamp(volume, 0f, 1f);
        lock (_sync)
        {
            if (_engine != null)
                _engine.MainMixerNode.OutputVolume = _masterVolume;
        }
    }

    public void PauseOutput()
    {
        _paused = true;
        _resumeSignal.Reset();
        lock (_sync) { try { _engine?.Pause(); } catch { /* shutting down */ } }
    }

    public void ResumeOutput()
    {
        lock (_sync)
        {
            try { if (_running) _engine?.StartAndReturnError(out _); }
            catch { /* shutting down */ }
        }
        _paused = false;
        _resumeSignal.Set();
    }

    void EnsureStarted()
    {
        lock (_sync)
        {
            if (_running || _initFailed)
                return;
            try
            {
                var session = AVAudioSession.SharedInstance();
                session.SetCategory(AVAudioSessionCategory.Playback);
                session.SetActive(true);

                var format = new AVAudioFormat(SampleRate, (uint)Channels);
                var engine = new AVAudioEngine();

                // Render callback runs on a realtime CoreAudio thread - must
                // not allocate, lock with contention, or block. It only ever
                // drains the ring buffer written by MixerLoop below.
                var sourceNode = new AVAudioSourceNode(format, (isSilence, timestamp, frameCount, audioBufferList) =>
                {
                    unsafe
                    {
                        var buffers = (AudioBuffers*)audioBufferList;
                        // Interleaved stereo 16-bit PCM output.
                        var outPtr = (short*)(*buffers)[0];
                        int framesNeeded = (int)frameCount;
                        int framesWritten = DrainRing(outPtr, framesNeeded);
                        isSilence.Data = framesWritten == 0;
                        return 0; // noErr
                    }
                });

                engine.AttachNode(sourceNode);
                engine.Connect(sourceNode, engine.MainMixerNode, format);
                engine.MainMixerNode.OutputVolume = _masterVolume;

                if (!engine.StartAndReturnError(out var err))
                    throw new InvalidOperationException($"AVAudioEngine start failed: {err?.LocalizedDescription}");

                _engine = engine;
                _sourceNode = sourceNode;
                _running = true;
                _mixerThread = new Thread(MixerLoop)
                {
                    IsBackground = true,
                    Name = "spu-mixer-ios",
                    Priority = ThreadPriority.AboveNormal,
                };
                _mixerThread.Start();
                SessionLog.Info($"AVAudioEngine started: {SampleRate} Hz stereo, ring {RingCapacityFrames} frames.");
            }
            catch (Exception ex)
            {
                _initFailed = true;
                _running = false;
                try { _engine?.Stop(); } catch { /* ignore */ }
                _engine = null;
                SessionLog.Error($"Audio init failed, game stays silent: {ex}");
            }
        }
    }

    void MixerLoop()
    {
        while (_running)
        {
            _resumeSignal.Wait();
            if (!_running) break;

            var spu = _spu;
            if (spu == null)
            {
                Thread.Sleep(5);
                continue;
            }

            // Backpressure: don't run too far ahead of the realtime consumer.
            lock (_sync)
            {
                if (_ringCountFrames > RingCapacityFrames - FramesPerBuffer)
                {
                    Monitor.Wait(_sync, 5);
                    continue;
                }
            }

            spu.Mix(_mixBuf, FramesPerBuffer);
            WriteRing(_mixBuf, FramesPerBuffer);
        }
    }

    void WriteRing(short[] src, int frames)
    {
        lock (_sync)
        {
            for (int f = 0; f < frames; f++)
            {
                int w = (_ringWriteFrame + f) % RingCapacityFrames;
                _ring[w * Channels] = src[f * Channels];
                _ring[w * Channels + 1] = src[f * Channels + 1];
            }
            _ringWriteFrame = (_ringWriteFrame + frames) % RingCapacityFrames;
            _ringCountFrames = Math.Min(RingCapacityFrames, _ringCountFrames + frames);
            Monitor.PulseAll(_sync);
        }
    }

    unsafe int DrainRing(short* dst, int framesNeeded)
    {
        lock (_sync)
        {
            int framesAvail = Math.Min(framesNeeded, _ringCountFrames);
            for (int f = 0; f < framesAvail; f++)
            {
                int r = (_ringReadFrame + f) % RingCapacityFrames;
                dst[f * Channels] = _ring[r * Channels];
                dst[f * Channels + 1] = _ring[r * Channels + 1];
            }
            // Silence-fill any shortfall rather than glitching/repeating.
            for (int f = framesAvail; f < framesNeeded; f++)
            {
                dst[f * Channels] = 0;
                dst[f * Channels + 1] = 0;
            }
            _ringReadFrame = (_ringReadFrame + framesAvail) % RingCapacityFrames;
            _ringCountFrames -= framesAvail;
            Monitor.PulseAll(_sync);
            return framesAvail;
        }
    }

    public void Dispose()
    {
        _running = false;
        _resumeSignal.Set();
        lock (_sync)
        {
            try { _mixerThread?.Join(500); } catch { /* ignore */ }
            _mixerThread = null;
            if (_engine == null)
                return;
            try { _engine.Stop(); } catch { /* ignore */ }
            _engine = null;
            _sourceNode = null;
        }
    }
}
