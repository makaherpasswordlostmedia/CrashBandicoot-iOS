using RecompOne.Runtime.Context;
using RecompOne.Runtime.Events;
using RecompOne.Runtime.Memory;
using BiosKernel = RecompOne.Runtime.Bios.Bios;
namespace RecompOne.Runtime.Dispatch;

public static class Dispatcher
{
    static readonly OverlayLoadedEvent _overlayEvent = new();
    static readonly Dictionary<string, IOverlay> _registry = new(StringComparer.OrdinalIgnoreCase);
    static readonly Dictionary<int, string> _lbaToName = [];
    static readonly List<string> _active = [];
    static readonly Dictionary<uint, Action<CpuContext, IMemory>> _funcMap = [];
    private static IOverlay? _pending;
    public static void Register(string name, IOverlay overlay)
    {
        _registry[name] = overlay;
        if (overlay.LbaStart >= 0) _lbaToName[overlay.LbaStart] = name;
    }

    public static string[] ActiveNames
    {
        get { lock (_active) return _active.ToArray(); }
    }
    
    public static IReadOnlyDictionary<string, IOverlay> Overlays => _registry;

    public static void LoadByLba(int lba)
    {
        if (!_lbaToName.TryGetValue(lba, out var name)) return;
        var overlay = _registry[name];
        if(overlay.Base == 0) {
            Log.Overlay($"LoadByLba: lba={lba} -> '{name}' has no Base, loading immediately");
            Load(name);
            return;
        }

        Log.Overlay($"LoadByLba: lba={lba} -> '{name}' pending, waiting for write into 0x{overlay.Base:X8}..0x{overlay.Base + overlay.Size:X8}");
        _pending = overlay;
    }

    public static void NotifyWrite(uint phys, uint size = 1)
    {
        var p = _pending;
        if (p == null) return;
        uint start = p.Base & 0x1FFFFFFFu;
        // Was hardcoded to start + 0x800 (2KB) - a leftover guess from before
        // overlays carried their own Size. Real overlays are the full CD
        // binary length (OverlayWriter emits Size = discBin.Length), commonly
        // tens to hundreds of KB, so a 2KB window could miss a write that
        // lands anywhere past the first 2KB of the overlay's own range -
        // exactly the failure mode the interval-overlap fix above was
        // supposed to close. Use the overlay's actual size instead.
        uint overlaySize = p.Size > 0 ? p.Size : 0x800u;
        uint end = start + overlaySize;
        // Overlap test between the written range [phys, phys+size) and the
        // pending overlay's load window [start, end), not a single-point
        // containment check - a bulk CD-read write (LoadBytes) that starts
        // before the overlay's address but extends into or across it must
        // still trigger the load. size defaults to 1 so single-byte/aligned
        // WriteU8/16/32 callers keep their previous point-check behavior.
        if (phys >= end || phys + size <= start)
        {
            Log.Overlay($"NotifyWrite: MISS write=[0x{phys:X8}..0x{phys + size:X8}) vs pending '{p.Name}' window=[0x{start:X8}..0x{end:X8})");
            return;
        }
        Log.Overlay($"NotifyWrite: HIT write=[0x{phys:X8}..0x{phys + size:X8}) overlaps pending '{p.Name}' window=[0x{start:X8}..0x{end:X8}) -> loading");
        _pending = null;
        Load(p.Name);
    }
    public static void ClearPending() => _pending = null;

    public static void Load(string name)
    {
        if (!_registry.TryGetValue(name, out var overlay))
            throw new KeyNotFoundException($"overlay not registered: {name}");

        bool already;
        lock (_active) already = _active.Remove(name);

        if (!already) HandleRegionOverwrites(overlay);

        lock (_active) _active.Add(name);
        foreach (var (addr, fn) in overlay.Functions)
            _funcMap[addr] = fn;

        if (already) return;
        Runtime.OverlayLog.Record(name, OverlayEventKind.Loaded);
        // Was Console.WriteLine only, which on iOS/TrollStore has no attached
        // console and is silently discarded - checkpoint.log never showed a
        // single "loaded overlay" line across any session, which is exactly
        // why this stall was invisible before. Route it through Log.Overlay
        // (durable, goes to checkpoint.log) instead/in addition.
        Log.Overlay($"loaded overlay: {name} ({overlay.Functions.Count} functions, base=0x{overlay.Base:X8}, size=0x{overlay.Size:X})");

        if (Event.HasAnyListeners<OverlayLoadedEvent>())
        {
            var e = _overlayEvent;
            e.Context = Runtime.Cpu!; e.Memory = Runtime.Mem!;
            e.Name = name;
            Event.Dispatch(e);
        }
    }

    static void HandleRegionOverwrites(IOverlay overlay)
    {
        uint newStart = overlay.Base & 0x1FFFFFFFu;
        uint newEnd = newStart + overlay.Size;
        bool hasRegion = overlay.Base != 0 && overlay.Size != 0;

        List<string>? overwritten = null;
        List<(string Name, int Funcs)>? vramCollisions = null;

        lock (_active)
        {
            foreach (var activeName in _active)
            {
                var other = _registry[activeName];
                bool otherHasRegion = other.Base != 0 && other.Size != 0;

                if (hasRegion && otherHasRegion)
                {
                    uint s = other.Base & 0x1FFFFFFFu;
                    uint e = s + other.Size;

                    if (s < newEnd && e > newStart)
                    {
                        if (s >= newStart && e <= newEnd)
                        {
                            overwritten ??= [];
                            overwritten.Add(activeName);
                        }
                        continue;
                    }
                }

                int shared = CountSharedFunctions(overlay, other);
                if (shared > 0)
                {
                    vramCollisions ??= [];
                    vramCollisions.Add((activeName, shared));
                }
            }

            if (overwritten != null)
                foreach (var d in overwritten) _active.Remove(d);
        }

        if (overwritten != null)
        {
            Rebuild();
            foreach (var d in overwritten)
            {
                Runtime.OverlayLog.Record(d, OverlayEventKind.Overwritten, overlay.Name);
                Log.Overlay($"overlay {d} overwritten by {overlay.Name}");
            }
        }

        if (vramCollisions != null)
        {
            foreach (var (otherName, n) in vramCollisions)
            {
                Runtime.OverlayLog.Record(overlay.Name, OverlayEventKind.VramCollision, $"{otherName} ({n} funcs)");
                Log.Overlay($"overlay {overlay.Name} vram collision with {otherName}: {n} functions");
            }
        }
    }

    static int CountSharedFunctions(IOverlay a, IOverlay b)
    {
        var smaller = a.Functions.Count <= b.Functions.Count ? a : b;
        var larger = ReferenceEquals(smaller, a) ? b : a;

        int n = 0;
        foreach (var addr in smaller.Functions.Keys)
            if (larger.Functions.ContainsKey(addr)) n++;
        return n;
    }

    public static void TryLoad(string name)
    {
        if (_registry.ContainsKey(name))
            Load(name);
    }

    public static void Unload(string name)
    {
        bool removed;
        lock (_active) removed = _active.Remove(name);
        if (!removed) return;
        Rebuild();
        Runtime.OverlayLog.Record(name, OverlayEventKind.Unloaded);
    }

    // Diagnostics for the black-screen investigation: "last function address
    // Dispatcher.Call actually reached" is the single most useful fact we
    // don't otherwise have once GlBackend.BeginCalls is stuck at 0 - it
    // tells us whether the game is still making normal calls (and if so,
    // which one it's stuck inside/before) versus never getting into
    // Dispatcher.Call at all after some point (which would instead point at
    // the CPU loop itself, or a BIOS HLE call that never returns).
    // Volatile: read from PresentFrame() on the render thread while written
    // from the emulated CPU thread.
    public static volatile uint LastCallAddr;
    public static long CallCount;

    public static void Call(CpuContext c, IMemory m, uint addr)
    {
        LastCallAddr = addr;
        CallCount++;
        Sdk.LibEtc.MaybeCatchUpVBlank();
        if (BiosKernel.TryDispatch(c, m, addr)) return;
        if (!_funcMap.TryGetValue(addr, out var fn))
            throw new InvalidOperationException($"unmapped call: 0x{addr:X8}");
        fn(c, m);
        HealGeomClobberedRegs(c, m);
    }

    /// <summary>
    /// Geom stubs save real GP/SP on scratchpad then reuse those regs as temps.
    /// A recompiler <c>return</c> instead of <c>jr</c> into a clip jump-table
    /// leaves GP as an AND mask and SP as a small index (often 0) — the next
    /// prologue then writes RA to SP+0x14 = 0xFFFFFFFC.
    /// </summary>
    internal static void HealGeomClobberedRegs(CpuContext c, IMemory m)
    {
        if (c.GP is 0x00FFFFFFu or 0x0000FFFFu)
            c.GP = 0x800563FCu;

        // Real SP lives in KUSEG/KSEG0 RAM; temps are tiny or scratchpad pointers.
        bool spLooksTemp = c.SP < 0x8000u
                           || (c.SP >= 0x1F800000u && c.SP < 0x1F800400u)
                           || c.SP is 0x02000000u or 0xFFFFFFDFu;
        if (!spLooksTemp) return;

        uint saved = m.ReadU32(0x1F800034u);
        if (saved >= 0x80000000u && saved < 0x80800000u)
            c.SP = saved;
    }

    static void Rebuild()
    {
        _funcMap.Clear();
        lock (_active)
        {
            foreach (var name in _active)
                foreach (var (addr, fn) in _registry[name].Functions)
                    _funcMap[addr] = fn;
        }
    }
}
