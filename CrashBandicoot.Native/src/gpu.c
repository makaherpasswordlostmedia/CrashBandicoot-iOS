/* gpu.c - GLES2 hardware renderer (replaces the software rasterizer).
 * GP0/GP1 parsing is unchanged; drawing goes to a 1024x512 RGBA8 VRAM
 * texture through batched GLES 2.0 draw calls. Target: A5 / iOS 9.3.
 * The GL context is owned by the host (ios_host.m) and used only from
 * the emulation thread. */
#include "psx.h"
#include <math.h>
#ifdef __APPLE__
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>
#else
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

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
    /* No CPU-side VRAM any more; keep a non-NULL placeholder for runtime.c. */
    g_vram = (uint16_t *)calloc(2, sizeof(uint16_t));
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


static inline int imin3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int imax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

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


/* ================================================================== */
/* GLES2 backend                                                        */
/* VRAM lives in an RGBA8 texture (fb_tex, attached to fb_fbo):         */
/*   rgb = 5-bit level / 31, a = PS1 mask bit (bit 15).                 */
/* tx_tex is a lazily synced copy used as the sampling source so we     */
/* never sample the texture we are rendering into (undefined in ES2).   */
/* ================================================================== */

/* host services (ios_host.m) */
void plat_gl_make_current(void);
void plat_gl_screen(int *fbo, int *w, int *h);

static bool gl_ready;
static GLuint fb_tex, tx_tex, p24_tex, fb_fbo, ds_rb;
static GLuint p_prim, p_copy, p_pres;
static bool g_mask_used;

static struct { GLint page, clut, win, misc, vram; } U;      /* prim  */
static struct { GLint vram, misc; } UC;                       /* copy  */
static GLint UP_tex;                                          /* present */

static uint32_t *rgba_buf;        /* 1024*512 RGBA8 staging */
static uint16_t *read_buf;        /* image-read staging (halfwords) */
static uint16_t *pres_buf;        /* 24bpp present staging (separate from read_buf) */
static uint16_t *load_buf;        /* image-load staging (halfwords) */
static uint8_t lut5[32];

typedef struct { float x, y; uint8_t r, g, b, a; float u, v; } GVert;
#define MAX_VERTS 6144
static GVert vbuf[MAX_VERTS];
static int nv;

typedef struct {
    int mode;                       /* 0 = triangles, 1 = lines */
    int tex, raw, semi, blend, dither, setm, chkm;
    int depth, tpx, tpy, clx, cly;
    int wax, way, wox, woy;         /* texture window: and / or */
    int sl, st, sr, sb;             /* scissor (draw area), inclusive */
} Key;
static Key cur;
static bool have_cur;
static int bx0, by0, bx1, by1;      /* batch bbox, exclusive max */
static int dx0, dy0, dx1, dy1;      /* dirty rect (fb newer than tx), exclusive max */
static bool dirty;

static const char *VS_SRC =
    "attribute vec2 aPos; attribute vec4 aCol; attribute vec2 aUV;\n"
    "varying vec4 vCol; varying vec2 vUV;\n"
    "void main(){ vCol = aCol; vUV = aUV;\n"
    "  gl_Position = vec4(aPos.x / 512.0 - 1.0, aPos.y / 256.0 - 1.0, 0.0, 1.0); }\n";

static const char *FS_PRIM =
    "precision highp float;\n"
    "varying vec4 vCol; varying vec2 vUV;\n"
    "uniform sampler2D uVram;\n"
    "uniform vec4 uPage;  /* x,y = page origin, z = depth, w = textured */\n"
    "uniform vec4 uClut;  /* x,y = clut, z = raw, w = dither */\n"
    "uniform vec4 uWin;   /* and.x and.y or.x or.y */\n"
    "uniform vec4 uMisc;  /* x = window on, y = set mask, z = stp mode */\n"
    "const vec2 VSZ = vec2(1024.0, 512.0);\n"
    "vec4 fetch(vec2 p){ p = mod(p, VSZ); return texture2D(uVram, (p + 0.5) / VSZ); }\n"
    "float t16(vec4 c){ return floor(c.r*31.0+0.5) + floor(c.g*31.0+0.5)*32.0\n"
    "  + floor(c.b*31.0+0.5)*1024.0 + step(0.5, c.a)*32768.0; }\n"
    "float band(float a, float b){ float r = 0.0, p = 1.0;\n"
    "  for (int i = 0; i < 8; i++) { r += mod(floor(a/p),2.0) * mod(floor(b/p),2.0) * p; p *= 2.0; }\n"
    "  return r; }\n"
    "vec4 samp(vec2 uv){\n"
    "  float d = uPage.z;\n"
    "  if (d > 1.5) return fetch(uPage.xy + uv);\n"
    "  if (d < 0.5) {\n"
    "    float s = t16(fetch(uPage.xy + vec2(floor(uv.x*0.25), uv.y)));\n"
    "    float m = mod(uv.x, 4.0);\n"
    "    float dv = m < 0.5 ? 1.0 : (m < 1.5 ? 16.0 : (m < 2.5 ? 256.0 : 4096.0));\n"
    "    return fetch(uClut.xy + vec2(mod(floor(s/dv), 16.0), 0.0));\n"
    "  }\n"
    "  float s = t16(fetch(uPage.xy + vec2(floor(uv.x*0.5), uv.y)));\n"
    "  float dv = mod(uv.x, 2.0) < 0.5 ? 1.0 : 256.0;\n"
    "  return fetch(uClut.xy + vec2(mod(floor(s/dv), 256.0), 0.0));\n"
    "}\n"
    "float dith(vec2 fc){\n"
    "  vec2 p = mod(floor(fc), 4.0);\n"
    "  vec4 r = p.y < 0.5 ? vec4(-4.0,0.0,-3.0,1.0) : (p.y < 1.5 ? vec4(2.0,-2.0,3.0,-1.0)\n"
    "         : (p.y < 2.5 ? vec4(-3.0,1.0,-4.0,0.0) : vec4(3.0,-1.0,2.0,-2.0)));\n"
    "  vec4 s = vec4(p.x < 0.5 ? 1.0 : 0.0, (p.x > 0.5 && p.x < 1.5) ? 1.0 : 0.0,\n"
    "                (p.x > 1.5 && p.x < 2.5) ? 1.0 : 0.0, p.x > 2.5 ? 1.0 : 0.0);\n"
    "  return dot(r, s); }\n"
    "void main(){\n"
    "  vec3 col8 = floor(vCol.rgb * 255.0 + 0.5);\n"
    "  vec3 c8; float a = uMisc.y;\n"
    "  if (uPage.w > 0.5) {\n"
    "    vec2 uv = floor(vUV + 0.002);\n"
    "    if (uMisc.x > 0.5) uv = vec2(band(mod(uv.x,256.0), uWin.x) + uWin.z,\n"
    "                                 band(mod(uv.y,256.0), uWin.y) + uWin.w);\n"
    "    uv = mod(uv, 256.0);\n"
    "    vec4 t = samp(uv);\n"
    "    float stp = step(0.5, t.a);\n"
    "    if (t.r + t.g + t.b < 0.001 && stp < 0.5) discard;\n"
    "    if (uMisc.z > 0.5 && uMisc.z < 1.5 && stp > 0.5) discard;\n"
    "    if (uMisc.z > 1.5 && stp < 0.5) discard;\n"
    "    vec3 t8 = floor(t.rgb * 31.0 + 0.5) * 8.0;\n"
    "    c8 = uClut.z > 0.5 ? t8 : floor(t8 * col8 / 128.0);\n"
    "    a = max(stp, uMisc.y);\n"
    "  } else c8 = col8;\n"
    "  if (uClut.w > 0.5) c8 += dith(gl_FragCoord.xy);\n"
    "  c8 = clamp(c8, 0.0, 255.0);\n"
    "  gl_FragColor = vec4(min(floor(c8 / 8.0), 31.0) / 31.0, a);\n"
    "}\n";

static const char *FS_COPY =
    "precision highp float;\n"
    "varying vec2 vUV; uniform sampler2D uVram; uniform vec4 uMisc; /* x = mask only, y = set mask */\n"
    "void main(){ vec4 t = texture2D(uVram, vUV / vec2(1024.0, 512.0));\n"
    "  if (uMisc.x > 0.5) { if (t.a < 0.5) discard; gl_FragColor = vec4(0.0); return; }\n"
    "  gl_FragColor = vec4(t.rgb, max(t.a, uMisc.y)); }\n";

static const char *VS_PRES =
    "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
    "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *FS_PRES =
    "precision highp float; varying vec2 vUV; uniform sampler2D uTex;\n"
    "void main(){ gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0); }\n";

static GLuint sh_compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); plat_log("[GPU] shader error: %s", log); }
    return s;
}

static GLuint prog_build(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, sh_compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, sh_compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aCol");
    glBindAttribLocation(p, 2, "aUV");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(p, sizeof log, NULL, log); plat_log("[GPU] link error: %s", log); }
    return p;
}

static GLuint make_tex(GLenum fmt, GLenum type, GLint filter)
{
    GLuint t; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, VRAM_W, VRAM_H, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void ensure_gl(void)
{
    if (gl_ready) return;
    plat_gl_make_current();

    fb_tex  = make_tex(GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);   /* render target, sampled by present */
    tx_tex  = make_tex(GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);  /* sampling copy */
    p24_tex = make_tex(GL_RGB, GL_UNSIGNED_SHORT_5_6_5, GL_LINEAR);

    glGenFramebuffers(1, &fb_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex, 0);
    glGenRenderbuffers(1, &ds_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, ds_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, VRAM_W, VRAM_H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ds_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds_rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) plat_log("[GPU] VRAM FBO incomplete: 0x%x", st);

    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glClearColor(0, 0, 0, 0); glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    p_prim = prog_build(VS_SRC, FS_PRIM);
    p_copy = prog_build(VS_SRC, FS_COPY);
    p_pres = prog_build(VS_PRES, FS_PRES);
    U.vram = glGetUniformLocation(p_prim, "uVram");
    U.page = glGetUniformLocation(p_prim, "uPage");
    U.clut = glGetUniformLocation(p_prim, "uClut");
    U.win  = glGetUniformLocation(p_prim, "uWin");
    U.misc = glGetUniformLocation(p_prim, "uMisc");
    UC.vram = glGetUniformLocation(p_copy, "uVram");
    UC.misc = glGetUniformLocation(p_copy, "uMisc");
    UP_tex = glGetUniformLocation(p_pres, "uTex");

    for (int i = 0; i < 32; i++) lut5[i] = (uint8_t)((i * 255 + 15) / 31);
    rgba_buf = (uint32_t *)malloc((size_t)VRAM_W * VRAM_H * 4);
    read_buf = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * 2);
    load_buf = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * 2);
    pres_buf = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * 2);
    dirty = false; nv = 0; have_cur = false;
    gl_ready = true;
    plat_log("[GPU] GLES2 renderer ready: %s", (const char *)glGetString(GL_RENDERER));
}

/* ---- dirty tracking ------------------------------------------------ */
static void mark_dirty(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VRAM_W) x1 = VRAM_W; if (y1 > VRAM_H) y1 = VRAM_H;
    if (x0 >= x1 || y0 >= y1) return;
    if (!dirty) { dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1; dirty = true; return; }
    if (x0 < dx0) dx0 = x0; if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1; if (y1 > dy1) dy1 = y1;
}

static void copy_fb_to_tx(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glBindTexture(GL_TEXTURE_2D, tx_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, x, y, w, h);
}

static void sync_tx(void)
{
    if (!dirty) return;
    copy_fb_to_tx(dx0, dy0, dx1 - dx0, dy1 - dy0);
    dirty = false;
}

static bool rect_hits_dirty(int x0, int y0, int x1, int y1)
{
    if (!dirty) return false;
    if (x1 > VRAM_W || y1 > VRAM_H) return true;      /* wraps: be conservative */
    return x0 < dx1 && x1 > dx0 && y0 < dy1 && y1 > dy0;
}

/* ---- state helpers ------------------------------------------------- */
static void set_blend(bool on, int mode)
{
    if (!on) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    switch (mode) {
    case 0: glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO); break;
    case 1: glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO); break;
    case 2: glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO); break;
    default: glBlendColor(0.25f, 0.25f, 0.25f, 0.25f);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE, GL_ONE, GL_ZERO); break;
    }
}

/* stencil != 0  <=>  mask bit set. Only active once a game touches E6. */
static void set_stencil(bool check, int wval)
{
    if (!g_mask_used) { glDisable(GL_STENCIL_TEST); return; }
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    if (check) { glStencilFunc(GL_EQUAL, 0, 0xFF); glStencilOp(GL_KEEP, GL_KEEP, wval ? GL_INCR : GL_KEEP); }
    else       { glStencilFunc(GL_ALWAYS, wval, 0xFF); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); }
}

static void bind_vram_pass(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tx_tex);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GVert), &vbuf[0].x);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GVert), &vbuf[0].r);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GVert), &vbuf[0].u);
}

static void prim_pass(const Key *k, int stp_mode, bool blend_on, int wval)
{
    glUniform4f(U.misc, k->wax || k->way || k->wox || k->woy ? 1.0f : 0.0f, (float)k->setm, (float)stp_mode, 0.0f);
    set_blend(blend_on, k->blend);
    set_stencil(k->chkm, wval);
    glDrawArrays(k->mode ? GL_LINES : GL_TRIANGLES, 0, nv);
}

static void draw_batch(void)
{
    const Key *k = &cur;
    int sw = k->sr - k->sl + 1, sh = k->sb - k->st + 1;
    if (sw <= 0 || sh <= 0) return;
    bind_vram_pass();
    glEnable(GL_SCISSOR_TEST);
    glScissor(k->sl, k->st, sw, sh);
    glUseProgram(p_prim);
    glUniform1i(U.vram, 0);
    glUniform4f(U.page, (float)k->tpx, (float)k->tpy, (float)k->depth, (float)k->tex);
    glUniform4f(U.clut, (float)k->clx, (float)k->cly, (float)k->raw, (float)k->dither);
    glUniform4f(U.win, (float)k->wax, (float)k->way, (float)k->wox, (float)k->woy);

    if (k->tex && (k->semi || g_mask_used)) {
        /* opaque texels (stp = 0) first, then semi-transparent / masked texels */
        prim_pass(k, 1, false, k->setm);
        prim_pass(k, 2, k->semi != 0, 1);
    } else if (k->semi) {
        prim_pass(k, 0, true, k->setm);
    } else {
        prim_pass(k, 0, false, k->tex ? 0 : k->setm);
    }
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
}

static void flush_batch(void)
{
    if (!have_cur) return;
    if (nv) { draw_batch(); mark_dirty(bx0, by0, bx1, by1); }
    nv = 0; have_cur = false;
}

static void ensure_tex_synced(const Key *k)
{
    if (!k->tex) return;
    int pw = k->depth == 0 ? 64 : (k->depth == 1 ? 128 : 256);
    int cw = k->depth == 0 ? 16 : 256;
    bool hit = rect_hits_dirty(k->tpx, k->tpy, k->tpx + pw, k->tpy + 256);
    if (!hit && k->depth < 2) hit = rect_hits_dirty(k->clx, k->cly, k->clx + cw, k->cly + 1);
    if (hit) sync_tx();
}

static void begin_prim(const Key *k)
{
    if (have_cur && memcmp(&cur, k, sizeof *k) != 0) flush_batch();
    if (!have_cur) {
        cur = *k; have_cur = true;
        bx0 = VRAM_W; by0 = VRAM_H; bx1 = 0; by1 = 0;
        ensure_tex_synced(k);
    }
}

static void batch_bbox(float x0, float y0, float x1, float y1)
{
    int a = (int)floorf(x0) - 1, b = (int)floorf(y0) - 1, c = (int)ceilf(x1) + 1, d = (int)ceilf(y1) + 1;
    if (a < cur.sl) a = cur.sl; if (b < cur.st) b = cur.st;
    if (c > cur.sr + 1) c = cur.sr + 1; if (d > cur.sb + 1) d = cur.sb + 1;
    if (a < bx0) bx0 = a; if (b < by0) by0 = b;
    if (c > bx1) bx1 = c; if (d > by1) by1 = d;
}

static void make_key(Key *k, int mode, bool tex, bool raw, bool semi, bool dither, int clut)
{
    memset(k, 0, sizeof *k);
    k->mode = mode; k->tex = tex; k->raw = tex && raw; k->semi = semi;
    k->blend = semi ? blend_mode : 0;
    k->dither = dither; k->setm = set_mask; k->chkm = check_mask;
    if (tex) {
        k->depth = tp_depth >= 2 ? 2 : tp_depth;
        k->tpx = tp_x; k->tpy = tp_y;
        if (k->depth < 2) { k->clx = (clut & 0x3F) * 16; k->cly = (clut >> 6) & 0x1FF; }
        if (tw_mask_x || tw_mask_y) {
            k->wax = 255 - tw_mask_x * 8; k->way = 255 - tw_mask_y * 8;
            k->wox = (tw_off_x & tw_mask_x) * 8; k->woy = (tw_off_y & tw_mask_y) * 8;
        }
    }
    k->sl = draw_left; k->st = draw_top;
    k->sr = draw_right > VRAM_W - 1 ? VRAM_W - 1 : draw_right;
    k->sb = draw_bottom > VRAM_H - 1 ? VRAM_H - 1 : draw_bottom;
}

static inline void put_vert(float x, float y, int r, int g, int b, float u, float v)
{
    GVert *p = &vbuf[nv++];
    p->x = x; p->y = y; p->r = (uint8_t)r; p->g = (uint8_t)g; p->b = (uint8_t)b; p->a = 255; p->u = u; p->v = v;
}

/* ---- primitives ---------------------------------------------------- */
typedef struct { int x, y, r, g, b, u, v; } Vert;

static void emit_triangle(const Key *k, const Vert *a, const Vert *b, const Vert *c)
{
    int span_x = imax3(a->x, b->x, c->x) - imin3(a->x, b->x, c->x);
    int span_y = imax3(a->y, b->y, c->y) - imin3(a->y, b->y, c->y);
    if (span_x > 1023 || span_y > 511) return;
    if ((b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x) == 0) return;
    begin_prim(k);
    if (nv + 3 > MAX_VERTS) { Key kk = *k; flush_batch(); begin_prim(&kk); }
    const Vert *v[3] = { a, b, c };
    for (int i = 0; i < 3; i++)
        put_vert(v[i]->x + 0.5f, v[i]->y + 0.5f, v[i]->r, v[i]->g, v[i]->b, (float)v[i]->u, (float)v[i]->v);
    batch_bbox((float)imin3(a->x, b->x, c->x), (float)imin3(a->y, b->y, c->y),
               (float)imax3(a->x, b->x, c->x), (float)imax3(a->y, b->y, c->y));
}

static void draw_polygon(void)
{
    ensure_gl();
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
    Key k;
    make_key(&k, 0, tex, raw, semi, dither_on && (gouraud || (tex && !raw)), clut);
    emit_triangle(&k, &v[0], &v[1], &v[2]);
    if (quad) emit_triangle(&k, &v[1], &v[2], &v[3]);
}

static void draw_rectangle(void)
{
    ensure_gl();
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
    if (w <= 0 || h <= 0) return;
    Key k;
    make_key(&k, 0, tex, raw, semi, false, clut);
    begin_prim(&k);
    if (nv + 6 > MAX_VERTS) { flush_batch(); begin_prim(&k); }
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    float uu0 = (float)u0, vv0 = (float)v0, uu1 = (float)(u0 + w), vv1 = (float)(v0 + h);
    put_vert(x0, y0, cr, cg, cb, uu0, vv0); put_vert(x1, y0, cr, cg, cb, uu1, vv0); put_vert(x0, y1, cr, cg, cb, uu0, vv1);
    put_vert(x1, y0, cr, cg, cb, uu1, vv0); put_vert(x1, y1, cr, cg, cb, uu1, vv1); put_vert(x0, y1, cr, cg, cb, uu0, vv1);
    batch_bbox(x0, y0, x1, y1);
}

static void line_segment(int x0, int y0, int r0, int g0, int b0, int x1, int y1, int r1, int g1, int b1, bool semi, bool gouraud)
{
    (void)gouraud;
    ensure_gl();
    Key k;
    make_key(&k, 1, false, false, semi, dither_on, 0);
    begin_prim(&k);
    if (nv + 2 > MAX_VERTS) { flush_batch(); begin_prim(&k); }
    x0 += draw_ofs_x; y0 += draw_ofs_y; x1 += draw_ofs_x; y1 += draw_ofs_y;
    put_vert(x0 + 0.5f, y0 + 0.5f, r0, g0, b0, 0, 0);
    put_vert(x1 + 0.5f, y1 + 0.5f, r1, g1, b1, 0, 0);
    batch_bbox((float)(x0 < x1 ? x0 : x1), (float)(y0 < y1 ? y0 : y1), (float)(x0 > x1 ? x0 : x1), (float)(y0 > y1 ? y0 : y1));
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

/* ---- VRAM transfers ------------------------------------------------ */
/* split [x, x+w) over a wrapping axis of size lim into up to two segments */
static int split_wrap(int x, int w, int lim, int seg[2][2])
{
    x &= lim - 1;
    if (x + w <= lim) { seg[0][0] = x; seg[0][1] = w; return 1; }
    seg[0][0] = x; seg[0][1] = lim - x;
    seg[1][0] = 0; seg[1][1] = w - (lim - x);
    return 2;
}

static void stencil_rebuild(int x, int y, int w, int h)
{
    if (!g_mask_used || w <= 0 || h <= 0) return;
    copy_fb_to_tx(x, y, w, h);
    bind_vram_pass();
    glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h);
    glStencilMask(0xFF); glClearStencil(0); glClear(GL_STENCIL_BUFFER_BIT);
    glUseProgram(p_copy);
    glUniform1i(UC.vram, 0);
    glUniform4f(UC.misc, 1.0f, 0.0f, 0.0f, 0.0f);
    glDisable(GL_BLEND);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    nv = 0;
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    put_vert(x0, y0, 0, 0, 0, x0, y0); put_vert(x1, y0, 0, 0, 0, x1, y0); put_vert(x0, y1, 0, 0, 0, x0, y1);
    put_vert(x1, y0, 0, 0, 0, x1, y0); put_vert(x1, y1, 0, 0, 0, x1, y1); put_vert(x0, y1, 0, 0, 0, x0, y1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    nv = 0;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
}

void gpu_mask_touched(void)
{
    if (g_mask_used) return;
    ensure_gl();
    flush_batch();
    g_mask_used = true;
    stencil_rebuild(0, 0, VRAM_W, VRAM_H);
}

static void fill_rect(void)
{
    ensure_gl();
    flush_batch();
    int r = (int)(fifo[0] & 0xFF) >> 3, g = (int)((fifo[0] >> 8) & 0xFF) >> 3, b = (int)((fifo[0] >> 16) & 0xFF) >> 3;
    int x = (int)(fifo[1] & 0x3F0), y = (int)((fifo[1] >> 16) & 0x1FF);
    int w = (int)(((fifo[2] & 0x3FF) + 0xF) & ~0xF), h = (int)((fifo[2] >> 16) & 0x1FF);
    if (w <= 0 || h <= 0) return;
    int sx[2][2], sy[2][2];
    int nx = split_wrap(x, w, VRAM_W, sx), ny = split_wrap(y, h, VRAM_H, sy);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
    glClearColor(r / 31.0f, g / 31.0f, b / 31.0f, 0.0f);
    glClearStencil(0);
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < nx; i++) for (int j = 0; j < ny; j++) {
        glScissor(sx[i][0], sy[j][0], sx[i][1], sy[j][1]);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        mark_dirty(sx[i][0], sy[j][0], sx[i][0] + sx[i][1], sy[j][0] + sy[j][1]);
    }
    glDisable(GL_SCISSOR_TEST);
}

static void upload_rows(int x, int y, int w, int h, const uint32_t *rgba)
{
    glBindTexture(GL_TEXTURE_2D, fb_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (x + w <= VRAM_W && y + h <= VRAM_H) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        mark_dirty(x, y, x + w, y + h);
        return;
    }
    for (int r = 0; r < h; r++) {
        int yy = (y + r) & (VRAM_H - 1);
        int seg[2][2]; int n = split_wrap(x, w, VRAM_W, seg);
        int off = 0;
        for (int i = 0; i < n; i++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, seg[i][0], yy, seg[i][1], 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba + r * w + off);
            off += seg[i][1];
        }
    }
    mark_dirty(0, 0, VRAM_W, VRAM_H);
}

static void finish_image_load(void)
{
    ensure_gl();
    flush_batch();
    int n = load_w * load_h;
    for (int i = 0; i < n; i++) {
        uint16_t v = load_buf[i];
        uint32_t a = (set_mask || (v & 0x8000)) ? 255u : 0u;
        rgba_buf[i] = (uint32_t)lut5[v & 31] | ((uint32_t)lut5[(v >> 5) & 31] << 8)
                    | ((uint32_t)lut5[(v >> 10) & 31] << 16) | (a << 24);
    }
    upload_rows(load_x, load_y, load_w, load_h, rgba_buf);
    if (g_mask_used) {
        if (load_x + load_w <= VRAM_W && load_y + load_h <= VRAM_H) stencil_rebuild(load_x, load_y, load_w, load_h);
        else stencil_rebuild(0, 0, VRAM_W, VRAM_H);
    }
}

static void copy_vram(void)
{
    ensure_gl();
    flush_batch();
    int sx = (int)(fifo[1] & 0x3FF), sy = (int)((fifo[1] >> 16) & 0x1FF);
    int dx = (int)(fifo[2] & 0x3FF), dy = (int)((fifo[2] >> 16) & 0x1FF);
    int w = (int)(fifo[3] & 0x3FF); if (!w) w = 0x400;
    int h = (int)((fifo[3] >> 16) & 0x1FF); if (!h) h = 0x200;
    /* wrapping copies are clamped (rare) */
    if (sx + w > VRAM_W) w = VRAM_W - sx; if (dx + w > VRAM_W) w = VRAM_W - dx;
    if (sy + h > VRAM_H) h = VRAM_H - sy; if (dy + h > VRAM_H) h = VRAM_H - dy;
    if (w <= 0 || h <= 0) return;
    copy_fb_to_tx(sx, sy, w, h);                 /* snapshot source (handles overlap) */
    bind_vram_pass();
    glEnable(GL_SCISSOR_TEST); glScissor(dx, dy, w, h);
    glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST);
    glUseProgram(p_copy);
    glUniform1i(UC.vram, 0);
    glUniform4f(UC.misc, 0.0f, set_mask ? 1.0f : 0.0f, 0.0f, 0.0f);
    nv = 0;
    float x0 = (float)dx, y0 = (float)dy, x1 = (float)(dx + w), y1 = (float)(dy + h);
    float u0 = (float)sx, v0 = (float)sy, u1 = (float)(sx + w), v1 = (float)(sy + h);
    put_vert(x0, y0, 0, 0, 0, u0, v0); put_vert(x1, y0, 0, 0, 0, u1, v0); put_vert(x0, y1, 0, 0, 0, u0, v1);
    put_vert(x1, y0, 0, 0, 0, u1, v0); put_vert(x1, y1, 0, 0, 0, u1, v1); put_vert(x0, y1, 0, 0, 0, u0, v1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    nv = 0;
    glDisable(GL_SCISSOR_TEST);
    mark_dirty(dx, dy, dx + w, dy + h);
    stencil_rebuild(dx, dy, w, h);
}

/* Read a VRAM rect back as RGB555+mask halfwords (w*h, row major). Wrap-safe. */
static inline uint16_t px_to_555(const uint8_t *p)
{
    uint16_t r5 = (uint16_t)((p[0] * 31 + 127) / 255), g5 = (uint16_t)((p[1] * 31 + 127) / 255), b5 = (uint16_t)((p[2] * 31 + 127) / 255);
    return (uint16_t)(r5 | (g5 << 5) | (b5 << 10) | (p[3] >= 128 ? 0x8000 : 0));
}

/* Read a VRAM rect back as RGB555+mask halfwords (w*h, row major). Wrap-safe. */
static void readback_rect(int x, int y, int w, int h, uint16_t *out)
{
    flush_batch();
    glBindFramebuffer(GL_FRAMEBUFFER, fb_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (x + w <= VRAM_W && y + h <= VRAM_H) {          /* fast path: one call */
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba_buf);
        const uint8_t *p = (const uint8_t *)rgba_buf;
        for (int i = 0; i < w * h; i++, p += 4) out[i] = px_to_555(p);
        return;
    }
    for (int r = 0; r < h; r++) {
        int yy = (y + r) & (VRAM_H - 1);
        int seg[2][2]; int n = split_wrap(x, w, VRAM_W, seg);
        int off = 0;
        for (int i = 0; i < n; i++) {
            glReadPixels(seg[i][0], yy, seg[i][1], 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba_buf);
            const uint8_t *p = (const uint8_t *)rgba_buf;
            for (int k = 0; k < seg[i][1]; k++, p += 4) out[r * w + off + k] = px_to_555(p);
            off += seg[i][1];
        }
    }
}

static void begin_image_read(void)
{
    ensure_gl();
    read_x = (int)(fifo[1] & 0x3FF); read_y = (int)((fifo[1] >> 16) & 0x1FF);
    read_w = (int)(fifo[2] & 0x3FF); if (!read_w) read_w = 0x400;
    read_h = (int)((fifo[2] >> 16) & 0x1FF); if (!read_h) read_h = 0x200;
    readback_rect(read_x, read_y, read_w, read_h, read_buf);
    read_px = 0; read_active = true;
}

static uint16_t read_image_halfword(void)
{
    if (!read_active) return 0;
    uint16_t v = read_buf[read_px];
    if (++read_px >= read_w * read_h) read_active = false;
    return v;
}

static void store_image_halfword(uint16_t value)
{
    if (!load_active) return;
    load_buf[load_px] = value;
    if (++load_px >= load_w * load_h) { load_active = false; finish_image_load(); }
}

/* ---- present -------------------------------------------------------- */
void gpu_present(int dx, int dy, int dw, int dh, bool rgb24, bool enabled)
{
    ensure_gl();
    flush_batch();
    int sfbo, sw, sh; plat_gl_screen(&sfbo, &sw, &sh);
    glBindFramebuffer(GL_FRAMEBUFFER, sfbo);
    glViewport(0, 0, sw, sh);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!enabled || dw <= 0 || dh <= 0) return;
    if (dw > 1024) dw = 1024;
    if (dh > 512) dh = 512;

    float u0, v0, u1, v1; GLuint tex;
    if (!rgb24) {
        tex = fb_tex;
        u0 = (float)dx / 1024.0f; u1 = (float)(dx + dw) / 1024.0f;
        v0 = (float)dy / 512.0f;  v1 = (float)(dy + dh) / 512.0f;
    } else {
        /* 24bpp (FMV): pixels are packed 3 bytes each across the 16-bit VRAM */
        int hw = (dw * 3 + 1) / 2; if (hw > 1024) hw = 1024;
        readback_rect(dx, dy, hw, dh, pres_buf);
        uint16_t *out = (uint16_t *)rgba_buf;   /* safe: readback is done with rgba_buf */
        for (int y = 0; y < dh; y++) {
            const uint8_t *row = (const uint8_t *)(pres_buf + y * hw);
            for (int x = 0; x < dw; x++) {
                const uint8_t *p = row + x * 3;
                out[y * dw + x] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, sfbo);
        glBindTexture(GL_TEXTURE_2D, p24_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dw, dh, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, out);
        tex = p24_tex;
        u0 = 0; v0 = 0; u1 = (float)dw / 1024.0f; v1 = (float)dh / 512.0f;
    }

    float vw = (float)sw, vh = (float)sh, sx, sy;     /* fit 4:3 */
    if (vw / vh > 4.0f / 3.0f) { sy = 1.0f; sx = (vh * 4.0f / 3.0f) / vw; }
    else { sx = 1.0f; sy = (vw * 3.0f / 4.0f) / vh; }
    float q[16] = { -sx, -sy, u0, v1,   sx, -sy, u1, v1,   -sx, sy, u0, v0,   sx, sy, u1, v0 };

    glUseProgram(p_pres);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(UP_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0); glDisableVertexAttribArray(1); glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, q);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 16, q + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}


uint32_t gpu_read_data(void)
{
    if (!read_active) return gpu_read;
    uint16_t lo = read_image_halfword(), hi = read_image_halfword();
    return lo | ((uint32_t)hi << 16);
}

static void begin_image_load(void)
{
    ensure_gl();
    load_x = (int)(fifo[1] & 0x3FF); load_y = (int)((fifo[1] >> 16) & 0x1FF);
    load_w = (int)(fifo[2] & 0xFFFF); if (!load_w) load_w = 0x400; else load_w &= 0x3FF; if (!load_w) load_w = 0x400;
    load_h = (int)((fifo[2] >> 16) & 0xFFFF); if (!load_h) load_h = 0x200; else load_h &= 0x1FF; if (!load_h) load_h = 0x200;
    load_px = 0; load_active = true;
    fifo_n = 0;
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
    case 0xE6: set_mask = (word & 1) != 0; check_mask = (word & 2) != 0;
               if ((word & 3) && !g_mask_used) gpu_mask_touched();
               break;
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

