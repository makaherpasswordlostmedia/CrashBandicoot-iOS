/*
 * psx.h - shared declarations for the native (C) RecompOne runtime.
 *
 * This is a straight C port of RecompOne.Runtime (C#) sized for a 32-bit
 * armv7 device (iPad mini 1: A5, 512 MB, iOS 9.3.5). The recompiled game
 * code is produced by the existing C# recompiler and then converted to C by
 * tools/cs2c.py, so everything here has to keep the same names / semantics
 * that the generated code expects (CpuContext register names, memory
 * accessors, Dispatcher.Call, Gte.*, SDK replacement functions...).
 */
#ifndef PSX_H
#define PSX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* CPU context                                                          */
/* ------------------------------------------------------------------ */
typedef struct CpuContext {
    union {
        uint32_t gpr[32];
        struct {
            uint32_t Zero, At, V0, V1, A0, A1, A2, A3,
                     T0, T1, T2, T3, T4, T5, T6, T7,
                     S0, S1, S2, S3, S4, S5, S6, S7,
                     T8, T9, K0, K1, GP, SP, FP, RA;
        };
    };
    uint32_t HI, LO;
    uint32_t SR, Cause, EPC, BadVAddr, PRId;
} CpuContext;

typedef struct { uint32_t gpr[32]; uint32_t hi, lo; } CpuSnapshot;
static inline void cpu_snapshot(const CpuContext *c, CpuSnapshot *s)
{ memcpy(s->gpr, c->gpr, sizeof s->gpr); s->hi = c->HI; s->lo = c->LO; }
static inline void cpu_restore(CpuContext *c, const CpuSnapshot *s)
{ memcpy(c->gpr, s->gpr, sizeof s->gpr); c->HI = s->hi; c->LO = s->lo; }

typedef void (*RecompFn)(CpuContext *c);

extern CpuContext g_cpu;

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) plat_log(__VA_ARGS__)
extern int g_log_cd, g_log_sdk, g_log_bios, g_log_overlay, g_log_gpu;
#define LOGF(flag, ...) do { if (flag) plat_log(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ */
/* Platform services implemented by the host (iOS / test harness)        */
/* ------------------------------------------------------------------ */
uint64_t plat_time_ns(void);             /* monotonic clock                     */
void     plat_sleep_us(unsigned us);
void     plat_present(const uint16_t *vram, int dx, int dy, int dw, int dh,
                      bool rgb24, bool display_enabled);
void     plat_fatal(const char *msg);    /* never returns                       */
const char *plat_disc_path(void);        /* path of the .cue                    */
const char *plat_save_dir(void);         /* writable dir for memory cards       */
void     plat_input_poll(void);          /* refresh g_pad_* from touch / GC     */
void     plat_rumble(uint8_t large, uint8_t small_);

/* ------------------------------------------------------------------ */
/* Memory map                                                           */
/* ------------------------------------------------------------------ */
#define PS_RAM_SIZE      0x00200000u
#define PS_RAM_WINDOW    0x00800000u
#define PS_SCRATCH_BASE  0x1F800000u
#define PS_SCRATCH_SIZE  0x00000400u
#define PS_HW_BASE       0x1F801000u
#define PS_HW_SIZE       0x00002000u
#define PS_BIOS_BASE     0x1FC00000u
#define PS_BIOS_SIZE     0x00080000u
#define PS_PHYS_MASK     0x1FFFFFFFu

extern uint8_t *g_ram;         /* 2 MiB                                    */
extern uint8_t *g_scratch;     /* 1 KiB                                    */
extern uint8_t *g_hwregs;      /* 8 KiB                                    */
extern uint8_t *g_bios;        /* 512 KiB (only the KROM font is populated) */
extern int32_t  g_yield;       /* vblank catch-up counter                  */

uint32_t mem_read8_slow(uint32_t a);
uint32_t mem_read16_slow(uint32_t a);
uint32_t mem_read32_slow(uint32_t a);
void     mem_write8_slow(uint32_t a, uint32_t v);
void     mem_write16_slow(uint32_t a, uint32_t v);
void     mem_write32_slow(uint32_t a, uint32_t v);
void     mem_yield_slow(void);

static inline uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void st16(uint8_t *p, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); }

#define MEM_TICK() do { if (__builtin_expect(--g_yield <= 0, 0)) mem_yield_slow(); } while (0)

static inline uint32_t RD32(uint32_t a)
{
    uint32_t p = a & PS_PHYS_MASK;
    MEM_TICK();
    if (__builtin_expect(p < PS_RAM_WINDOW, 1)) return ld32(g_ram + (p & (PS_RAM_SIZE - 1)));
    if (__builtin_expect((p & ~(PS_SCRATCH_SIZE - 1)) == PS_SCRATCH_BASE, 1)) return ld32(g_scratch + (p & (PS_SCRATCH_SIZE - 1)));
    return mem_read32_slow(a);
}
static inline uint32_t RD16(uint32_t a)
{
    uint32_t p = a & PS_PHYS_MASK;
    MEM_TICK();
    if (__builtin_expect(p < PS_RAM_WINDOW, 1)) return ld16(g_ram + (p & (PS_RAM_SIZE - 1)));
    if (__builtin_expect((p & ~(PS_SCRATCH_SIZE - 1)) == PS_SCRATCH_BASE, 1)) return ld16(g_scratch + (p & (PS_SCRATCH_SIZE - 1)));
    return mem_read16_slow(a);
}
static inline uint32_t RD8(uint32_t a)
{
    uint32_t p = a & PS_PHYS_MASK;
    MEM_TICK();
    if (__builtin_expect(p < PS_RAM_WINDOW, 1)) return g_ram[p & (PS_RAM_SIZE - 1)];
    if (__builtin_expect((p & ~(PS_SCRATCH_SIZE - 1)) == PS_SCRATCH_BASE, 1)) return g_scratch[p & (PS_SCRATCH_SIZE - 1)];
    return mem_read8_slow(a);
}

/* Overlay hook: only used if a game config ever registers overlays (Crash
 * has none). Kept so the write path mirrors PSMemory.TrackWrite. */
extern uint32_t g_ovl_pending_lo, g_ovl_pending_hi; /* [lo,hi) window, 0/0 = none */
void disp_notify_write(uint32_t phys, uint32_t size);

static inline void WR32(uint32_t a, uint32_t v)
{
    uint32_t p = a & PS_PHYS_MASK;
    MEM_TICK();
    if (__builtin_expect(p < PS_RAM_WINDOW, 1)) {
        st32(g_ram + (p & (PS_RAM_SIZE - 1)), v);
        if (__builtin_expect(g_ovl_pending_hi != 0, 0)) disp_notify_write(p & (PS_RAM_SIZE - 1), 4);
        return;
    }
    if (__builtin_expect((p & ~(PS_SCRATCH_SIZE - 1)) == PS_SCRATCH_BASE, 1)) { st32(g_scratch + (p & (PS_SCRATCH_SIZE - 1)), v); return; }
    mem_write32_slow(a, v);
}
static inline void WR16(uint32_t a, uint32_t v)
{
    uint32_t p = a & PS_PHYS_MASK;
    MEM_TICK();
    if (__builtin_expect(p < PS_RAM_WINDOW, 1)) {
        st16(g_ram + (p & (PS_RAM_SIZE - 1)), v);
        if (__builtin_expect(g_ovl_pending_hi != 0, 0)) disp_notify_write(p & (PS_RAM_SIZE - 1), 2);
        return;
    }
    if (__builtin_expect((p & ~(PS_SCRATCH_SIZE - 1)) == PS_SCRATCH_BASE, 1)) { st16(g_scratch + (p & (PS_SCRATCH_SIZE - 1)), v); return; }
    mem_write16_slow(a, v);
}
static inline void WR8(uint32_t a, uint32_t v)
{
    uint32_t p = a & PS_PHYS_MASK;
    MEM_TICK();
    if (__builtin_expect(p < PS_RAM_WINDOW, 1)) {
        g_ram[p & (PS_RAM_SIZE - 1)] = (uint8_t)v;
        if (__builtin_expect(g_ovl_pending_hi != 0, 0)) disp_notify_write(p & (PS_RAM_SIZE - 1), 1);
        return;
    }
    if (__builtin_expect((p & ~(PS_SCRATCH_SIZE - 1)) == PS_SCRATCH_BASE, 1)) { g_scratch[p & (PS_SCRATCH_SIZE - 1)] = (uint8_t)v; return; }
    mem_write8_slow(a, v);
}

/* LWL/LWR/SWL/SWR */
static inline uint32_t RDWL(uint32_t cur, uint32_t a)
{
    int shift = (int)((a & 3u) * 8u);
    uint32_t word = RD32(a & ~3u);
    return (cur & (0x00FFFFFFu >> shift)) | (word << (24 - shift));
}
static inline uint32_t RDWR(uint32_t cur, uint32_t a)
{
    int shift = (int)((a & 3u) * 8u);
    uint32_t word = RD32(a & ~3u);
    return (cur & (0xFFFFFF00u << (24 - shift))) | (word >> shift);
}
static inline void WRWL(uint32_t a, uint32_t v)
{
    uint32_t al = a & ~3u; int shift = (int)((a & 3u) * 8u);
    uint32_t mem = RD32(al);
    WR32(al, (mem & (0xFFFFFF00u << shift)) | (v >> (24 - shift)));
}
static inline void WRWR(uint32_t a, uint32_t v)
{
    uint32_t al = a & ~3u; int shift = (int)((a & 3u) * 8u);
    uint32_t mem = RD32(al);
    WR32(al, (mem & (0x00FFFFFFu >> (24 - shift))) | (v << shift));
}

/* Bulk helpers used by HLE code */
void mem_load_bytes(uint32_t addr, const uint8_t *data, uint32_t n);
void mem_zero_range(uint32_t addr, uint32_t n);
bool mem_init(void);
void mem_shutdown(void);

/* ------------------------------------------------------------------ */
/* Dispatcher                                                           */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t addr; RecompFn fn; } FuncEntry;
typedef struct {
    const char *name;
    int         lba_start;   /* -1 if none */
    uint32_t    base, size;
    const FuncEntry *funcs;
    uint32_t    nfuncs;
} OverlayDesc;

void disp_register(const OverlayDesc *ov);
void disp_load(const char *name);
void disp_try_load(const char *name);
void disp_load_by_lba(int lba);
void disp_clear_pending(void);
void disp_call(CpuContext *c, uint32_t addr);
extern volatile uint32_t g_last_call_addr;
extern uint64_t g_call_count;

/* ------------------------------------------------------------------ */
/* Interrupts / BIOS                                                    */
/* ------------------------------------------------------------------ */
extern uint32_t g_intr_env_addr;   /* BiosB.IntrEnvInInterruptAddr */
void irq_deliver(int irq, CpuContext *c);
bool bios_try_dispatch(CpuContext *c, uint32_t addr);
void bios_a(CpuContext *c, uint32_t fn);
void bios_b(CpuContext *c, uint32_t fn);
void bios_c(CpuContext *c, uint32_t fn);
void bios_init(void);
void bios_refresh_pad(void);
void bios_syscall(CpuContext *c);
void bios_break(CpuContext *c);
void bios_deliver_event(uint32_t cls, uint32_t spec);
uint32_t bios_get_free_ev_slot(void);
void bios_sys_enq_int_rp(CpuContext *c);
void bios_sys_deq_int_rp(CpuContext *c);

/* ------------------------------------------------------------------ */
/* Disc                                                                 */
/* ------------------------------------------------------------------ */
typedef struct Disc Disc;
Disc   *disc_open(const char *cue_path);
void    disc_close(Disc *d);
/* Reads `size` bytes of a sector's payload the same way CueBin.ReadSectorData does. */
void    disc_read_sector_data(Disc *d, int lba, int size, uint8_t *out);
static inline void disc_read_sector(Disc *d, int lba, uint8_t *out2048) { disc_read_sector_data(d, lba, 2048, out2048); }
bool    disc_locate(Disc *d, const char *name, int *lba, uint32_t *size);
uint8_t*disc_read_file(Disc *d, const char *path, uint32_t *size); /* malloc'ed */
bool    disc_find_file(Disc *d, const char *name, char *out, size_t outsz);
extern Disc *g_disc;

/* ------------------------------------------------------------------ */
/* CD controller (register level)                                       */
/* ------------------------------------------------------------------ */
void    cd_init(Disc *d);
uint8_t cd_read(uint32_t phys);
void    cd_write(uint32_t phys, uint8_t v);
void    cd_advance_streaming(void);
void    cd_dma_read(uint32_t addr, uint32_t byte_count);
uint8_t cd_drive_status_byte(void);
void    cd_queue_async_seekl(uint8_t mm, uint8_t ss, uint8_t ff);
void    cd_queue_async_get_status(void);
void    cd_queue_async_set_mode(uint8_t mode);
void    cd_queue_async_read_sector(uint32_t count, uint32_t dst, uint32_t mode);
void    cd_load_to_memory(const char *path, uint32_t address, int offset, int length);
void    cd_read_sector_2048(int lba, uint8_t *out);          /* BIOS A(A5) path */
void    cd_read_sector_state(int lba, uint8_t *out2048);     /* seeks + sets last-read */
void    cd_read_sector_size(int lba, int size, uint8_t *out);/* stats + raw read */

/* ------------------------------------------------------------------ */
/* SDK HLE (functions that replace PsyQ library code)                   */
/* ------------------------------------------------------------------ */
#define DECL(x) void x(CpuContext *c)
DECL(LibCd_CdInit); DECL(LibCd_CdReset); DECL(LibCd_CdControl); DECL(LibCd_CdControlF);
DECL(LibCd_CdControlB); DECL(LibCd_CdSync); DECL(LibCd_CdReady); DECL(LibCd_CdRead);
DECL(LibCd_CdReadSync); DECL(LibCd_CdGetSector); DECL(LibCd_CdDataSync);
DECL(LibCd_CdSearchFile); DECL(LibCd_CdSyncCallback); DECL(LibCd_CdReadyCallback);
DECL(LibCd_CdReadCallback); DECL(LibCd_CdDataCallback); DECL(LibCd_CdStatus);
DECL(LibCd_CdMode); DECL(LibCd_CdLastCom); DECL(LibCd_CdMix);
DECL(LibEtc_VSync);
DECL(LibGpu_DrawOTag); DECL(LibGpu_DrawSync); DECL(LibGpu_PutDrawEnv); DECL(LibGpu_PutDispEnv);
DECL(LibCdStream_StSetRing); DECL(LibCdStream_StClearRing); DECL(LibCdStream_StUnSetRing);
DECL(LibCdStream_StSetStream); DECL(LibCdStream_StSetMask); DECL(LibCdStream_StGetNext);
DECL(LibCdStream_StFreeRing); DECL(LibCdStream_StGetBackloc);
DECL(LibPad_PadInitDirect); DECL(LibPad_PadStartCom); DECL(LibPad_PadStopCom);
DECL(LibPad_PadEnableCom); DECL(LibPad_PadChkVsync); DECL(LibPad_PadChkMtap);
DECL(LibPad_PadGetState); DECL(LibPad_PadInfoMode); DECL(LibPad_PadInfoAct);
DECL(LibPad_PadInfoComb); DECL(LibPad_PadSetMainMode); DECL(LibPad_PadSetActAlign);
DECL(LibPad_PadSetAct);
DECL(LibMem_RMemcpy);
#undef DECL

void libcd_tick(void);
int  libcd_current_lba(void);
double libcd_sectors_per_second(void);
void libcdstream_on_read_stream(int lba);
void libcdstream_on_stop_stream(void);
void libpad_refresh(void);
void libetc_maybe_catch_up_vblank(void);
uint32_t libetc_advance_vblank(CpuContext *c);

/* ------------------------------------------------------------------ */
/* Runtime frame glue                                                   */
/* ------------------------------------------------------------------ */
void runtime_present_frame(void);
void runtime_init(void);
void runtime_run_game(void);      /* Entry.Run equivalent; blocks forever */
extern int64_t g_present_calls;

/* ------------------------------------------------------------------ */
/* Frame clock                                                          */
/* ------------------------------------------------------------------ */
void frameclock_throttle(void);
extern volatile int g_skip_throttle;
extern volatile int g_paused;

/* ------------------------------------------------------------------ */
/* Pad state (active-low, matches Hardware/Controller.cs)               */
/* ------------------------------------------------------------------ */
#define PAD_SELECT   (1u << 0)
#define PAD_L3       (1u << 1)
#define PAD_R3       (1u << 2)
#define PAD_START    (1u << 3)
#define PAD_UP       (1u << 4)
#define PAD_RIGHT    (1u << 5)
#define PAD_DOWN     (1u << 6)
#define PAD_LEFT     (1u << 7)
#define PAD_L2       (1u << 8)
#define PAD_R2       (1u << 9)
#define PAD_L1       (1u << 10)
#define PAD_R1       (1u << 11)
#define PAD_TRIANGLE (1u << 12)
#define PAD_CIRCLE   (1u << 13)
#define PAD_CROSS    (1u << 14)
#define PAD_SQUARE   (1u << 15)
extern volatile uint32_t g_pad_pressed;   /* active-high mask set by host */
extern volatile uint8_t  g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;
uint16_t pad_state(void);                 /* active-low, as the game expects */

/* ------------------------------------------------------------------ */
/* GPU                                                                  */
/* ------------------------------------------------------------------ */
#define VRAM_W 1024
#define VRAM_H 512
extern uint16_t *g_vram;
void     gpu_init(void);
void     gpu_write_gp0(uint32_t w);
void     gpu_write_gp1(uint32_t w);
uint32_t gpu_read_data(void);
uint32_t gpu_read_stat(void);
bool     gpu_pal(void);
void     gpu_display_info(int *x, int *y, int *w, int *h, bool *rgb24, bool *enabled);
extern volatile int g_gpu_dirty;

/* ------------------------------------------------------------------ */
/* GTE                                                                  */
/* ------------------------------------------------------------------ */
void     Gte_Execute(uint32_t cmd);
uint32_t Gte_Read(int reg);
void     Gte_Write(int reg, uint32_t v);
uint32_t Gte_ReadControl(int reg);
void     Gte_WriteControl(int reg, uint32_t v);
static inline void Gte_LoadWord(int reg, uint32_t v) { Gte_Write(reg, v); }
static inline uint32_t Gte_StoreWord(int reg) { return Gte_Read(reg); }
static inline bool Gte_GetCondition(void) { return false; }

/* ------------------------------------------------------------------ */
/* SPU / XA / MDEC / timers / DMA                                       */
/* ------------------------------------------------------------------ */
#define SPU_RAM_SIZE (512 * 1024)
void     spu_init(void);
uint16_t spu_read_reg16(uint32_t phys);
void     spu_write_reg16(uint32_t phys, uint16_t v);
uint32_t spu_transfer_addr_bytes(void);
void     spu_dma_write(uint32_t spu_addr, const uint8_t *data, uint32_t n);
void     spu_mix(int16_t *dst, int frames);
void     spu_set_master_volume(float v);

void     xa_reset(void);
void     xa_decode_sector(const uint8_t *sec, int off, uint8_t coding);
int      xa_buffered_samples(void);
bool     xa_next(int16_t *l, int16_t *r);

void     mdec_init(void);
uint32_t mdec_read_data(void);
uint32_t mdec_read_status(void);
void     mdec_write0(uint32_t w);
void     mdec_write_control(uint32_t w);

bool     timers_in_range(uint32_t phys);
bool     timers_read(uint32_t phys, uint32_t *v);
bool     timers_write(uint32_t phys, uint32_t v);

uint32_t dma_read_dicr(void);
void     dma_write_dicr(uint32_t v);
void     dma_run(int channel, uint32_t madr, uint32_t bcr, uint32_t chcr);

/* Memory cards */
typedef struct MemoryCard MemoryCard;
extern MemoryCard *g_card_a, *g_card_b;
struct MemoryCard {
    uint8_t  d[0x20000];
    char     path[512];
    bool     enabled;
};
void     card_init(MemoryCard *m, const char *path);
void     card_flush(MemoryCard *m);
void     card_format(MemoryCard *m);
int      card_find(MemoryCard *m, const char *name);
int      card_chain(MemoryCard *m, int first, int *out, int max);
int      card_create(MemoryCard *m, const char *name, int blocks);
int      card_file_size(MemoryCard *m, int block);
void     card_frame_read(MemoryCard *m, int frame, uint8_t *dst);
void     card_frame_write(MemoryCard *m, int frame, const uint8_t *src);
uint8_t  card_read_byte(MemoryCard *m, const int *chain, int n, int pos);
void     card_write_byte(MemoryCard *m, const int *chain, int n, int pos, uint8_t v);
void     card_delete(MemoryCard *m, const char *name);
typedef struct { char name[24]; int size; int block; } CardDirEnt;
int      card_match(MemoryCard *m, const char *pat, CardDirEnt *out, int max);

/* KROM font */
void     krom_install(uint8_t *bios);
uint32_t krom2_raw_add(uint32_t code);
uint32_t krom2_offset(uint32_t code);

/* Generated code entry points (gen/entry.c) */
void game_entry_run(void);

/* misc */
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

#ifdef __cplusplus
}
#endif
#endif /* PSX_H */
