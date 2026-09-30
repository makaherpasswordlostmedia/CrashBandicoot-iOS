/* dispatch.c - Dispatcher, Interrupts, LibEtc(VSync), Runtime.PresentFrame,
 * FrameClock and pad glue ported from RecompOne.Runtime. */
#include "psx.h"
#include <pthread.h>

volatile uint32_t g_last_call_addr;
uint64_t g_call_count;
int64_t  g_present_calls;
volatile int g_skip_throttle;
volatile int g_paused;
int g_log_cd, g_log_sdk, g_log_bios, g_log_overlay = 1, g_log_gpu;

/* ------------------------------------------------------------------ */
/* Function map (open addressing, power of two)                         */
/* ------------------------------------------------------------------ */
#define MAX_OVERLAYS 16
#define FMAP_BITS 16
#define FMAP_SIZE (1u << FMAP_BITS)

static const OverlayDesc *g_registry[MAX_OVERLAYS];
static int g_registry_n;
static int g_active[MAX_OVERLAYS];      /* indices into registry, in load order */
static int g_active_n;
static const OverlayDesc *g_pending;
static FuncEntry g_fmap[FMAP_SIZE];      /* fn == NULL -> empty */

static inline uint32_t fhash(uint32_t a) { return ((a >> 2) * 2654435761u) >> (32 - FMAP_BITS); }

static void fmap_put(uint32_t addr, RecompFn fn)
{
    uint32_t h = fhash(addr);
    for (uint32_t n = 0; n < FMAP_SIZE; n++, h = (h + 1) & (FMAP_SIZE - 1)) {
        if (!g_fmap[h].fn) { g_fmap[h].addr = addr; g_fmap[h].fn = fn; return; }
        if (g_fmap[h].addr == addr) { g_fmap[h].fn = fn; return; }
    }
    plat_fatal("function map full");
}

static inline RecompFn fmap_get(uint32_t addr)
{
    uint32_t h = fhash(addr);
    for (;;) {
        const FuncEntry *e = &g_fmap[h];
        if (e->addr == addr && e->fn) return e->fn;
        if (!e->fn) return NULL;
        h = (h + 1) & (FMAP_SIZE - 1);
    }
}

static void fmap_rebuild(void)
{
    memset(g_fmap, 0, sizeof g_fmap);
    for (int i = 0; i < g_active_n; i++) {
        const OverlayDesc *o = g_registry[g_active[i]];
        for (uint32_t k = 0; k < o->nfuncs; k++) fmap_put(o->funcs[k].addr, o->funcs[k].fn);
    }
}

void disp_register(const OverlayDesc *ov)
{
    if (g_registry_n < MAX_OVERLAYS) g_registry[g_registry_n++] = ov;
}

static int find_registry(const char *name)
{
    for (int i = 0; i < g_registry_n; i++)
        if (strcasecmp(g_registry[i]->name, name) == 0) return i;
    return -1;
}

static int active_index(int reg)
{
    for (int i = 0; i < g_active_n; i++) if (g_active[i] == reg) return i;
    return -1;
}

void disp_load(const char *name)
{
    int reg = find_registry(name);
    if (reg < 0) { LOG("[OVERLAY] not registered: %s", name); return; }
    const OverlayDesc *ov = g_registry[reg];
    int ai = active_index(reg);
    bool already = ai >= 0;
    if (already) { memmove(&g_active[ai], &g_active[ai + 1], (size_t)(g_active_n - ai - 1) * sizeof(int)); g_active_n--; }

    if (!already && ov->base != 0 && ov->size != 0) {
        /* drop overlays that are entirely overwritten by this one */
        uint32_t ns = ov->base & 0x1FFFFFFFu, ne = ns + ov->size;
        for (int i = 0; i < g_active_n;) {
            const OverlayDesc *o = g_registry[g_active[i]];
            if (o->base && o->size) {
                uint32_t s = o->base & 0x1FFFFFFFu, e = s + o->size;
                if (s < ne && e > ns && s >= ns && e <= ne) {
                    LOGF(g_log_overlay, "[OVERLAY] %s overwritten by %s", o->name, ov->name);
                    memmove(&g_active[i], &g_active[i + 1], (size_t)(g_active_n - i - 1) * sizeof(int));
                    g_active_n--;
                    continue;
                }
            }
            i++;
        }
    }
    g_active[g_active_n++] = reg;
    if (!already) LOGF(g_log_overlay, "[OVERLAY] loaded overlay: %s (%u functions, base=0x%08X, size=0x%X)",
                       ov->name, ov->nfuncs, ov->base, ov->size);
    fmap_rebuild();
}

void disp_try_load(const char *name) { if (find_registry(name) >= 0) disp_load(name); }

void disp_load_by_lba(int lba)
{
    for (int i = 0; i < g_registry_n; i++) {
        const OverlayDesc *o = g_registry[i];
        if (o->lba_start != lba) continue;
        if (o->base == 0) { disp_load(o->name); return; }
        g_pending = o;
        g_ovl_pending_lo = o->base & 0x1FFFFFFFu;
        g_ovl_pending_hi = g_ovl_pending_lo + (o->size ? o->size : 0x800u);
        return;
    }
}

void disp_clear_pending(void) { g_pending = NULL; g_ovl_pending_lo = g_ovl_pending_hi = 0; }

void disp_notify_write(uint32_t phys, uint32_t size)
{
    if (!g_pending) return;
    if (phys >= g_ovl_pending_hi || phys + size <= g_ovl_pending_lo) return;
    const OverlayDesc *p = g_pending;
    disp_clear_pending();
    disp_load(p->name);
}

/* Geom stubs save real GP/SP on scratchpad and then reuse those regs as
 * temps; heal them after every dispatcher call (see Dispatcher.cs). */
static inline void heal_geom(CpuContext *c)
{
    if (__builtin_expect(c->GP == 0x00FFFFFFu || c->GP == 0x0000FFFFu, 0))
        c->GP = 0x800563FCu;
    uint32_t sp = c->SP;
    bool temp = sp < 0x8000u || (sp >= 0x1F800000u && sp < 0x1F800400u) || sp == 0x02000000u || sp == 0xFFFFFFDFu;
    if (__builtin_expect(temp, 0)) {
        uint32_t saved = RD32(0x1F800034u);
        if (saved >= 0x80000000u && saved < 0x80800000u) c->SP = saved;
    }
}

void disp_call(CpuContext *c, uint32_t addr)
{
    g_last_call_addr = addr;
    g_call_count++;
    libetc_maybe_catch_up_vblank();
    cd_advance_streaming();
    if (bios_try_dispatch(c, addr)) return;
    RecompFn fn = fmap_get(addr);
    if (!fn) {
        char msg[160];
        snprintf(msg, sizeof msg, "unmapped call: 0x%08X (RA=%08X SP=%08X)", addr, c->RA, c->SP);
        plat_fatal(msg);
        return;
    }
    fn(c);
    heal_geom(c);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                           */
/* ------------------------------------------------------------------ */
uint32_t g_intr_env_addr;
static bool g_irq_ever_delivered;
static long g_irq_delivered[8];

void irq_deliver(int irq, CpuContext *cpu)
{
    uint32_t env = g_intr_env_addr;
    if (env == 0) return;
    uint32_t handler = RD32(env + 2u + (uint32_t)irq * 4u);
    if (handler == 0) return;
    if (!g_irq_ever_delivered) {
        g_irq_ever_delivered = true;
        LOG("[IRQ] first delivery irq=%d env=0x%08X handler=0x%08X", irq, env, handler);
    }
    CpuSnapshot snap;
    cpu_snapshot(cpu, &snap);
    WR16(env, 1);
    disp_call(cpu, handler);
    WR16(env, 0);
    cpu_restore(cpu, &snap);
    if (irq >= 0 && irq < 8) g_irq_delivered[irq]++;
}

/* ------------------------------------------------------------------ */
/* Frame clock (60 Hz)                                                  */
/* ------------------------------------------------------------------ */
static uint64_t g_next_frame_ns;

void frameclock_throttle(void)
{
    while (g_paused) { plat_sleep_us(10000); g_next_frame_ns = plat_time_ns(); }
    if (g_skip_throttle) return;
    const uint64_t frame = 16666667ull;
    if (g_next_frame_ns == 0) g_next_frame_ns = plat_time_ns();
    g_next_frame_ns += frame;
    uint64_t now = plat_time_ns();
    if (g_next_frame_ns <= now) {
        if (now - g_next_frame_ns > frame) g_next_frame_ns = now;
        return;
    }
    uint64_t wait = g_next_frame_ns - now;
    if (wait > 2000000ull) plat_sleep_us((unsigned)((wait - 1500000ull) / 1000ull));
    while (plat_time_ns() < g_next_frame_ns) { /* spin the last ~1.5ms */ }
}

/* ------------------------------------------------------------------ */
/* Pad                                                                  */
/* ------------------------------------------------------------------ */
volatile uint32_t g_pad_pressed;
volatile uint8_t g_pad_lx = 0x80, g_pad_ly = 0x80, g_pad_rx = 0x80, g_pad_ry = 0x80;

uint16_t pad_state(void) { return (uint16_t)(~g_pad_pressed & 0xFFFFu); }

/* ------------------------------------------------------------------ */
/* LibEtc: VSync + vblank pump                                          */
/* ------------------------------------------------------------------ */
#define VBLANK_COUNT_ADDR  0x800549F0u
#define TICKS_ELAPSED_ADDR 0x80034520u
#define TICKS_PER_VBLANK   17u

static uint64_t g_last_vblank_ns;
static int g_catchup_guard;

static uint32_t tick_counters(void)
{
    g_last_vblank_ns = plat_time_ns();
    uint32_t count = RD32(VBLANK_COUNT_ADDR) + 1;
    WR32(VBLANK_COUNT_ADDR, count);
    WR32(TICKS_ELAPSED_ADDR, RD32(TICKS_ELAPSED_ADDR) + TICKS_PER_VBLANK);
    return count;
}

static bool cpu_looks_safe_for_irq(void)
{
    return (g_cpu.GP & 0xFF000000u) == 0x80000000u && (g_cpu.SP & 0xFF000000u) == 0x80000000u;
}

void libetc_maybe_catch_up_vblank(void)
{
    if (g_catchup_guard || !g_ram) return;
    if (!cpu_looks_safe_for_irq()) return;
    uint64_t now = plat_time_ns();
    if (g_last_vblank_ns == 0) { g_last_vblank_ns = now; return; }
    if (now - g_last_vblank_ns < 16666667ull) return;
    g_catchup_guard++;
    g_last_vblank_ns = now;
    irq_deliver(0, &g_cpu);
    tick_counters();
    g_catchup_guard--;
}

uint32_t libetc_advance_vblank(CpuContext *c)
{
    (void)c;
    g_catchup_guard++;
    runtime_present_frame();
    uint32_t n = tick_counters();
    g_catchup_guard--;
    return n;
}

void LibEtc_VSync(CpuContext *c)
{
    uint32_t target = c->A0;
    uint32_t count = RD32(VBLANK_COUNT_ADDR);
    long spins = 0;
    const long hard = 6000;
    while (count < target) {
        count = libetc_advance_vblank(c);
        if (++spins >= hard) { LOG("[VSYNC] aborting wait, target=%u count=%u", target, count); break; }
    }
    c->V0 = 0;
}

/* ------------------------------------------------------------------ */
/* Runtime.PresentFrame                                                 */
/* ------------------------------------------------------------------ */
void runtime_present_frame(void)
{
    g_present_calls++;
    int x, y, w, h; bool rgb24, en;
    gpu_display_info(&x, &y, &w, &h, &rgb24, &en);
    plat_present(g_vram, x, y, w, h, rgb24, en);
    frameclock_throttle();
    libcd_tick();
    cd_advance_streaming();
    bios_refresh_pad();
    libpad_refresh();
    irq_deliver(0, &g_cpu);
}
