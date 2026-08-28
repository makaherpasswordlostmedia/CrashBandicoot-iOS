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
    // for 11+ seconds in captured logs, since QueueAsyncReadSector runs
    // synchronously on that thread and PresentFrame can't be called again
    // until it returns). Most likely cause: iOS Data Protection decryption
    // and/or page cache population happening lazily per-region rather than
    // for the whole file at open time.
    // Fix: walk the whole data track sequentially on a background thread
    // right after opening, so by the time gameplay's own seeks reach a
    // given offset the pages are already warm. This thread reads through
    // the same _ioGate lock as real reads, so it never races a real read -
    // worst case a real read waits briefly behind a warmup read of the
    // same file, which is still far better than the multi-second stalls
    // this replaces. Deliberately fire-and-forget: if it doesn't finish
    // before the disc does, that's fine, it was pure readahead.
    private void StartBackgroundWarmup()
    {
        var thread = new Thread(() =>
        {
            try
            {
                foreach (var t in _tracks)
                {
                    var stream = GetStream(t.BinPath);
                    const int chunk = 64 * 1024; // small enough that a real read never waits long behind a warmup chunk
                    var buf = new byte[chunk];
                    long remaining;
                    lock (_ioGate) remaining = stream.Length;
                    long pos = 0;
                    while (pos < remaining)
                    {
                        int want = (int)Math.Min(chunk, remaining - pos);
                        lock (_ioGate)
                        {
                            stream.Seek(pos, SeekOrigin.Begin);
                            stream.ReadExactly(buf, 0, want);
                        }
                        pos += want;
                    }
                }
            }
            catch
            {
                // Best-effort warmup only - a failure here (disposed mid-read,
                // I/O error) must never surface as a game-facing error since
                // nothing depends on this thread completing.
            }
        })
        { IsBackground = true, Name = "CueBin-Warmup" };
        thread.Start();
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
