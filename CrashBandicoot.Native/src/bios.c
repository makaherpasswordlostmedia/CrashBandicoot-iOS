/* bios.c - Bios, BiosA, BiosB, BiosC ported from RecompOne.Runtime.Bios */
#include "psx.h"
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */
static void read_cstr(uint32_t addr, char *out, size_t max)
{
    size_t i = 0; uint8_t b;
    while ((b = (uint8_t)RD8(addr++)) != 0) { if (i + 1 < max) out[i++] = (char)b; }
    out[i] = 0;
}

static void format_string(CpuContext *c, const char *fmt, char *out, size_t max)
{
    size_t o = 0; int ai = 0;
    for (size_t i = 0; fmt[i];) {
        if (fmt[i] != '%') { if (o + 1 < max) out[o++] = fmt[i]; i++; continue; }
        if (!fmt[++i]) break;
        if (fmt[i] == '%') { if (o + 1 < max) out[o++] = '%'; i++; continue; }
        if (fmt[i] == '-') i++;
        bool zero = fmt[i] == '0'; if (zero) i++;
        int width = 0; while (isdigit((unsigned char)fmt[i])) width = width * 10 + (fmt[i++] - '0');
        if (fmt[i] == '.') { i++; while (isdigit((unsigned char)fmt[i])) i++; }
        if (!fmt[i]) break;
        char conv = fmt[i++];
        int idx = ai++;
        uint32_t arg = idx == 0 ? c->A1 : idx == 1 ? c->A2 : idx == 2 ? c->A3 : RD32(c->SP + 16u + (uint32_t)((idx - 3) * 4));
        char tmp[300];
        switch (conv) {
        case 'd': case 'i': snprintf(tmp, sizeof tmp, "%d", (int)arg); break;
        case 'u': snprintf(tmp, sizeof tmp, "%u", arg); break;
        case 'x': snprintf(tmp, sizeof tmp, "%x", arg); break;
        case 'X': snprintf(tmp, sizeof tmp, "%X", arg); break;
        case 'o': snprintf(tmp, sizeof tmp, "%o", arg); break;
        case 'c': snprintf(tmp, sizeof tmp, "%c", (char)(arg & 0xFF)); break;
        case 's': if (arg) read_cstr(arg, tmp, sizeof tmp); else strcpy(tmp, "(null)"); break;
        default: snprintf(tmp, sizeof tmp, "%%%c", conv); ai--; break;
        }
        int len = (int)strlen(tmp);
        for (int p = len; p < width; p++) if (o + 1 < max) out[o++] = zero ? '0' : ' ';
        for (int k = 0; k < len; k++) if (o + 1 < max) out[o++] = tmp[k];
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* Heap (BIOS A(33h)... malloc family)                                  */
/* ------------------------------------------------------------------ */
#define HEAP_MAX 4096
typedef struct { uint32_t start, end; } Span;
static Span g_free[HEAP_MAX]; static int g_free_n;
typedef struct { uint32_t addr, size; } Busy;
static Busy g_busy[HEAP_MAX]; static int g_busy_n;

static void free_insert(uint32_t s, uint32_t e)
{
    int i = 0;
    while (i < g_free_n && g_free[i].start < s) i++;
    if (g_free_n >= HEAP_MAX) return;
    memmove(&g_free[i + 1], &g_free[i], (size_t)(g_free_n - i) * sizeof(Span));
    g_free[i].start = s; g_free[i].end = e; g_free_n++;
}
static void init_heap(uint32_t base, uint32_t size)
{
    g_free_n = g_busy_n = 0;
    free_insert(base, base + size);
}
static uint32_t h_malloc(uint32_t size)
{
    if (size == 0) size = 4;
    size = (size + 3) & ~3u;
    for (int i = 0; i < g_free_n; i++) {
        if (g_free[i].end - g_free[i].start >= size) {
            uint32_t addr = g_free[i].start, end = g_free[i].end;
            memmove(&g_free[i], &g_free[i + 1], (size_t)(g_free_n - i - 1) * sizeof(Span)); g_free_n--;
            if (end - addr > size) free_insert(addr + size, end);
            if (g_busy_n < HEAP_MAX) { g_busy[g_busy_n].addr = addr; g_busy[g_busy_n].size = size; g_busy_n++; }
            return addr;
        }
    }
    return 0;
}
static int busy_find(uint32_t addr) { for (int i = 0; i < g_busy_n; i++) if (g_busy[i].addr == addr) return i; return -1; }
static void h_free(uint32_t addr)
{
    if (!addr) return;
    int bi = busy_find(addr); if (bi < 0) return;
    uint32_t size = g_busy[bi].size;
    g_busy[bi] = g_busy[--g_busy_n];
    uint32_t end = addr + size;
    for (int i = 0; i < g_free_n; i++)
        if (g_free[i].start == end) {
            end = g_free[i].end;
            memmove(&g_free[i], &g_free[i + 1], (size_t)(g_free_n - i - 1) * sizeof(Span)); g_free_n--;
            break;
        }
    free_insert(addr, end);
}

/* byte-wise memory helpers over emulated RAM */
static uint32_t b_memcpy(uint32_t dst, uint32_t src, uint32_t n) { for (uint32_t i = 0; i < n; i++) WR8(dst + i, RD8(src + i)); return dst; }
static uint32_t b_memset(uint32_t p, uint8_t v, uint32_t n) { for (uint32_t i = 0; i < n; i++) WR8(p + i, v); return p; }
static uint32_t b_memmove(uint32_t d, uint32_t s, uint32_t n)
{
    if (d <= s || d >= s + n) return b_memcpy(d, s, n);
    for (uint32_t i = n; i > 0; i--) WR8(d + i - 1, RD8(s + i - 1));
    return d;
}
static uint32_t b_memcmp(uint32_t a, uint32_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { uint8_t x = (uint8_t)RD8(a + i), y = (uint8_t)RD8(b + i); if (x != y) return x < y ? 0xFFFFFFFFu : 1u; }
    return 0;
}
static uint32_t b_memchr(uint32_t p, uint8_t v, uint32_t n) { for (uint32_t i = 0; i < n; i++) if ((uint8_t)RD8(p + i) == v) return p + i; return 0; }
static uint32_t h_realloc(uint32_t ptr, uint32_t size)
{
    if (!ptr) return h_malloc(size);
    if (!size) { h_free(ptr); return 0; }
    int bi = busy_find(ptr);
    uint32_t old = bi >= 0 ? g_busy[bi].size : 0;
    uint32_t np = h_malloc(size);
    if (!np) return 0;
    b_memcpy(np, ptr, old < size ? old : size);
    h_free(ptr);
    return np;
}
static uint32_t b_strlen(uint32_t a) { uint32_t n = 0; while (RD8(a++) != 0) n++; return n; }
static uint32_t b_strcmp(uint32_t a, uint32_t b)
{
    for (;;) { uint8_t x = (uint8_t)RD8(a++), y = (uint8_t)RD8(b++); if (x != y) return x < y ? 0xFFFFFFFFu : 1u; if (!x) return 0; }
}
static uint32_t b_strncmp(uint32_t a, uint32_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { uint8_t x = (uint8_t)RD8(a++), y = (uint8_t)RD8(b++); if (x != y) return x < y ? 0xFFFFFFFFu : 1u; if (!x) return 0; }
    return 0;
}
static uint32_t b_strcpy(uint32_t dst, uint32_t src) { uint32_t d = dst; uint8_t b; do { b = (uint8_t)RD8(src++); WR8(d++, b); } while (b); return dst; }
static uint32_t b_strncpy(uint32_t dst, uint32_t src, uint32_t n)
{
    uint32_t d = dst;
    for (uint32_t i = 0; i < n; i++) { uint8_t b = (uint8_t)RD8(src++); WR8(d++, b); if (!b) { while (++i < n) WR8(d++, 0); break; } }
    return dst;
}
static uint32_t b_strcat(uint32_t dst, uint32_t src) { uint32_t d = dst; while (RD8(d)) d++; uint8_t b; do { b = (uint8_t)RD8(src++); WR8(d++, b); } while (b); return dst; }
static uint32_t b_strncat(uint32_t dst, uint32_t src, uint32_t n)
{
    uint32_t d = dst; while (RD8(d)) d++;
    for (uint32_t i = 0; i < n; i++) { uint8_t b = (uint8_t)RD8(src++); if (!b) break; WR8(d++, b); }
    WR8(d, 0); return dst;
}
static uint32_t b_strchr(uint32_t s, uint32_t ch)
{
    uint8_t t = (uint8_t)ch; for (uint32_t a = s;; a++) { uint8_t b = (uint8_t)RD8(a); if (b == t) return a; if (!b) return 0; }
}
static uint32_t b_strrchr(uint32_t s, uint32_t ch)
{
    uint8_t t = (uint8_t)ch; uint32_t last = 0; for (uint32_t a = s;; a++) { uint8_t b = (uint8_t)RD8(a); if (b == t) last = a; if (!b) return last; }
}
static uint32_t b_strpbrk(uint32_t s, uint32_t acc) { for (uint32_t a = s;; a++) { uint8_t b = (uint8_t)RD8(a); if (!b) return 0; if (b_strchr(acc, b)) return a; } }
static uint32_t b_strspn(uint32_t s, uint32_t acc) { uint32_t n = 0; for (;; n++) { uint8_t b = (uint8_t)RD8(s + n); if (!b || !b_strchr(acc, b)) return n; } }
static uint32_t b_strcspn(uint32_t s, uint32_t rej) { uint32_t n = 0; for (;; n++) { uint8_t b = (uint8_t)RD8(s + n); if (!b || b_strchr(rej, b)) return n; } }
static uint32_t b_strstr(uint32_t s, uint32_t sub)
{
    uint32_t sl = b_strlen(sub); if (!sl) return s;
    uint32_t l = b_strlen(s);
    for (uint32_t i = 0; i + sl <= l; i++) if (b_strncmp(s + i, sub, sl) == 0) return s + i;
    return 0;
}
static uint32_t g_strtok;
static uint32_t b_strtok(uint32_t s, uint32_t delim)
{
    if (s) g_strtok = s;
    while (g_strtok && RD8(g_strtok) && b_strchr(delim, RD8(g_strtok))) g_strtok++;
    if (RD8(g_strtok) == 0) return 0;
    uint32_t start = g_strtok;
    while (RD8(g_strtok) && !b_strchr(delim, RD8(g_strtok))) g_strtok++;
    if (RD8(g_strtok)) { WR8(g_strtok, 0); g_strtok++; }
    return start;
}
static uint32_t g_rand = 1;
static uint32_t b_rand(void) { g_rand = g_rand * 1103515245u + 12345u; return (g_rand >> 16) & 0x7FFFu; }

static uint32_t b_strtoul(uint32_t str, uint32_t endp, uint32_t base)
{
    char s[128]; read_cstr(str, s, sizeof s);
    int i = 0; while (isspace((unsigned char)s[i])) i++;
    if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X') && (base == 0 || base == 16)) { i += 2; base = 16; }
    else if (base == 0) base = s[i] == '0' ? 8u : 10u;
    uint32_t r = 0;
    for (; s[i]; i++) {
        int d = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : s[i] >= 'a' && s[i] <= 'f' ? s[i] - 'a' + 10 : s[i] >= 'A' && s[i] <= 'F' ? s[i] - 'A' + 10 : -1;
        if (d < 0 || d >= (int)base) break;
        r = r * base + (uint32_t)d;
    }
    if (endp) WR32(endp, str + (uint32_t)i);
    return r;
}
static int32_t b_atoi(uint32_t a)
{
    char s[64]; read_cstr(a, s, sizeof s);
    int i = 0, sign = 1, r = 0; while (isspace((unsigned char)s[i])) i++;
    if (s[i] == '-') { sign = -1; i++; } else if (s[i] == '+') i++;
    while (isdigit((unsigned char)s[i])) r = r * 10 + (s[i++] - '0');
    return sign * r;
}
static int32_t b_strtol(uint32_t str, uint32_t endp, uint32_t base)
{
    char s[64]; read_cstr(str, s, sizeof s);
    int i = 0, sign = 1; while (isspace((unsigned char)s[i])) i++;
    if (s[i] == '-') { sign = -1; i++; } else if (s[i] == '+') i++;
    return sign * (int32_t)b_strtoul(str + (uint32_t)i, endp, base);
}

/* ------------------------------------------------------------------ */
/* file handles + card handles (A(00h)-A(04h))                          */
/* ------------------------------------------------------------------ */
typedef struct { bool used; char name[32]; uint8_t *data; uint32_t len, off; } OpenFile;
typedef struct { bool used; MemoryCard *card; int chain[16]; int nchain; int size, pos; } CardFile;
#define MAXFD 32
static OpenFile g_files[MAXFD]; static CardFile g_cfiles[MAXFD];
static uint32_t g_next_handle = 2;
static uint32_t g_errno;
static int g_ff_n, g_ff_idx; static CardDirEnt g_ff[16];

static MemoryCard *card_for(const char *path)
{
    if (!strncasecmp(path, "bu00:", 5)) return g_card_a && g_card_a->enabled ? g_card_a : NULL;
    if (!strncasecmp(path, "bu10:", 5)) return g_card_b && g_card_b->enabled ? g_card_b : NULL;
    return NULL;
}
static const char *card_name(const char *path) { const char *c = strchr(path, ':'); return c ? c + 1 : path; }

static void extract_file_name(const char *raw, char *out, size_t max)
{
    const char *p = strchr(raw, ':'); p = p ? p + 1 : raw;
    char tmp[128]; snprintf(tmp, sizeof tmp, "%s", p);
    char *semi = strchr(tmp, ';'); if (semi) *semi = 0;
    for (char *q = tmp; *q; q++) if (*q == '\\') *q = '/';
    char *sl = strrchr(tmp, '/');
    snprintf(out, max, "%s", sl ? sl + 1 : tmp);
}
static void overlay_name(const char *fname, char *out, size_t max)
{
    snprintf(out, max, "%s", fname);
    char *dot = strrchr(out, '.'); if (dot) *dot = 0;
    for (char *q = out; *q; q++) *q = (char)tolower((unsigned char)*q);
}

static int alloc_fd_slot(void) { for (int i = 0; i < MAXFD; i++) if (!g_files[i].used && !g_cfiles[i].used) return i; return -1; }
/* handles are small ints starting at 2; map handle -> slot by (handle-2)%MAXFD */
static OpenFile *of_get(uint32_t h) { OpenFile *f = &g_files[(h - 2) % MAXFD]; return f->used ? f : NULL; }
static CardFile *cf_get(uint32_t h) { CardFile *f = &g_cfiles[(h - 2) % MAXFD]; return f->used ? f : NULL; }

/* ------------------------------------------------------------------ */
/* Events / cards / pad (BIOS B)                                        */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t status, cls, spec, mode, func; } EvCB;
#define MAX_EVENTS 64
static EvCB g_ev[MAX_EVENTS];
#define MAX_THREADS 4
static bool g_th_used[MAX_THREADS];
static uint32_t g_int_chain[4];
static uint32_t g_pad_buf, g_card_chan, g_card_status = 1;
static bool g_card_new_a = true, g_card_new_b = true;
static uint32_t g_conf_ev = 16, g_conf_tcb = 4, g_conf_stack;

void bios_deliver_event(uint32_t cls, uint32_t spec)
{
    for (int i = 0; i < MAX_EVENTS; i++)
        if (g_ev[i].status == 2u && g_ev[i].cls == cls && g_ev[i].spec == spec) g_ev[i].status = 4u;
}

static void deliver_event_intr(CpuContext *c, uint32_t cls, uint32_t spec)
{
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (g_ev[i].status != 2u && g_ev[i].status != 4u) continue;
        if (g_ev[i].cls != cls || g_ev[i].spec != spec) continue;
        if ((g_ev[i].mode & 0x1000u) && !(g_ev[i].mode & 0x2000u) && g_ev[i].func) {
            CpuSnapshot snap; cpu_snapshot(c, &snap);
            disp_call(c, g_ev[i].func);
            cpu_restore(c, &snap);
        }
        g_ev[i].status = 4u;
    }
}
static void signal_sw_card(CpuContext *c, uint32_t spec) { deliver_event_intr(c, 0xF4000001u, spec); }
static void signal_hw_card(CpuContext *c, uint32_t spec) { deliver_event_intr(c, 0xF0000011u, spec); }

static void card_complete(CpuContext *c, uint32_t port)
{
    MemoryCard *cd = (port & 0x10u) ? g_card_b : g_card_a;
    g_card_chan = port;
    g_card_status = cd->enabled ? 1u : 0x11u;
    signal_sw_card(c, cd->enabled ? 0x0004u : 0x8000u);
}
static void card_info(CpuContext *c)
{
    g_card_chan = c->A0;
    bool slot_b = (c->A0 & 0x10u) != 0;
    MemoryCard *cd = slot_b ? g_card_b : g_card_a;
    g_card_status = cd->enabled ? 1u : 0x11u;
    uint32_t spec;
    if (!cd->enabled) spec = 0x0100u;
    else if (slot_b ? g_card_new_b : g_card_new_a) spec = 0x2000u;
    else spec = 0x0004u;
    signal_sw_card(c, spec);
    signal_hw_card(c, spec);
    c->V0 = 1;
}
static void card_read_b(CpuContext *c)
{
    MemoryCard *cd = (c->A0 & 0x10u) ? g_card_b : g_card_a;
    g_card_chan = c->A0;
    bool ok = cd->enabled && c->A2 != 0u;
    if (ok) {
        uint8_t f[0x80];
        card_frame_read(cd, (int)(c->A1 & 0x3FFu), f);
        for (uint32_t i = 0; i < 0x80u; i++) WR8(c->A2 + i, f[i]);
        g_card_status = 1u;
    } else g_card_status = 0x11u;
    signal_hw_card(c, ok ? 0x0004u : 0x0100u);
    c->V0 = 1;
}
static void card_write_b(CpuContext *c)
{
    MemoryCard *cd = (c->A0 & 0x10u) ? g_card_b : g_card_a;
    bool slot_b = (c->A0 & 0x10u) != 0;
    g_card_chan = c->A0;
    uint32_t sector = c->A1 & 0x3FFu;
    bool ok = cd->enabled && sector <= 0x400u;
    if (ok) {
        if (c->A2 != 0u && sector < 0x400u) {
            uint8_t f[0x80];
            for (uint32_t i = 0; i < 0x80u; i++) f[i] = (uint8_t)RD8(c->A2 + i);
            card_frame_write(cd, (int)sector, f);
        }
        if (slot_b) g_card_new_b = false; else g_card_new_a = false;
        g_card_status = 1u;
    } else g_card_status = 0x11u;
    signal_hw_card(c, ok ? 0x0004u : 0x0100u);
    c->V0 = ok ? 1u : 0u;
}

uint32_t bios_get_free_ev_slot(void)
{
    for (int i = 0; i < MAX_EVENTS; i++) if (g_ev[i].status == 0u) return (uint32_t)i;
    return 0xFFFFFFFFu;
}

static void pad_read_into_buf(void)
{
    if (!g_pad_buf) return;
    plat_input_poll();
    uint16_t s = pad_state(), s2 = 0xFFFF;
    uint16_t sw = (uint16_t)((s >> 8) | (s << 8)), sw2 = (uint16_t)((s2 >> 8) | (s2 << 8));
    WR32(g_pad_buf, ((uint32_t)sw2 << 16) | sw);
    WR8(g_pad_buf + 4, g_pad_rx); WR8(g_pad_buf + 5, g_pad_ry);
    WR8(g_pad_buf + 6, g_pad_lx); WR8(g_pad_buf + 7, g_pad_ly);
}
void bios_refresh_pad(void) { pad_read_into_buf(); }

void bios_sys_enq_int_rp(CpuContext *c)
{
    uint32_t pr = c->A0 & 3u, st = c->A1;
    c->V0 = g_int_chain[pr];
    WR32(st, g_int_chain[pr]);
    g_int_chain[pr] = st;
}
void bios_sys_deq_int_rp(CpuContext *c)
{
    uint32_t pr = c->A0 & 3u, st = c->A1;
    if (g_int_chain[pr] == st) { g_int_chain[pr] = RD32(st); c->V0 = 1; return; }
    uint32_t cur = g_int_chain[pr];
    while (cur) {
        uint32_t next = RD32(cur);
        if (next == st) { WR32(cur, RD32(st)); c->V0 = 1; return; }
        cur = next;
    }
    c->V0 = 0;
}

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func)
{
    for (int i = 0; i < MAX_EVENTS; i++)
        if (g_ev[i].status == 0u) { g_ev[i] = (EvCB){ 1u, cls, spec, mode, func }; return 0xF0000000u | (uint32_t)i; }
    return 0xFFFFFFFFu;
}
static int ev_slot(uint32_t ev) { int i = (int)(ev & 0xFFu); return i < MAX_EVENTS ? i : -1; }

static uint32_t card_format_dev(CpuContext *c, uint32_t pathp)
{
    (void)c;
    char p[64]; read_cstr(pathp, p, sizeof p);
    MemoryCard *cd = card_for(p);
    if (!cd) { bios_deliver_event(0xF0000011u, 0x8000u); return 0; }
    card_format(cd);
    bios_deliver_event(0xF0000011u, 0x0004u);
    return 1;
}
static uint32_t first_file(CpuContext *c, uint32_t wildp, uint32_t dirp);
static uint32_t next_file_entry(uint32_t dirp)
{
    if (g_ff_idx >= g_ff_n) return 0;
    CardDirEnt *e = &g_ff[g_ff_idx++];
    int n = (int)strlen(e->name); if (n > 20) n = 20;
    for (int i = 0; i < 20; i++) WR8(dirp + (uint32_t)i, i < n ? (uint8_t)e->name[i] : 0);
    WR32(dirp + 0x14u, n < 20 ? 0x50u : 0u);
    WR32(dirp + 0x18u, (uint32_t)e->size);
    WR32(dirp + 0x1Cu, 0u);
    WR32(dirp + 0x20u, (uint32_t)(e->block * 0x40));
    WR32(dirp + 0x24u, 0u);
    return dirp;
}
static uint32_t first_file(CpuContext *c, uint32_t wildp, uint32_t dirp)
{
    char w[64]; read_cstr(wildp, w, sizeof w);
    MemoryCard *cd = card_for(w);
    if (!cd) return 0;
    g_ff_n = card_match(cd, card_name(w), g_ff, 16); g_ff_idx = 0;
    signal_hw_card(c, 0x0004u);
    return next_file_entry(dirp);
}

/* ------------------------------------------------------------------ */
void bios_init(void) { }

bool bios_try_dispatch(CpuContext *c, uint32_t addr)
{
    switch (addr) {
    case 0xA0u: bios_a(c, c->T1); return true;
    case 0xB0u: bios_b(c, c->T1); return true;
    case 0xC0u: bios_c(c, c->T1); return true;
    }
    return (addr & 0xFF000000u) == 0xBFC00000u;
}

void bios_syscall(CpuContext *c) { (void)c; }
void bios_break(CpuContext *c) { (void)c; }

void bios_c(CpuContext *c, uint32_t fn)
{
    switch (fn) {
    case 0x02: bios_sys_enq_int_rp(c); break;
    case 0x03: bios_sys_deq_int_rp(c); break;
    case 0x04: c->V0 = bios_get_free_ev_slot(); break;
    case 0x05: case 0x11: case 0x18: case 0x19: case 0x1D: c->V0 = 0; break;
    default: break;
    }
}

void bios_b(CpuContext *c, uint32_t fn)
{
    switch (fn) {
    case 0x00: c->V0 = 0; break;
    case 0x02: case 0x03: c->V0 = 0; break;
    case 0x07: bios_deliver_event(c->A0, c->A1); break;
    case 0x08: c->V0 = open_event(c->A0, c->A1, c->A2, c->A3); break;
    case 0x09: { int s = ev_slot(c->A0); if (s >= 0) memset(&g_ev[s], 0, sizeof(EvCB)); c->V0 = 1; break; }
    case 0x0A: {
        int s = ev_slot(c->A0); c->V0 = 0;
        if (s >= 0 && g_ev[s].status == 4u) { g_ev[s].status = 2u; c->V0 = 1; }
        break; }
    case 0x0B: {
        int s = ev_slot(c->A0); c->V0 = 0;
        if (s >= 0 && g_ev[s].status == 4u) { g_ev[s].status = 2u; c->V0 = 1; }
        break; }
    case 0x0C: { int s = ev_slot(c->A0); if (s >= 0) g_ev[s].status = 2u; c->V0 = 1; break; }
    case 0x0D: { int s = ev_slot(c->A0); if (s >= 0 && g_ev[s].status != 0u) g_ev[s].status = 1u; c->V0 = 1; break; }
    case 0x0E: {
        c->V0 = 0xFFFFFFFFu;
        for (int i = 0; i < MAX_THREADS; i++) if (!g_th_used[i]) { g_th_used[i] = true; c->V0 = 0xFF000000u | (uint32_t)i; break; }
        break; }
    case 0x0F: { int i = (int)(c->A0 & 0xFFu); if (i < MAX_THREADS) g_th_used[i] = false; c->V0 = 1; break; }
    case 0x15: g_pad_buf = c->A1; break;
    case 0x16: pad_read_into_buf(); break;
    case 0x18: g_intr_env_addr = 0; break;
    case 0x19:
        g_intr_env_addr = c->A0 ? c->A0 - 0x36u : 0u;
        LOG("[BIOS] SetIntrEnv A0=0x%08X -> env=0x%08X", c->A0, g_intr_env_addr);
        break;
    case 0x20: for (int i = 0; i < MAX_EVENTS; i++) if (g_ev[i].status == 4u && g_ev[i].cls == c->A0 && g_ev[i].spec == c->A1) g_ev[i].status = 2u; break;
    case 0x2F: case 0x30: case 0x31: c->V0 = 0; break;
    case 0x32: bios_a(c, 0x00); break;
    case 0x33: bios_a(c, 0x01); break;
    case 0x34: bios_a(c, 0x02); break;
    case 0x35: bios_a(c, 0x03); break;
    case 0x36: bios_a(c, 0x04); break;
    case 0x37: bios_a(c, 0x05); break;
    case 0x38: bios_a(c, 0x06); break;
    case 0x39: c->V0 = c->A0 <= 2u ? 2u : 0u; break;
    case 0x3A: case 0x3C: c->V0 = 0xFFFFFFFFu; break;
    case 0x3B: case 0x3D: c->V0 = c->A0; break;
    case 0x3E: c->V0 = 0; break;
    case 0x3F: c->V0 = c->A0; break;
    case 0x40: c->V0 = 1; break;
    case 0x41: c->V0 = card_format_dev(c, c->A0); break;
    case 0x42: c->V0 = first_file(c, c->A0, c->A1); break;
    case 0x43: c->V0 = next_file_entry(c->A0); break;
    case 0x44: c->V0 = 0; break;
    case 0x45: {
        char p[64]; read_cstr(c->A0, p, sizeof p);
        MemoryCard *cd = card_for(p);
        if (!cd) { c->V0 = 0; break; }
        card_delete(cd, card_name(p)); c->V0 = 1; break; }
    case 0x46: c->V0 = 0; break;
    case 0x47: c->V0 = bios_get_free_ev_slot(); break;
    case 0x48: c->V0 = 0xFFFFFFFFu; break;
    case 0x4A: case 0x4B: case 0x4C: c->V0 = 1; break;
    case 0x4D: card_info(c); break;
    case 0x4E: card_write_b(c); break;
    case 0x4F: card_read_b(c); break;
    case 0x50: g_card_status = 1u; break;
    case 0x51: c->V0 = krom2_raw_add(c->A0); break;
    case 0x53: c->V0 = krom2_offset(c->A0); break;
    case 0x54: c->V0 = g_errno; break;
    case 0x55: case 0x56: case 0x57: c->V0 = 0; break;
    case 0x58: c->V0 = g_card_chan; break;
    case 0x59: { char p[64]; read_cstr(c->A0, p, sizeof p); c->V0 = card_for(p) ? 1u : 0u; break; }
    case 0x5B: c->V0 = 0; break;
    case 0x5C: c->V0 = g_card_status; break;
    case 0x5D: c->V0 = g_card_status = 1u; break;
    default: break;
    }
}

void bios_a(CpuContext *c, uint32_t fn)
{
    switch (fn) {
    case 0x00: {
        char raw[128]; read_cstr(c->A0, raw, sizeof raw);
        MemoryCard *card = card_for(raw);
        if (card) {
            const char *cn = card_name(raw);
            int first = card_find(card, cn);
            if (first == 0 && (c->A1 & 0x200u)) first = card_create(card, cn, (int)(c->A1 >> 16));
            if (first == 0) { c->V0 = 0xFFFFFFFFu; g_errno = 2; break; }
            int slot = alloc_fd_slot();
            if (slot < 0) { c->V0 = 0xFFFFFFFFu; g_errno = 24; break; }
            CardFile *f = &g_cfiles[slot];
            f->used = true; f->card = card;
            f->nchain = card_chain(card, first, f->chain, 16);
            f->size = card_file_size(card, first); f->pos = 0;
            uint32_t h = (uint32_t)slot + 2u;
            c->V0 = h; g_errno = 0;
            break;
        }
        if (!g_disc) { c->V0 = 0xFFFFFFFFu; g_errno = 13; break; }
        if (!strncasecmp(raw, "sim:", 4)) { c->V0 = 0xFFFFFFFFu; g_errno = 2; break; }
        char fname[64]; extract_file_name(raw, fname, sizeof fname);
        char found[128];
        if (!disc_find_file(g_disc, fname, found, sizeof found)) { c->V0 = 0xFFFFFFFFu; g_errno = 2; break; }
        uint32_t len = 0; uint8_t *data = disc_read_file(g_disc, found, &len);
        int slot = alloc_fd_slot();
        if (!data || slot < 0) { free(data); c->V0 = 0xFFFFFFFFu; g_errno = 16; break; }
        OpenFile *f = &g_files[slot];
        f->used = true; f->data = data; f->len = len; f->off = 0;
        overlay_name(fname, f->name, sizeof f->name);
        c->V0 = (uint32_t)slot + 2u; g_errno = 0;
        break; }
    case 0x01: {
        CardFile *cf = cf_get(c->A0);
        if (cf) {
            int no = c->A2 == 0 ? (int)c->A1 : c->A2 == 1 ? cf->pos + (int)c->A1 : c->A2 == 2 ? cf->size + (int)c->A1 : cf->pos;
            cf->pos = clampi(no, 0, cf->size); c->V0 = (uint32_t)cf->pos; g_errno = 0; break;
        }
        OpenFile *f = of_get(c->A0);
        if (!f) { c->V0 = 0xFFFFFFFFu; g_errno = 9; break; }
        int64_t no = c->A2 == 0 ? (int)c->A1 : c->A2 == 1 ? (int64_t)f->off + (int)c->A1 : c->A2 == 2 ? (int64_t)f->len + (int)c->A1 : f->off;
        if (no < 0) no = 0; if (no > (int64_t)f->len) no = f->len;
        f->off = (uint32_t)no; c->V0 = f->off; g_errno = 0;
        break; }
    case 0x02: {
        CardFile *cf = cf_get(c->A0);
        if (cf) {
            int n = (int)c->A2; if (n > cf->size - cf->pos) n = cf->size - cf->pos;
            for (int i = 0; i < n; i++) WR8(c->A1 + (uint32_t)i, card_read_byte(cf->card, cf->chain, cf->nchain, cf->pos + i));
            cf->pos += n; signal_sw_card(c, 0x0004u); c->V0 = (uint32_t)n; g_errno = 0; break;
        }
        OpenFile *f = of_get(c->A0);
        if (!f) { c->V0 = 0xFFFFFFFFu; g_errno = 9; break; }
        uint32_t cnt = c->A2 < f->len - f->off ? c->A2 : f->len - f->off;
        for (uint32_t i = 0; i < cnt; i++) WR8(c->A1 + i, f->data[f->off + i]);
        f->off += cnt; c->V0 = cnt; g_errno = 0;
        if (f->off >= f->len) disp_try_load(f->name);
        break; }
    case 0x03: {
        CardFile *cf = cf_get(c->A0);
        if (cf) {
            int n = (int)c->A2; if (n > cf->size - cf->pos) n = cf->size - cf->pos;
            for (int i = 0; i < n; i++) card_write_byte(cf->card, cf->chain, cf->nchain, cf->pos + i, (uint8_t)RD8(c->A1 + (uint32_t)i));
            card_flush(cf->card);
            cf->pos += n; signal_sw_card(c, 0x0004u); c->V0 = (uint32_t)n; g_errno = 0; break;
        }
        c->V0 = 0xFFFFFFFFu; g_errno = 9; break; }
    case 0x04: {
        uint32_t h = c->A0;
        CardFile *cf = cf_get(h); if (cf) cf->used = false;
        OpenFile *f = of_get(h); if (f) { free(f->data); f->data = NULL; f->used = false; }
        c->V0 = h; g_errno = 0; break; }
    case 0x05: c->V0 = 0xFFFFFFFFu; break;
    case 0x06: plat_fatal("game called exit()"); break;
    case 0x07: c->V0 = c->A0 <= 2u ? 2u : 0u; break;
    case 0x08: c->V0 = 0xFFFFFFFFu; break;
    case 0x09: case 0x3C: c->V0 = c->A0; break;
    case 0x0A: c->V0 = isdigit((int)(c->A0 & 0xFF)) ? (c->A0 & 0xFFu) - '0' : 0xFFFFFFFFu; break;
    case 0x0B: c->V0 = 0; break;
    case 0x0C: c->V0 = b_strtoul(c->A0, c->A1, c->A2); break;
    case 0x0D: c->V0 = (uint32_t)b_strtol(c->A0, c->A1, c->A2); break;
    case 0x0E: case 0x0F: c->V0 = (uint32_t)((int)c->A0 < 0 ? -(int)c->A0 : (int)c->A0); break;
    case 0x10: case 0x11: c->V0 = (uint32_t)b_atoi(c->A0); break;
    case 0x12: {
        char s[64]; read_cstr(c->A0, s, sizeof s);
        int i = 0, sign = 1, r = 0; bool ok = false;
        while (isspace((unsigned char)s[i])) i++;
        if (s[i] == '-') { sign = -1; i++; }
        while (isdigit((unsigned char)s[i])) { r = r * 10 + (s[i++] - '0'); ok = true; }
        if (!ok) { c->V0 = 0; break; }
        WR32(c->A1, (uint32_t)(sign * r)); c->V0 = 1; break; }
    case 0x13: {
        uint32_t b = c->A0;
        WR32(b + 0x00, c->RA); WR32(b + 0x04, c->SP); WR32(b + 0x08, c->FP); WR32(b + 0x0C, c->GP);
        WR32(b + 0x10, c->S0); WR32(b + 0x14, c->S1); WR32(b + 0x18, c->S2); WR32(b + 0x1C, c->S3);
        WR32(b + 0x20, c->S4); WR32(b + 0x24, c->S5); WR32(b + 0x28, c->S6); WR32(b + 0x2C, c->S7);
        c->V0 = 0; break; }
    case 0x14: {
        uint32_t b = c->A0;
        c->RA = RD32(b + 0x00); c->SP = RD32(b + 0x04); c->FP = RD32(b + 0x08); c->GP = RD32(b + 0x0C);
        c->S0 = RD32(b + 0x10); c->S1 = RD32(b + 0x14); c->S2 = RD32(b + 0x18); c->S3 = RD32(b + 0x1C);
        c->S4 = RD32(b + 0x20); c->S5 = RD32(b + 0x24); c->S6 = RD32(b + 0x28); c->S7 = RD32(b + 0x2C);
        c->V0 = c->A1 ? c->A1 : 1u; break; }
    case 0x15: c->V0 = b_strcat(c->A0, c->A1); break;
    case 0x16: c->V0 = b_strncat(c->A0, c->A1, c->A2); break;
    case 0x17: c->V0 = b_strcmp(c->A0, c->A1); break;
    case 0x18: c->V0 = b_strncmp(c->A0, c->A1, c->A2); break;
    case 0x19: c->V0 = b_strcpy(c->A0, c->A1); break;
    case 0x1A: c->V0 = b_strncpy(c->A0, c->A1, c->A2); break;
    case 0x1B: c->V0 = b_strlen(c->A0); break;
    case 0x1C: case 0x1E: c->V0 = b_strchr(c->A0, c->A1); break;
    case 0x1D: case 0x1F: c->V0 = b_strrchr(c->A0, c->A1); break;
    case 0x20: c->V0 = b_strpbrk(c->A0, c->A1); break;
    case 0x21: c->V0 = b_strspn(c->A0, c->A1); break;
    case 0x22: c->V0 = b_strcspn(c->A0, c->A1); break;
    case 0x23: c->V0 = b_strtok(c->A0, c->A1); break;
    case 0x24: c->V0 = b_strstr(c->A0, c->A1); break;
    case 0x25: c->V0 = (uint8_t)toupper((int)(c->A0 & 0xFF)); break;
    case 0x26: c->V0 = (uint8_t)tolower((int)(c->A0 & 0xFF)); break;
    case 0x27: b_memcpy(c->A1, c->A0, c->A2); c->V0 = c->A1; break;
    case 0x28: b_memset(c->A0, 0, c->A1); break;
    case 0x29: case 0x2D: c->V0 = b_memcmp(c->A0, c->A1, c->A2); break;
    case 0x2A: c->V0 = b_memcpy(c->A0, c->A1, c->A2); break;
    case 0x2B: c->V0 = b_memset(c->A0, (uint8_t)c->A1, c->A2); break;
    case 0x2C: c->V0 = b_memmove(c->A0, c->A1, c->A2); break;
    case 0x2E: c->V0 = b_memchr(c->A0, (uint8_t)c->A1, c->A2); break;
    case 0x2F: c->V0 = b_rand(); break;
    case 0x30: g_rand = c->A0; break;
    case 0x31: {
        uint32_t qb = c->A0, qn = c->A1, qs = c->A2, qc = c->A3;
        for (uint32_t i = 1; i < qn; i++) {
            uint32_t j = i;
            while (j > 0) {
                uint32_t pa = qb + (j - 1) * qs, pb = qb + j * qs;
                c->A0 = pa; c->A1 = pb;
                disp_call(c, qc);
                if ((int)c->V0 <= 0) break;
                for (uint32_t k = 0; k < qs; k++) { uint32_t t = RD8(pa + k); WR8(pa + k, RD8(pb + k)); WR8(pb + k, t); }
                j--;
            }
        }
        break; }
    case 0x32: c->V0 = 0; break;
    case 0x33: c->V0 = h_malloc(c->A0); break;
    case 0x34: h_free(c->A0); break;
    case 0x35: {
        uint32_t key = c->A0, ub = c->A1, nel = c->A2, width = c->A3;
        uint32_t cmp = RD32(c->SP + 0x10u);
        c->V0 = 0;
        for (uint32_t i = 0; i < nel; i++) {
            uint32_t elem = ub + i * width;
            c->A0 = key; c->A1 = elem;
            disp_call(c, cmp);
            if (c->V0 == 0) { c->V0 = elem; break; }
        }
        if (c->V0 == 0) { uint32_t ne = ub + nel * width; b_memcpy(ne, key, width); c->V0 = ne; }
        break; }
    case 0x36: {
        uint32_t key = c->A0, ub = c->A1, nel = c->A2, width = c->A3;
        uint32_t cmp = RD32(c->SP + 0x10u);
        int lo = 0, hi = (int)nel - 1;
        c->V0 = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            uint32_t elem = ub + (uint32_t)mid * width;
            c->A0 = key; c->A1 = elem;
            disp_call(c, cmp);
            int diff = (int)c->V0;
            if (diff == 0) { c->V0 = elem; break; }
            if (diff < 0) hi = mid - 1; else lo = mid + 1;
        }
        break; }
    case 0x37: {
        uint32_t total = c->A0 * c->A1, p = h_malloc(total);
        if (p) b_memset(p, 0, total);
        c->V0 = p; break; }
    case 0x38: c->V0 = h_realloc(c->A0, c->A1); break;
    case 0x39: init_heap(c->A0, c->A1); break;
    case 0x3A: plat_fatal("game called _exit"); break;
    case 0x3B: c->V0 = 0xFFFFFFFFu; break;
    case 0x3D: c->V0 = 0; break;
    case 0x3E: c->V0 = c->A0; break;
    case 0x3F: {
        char fmt[256], out[512]; read_cstr(c->A0, fmt, sizeof fmt);
        format_string(c, fmt, out, sizeof out);
        LOGF(g_log_bios, "[printf] %s", out);
        c->V0 = 0; break; }
    case 0x40: plat_fatal("BIOS A(40h) SystemErrorUnresolvedException"); break;
    case 0x41: case 0x42: case 0x43: case 0x4D: case 0x51: c->V0 = 0; break;
    case 0x78: cd_queue_async_seekl((uint8_t)RD8(c->A0), (uint8_t)RD8(c->A0 + 1), (uint8_t)RD8(c->A0 + 2)); c->V0 = 1; break;
    case 0x7C: cd_queue_async_get_status(); c->V0 = 1; break;
    case 0x7E: cd_queue_async_read_sector(c->A0, c->A1, c->A2); c->V0 = 1; break;
    case 0x81: cd_queue_async_set_mode((uint8_t)c->A0); c->V0 = 1; break;
    case 0x9C: g_conf_ev = c->A0; g_conf_tcb = c->A1; g_conf_stack = c->A2; break;
    case 0x9D:
        if (c->A0) WR32(c->A0, g_conf_ev);
        if (c->A1) WR32(c->A1, g_conf_tcb);
        if (c->A2) WR32(c->A2, g_conf_stack);
        break;
    case 0xA4: c->V0 = 0xFFFFFFFFu; break;
    case 0xA5: {
        uint32_t count = c->A0, lba = c->A1, buf = c->A2;
        uint8_t sec[2048];
        for (uint32_t i = 0; i < count; i++) {
            cd_read_sector_state((int)(lba + i), sec);
            mem_load_bytes(buf + i * 2048u, sec, 2048);
        }
        c->V0 = count; break; }
    case 0xA6: c->V0 = cd_drive_status_byte(); break;
    case 0xAB: card_info(c); break;
    case 0xAC: card_complete(c, c->A0); c->V0 = 1; break;
    case 0xB4: c->V0 = c->A0 == 0 ? 0xFFFFFFFFu : (c->A0 == 1 || c->A0 == 2) ? 1u : 0u; break;
    default: break;
    }
}
