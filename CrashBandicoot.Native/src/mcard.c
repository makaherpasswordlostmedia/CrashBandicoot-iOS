/* mcard.c - MemoryCard ported from RecompOne.Runtime.Hardware.MemoryCard */
#include "psx.h"
#include <unistd.h>

#define CARD_SIZE 0x20000
#define FR  0x80
#define BLK 0x2000
#define DIR 15

MemoryCard *g_card_a, *g_card_b;

static uint8_t sum(const uint8_t *d, int o) { uint8_t x = 0; for (int i = 0; i < 0x7F; i++) x ^= d[o + i]; return x; }
static void fix(MemoryCard *m, int o) { m->d[o + 0x7F] = sum(m->d, o); }

void card_flush(MemoryCard *m)
{
    if (!m->path[0]) return;
    /* write to a temp file and rename so a kill mid-write can't corrupt saves */
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", m->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { LOG("[CARD] cannot write %s", tmp); return; }
    fwrite(m->d, 1, CARD_SIZE, f);
    fflush(f);
    fclose(f);
    rename(tmp, m->path);
}

void card_format(MemoryCard *m)
{
    memset(m->d, 0, CARD_SIZE);
    m->d[0] = 0x4D; m->d[1] = 0x43; fix(m, 0);
    for (int i = 1; i <= DIR; i++) { int o = i * FR; m->d[o] = 0xA0; m->d[o + 8] = 0xFF; m->d[o + 9] = 0xFF; fix(m, o); }
    for (int i = 16; i <= 35; i++) {
        int o = i * FR;
        m->d[o] = m->d[o + 1] = m->d[o + 2] = m->d[o + 3] = 0xFF;
        m->d[o + 8] = 0xFF; m->d[o + 9] = 0xFF; fix(m, o);
    }
    card_flush(m);
}

void card_init(MemoryCard *m, const char *path)
{
    memset(m, 0, sizeof *m);
    snprintf(m->path, sizeof m->path, "%s", path);
    m->enabled = true;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(m->d, 1, CARD_SIZE, f);
        (void)n;
        fclose(f);
    } else card_format(m);
}

static int name_of(MemoryCard *m, int b, char *out)
{
    int o = b * FR + 0x0A, n = 0;
    while (n < 20 && m->d[o + n] != 0) n++;
    memcpy(out, m->d + o, (size_t)n); out[n] = 0;
    return n;
}

int card_file_size(MemoryCard *m, int b) { return (int)ld32(m->d + b * FR + 4); }

int card_find(MemoryCard *m, const char *name)
{
    char nm[24];
    for (int b = 1; b <= DIR; b++) {
        if (m->d[b * FR] != 0x51) continue;
        name_of(m, b, nm);
        if (!strcmp(nm, name)) return b;
    }
    return 0;
}

int card_chain(MemoryCard *m, int first, int *out, int max)
{
    int n = 0, b = first, guard = 0;
    while (b >= 1 && b <= DIR && guard++ < DIR && n < max) {
        out[n++] = b;
        int next = m->d[b * FR + 8] | (m->d[b * FR + 9] << 8);
        if (next == 0xFFFF) break;
        b = next + 1;
    }
    return n;
}

int card_create(MemoryCard *m, const char *name, int blocks)
{
    if (blocks < 1) blocks = 1;
    if (card_find(m, name) != 0) return 0;
    int freeb[DIR], nf = 0;
    for (int b = 1; b <= DIR && nf < blocks; b++) if (m->d[b * FR] == 0xA0) freeb[nf++] = b;
    if (nf < blocks) return 0;
    for (int i = 0; i < blocks; i++) {
        int b = freeb[i], o = b * FR;
        memset(m->d + o, 0, FR);
        m->d[o] = (uint8_t)(i == 0 ? 0x51 : i == blocks - 1 ? 0x53 : 0x52);
        int next = i == blocks - 1 ? 0xFFFF : freeb[i + 1] - 1;
        m->d[o + 8] = (uint8_t)next; m->d[o + 9] = (uint8_t)(next >> 8);
        if (i == 0) {
            int sz = blocks * BLK;
            st32(m->d + o + 4, (uint32_t)sz);
            size_t l = strlen(name); if (l > 20) l = 20;
            memcpy(m->d + o + 0x0A, name, l);
        }
        fix(m, o);
        memset(m->d + b * BLK, 0, BLK);
    }
    card_flush(m);
    return freeb[0];
}

void card_frame_read(MemoryCard *m, int frame, uint8_t *dst)
{
    if ((unsigned)frame < CARD_SIZE / FR) memcpy(dst, m->d + frame * FR, FR);
}

void card_frame_write(MemoryCard *m, int frame, const uint8_t *src)
{
    if ((unsigned)frame >= CARD_SIZE / FR) return;
    memcpy(m->d + frame * FR, src, FR);
    card_flush(m);
}

uint8_t card_read_byte(MemoryCard *m, const int *chain, int n, int pos)
{
    int bi = pos / BLK, off = pos % BLK;
    return bi < n ? m->d[chain[bi] * BLK + off] : 0;
}

void card_write_byte(MemoryCard *m, const int *chain, int n, int pos, uint8_t v)
{
    int bi = pos / BLK, off = pos % BLK;
    if (bi < n) m->d[chain[bi] * BLK + off] = v;
}

void card_delete(MemoryCard *m, const char *name)
{
    int first = card_find(m, name);
    if (!first) return;
    int ch[DIR];
    int n = card_chain(m, first, ch, DIR);
    for (int i = 0; i < n; i++) { m->d[ch[i] * FR] = 0xA0; fix(m, ch[i] * FR); }
    card_flush(m);
}

static bool glob(const char *pat, const char *name)
{
    if (!*pat) return true;
    size_t pi = 0, ni = 0, pl = strlen(pat), nl = strlen(name);
    while (pi < pl) {
        if (pat[pi] == '*') return true;
        if (ni >= nl) return false;
        if (pat[pi] != '?' && pat[pi] != name[ni]) return false;
        pi++; ni++;
    }
    return ni == nl;
}

int card_match(MemoryCard *m, const char *pat, CardDirEnt *out, int max)
{
    int n = 0;
    char nm[24];
    for (int b = 1; b <= DIR && n < max; b++) {
        if (m->d[b * FR] != 0x51) continue;
        name_of(m, b, nm);
        if (glob(pat, nm)) { snprintf(out[n].name, sizeof out[n].name, "%s", nm); out[n].size = card_file_size(m, b); out[n].block = b; n++; }
    }
    return n;
}
