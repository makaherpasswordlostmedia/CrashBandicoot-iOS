using System.Diagnostics;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Sdk;

public static class LibCdStream
{
    const int HeaderSize = 32;
    const int SlotData = 2016;
    const ushort VideoMagic = 0x0160;

    public static bool InUse { get; private set; }
    static uint _statusBase;
    static int _slots;
    static uint _dataBase;

    static volatile bool _active;
    static volatile bool _reading;
    static int _pendingLba = -1;
    static int _streamLba = -1;
    static int _streamStartLba;
    static readonly Stopwatch _clock = new();

    static int _writeIdx;
    static bool[] _busy = System.Array.Empty<bool>();
    static readonly Queue<(int start, int n)> _ready = new();
    static int _prevStart = -1, _prevN;

    static Thread? _thread;
    static volatile bool _run;
    static readonly object _lock = new();

    public static void StSetRing(CpuContext c, IMemory m)
    {
        InUse = true;
        lock (_lock)
        {
            _statusBase = c.A0;
            _slots = (int)c.A1;
            _dataBase = _statusBase + (uint)(_slots * HeaderSize);
            ResetRing(m);
        }
        EnsureThread();
        Log.Sdk($"StSetRing base=0x{_statusBase:X8} slots={_slots} data=0x{_dataBase:X8}");
    }

    public static void StClearRing(CpuContext c, IMemory m)
    {
        lock (_lock) ResetRing(m);
        c.V0 = 0;
        Log.Sdk("StClearRing");
    }

    public static void StUnSetRing(CpuContext c, IMemory m)
    {
        _active = false;
        _reading = false;
        Log.Sdk("StUnSetRing");
    }

    public static void StSetStream(CpuContext c, IMemory m)
    {
        lock (_lock)
        {
            _streamLba = -1;
            ResetRing(m);
            XaAudio.Reset();
        }
        _active = true;
        EnsureThread();
        Log.Sdk("StSetStream");
    }

    public static void StSetMask(CpuContext c, IMemory m) { c.V0 = 0; Log.Sdk("StSetMask"); }

    static long _getNextCalls, _getNextEmpty;

    public static void StGetNext(CpuContext c, IMemory m)
    {
        if (!_active) { c.V0 = 1; return; }
        _getNextCalls++;

        lock (_lock)
        {
            if (_prevStart >= 0)
            {
                for (int i = 0; i < _prevN; i++) _busy[_prevStart + i] = false;
                _prevStart = -1;
            }

            if (_ready.Count == 0)
            {
                _getNextEmpty++;
                // DIAGNOSTIC: logs every 500th empty poll, so a persistently
                // stuck loading screen shows up as a long run of these with
                // _getNextEmpty tracking _getNextCalls almost 1:1 - i.e.
                // the ring is essentially always empty, meaning
                // StreamLoop's video-magic filter (see its own diagnostic
                // log above) is never finding a matching sector to hand
                // over.
                if (_getNextEmpty % 500 == 0)
                    RecompOne.Runtime.Log.Sdk($"StGetNext: empty (calls={_getNextCalls}, empty={_getNextEmpty}, streamLba={_streamLba})");
                c.V0 = 1; return;
            }

            var (start, n) = _ready.Dequeue();
            uint dataPtr = _dataBase + (uint)(start * SlotData);
            uint hdrPtr = _statusBase + (uint)(start * HeaderSize);
            m.WriteU32(c.A0, dataPtr);
            m.WriteU32(c.A1, hdrPtr);
            _prevStart = start;
            _prevN = n;
            c.V0 = 0;
        }
    }

    public static void StFreeRing(CpuContext c, IMemory m) { c.V0 = 0; Log.Sdk("StFreeRing"); }

    public static void StGetBackloc(CpuContext c, IMemory m) { c.V0 = 0xFFFFFFFFu; Log.Sdk("StGetBackloc"); }

    internal static void OnReadStream(int lba)
    {
        if (!InUse) return;
        _pendingLba = lba;
        _reading = true;
        EnsureThread();
    }

    internal static void OnStopStream()
    {
        _reading = false;
        // Drop the queued XA as well: with a deep ring the old audio would
        // otherwise keep playing well past the Stop/Pause command.
        XaAudio.Reset();
    }

    static void ResetRing(IMemory m)
    {
        _writeIdx = 0;
        _prevStart = -1;
        _prevN = 0;
        _ready.Clear();
        _busy = _slots > 0 ? new bool[_slots] : System.Array.Empty<bool>();
        for (int i = 0; i < _slots; i++)
            m.WriteU16(_statusBase + (uint)(i * HeaderSize), 0);
    }

    static void EnsureThread()
    {
        if (_thread is { IsAlive: true }) return;
        _run = true;
        _thread = new Thread(StreamLoop) { IsBackground = true, Name = "CdStream" };
        _thread.Start();
    }

    static void StreamLoop()
    {
        while (_run)
        {
            var cd = Runtime.Cd;
            var m = Runtime.Mem;
            if (cd == null || m == null || !_active || !_reading || _slots <= 0)
            {
                Thread.Sleep(2);
                continue;
            }

            if (_streamLba < 0)
            {
                _streamLba = _pendingLba >= 0 ? _pendingLba : LibCd.CurrentLba;
                _streamStartLba = _streamLba;
                _clock.Restart();
            }

            byte[] sec;
            try { lock (LibCd.DiscLock) sec = cd.ReadSectorData(_streamLba, 2336); }
            catch { Thread.Sleep(2); continue; }

            if ((sec[2] & 0x04) != 0) { XaAudio.DecodeSector(sec, 8, sec[3]); _streamLba++; continue; }

            // FIXED: previously required magic=0x0160 && chan==0 (STR/FMV
            // sectors only), which starved StGetNext forever whenever the
            // game used StSetStream/StGetNext for plain data (e.g. level
            // geometry) streaming instead of video playback - CD reads
            // never stopped, but the ring stayed empty and the loading
            // screen never advanced (see git history for the diagnostic
            // that found this). The real PS1 SDK doesn't filter here: it
            // hands every non-XA-audio sector to the caller via StGetNext
            // and lets the game's own STR/data parser decide what to do
            // with it based on the sector header. We do the same: accept
            // any sector that isn't XA audio, and only use the STR magic
            // to decide how many logical "frame" sectors to batch (n),
            // falling back to delivering a single sector at a time for
            // non-STR data so nothing gets silently dropped.
            bool isStr = Read16(sec, 8) == VideoMagic && Read16(sec, 12) == 0;
            int n = isStr ? Read16(sec, 14) : 1;
            if (n <= 0 || n > _slots) n = 1;

            double delivered = _clock.Elapsed.TotalSeconds * LibCd.SectorsPerSecond;
            if ((_streamLba - _streamStartLba) + n > delivered) { Thread.Sleep(1); continue; }

            int start;
            lock (_lock)
            {
                if (_writeIdx + n > _slots) _writeIdx = 0;
                start = _writeIdx;
                bool free = true;
                for (int i = 0; i < n; i++) if (_busy[start + i]) { free = false; break; }
                if (!free) { Thread.Sleep(1); continue; }
            }

            if (!CollectFrame(cd, m, start, n)) continue;

            lock (_lock)
            {
                for (int i = 0; i < n; i++) _busy[start + i] = true;
                _ready.Enqueue((start, n));
                _writeIdx = start + n;
            }
        }
    }

    static bool CollectFrame(Cdrom.CdController cd, IMemory m, int start, int n)
    {
        int collected = 0;
        int lba = _streamLba;
        while (collected < n)
        {
            byte[] sec;
            try { lock (LibCd.DiscLock) sec = cd.ReadSectorData(lba, 2336); }
            catch { return false; }
            lba++;

            if ((sec[2] & 0x04) != 0) { XaAudio.DecodeSector(sec, 8, sec[3]); continue; }
            // FIXED: was `if (Read16(sec, 8) != VideoMagic) continue;` here
            // too, which for non-STR data sectors meant this while loop
            // (collected < n) could spin forever advancing lba without
            // ever incrementing collected. Non-XA sectors are now always
            // collected regardless of magic - see the StreamLoop comment
            // above for why filtering on STR magic was wrong in general.

            uint hdr = _statusBase + (uint)((start + collected) * HeaderSize);
            uint dat = _dataBase + (uint)((start + collected) * SlotData);
            for (int j = 0; j < HeaderSize; j++) m.WriteU8(hdr + (uint)j, sec[8 + j]);
            for (int j = 0; j < SlotData; j++) m.WriteU8(dat + (uint)j, sec[8 + HeaderSize + j]);
            collected++;
        }
        _streamLba = lba;
        Thread.MemoryBarrier();
        return true;
    }

    static ushort Read16(byte[] b, int o) => (ushort)(b[o] | (b[o + 1] << 8));
}
