namespace CrashBandicoot.IosHost;

/// <summary>
/// Appends a line to Documents/checkpoint.log via raw POSIX open/write/close,
/// deliberately bypassing every managed I/O and logging path
/// (File.AppendAllText, NSLog, SessionLog, etc). The point: if the Mono
/// runtime, GC, or any managed subsystem is what's crashing, a managed
/// logger crashes right along with it and you're back to silent failures. A
/// raw POSIX write from a P/Invoke has the fewest possible moving parts
/// between "something went wrong" and "there is a line about it in a file
/// you can pull off the device".
///
/// Extracted from GameViewController.Checkpoint into its own file so
/// IosPlatformHost (and anything else on the render thread) can log
/// per-frame progress too - the previous version only logged at RunGame's
/// entry/exit, which left the entire span of Recompiled.Entry.Run (the
/// whole gameplay session, potentially hours) as one black box. Every prior
/// field crash report caught crash-game-main deep inside that call with no
/// way to tell which phase of which frame it was on when the process
/// aborted. Log(...) below is meant to be called from inside the per-frame
/// Present() path so the last line in checkpoint.log after a crash pinpoints
/// the exact phase (GL present vs swap vs a periodic heartbeat with a frame
/// counter) instead of just "somewhere in Entry.Run".
/// </summary>
static class DiskLog
{
    const int O_WRONLY = 0x0001;
    const int O_CREAT = 0x0200;
    const int O_APPEND = 0x0008;

    [System.Runtime.InteropServices.DllImport("libc", SetLastError = true)]
    static extern int open(string path, int flags, int mode);
    [System.Runtime.InteropServices.DllImport("libc")]
    static extern IntPtr write(int fd, IntPtr buf, UIntPtr count);
    [System.Runtime.InteropServices.DllImport("libc")]
    static extern int close(int fd);
    [System.Runtime.InteropServices.DllImport("libc")]
    static extern int pthread_threadid_np(IntPtr thread, out ulong threadId);

    public static void Log(string stage)
    {
        try
        {
            pthread_threadid_np(IntPtr.Zero, out var tid);
            var docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            var path = System.IO.Path.Combine(docs, "checkpoint.log");
            var line = $"{DateTime.UtcNow:HH:mm:ss.fff} [tid={tid}] {stage}\n";
            var bytes = System.Text.Encoding.UTF8.GetBytes(line);
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0x1A4 /* 0644 */);
            if (fd < 0) return;
            unsafe
            {
                fixed (byte* p = bytes)
                    write(fd, (IntPtr)p, (UIntPtr)bytes.Length);
            }
            close(fd);
        }
        catch
        {
            // Logging must never itself be the thing that crashes.
        }
    }
}
