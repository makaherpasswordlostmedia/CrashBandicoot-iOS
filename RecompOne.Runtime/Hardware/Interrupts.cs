using RecompOne.Runtime.Bios;
using RecompOne.Runtime.Context;
using RecompOne.Runtime.Dispatch;
using RecompOne.Runtime.Memory;

namespace RecompOne.Runtime;

public static class Interrupts
{
    // Diagnostics for the black-screen investigation: PresentFrame() calls
    // DispatchIrq(0) (IRQ0 = VBlank) unconditionally every single host
    // frame - it's the *only* path that ever runs the game's VBlank
    // handler. If IntrEnvInInterruptAddr is still 0 (BIOS B(19h) -
    // "SetIntrEnv" or equivalent - never called yet) or the handler slot
    // for this irq is 0 (registered for a different irq number, or not
    // registered at all), Deliver below silently no-ops - every single
    // VBlank is dropped on the floor with zero indication anywhere. A game
    // whose main loop synchronizes on VBlank (extremely common on PS1 -
    // "wait for vsync, then draw") would sit there forever calling
    // PresentFrame each host frame (explaining the healthy, climbing
    // PresentFrameCalls counter with zero real progress) while never
    // reaching the GP0 draw calls gated behind that wait - which is exactly
    // the begin=0-after-300-frames symptom from the last investigation
    // pass. Logged once per distinct (irq, intrEnv, handler) combination
    // rather than every frame, so a healthy 60x/sec VBlank delivery doesn't
    // flood checkpoint.log once it *is* working.
    static (int irq, uint intrEnv, uint handler) _lastLoggedMiss = (-1, 0, 0);
    static bool _everDelivered;

    /// <summary>
    /// Total successful (non-dropped) deliveries per IRQ line, index by irq
    /// number (0=VBlank, 1=GPU, 2=CDROM, ...). Exposed so a host's periodic
    /// diagnostic dump (see IosPlatformHost.Present's verbose CD-state log)
    /// can report "VBlank delivered N times" alongside frame/CD state - a
    /// count that's climbing steadily but the game still isn't drawing
    /// points at the handler *running* but not doing the expected work,
    /// versus a count stuck at 0 pointing at Deliver's drop paths above.
    /// </summary>
    public static readonly long[] DeliveredCount = new long[8];

    public static void Deliver(int irq, CpuContext cpu, IMemory mem)
    {
        uint intrEnv = BiosB.IntrEnvInInterruptAddr;
        if (intrEnv == 0)
        {
            LogMissOnce(irq, 0, 0, "IntrEnvInInterruptAddr is 0 - BIOS B(19h)/SetIntrEnv never called yet, no interrupt table exists at all");
            return;
        }

        uint handler = mem.ReadU32(intrEnv + 2u + (uint)irq * 4u);
        if (handler == 0)
        {
            LogMissOnce(irq, intrEnv, 0, $"handler slot for irq={irq} at intrEnv+2+irq*4=0x{intrEnv + 2u + (uint)irq * 4u:X8} is 0 - table exists but nothing registered for this irq specifically");
            return;
        }

        if (!_everDelivered)
        {
            _everDelivered = true;
            RecompOne.Runtime.Log.Sink?.Invoke($"[IRQ] Interrupts.Deliver: FIRST successful delivery, irq={irq}, intrEnv=0x{intrEnv:X8}, handler=0x{handler:X8}");
        }

        //takes a snap, apparently interrupt callbacks dont operate at the same context? could be wrong in mips3000, need to check furter TODO, seens to be accurate
        var snap = cpu.Snapshot();
        mem.WriteU16(intrEnv, 1);
        Dispatcher.Call(cpu, mem, handler);
        mem.WriteU16(intrEnv, 0);
        cpu.Restore(snap);
        if (irq >= 0 && irq < DeliveredCount.Length) DeliveredCount[irq]++;
    }

    static void LogMissOnce(int irq, uint intrEnv, uint handler, string reason)
    {
        var key = (irq, intrEnv, handler);
        if (_lastLoggedMiss == key) return;
        _lastLoggedMiss = key;
        // Log.Sink directly (not the gated Log.Bios helper) - this
        // diagnostic is too load-bearing to be silently invisible whenever
        // Log.BiosOn happens to be false (its default, and there is no iOS
        // UI to flip it - that toggle only exists in
        // AndroidRuntimeHost/DevMenuOverlay.Debug.cs). Every VBlank drop is
        // exactly the kind of silent failure this whole investigation keeps
        // running into; it must always reach checkpoint.log.
        RecompOne.Runtime.Log.Sink?.Invoke($"[IRQ] Interrupts.Deliver: DROPPED irq={irq} - {reason}");
    }
}
