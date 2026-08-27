namespace RecompOne.Runtime;

public static class Log
{
    public static bool BiosOn = false;
    public static bool SpuOn = false;
    public static bool GpuOn = false;
    public static bool DmaOn = false;
    public static bool CdOn = false;
    public static bool SdkOn = false;
    public static bool MdecOn = false;
    public static bool OverlayOn = false;

    // Console.WriteLine on iOS writes to stdout, which is not visible
    // anywhere without an attached debugger/Xcode console session - so on
    // a device installed via TrollStore, every one of these category logs
    // was silently discarded. Platform hosts that have their own durable
    // logging (e.g. CrashBandicoot.IosHost's DiskLog, which appends to
    // Documents/checkpoint.log) can assign this to also route category
    // logs there. Left null by default so non-iOS hosts (desktop/Android,
    // which do have a visible console) see no behavior change.
    public static Action<string>? Sink;

    static void Emit(string line)
    {
        Console.WriteLine(line);
        Sink?.Invoke(line);
    }

    public static void Mdec(string m)
    {
        if (MdecOn) Emit($"[MDEC] {m}");
    }

    public static void Bios(string m)
    {
        if (BiosOn) Emit($"[BIOS] {m}");
    }

    public static void Spu(string m)
    {
        if (SpuOn) Emit($"[SPU] {m}");
    }

    public static void Gpu(string m)
    {
        if (GpuOn) Emit($"[GPU] {m}");
    }

    public static void Dma(string m)
    {
        if (DmaOn) Emit($"[DMA] {m}");
    }

    public static void Cd(string m)
    {
        if (CdOn) Emit($"[CD] {m}");
    }

    public static void Sdk(string m)
    {
        if (SdkOn) Emit($"[SDK] {m}");
    }

    // Console.WriteLine("[Dispatcher] ...") calls elsewhere in Dispatcher.cs
    // predate this Sink mechanism and still go straight to Console.WriteLine,
    // so on iOS/TrollStore (no attached console) they were invisible in
    // checkpoint.log same as every other category before Sink existed - this
    // gives overlay loading a route into the same durable log the CD/SDK
    // traces already use, on by default since it's the thing currently being
    // diagnosed (a black screen caused by an overlay never loading).
    public static bool OverlayOnDefaultTrue = true;
    public static void Overlay(string m)
    {
        if (OverlayOn || OverlayOnDefaultTrue) Emit($"[OVERLAY] {m}");
    }
}
