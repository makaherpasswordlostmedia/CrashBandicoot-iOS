/* mdec.c - Hardware/Mdec.cs ported to C (integer IDCT is bit-exact with the C# one;
 * the YUV->RGB step uses fixed point instead of double). */
#include "psx.h"

static const int ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
static const int16_t DEFAULT_SCALE[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, (int16_t)0xE707, (int16_t)0xB8E3, (int16_t)0x9592, (int16_t)0x8275,
    0x7641, 0x30FB, (int16_t)0xCF04, (int16_t)0x89BE, (int16_t)0x89BE, (int16_t)0xCF04, 0x30FB, 0x7641,
    0x6A6D, (int16_t)0xE707, (int16_t)0x8275, (int16_t)0xB8E3, 0x471C, 0x7D8A, 0x18F8, (int16_t)0x9592,
    0x5A82, (int16_t)0xA57D, (int16_t)0xA57D, 0x5A82, 0x5A82, (int16_t)0xA57D, (int16_t)0xA57D, 0x5A82,
    0x471C, (int16_t)0x8275, 0x18F8, 0x6A6D, (int16_t)0x9592, (int16_t)0xE707, 0x7D8A, (int16_t)0xB8E3,
    0x30FB, (int16_t)0x89BE, 0x7641, (int16_t)0xCF04, (int16_t)0xCF04, 0x7641, (int16_t)0x89BE, 0x30FB,
    0x18F8, (int16_t)0xB8E3, 0x6A6D, (int16_t)0x8275, 0x7D8A, (int16_t)0x9592, 0x471C, (int16_t)0xE707,
};

enum { M_IDLE, M_DECODE, M_QUANT, M_SCALE };

static uint8_t quant_luma[64], quant_chroma[64];
static int16_t scale_[64];
static int depth; static bool is_signed, bit15, enable_in, enable_out;
static int mode = M_IDLE, params_remaining = -1;
static uint16_t *in_hw; static int in_n, in_cap;
static uint8_t table_bytes[128]; static int table_n;
static bool quant_color;
static uint32_t *out_q; static int out_head, out_n, out_cap;
static int read_pos;

void mdec_init(void) { memcpy(scale_, DEFAULT_SCALE, sizeof scale_); }

static void out_push(uint32_t w)
{
    if (out_head > 0 && out_head == out_n) { out_head = out_n = 0; }
    if (out_n == out_cap) { out_cap = out_cap ? out_cap * 2 : 4096; out_q = (uint32_t *)realloc(out_q, (size_t)out_cap * 4); }
    out_q[out_n++] = w;
}
static int out_count(void) { return out_n - out_head; }

uint32_t mdec_read_status(void)
{
    uint32_t stat = 0;
    if (out_count() == 0) stat |= 1u << 31;
    if (mode != M_IDLE) stat |= 1u << 29;
    if (enable_in && params_remaining > 0) stat |= 1u << 28;
    if (enable_out && out_count() > 0) stat |= 1u << 27;
    stat |= (uint32_t)(depth & 3) << 25;
    if (is_signed) stat |= 1u << 24;
    if (bit15) stat |= 1u << 23;
    uint32_t remaining = params_remaining > 0 ? (uint32_t)(params_remaining - 1) : 0xFFFFu;
    stat |= remaining & 0xFFFFu;
    return stat;
}

uint32_t mdec_read_data(void) { return out_count() > 0 ? out_q[out_head++] : 0u; }

void mdec_write_control(uint32_t value)
{
    if (value & (1u << 31)) {
        mode = M_IDLE; params_remaining = -1; in_n = 0; table_n = 0; out_head = out_n = 0;
        enable_in = enable_out = false;
        return;
    }
    enable_in = (value & (1u << 30)) != 0;
    enable_out = (value & (1u << 29)) != 0;
}

static int signed10(uint16_t n) { int v = n & 0x3FF; return (v & 0x200) ? v - 0x400 : v; }
static uint16_t next_hw(void) { return read_pos < in_n ? in_hw[read_pos++] : 0xFE00; }

static void idct_core(int *blk)
{
    int64_t tmp[64];
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++) sum += (int64_t)blk[y + z * 8] * (scale_[x + z * 8] / 8);
                tmp[x + y * 8] = (sum + 0xFFF) / 0x2000;
            }
        for (int i = 0; i < 64; i++) blk[i] = (int)tmp[i];
    }
}

static bool decode_block(const uint8_t *qt, int *block)
{
    memset(block, 0, 64 * sizeof(int));
    uint16_t n = next_hw();
    while (n == 0xFE00 && read_pos < in_n) n = next_hw();
    if (n == 0xFE00) return false;
    int qscale = (n >> 10) & 0x3F;
    int val = signed10(n) * qt[0];
    int k = 0;
    for (;;) {
        if (qscale == 0) val = signed10(n) * 2;
        val = clampi(val, -0x400, 0x3FF);
        block[ZIGZAG[k]] = val;
        n = next_hw();
        if (n == 0xFE00) break;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (signed10(n) * qt[k] * qscale + 4) / 8;
    }
    idct_core(block);
    return true;
}

static void yuv_to_rgb(const int *cr, const int *cb, const int *y, int xx, int yy, uint8_t *dst)
{
    for (int py = 0; py < 8; py++)
        for (int px = 0; px < 8; px++) {
            int c = ((px + xx) / 2) + ((py + yy) / 2) * 8;
            /* 1.402, -0.3437, -0.7143, 1.772 in 16.16 fixed point, rounded */
            int r = (91881 * cr[c] + 32768) >> 16;
            int b = (116130 * cb[c] + 32768) >> 16;
            int g = (-22525 * cb[c] - 46812 * cr[c] + 32768) >> 16;
            int yv = y[px + py * 8];
            int ri = clampi(yv + r, -128, 127), gi = clampi(yv + g, -128, 127), bi = clampi(yv + b, -128, 127);
            if (!is_signed) { ri ^= 0x80; gi ^= 0x80; bi ^= 0x80; }
            int o = ((px + xx) + (py + yy) * 16) * 4;
            dst[o] = (uint8_t)ri; dst[o + 1] = (uint8_t)gi; dst[o + 2] = (uint8_t)bi;
        }
}

static void pack_bytes(const uint8_t *bytes, int n)
{
    for (int i = 0; i < n; i += 4)
        out_push((uint32_t)(bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) | ((uint32_t)bytes[i + 3] << 24)));
}

static void push_color(const uint8_t *rgb)
{
    if (depth == 3) {
        uint16_t px[256];
        for (int i = 0; i < 256; i++) {
            int r = rgb[i * 4] >> 3, g = rgb[i * 4 + 1] >> 3, b = rgb[i * 4 + 2] >> 3;
            uint16_t v = (uint16_t)(r | (g << 5) | (b << 10));
            if (bit15) v |= 0x8000;
            px[i] = v;
        }
        for (int i = 0; i < 256; i += 2) out_push((uint32_t)(px[i] | ((uint32_t)px[i + 1] << 16)));
    } else {
        uint8_t bytes[256 * 3];
        for (int i = 0; i < 256; i++) { bytes[i * 3] = rgb[i * 4]; bytes[i * 3 + 1] = rgb[i * 4 + 1]; bytes[i * 3 + 2] = rgb[i * 4 + 2]; }
        pack_bytes(bytes, 256 * 3);
    }
}

static void push_mono(const int *y)
{
    uint8_t bytes[64]; int nb = depth == 0 ? 32 : 64;
    memset(bytes, 0, sizeof bytes);
    for (int i = 0; i < 64; i++) {
        int v = clampi(y[i], -128, 127);
        if (!is_signed) v ^= 0x80;
        if (depth == 1) bytes[i] = (uint8_t)v;
        else {
            int nib = (v & 0xFF) >> 4;
            if ((i & 1) == 0) bytes[i / 2] = (uint8_t)nib; else bytes[i / 2] |= (uint8_t)(nib << 4);
        }
    }
    pack_bytes(bytes, nb);
}

static void decode_all(void)
{
    read_pos = 0;
    bool color = depth >= 2;
    int cr_b[64], cb_b[64], y_b[64];
    uint8_t rgb[16 * 16 * 4];
    while (read_pos < in_n) {
        if (color) {
            if (!decode_block(quant_chroma, cr_b)) break;
            decode_block(quant_chroma, cb_b);
            for (int q = 0; q < 4; q++) {
                decode_block(quant_luma, y_b);
                yuv_to_rgb(cr_b, cb_b, y_b, (q & 1) * 8, (q >> 1) * 8, rgb);
            }
            push_color(rgb);
        } else {
            if (!decode_block(quant_luma, y_b)) break;
            push_mono(y_b);
        }
    }
}

static void begin_command(uint32_t word)
{
    uint32_t cmd = (word >> 29) & 7;
    switch (cmd) {
    case 1:
        depth = (int)((word >> 27) & 3); is_signed = (word & (1u << 26)) != 0; bit15 = (word & (1u << 25)) != 0;
        params_remaining = (int)(word & 0xFFFF); in_n = 0; mode = M_DECODE;
        break;
    case 2: quant_color = (word & 1) != 0; params_remaining = quant_color ? 32 : 16; table_n = 0; mode = M_QUANT; break;
    case 3: params_remaining = 32; table_n = 0; mode = M_SCALE; break;
    default:
        depth = (int)((word >> 27) & 3); is_signed = (word & (1u << 26)) != 0; bit15 = (word & (1u << 25)) != 0;
        break;
    }
}

void mdec_write0(uint32_t word)
{
    if (mode == M_IDLE) { begin_command(word); return; }
    if (mode == M_DECODE) {
        if (in_n + 2 > in_cap) { in_cap = in_cap ? in_cap * 2 : 8192; in_hw = (uint16_t *)realloc(in_hw, (size_t)in_cap * 2); }
        in_hw[in_n++] = (uint16_t)word; in_hw[in_n++] = (uint16_t)(word >> 16);
    } else if (table_n + 4 <= (int)sizeof table_bytes) {
        table_bytes[table_n++] = (uint8_t)word; table_bytes[table_n++] = (uint8_t)(word >> 8);
        table_bytes[table_n++] = (uint8_t)(word >> 16); table_bytes[table_n++] = (uint8_t)(word >> 24);
    }
    if (--params_remaining > 0) return;
    if (mode == M_DECODE) decode_all();
    else if (mode == M_QUANT) {
        memcpy(quant_luma, table_bytes, 64);
        if (quant_color) memcpy(quant_chroma, table_bytes + 64, 64);
    } else if (mode == M_SCALE) {
        for (int i = 0; i < 64; i++) scale_[i] = (int16_t)(table_bytes[i * 2] | (table_bytes[i * 2 + 1] << 8));
    }
    mode = M_IDLE; params_remaining = -1;
}
