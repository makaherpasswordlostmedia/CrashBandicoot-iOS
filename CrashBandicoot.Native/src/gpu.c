/* gpu.c - Gpu.cs / GpuCommands.cs / GpuRaster.cs ported to C.
 * Pure software rasterizer drawing into a 1024x512 RGB555 VRAM (the
 * "non-HLE" path of the C# runtime). The host only has to upload the
 * current display rectangle each frame, which is what an A5-class GPU can
 * actually keep up with. */
#include "psx.h"

uint16_t *g_vram;
volatile int g_gpu_dirty;

static int draw_left, draw_top, draw_right = VRAM_W - 1, draw_bottom = VRAM_H - 1;
static int draw_ofs_x, draw_ofs_y;
static int tp_x, tp_y, tp_depth, blend_mode;
static bool dither_on, tex_disable;
static int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
static bool set_mask, check_mask;
static int disp_x, disp_y;
static int h_range1 = 0x200, h_range2 = 0xC00, v_range1 = 0x10, v_range2 = 0x100;
static int hres;
static bool hres368, vres480, pal, disp24, interlace, disp_disabled = true;
static int dma_dir;

static uint32_t fifo[16 + 1];
static int fifo_n, need;
static bool polyline;
static uint32_t *poly_buf; static int poly_n, poly_cap;
static bool load_active, read_active;
static int load_x, load_y, load_w, load_h, load_px;
static int read_x, read_y, read_w, read_h, read_px;
static uint32_t gpu_read;
static bool stat_field;

#define LEN_POLYLINE (-1)
#define LEN_IMAGELOAD (-2)

void gpu_init(void)
{
    g_vram = (uint16_t *)calloc((size_t)VRAM_W * VRAM_H, sizeof(uint16_t));
}

bool gpu_pal(void) { return pal; }
void gpu_end_frame(void) { g_gpu_dirty = 1; }

static int cycles_per_pixel(void) { return hres368 ? 7 : (hres == 0 ? 10 : hres == 1 ? 8 : hres == 2 ? 5 : 4); }

void gpu_display_info(int *x, int *y, int *w, int *h, bool *rgb24, bool *enabled)
{
    int ww = ((h_range2 - h_range1) / cycles_per_pixel() + 2) & ~3;
    ww = clampi(ww, 0, VRAM_W);
    int lines = v_range2 - v_range1;
    if (vres480) lines <<= 1;
    lines = clampi(lines, 0, VRAM_H);
    *x = disp_x; *y = disp_y; *w = ww; *h = lines; *rgb24 = disp24; *enabled = !disp_disabled;
}

uint32_t gpu_read_stat(void)
{
    uint32_t s = 0;
    s |= (uint32_t)((tp_x / 64) & 0xF);
    s |= (uint32_t)(((tp_y / 256) & 1) << 4);
    s |= (uint32_t)((blend_mode & 3) << 5);
    s |= (uint32_t)((tp_depth & 3) << 7);
    if (dither_on) s |= 1u << 9;
    s |= 1u << 10;
    if (set_mask) s |= 1u << 11;
    if (check_mask) s |= 1u << 12;
    s |= 1u << 13;
    if (tex_disable) s |= 1u << 15;
    if (hres368) s |= 1u << 16;
    s |= (uint32_t)((hres & 3) << 17);
    if (vres480) s |= 1u << 19;
    if (pal) s |= 1u << 20;
    if (disp24) s |= 1u << 21;
    if (interlace) s |= 1u << 22;
    if (disp_disabled) s |= 1u << 23;
    s |= 1u << 26; s |= 1u << 27; s |= 1u << 28;
    s |= (uint32_t)((dma_dir & 3) << 29);
    switch (dma_dir) { case 1: s |= 1u << 25; break; case 2: s |= 1u << 28; break; case 3: s |= 1u << 27; break; }
    stat_field = !stat_field;
    if (stat_field) s |= 1u << 31;
    return s;
}

static uint16_t read_image_halfword(void)
{
    if (!read_active) return 0;
    int x = (read_x + (read_px % read_w)) & (VRAM_W - 1);
    int y = (read_y + (read_px / read_w)) & (VRAM_H - 1);
    uint16_t v = g_vram[y * VRAM_W + x];
    if (++read_px >= read_w * read_h) read_active = false;
    return v;
}

uint32_t gpu_read_data(void)
{
    if (!read_active) return gpu_read;
    uint16_t lo = read_image_halfword(), hi = read_image_halfword();
    return lo | ((uint32_t)hi << 16);
}

/* ------------------------------------------------------------------ */
static int command_length(uint32_t word)
{
    uint32_t op = word >> 24;
    if (op == 0x02) return 3;
    if (op >= 0x20 && op <= 0x3F) {
        int n = (word & (1u << 27)) ? 4 : 3;
        bool shaded = (word & (1u << 28)) != 0, tex = (word & (1u << 26)) != 0;
        return 1 + n + (shaded ? n - 1 : 0) + (tex ? n : 0);
    }
    if (op >= 0x40 && op <= 0x5F) {
        if (word & (1u << 27)) return LEN_POLYLINE;
        return 1 + 2 + ((word & (1u << 28)) ? 1 : 0);
    }
    if (op >= 0x60 && op <= 0x7F) {
        int sz = (int)((word >> 27) & 3);
        bool tex = (word & (1u << 26)) != 0;
        return 1 + 1 + (tex ? 1 : 0) + (sz == 0 ? 1 : 0);
    }
    if (op >= 0x80 && op <= 0x9F) return 4;
    if (op >= 0xA0 && op <= 0xBF) return LEN_IMAGELOAD;
    if (op >= 0xC0 && op <= 0xDF) return 3;
    return 1;
}

static int sign_ext11(uint32_t v) { return (v & 0x400) ? (int)(v | 0xFFFFF800u) : (int)v; }
static inline int coord_x(uint32_t w) { int x = (int)(w & 0x7FF); return (x & 0x400) ? x - 0x800 : x; }
static inline int coord_y(uint32_t w) { int y = (int)((w >> 16) & 0x7FF); return (y & 0x400) ? y - 0x800 : y; }
static uint16_t to15(int r, int g, int b) { return (uint16_t)(((r >> 3) & 0x1F) | (((g >> 3) & 0x1F) << 5) | (((b >> 3) & 0x1F) << 10)); }

static void set_draw_mode(uint32_t w)
{
    tp_x = (int)(w & 0xF) * 64; tp_y = (int)((w >> 4) & 1) * 256;
    blend_mode = (int)((w >> 5) & 3); tp_depth = (int)((w >> 7) & 3);
    dither_on = (w & (1u << 9)) != 0; tex_disable = (w & (1u << 11)) != 0;
}
static void set_texpage_from_word(uint32_t tp)
{
    tp_x = (int)(tp & 0xF) * 64; tp_y = (int)((tp >> 4) & 1) * 256;
    blend_mode = (int)((tp >> 5) & 3); tp_depth = (int)((tp >> 7) & 3);
    tex_disable = (tp & (1u << 11)) != 0;
}

/* ------------------------------------------------------------------ */
/* Pixel ops                                                            */
/* ------------------------------------------------------------------ */
static const int8_t dither_tab[4][4] = {
    { -4,  0, -3,  1 }, {  2, -2,  3, -1 }, { -3,  1, -4,  0 }, {  3, -1,  2, -2 },
};
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline void plot(int x, int y, int r, int g, int b, bool semi, bool dither, bool mask_bit)
{
    if (x < draw_left || x > draw_right || y < draw_top || y > draw_bottom) return;
    if (x < 0 || x >= VRAM_W || y < 0 || y >= VRAM_H) return;
    uint16_t *dst = &g_vram[y * VRAM_W + x];
    uint16_t bg = *dst;
    if (check_mask && (bg & 0x8000)) return;
    if (dither) { int d = dither_tab[y & 3][x & 3]; r = clamp255(r + d); g = clamp255(g + d); b = clamp255(b + d); }
    int fr = r >> 3, fg = g >> 3, fb = b >> 3;
    if (fr > 31) fr = 31; if (fg > 31) fg = 31; if (fb > 31) fb = 31;
    if (semi) {
        int br = bg & 0x1F, bgn = (bg >> 5) & 0x1F, bbl = (bg >> 10) & 0x1F;
        switch (blend_mode) {
        case 0: fr = (br + fr) >> 1; fg = (bgn + fg) >> 1; fb = (bbl + fb) >> 1; break;
        case 1: fr = br + fr; fg = bgn + fg; fb = bbl + fb; if (fr > 31) fr = 31; if (fg > 31) fg = 31; if (fb > 31) fb = 31; break;
        case 2: fr = br - fr; fg = bgn - fg; fb = bbl - fb; if (fr < 0) fr = 0; if (fg < 0) fg = 0; if (fb < 0) fb = 0; break;
        default: fr = br + (fr >> 2); fg = bgn + (fg >> 2); fb = bbl + (fb >> 2); if (fr > 31) fr = 31; if (fg > 31) fg = 31; if (fb > 31) fb = 31; break;
        }
    }
    uint16_t o = (uint16_t)(fr | (fg << 5) | (fb << 10));
    if (set_mask || mask_bit) o |= 0x8000;
    *dst = o;
}

static inline uint16_t fetch_texel(int u, int v, int clut)
{
    u = (u & ~(tw_mask_x * 8)) | ((tw_off_x & tw_mask_x) * 8);
    v = (v & ~(tw_mask_y * 8)) | ((tw_off_y & tw_mask_y) * 8);
    u &= 0xFF; v &= 0xFF;
    int row = (tp_y + v) & (VRAM_H - 1);
    const uint16_t *rowp = g_vram + row * VRAM_W;
    if (tp_depth >= 2) return rowp[(tp_x + u) & (VRAM_W - 1)];
    int clut_x = (clut & 0x3F) * 16, clut_y = (clut >> 6) & 0x1FF;
    int index;
    if (tp_depth == 0) {
        uint16_t block = rowp[(tp_x + (u >> 2)) & (VRAM_W - 1)];
        index = (block >> ((u & 3) * 4)) & 0xF;
    } else {
        uint16_t block = rowp[(tp_x + (u >> 1)) & (VRAM_W - 1)];
        index = (block >> ((u & 1) * 8)) & 0xFF;
    }
    return g_vram[(clut_y & (VRAM_H - 1)) * VRAM_W + ((clut_x + index) & (VRAM_W - 1))];
}

/* ------------------------------------------------------------------ */
/* Triangles                                                            */
/* ------------------------------------------------------------------ */
typedef struct { int x, y, r, g, b, u, v; } Vert;

static inline bool top_left(const Vert *p0, const Vert *p1)
{
    int dy = p1->y - p0->y, dx = p1->x - p0->x;
    return dy < 0 || (dy == 0 && dx > 0);
}
static inline int imin3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int imax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

/* fixed point plane: value = base + dx*x + dy*y  (16.16) */
typedef struct { int32_t base, dx, dy; } Plane;

static Plane make_plane(int32_t va, int32_t vb, int32_t vc, int32_t w0row, int32_t w1row, int32_t w2row,
                        int sx0, int sx1, int sx2, int sy0, int sy1, int sy2, int32_t area)
{
    Plane p;
    p.base = (int32_t)((((int64_t)w0row * va + (int64_t)w1row * vb + (int64_t)w2row * vc) * 65536) / area);
    p.dx   = (int32_t)((((int64_t)sx0 * va + (int64_t)sx1 * vb + (int64_t)sx2 * vc) * 65536) / area);
    p.dy   = (int32_t)((((int64_t)sy0 * va + (int64_t)sy1 * vb + (int64_t)sy2 * vc) * 65536) / area);
    return p;
}

static void raster_triangle(Vert a, Vert b, Vert c, bool tex, bool gouraud, bool semi, bool raw, int clut)
{
    int span_x = imax3(a.x, b.x, c.x) - imin3(a.x, b.x, c.x);
    int span_y = imax3(a.y, b.y, c.y) - imin3(a.y, b.y, c.y);
    if (span_x > 1023 || span_y > 511) return;
    int32_t area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) { Vert t = b; b = c; c = t; area = -area; }
    int min_x = imax3(draw_left, imin3(a.x, b.x, c.x), draw_left);
    int max_x = imin3(draw_right, imax3(a.x, b.x, c.x), draw_right);
    int min_y = imax3(draw_top, imin3(a.y, b.y, c.y), draw_top);
    int max_y = imin3(draw_bottom, imax3(a.y, b.y, c.y), draw_bottom);
    if (min_x < 0) min_x = 0; if (min_y < 0) min_y = 0;
    if (max_x >= VRAM_W) max_x = VRAM_W - 1; if (max_y >= VRAM_H) max_y = VRAM_H - 1;
    if (min_x > max_x || min_y > max_y) return;

    int bias0 = top_left(&b, &c) ? 0 : -1;
    int bias1 = top_left(&c, &a) ? 0 : -1;
    int bias2 = top_left(&a, &b) ? 0 : -1;
    bool dither_tex = dither_on && !raw;
    int sx0 = b.y - c.y, sy0 = c.x - b.x;
    int sx1 = c.y - a.y, sy1 = a.x - c.x;
    int sx2 = a.y - b.y, sy2 = b.x - a.x;
    int32_t w0row = (c.x - b.x) * (min_y - b.y) - (c.y - b.y) * (min_x - b.x);
    int32_t w1row = (a.x - c.x) * (min_y - c.y) - (a.y - c.y) * (min_x - c.x);
    int32_t w2row = (b.x - a.x) * (min_y - a.y) - (b.y - a.y) * (min_x - a.x);

    Plane pr, pg, pb, pu, pv;
    memset(&pr, 0, sizeof pr); pg = pb = pu = pv = pr;
    if (gouraud) {
        pr = make_plane(a.r, b.r, c.r, w0row, w1row, w2row, sx0, sx1, sx2, sy0, sy1, sy2, area);
        pg = make_plane(a.g, b.g, c.g, w0row, w1row, w2row, sx0, sx1, sx2, sy0, sy1, sy2, area);
        pb = make_plane(a.b, b.b, c.b, w0row, w1row, w2row, sx0, sx1, sx2, sy0, sy1, sy2, area);
    }
    if (tex) {
        pu = make_plane(a.u, b.u, c.u, w0row, w1row, w2row, sx0, sx1, sx2, sy0, sy1, sy2, area);
        pv = make_plane(a.v, b.v, c.v, w0row, w1row, w2row, sx0, sx1, sx2, sy0, sy1, sy2, area);
    }

    for (int y = min_y; y <= max_y; y++, w0row += sy0, w1row += sy1, w2row += sy2) {
        int32_t w0 = w0row, w1 = w1row, w2 = w2row;
        int32_t fr = pr.base, fg = pg.base, fb = pb.base, fu = pu.base, fv = pv.base;
        for (int x = min_x; x <= max_x; x++, w0 += sx0, w1 += sx1, w2 += sx2,
             fr += pr.dx, fg += pg.dx, fb += pb.dx, fu += pu.dx, fv += pv.dx) {
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
            int r, g, bl;
            if (gouraud) { r = fr >> 16; g = fg >> 16; bl = fb >> 16; }
            else { r = a.r; g = a.g; bl = a.b; }
            if (tex) {
                uint16_t texel = fetch_texel(fu >> 16, fv >> 16, clut);
                if (texel == 0) continue;
                bool stp = (texel & 0x8000) != 0;
                int tr = (texel & 0x1F) << 3, tg = ((texel >> 5) & 0x1F) << 3, tb = ((texel >> 10) & 0x1F) << 3;
                if (!raw) { tr = tr * r >> 7; tg = tg * g >> 7; tb = tb * bl >> 7; }
                plot(x, y, tr, tg, tb, semi && stp, dither_tex, stp);
            } else {
                plot(x, y, r, g, bl, semi, dither_on && gouraud, false);
            }
        }
        pr.base += pr.dy; pg.base += pg.dy; pb.base += pb.dy; pu.base += pu.dy; pv.base += pv.dy;
    }
}

static void draw_polygon(void)
{
    uint32_t cmd = fifo[0];
    bool gouraud = (cmd & (1u << 28)) != 0, quad = (cmd & (1u << 27)) != 0;
    bool tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
    int n = quad ? 4 : 3, idx = 1, clut = 0;
    Vert v[4];
    int cr = (int)(cmd & 0xFF), cg = (int)((cmd >> 8) & 0xFF), cb = (int)((cmd >> 16) & 0xFF);
    for (int i = 0; i < n; i++) {
        if (gouraud && i > 0) {
            uint32_t cw = fifo[idx++];
            cr = (int)(cw & 0xFF); cg = (int)((cw >> 8) & 0xFF); cb = (int)((cw >> 16) & 0xFF);
        }
        v[i].r = cr; v[i].g = cg; v[i].b = cb;
        uint32_t vw = fifo[idx++];
        v[i].x = draw_ofs_x + coord_x(vw);
        v[i].y = draw_ofs_y + coord_y(vw);
        v[i].u = v[i].v = 0;
        if (tex) {
            uint32_t uvw = fifo[idx++];
            v[i].u = (int)(uvw & 0xFF); v[i].v = (int)((uvw >> 8) & 0xFF);
            if (i == 0) clut = (int)((uvw >> 16) & 0xFFFF);
            else if (i == 1) set_texpage_from_word((uvw >> 16) & 0xFFFF);
        }
    }
    raster_triangle(v[0], v[1], v[2], tex, gouraud, semi, raw, clut);
    if (quad) raster_triangle(v[1], v[2], v[3], tex, gouraud, semi, raw, clut);
}

static void draw_rectangle(void)
{
    uint32_t cmd = fifo[0];
    int sz = (int)((cmd >> 27) & 3);
    bool tex = (cmd & (1u << 26)) != 0, semi = (cmd & (1u << 25)) != 0, raw = (cmd & (1u << 24)) != 0;
    int cr = (int)(cmd & 0xFF), cg = (int)((cmd >> 8) & 0xFF), cb = (int)((cmd >> 16) & 0xFF);
    int idx = 1;
    uint32_t vw = fifo[idx++];
    int x = draw_ofs_x + coord_x(vw), y = draw_ofs_y + coord_y(vw);
    int u0 = 0, v0 = 0, clut = 0;
    if (tex) { uint32_t uvw = fifo[idx++]; u0 = (int)(uvw & 0xFF); v0 = (int)((uvw >> 8) & 0xFF); clut = (int)((uvw >> 16) & 0xFFFF); }
    int w, h;
    if (sz == 0) { uint32_t wh = fifo[idx]; w = (int)(wh & 0xFFFF); h = (int)((wh >> 16) & 0xFFFF); }
    else w = h = sz == 1 ? 1 : sz == 2 ? 8 : 16;
    if (w > 1024) w = 1024; if (h > 512) h = 512;
    int x0 = x < draw_left ? draw_left : x, x1 = x + w - 1 > draw_right ? draw_right : x + w - 1;
    int y0 = y < draw_top ? draw_top : y, y1 = y + h - 1 > draw_bottom ? draw_bottom : y + h - 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= VRAM_W) x1 = VRAM_W - 1; if (y1 >= VRAM_H) y1 = VRAM_H - 1;
    if (x0 > x1 || y0 > y1) return;
    if (!tex && !semi && !check_mask) {
        uint16_t col = to15(cr, cg, cb); if (set_mask) col |= 0x8000;
        for (int py = y0; py <= y1; py++) {
            uint16_t *row = g_vram + py * VRAM_W;
            for (int px = x0; px <= x1; px++) row[px] = col;
        }
        return;
    }
    for (int py = y0; py <= y1; py++)
        for (int px = x0; px <= x1; px++) {
            if (tex) {
                uint16_t texel = fetch_texel((u0 + (px - x)) & 0xFF, (v0 + (py - y)) & 0xFF, clut);
                if (texel == 0) continue;
                bool stp = (texel & 0x8000) != 0;
                int tr = (texel & 0x1F) << 3, tg = ((texel >> 5) & 0x1F) << 3, tb = ((texel >> 10) & 0x1F) << 3;
                if (!raw) { tr = tr * cr >> 7; tg = tg * cg >> 7; tb = tb * cb >> 7; }
                plot(px, py, tr, tg, tb, semi && stp, false, stp);
            } else plot(px, py, cr, cg, cb, semi, false, false);
        }
}

static void line_segment(int x0, int y0, int r0, int g0, int b0, int x1, int y1, int r1, int g1, int b1, bool semi, bool gouraud)
{
    (void)gouraud;
    x0 += draw_ofs_x; y0 += draw_ofs_y; x1 += draw_ofs_x; y1 += draw_ofs_y;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int steps = dx > dy ? dx : dy;
    if (steps == 0) { plot(x0, y0, r0, g0, b0, semi, dither_on, false); return; }
    for (int i = 0; i <= steps; i++) {
        int x = x0 + (int)(((int64_t)(x1 - x0) * i + (x1 >= x0 ? steps / 2 : -steps / 2)) / steps);
        int y = y0 + (int)(((int64_t)(y1 - y0) * i + (y1 >= y0 ? steps / 2 : -steps / 2)) / steps);
        int r = r0 + (int)((int64_t)(r1 - r0) * i / steps);
        int g = g0 + (int)((int64_t)(g1 - g0) * i / steps);
        int b = b0 + (int)((int64_t)(b1 - b0) * i / steps);
        plot(x, y, r, g, b, semi, dither_on, false);
    }
}

static void draw_line(void)
{
    uint32_t cmd = fifo[0];
    bool gouraud = (cmd & (1u << 28)) != 0, semi = (cmd & (1u << 25)) != 0;
    int idx = 1;
    int r0 = (int)(cmd & 0xFF), g0 = (int)((cmd >> 8) & 0xFF), b0 = (int)((cmd >> 16) & 0xFF);
    uint32_t v0w = fifo[idx++];
    int r1 = r0, g1 = g0, b1 = b0;
    if (gouraud) { uint32_t cw = fifo[idx++]; r1 = (int)(cw & 0xFF); g1 = (int)((cw >> 8) & 0xFF); b1 = (int)((cw >> 16) & 0xFF); }
    uint32_t v1w = fifo[idx++];
    line_segment(coord_x(v0w), coord_y(v0w), r0, g0, b0, coord_x(v1w), coord_y(v1w), r1, g1, b1, semi, gouraud);
}

static void execute_polyline(void)
{
    uint32_t cmd = poly_buf[0];
    bool gouraud = (cmd & (1u << 28)) != 0, semi = (cmd & (1u << 25)) != 0;
    int r = (int)(cmd & 0xFF), g = (int)((cmd >> 8) & 0xFF), b = (int)((cmd >> 16) & 0xFF);
    int idx = 1; bool first = true;
    int px = 0, py = 0, pr = r, pg = g, pb = b; bool have_prev = false;
    while (idx < poly_n) {
        if (gouraud && !first) { uint32_t cw = poly_buf[idx++]; r = (int)(cw & 0xFF); g = (int)((cw >> 8) & 0xFF); b = (int)((cw >> 16) & 0xFF); }
        if (idx >= poly_n) break;
        uint32_t vw = poly_buf[idx++];
        int x = coord_x(vw), y = coord_y(vw);
        if (have_prev) line_segment(px, py, pr, pg, pb, x, y, r, g, b, semi, gouraud);
        px = x; py = y; pr = r; pg = g; pb = b; have_prev = true; first = false;
    }
}

/* ------------------------------------------------------------------ */
/* Transfers                                                            */
/* ------------------------------------------------------------------ */
static void fill_rect(void)
{
    uint16_t color = to15((int)(fifo[0] & 0xFF), (int)((fifo[0] >> 8) & 0xFF), (int)((fifo[0] >> 16) & 0xFF));
    int x = (int)(fifo[1] & 0x3F0), y = (int)((fifo[1] >> 16) & 0x1FF);
    int w = (int)(((fifo[2] & 0x3FF) + 0xF) & ~0xF), h = (int)((fifo[2] >> 16) & 0x1FF);
    for (int dy = 0; dy < h; dy++) {
        uint16_t *row = g_vram + ((y + dy) & (VRAM_H - 1)) * VRAM_W;
        for (int dx = 0; dx < w; dx++) row[(x + dx) & (VRAM_W - 1)] = color;
    }
}

static void copy_vram(void)
{
    int sx = (int)(fifo[1] & 0x3FF), sy = (int)((fifo[1] >> 16) & 0x1FF);
    int dx = (int)(fifo[2] & 0x3FF), dy = (int)((fifo[2] >> 16) & 0x1FF);
    int w = (int)(fifo[3] & 0x3FF); if (!w) w = 0x400;
    int h = (int)((fifo[3] >> 16) & 0x1FF); if (!h) h = 0x200;
    /* Overlapping copies go through a temp row buffer like real hardware's line buffer */
    uint16_t line[VRAM_W];
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) line[col] = g_vram[((sy + row) & (VRAM_H - 1)) * VRAM_W + ((sx + col) & (VRAM_W - 1))];
        for (int col = 0; col < w; col++) {
            int d = ((dy + row) & (VRAM_H - 1)) * VRAM_W + ((dx + col) & (VRAM_W - 1));
            uint16_t px = line[col];
            if (check_mask && (g_vram[d] & 0x8000)) continue;
            if (set_mask) px |= 0x8000;
            g_vram[d] = px;
        }
    }
}

static void begin_image_load(void)
{
    load_x = (int)(fifo[1] & 0x3FF); load_y = (int)((fifo[1] >> 16) & 0x1FF);
    load_w = (int)(fifo[2] & 0xFFFF); if (!load_w) load_w = 0x400; else load_w &= 0x3FF; if (!load_w) load_w = 0x400;
    load_h = (int)((fifo[2] >> 16) & 0xFFFF); if (!load_h) load_h = 0x200; else load_h &= 0x1FF; if (!load_h) load_h = 0x200;
    load_px = 0; load_active = true;
    fifo_n = 0;
}

static void store_image_halfword(uint16_t value)
{
    if (!load_active) return;
    uint16_t stored = set_mask ? (uint16_t)(value | 0x8000) : value;
    int x = (load_x + (load_px % load_w)) & (VRAM_W - 1);
    int y = (load_y + (load_px / load_w)) & (VRAM_H - 1);
    int idx = y * VRAM_W + x;
    if (!(check_mask && (g_vram[idx] & 0x8000))) g_vram[idx] = stored;
    if (++load_px >= load_w * load_h) load_active = false;
}

static void begin_image_read(void)
{
    read_x = (int)(fifo[1] & 0x3FF); read_y = (int)((fifo[1] >> 16) & 0x1FF);
    read_w = (int)(fifo[2] & 0x3FF); if (!read_w) read_w = 0x400;
    read_h = (int)((fifo[2] >> 16) & 0x1FF); if (!read_h) read_h = 0x200;
    read_px = 0; read_active = true;
}

static void execute(void)
{
    uint32_t word = fifo[0], op = word >> 24;
    if (op == 0x02) fill_rect();
    else if (op >= 0x20 && op <= 0x3F) draw_polygon();
    else if (op >= 0x40 && op <= 0x5F) draw_line();
    else if (op >= 0x60 && op <= 0x7F) draw_rectangle();
    else if (op >= 0x80 && op <= 0x9F) copy_vram();
    else if (op >= 0xA0 && op <= 0xBF) begin_image_load();
    else if (op >= 0xC0 && op <= 0xDF) begin_image_read();
    else switch (op) {
    case 0xE1: set_draw_mode(word); break;
    case 0xE2: tw_mask_x = (int)(word & 0x1F); tw_mask_y = (int)((word >> 5) & 0x1F);
               tw_off_x = (int)((word >> 10) & 0x1F); tw_off_y = (int)((word >> 15) & 0x1F); break;
    case 0xE3: draw_left = (int)(word & 0x3FF); draw_top = (int)((word >> 10) & 0x3FF); break;
    case 0xE4: draw_right = (int)(word & 0x3FF); draw_bottom = (int)((word >> 10) & 0x3FF); break;
    case 0xE5: draw_ofs_x = sign_ext11(word & 0x7FF); draw_ofs_y = sign_ext11((word >> 11) & 0x7FF); break;
    case 0xE6: set_mask = (word & 1) != 0; check_mask = (word & 2) != 0; break;
    }
}

void gpu_write_gp0(uint32_t word)
{
    if (load_active) { store_image_halfword((uint16_t)word); store_image_halfword((uint16_t)(word >> 16)); return; }
    if (polyline) {
        if ((word & 0xF000F000u) == 0x50005000u) { polyline = false; execute_polyline(); fifo_n = 0; poly_n = 0; }
        else {
            if (poly_n == poly_cap) { poly_cap = poly_cap ? poly_cap * 2 : 256; poly_buf = (uint32_t *)realloc(poly_buf, (size_t)poly_cap * 4); }
            poly_buf[poly_n++] = word;
        }
        return;
    }
    if (fifo_n < 16) fifo[fifo_n++] = word;
    if (fifo_n == 1) {
        need = command_length(word);
        if (need == LEN_POLYLINE) {
            polyline = true; poly_n = 0;
            if (!poly_cap) { poly_cap = 256; poly_buf = (uint32_t *)malloc((size_t)poly_cap * 4); }
            poly_buf[poly_n++] = word;
            fifo_n = 0;
            return;
        }
        if (need == LEN_IMAGELOAD) need = 3;
    }
    if (fifo_n >= need) { execute(); if (!load_active) fifo_n = 0; }
}

static void gp1_reset(void)
{
    fifo_n = 0; polyline = load_active = read_active = false;
    disp_disabled = true; dma_dir = 0;
    tp_x = tp_y = tp_depth = blend_mode = 0; dither_on = tex_disable = false;
    tw_mask_x = tw_mask_y = tw_off_x = tw_off_y = 0;
    draw_left = draw_top = 0; draw_right = VRAM_W - 1; draw_bottom = VRAM_H - 1;
    draw_ofs_x = draw_ofs_y = 0; set_mask = check_mask = false; disp_x = disp_y = 0;
}

void gpu_write_gp1(uint32_t word)
{
    uint32_t op = (word >> 24) & 0xFF, p = word & 0xFFFFFF;
    switch (op) {
    case 0x05: disp_x = (int)(p & 0x3FF); disp_y = (int)((p >> 10) & 0x1FF); break;
    case 0x06: h_range1 = (int)(p & 0xFFF); h_range2 = (int)((p >> 12) & 0xFFF); break;
    case 0x07: v_range1 = (int)(p & 0x3FF); v_range2 = (int)((p >> 10) & 0x3FF); break;
    case 0x08:
        hres = (int)(p & 3); hres368 = (p & 0x40) != 0; vres480 = (p & 4) != 0;
        pal = (p & 8) != 0; disp24 = (p & 0x10) != 0; interlace = (p & 0x20) != 0;
        break;
    case 0x00: gp1_reset(); break;
    case 0x01: fifo_n = 0; polyline = false; load_active = false; break;
    case 0x03: disp_disabled = (p & 1) != 0; break;
    case 0x04: dma_dir = (int)(p & 3); break;
    case 0x10:
        switch (p & 0xFF) {
        case 0x03: gpu_read = (uint32_t)(draw_left | (draw_top << 10)); break;
        case 0x04: gpu_read = (uint32_t)(draw_right | (draw_bottom << 10)); break;
        case 0x05: gpu_read = (uint32_t)((draw_ofs_x & 0x7FF) | ((draw_ofs_y & 0x7FF) << 11)); break;
        default: gpu_read = 0; break;
        }
        break;
    }
}
