namespace RecompOne.Runtime.Cdrom;

public sealed class CueBin : IDisposable
{
    private record Track(string BinPath, int Number, string Mode, int SectorSize, int DataOffset, long FileOffset);

    private readonly List<Track> _tracks = [];
    private readonly Dictionary<string, FileStream> _files = [];
    private readonly object _ioGate = new();

    private CueBin() {}

    public static CueBin Open(string cuePath)
    {
        var cb = new CueBin();
        cb.Parse(cuePath);
        cb.StartBackgroundWarmup();
        return cb;
    }

    // Root cause of the multi-second freezes/black-screen reports: the
    // first read of a given region of the .bin is dramatically slower than
    // later reads of the same region (measured ~90ms/sector cold vs
    // ~0.25ms/sector warm - a 128-sector cold read blocked the game thread
    // for 11+ seconds in captured logs, since CdRead (LibCd.cs) runs the
    // whole transfer synchronously on the calling thread and PresentFrame
    // can't be called again until it returns). Most likely cause: iOS Data
    // Protection decryption and/or page cache population happening lazily
    // per-region rather than for the whole file at open time.
    //
    // Fix: walk the whole data track on background threads right after
    // opening, so by the time gameplay's own seeks reach a given offset
    // the pages are already warm.
    //
    // FIXED: this used to be a single thread walking strictly from byte 0,
    // which for every session in the field lost the race against gameplay
    // to a specific hot region around lba 55919 - every captured trace
    // shows gameplay reaching that lba roughly 10-15s after disc open,
    // which was not enough time for a single-threaded sequential warmup to
    // get there first on a multi-hundred-MB image, so the "warm" read
    // still hit cold pages. Splitting the file into N equal-sized ranges
    // and warming them concurrently (N = a handful of background threads)
    // cuts wall-clock warmup time roughly by that factor for any given
    // offset, which is what actually matters here - we don't care about
    // total warmup completion time, we care about time-to-warm for
    // whichever offset gameplay reaches first, and concurrent range
    // coverage gets every offset warm sooner than a single sweep can.
    // Threads share the same _ioGate as real reads, so a real read only
    // ever waits behind a single in-flight warmup chunk (64KB), never the
    // whole file.
    private void StartBackgroundWarmup()
    {
        const int warmupThreads = 4;
        long totalLength;
        lock (_ioGate) totalLength = _tracks.Count > 0 ? GetStream(_tracks[0].BinPath).Length : 0;
        if (totalLength <= 0) return;

        // Marker line: if this never appears in checkpoint.log, the build
        // on device predates the concurrent-warmup fix (or _tracks was
        // somehow empty) - the single most reliable way to tell "did my
        // fix actually ship" apart from "did the fix not work" from a log
        // alone, since both look identical if you only look at CdRead
        // timings.
        RecompOne.Runtime.Log.Cd($"CueBin: warmup started, {warmupThreads} threads, {totalLength} bytes total");

        long perThread = totalLength / warmupThreads;
        for (int t = 0; t < warmupThreads; t++)
        {
            long start = t * perThread;
            long end = t == warmupThreads - 1 ? totalLength : start + perThread;
            var thread = new Thread(() => WarmupRange(start, end))
            { IsBackground = true, Name = $"CueBin-Warmup-{t}" };
            thread.Start();
        }
    }

    private void WarmupRange(long start, long end)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            foreach (var t in _tracks)
            {
                var stream = GetStream(t.BinPath);
                const int chunk = 64 * 1024; // small enough that a real read never waits long behind a warmup chunk
                var buf = new byte[chunk];
                long remaining;
                lock (_ioGate) remaining = stream.Length;
                long pos = Math.Min(start, remaining);
                long rangeEnd = Math.Min(end, remaining);
                while (pos < rangeEnd)
                {
                    int want = (int)Math.Min(chunk, rangeEnd - pos);
                    lock (_ioGate)
                    {
                        stream.Seek(pos, SeekOrigin.Begin);
                        stream.ReadExactly(buf, 0, want);
                    }
                    pos += want;
                }
            }
            RecompOne.Runtime.Log.Cd($"CueBin: warmup range [{start}..{end}) done in {sw.ElapsedMilliseconds}ms");
        }
        catch
        {
            // Best-effort warmup only - a failure here (disposed mid-read,
            // I/O error) must never surface as a game-facing error since
            // nothing depends on this thread completing.
        }
    }

    private void Parse(string cuePath)
    {
        string dir = Path.GetDirectoryName(Path.GetFullPath(cuePath)) ?? "";
        string? currentFile = null;
        int trackNum = 0;
        string mode = "MODE2/2352";

        foreach (var raw in File.ReadLines(cuePath))
        {
            var line = raw.Trim();
            if (line.StartsWith("FILE ", StringComparison.OrdinalIgnoreCase))
            {
                int a = line.IndexOf('"') + 1;
                int b = line.LastIndexOf('"');
                currentFile = Path.Combine(dir, line[a..b]);
            }
            else if (line.StartsWith("TRACK ", StringComparison.OrdinalIgnoreCase))
            {
                var p = line.Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
                trackNum = int.Parse(p[1]);
                mode = p[2];
            }
            else if (line.StartsWith("INDEX 01 ", StringComparison.OrdinalIgnoreCase))
            {
                long sectors = MsfToSectors(line[9..].Trim());
                int ss = GetSectorSize(mode);
                _tracks.Add(new Track(currentFile!, trackNum, mode, ss, GetDataOffset(mode), sectors * ss));
            }
        }
    }

    public byte[] ReadSector(int lba) => ReadSectorData(lba, 2048);

    public byte[] ReadSectorData(int lba, int size)
    {
        var t = DataTrack();
        var stream = GetStream(t.BinPath);
        int offset = t.SectorSize == 2352
            ? size switch { >= 2340 => 12, >= 2329 => 16, _ => 24 }
            : t.DataOffset;
        long pos = t.FileOffset + (long)lba * t.SectorSize + offset;
        var buf = new byte[size];
        if (lba < 0) return buf;
        int want = Math.Min(size, t.SectorSize - offset);
        lock (_ioGate)
        {
            if (pos >= stream.Length) return buf;
            int avail = (int)Math.Min(want, stream.Length - pos);
            stream.Seek(pos, SeekOrigin.Begin);
            stream.ReadExactly(buf, 0, avail);
        }
        return buf;
    }

    private Track DataTrack() => _tracks.Find(t => !t.Mode.Equals("AUDIO", StringComparison.OrdinalIgnoreCase)) ?? throw new InvalidOperationException("no data track was found in cue sheet");

    private FileStream GetStream(string path)
    {
        lock (_ioGate)
        {
            if (!_files.TryGetValue(path, out var s))
                _files[path] = s = File.OpenRead(path);
            return s;
        }
    }

    private static long MsfToSectors(string msf)
    {
        var p = msf.Split(':');
        return long.Parse(p[0]) * 60 * 75 + long.Parse(p[1]) * 75 + long.Parse(p[2]);
    }

    private static int GetSectorSize(string mode) => mode switch
    {
        "MODE1/2048" => 2048,
        "MODE2/2336" => 2336,
        _ => 2352,
    };

    private static int GetDataOffset(string mode) => mode switch
    {
        "MODE1/2352" => 16,
        "MODE2/2352" => 24,
        "MODE2/2336" => 8,
        _ => 0,
    };

    public void Dispose()
    {
        foreach (var s in _files.Values) s.Dispose();
        _files.Clear();
    }
}
