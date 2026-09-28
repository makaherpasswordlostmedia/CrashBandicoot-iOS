using System.Runtime.InteropServices;
using System.Text;
using Foundation;

namespace CrashBandicoot.IosHost;

/// <summary>
/// Durable on-device log: appends every line to checkpoint.log via libc
/// fopen/fwrite/fflush (no managed FileStream, no Console), and mirrors each
/// line to NSLog so it also shows up in Console.app / idevicesyslog.
///
/// WHY THE OLD VERSION PRODUCED "NO LOGS":
/// it called libc open(path, flags, mode) through P/Invoke. open() is a
/// C *variadic* function, and on Apple arm64 variadic arguments are passed on
/// the STACK, not in registers - so the callee never saw our 0644 and read
/// garbage instead. The file was created with a random mode (often without
/// the owner-write bit), the very next open(O_WRONLY) failed with EACCES, and
/// the "if (fd &lt; 0) return;" swallowed it: an empty or unreadable
/// checkpoint.log and not a single error anywhere. fopen() is not variadic,
/// so it does not have this problem. (For the same reason NSLog is only ever
/// called below with a pre-escaped format string and ZERO variadic args.)
///
/// Other hardening:
///  - tries several directories (Documents via NSFileManager, Documents via
///    Environment, tmp, /var/mobile/Documents for no-sandbox installs) until
///    one is writable, and reports which one won;
///  - keeps the FILE* open and fflush()es after every line (data is in the
///    kernel before any crash, without an open/close per line);
///  - flood guard: a per-second budget for ordinary lines so verbose
///    [GPU]/[CD]/[SDK] categories cannot starve the game thread, while
///    [FATAL]/[STALL]/EXCEPTION/FAILED lines always bypass it;
///  - size cap with rotation (checkpoint.log -> checkpoint.prev.log);
///  - every failure goes to NSLog and is exposed via <see cref="Status"/> so
///    it can be shown ON SCREEN.
/// </summary>
static class DiskLog
{
    const string Libc = "/usr/lib/libSystem.dylib";
    const string FoundationLib = "/System/Library/Frameworks/Foundation.framework/Foundation";
    const string FileName = "checkpoint.log";
    const string PrevFileName = "checkpoint.prev.log";
    const long MaxFileBytes = 8L * 1024 * 1024;
    const int MaxOrdinaryLinesPerSecond = 400;

    [DllImport(Libc, SetLastError = true)] static extern IntPtr fopen(string path, string mode);
    [DllImport(Libc)] static extern unsafe nuint fwrite(byte* ptr, nuint size, nuint count, IntPtr stream);
    [DllImport(Libc)] static extern int fflush(IntPtr stream);
    [DllImport(Libc)] static extern int fclose(IntPtr stream);
    [DllImport(Libc)] static extern int pthread_threadid_np(IntPtr thread, out ulong threadId);
    [DllImport(Libc, SetLastError = true)] static extern int rename(string oldPath, string newPath);
    // Called with exactly one argument (already-escaped format) - see class doc.
    [DllImport(FoundationLib, EntryPoint = "NSLog")] static extern void NSLogNative(IntPtr format);

    static readonly object Gate = new();
    static IntPtr _file;
    static string? _path;
    static string _lastError = "not initialised";
    static long _bytesWritten;
    static int _linesThisSecond;
    static long _secondStamp;
    static long _suppressed;
    static long _totalLines;
    static bool _initTried;

    /// <summary>Full path of the file currently being written, or null if no location was writable.</summary>
    public static string? ActivePath { get { lock (Gate) return _path; } }

    /// <summary>One-line summary for the on-screen overlay: where logs go, or why they can't.</summary>
    public static string Status
    {
        get
        {
            lock (Gate)
                return _file != IntPtr.Zero
                    ? $"log OK ({_totalLines} lines, {_bytesWritten / 1024} KB): {ShortPath(_path)}"
                    : $"LOG BROKEN: {_lastError}";
        }
    }

    static string ShortPath(string? p)
    {
        if (p == null) return "?";
        var i = p.IndexOf("/Documents/", StringComparison.Ordinal);
        return i >= 0 ? p[(i + 1)..] : p;
    }

    public static void Log(string stage)
    {
        try
        {
            var critical = IsCritical(stage);
            lock (Gate)
            {
                _totalLines++;
                if (!_initTried) OpenLocked();

                if (!critical)
                {
                    var sec = Environment.TickCount64 / 1000;
                    if (sec != _secondStamp)
                    {
                        if (_suppressed > 0)
                            WriteLocked($"[LOG] suppressed {_suppressed} lines in the previous second (flood guard, {MaxOrdinaryLinesPerSecond}/s)");
                        _suppressed = 0;
                        _secondStamp = sec;
                        _linesThisSecond = 0;
                    }
                    if (++_linesThisSecond > MaxOrdinaryLinesPerSecond)
                    {
                        _suppressed++;
                        return;
                    }
                }
                WriteLocked(stage);
            }
        }
        catch
        {
            // Logging must never itself be the thing that crashes.
        }
    }

    static bool IsCritical(string s) =>
        s.StartsWith("[FATAL]", StringComparison.Ordinal) ||
        s.StartsWith("[STALL]", StringComparison.Ordinal) ||
        s.StartsWith("[LOG]", StringComparison.Ordinal) ||
        s.Contains("EXCEPTION", StringComparison.Ordinal) ||
        s.Contains("FAILED", StringComparison.Ordinal) ||
        s.Contains("STALLED", StringComparison.Ordinal) ||
        s.Contains("BURST", StringComparison.Ordinal);

    static void WriteLocked(string text)
    {
        pthread_threadid_np(IntPtr.Zero, out var tid);
        var line = $"{DateTime.UtcNow:HH:mm:ss.fff} [tid={tid}] {text}";

        // Mirror to the system log: reaches Console.app / idevicesyslog even
        // if every file location turns out to be unwritable.
        NSLogSafe(line);

        if (_file == IntPtr.Zero) return;

        var bytes = Encoding.UTF8.GetBytes(line + "\n");
        unsafe
        {
            fixed (byte* p = bytes)
            {
                var written = fwrite(p, 1, (nuint)bytes.Length, _file);
                if ((int)written != bytes.Length)
                {
                    _lastError = $"fwrite short write ({written}/{bytes.Length}) errno={Marshal.GetLastWin32Error()}";
                    NSLogSafe($"[DiskLog] {_lastError}");
                }
            }
        }
        fflush(_file);
        _bytesWritten += bytes.Length;

        if (_bytesWritten > MaxFileBytes) RotateLocked();
    }

    static void RotateLocked()
    {
        if (_path == null) return;
        var path = _path;
        fclose(_file);
        _file = IntPtr.Zero;
        var prev = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(path)!, PrevFileName);
        rename(path, prev);
        _file = fopen(path, "a");
        _bytesWritten = 0;
        if (_file == IntPtr.Zero)
        {
            _lastError = $"reopen after rotation failed errno={Marshal.GetLastWin32Error()}";
            NSLogSafe($"[DiskLog] {_lastError}");
        }
        else
        {
            WriteLocked("[LOG] rotated: previous log is checkpoint.prev.log");
        }
    }

    static void OpenLocked()
    {
        _initTried = true;
        var attempts = new StringBuilder();

        foreach (var dir in CandidateDirectories())
        {
            if (string.IsNullOrEmpty(dir)) continue;
            try { System.IO.Directory.CreateDirectory(dir); } catch { /* fopen below reports the real failure */ }

            var path = System.IO.Path.Combine(dir, FileName);
            var f = fopen(path, "a");
            if (f != IntPtr.Zero)
            {
                _file = f;
                _path = path;
                _lastError = "ok";
                _bytesWritten = 0;
                WriteLocked("==================== new session ====================");
                WriteLocked($"[LOG] writing to {path}" +
                            (attempts.Length > 0 ? $" (earlier locations failed: {attempts})" : ""));
                try
                {
                    WriteLocked($"[LOG] device={UIKit.UIDevice.CurrentDevice.Model} os={UIKit.UIDevice.CurrentDevice.SystemVersion} " +
                                $"runtime={RuntimeInformation.FrameworkDescription} arch={RuntimeInformation.ProcessArchitecture} " +
                                $"home={Environment.GetEnvironmentVariable("HOME")}");
                }
                catch { /* UIKit not ready this early - not fatal */ }
                return;
            }

            attempts.Append($"{dir} (errno {Marshal.GetLastWin32Error()}); ");
        }

        _lastError = $"no writable location: {attempts}";
        NSLogSafe($"[DiskLog] {_lastError}");
    }

    static IEnumerable<string?> CandidateDirectories()
    {
        string? nsDocs = null;
        try
        {
            nsDocs = NSFileManager.DefaultManager
                .GetUrls(NSSearchPathDirectory.DocumentDirectory, NSSearchPathDomain.User)
                .FirstOrDefault()?.Path;
        }
        catch { }
        yield return nsDocs;

        string? envDocs = null;
        try { envDocs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments); } catch { }
        if (envDocs != nsDocs) yield return envDocs;

        string? tmp = null;
        try { tmp = System.IO.Path.GetTempPath(); } catch { }
        yield return tmp;

        // No-sandbox (TrollStore) processes can write here even when the
        // container path can't be resolved.
        yield return "/var/mobile/Documents";
    }

    /// <summary>NSLog with the message escaped into the format string (no variadic args - see class doc).</summary>
    static void NSLogSafe(string message)
    {
        try
        {
            using var fmt = new NSString(message.Replace("%", "%%"));
            NSLogNative(fmt.Handle);
        }
        catch { }
    }
}
