/* cdc.c - CdController ported from RecompOne.Runtime.Cdrom.
 * All state is touched only from the emulation thread; the FMV / XA helper
 * threads read sectors through disc_read_sector_data() directly. */
#include "psx.h"

#define FIFO_MAX 64
#define PENDING_MAX 32

typedef struct { uint8_t type; uint8_t len; uint8_t resp[8]; } PendIrq;

static uint8_t g_index;
static uint8_t g_param[FIFO_MAX]; static int g_param_n;
static uint8_t g_resp[FIFO_MAX]; static int g_resp_n, g_resp_pos;
static PendIrq g_pend[PENDING_MAX]; static int g_pend_n, g_pend_head;
static uint8_t g_irq_flags;
static int g_seek_lba;
static uint8_t g_data_buf[2048];
static int g_data_len = 2048;
static int g_data_pos;
static bool g_data_ready, g_reading, g_stream_pending;
static uint8_t g_last_irq;
static bool g_has_read_any;
static int g_last_read_lba;
static int64_t g_sectors_read;

/* ------------------------------------------------------------------ */
static int bcd_to_lba(uint8_t mm, uint8_t ss, uint8_t ff)
{
    int m = (mm >> 4) * 10 + (mm & 0xF), s = (ss >> 4) * 10 + (ss & 0xF), f = (ff >> 4) * 10 + (ff & 0xF);
    return (m * 60 + s) * 75 + f - 150;
}
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void lba_to_bcd(int lba, uint8_t *mm, uint8_t *ss, uint8_t *ff)
{
    int total = lba + 150;
    *ff = to_bcd(total % 75); *ss = to_bcd((total / 75) % 60); *mm = to_bcd(total / 75 / 60);
}

static uint8_t drive_status(void) { uint8_t s = 0x02; if (g_reading) s |= 0x20; return s; }
uint8_t cd_drive_status_byte(void) { return drive_status(); }

static void set_in_interrupt(uint16_t v) { if (g_intr_env_addr) WR16(g_intr_env_addr, v); }

static void resp_set(const uint8_t *b, int n)
{
    g_resp_n = n; g_resp_pos = 0;
    if (n > FIFO_MAX) g_resp_n = FIFO_MAX;
    memcpy(g_resp, b, (size_t)g_resp_n);
}

static void deliver_immediate(uint8_t type, const uint8_t *resp, int n)
{
    resp_set(resp, n);
    g_irq_flags = type;
    g_last_irq = type;
    set_in_interrupt(1);
}

static void queue_irq(uint8_t type, const uint8_t *resp, int n)
{
    if (g_irq_flags == 0 && g_pend_n == 0) { deliver_immediate(type, resp, n); return; }
    if (g_pend_n >= PENDING_MAX) return;
    PendIrq *p = &g_pend[(g_pend_head + g_pend_n) % PENDING_MAX];
    p->type = type; p->len = (uint8_t)(n > 8 ? 8 : n); memcpy(p->resp, resp, p->len);
    g_pend_n++;
}
#define QI(type, ...) do { const uint8_t _r[] = { __VA_ARGS__ }; queue_irq((type), _r, (int)sizeof _r); } while (0)

static void deliver_next(void)
{
    PendIrq p = g_pend[g_pend_head];
    g_pend_head = (g_pend_head + 1) % PENDING_MAX; g_pend_n--;
    resp_set(p.resp, p.len);
    g_irq_flags = p.type; g_last_irq = p.type;
    set_in_interrupt(1);
}

static void read_next_sector(void)
{
    disc_read_sector(g_disc, g_seek_lba, g_data_buf);
    g_data_len = 2048;
    g_last_read_lba = g_seek_lba;
    g_has_read_any = true;
    g_sectors_read++;
    g_seek_lba++;
}

static void after_ack(void)
{
    if (g_pend_n > 0) { deliver_next(); return; }
    if (g_reading && g_last_irq == 1) g_stream_pending = true;
    set_in_interrupt(0);
}

void cd_advance_streaming(void)
{
    if (!g_reading || !g_stream_pending) return;
    if (g_irq_flags != 0 || g_pend_n > 0) return;
    g_stream_pending = false;
    read_next_sector();
    const uint8_t r[] = { drive_status() };
    deliver_immediate(1, r, 1);
}

static uint8_t read_data_byte(void)
{
    if (!g_data_ready || g_data_pos >= g_data_len) { g_data_ready = false; return 0; }
    uint8_t b = g_data_buf[g_data_pos++];
    if (g_data_pos >= g_data_len) g_data_ready = false;
    return b;
}

void cd_dma_read(uint32_t addr, uint32_t byte_count)
{
    for (uint32_t i = 0; i < byte_count; i++)
        WR8(addr + i, g_data_pos < g_data_len ? g_data_buf[g_data_pos++] : 0);
    if (g_data_pos >= g_data_len) g_data_ready = false;
}

static void execute_command(uint8_t cmd)
{
    uint8_t prm[FIFO_MAX]; int np = g_param_n;
    memcpy(prm, g_param, (size_t)np); g_param_n = 0;
    LOGF(g_log_cd, "[CD] cmd 0x%02X", cmd);
    switch (cmd) {
    case 0x01: QI(3, drive_status()); break;
    case 0x02:
        if (np >= 3) g_seek_lba = bcd_to_lba(prm[0], prm[1], prm[2]);
        QI(3, drive_status());
        break;
    case 0x06:
    case 0x1B:
        g_reading = true;
        read_next_sector();
        QI(3, drive_status());
        QI(1, drive_status());
        break;
    case 0x08:
    case 0x09:
        g_reading = false; g_stream_pending = false;
        QI(3, drive_status());
        QI(2, drive_status());
        break;
    case 0x0A: QI(3, drive_status()); QI(2, drive_status()); break;
    case 0x0B: case 0x0C: case 0x0E: case 0x0D: QI(3, drive_status()); break;
    case 0x10: {
        if (!g_has_read_any) { QI(5, 0x80); break; }
        uint8_t mm, ss, ff; lba_to_bcd(g_last_read_lba, &mm, &ss, &ff);
        QI(3, mm, ss, ff, 0x02, 0x00, 0x00, 0x00, 0x00);
        break; }
    case 0x11: {
        if (!g_has_read_any) { QI(5, 0x80); break; }
        uint8_t mm, ss, ff; lba_to_bcd(g_last_read_lba, &mm, &ss, &ff);
        QI(3, 0x01, 0x01, mm, ss, ff, mm, ss, ff);
        break; }
    case 0x0F: QI(3, drive_status(), 0x00, 0x00, 0x00, 0x00); break;
    case 0x12: QI(3, drive_status()); QI(2, drive_status()); break;
    case 0x13: QI(3, drive_status(), 0x01, 0x01); break;
    case 0x14: QI(3, drive_status(), 0x00, 0x02); break;
    case 0x19: {
        uint8_t sub = np > 0 ? prm[0] : 0;
        if (sub == 0x20) { QI(3, 0x94, 0x09, 0x19, 0xC0); }
        else { QI(3, drive_status()); }
        break; }
    case 0x1E: QI(3, drive_status()); QI(2, drive_status()); break;
    case 0x1A: QI(3, 0x02, 0x00); QI(2, 0x02, 0x00, 0x20, 0x00, 0x53, 0x43, 0x45, 0x41); break;
    case 0x03: case 0x07: case 0x04: case 0x05: QI(3, drive_status()); break;
    case 0x15: case 0x16: QI(3, drive_status()); QI(2, drive_status()); break;
    default:
        QI(5, drive_status(), 0x40);
        break;
    }
}

uint8_t cd_read(uint32_t phys)
{
    switch (phys & 3) {
    case 0:
        return (uint8_t)((g_index & 3) | (g_param_n == 0 ? 0x08 : 0) | 0x10 |
                         ((g_resp_pos < g_resp_n) ? 0x20 : 0) | (g_data_ready ? 0x40 : 0));
    case 1:
        return g_resp_pos < g_resp_n ? g_resp[g_resp_pos++] : 0;
    case 2:
        return read_data_byte();
    default:
        return g_index == 1 ? g_irq_flags : 0;
    }
}

void cd_write(uint32_t phys, uint8_t val)
{
    switch (phys & 3) {
    case 0: g_index = val & 3; break;
    case 1: if (g_index == 0) execute_command(val); break;
    case 2:
        if (g_index == 0) { if (g_param_n < FIFO_MAX) g_param[g_param_n++] = val; }
        else if (g_index == 1) g_param_n = 0;
        break;
    case 3:
        if (g_index == 0) {
            if (val & 0x80) { g_data_pos = 0; g_data_ready = true; } else g_data_ready = false;
        } else if (g_index == 1) {
            g_irq_flags &= (uint8_t)~val;
            if (g_irq_flags == 0) after_ack();
        }
        break;
    }
}

void cd_init(Disc *d) { g_disc = d; }

void cd_load_to_memory(const char *path, uint32_t address, int offset, int length)
{
    uint32_t sz = 0;
    uint8_t *data = disc_read_file(g_disc, path, &sz);
    if (!data) { char m[256]; snprintf(m, sizeof m, "boot file not found on disc: %s", path); plat_fatal(m); return; }
    int count = length < 0 ? (int)sz - offset : length;
    if (offset + count > (int)sz) count = (int)sz - offset;
    mem_load_bytes(address, data + offset, (uint32_t)count);
    LOG("[CD] %s -> 0x%08X | %d bytes", path, address, count);
    free(data);
}

void cd_read_sector_2048(int lba, uint8_t *out) { disc_read_sector(g_disc, lba, out); }

/* CdController.ReadSectorData(lba): seeks, reads, returns 2048 bytes */
void cd_read_sector_state(int lba, uint8_t *out2048)
{
    g_seek_lba = lba;
    read_next_sector();
    memcpy(out2048, g_data_buf, 2048);
}

/* CdController.ReadSectorData(lba,size): raw read + stats */
void cd_read_sector_size(int lba, int size, uint8_t *out)
{
    g_last_read_lba = lba;
    g_sectors_read++;
    disc_read_sector_data(g_disc, lba, size, out);
}

void cd_queue_async_seekl(uint8_t mm, uint8_t ss, uint8_t ff)
{
    g_seek_lba = bcd_to_lba(mm, ss, ff);
    QI(3, drive_status());
    QI(2, drive_status());
}
void cd_queue_async_get_status(void) { QI(3, drive_status()); }
void cd_queue_async_set_mode(uint8_t mode) { (void)mode; QI(3, drive_status()); }

/* The C# version copies the sectors on a worker thread; disc reads here are
 * fast enough (pread on local flash) that doing it inline keeps the IRQ
 * ordering identical without any locking. */
void cd_queue_async_read_sector(uint32_t count, uint32_t dst, uint32_t mode)
{
    (void)mode;
    int lba = g_seek_lba;
    g_seek_lba += (int)count;
    g_reading = true;
    uint8_t sec[2048];
    for (uint32_t i = 0; i < count; i++) {
        disc_read_sector_data(g_disc, lba, 2048, sec);
        g_sectors_read++;
        mem_load_bytes(dst + i * 2048u, sec, 2048);
        lba++;
    }
    g_reading = false;
    g_last_read_lba = lba - 1;
    QI(3, drive_status());
    QI(1, drive_status());
    QI(2, drive_status());
}
