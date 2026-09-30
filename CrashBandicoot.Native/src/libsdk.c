/* libsdk.c - RecompOne.Runtime.Sdk.* ported to C.
 * Streaming / XA pumping use two small pthreads exactly like the C# host did;
 * they only touch the disc (pread, thread safe) and emulated RAM bytes. */
#include "psx.h"
#include <pthread.h>

/* ================================================================== */
/* LibCd                                                                */
/* ================================================================== */
enum { CD_NOP = 0x01, CD_SETLOC = 0x02, CD_PLAY = 0x03, CD_READN = 0x06, CD_STOP = 0x08, CD_PAUSE = 0x09,
       CD_INIT = 0x0A, CD_MUTE = 0x0B, CD_DEMUTE = 0x0C, CD_SETFILTER = 0x0D, CD_SETMODE = 0x0E,
       CD_GETLOCL = 0x10, CD_GETLOCP = 0x11, CD_SEEKL = 0x15, CD_SEEKP = 0x16, CD_READS = 0x1B };
#define CD_COMPLETE  0x02
#define CD_DATAREADY 0x01

static uint8_t g_status = 0x02, g_mode, g_com;
static uint8_t g_pos[4];
static uint8_t g_last_result[8];
static int     g_last_intr = CD_COMPLETE;
static uint32_t g_cb_sync, g_cb_ready, g_cb_data;
static volatile bool g_read_active;
static bool g_xa_active;
static volatile uint8_t g_filter_file, g_filter_channel;
static pthread_mutex_t g_pos_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_xa_thread; static volatile bool g_xa_run, g_xa_started;

static int bcd(uint8_t b) { return (b >> 4) * 10 + (b & 0xF); }
static uint8_t to_bcd(int n) { return (uint8_t)(((n / 10) << 4) + (n % 10)); }
static int pos_to_int(const uint8_t *p) { return (bcd(p[0]) * 60 + bcd(p[1])) * 75 + bcd(p[2]) - 150; }
static void int_to_pos(int i, uint8_t *mm, uint8_t *ss, uint8_t *ff)
{ i += 150; *ff = to_bcd(i % 75); *ss = to_bcd(i / 75 % 60); *mm = to_bcd(i / 75 / 60); }

int libcd_current_lba(void)
{
    pthread_mutex_lock(&g_pos_mx);
    int v = pos_to_int(g_pos);
    pthread_mutex_unlock(&g_pos_mx);
    return v;
}
double libcd_sectors_per_second(void) { return (g_mode & 0x80) ? 150.0 : 75.0; }

static void advance_pos(int n)
{
    pthread_mutex_lock(&g_pos_mx);
    int_to_pos(pos_to_int(g_pos) + n, &g_pos[0], &g_pos[1], &g_pos[2]);
    pthread_mutex_unlock(&g_pos_mx);
}

static void write_result(uint32_t addr)
{
    for (int i = 0; i < 8; i++) WR8(addr + (uint32_t)i, g_last_result[i]);
}

static int sector_size(uint8_t mode)
{
    if (mode & 0x20) return 2340;
    if (mode & 0x10) return 2328;
    return 2048;
}

static bool needs_loc(uint8_t com)
{ return com == CD_PLAY || com == CD_READN || com == CD_SEEKL || com == CD_SEEKP || com == CD_READS; }

static void *xa_loop(void *arg)
{
    (void)arg;
    uint8_t sec[2352];
    while (g_xa_run) {
        bool pumped = false;
        if (g_read_active && (g_mode & 0x40) && g_disc) {
            const int min_buf = 8192, max_scan = 64;
            bool use_filter = (g_mode & 0x08) != 0;
            int scanned = 0;
            while (g_read_active && xa_buffered_samples() < min_buf && scanned < max_scan) {
                int lba = libcd_current_lba();
                if (lba < 0) break;
                disc_read_sector_data(g_disc, lba, 2336, sec);
                advance_pos(1);
                scanned++;
                if ((sec[2] & 0x04) == 0) continue;
                if (use_filter && (sec[0] != g_filter_file || sec[1] != g_filter_channel)) continue;
                xa_decode_sector(sec, 8, sec[3]);
                pumped = true;
            }
        }
        plat_sleep_us(pumped ? 1000 : 4000);
    }
    return NULL;
}

static void ensure_xa_thread(void)
{
    if (g_xa_started) return;
    g_xa_started = true; g_xa_run = true;
    pthread_create(&g_xa_thread, NULL, xa_loop, NULL);
}

static int exec_command(uint8_t com, uint32_t param, uint32_t result)
{
    g_com = com;
    g_last_intr = CD_COMPLETE;
    switch (com) {
    case CD_SETLOC:
        if (param) { pthread_mutex_lock(&g_pos_mx); for (int i = 0; i < 4; i++) g_pos[i] = (uint8_t)RD8(param + (uint32_t)i); pthread_mutex_unlock(&g_pos_mx); }
        break;
    case CD_SETMODE: if (param) g_mode = (uint8_t)RD8(param); break;
    case CD_SETFILTER: if (param) { g_filter_file = (uint8_t)RD8(param); g_filter_channel = (uint8_t)RD8(param + 1); } break;
    case CD_READN:
        g_read_active = true;
        disp_load_by_lba(libcd_current_lba());
        ensure_xa_thread();
        break;
    case CD_READS:
        g_xa_active = true; g_read_active = false;
        libcdstream_on_read_stream(libcd_current_lba());
        break;
    case CD_GETLOCL: case CD_GETLOCP:
        pthread_mutex_lock(&g_pos_mx);
        g_last_result[0] = g_pos[0]; g_last_result[1] = g_pos[1]; g_last_result[2] = g_pos[2];
        pthread_mutex_unlock(&g_pos_mx);
        g_last_result[3] = g_mode; g_last_result[4] = g_filter_file; g_last_result[5] = g_filter_channel;
        g_last_result[6] = g_last_result[7] = 0;
        if (result) write_result(result);
        return 0;
    case CD_PAUSE: case CD_STOP: case CD_INIT:
        libcdstream_on_stop_stream();
        g_read_active = false; g_xa_active = false;
        disp_clear_pending();
        break;
    default: break;
    }
    g_last_result[0] = g_status;
    for (int i = 1; i < 8; i++) g_last_result[i] = 0;
    if (result) write_result(result);
    return 0;
}

static int command_wait(uint8_t com, uint32_t param, uint32_t result)
{
    if (param != 0 && needs_loc(com)) exec_command(CD_SETLOC, param, 0);
    return exec_command(com, param, result);
}

static int sync_result(uint32_t result) { if (result) write_result(result); return g_last_intr; }

static void cd_reset_state(void)
{
    libcdstream_on_stop_stream();
    g_status = 0x02; g_mode = 0; g_com = 0; g_last_intr = CD_COMPLETE;
    g_cb_sync = g_cb_ready = g_cb_data = 0;
    g_read_active = false; g_xa_active = false;
    g_filter_file = g_filter_channel = 0;
    memset(g_pos, 0, sizeof g_pos); memset(g_last_result, 0, sizeof g_last_result);
    disp_clear_pending();
}

void LibCd_CdInit(CpuContext *c) { cd_reset_state(); g_last_intr = CD_COMPLETE; g_last_result[0] = g_status; c->V0 = 0; }
void LibCd_CdReset(CpuContext *c) { cd_reset_state(); g_last_intr = CD_COMPLETE; g_last_result[0] = g_status; c->V0 = 1; }
void LibCd_CdControl(CpuContext *c) { c->V0 = command_wait((uint8_t)c->A0, c->A1, c->A2) == 0 ? 1u : 0u; }
void LibCd_CdControlF(CpuContext *c) { c->V0 = command_wait((uint8_t)c->A0, c->A1, 0) == 0 ? 1u : 0u; }
void LibCd_CdControlB(CpuContext *c)
{
    if (command_wait((uint8_t)c->A0, c->A1, c->A2) != 0) { c->V0 = 0; return; }
    c->V0 = sync_result(c->A2) == CD_COMPLETE ? 1u : 0u;
}
void LibCd_CdSync(CpuContext *c) { c->V0 = (uint32_t)sync_result(c->A1); }
void LibCd_CdReady(CpuContext *c) { if (c->A1) write_result(c->A1); c->V0 = (uint32_t)g_last_intr; }

void LibCd_CdRead(CpuContext *c)
{
    int sectors = (int)c->A0;
    uint32_t buf = c->A1;
    g_mode = (uint8_t)c->A2;
    int lba = libcd_current_lba();
    int size = sector_size(g_mode);
    disp_load_by_lba(lba);
    LOGF(g_log_sdk, "[SDK] CdRead sectors=%d buf=0x%08X mode=0x%02X lba=%d size=%d", sectors, buf, g_mode, lba, size);
    uint8_t *tmp = (uint8_t *)malloc(2352);
    for (int i = 0; i < sectors; i++) {
        cd_read_sector_size(lba + i, size, tmp);
        mem_load_bytes(buf + (uint32_t)(i * size), tmp, (uint32_t)size);
    }
    free(tmp);
    g_last_intr = CD_COMPLETE;
    c->V0 = 1;
}

void LibCd_CdReadSync(CpuContext *c) { if (c->A1) write_result(c->A1); c->V0 = 0; }

void LibCd_CdGetSector(CpuContext *c)
{
    uint32_t madr = c->A0; int words = (int)c->A1;
    uint8_t sec[2048];
    cd_read_sector_state(libcd_current_lba(), sec);
    int bytes = words * 4 < 2048 ? words * 4 : 2048;
    for (int j = 0; j < bytes; j++) WR8(madr + (uint32_t)j, sec[j]);
    c->V0 = 1;
}
void LibCd_CdDataSync(CpuContext *c) { c->V0 = 0; }

void LibCd_CdSearchFile(CpuContext *c)
{
    uint32_t fp = c->A0;
    char name[128]; uint32_t i;
    for (i = 0; i < 127; i++) { uint8_t b = (uint8_t)RD8(c->A1 + i); if (!b) break; name[i] = (char)b; }
    name[i] = 0;
    int lba; uint32_t size;
    if (!g_disc || !disc_locate(g_disc, name, &lba, &size)) { LOGF(g_log_sdk, "[SDK] CdSearchFile '%s' not found", name); c->V0 = 0; return; }
    uint8_t mm, ss, ff; int_to_pos(lba, &mm, &ss, &ff);
    WR8(fp + 0, mm); WR8(fp + 1, ss); WR8(fp + 2, ff); WR8(fp + 3, 0);
    WR32(fp + 4, size);
    const char *bn = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') bn = p + 1;
    for (int k = 0; k < 16; k++) WR8(fp + 8 + (uint32_t)k, k < (int)strlen(bn) ? (uint8_t)bn[k] : 0);
    c->V0 = fp;
}

void LibCd_CdSyncCallback(CpuContext *c) { c->V0 = g_cb_sync; g_cb_sync = c->A0; }
void LibCd_CdReadyCallback(CpuContext *c) { c->V0 = g_cb_ready; g_cb_ready = c->A0; }
void LibCd_CdReadCallback(CpuContext *c) { c->V0 = g_cb_data; g_cb_data = c->A0; }
void LibCd_CdDataCallback(CpuContext *c) { c->V0 = g_cb_data; g_cb_data = c->A0; }
void LibCd_CdStatus(CpuContext *c) { c->V0 = g_status; }
void LibCd_CdMode(CpuContext *c) { c->V0 = g_mode; }
void LibCd_CdLastCom(CpuContext *c) { c->V0 = g_com; }
void LibCd_CdMix(CpuContext *c) { c->V0 = 1; }

void libcd_tick(void)
{
    bool xa_mode = (g_mode & 0x40) != 0;
    if (g_read_active && xa_mode) return;
    if (!g_read_active || g_cb_data == 0) return;
    CpuContext *c = &g_cpu;
    CpuSnapshot snap; cpu_snapshot(c, &snap);
    const int max_per_tick = 32;
    int guard = 0;
    while (g_cb_data != 0 && guard++ < max_per_tick) {
        g_last_intr = CD_DATAREADY;
        if (g_cb_ready) { c->A0 = CD_DATAREADY; c->A1 = 0; disp_call(c, g_cb_ready); }
        advance_pos(1);
        disp_load_by_lba(libcd_current_lba());
        if (g_cb_data) { c->A0 = CD_DATAREADY; c->A1 = 0; disp_call(c, g_cb_data); }
    }
    cpu_restore(c, &snap);
}

/* ================================================================== */
/* LibCdStream (FMV ring buffer)                                        */
/* ================================================================== */
#define HDR 32
#define SLOT_DATA 2016
#define VIDEO_MAGIC 0x0160
#define MAX_SLOTS 256

static pthread_mutex_t g_st_mx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_st_status_base, g_st_data_base;
static int g_st_slots;
static volatile bool g_st_active, g_st_reading;
static int g_st_pending_lba = -1, g_st_lba = -1, g_st_start_lba;
static uint64_t g_st_clock_ns;
static int g_st_write_idx;
static bool g_st_busy[MAX_SLOTS];
static struct { int start, n; } g_st_ready[MAX_SLOTS]; static int g_st_ready_n;
static int g_st_prev_start = -1, g_st_prev_n;
static pthread_t g_st_thread; static volatile bool g_st_run, g_st_started;
bool g_st_in_use;

static void st_reset_ring(void)
{
    g_st_write_idx = 0; g_st_prev_start = -1; g_st_prev_n = 0; g_st_ready_n = 0;
    memset(g_st_busy, 0, sizeof g_st_busy);
    for (int i = 0; i < g_st_slots; i++) WR16(g_st_status_base + (uint32_t)(i * HDR), 0);
}

static uint16_t rd16b(const uint8_t *b, int o) { return (uint16_t)(b[o] | (b[o + 1] << 8)); }

static bool collect_frame(int start, int n)
{
    int collected = 0, lba = g_st_lba;
    uint8_t sec[2352];
    while (collected < n) {
        cd_read_sector_size(lba, 2336, sec);
        lba++;
        if (sec[2] & 0x04) { xa_decode_sector(sec, 8, sec[3]); continue; }
        uint32_t hdr = g_st_status_base + (uint32_t)((start + collected) * HDR);
        uint32_t dat = g_st_data_base + (uint32_t)((start + collected) * SLOT_DATA);
        for (int j = 0; j < HDR; j++) WR8(hdr + (uint32_t)j, sec[8 + j]);
        for (int j = 0; j < SLOT_DATA; j++) WR8(dat + (uint32_t)j, sec[8 + HDR + j]);
        collected++;
    }
    g_st_lba = lba;
    __sync_synchronize();
    return true;
}

static void *st_loop(void *arg)
{
    (void)arg;
    uint8_t sec[2352];
    while (g_st_run) {
        if (!g_disc || !g_st_active || !g_st_reading || g_st_slots <= 0) { plat_sleep_us(2000); continue; }
        if (g_st_lba < 0) {
            g_st_lba = g_st_pending_lba >= 0 ? g_st_pending_lba : libcd_current_lba();
            g_st_start_lba = g_st_lba;
            g_st_clock_ns = plat_time_ns();
        }
        disc_read_sector_data(g_disc, g_st_lba, 2336, sec);
        if (sec[2] & 0x04) { xa_decode_sector(sec, 8, sec[3]); g_st_lba++; continue; }
        bool is_str = rd16b(sec, 8) == VIDEO_MAGIC && rd16b(sec, 12) == 0;
        int n = is_str ? rd16b(sec, 14) : 1;
        if (n <= 0 || n > g_st_slots) n = 1;
        double delivered = (double)(plat_time_ns() - g_st_clock_ns) * 1e-9 * libcd_sectors_per_second();
        if ((g_st_lba - g_st_start_lba) + n > delivered) { plat_sleep_us(1000); continue; }
        int start;
        pthread_mutex_lock(&g_st_mx);
        if (g_st_write_idx + n > g_st_slots) g_st_write_idx = 0;
        start = g_st_write_idx;
        bool free_ = true;
        for (int i = 0; i < n; i++) if (g_st_busy[start + i]) { free_ = false; break; }
        pthread_mutex_unlock(&g_st_mx);
        if (!free_) { plat_sleep_us(1000); continue; }
        if (!collect_frame(start, n)) continue;
        pthread_mutex_lock(&g_st_mx);
        for (int i = 0; i < n; i++) g_st_busy[start + i] = true;
        if (g_st_ready_n < MAX_SLOTS) { g_st_ready[g_st_ready_n].start = start; g_st_ready[g_st_ready_n].n = n; g_st_ready_n++; }
        g_st_write_idx = start + n;
        pthread_mutex_unlock(&g_st_mx);
    }
    return NULL;
}

static void ensure_st_thread(void)
{
    if (g_st_started) return;
    g_st_started = true; g_st_run = true;
    pthread_create(&g_st_thread, NULL, st_loop, NULL);
}

void LibCdStream_StSetRing(CpuContext *c)
{
    g_st_in_use = true;
    pthread_mutex_lock(&g_st_mx);
    g_st_status_base = c->A0;
    g_st_slots = (int)c->A1; if (g_st_slots > MAX_SLOTS) g_st_slots = MAX_SLOTS;
    g_st_data_base = g_st_status_base + (uint32_t)(g_st_slots * HDR);
    st_reset_ring();
    pthread_mutex_unlock(&g_st_mx);
    ensure_st_thread();
}
void LibCdStream_StClearRing(CpuContext *c) { pthread_mutex_lock(&g_st_mx); st_reset_ring(); pthread_mutex_unlock(&g_st_mx); c->V0 = 0; }
void LibCdStream_StUnSetRing(CpuContext *c) { (void)c; g_st_active = false; g_st_reading = false; }
void LibCdStream_StSetStream(CpuContext *c)
{
    (void)c;
    pthread_mutex_lock(&g_st_mx);
    g_st_lba = -1; st_reset_ring(); xa_reset();
    pthread_mutex_unlock(&g_st_mx);
    g_st_active = true;
    ensure_st_thread();
}
void LibCdStream_StSetMask(CpuContext *c) { c->V0 = 0; }
void LibCdStream_StGetNext(CpuContext *c)
{
    if (!g_st_active) { c->V0 = 1; return; }
    pthread_mutex_lock(&g_st_mx);
    if (g_st_prev_start >= 0) {
        for (int i = 0; i < g_st_prev_n; i++) g_st_busy[g_st_prev_start + i] = false;
        g_st_prev_start = -1;
    }
    if (g_st_ready_n == 0) { pthread_mutex_unlock(&g_st_mx); c->V0 = 1; return; }
    int start = g_st_ready[0].start, n = g_st_ready[0].n;
    memmove(&g_st_ready[0], &g_st_ready[1], (size_t)(g_st_ready_n - 1) * sizeof g_st_ready[0]); g_st_ready_n--;
    uint32_t dp = g_st_data_base + (uint32_t)(start * SLOT_DATA);
    uint32_t hp = g_st_status_base + (uint32_t)(start * HDR);
    WR32(c->A0, dp); WR32(c->A1, hp);
    g_st_prev_start = start; g_st_prev_n = n;
    pthread_mutex_unlock(&g_st_mx);
    c->V0 = 0;
}
void LibCdStream_StFreeRing(CpuContext *c) { c->V0 = 0; }
void LibCdStream_StGetBackloc(CpuContext *c) { c->V0 = 0xFFFFFFFFu; }

void libcdstream_on_read_stream(int lba)
{
    if (!g_st_in_use) return;
    g_st_pending_lba = lba; g_st_reading = true;
    ensure_st_thread();
}
void libcdstream_on_stop_stream(void) { g_st_reading = false; xa_reset(); }

/* ================================================================== */
/* LibGpu                                                               */
/* ================================================================== */
extern void gpu_end_frame(void);

void LibGpu_DrawOTag(CpuContext *c)
{
    uint32_t addr = c->A0 & 0x1FFFFCu;
    for (int guard = 0; guard < 0x100000; guard++) {
        uint32_t header = RD32(addr);
        uint32_t count = header >> 24;
        for (uint32_t i = 0; i < count; i++) gpu_write_gp0(RD32(addr + 4u + i * 4u));
        uint32_t next = header & 0xFFFFFFu;
        if (next == 0xFFFFFFu || (next & 0x800000u) != 0) break;
        addr = next & 0x1FFFFCu;
    }
    gpu_end_frame();
}
void LibGpu_DrawSync(CpuContext *c) { c->V0 = 0; }

static int16_t s16(uint32_t a) { return (int16_t)RD16(a); }
static uint32_t get_cs(int x, int y) { x = clampi(x, 0, VRAM_W - 1); y = clampi(y, 0, VRAM_H - 1); return 0xE3000000u | (((uint32_t)y & 0x3FF) << 10) | ((uint32_t)x & 0x3FF); }
static uint32_t get_ce(int x, int y) { x = clampi(x, 0, VRAM_W - 1); y = clampi(y, 0, VRAM_H - 1); return 0xE4000000u | (((uint32_t)y & 0x3FF) << 10) | ((uint32_t)x & 0x3FF); }
static uint32_t get_ofs(int x, int y) { return 0xE5000000u | (((uint32_t)y & 0x7FF) << 11) | ((uint32_t)x & 0x7FF); }
static uint32_t get_mode(int dfe, int dtd, uint16_t tpage)
{ return (dtd ? 0xE1000200u : 0xE1000000u) | (dfe ? 0x400u : 0u) | ((uint32_t)tpage & 0x9FF); }
static uint32_t get_tw(int x, int y, int w, int h)
{
    uint32_t c0 = ((uint32_t)x & 0xFF) >> 3, c1 = ((uint32_t)y & 0xFF) >> 3;
    uint32_t c2 = ((uint32_t)(-w) & 0xFF) >> 3, c3 = ((uint32_t)(-h) & 0xFF) >> 3;
    return 0xE2000000u | (c1 << 15) | (c0 << 10) | (c3 << 5) | c2;
}

void LibGpu_PutDrawEnv(CpuContext *c)
{
    uint32_t env = c->A0;
    int clipX = s16(env + 0x00), clipY = s16(env + 0x02), clipW = s16(env + 0x04), clipH = s16(env + 0x06);
    int ofsX = s16(env + 0x08), ofsY = s16(env + 0x0A);
    int twX = s16(env + 0x0C), twY = s16(env + 0x0E), twW = s16(env + 0x10), twH = s16(env + 0x12);
    uint16_t tpage = (uint16_t)RD16(env + 0x14);
    uint8_t dtd = (uint8_t)RD8(env + 0x16), dfe = (uint8_t)RD8(env + 0x17), isbg = (uint8_t)RD8(env + 0x18);
    uint8_t r0 = (uint8_t)RD8(env + 0x19), g0 = (uint8_t)RD8(env + 0x1A), b0 = (uint8_t)RD8(env + 0x1B);
    gpu_write_gp0(get_cs(clipX, clipY));
    gpu_write_gp0(get_ce(clipX + clipW - 1, clipY + clipH - 1));
    gpu_write_gp0(get_ofs(ofsX, ofsY));
    gpu_write_gp0(get_mode(dfe, dtd, tpage));
    gpu_write_gp0(get_tw(twX, twY, twW, twH));
    gpu_write_gp0(0xE6000000u);
    if (isbg) {
        int w = clampi(clipW, 0, VRAM_W - 1), h = clampi(clipH, 0, VRAM_H - 1);
        int x = clipX - ofsX, y = clipY - ofsY;
        gpu_write_gp0(0x60000000u | ((uint32_t)b0 << 16) | ((uint32_t)g0 << 8) | r0);
        gpu_write_gp0(((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
        gpu_write_gp0(((uint32_t)(uint16_t)h << 16) | (uint16_t)w);
    }
    c->V0 = c->A0;
}

void LibGpu_PutDispEnv(CpuContext *c)
{
    uint32_t env = c->A0;
    int dispX = s16(env + 0x00), dispY = s16(env + 0x02), dispW = s16(env + 0x04), dispH = s16(env + 0x06);
    int scrX = s16(env + 0x08), scrY = s16(env + 0x0A), scrW = s16(env + 0x0C), scrH = s16(env + 0x0E);
    uint8_t isinter = (uint8_t)RD8(env + 0x10), isrgb24 = (uint8_t)RD8(env + 0x11);
    bool pal = gpu_pal();
    gpu_write_gp1(0x05000000u | (((uint32_t)dispY & 0x3FF) << 10) | ((uint32_t)dispX & 0x3FF));
    int hStart = scrX * 10 + 0x260, vStart = scrY + (pal ? 0x13 : 0x10);
    int hEnd = hStart + (scrW != 0 ? scrW * 10 : 2560), vEnd = vStart + (scrH != 0 ? scrH : 240);
    hStart = clampi(hStart, 500, 3290);
    hEnd = clampi(hEnd, hStart + 0x50, 3290);
    vStart = clampi(vStart, 0x10, pal ? 310 : 256);
    vEnd = clampi(vEnd, vStart + 2, pal ? 312 : 258);
    gpu_write_gp1(0x06000000u | (((uint32_t)hEnd & 0xFFF) << 12) | ((uint32_t)hStart & 0xFFF));
    gpu_write_gp1(0x07000000u | (((uint32_t)vEnd & 0x3FF) << 10) | ((uint32_t)vStart & 0x3FF));
    uint32_t mode = 0x08000000u;
    if (pal) mode |= 0x8;
    if (isrgb24) mode |= 0x10;
    if (isinter) mode |= 0x20;
    if (dispW <= 280) { }
    else if (dispW <= 352) mode |= 1;
    else if (dispW <= 400) mode |= 0x40;
    else if (dispW <= 560) mode |= 2;
    else mode |= 3;
    if (dispH > (pal ? 288 : 256)) mode |= 0x24;
    gpu_write_gp1(mode);
    c->V0 = c->A0;
}

/* ================================================================== */
/* LibPad                                                               */
/* ================================================================== */
static uint32_t g_pbuf1, g_pbuf2;
static int g_small_motor = 0, g_large_motor = 1;

void LibPad_PadInitDirect(CpuContext *c) { g_pbuf1 = c->A0; g_pbuf2 = c->A1; c->V0 = 0; }
void LibPad_PadStartCom(CpuContext *c) { libpad_refresh(); c->V0 = 0; }
void LibPad_PadStopCom(CpuContext *c) { c->V0 = 0; }
void LibPad_PadEnableCom(CpuContext *c) { c->V0 = 0; }
void LibPad_PadChkVsync(CpuContext *c) { c->V0 = 1; }
void LibPad_PadChkMtap(CpuContext *c) { c->V0 = 0; }
void LibPad_PadGetState(CpuContext *c) { c->V0 = ((c->A0 & 0x10u) == 0) ? 6u : 0u; }
void LibPad_PadInfoMode(CpuContext *c) { c->V0 = 0; }
void LibPad_PadInfoComb(CpuContext *c) { c->V0 = 0; }
void LibPad_PadInfoAct(CpuContext *c) { c->V0 = (int)c->A2 < 0 ? 2u : 1u; }
void LibPad_PadSetMainMode(CpuContext *c) { c->V0 = 0; }
void LibPad_PadSetActAlign(CpuContext *c)
{
    if (c->A0 & 0x10u) { c->V0 = 1; return; }
    uint32_t ptr = c->A1, len = c->A2;
    if (!ptr || len < 2) { c->V0 = 0; return; }
    for (int i = 0; i < (int)len && i < 6; i++) {
        uint8_t v = (uint8_t)RD8(ptr + (uint32_t)i);
        if (v == 0x00) g_small_motor = i; else if (v == 0x01) g_large_motor = i;
    }
    c->V0 = 1;
}
void LibPad_PadSetAct(CpuContext *c)
{
    if (c->A0 & 0x10u) { c->V0 = 1; return; }
    uint32_t ptr = c->A1, len = c->A2;
    if (!ptr || !len) { c->V0 = 0; return; }
    uint8_t small_ = g_small_motor < (int)len ? (uint8_t)RD8(ptr + (uint32_t)g_small_motor) : 0;
    uint8_t large = g_large_motor < (int)len ? (uint8_t)RD8(ptr + (uint32_t)g_large_motor) : 0;
    plat_rumble(large, small_);
    c->V0 = 1;
}

static void write_pad(uint32_t buf, uint16_t buttons, bool present, uint8_t rx, uint8_t ry, uint8_t lx, uint8_t ly)
{
    WR8(buf + 0, present ? 0x00 : 0xFF);
    WR8(buf + 1, present ? 0x41 : 0xFF);
    WR8(buf + 2, buttons & 0xFF);
    WR8(buf + 3, buttons >> 8);
    WR8(buf + 4, present ? rx : 0x80); WR8(buf + 5, present ? ry : 0x80);
    WR8(buf + 6, present ? lx : 0x80); WR8(buf + 7, present ? ly : 0x80);
}
void libpad_refresh(void)
{
    if (g_pbuf1) write_pad(g_pbuf1, pad_state(), true, g_pad_rx, g_pad_ry, g_pad_lx, g_pad_ly);
    if (g_pbuf2) write_pad(g_pbuf2, 0xFFFF, false, 0x80, 0x80, 0x80, 0x80);
}

/* ================================================================== */
/* LibMem                                                               */
/* ================================================================== */
void LibMem_RMemcpy(CpuContext *c)
{
    uint32_t src = c->A0, dst = c->A1, n = c->A2;
    if (!n) return;
    if (dst > src && dst < src + n * 2u) {
        for (uint32_t i = n; i > 0; i--) WR16(dst + (i - 1) * 2u, RD16(src + (i - 1) * 2u));
        return;
    }
    for (uint32_t i = 0; i < n; i++) WR16(dst + i * 2u, RD16(src + i * 2u));
}
