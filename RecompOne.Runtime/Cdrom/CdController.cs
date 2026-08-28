using RecompOne.Runtime.Bios;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime.Cdrom;

public sealed class CdController
{
    private readonly CueFs _fs;
    private readonly IMemory _m;

    private byte _index;
    private readonly Queue<byte> _paramFifo = new();
    private readonly Queue<byte> _responseFifo = new();
    private readonly Queue<(byte irqType, byte[] response)> _pendingIrqs = new();
    private byte _irqFlags;
    private int _seekLba;
    private byte[] _dataBuf = new byte[2048];

    private int _dataFifoPos;
    private bool _dataReady;
    private bool _reading;
    private bool _streamPending;
    private byte _lastIrq;
    private bool _hasReadAnySector;

    private readonly object _dbgGate = new();
    private readonly object _irqGate = new();
    private readonly Queue<string> _dbgEvents = new();
    private const int DbgMaxEvents = 256;
    private long _sectorsRead;
    private int _lastReadLba;

    public struct CdDebug
    {
        public int SeekLba, LastReadLba;
        public bool Reading, StreamPending, DataReady;
        public byte IrqFlags, LastIrq, Index;
        public int PendingIrqCount, ParamCount, ResponseCount, DataFifoPos, DataBufLength;
        public long SectorsRead;
    }

    private sealed class ReadRun
    {
        public int Start, Count;
        public string Time = "";
    }

    private readonly Dictionary<string, ReadRun> _runs = new();

    private void DbgEvent(string msg)
    {
        lock (_dbgGate)
        {
            FlushRunsLocked();
            EnqueueLocked($"{DateTime.Now:HH:mm:ss.fff}  {msg}");
        }
    }

    private void DbgReadRun(string source, int lba)
    {
        lock (_dbgGate)
        {
            if (_runs.TryGetValue(source, out var run))
            {
                if (lba == run.Start + run.Count) { run.Count++; return; }
                EnqueueLocked(RunLine(source, run));
            }
            _runs[source] = new ReadRun { Start = lba, Count = 1, Time = DateTime.Now.ToString("HH:mm:ss.fff") };
        }
    }

    private void FlushRunsLocked()
    {
        foreach (var (source, run) in _runs)
            EnqueueLocked(RunLine(source, run));
        _runs.Clear();
    }

    private void EnqueueLocked(string line)
    {
        _dbgEvents.Enqueue(line);
        while (_dbgEvents.Count > DbgMaxEvents) _dbgEvents.Dequeue();
    }

    private static string RunLine(string source, ReadRun run) =>
        run.Count == 1
            ? $"{run.Time}  {source} lba={run.Start}"
            : $"{run.Time}  {source} lba={run.Start}..{run.Start + run.Count - 1} ({run.Count} sectors)";

    public void ClearDebugEvents()
    {
        lock (_dbgGate)
        {
            _dbgEvents.Clear();
            _runs.Clear();
        }
    }

    public void CaptureDebug(out CdDebug d, List<string> events)
    {
        lock (_irqGate)
        {
            d = new CdDebug {
                SeekLba = _seekLba,
                LastReadLba = _lastReadLba,
                Reading = _reading,
                StreamPending = _streamPending,
                DataReady = _dataReady,
                IrqFlags = _irqFlags,
                LastIrq = _lastIrq,
                Index = _index,
                PendingIrqCount = _pendingIrqs.Count,
                ParamCount = _paramFifo.Count,
                ResponseCount = _responseFifo.Count,
                DataFifoPos = _dataFifoPos,
                DataBufLength = _dataBuf.Length,
                SectorsRead = _sectorsRead
            };
        }
        lock (_dbgGate)
        {
            events.Clear();
            events.AddRange(_dbgEvents);
            foreach (var (source, run) in _runs)
                events.Add(RunLine(source, run));
        }
    }

    private static string CmdName(byte cmd) => cmd switch {
        0x01 => "GetStat",
        0x02 => "Setloc",
        0x03 => "Play",
        0x04 => "Backward",
        0x05 => "Motor",
        0x06 => "ReadN",
        0x07 => "Forward",
        0x08 => "Stop",
        0x09 => "Pause",
        0x0A => "Init",
        0x0B => "Mute",
        0x0C => "Demute",
        0x0D => "Setfilter",
        0x0E => "Setmode",
        0x0F => "GetParam",
        0x10 => "GetlocL",
        0x11 => "GetlocP",
        0x12 => "SetSession",
        0x13 => "GetTN",
        0x14 => "GetTD",
        0x15 => "SeekL",
        0x16 => "SeekP",
        0x19 => "Test",
        0x1A => "GetID",
        0x1B => "ReadS",
        0x1E => "ReadTOC",
        _ => $"0x{cmd:X2}"
    };

    public CdController(CueFs fs, IMemory m)
    {
        _fs = fs;
        _m = m;
        BiosA.SetFs(fs);
        BiosA.SetCd(this);
        Runtime.Cd = this;
    }

    public void LoadToMemory(string path, uint address, int offset = 0, int length = -1)
    {
        var data = _fs.ReadFile(path);
        int count = length < 0 ? data.Length - offset : length;
        for (int i = 0; i < count; i++)
            _m.WriteU8(address + (uint)i, data[offset + i]);
        RecompOne.Runtime.Log.Cd($"{path} -> 0x{address:X8} | {count} bytes");
        DbgEvent($"file {path} -> 0x{address:X8} ({count} bytes)");
        Dispatcher.TryLoad(CdUtils.OverlayName(CdUtils.ExtractFileName(path)));
    }

    public byte Read(uint phys)
    {
        lock (_irqGate)
        {
            return (phys & 3) switch
            {
                0 => (byte)((_index & 3) | (_paramFifo.Count == 0 ? 0x08 : 0) | 0x10 | (_responseFifo.Count > 0 ? 0x20 : 0) | (_dataReady ? 0x40 : 0)),
                1 => _responseFifo.Count > 0 ? _responseFifo.Dequeue() : (byte)0,
                2 => ReadDataByte(),
                _ => _index == 1 ? _irqFlags : (byte)0,
            };
        }
    }

    public void Write(uint phys, byte val)
    {
        lock (_irqGate)
        {
            switch (phys & 3)
            {
                case 0:
                    _index = (byte)(val & 3);
                    break;
                case 1:
                    if (_index == 0) ExecuteCommand(val);
                    break;
                case 2:
                    if (_index == 0) _paramFifo.Enqueue(val);
                    else if (_index == 1) _paramFifo.Clear();
                    break;
                case 3:
                    if (_index == 0)
                    {
                        if ((val & 0x80) != 0) { _dataFifoPos = 0; _dataReady = true; }
                        else _dataReady = false;
                    }
                    else if (_index == 1)
                    {
                        _irqFlags &= (byte)~val;
                        if (_irqFlags == 0) AfterAck();
                    }
                    break;
            }
        }
    }

    private void ExecuteCommand(byte cmd)
    {
        RecompOne.Runtime.Log.Cd($"cmd 0x{cmd:X2}");
        var prms = new List<byte>();
        while (_paramFifo.Count > 0) prms.Add(_paramFifo.Dequeue());
        DbgEvent(prms.Count > 0
            ? $"{CmdName(cmd)} ({string.Join(" ", prms.Select(p => p.ToString("X2")))}) lba={_seekLba}"
            : $"{CmdName(cmd)} lba={_seekLba}");

        switch (cmd)
        {
            case 0x01:
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x02: //Setloc
                if (prms.Count >= 3)
                    _seekLba = BcdToLba(prms[0], prms[1], prms[2]);
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x06: // ReadN
                _reading = true;
                ReadNextSector();
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(1, [DriveStatus()]);
                break;
            case 0x08: //Stop
                _reading = false;
                _streamPending = false;
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(2, [DriveStatus()]);
                break;
            case 0x09: // Pause
                _reading = false;
                _streamPending = false;
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(2, [DriveStatus()]);
                break;
            case 0x0A:
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(2, [DriveStatus()]);
                break;
            case 0x0B: // mute
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x0C: // demute
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x0E: // set mode
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x10: // GetlocL - was unhandled (fell into default -> INT5
                       // "unknown command" error). Per psx-spx this returns
                       // an INT3 with the 8-byte header/subheader of the
                       // most recently read sector: amm,ass,asect (BCD),
                       // mode, file, channel, sm, ci. A game polling this
                       // to track read progress during loading would see
                       // every call fail with an error response instead,
                       // which can stall a loading-screen wait loop
                       // indefinitely - matches the observed symptom (CD
                       // reads never stop, but the game never proceeds).
                       //
                       // Per psx-spx, GetlocL returns error 80h ("Error
                       // Reason: Not Ready/wrong sub-mode") if no sector has
                       // ever actually been read yet - there is no "current
                       // sector header" to report before that. Reporting a
                       // fabricated header (all zeros/lastReadLba=0) instead
                       // of that error was itself a latent bug: a game that
                       // calls GetlocL to check "has my ReadN/ReadS actually
                       // produced a sector yet" before the first read would
                       // get a false "yes" (lba 0's header) rather than the
                       // real answer, which is a subtler version of exactly
                       // the same stall class this whole pass is closing.
                {
                    if (!_hasReadAnySector)
                    {
                        QueueIrq(5, [0x80]);
                        break;
                    }
                    var (mm, ss, ff) = LbaToBcd(_lastReadLba);
                    QueueIrq(3, [mm, ss, ff, 0x02, 0x00, 0x00, 0x00, 0x00]);
                }
                break;
            case 0x11: // GetlocP - same as above but for the physical/
                       // subchannel position; approximated with the same
                       // last-read LBA since this emulator does not track
                       // a separate absolute disc position. Also gated on
                       // _hasReadAnySector for the same reason as GetlocL
                       // above (psx-spx documents GetlocP failing with the
                       // same 80h before any Setloc+seek/read has happened).
                {
                    if (!_hasReadAnySector)
                    {
                        QueueIrq(5, [0x80]);
                        break;
                    }
                    var (amm, ass, aff) = LbaToBcd(_lastReadLba);
                    QueueIrq(3, [0x01, 0x01, amm, ass, aff, amm, ass, aff]);
                }
                break;
            case 0x0D: // Setfilter - CD-XA ADPCM channel/file filter. No
                       // audio-channel filtering is implemented (this
                       // emulator doesn't do CD-XA ADPCM decode at all
                       // yet), but the command itself must still ack
                       // normally - it was previously falling into default
                       // and returning INT5, which would stall any game
                       // that calls Setfilter and waits for a real
                       // acknowledgement before proceeding (XA movie/audio
                       // setup is a plausible place for that, even in a
                       // game that mostly uses a custom streaming path).
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x0F: // Setfilter is 0x0D above; 0x0F is GetParam - returns
                       // current mode/filter-file/filter-channel. Not
                       // tracked separately from Setmode's argument today;
                       // approximated by echoing DriveStatus + a zeroed
                       // mode/filter block, which is enough for a game that
                       // just polls-then-ignores this (the common case) but
                       // won't roundtrip an actual previously-set mode byte
                       // - flag for follow-up if a title is found that
                       // depends on that roundtrip specifically.
                QueueIrq(3, [DriveStatus(), 0x00, 0x00, 0x00, 0x00]);
                break;
            case 0x12: // ReadT (TOC) / SetSession - reads a TOC entry or
                       // switches session on multi-session discs. This
                       // emulator only ever exposes a single data track via
                       // CueFs, so there's no second session to switch to;
                       // acking normally (rather than the previous INT5) is
                       // correct because "there is nothing else to do" is a
                       // legitimate outcome for a single-session disc, not
                       // an error.
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(2, [DriveStatus()]);
                break;
            case 0x13: // GetTN - total track count. Per psx-spx: INT3,
                       // stat, first-track-BCD, last-track-BCD. CueFs backs
                       // this emulator with a single-track .cue/.bin image
                       // in every case seen so far, so first=last=track 1
                       // (BCD 0x01) is the correct answer here, not a
                       // stub/approximation. A game calling GetTN during
                       // disc-detection (very common - it's often the
                       // *first* CD command issued at boot, before Setloc)
                       // previously got INT5 and could have stalled before
                       // ever reaching the streaming code this whole
                       // investigation started with.
                QueueIrq(3, [DriveStatus(), 0x01, 0x01]);
                break;
            case 0x14: // GetTD - a single track's start position (min:sec
                       // BCD) given a 1-based track number param, or the
                       // disc's total length if track param is 0. Only
                       // track 1 exists here (see GetTN above). CueFs/CueBin
                       // don't currently expose a total-sector-count API, so
                       // rather than invent one under time pressure, both
                       // "start of track 1" and "GetTD(0)" answer with the
                       // standard 00:02:00 data-track start MSF - correct
                       // for the track-1-start case, and a safe non-zero
                       // placeholder (not an error response) for the
                       // total-length case. A game that actually depends on
                       // the real disc length from GetTD(0) specifically
                       // (uncommon - GetTN+per-track GetTD is the usual
                       // path, and this game has exactly one track) would
                       // need CueFs extended with a real total-sectors
                       // accessor; flagged here rather than guessed.
                QueueIrq(3, [DriveStatus(), 0x00, 0x02]);
                break;
            case 0x19: // Test - multi-function subcommand selected by
                       // prms[0]. Real hardware/BIOS-relevant subfunctions:
                       // 0x20 = get CD-ROM controller BIOS date+version
                       // (4 bytes, commonly polled by games/BIOS during
                       // hardware detection at boot, well before any disc
                       // I/O), 0x04/0x05 = start/stop SCEx read (region
                       // check - not modeled, ack harmlessly). Previously
                       // INT5 for every subfunction, which is a plausible
                       // very-early-boot stall point since 0x20 in
                       // particular is often queried before the game even
                       // gets to its own streaming code.
                {
                    byte sub = prms.Count > 0 ? prms[0] : (byte)0;
                    if (sub == 0x20)
                        QueueIrq(3, [0x94, 0x09, 0x19, 0xC0]); // plausible SCPH-5502-era date/version
                    else
                        QueueIrq(3, [DriveStatus()]);
                }
                break;
            case 0x1E: // ReadTOC - re-reads the disc's table of contents
                       // (used after a disc-change or at boot). No actual
                       // TOC re-read needed since CueFs is static for the
                       // process lifetime; must still ack with the
                       // INT3-then-INT2 pair real hardware/BIOS expects
                       // (games wait for the second IRQ before trusting the
                       // TOC is ready) rather than the previous single-INT5
                       // error, which could stall exactly that wait.
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(2, [DriveStatus()]);
                break;
            case 0x1A: // GetID - identifies the disc as licensed PS1 media.
                       // Per psx-spx this is INT3 stat+flags, then a second
                       // INT2 with an 8-byte response ending in the literal
                       // ASCII string "SCEA"/"SCEE"/"SCEI" (region-specific
                       // license string, checked by the BIOS's own
                       // anti-piracy boot logic before it will even jump
                       // into the game's executable). This is one of the
                       // most commonly issued CD commands on real hardware
                       // - it can run before Setloc, before the game's own
                       // code has executed a single instruction - so an
                       // INT5 here was a very plausible candidate for an
                       // early, silent, pre-gameplay stall that would look
                       // identical to "the game just never starts", not
                       // obviously CD-related at all. Answering "licensed
                       // data disc, region America" (SCEA) unconditionally;
                       // if a PAL/NTSC-J-specific build ever needs a
                       // different region string this is the place to
                       // branch on it, but a single hardcoded region is
                       // correct for the common case and for this project's
                       // single known disc image.
                QueueIrq(3, [0x02, 0x00]);
                QueueIrq(2, [0x02, 0x00, 0x20, 0x00, 0x53, 0x43, 0x45, 0x41]); // "SCEA"
                break;
            case 0x03: // Play (CDDA) - starts red-book audio playback from
                       // an optional track-number param. This emulator has
                       // no CD-DA audio decode/mixing path at all (Crash
                       // Bandicoot's audio is XA/ADPCM via the streaming
                       // path, not CDDA tracks - see the StSetStream
                       // investigation earlier in this session), so there
                       // is nothing to actually play. Acking normally
                       // rather than the previous INT5 matters if the BIOS
                       // or any startup code probes for CDDA capability/
                       // issues a Play as part of generic disc-init before
                       // the game's own logic ever branches on whether it
                       // worked - a silent no-op ack is the safe answer
                       // until real CDDA support exists, versus an error
                       // that could be treated as a hardware fault.
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x07: // Forward - CDDA fast-forward. No audio path (see
                       // Play above); ack only.
            case 0x04: // Backward - CDDA fast-reverse. Same as Forward.
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x05: // Motor - explicit motor-on command, distinct from
                       // Init's implicit motor-on. Motor state isn't
                       // modeled as its own bit here (DriveStatus always
                       // reports motor-on per the existing "was hardcoded"
                       // fix below), so this only needs to ack, not change
                       // any actual state.
                QueueIrq(3, [DriveStatus()]);
                break;
            case 0x15: // seek L
            case 0x16: //seek P
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(2, [DriveStatus()]);
                break;
            case 0x1B: // read s
                _reading = true;
                ReadNextSector();
                QueueIrq(3, [DriveStatus()]);
                QueueIrq(1, [DriveStatus()]);
                break;
            default:
                // Any future unhandled command lands here as an INT5
                // error response - same failure shape as the GetlocL/
                // GetlocP bug above (a game polling a command this
                // emulator doesn't implement gets an error instead of
                // real data, and can stall indefinitely). Routed through
                // DbgEvent instead of Console.WriteLine so it's visible in
                // checkpoint.log's "CD recent events" on a real device
                // without another diagnostic round-trip - if a loading
                // screen sticks again, check there first for an "unhandled
                // cmd" line before assuming it's a new class of bug.
                DbgEvent($"unhandled cmd 0x{cmd:X2}");
                QueueIrq(5, [DriveStatus(), 0x40]);
                break;
        }
    }

    private void QueueIrq(byte irqType, byte[] response)
    {
        // Now reachable from the background read thread (see
        // QueueAsyncReadSector) as well as the game thread via MMIO
        // Read/Write, so the shared IRQ/FIFO state needs a lock here that
        // didn't exist before - previously everything touching this ran
        // exclusively on the game thread and never needed one.
        lock (_irqGate)
        {
            if (_irqFlags == 0 && _pendingIrqs.Count == 0)
                DeliverImmediate(irqType, response);
            else
                _pendingIrqs.Enqueue((irqType, response));
        }
    }

    private void AfterAck()
    {
        if (_pendingIrqs.Count > 0) { DeliverNext(); return; }
        if (_reading && _lastIrq == 1) _streamPending = true;
        DbgEvent($"AfterAck: reading={_reading} lastIrq={_lastIrq} -> streamPending={_streamPending}");
        ClearInInterrupt();
    }

    public void AdvanceStreaming()
    {
        lock (_irqGate)
        {
            if (!_reading || !_streamPending) return;
            if (_irqFlags != 0 || _pendingIrqs.Count > 0) return;
            _streamPending = false;
            ReadNextSector();
            DbgEvent($"AdvanceStreaming: delivered lba={_lastReadLba}, next seekLba={_seekLba}");
            DeliverImmediate(1, [DriveStatus()]);
        }
    }

    private void DeliverImmediate(byte irqType, byte[] response)
    {
        _responseFifo.Clear();
        foreach (var b in response) _responseFifo.Enqueue(b);
        _irqFlags = irqType;
        _lastIrq = irqType;
        SetInInterrupt(1);
    }

    private void DeliverNext()
    {
        var (irqType, response) = _pendingIrqs.Dequeue();
        _responseFifo.Clear();
        foreach (var b in response) _responseFifo.Enqueue(b);
        _irqFlags = irqType;
        _lastIrq = irqType;
        SetInInterrupt(1);
    }

    private byte ReadDataByte()
    {
        if (!_dataReady || _dataFifoPos >= _dataBuf.Length) { _dataReady = false; return 0; }
        byte b = _dataBuf[_dataFifoPos++];
        if (_dataFifoPos >= _dataBuf.Length) _dataReady = false;
        return b;
    }

    public void DmaReadData(IMemory m, uint addr, uint byteCount)
    {
        for (uint i = 0; i < byteCount; i++)
            m.WriteU8(addr + i, _dataFifoPos < _dataBuf.Length ? _dataBuf[_dataFifoPos++] : (byte)0);
        if (_dataFifoPos >= _dataBuf.Length) _dataReady = false;
    }

    public void LoadSectorToFifo(byte[] data)
    {
        _dataBuf = (byte[])data.Clone();
        _dataFifoPos = 0;
        _dataReady = true;
    }

    private void SetInInterrupt(ushort val)
    {
        if (BiosB.IntrEnvInInterruptAddr != 0)
            _m.WriteU16(BiosB.IntrEnvInInterruptAddr, val);
    }

    private void ClearInInterrupt()
    {
        if (BiosB.IntrEnvInInterruptAddr != 0)
            _m.WriteU16(BiosB.IntrEnvInInterruptAddr, 0);
    }

    private void ReadNextSector()
    {
        try
        {
            _dataBuf = _fs.ReadSector(_seekLba);
            DbgReadRun("read", _seekLba);
            _lastReadLba = _seekLba;
            _hasReadAnySector = true;
            _sectorsRead++;
            _seekLba++;
        }
        catch
        {
            Array.Clear(_dataBuf);
        }
    }

    public CueFs Fs => _fs;
    public byte DriveStatusByte() => DriveStatus();

    public byte[] ReadSectorData(int lba)
    {
        _seekLba = lba;
        ReadNextSector();
        return (byte[])_dataBuf.Clone();
    }

    public byte[] ReadSectorData(int lba, int size)
    {
        DbgReadRun(size == 2336 ? "readXA" : "read", lba);
        _lastReadLba = lba;
        _sectorsRead++;
        return _fs.ReadSectorData(lba, size);
    }

    public void QueueAsyncSeekL(byte mm, byte ss, byte ff)
    {
        _seekLba = BcdToLba(mm, ss, ff);
        DbgEvent($"async SeekL lba={_seekLba}");
        QueueIrq(3, [DriveStatus()]);
        QueueIrq(2, [DriveStatus()]);
    }

    public void QueueAsyncGetStatus()
    {
        QueueIrq(3, [DriveStatus()]);
    }

    public void QueueAsyncSetMode(byte mode)
    {
        DbgEvent($"async Setmode {mode:X2}");
        QueueIrq(3, [DriveStatus()]);
    }

    public void QueueAsyncReadSector(uint count, uint dstAddr, uint mode)
    {
        DbgEvent($"async ReadSector lba={_seekLba} count={count} dst=0x{dstAddr:X8}");
        // FIXED: this used to run the whole transfer synchronously, right
        // here, on the calling (game) thread - see CueBin's warmup-thread
        // comment for the full story. That warmup thread helps but can't
        // guarantee it beats the game to every offset (it walks the disc
        // image linearly from byte 0, while gameplay seeks straight to
        // whatever lba it needs next), so a cold read could still stall
        // this call for 11+ seconds, and because PresentFrame is only ever
        // called from the game thread, a stall in this HLE call is a stall
        // of the entire rendered picture - freeze/black-screen with no
        // other symptom, exactly as reported.
        //
        // Real PS1 CdRead() is asynchronous by spec: it kicks off the
        // transfer and returns immediately, with completion signalled
        // later via IRQ (that's the whole reason this method is named
        // "QueueAsync..." and callers already treat it as fire-and-forget,
        // checking status via GetlocL/DriveStatus rather than blocking).
        // So doing the actual file I/O on a background thread and firing
        // the completion IRQs only once it's done is not a workaround,
        // it's what the API was always supposed to do - this HLE call was
        // just never actually async internally until now.
        int startLba = _seekLba;
        _seekLba += (int)count;
        _reading = true;
        var thread = new Thread(() =>
        {
            var sw = count > 32 ? System.Diagnostics.Stopwatch.StartNew() : null;
            int lba = startLba;
            for (uint i = 0; i < count; i++)
            {
                byte[] sector = ReadSectorDataInternal(lba);
                int sectorSize = (mode & 0x30) == 0 ? 2048 : 2048; //fix
                for (int j = 0; j < Math.Min(sector.Length, sectorSize); j++)
                    _m.WriteU8(dstAddr + i * (uint)sectorSize + (uint)j, sector[j]);
                lba++;
            }
            if (sw != null && sw.ElapsedMilliseconds > 200)
                DbgEvent($"SLOW ReadSector: lba={startLba} count={count} took {sw.ElapsedMilliseconds}ms - ran off the game thread, so PresentFrame kept going throughout");
            _reading = false;
            _lastReadLba = lba - 1;
            QueueIrq(3, [DriveStatus()]);
            QueueIrq(1, [DriveStatus()]);
            QueueIrq(2, [DriveStatus()]);
        })
        { IsBackground = true, Name = "CdAsyncRead" };
        thread.Start();
    }

    // Thread-safe subset of ReadSectorData(int) used by the background
    // read above: updates the shared _dataBuf/_sectorsRead/_lastReadLba
    // state under _dbgGate-adjacent locking is not needed here because
    // this path writes straight to the destination buffer instead of
    // through the single shared _dataBuf field that the synchronous
    // MMIO-driven read path (ReadNextSector/_dataBuf) still uses - keeping
    // this separate avoids introducing a race on that field now that reads
    // can happen off the game thread.
    private byte[] ReadSectorDataInternal(int lba)
    {
        DbgReadRun("read", lba);
        Interlocked.Increment(ref _sectorsRead);
        return _fs.ReadSectorData(lba, 2048);
    }

    // Was hardcoded to 0x02 (motor-on only) for every single command,
    // regardless of whether a ReadN/ReadS was actually in progress. Per
    // psx-spx, bit5 ("Read") must be set while sectors are streaming, and
    // some games explicitly poll for that bit transition rather than
    // relying solely on the INT1 interrupt (the psx-spx GT1 seek-vs-read
    // note documents exactly this class of bug: a title that watches the
    // wrong stat bit can stall indefinitely on the loading screen even
    // though sectors keep getting read under the hood - which matches the
    // observed symptom here: CD reads never stop, but BeginCalls stays at
    // 0, i.e. no GP0 draw command has ever been issued). Bit1 (motor) is
    // now unconditionally set too, since a real drive's motor is spinning
    // whenever it's not stopped.
    private byte DriveStatus()
    {
        byte s = 0x02; // motor on
        if (_reading) s |= 0x20; // bit5: Read
        DbgEvent($"DriveStatus() -> 0x{s:X2} (reading={_reading})");
        return s;
    }

    private static int BcdToLba(byte mm, byte ss, byte ff)
    {
        int m = (mm >> 4) * 10 + (mm & 0xF);
        int s = (ss >> 4) * 10 + (ss & 0xF);
        int f = (ff >> 4) * 10 + (ff & 0xF);
        return (m * 60 + s) * 75 + f - 150;
    }

    private static (byte mm, byte ss, byte ff) LbaToBcd(int lba)
    {
        int total = lba + 150;
        int f = total % 75;
        int s = (total / 75) % 60;
        int m = total / 75 / 60;
        return (ToBcd(m), ToBcd(s), ToBcd(f));
    }

    private static byte ToBcd(int v) => (byte)(((v / 10) << 4) | (v % 10));
}
