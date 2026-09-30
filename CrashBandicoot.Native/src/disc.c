/* disc.c - CueBin + CueFs (ISO9660) ported from RecompOne.Runtime.Cdrom */
#include "psx.h"
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <libgen.h>
#include <sys/stat.h>
#include <dirent.h>

#define MAX_TRACKS 100

typedef struct {
    int  fd;
    char mode[24];
    int  sector_size, data_offset;
    int64_t file_offset;
} Track;

struct Disc {
    Track tracks[MAX_TRACKS];
    int   ntracks;
    int   data_track;
};

Disc *g_disc;

static int64_t msf_to_sectors(const char *s)
{
    int m = 0, sec = 0, f = 0;
    sscanf(s, "%d:%d:%d", &m, &sec, &f);
    return ((int64_t)m * 60 + sec) * 75 + f;
}

static int sector_size_for(const char *mode)
{
    if (!strcasecmp(mode, "MODE1/2048")) return 2048;
    if (!strcasecmp(mode, "MODE2/2336")) return 2336;
    return 2352;
}
static int data_offset_for(const char *mode)
{
    if (!strcasecmp(mode, "MODE1/2352")) return 16;
    if (!strcasecmp(mode, "MODE2/2352")) return 24;
    if (!strcasecmp(mode, "MODE2/2336")) return 8;
    return 0;
}

static int open_bin(const char *dir, const char *cue_path, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) return fd;
    /* Case-insensitive directory scan (Documents may have been renamed). */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, name)) {
                snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                fd = open(path, O_RDONLY);
                break;
            }
        }
        closedir(d);
        if (fd >= 0) return fd;
    }
    /* Last resort: cue basename + .bin */
    char alt[1024];
    snprintf(alt, sizeof alt, "%s", cue_path);
    char *dot = strrchr(alt, '.');
    if (dot) { strcpy(dot, ".bin"); fd = open(alt, O_RDONLY); }
    return fd;
}

Disc *disc_open(const char *cue_path)
{
    FILE *f = fopen(cue_path, "r");
    if (!f) { LOG("[DISC] cannot open cue: %s", cue_path); return NULL; }
    Disc *d = (Disc *)calloc(1, sizeof *d);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", cue_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");

    int cur_fd = -1, mode_set = 0;
    char mode[24] = "MODE2/2352";
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!strncasecmp(s, "FILE ", 5)) {
            char *a = strchr(s, '"'), *b = strrchr(s, '"');
            if (a && b && b > a) {
                *b = 0;
                cur_fd = open_bin(dir, cue_path, a + 1);
                if (cur_fd < 0) LOG("[DISC] cannot open bin: %s/%s", dir, a + 1);
            }
        } else if (!strncasecmp(s, "TRACK ", 6)) {
            int n; char m[24] = {0};
            if (sscanf(s + 6, "%d %23s", &n, m) == 2) { snprintf(mode, sizeof mode, "%s", m); mode_set = 1; }
        } else if (!strncasecmp(s, "INDEX 01 ", 9) && d->ntracks < MAX_TRACKS) {
            Track *t = &d->tracks[d->ntracks++];
            t->fd = cur_fd;
            snprintf(t->mode, sizeof t->mode, "%s", mode);
            t->sector_size = sector_size_for(mode);
            t->data_offset = data_offset_for(mode);
            t->file_offset = msf_to_sectors(s + 9) * t->sector_size;
        }
    }
    (void)mode_set;
    fclose(f);
    d->data_track = -1;
    for (int i = 0; i < d->ntracks; i++)
        if (strcasecmp(d->tracks[i].mode, "AUDIO") != 0 && d->tracks[i].fd >= 0) { d->data_track = i; break; }
    if (d->data_track < 0) { LOG("[DISC] no data track in %s", cue_path); free(d); return NULL; }
    return d;
}

void disc_close(Disc *d)
{
    if (!d) return;
    for (int i = 0; i < d->ntracks; i++) if (d->tracks[i].fd >= 0) close(d->tracks[i].fd);
    free(d);
}

void disc_read_sector_data(Disc *d, int lba, int size, uint8_t *out)
{
    memset(out, 0, (size_t)size);
    if (!d || lba < 0) return;
    Track *t = &d->tracks[d->data_track];
    int offset = t->sector_size == 2352 ? (size >= 2340 ? 12 : size >= 2329 ? 16 : 24) : t->data_offset;
    int64_t pos = t->file_offset + (int64_t)lba * t->sector_size + offset;
    int want = size < t->sector_size - offset ? size : t->sector_size - offset;
    ssize_t n = pread(t->fd, out, (size_t)want, (off_t)pos);
    (void)n; /* short read leaves zeroes, like CueBin */
}

/* ---------------------------- ISO9660 ---------------------------------- */
typedef struct { int lba; uint32_t size; bool is_dir; char name[64]; } DEnt;

static void parse_entry(const uint8_t *data, int off, DEnt *e)
{
    e->lba = (int32_t)ld32(data + off + 2);
    e->size = ld32(data + off + 10);
    e->is_dir = (data[off + 25] & 0x02) != 0;
    int nl = data[off + 32];
    if (nl > 63) nl = 63;
    memcpy(e->name, data + off + 33, (size_t)nl);
    e->name[nl] = 0;
    char *semi = strchr(e->name, ';');
    if (semi) *semi = 0;
}

static uint8_t *read_extent(Disc *d, int lba, uint32_t size)
{
    uint8_t *r = (uint8_t *)calloc(1, size ? size : 1);
    uint32_t done = 0; int cur = lba;
    uint8_t sec[2048];
    while (done < size) {
        disc_read_sector_data(d, cur++, 2048, sec);
        uint32_t n = size - done < 2048 ? size - done : 2048;
        memcpy(r + done, sec, n);
        done += n;
    }
    return r;
}

static void root_entry(Disc *d, DEnt *e)
{
    uint8_t pvd[2048];
    disc_read_sector_data(d, 16, 2048, pvd);
    parse_entry(pvd, 156, e);
}

typedef struct { DEnt *v; int n, cap; } DList;

static void dlist_entries(Disc *d, const DEnt *dir, DList *out)
{
    out->v = NULL; out->n = out->cap = 0;
    uint8_t *data = read_extent(d, dir->lba, dir->size);
    uint32_t i = 0;
    while (i < dir->size) {
        uint8_t len = data[i];
        if (len == 0) { i = (i / 2048 + 1) * 2048; continue; }
        if (i + 34 > dir->size) break;
        DEnt e;
        parse_entry(data, (int)i, &e);
        bool special = data[i + 32] == 1 && (data[i + 33] == 0 || data[i + 33] == 1); /* "." and ".." */
        if (!special) {
            if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 32; out->v = (DEnt *)realloc(out->v, (size_t)out->cap * sizeof(DEnt)); }
            out->v[out->n++] = e;
        }
        i += len;
    }
    free(data);
}

static void strip_version(char *s) { char *p = strchr(s, ';'); if (p) *p = 0; }

static bool search_entry(Disc *d, const DEnt *dir, const char *upper, DEnt *found, char *path_out, size_t psz, const char *base)
{
    DList l; dlist_entries(d, dir, &l);
    bool ok = false;
    for (int i = 0; i < l.n && !ok; i++) {
        if (l.v[i].is_dir) {
            char sub[512];
            snprintf(sub, sizeof sub, "%s%s%s", base, base[0] ? "/" : "", l.v[i].name);
            ok = search_entry(d, &l.v[i], upper, found, path_out, psz, sub);
        } else if (!strcasecmp(l.v[i].name, upper)) {
            *found = l.v[i];
            if (path_out) snprintf(path_out, psz, "%s%s%s", base, base[0] ? "/" : "", l.v[i].name);
            ok = true;
        }
    }
    free(l.v);
    return ok;
}

static bool locate_entry(Disc *d, const char *name_in, DEnt *out)
{
    char name[512];
    while (*name_in == '/' || *name_in == '\\') name_in++;
    snprintf(name, sizeof name, "%s", name_in);
    /* try exact path */
    {
        char tmp[512]; snprintf(tmp, sizeof tmp, "%s", name);
        DEnt cur; root_entry(d, &cur);
        char *save = NULL; bool ok = true;
        char *parts[32]; int np = 0;
        for (char *p = strtok_r(tmp, "/\\", &save); p && np < 32; p = strtok_r(NULL, "/\\", &save)) parts[np++] = p;
        for (int i = 0; i < np && ok; i++) {
            strip_version(parts[i]);
            bool want_dir = i < np - 1;
            DList l; dlist_entries(d, &cur, &l);
            bool hit = false;
            for (int k = 0; k < l.n; k++)
                if (l.v[k].is_dir == want_dir && !strcasecmp(l.v[k].name, parts[i])) { cur = l.v[k]; hit = true; break; }
            free(l.v);
            ok = hit;
        }
        if (ok && np > 0) { *out = cur; return true; }
    }
    /* fallback: search by basename anywhere on the disc */
    const char *bn = name;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') bn = p + 1;
    char up[64]; snprintf(up, sizeof up, "%s", bn); strip_version(up);
    DEnt root; root_entry(d, &root);
    return search_entry(d, &root, up, out, NULL, 0, "");
}

bool disc_locate(Disc *d, const char *name, int *lba, uint32_t *size)
{
    DEnt e;
    if (!locate_entry(d, name, &e)) return false;
    *lba = e.lba; *size = e.size;
    return true;
}

uint8_t *disc_read_file(Disc *d, const char *path, uint32_t *size)
{
    DEnt e;
    if (!locate_entry(d, path, &e)) return NULL;
    *size = e.size;
    return read_extent(d, e.lba, e.size);
}

bool disc_find_file(Disc *d, const char *name, char *out, size_t outsz)
{
    char up[128]; snprintf(up, sizeof up, "%s", name);
    for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
    DEnt root, found; root_entry(d, &root);
    return search_entry(d, &root, up, &found, out, outsz, "");
}
