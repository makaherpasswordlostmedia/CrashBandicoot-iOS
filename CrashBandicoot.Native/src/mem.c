/* mem.c - PSMemory + Dma + Timers ported from RecompOne.Runtime */
#include "psx.h"

uint8_t *g_ram, *g_scratch, *g_hwregs, *g_bios;
int32_t  g_yield = 65536;
uint32_t g_ovl_pending_lo, g_ovl_pending_hi;
CpuContext g_cpu;

/* Crash SCUS-94900 .sbss global pointer. Geom jump-table stubs temporarily
 * load $gp with AND masks; if a stub is left half-executed a later
 * gp-relative access lands on 0x01000003. PSMemory heals that case. */
#define CRASH_GP 0x800563FCu

bool mem_init(void)
{
    g_ram     = (uint8_t *)calloc(1, PS_RAM_SIZE);
    g_scratch = (uint8_t *)calloc(1, PS_SCRATCH_SIZE);
    g_hwregs  = (uint8_t *)calloc(1, PS_HW_SIZE);
    g_bios    = (uint8_t *)calloc(1, PS_BIOS_SIZE);
    if (!g_ram || !g_scratch || !g_hwregs || !g_bios) return false;
    krom_install(g_bios);
    return true;
}

void mem_shutdown(void)
{
    free(g_ram); free(g_scratch); free(g_hwregs); free(g_bios);
    g_ram = g_scratch = g_hwregs = g_bios = NULL;
}

/* ------------------------------------------------------------------ */
static int g_unmapped_logged;

static void log_unmapped(const char *what, uint32_t addr, int size)
{
    if (g_unmapped_logged++ >= 64) return;
    CpuContext *c = &g_cpu;
    LOG("[MEM] unmapped %s 0x%08X size=%d RA=%08X SP=%08X GP=%08X A0=%08X A1=%08X S0=%08X",
        what, addr, size, c->RA, c->SP, c->GP, c->A0, c->A1, c->S0);
}

/* Returns pointer into backing store, or NULL if unmapped. */
static uint8_t *map(uint32_t addr, int size, bool allow_heal)
{
    uint32_t phys = addr & PS_PHYS_MASK;
    (void)size;
    if (phys < PS_RAM_WINDOW) return g_ram + (phys & (PS_RAM_SIZE - 1));
    if (phys >= PS_SCRATCH_BASE && phys < PS_SCRATCH_BASE + PS_SCRATCH_SIZE) return g_scratch + (phys - PS_SCRATCH_BASE);
    if (phys >= PS_HW_BASE && phys < PS_HW_BASE + PS_HW_SIZE) return g_hwregs + (phys - PS_HW_BASE);
    if (phys >= PS_BIOS_BASE && phys < PS_BIOS_BASE + PS_BIOS_SIZE) return g_bios + (phys - PS_BIOS_BASE);
    if (allow_heal) {
        CpuContext *c = &g_cpu;
        if (c->GP == 0x00FFFFFFu || c->GP == 0x0000FFFFu) {
            int32_t delta = (int32_t)(addr - c->GP);
            if (delta >= -0x8000 && delta < 0x8000) {
                c->GP = CRASH_GP;
                return map(CRASH_GP + (uint32_t)delta, size, false);
            }
        }
    }
    return NULL;
}

void mem_yield_slow(void)
{
    g_yield = 65536;
    libetc_maybe_catch_up_vblank();
}

static inline bool is_cd(uint32_t p)  { return p >= 0x1F801800u && p <= 0x1F801803u; }
static inline bool is_spu(uint32_t p) { return p >= 0x1F801C00u && p < 0x1F801E80u; }
static inline bool is_dma_chcr(uint32_t p) { return p >= 0x1F801080u && p < 0x1F8010F0u && (p & 0xFu) == 8u; }

static inline uint32_t hw32(uint32_t phys) { return ld32(g_hwregs + (phys - PS_HW_BASE)); }
static inline void hw32w(uint32_t phys, uint32_t v) { st32(g_hwregs + (phys - PS_HW_BASE), v); }

uint32_t mem_read8_slow(uint32_t a)
{
    uint32_t p = a & PS_PHYS_MASK;
    if (is_cd(p)) return cd_read(p);
    uint8_t *m = map(a, 1, true);
    if (!m) { log_unmapped("read8", a, 1); return 0; }
    return m[0];
}

uint32_t mem_read16_slow(uint32_t a)
{
    uint32_t p = a & PS_PHYS_MASK;
    if (is_cd(p)) return cd_read(p);
    if (is_spu(p)) return spu_read_reg16(p);
    uint32_t tv;
    if (timers_in_range(p) && timers_read(p, &tv)) return (uint16_t)tv;
    uint8_t *m = map(a, 2, true);
    if (!m) { log_unmapped("read16", a, 2); return 0; }
    return ld16(m);
}

uint32_t mem_read32_slow(uint32_t a)
{
    uint32_t p = a & PS_PHYS_MASK;
    if (p == 0x1F801810u) return gpu_read_data();
    if (p == 0x1F801814u) return gpu_read_stat();
    if (p == 0x1F801820u) return mdec_read_data();
    if (p == 0x1F801824u) return mdec_read_status();
    if (p == 0x1F8010F4u) return dma_read_dicr();
    if (is_cd(p)) return cd_read(p);
    if (is_spu(p)) return (uint32_t)spu_read_reg16(p) | ((uint32_t)spu_read_reg16(p + 2) << 16);
    uint32_t tv;
    if (timers_in_range(p) && timers_read(p, &tv)) return tv;
    uint8_t *m = map(a, 4, true);
    if (!m) { log_unmapped("read32", a, 4); return 0; }
    return ld32(m);
}

void mem_write8_slow(uint32_t a, uint32_t v)
{
    uint32_t p = a & PS_PHYS_MASK;
    if (is_cd(p)) { cd_write(p, (uint8_t)v); return; }
    uint8_t *m = map(a, 1, true);
    if (!m) { log_unmapped("write8", a, 1); return; }
    if (p >= PS_BIOS_BASE && p < PS_BIOS_BASE + PS_BIOS_SIZE) return; /* ROM */
    m[0] = (uint8_t)v;
}

void mem_write16_slow(uint32_t a, uint32_t v)
{
    uint32_t p = a & PS_PHYS_MASK;
    if (is_cd(p)) { cd_write(p, (uint8_t)v); return; }
    if (is_spu(p)) { spu_write_reg16(p, (uint16_t)v); return; }
    if (timers_in_range(p) && timers_write(p, v)) return;
    uint8_t *m = map(a, 2, true);
    if (!m) { log_unmapped("write16", a, 2); return; }
    if (p >= PS_BIOS_BASE && p < PS_BIOS_BASE + PS_BIOS_SIZE) return;
    st16(m, v);
}

void mem_write32_slow(uint32_t a, uint32_t v)
{
    uint32_t p = a & PS_PHYS_MASK;
    if (p == 0x1F801810u) { gpu_write_gp0(v); return; }
    if (p == 0x1F801814u) { gpu_write_gp1(v); return; }
    if (p == 0x1F801820u) { mdec_write0(v); return; }
    if (p == 0x1F801824u) { mdec_write_control(v); return; }
    if (p == 0x1F8010F4u) { dma_write_dicr(v); return; }
    if (is_dma_chcr(p) && (v & 0x01000000u) != 0) {
        hw32w(p, v & ~0x01000000u);
        dma_run((int)((p - 0x1F801080u) / 0x10u), hw32(p - 8u), hw32(p - 4u), v);
        return;
    }
    if (is_cd(p)) { cd_write(p, (uint8_t)v); return; }
    if (is_spu(p)) { spu_write_reg16(p, (uint16_t)v); spu_write_reg16(p + 2, (uint16_t)(v >> 16)); return; }
    if (timers_in_range(p) && timers_write(p, v)) return;
    uint8_t *m = map(a, 4, true);
    if (!m) { log_unmapped("write32", a, 4); return; }
    if (p >= PS_BIOS_BASE && p < PS_BIOS_BASE + PS_BIOS_SIZE) return;
    st32(m, v);
}

void mem_load_bytes(uint32_t addr, const uint8_t *data, uint32_t n)
{
    if (!n) return;
    uint32_t phys = addr & PS_PHYS_MASK;
    if (phys < PS_RAM_SIZE && (uint64_t)phys + n <= PS_RAM_SIZE) {
        memcpy(g_ram + phys, data, n);
        if (g_ovl_pending_hi) disp_notify_write(phys, n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) WR8(addr + i, data[i]);
}

void mem_zero_range(uint32_t addr, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) WR8(addr + i, 0);
}

/* ------------------------------------------------------------------ */
/* DMA                                                                  */
/* ------------------------------------------------------------------ */
static uint32_t g_dicr;

uint32_t dma_read_dicr(void) { return g_dicr; }

void dma_write_dicr(uint32_t val)
{
    uint32_t flags = (g_dicr >> 24) & 0x7Fu;
    flags &= ~((val >> 24) & 0x7Fu);
    g_dicr = (val & 0x00FFFFFFu) | (flags << 24);
    if ((g_dicr & 0x8000u) != 0 || (((g_dicr >> 23) & 1u) != 0 && flags != 0))
        g_dicr |= 0x80000000u;
}

static uint32_t word_count(uint32_t bcr)
{
    uint32_t size = bcr & 0xFFFFu, blocks = (bcr >> 16) & 0xFFFFu;
    uint32_t total = blocks == 0 ? size : size * blocks;
    return total == 0 ? 0x10000u : total;
}

static void dma_complete(int ch)
{
    bool master = (g_dicr & (1u << 23)) != 0;
    bool enabled = (g_dicr & (1u << (16 + ch))) != 0;
    if (!master || !enabled) return;
    g_dicr |= 1u << (24 + ch);
    irq_deliver(3, &g_cpu);
}

static void dma_gpu(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    uint32_t sync = (chcr >> 9) & 3u;
    if (sync == 2) {
        uint32_t addr = madr & 0x1FFFFCu;
        for (int guard = 0; guard < 0x100000; guard++) {
            uint32_t header = RD32(addr);
            uint32_t count = header >> 24;
            for (uint32_t i = 0; i < count; i++) gpu_write_gp0(RD32(addr + 4u + i * 4u));
            uint32_t next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu || (next & 0x800000u) != 0) break;
            addr = next & 0x1FFFFCu;
        }
    } else if (chcr & 1u) {
        uint32_t words = word_count(bcr);
        for (uint32_t i = 0; i < words; i++) gpu_write_gp0(RD32(madr + i * 4u));
    } else {
        uint32_t words = word_count(bcr);
        for (uint32_t i = 0; i < words; i++) WR32(madr + i * 4u, gpu_read_data());
    }
}

void dma_run(int channel, uint32_t madr, uint32_t bcr, uint32_t chcr)
{
    switch (channel) {
    case 0: { uint32_t w = word_count(bcr); for (uint32_t i = 0; i < w; i++) mdec_write0(RD32(madr + i * 4u)); break; }
    case 1: { uint32_t w = word_count(bcr); for (uint32_t i = 0; i < w; i++) WR32(madr + i * 4u, mdec_read_data()); break; }
    case 2: dma_gpu(madr, bcr, chcr); break;
    case 3: cd_dma_read(madr, word_count(bcr) * 4u); break;
    case 4: {
        if ((chcr & 1u) == 0) break;
        uint32_t bytes = word_count(bcr) * 4u;
        uint8_t *buf = (uint8_t *)malloc(bytes ? bytes : 1);
        if (!buf) break;
        for (uint32_t i = 0; i < bytes; i++) buf[i] = (uint8_t)RD8(madr + i);
        spu_dma_write(spu_transfer_addr_bytes(), buf, bytes);
        free(buf);
        break;
    }
    case 6: {
        uint32_t count = bcr & 0xFFFFu;
        if (count == 0) break;
        uint32_t addr = madr;
        for (uint32_t i = 0; i < count - 1; i++) { WR32(addr, (addr - 4u) & 0x00FFFFFFu); addr -= 4u; }
        WR32(addr, 0x00FFFFFFu);
        break;
    }
    default: return;
    }
    dma_complete(channel);
}

/* ------------------------------------------------------------------ */
/* Root counters (approximation, as in Timers.cs)                       */
/* ------------------------------------------------------------------ */
#define T_BASE 0x1F801100u
#define T_END  0x1F801130u
static uint64_t g_t_reset_ns[3];
static uint16_t g_t_mode[3], g_t_target[3];
static const double SYSCLK = 33868800.0, HBLANK = 15780.0, DOTCLK = 5322240.0;

bool timers_in_range(uint32_t phys) { return phys >= T_BASE && phys < T_END; }

static double t_rate(int t)
{
    switch (t) {
    case 0: return (g_t_mode[0] & 0x100u) ? DOTCLK : SYSCLK;
    case 1: return (g_t_mode[1] & 0x100u) ? HBLANK : SYSCLK;
    default: return (g_t_mode[2] & 0x200u) ? SYSCLK / 8.0 : SYSCLK;
    }
}

bool timers_read(uint32_t phys, uint32_t *value)
{
    *value = 0;
    if (!timers_in_range(phys)) return false;
    int t = (int)((phys - T_BASE) / 0x10u);
    switch ((phys - T_BASE) & 0xFu) {
    case 0x0: {
        double el = (double)(plat_time_ns() - g_t_reset_ns[t]) * 1e-9;
        int64_t ticks = (int64_t)(el * t_rate(t));
        *value = (uint16_t)(ticks & 0xFFFF);
        break; }
    case 0x4: *value = g_t_mode[t]; break;
    case 0x8: *value = g_t_target[t]; break;
    }
    return true;
}

bool timers_write(uint32_t phys, uint32_t value)
{
    if (!timers_in_range(phys)) return false;
    int t = (int)((phys - T_BASE) / 0x10u);
    switch ((phys - T_BASE) & 0xFu) {
    case 0x0: g_t_reset_ns[t] = plat_time_ns(); break;
    case 0x4: g_t_mode[t] = (uint16_t)value; g_t_reset_ns[t] = plat_time_ns(); break;
    case 0x8: g_t_target[t] = (uint16_t)value; break;
    }
    return true;
}
