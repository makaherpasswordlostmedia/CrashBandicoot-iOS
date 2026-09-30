/* gpu_gles2.inc - PSX GPU on OpenGL ES 2.0.
 *
 * Replaces the software rasterizer completely (there is NO CPU fallback):
 *   - VRAM is a 1024x512 RGBA8 texture (tex_a) used as a render target.
 *     Each PSX 5-bit channel k is stored as byte k*8, the mask bit as alpha 0/255,
 *     so every 16-bit VRAM word survives a round trip bit-exactly (CLUT indices etc.).
 *   - A second texture (tex_b) is a read-only snapshot of VRAM. Shaders sample
 *     textures/CLUTs from it (GLES2 cannot sample the texture being rendered to).
 *     It is refreshed lazily, only when a draw samples an area that changed.
 *   - Primitives are batched (client-side vertex arrays) and flushed on state change.
 *   - Semi-transparency uses fixed-function blending (modes 0..3); textured semi
 *     primitives are drawn in two passes (opaque texels, then blended texels).
 *   - gpu_present() draws the display rectangle to the screen FBO, handles 15/24 bit.
 *
 * Host hooks (ios_host.m): plat_gl_make_current(), plat_gl_screen().
 * Checked strings for `strings binary | grep`: "GLES2 renderer ready".
 */
#include "psx.h"
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>

void plat_gl_make_current(void);
void plat_gl_screen(int *fbo, int *w, int *h);

/* marker so the backend can be identified from the binary */
__attribute__((used)) const char g_gpu_backend_tag[] = "GPU_BACKEND=GLES2(no-software-fallback)";

uint16_t *g_vram;            /* CPU scratch only (image read-back); the real VRAM is on the GPU */
volatile int g_gpu_dirty;

/* ------------------------------------------------------------------ */
/* GPU register state (PSX GPU register state)     */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* GL objects                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    GLuint id;
    GLint vram, dith, setmask, chk, pass, twin, twand, twor, rect, rgb24, off;
    float cv[8]; unsigned cmask;      /* last uniform values sent (flush_batch only) */
} Prog;

static GLuint tex_a, tex_b, tex_dith, fbo_a;
static Prog pf, pt, pd, pc;           /* flat, textured, display, vram-copy */
static bool gl_ready;
static uint16_t *load_buf;
static uint8_t *scr_a, *scr_b, *scr_c;    /* 1024*512*4 scratch buffers */

static unsigned st_frames, st_draws, st_tris, st_syncs, st_uploads, st_reads, st_fills, st_copies;
static unsigned st_last_draws, st_last_tris;
static uint64_t st_last_ns;
/* why a batch was flushed (submit): 0 tex/flat program, 1 pass, 2 blend, 3 prim, 4 tex window, 5 scissor, 6 mask, 7 vertex cap */
enum { FR_TEX, FR_PASS, FR_BLEND, FR_PRIM, FR_TW, FR_SCISSOR, FR_MASK, FR_CAP, FR_N };
static unsigned st_fr[FR_N], st_fr_last[FR_N];
static uint64_t st_flush_ns, st_flush_ns_last, st_present_ns, st_present_ns_last;
static unsigned st_new_frames, st_new_last, st_work_last;
static bool first_draw_logged;
static void gl_invalidate(void);     /* defined with the GL state shadow, below */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------------ */
/* Shaders                                                              */
/* ------------------------------------------------------------------ */
static const char VS_MAIN[] =
    "attribute vec2 a_pos; attribute vec4 a_col; attribute vec2 a_uv;\n"
    "attribute vec4 a_tp; attribute vec4 a_cl;\n"
    "varying vec4 v_col; varying vec2 v_uv; varying vec4 v_tp; varying vec4 v_cl;\n"
    "void main(){ v_col = a_col; v_uv = a_uv; v_tp = a_tp; v_cl = a_cl;\n"
    "  gl_Position = vec4(a_pos.x * 0.001953125 - 1.0, a_pos.y * 0.00390625 - 1.0, 0.0, 1.0); }\n";

static const char VS_DISP[] =
    "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;\n"
    "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

/* Main fragment shader. With TEXTURED defined it samples the snapshot texture.
 * Bit-exact tricks: every division is a multiplication by a power of two. */
static const char FS_MAIN[] =
    "precision highp float;\n"
    "varying vec4 v_col; varying vec2 v_uv; varying vec4 v_tp; varying vec4 v_cl;\n"
    "uniform sampler2D u_vram; uniform sampler2D u_dith;\n"
    "uniform float u_setmask; uniform float u_chk;\n"
    "#ifdef TEXTURED\n"
    "uniform float u_pass;\n"
    "uniform float u_twin; uniform vec2 u_twand; uniform vec2 u_twor;\n"
    "vec4 fetch(vec2 p){ p = mod(p, vec2(1024.0, 512.0));\n"
    "  return texture2D(u_vram, (p + 0.5) * vec2(0.0009765625, 0.001953125)); }\n"
    "vec4 bits(vec4 t){ return vec4(floor((t.rgb * 255.0 + 0.5) * 0.125), step(0.5, t.a)); }\n"
    "float word(vec4 b){ return b.r + b.g * 32.0 + b.b * 1024.0 + b.a * 32768.0; }\n"
    "float band(float a, float b){ float r = 0.0; float p = 1.0;\n"
    "  for (int i = 0; i < 8; i++) { r += p * mod(floor(a / p), 2.0) * mod(floor(b / p), 2.0); p *= 2.0; }\n"
    "  return r; }\n"
    "float twin(float x, float a, float o){ float t = band(x, a); return t + o - band(t, o); }\n"
    "#endif\n"
    "void main(){\n"
    "  float u_dither = floor(v_cl.z + 0.5);\n"
    "#ifdef TEXTURED\n"
    "  vec2 u_tp = floor(v_tp.xy + 0.5); float u_depth = floor(v_tp.z + 0.5); float u_raw = floor(v_tp.w + 0.5);\n"
    "  vec2 u_clut = floor(v_cl.xy + 0.5);\n"
    "#endif\n"
    "  float d = 0.0;\n"
    "  if (u_dither > 0.5) {\n"
    "    vec2 q = mod(floor(gl_FragCoord.xy), 4.0);\n"
    "    d = floor(texture2D(u_dith, (q + 0.5) * 0.25).r * 255.0 + 0.5) - 4.0;\n"
    "  }\n"
    "  if (u_chk > 0.5) {\n"
    "    if (texture2D(u_vram, gl_FragCoord.xy * vec2(0.0009765625, 0.001953125)).a > 0.5) discard;\n"
    "  }\n"
    "  vec3 c8 = floor(v_col.rgb * 255.0 + 0.5);\n"
    "  float alpha = u_setmask;\n"
    "  vec3 o;\n"
    "#ifdef TEXTURED\n"
    "  vec2 uv = floor(v_uv + 0.004);\n"
    "  float u = mod(uv.x, 256.0); float v = mod(uv.y, 256.0);\n"
    "  if (u_twin > 0.5) { u = twin(u, u_twand.x, u_twor.x); v = twin(v, u_twand.y, u_twor.y); }\n"
    "  vec4 tk;\n"
    "  if (u_depth < 0.5) {\n"
    "    float wd = word(bits(fetch(vec2(u_tp.x + floor(u * 0.25), u_tp.y + v))));\n"
    "    float s = mod(u, 4.0);\n"
    "    float inv = s < 0.5 ? 1.0 : (s < 1.5 ? 0.0625 : (s < 2.5 ? 0.00390625 : 0.000244140625));\n"
    "    float idx = mod(floor(wd * inv), 16.0);\n"
    "    tk = bits(fetch(vec2(u_clut.x + idx, u_clut.y)));\n"
    "  } else if (u_depth < 1.5) {\n"
    "    float wd = word(bits(fetch(vec2(u_tp.x + floor(u * 0.5), u_tp.y + v))));\n"
    "    float inv = mod(u, 2.0) < 0.5 ? 1.0 : 0.00390625;\n"
    "    float idx = mod(floor(wd * inv), 256.0);\n"
    "    tk = bits(fetch(vec2(u_clut.x + idx, u_clut.y)));\n"
    "  } else {\n"
    "    tk = bits(fetch(vec2(u_tp.x + u, u_tp.y + v)));\n"
    "  }\n"
    "  if (tk.r + tk.g + tk.b + tk.a < 0.5) discard;\n"
    "  if (u_pass > 0.5 && u_pass < 1.5 && tk.a > 0.5) discard;\n"
    "  if (u_pass > 1.5 && tk.a < 0.5) discard;\n"
    "  alpha = max(alpha, tk.a);\n"
    "  vec3 m = tk.rgb * 8.0;\n"
    "  if (u_raw < 0.5) m = floor(m * c8 * 0.0078125);\n"
    "  o = min(floor(clamp(m + d, 0.0, 255.0) * 0.125), 31.0);\n"
    "#else\n"
    "  o = min(floor(clamp(c8 + d, 0.0, 255.0) * 0.125), 31.0);\n"
    "#endif\n"
    "  gl_FragColor = vec4(o * (8.0 / 255.0), alpha);\n"
    "}\n";

static const char FS_DISP[] =
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_vram; uniform vec4 u_rect; uniform float u_rgb24;\n"
    "vec4 fetch(vec2 p){ p = mod(p, vec2(1024.0, 512.0));\n"
    "  return texture2D(u_vram, (p + 0.5) * vec2(0.0009765625, 0.001953125)); }\n"
    "float word(vec4 t){ vec4 b = vec4(floor((t.rgb * 255.0 + 0.5) * 0.125), step(0.5, t.a));\n"
    "  return b.r + b.g * 32.0 + b.b * 1024.0 + b.a * 32768.0; }\n"
    "void main(){\n"
    "  vec2 px = v_uv * u_rect.zw;\n"
    "  if (u_rgb24 < 0.5) {\n"
    "    vec2 p = clamp(u_rect.xy + px, u_rect.xy + 0.5, u_rect.xy + u_rect.zw - 0.5);\n"
    "    vec3 c = texture2D(u_vram, p * vec2(0.0009765625, 0.001953125)).rgb * 1.0282258;\n"
    "    gl_FragColor = vec4(min(c, 1.0), 1.0);\n"
    "  } else {\n"
    "    float dx = floor(min(px.x, u_rect.z - 0.5)); float dy = floor(min(px.y, u_rect.w - 0.5));\n"
    "    float o = 3.0 * dx; float wi = floor(o * 0.5); float odd = o - 2.0 * wi;\n"
    "    vec2 base = vec2(u_rect.x + wi, u_rect.y + dy);\n"
    "    float w0 = word(fetch(base)); float w1 = word(fetch(base + vec2(1.0, 0.0)));\n"
    "    float hi0 = floor(w0 * 0.00390625); float lo0 = w0 - hi0 * 256.0;\n"
    "    float hi1 = floor(w1 * 0.00390625); float lo1 = w1 - hi1 * 256.0;\n"
    "    vec3 c = odd < 0.5 ? vec3(lo0, hi0, lo1) : vec3(hi0, lo1, hi1);\n"
    "    gl_FragColor = vec4(c * (1.0 / 255.0), 1.0);\n"
    "  }\n"
    "}\n";

static const char FS_COPY[] =
    "precision highp float;\n"
    "varying vec4 v_col; varying vec2 v_uv;\n"
    "uniform sampler2D u_vram; uniform vec2 u_off; uniform float u_setmask; uniform float u_chk;\n"
    "void main(){\n"
    "  vec2 fc = floor(gl_FragCoord.xy);\n"
    "  vec2 inv = vec2(0.0009765625, 0.001953125);\n"
    "  if (u_chk > 0.5) { if (texture2D(u_vram, (fc + 0.5) * inv).a > 0.5) discard; }\n"
    "  vec2 s = mod(fc + u_off, vec2(1024.0, 512.0));\n"
    "  vec4 t = texture2D(u_vram, (s + 0.5) * inv);\n"
    "  gl_FragColor = vec4(t.rgb, max(t.a, u_setmask));\n"
    "}\n";

static GLuint compile_shader(GLenum type, int nsrc, const char **src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, nsrc, src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; log[0] = 0;
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        plat_log("[GPU] shader error: %s", log);
        plat_fatal("GLES2 shader compile failed (see crash.log, line \"[GPU] shader error\")");
    }
    return s;
}

static void build_prog(Prog *P, const char *vs, const char *defs, const char *fs, const char *name)
{
    const char *vsrc[1] = { vs };
    const char *fsrc[2] = { defs, fs };
    GLuint v = compile_shader(GL_VERTEX_SHADER, 1, vsrc);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, 2, fsrc);
    P->id = glCreateProgram();
    glAttachShader(P->id, v); glAttachShader(P->id, f);
    glBindAttribLocation(P->id, 0, "a_pos");
    glBindAttribLocation(P->id, 1, "a_col");
    glBindAttribLocation(P->id, 2, "a_uv");
    glBindAttribLocation(P->id, 3, "a_tp");
    glBindAttribLocation(P->id, 4, "a_cl");
    glLinkProgram(P->id);
    GLint ok = 0; glGetProgramiv(P->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; log[0] = 0;
        glGetProgramInfoLog(P->id, sizeof log, NULL, log);
        plat_log("[GPU] shader error: link %s: %s", name, log);
        plat_fatal("GLES2 shader link failed (see crash.log, line \"[GPU] shader error\")");
    }
#define U(n, s) P->n = glGetUniformLocation(P->id, s)
    U(vram, "u_vram"); U(dith, "u_dith"); U(setmask, "u_setmask"); U(chk, "u_chk"); U(pass, "u_pass");
    U(twin, "u_twin"); U(twand, "u_twand"); U(twor, "u_twor");
    U(rect, "u_rect"); U(rgb24, "u_rgb24"); U(off, "u_off");
#undef U
}

static void make_tex(GLuint *t, GLint filter, const void *zero)
{
    glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_W, VRAM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void gl_init(void)
{
    glDisable(GL_DITHER); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    scr_a = (uint8_t *)malloc((size_t)VRAM_W * VRAM_H * 4);
    scr_b = (uint8_t *)malloc((size_t)VRAM_W * VRAM_H * 4);
    scr_c = (uint8_t *)malloc((size_t)VRAM_W * VRAM_H * 4);
    load_buf = (uint16_t *)malloc((size_t)VRAM_W * VRAM_H * 2);
    if (!scr_a || !scr_b || !scr_c || !load_buf) plat_fatal("out of memory (gpu scratch)");
    memset(scr_a, 0, (size_t)VRAM_W * VRAM_H * 4);

    glActiveTexture(GL_TEXTURE0);
    make_tex(&tex_a, GL_LINEAR, scr_a);
    make_tex(&tex_b, GL_NEAREST, scr_a);

    glGenFramebuffers(1, &fbo_a);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_a, 0);
    GLenum fs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fs != GL_FRAMEBUFFER_COMPLETE) {
        plat_log("[GPU] VRAM FBO incomplete: 0x%x", (unsigned)fs);
        plat_fatal("GLES2: VRAM framebuffer is incomplete (see crash.log, line \"[GPU] VRAM FBO incomplete\")");
    }

    /* 4x4 ordered-dither table (+4 bias so it fits in unsigned bytes) */
    static const uint8_t dth[16] = { 0,4,1,5,  6,2,7,3,  1,5,0,4,  7,3,6,2 };
    glGenTextures(1, &tex_dith);
    glBindTexture(GL_TEXTURE_2D, tex_dith);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 4, 4, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, dth);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    build_prog(&pf, VS_MAIN, "\n", FS_MAIN, "flat");
    build_prog(&pt, VS_MAIN, "#define TEXTURED 1\n", FS_MAIN, "textured");
    build_prog(&pd, VS_DISP, "\n", FS_DISP, "display");
    build_prog(&pc, VS_MAIN, "\n", FS_COPY, "copy");
    glUseProgram(pf.id); glUniform1i(pf.vram, 0); glUniform1i(pf.dith, 1);      /* never change */
    glUseProgram(pt.id); glUniform1i(pt.vram, 0); glUniform1i(pt.dith, 1);
    gl_invalidate();

    glClearColor(0, 0, 0, 0);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);

    gl_ready = true;
    plat_log("[GPU] GLES2 renderer ready (VRAM %dx%d RGBA8 on GPU, software rasterizer compiled out)", VRAM_W, VRAM_H);
    plat_log("[GPU] GL_RENDERER=%s", (const char *)glGetString(GL_RENDERER));
    plat_log("[GPU] GL_VENDOR=%s", (const char *)glGetString(GL_VENDOR));
    plat_log("[GPU] GL_VERSION=%s", (const char *)glGetString(GL_VERSION));
}

void gpu_init(void)
{
    g_vram = (uint16_t *)calloc((size_t)VRAM_W * VRAM_H, sizeof(uint16_t));
    plat_gl_make_current();          /* creates the EAGL context on the emulation thread */
    gl_init();
}

/* ------------------------------------------------------------------ */
/* Snapshot (tex_b) tracking                                            */
/* ------------------------------------------------------------------ */
/* dirty = bounding box (half-open) of everything drawn into tex_a since tex_b was last synced */
static struct { int x0, y0, x1, y1; bool valid; } dirty;

static void dirty_add(int x0, int y0, int x1, int y1)
{
    if (!dirty.valid) { dirty.x0 = x0; dirty.y0 = y0; dirty.x1 = x1; dirty.y1 = y1; dirty.valid = true; return; }
    if (x0 < dirty.x0) dirty.x0 = x0;
    if (y0 < dirty.y0) dirty.y0 = y0;
    if (x1 > dirty.x1) dirty.x1 = x1;
    if (y1 > dirty.y1) dirty.y1 = y1;
}

static bool dirty_hit(int x, int y, int w, int h)
{
    if (!dirty.valid || w <= 0 || h <= 0) return false;
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    if (x + w > VRAM_W) return dirty_hit(x, y, VRAM_W - x, h) || dirty_hit(0, y, x + w - VRAM_W, h);
    if (y + h > VRAM_H) return dirty_hit(x, y, w, VRAM_H - y) || dirty_hit(x, 0, w, y + h - VRAM_H);
    return x < dirty.x1 && x + w > dirty.x0 && y < dirty.y1 && y + h > dirty.y0;
}

/* ------------------------------------------------------------------ */
/* Vertex batching                                                      */
/* ------------------------------------------------------------------ */
/* tp = (texpage x, texpage y, depth, raw), cl = (clut x, clut y, dither, 0): per-vertex copies of
 * per-primitive state, so primitives with different texpage/CLUT/dither share one draw call. */
typedef struct { float x, y; uint8_t r, g, b, a; float u, v; float tp[4]; float cl[4]; } GVert;   /* 52 bytes */
#define MAXV 6144
static GVert vb[MAXV];
static int vn;

typedef struct {
    int prim, tex, pass, blend_on, blend, setmask, chk;
    int twin, twax, tway, twox, twoy;
    int scl, sct, scr, scb;
} Key;
static Key ck = { .prim = -1 };

/* Shadow of the GL state flush_batch() relies on. Everything else that touches GL (fills, copies,
 * uploads, readbacks, present) calls gl_invalidate() when done, so the next flush re-sends it. */
static struct { GLuint prog; bool tex_ok, target_ok, attribs_vb, sc_ok; int blend; int sc[4]; } gs = { .blend = -2 };

static void gl_invalidate(void)
{
    memset(&gs, 0, sizeof gs);
    gs.blend = -2;
}

static void bind_vram_target(void)
{
    if (gs.target_ok) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
    glViewport(0, 0, VRAM_W, VRAM_H);
    glEnable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    gs.target_ok = true;
}

static inline void u1f(Prog *P, int slot, GLint loc, float v)
{
    if (((P->cmask >> slot) & 1u) && P->cv[slot] == v) return;
    P->cv[slot] = v; P->cmask |= 1u << slot;
    glUniform1f(loc, v);
}

static inline void u2f(Prog *P, int slot, GLint loc, float a, float b)
{
    if (((P->cmask >> slot) & 3u) == 3u && P->cv[slot] == a && P->cv[slot + 1] == b) return;
    P->cv[slot] = a; P->cv[slot + 1] = b; P->cmask |= 3u << slot;
    glUniform2f(loc, a, b);
}

static void set_attribs(const GVert *p)
{
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1); glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3); glEnableVertexAttribArray(4);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GVert), &p->x);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GVert), &p->r);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GVert), &p->u);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GVert), p->tp);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(GVert), p->cl);
}

static void flush_batch(void)
{
    if (vn == 0) return;
    const Key *k = &ck;
    Prog *P = k->tex ? &pt : &pf;
    const bool timed = (st_draws & 15u) == 0;          /* sample 1 in 16 flushes for the CPU-time estimate */
    uint64_t t0 = timed ? plat_time_ns() : 0;

    bind_vram_target();
    if (gs.prog != P->id) { glUseProgram(P->id); gs.prog = P->id; }
    if (!gs.tex_ok) {
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex_dith);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_b);
        gs.tex_ok = true;
    }
    u1f(P, 0, P->setmask, k->setmask ? 1.0f : 0.0f);
    u1f(P, 1, P->chk, k->chk ? 1.0f : 0.0f);
    if (k->tex) {
        u1f(P, 2, P->pass, (float)k->pass);
        u1f(P, 3, P->twin, k->twin ? 1.0f : 0.0f);
        u2f(P, 4, P->twand, (float)k->twax, (float)k->tway);
        u2f(P, 6, P->twor, (float)k->twox, (float)k->twoy);
    }
    int sc[4] = { k->scl, k->sct, k->scr - k->scl + 1, k->scb - k->sct + 1 };
    if (!gs.sc_ok || memcmp(gs.sc, sc, sizeof sc) != 0) {
        glScissor(sc[0], sc[1], sc[2], sc[3]);
        memcpy(gs.sc, sc, sizeof sc); gs.sc_ok = true;
    }
    int wb = k->blend_on ? (k->blend & 3) : -1;
    if (gs.blend != wb) {
        if (wb < 0) glDisable(GL_BLEND);
        else {
            glEnable(GL_BLEND);
            switch (wb) {
            case 0: glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
                    glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_ONE, GL_ZERO);
                    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD); break;
            case 1: glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
                    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD); break;
            case 2: glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
                    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD); break;
            default: glBlendColor(0.25f, 0.25f, 0.25f, 0.25f);
                    glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE, GL_ONE, GL_ZERO);
                    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD); break;
            }
        }
        gs.blend = wb;
    }
    if (!gs.attribs_vb) { set_attribs(vb); gs.attribs_vb = true; }
    glDrawArrays(k->prim ? GL_LINES : GL_TRIANGLES, 0, vn);
    st_draws++; st_tris += (unsigned)(vn / 3);
    vn = 0;
    if (!first_draw_logged) {
        first_draw_logged = true;
        GLenum e = glGetError();
        plat_log("[GPU] first draw call submitted to GLES2 (glGetError=0x%x)", (unsigned)e);
    }
    if (timed) st_flush_ns += (plat_time_ns() - t0) * 16u;
}

static void sync_snapshot(void)
{
    flush_batch();
    if (!dirty.valid) return;
    int x0 = imax(dirty.x0, 0), y0 = imax(dirty.y0, 0), x1 = imin(dirty.x1, VRAM_W), y1 = imin(dirty.y1, VRAM_H);
    dirty.valid = false;
    if (x1 <= x0 || y1 <= y0) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_a);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_b);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, x0, y0, x1 - x0, y1 - y0);
    st_syncs++;
    gl_invalidate();
}

/* Queue n vertices. tex/semi/raw/dither describe the primitive, prim 0=triangles 1=lines. */
static void submit(const GVert *v, int n, int prim, bool tex, bool semi, bool raw, bool dither, int clut)
{
    int l = imax(draw_left, 0), t = imax(draw_top, 0);
    int r = imin(draw_right, VRAM_W - 1), b = imin(draw_bottom, VRAM_H - 1);
    if (r < l || b < t) return;

    float fx0 = v[0].x, fx1 = v[0].x, fy0 = v[0].y, fy1 = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].x < fx0) fx0 = v[i].x;
        if (v[i].x > fx1) fx1 = v[i].x;
        if (v[i].y < fy0) fy0 = v[i].y;
        if (v[i].y > fy1) fy1 = v[i].y;
    }
    int bx0 = imax((int)fx0 - 1, l), by0 = imax((int)fy0 - 1, t);
    int bx1 = imin((int)fx1 + 2, r + 1), by1 = imin((int)fy1 + 2, b + 1);
    if (bx1 <= bx0 || by1 <= by0) return;       /* entirely outside the drawing area */

    int depth = tp_depth > 2 ? 2 : tp_depth;
    int clutx = (clut & 0x3F) * 16, cluty = (clut >> 6) & 0x1FF;
    int npass = (tex && semi) ? 2 : 1;
    for (int pi = 0; pi < npass; pi++) {
        Key k; memset(&k, 0, sizeof k);
        k.prim = prim; k.tex = tex ? 1 : 0;
        k.pass = (tex && semi) ? pi + 1 : 0;
        k.blend_on = semi && (!tex || pi == 1);
        k.blend = k.blend_on ? (blend_mode & 3) : 0;
        k.setmask = set_mask; k.chk = check_mask;
        k.scl = l; k.sct = t; k.scr = r; k.scb = b;
        if (tex) {
            int ua = (~(tw_mask_x * 8)) & 0xFF, uo = ((tw_off_x & tw_mask_x) * 8) & 0xFF;
            int va = (~(tw_mask_y * 8)) & 0xFF, vo = ((tw_off_y & tw_mask_y) * 8) & 0xFF;
            k.twin = (tw_mask_x || tw_mask_y);
            k.twax = ua; k.tway = va; k.twox = uo; k.twoy = vo;
            int wid = depth == 0 ? 64 : depth == 1 ? 128 : 256;
            int cw = depth == 0 ? 16 : depth == 1 ? 256 : 0;
            if (dirty_hit(tp_x, tp_y, wid, 256) || dirty_hit(clutx, cluty, cw, 1)) sync_snapshot();
        }
        if (check_mask && dirty_hit(bx0, by0, bx1 - bx0, by1 - by0)) sync_snapshot();

        if (vn + n > MAXV) { st_fr[FR_CAP]++; flush_batch(); ck = k; }
        else if (memcmp(&k, &ck, sizeof k) != 0) {
            if (vn > 0) {
                if (ck.prim != k.prim) st_fr[FR_PRIM]++;
                else if (ck.tex != k.tex) st_fr[FR_TEX]++;
                else if (ck.pass != k.pass) st_fr[FR_PASS]++;
                else if (ck.blend_on != k.blend_on || ck.blend != k.blend) st_fr[FR_BLEND]++;
                else if (ck.twin != k.twin || ck.twax != k.twax || ck.tway != k.tway || ck.twox != k.twox || ck.twoy != k.twoy) st_fr[FR_TW]++;
                else if (ck.scl != k.scl || ck.sct != k.sct || ck.scr != k.scr || ck.scb != k.scb) st_fr[FR_SCISSOR]++;
                else st_fr[FR_MASK]++;
            }
            flush_batch(); ck = k;
        }
        GVert *dv = vb + vn;
        memcpy(dv, v, (size_t)n * sizeof(GVert));
        for (int i = 0; i < n; i++) {
            dv[i].tp[0] = tex ? (float)tp_x : 0.0f;  dv[i].tp[1] = tex ? (float)tp_y : 0.0f;
            dv[i].tp[2] = (float)depth;              dv[i].tp[3] = raw ? 1.0f : 0.0f;
            dv[i].cl[0] = (float)clutx;              dv[i].cl[1] = (float)cluty;
            dv[i].cl[2] = dither ? 1.0f : 0.0f;      dv[i].cl[3] = 0.0f;
        }
        vn += n;
        dirty_add(bx0, by0, bx1, by1);
    }
}

/* ------------------------------------------------------------------ */
/* Register / helpers (GP0/GP1 handling)                        */
/* ------------------------------------------------------------------ */
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
/* Primitives                                                           */
/* ------------------------------------------------------------------ */
typedef struct { int x, y, r, g, b, u, v; } Vert;

static inline int imax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }
static inline int imin3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }

static inline GVert mkv(const Vert *p)
{
    GVert o;
    memset(&o, 0, sizeof o);
    o.x = (float)p->x + 0.5f; o.y = (float)p->y + 0.5f;     /* +0.5: PSX tests pixel corners, GL tests centers */
    o.r = (uint8_t)p->r; o.g = (uint8_t)p->g; o.b = (uint8_t)p->b; o.a = 255;
    o.u = (float)p->u; o.v = (float)p->v;
    return o;
}

static void poly_tri(const Vert *a, const Vert *b, const Vert *c, bool tex, bool gouraud, bool semi, bool raw, int clut)
{
    int span_x = imax3(a->x, b->x, c->x) - imin3(a->x, b->x, c->x);
    int span_y = imax3(a->y, b->y, c->y) - imin3(a->y, b->y, c->y);
    if (span_x > 1023 || span_y > 511) return;
    int32_t area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    bool dth = tex ? (dither_on && !raw) : (dither_on && gouraud);
    GVert g[3] = { mkv(a), mkv(b), mkv(c) };
    submit(g, 3, 0, tex, semi, raw, dth, clut);
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
    poly_tri(&v[0], &v[1], &v[2], tex, gouraud, semi, raw, clut);
    if (quad) poly_tri(&v[1], &v[2], &v[3], tex, gouraud, semi, raw, clut);
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
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    if (w <= 0 || h <= 0) return;
    Vert a = { x,     y,     cr, cg, cb, u0,     v0 };
    Vert b = { x + w, y,     cr, cg, cb, u0 + w, v0 };
    Vert c = { x,     y + h, cr, cg, cb, u0,     v0 + h };
    Vert d = { x + w, y + h, cr, cg, cb, u0 + w, v0 + h };
    GVert g[6] = { mkv(&a), mkv(&b), mkv(&c), mkv(&b), mkv(&d), mkv(&c) };
    submit(g, 6, 0, tex, semi, raw, false, clut);
}

static void line_segment(int x0, int y0, int r0, int g0, int b0, int x1, int y1, int r1, int g1, int b1, bool semi, bool gouraud)
{
    (void)gouraud;
    x0 += draw_ofs_x; y0 += draw_ofs_y; x1 += draw_ofs_x; y1 += draw_ofs_y;
    Vert a = { x0, y0, r0, g0, b0, 0, 0 }, b = { x1, y1, r1, g1, b1, 0, 0 };
    if (x0 == x1 && y0 == y1) {           /* single pixel */
        Vert c = { x0 + 1, y0, r0, g0, b0, 0, 0 }, d = { x0, y0 + 1, r0, g0, b0, 0, 0 }, e = { x0 + 1, y0 + 1, r0, g0, b0, 0, 0 };
        GVert q[6] = { mkv(&a), mkv(&c), mkv(&d), mkv(&c), mkv(&e), mkv(&d) };
        submit(q, 6, 0, false, semi, false, dither_on, 0);
        return;
    }
    GVert l[2] = { mkv(&a), mkv(&b) };
    submit(l, 2, 1, false, semi, false, dither_on, 0);
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
/* VRAM transfers                                                       */
/* ------------------------------------------------------------------ */
static void fill_rect(void)
{
    flush_batch();
    int cr = (int)(fifo[0] & 0xFF) >> 3, cg = (int)((fifo[0] >> 8) & 0xFF) >> 3, cb = (int)((fifo[0] >> 16) & 0xFF) >> 3;
    int x = (int)(fifo[1] & 0x3F0), y = (int)((fifo[1] >> 16) & 0x1FF);
    int w = (int)(((fifo[2] & 0x3FF) + 0xF) & ~0xF), h = (int)((fifo[2] >> 16) & 0x1FF);
    if (w <= 0 || h <= 0) return;
    bind_vram_target();
    glDisable(GL_BLEND);
    glClearColor((float)(cr * 8) / 255.0f, (float)(cg * 8) / 255.0f, (float)(cb * 8) / 255.0f, 0.0f);
    for (int oy = 0; oy < h; ) {
        int py = (y + oy) & (VRAM_H - 1), ph = imin(h - oy, VRAM_H - py);
        for (int ox = 0; ox < w; ) {
            int px = (x + ox) & (VRAM_W - 1), pw = imin(w - ox, VRAM_W - px);
            glScissor(px, py, pw, ph);
            glClear(GL_COLOR_BUFFER_BIT);
            dirty_add(px, py, px + pw, py + ph);
            ox += pw;
        }
        oy += ph;
    }
    st_fills++;
    gl_invalidate();
}

static void copy_vram(void)
{
    int sx = (int)(fifo[1] & 0x3FF), sy = (int)((fifo[1] >> 16) & 0x1FF);
    int dx = (int)(fifo[2] & 0x3FF), dy = (int)((fifo[2] >> 16) & 0x1FF);
    int w = (int)(fifo[3] & 0x3FF); if (!w) w = 0x400;
    int h = (int)((fifo[3] >> 16) & 0x1FF); if (!h) h = 0x200;
    flush_batch();
    if (dirty_hit(sx, sy, w, h) || (check_mask && dirty_hit(dx, dy, w, h))) sync_snapshot();

    static const GVert fq[6] = {
        { 0, 0, 255, 255, 255, 255, 0, 0 }, { VRAM_W, 0, 255, 255, 255, 255, 0, 0 }, { 0, VRAM_H, 255, 255, 255, 255, 0, 0 },
        { VRAM_W, 0, 255, 255, 255, 255, 0, 0 }, { VRAM_W, VRAM_H, 255, 255, 255, 255, 0, 0 }, { 0, VRAM_H, 255, 255, 255, 255, 0, 0 },
    };
    bind_vram_target();
    glDisable(GL_BLEND);
    glUseProgram(pc.id);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_b);
    glUniform1i(pc.vram, 0);
    glUniform1f(pc.setmask, set_mask ? 1.0f : 0.0f);
    glUniform1f(pc.chk, check_mask ? 1.0f : 0.0f);
    set_attribs(fq);
    for (int oy = 0; oy < h; ) {
        int py = (dy + oy) & (VRAM_H - 1), ph = imin(h - oy, VRAM_H - py);
        for (int ox = 0; ox < w; ) {
            int px = (dx + ox) & (VRAM_W - 1), pw = imin(w - ox, VRAM_W - px);
            glUniform2f(pc.off, (float)((sx + ox) - px), (float)((sy + oy) - py));
            glScissor(px, py, pw, ph);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            dirty_add(px, py, px + pw, py + ph);
            ox += pw;
        }
        oy += ph;
    }
    st_copies++;
    gl_invalidate();
}

/* read tex_a region (with wrap) into out as RGBA bytes, w*h*4 */
static void read_rect_rgba(int x, int y, int w, int h, uint8_t *out)
{
    flush_batch();
    bind_vram_target();
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    for (int oy = 0; oy < h; ) {
        int py = (y + oy) & (VRAM_H - 1), ph = imin(h - oy, VRAM_H - py);
        for (int ox = 0; ox < w; ) {
            int px = (x + ox) & (VRAM_W - 1), pw = imin(w - ox, VRAM_W - px);
            glReadPixels(px, py, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, scr_c);
            for (int r = 0; r < ph; r++)
                memcpy(out + (((size_t)(oy + r) * w) + ox) * 4, scr_c + (size_t)r * pw * 4, (size_t)pw * 4);
            ox += pw;
        }
        oy += ph;
    }
    st_reads++;
    gl_invalidate();
}

static void upload_piece(int x, int y, int w, int h, const void *px)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_a);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, tex_b);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gl_invalidate();
}

/* CPU -> VRAM (written into both the render target and the snapshot, so no sync is needed) */
static void upload_words(int x, int y, int w, int h, const uint16_t *src)
{
    flush_batch();
    size_t n = (size_t)w * h;
    if (check_mask) read_rect_rgba(x, y, w, h, scr_b);
    for (size_t i = 0; i < n; i++) {
        uint16_t p = src[i];
        uint8_t *d = scr_a + i * 4;
        if (check_mask && scr_b[i * 4 + 3] >= 128) { memcpy(d, scr_b + i * 4, 4); continue; }
        d[0] = (uint8_t)((p & 31) << 3); d[1] = (uint8_t)(((p >> 5) & 31) << 3); d[2] = (uint8_t)(((p >> 10) & 31) << 3);
        d[3] = (set_mask || (p & 0x8000)) ? 255 : 0;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    x &= VRAM_W - 1; y &= VRAM_H - 1;
    for (int oy = 0; oy < h; ) {
        int py = (y + oy) & (VRAM_H - 1), ph = imin(h - oy, VRAM_H - py);
        for (int ox = 0; ox < w; ) {
            int px = (x + ox) & (VRAM_W - 1), pw = imin(w - ox, VRAM_W - px);
            if (pw == w && ph == h) upload_piece(px, py, pw, ph, scr_a);
            else {
                for (int r = 0; r < ph; r++)
                    memcpy(scr_c + (size_t)r * pw * 4, scr_a + (((size_t)(oy + r) * w) + ox) * 4, (size_t)pw * 4);
                upload_piece(px, py, pw, ph, scr_c);
            }
            ox += pw;
        }
        oy += ph;
    }
    st_uploads++;
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
    load_buf[load_px] = value;
    if (++load_px >= load_w * load_h) {
        load_active = false;
        upload_words(load_x, load_y, load_w, load_h, load_buf);
    }
}

static void begin_image_read(void)
{
    read_x = (int)(fifo[1] & 0x3FF); read_y = (int)((fifo[1] >> 16) & 0x1FF);
    read_w = (int)(fifo[2] & 0x3FF); if (!read_w) read_w = 0x400;
    read_h = (int)((fifo[2] >> 16) & 0x1FF); if (!read_h) read_h = 0x200;
    read_px = 0; read_active = true;
    read_rect_rgba(read_x, read_y, read_w, read_h, scr_a);
    size_t n = (size_t)read_w * read_h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *s = scr_a + i * 4;
        g_vram[i] = (uint16_t)((s[0] >> 3) | ((s[1] >> 3) << 5) | ((s[2] >> 3) << 10) | (s[3] >= 128 ? 0x8000 : 0));
    }
}

static uint16_t read_image_halfword(void)
{
    if (!read_active) return 0;
    uint16_t v = g_vram[read_px];
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
/* GP0 / GP1                                                            */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* Presentation (called by plat_present on the emulation thread)        */
/* ------------------------------------------------------------------ */
void gpu_present(int dx, int dy, int dw, int dh, bool rgb24, bool enabled)
{
    uint64_t t_begin = plat_time_ns();
    flush_batch();
    int fbo = 0, sw = 0, sh = 0;
    plat_gl_screen(&fbo, &sw, &sh);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(0, 0, sw, sh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    if (enabled && dw > 0 && dh > 0 && sw > 0 && sh > 0) {
        if (dw > VRAM_W) dw = VRAM_W;
        if (dh > VRAM_H) dh = VRAM_H;
        dx &= VRAM_W - 1; dy &= VRAM_H - 1;
        float vw = (float)sw, vh = (float)sh, fx, fy;          /* fit 4:3 inside the view */
        if (vw / vh > 4.0f / 3.0f) { fy = 1.0f; fx = (vh * 4.0f / 3.0f) / vw; }
        else { fx = 1.0f; fy = (vw * 3.0f / 4.0f) / vh; }
        float quad[16] = { -fx, -fy, 0, 1,   fx, -fy, 1, 1,   -fx, fy, 0, 0,   fx, fy, 1, 0 };
        glUseProgram(pd.id);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_a);
        glUniform1i(pd.vram, 0);
        glUniform4f(pd.rect, (float)dx, (float)dy, (float)dw, (float)dh);
        glUniform1f(pd.rgb24, rgb24 ? 1.0f : 0.0f);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnableVertexAttribArray(0); glDisableVertexAttribArray(1); glEnableVertexAttribArray(2);
        glDisableVertexAttribArray(3); glDisableVertexAttribArray(4);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(2);
    }

    uint64_t t_end = plat_time_ns();
    st_present_ns += t_end - t_begin;
    gl_invalidate();

    st_frames++;
    unsigned work = st_draws + st_fills + st_uploads + st_copies;
    if (work != st_work_last) { st_new_frames++; st_work_last = work; }
    if (st_frames == 1) st_last_ns = t_end;
    if (st_frames == 1 || (st_frames % 600) == 0) {
        unsigned win = st_frames == 1 ? 1 : 600;
        double secs = (double)(t_end - st_last_ns) / 1e9;
        double vs = (secs > 0.0 && st_frames > 1) ? win / secs : 0.0;
        double nw = (secs > 0.0 && st_frames > 1) ? (st_new_frames - st_new_last) / secs : 0.0;
        double per = 1.0 / (double)win;
        plat_log("[GPU] frame %u: vsync/s=%.1f (%.0f%% of 60) new-frames/s=%.1f | draws/f=%.1f tris/f=%.0f | cpu-in-GL ms/f: flush~%.2f present~%.2f (period %.1f)",
                 st_frames, vs, vs * (100.0 / 60.0), nw,
                 (st_draws - st_last_draws) * per, (st_tris - st_last_tris) * per,
                 (double)(st_flush_ns - st_flush_ns_last) * 1e-6 * per, (double)(st_present_ns - st_present_ns_last) * 1e-6 * per,
                 vs > 0.0 ? 1000.0 / vs : 0.0);
        plat_log("[GPU]   flush causes /f: tex=%.1f pass=%.1f blend=%.1f prim=%.1f tw=%.1f scissor=%.1f mask=%.1f cap=%.1f | totals draws=%u tris=%u syncs=%u uploads=%u reads=%u fills=%u copies=%u",
                 (st_fr[FR_TEX] - st_fr_last[FR_TEX]) * per, (st_fr[FR_PASS] - st_fr_last[FR_PASS]) * per,
                 (st_fr[FR_BLEND] - st_fr_last[FR_BLEND]) * per, (st_fr[FR_PRIM] - st_fr_last[FR_PRIM]) * per,
                 (st_fr[FR_TW] - st_fr_last[FR_TW]) * per, (st_fr[FR_SCISSOR] - st_fr_last[FR_SCISSOR]) * per,
                 (st_fr[FR_MASK] - st_fr_last[FR_MASK]) * per, (st_fr[FR_CAP] - st_fr_last[FR_CAP]) * per,
                 st_draws, st_tris, st_syncs, st_uploads, st_reads, st_fills, st_copies);
        memcpy(st_fr_last, st_fr, sizeof st_fr);
        st_last_draws = st_draws; st_last_tris = st_tris; st_last_ns = t_end;
        st_flush_ns_last = st_flush_ns; st_present_ns_last = st_present_ns; st_new_last = st_new_frames;
    }
}
