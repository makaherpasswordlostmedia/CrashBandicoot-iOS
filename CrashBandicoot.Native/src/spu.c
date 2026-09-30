/* spu.c - Spu.cs + XaAudio.cs ported to C. spu_mix() runs on the audio
 * callback thread, register writes on the emulation thread -> one mutex. */
#include "psx.h"
#include "spu_tables.h"
#include <pthread.h>

#define SPU_BASE 0x1F801C00u

enum { PH_OFF, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

typedef struct {
    uint16_t vol_l, vol_r, pitch, start_addr, repeat_addr, adsr_lo, adsr_hi;
    int16_t  adsr_vol;
    uint32_t cur_addr, pitch_counter;
    int      phase, adsr_cycle;
    bool     end_x, ignore_loop, has_block;
    int      old, older;
    int16_t  cur_vol_l, cur_vol_r;
    int      vol_cyc_l, vol_cyc_r;
    int16_t  buf[31];
} Voice;

static uint8_t *spu_ram;
static Voice V[24];
static pthread_mutex_t g_spu_mx = PTHREAD_MUTEX_INITIALIZER;
static uint16_t main_vol_l, main_vol_r; static int16_t main_cur_l, main_cur_r; static int main_cyc_l, main_cyc_r;
static uint16_t reverb_vol_l, reverb_vol_r;
static uint16_t kon, kon_hi, koff, koff_hi, pmon, pmon_hi, non_, non_hi, eon, eon_hi;
static uint32_t endx;
static uint16_t spucnt, transfer_addr, transfer_ctrl = 4;
static uint16_t cd_vol_l, cd_vol_r, ext_vol_l, ext_vol_r, reverb_start;
static int noise_level = 1, noise_timer;
static float g_master = 1.0f;

static const int K0[5] = { 0, 60, 115, 98, 122 };
static const int K1[5] = { 0, 0, -52, -55, -60 };
static const int ADSR_DOWN[4] = { -8, -7, -6, -5 };
static const int ADSR_UP[4] = { 7, 6, 5, 4 };

void spu_init(void) { spu_ram = (uint8_t *)calloc(1, SPU_RAM_SIZE); }
void spu_set_master_volume(float v) { g_master = v; }
uint32_t spu_transfer_addr_bytes(void) { return (uint32_t)transfer_addr << 3; }

void spu_dma_write(uint32_t a, const uint8_t *data, uint32_t n)
{
    pthread_mutex_lock(&g_spu_mx);
    for (uint32_t i = 0; i < n; i++) spu_ram[(a + i) & (SPU_RAM_SIZE - 1)] = data[i];
    pthread_mutex_unlock(&g_spu_mx);
}

static uint16_t read_reg(uint32_t phys)
{
    uint32_t off = phys - SPU_BASE;
    if (off < 0x180u) {
        Voice *v = &V[off >> 4];
        switch (off & 0xF) {
        case 0x0: return v->vol_l; case 0x2: return v->vol_r; case 0x4: return v->pitch; case 0x6: return v->start_addr;
        case 0x8: return v->adsr_lo; case 0xA: return v->adsr_hi;
        case 0xC: return (v->phase == PH_ATTACK && v->adsr_vol == 0) ? 1 : (uint16_t)v->adsr_vol;
        case 0xE: return v->repeat_addr;
        default: return 0;
        }
    }
    switch (off) {
    case 0x180: return main_vol_l; case 0x182: return main_vol_r; case 0x184: return reverb_vol_l; case 0x186: return reverb_vol_r;
    case 0x188: return kon; case 0x18A: return kon_hi; case 0x18C: return koff; case 0x18E: return koff_hi;
    case 0x190: return pmon; case 0x192: return pmon_hi; case 0x194: return non_; case 0x196: return non_hi;
    case 0x198: return eon; case 0x19A: return eon_hi;
    case 0x19C: return (uint16_t)(endx & 0xFFFF); case 0x19E: return (uint16_t)(endx >> 16);
    case 0x1A2: return reverb_start; case 0x1A6: return transfer_addr; case 0x1AA: return spucnt; case 0x1AC: return transfer_ctrl;
    case 0x1AE: return spucnt & 0x3F;
    case 0x1B0: return cd_vol_l; case 0x1B2: return cd_vol_r; case 0x1B4: return ext_vol_l; case 0x1B6: return ext_vol_r;
    default: return 0;
    }
}

static void key_on(uint16_t mask, bool hi)
{
    int b = hi ? 16 : 0;
    for (int i = 0; i < (hi ? 8 : 16); i++) {
        if (!(mask & (1 << i))) continue;
        Voice *v = &V[b + i];
        v->phase = PH_ATTACK; v->adsr_vol = 0; v->adsr_cycle = 0;
        v->cur_addr = (uint32_t)v->start_addr << 3;
        v->ignore_loop = false; v->pitch_counter = 0; v->old = v->older = 0;
        v->has_block = false; v->end_x = false;
        endx &= ~(1u << (b + i));
    }
}
static void key_off(uint16_t mask, bool hi)
{
    int b = hi ? 16 : 0;
    for (int i = 0; i < (hi ? 8 : 16); i++) {
        if (!(mask & (1 << i))) continue;
        Voice *v = &V[b + i];
        if (v->phase != PH_OFF) v->phase = PH_RELEASE;
    }
}

static void write_reg(uint32_t phys, uint16_t val)
{
    uint32_t off = phys - SPU_BASE;
    if (off < 0x180u) {
        Voice *v = &V[off >> 4];
        switch (off & 0xF) {
        case 0x0: v->vol_l = val; break; case 0x2: v->vol_r = val; break; case 0x4: v->pitch = val; break;
        case 0x6: v->start_addr = val; break; case 0x8: v->adsr_lo = val; break; case 0xA: v->adsr_hi = val; break;
        case 0xC: v->adsr_vol = (int16_t)val; break;
        case 0xE: v->repeat_addr = val; v->ignore_loop = true; break;
        }
        return;
    }
    switch (off) {
    case 0x180: main_vol_l = val; break; case 0x182: main_vol_r = val; break;
    case 0x184: reverb_vol_l = val; break; case 0x186: reverb_vol_r = val; break;
    case 0x188: key_on(val, false); kon = val; break; case 0x18A: key_on(val, true); kon_hi = val; break;
    case 0x18C: key_off(val, false); koff = val; break; case 0x18E: key_off(val, true); koff_hi = val; break;
    case 0x190: pmon = val; break; case 0x192: pmon_hi = val; break;
    case 0x194: non_ = val; break; case 0x196: non_hi = val; break;
    case 0x198: eon = val; break; case 0x19A: eon_hi = val; break;
    case 0x1A2: reverb_start = val; break; case 0x1A6: transfer_addr = val; break;
    case 0x1AA: spucnt = val; break; case 0x1AC: transfer_ctrl = val; break;
    case 0x1B0: cd_vol_l = val; break; case 0x1B2: cd_vol_r = val; break;
    case 0x1B4: ext_vol_l = val; break; case 0x1B6: ext_vol_r = val; break;
    }
}

uint16_t spu_read_reg16(uint32_t phys) { pthread_mutex_lock(&g_spu_mx); uint16_t r = read_reg(phys); pthread_mutex_unlock(&g_spu_mx); return r; }
void spu_write_reg16(uint32_t phys, uint16_t v) { pthread_mutex_lock(&g_spu_mx); write_reg(phys, v); pthread_mutex_unlock(&g_spu_mx); }

/* ------------------------------------------------------------------ */
static void env_tick(int16_t *level, int *cycle, int shift, int step_idx, bool exp_, bool decrease, bool neg_phase)
{
    int mag = neg_phase ? -*level : *level;
    int cycles = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    int step = (decrease ? ADSR_DOWN : ADSR_UP)[step_idx] << (11 - shift > 0 ? 11 - shift : 0);
    if (exp_ && !decrease && mag > 0x6000) cycles *= 4;
    if (exp_ && decrease) { step = step * mag / 0x8000; if (step == 0) step = -1; }
    if (++*cycle < cycles) return;
    *cycle = 0;
    int next = clampi(mag + step, 0, 0x7FFF);
    *level = (int16_t)(neg_phase ? -next : next);
}

static void sweep_tick(uint16_t reg, int16_t *level, int *cycle)
{
    if (!(reg & 0x8000)) { *level = (int16_t)(reg << 1); return; }
    bool exp_ = (reg & 0x4000) != 0, dec = (reg & 0x2000) != 0, neg = (reg & 0x1000) != 0;
    env_tick(level, cycle, (reg >> 2) & 0x1F, reg & 3, exp_, dec, neg);
}

static int16_t decode_sample(Voice *v, int nibble, int shift, int k0, int k1)
{
    int s = (nibble << 28) >> 28;
    s = (s << 12) >> shift;
    s += (v->old * k0 + v->older * k1) >> 6;
    s = clampi(s, -32768, 32767);
    v->older = v->old; v->old = s;
    return (int16_t)s;
}

static void decode_block(Voice *v, int index)
{
    v->buf[0] = v->buf[28]; v->buf[1] = v->buf[29]; v->buf[2] = v->buf[30];
    uint32_t addr = v->cur_addr & (SPU_RAM_SIZE - 1);
    uint8_t hdr = spu_ram[addr], flags = spu_ram[(addr + 1) & (SPU_RAM_SIZE - 1)];
    int shift = hdr & 0xF; if (shift > 12) shift = 9;
    int filter = (hdr >> 4) & 7; if (filter > 4) filter = 4;
    int k0 = K0[filter], k1 = K1[filter], pos = 3;
    for (int i = 0; i < 14; i++) {
        uint8_t b = spu_ram[(addr + 2 + (uint32_t)i) & (SPU_RAM_SIZE - 1)];
        v->buf[pos++] = decode_sample(v, b & 0xF, shift, k0, k1);
        v->buf[pos++] = decode_sample(v, b >> 4, shift, k0, k1);
    }
    if ((flags & 4) && !v->ignore_loop) v->repeat_addr = (uint16_t)(addr >> 3);
    v->cur_addr += 16;
    if (flags & 1) {
        v->end_x = true; endx |= 1u << index;
        v->cur_addr = (uint32_t)v->repeat_addr << 3;
        if (!(flags & 2)) { v->adsr_vol = 0; v->phase = PH_RELEASE; }
    }
}

static void tick_adsr(Voice *v)
{
    if (v->phase == PH_OFF) return;
    int lo = v->adsr_lo, hi = v->adsr_hi;
    bool atk_exp = (lo >> 15 & 1) != 0;
    int atk_shift = (lo >> 10) & 0x1F, atk_step = (lo >> 8) & 3;
    int dec_shift = (lo >> 4) & 0xF, sus_lvl = ((lo & 0xF) + 1) << 11;
    bool sus_exp = (hi >> 15 & 1) != 0, sus_dec = (hi >> 14 & 1) != 0;
    int sus_shift = (hi >> 8) & 0x1F, sus_step = (hi >> 6) & 3;
    bool rel_exp = (hi >> 5 & 1) != 0; int rel_shift = hi & 0x1F;
    if (v->phase == PH_ATTACK && v->adsr_vol >= 0x7FFF) { v->phase = PH_DECAY; v->adsr_cycle = 0; }
    if (v->phase == PH_DECAY && v->adsr_vol <= sus_lvl) { v->phase = PH_SUSTAIN; v->adsr_cycle = 0; }
    bool decrease = false, exp_ = false; int shift = 0, step_idx = 0;
    switch (v->phase) {
    case PH_ATTACK: decrease = false; shift = atk_shift; step_idx = atk_step; exp_ = atk_exp; break;
    case PH_DECAY: decrease = true; shift = dec_shift; step_idx = 0; exp_ = true; break;
    case PH_SUSTAIN: decrease = sus_dec; shift = sus_shift; step_idx = sus_step; exp_ = sus_exp; break;
    case PH_RELEASE: decrease = true; shift = rel_shift; step_idx = 0; exp_ = rel_exp; break;
    }
    env_tick(&v->adsr_vol, &v->adsr_cycle, shift, step_idx, exp_, decrease, false);
    if (v->phase == PH_RELEASE && v->adsr_vol == 0) v->phase = PH_OFF;
}

static void tick_noise(void)
{
    int shift = (spucnt >> 10) & 0xF, step = ((spucnt >> 8) & 3) + 4;
    noise_timer -= step;
    int parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
    if (noise_timer < 0) {
        noise_level = ((noise_level << 1) | parity) & 0xFFFF;
        noise_timer += 0x20000 >> shift;
        if (noise_timer < 0) noise_timer += 0x20000 >> shift;
    }
}

static void tick(int *out_l, int *out_r)
{
    tick_noise();
    int sum_l = 0, sum_r = 0;
    uint32_t non_mask = (uint32_t)(non_ | ((uint32_t)non_hi << 16));
    uint32_t pmon_mask = (uint32_t)(pmon | ((uint32_t)pmon_hi << 16));
    int prev_outx = 0;
    for (int i = 0; i < 24; i++) {
        Voice *v = &V[i];
        if (v->phase == PH_OFF) { prev_outx = 0; continue; }
        tick_adsr(v);
        sweep_tick(v->vol_l, &v->cur_vol_l, &v->vol_cyc_l);
        sweep_tick(v->vol_r, &v->cur_vol_r, &v->vol_cyc_r);
        if (!v->has_block) { memset(v->buf, 0, sizeof v->buf); decode_block(v, i); v->has_block = true; }
        int sample;
        if (non_mask & (1u << i)) sample = (int16_t)noise_level;
        else {
            int idx = (int)(v->pitch_counter >> 12), fi = (int)((v->pitch_counter >> 4) & 0xFF);
            sample = ((SPU_GAUSS[0x0FF - fi] * v->buf[idx]) >> 15)
                   + ((SPU_GAUSS[0x1FF - fi] * v->buf[idx + 1]) >> 15)
                   + ((SPU_GAUSS[0x100 + fi] * v->buf[idx + 2]) >> 15)
                   + ((SPU_GAUSS[0x000 + fi] * v->buf[idx + 3]) >> 15);
        }
        int amp = (sample * v->adsr_vol) >> 15;
        int step = v->pitch;
        if (i > 0 && (pmon_mask & (1u << i))) {
            int factor = clampi(prev_outx, -0x8000, 0x7FFF) + 0x8000;
            step = (((int16_t)(uint16_t)step * factor) >> 15) & 0xFFFF;
        }
        if (step > 0x3FFF) step = 0x4000;
        v->pitch_counter += (uint32_t)step;
        if ((v->pitch_counter >> 12) >= 28) { v->pitch_counter -= 28u << 12; decode_block(v, i); }
        prev_outx = amp;
        sum_l += (amp * v->cur_vol_l) >> 15;
        sum_r += (amp * v->cur_vol_r) >> 15;
    }
    *out_l = clampi(sum_l, -32768, 32767);
    *out_r = clampi(sum_r, -32768, 32767);
}

void spu_mix(int16_t *dst, int frames)
{
    pthread_mutex_lock(&g_spu_mx);
    for (int n = 0; n < frames; n++) {
        sweep_tick(main_vol_l, &main_cur_l, &main_cyc_l);
        sweep_tick(main_vol_r, &main_cur_r, &main_cyc_r);
        int l, r; tick(&l, &r);
        int mix_l = l, mix_r = r;
        int16_t xl, xr;
        if (xa_next(&xl, &xr)) {
            mix_l += (xl * (int16_t)cd_vol_l) >> 15;
            mix_r += (xr * (int16_t)cd_vol_r) >> 15;
        }
        mix_l = (clampi(mix_l, -32768, 32767) * main_cur_l) >> 15;
        mix_r = (clampi(mix_r, -32768, 32767) * main_cur_r) >> 15;
        dst[n * 2] = (int16_t)(mix_l * g_master);
        dst[n * 2 + 1] = (int16_t)(mix_r * g_master);
    }
    pthread_mutex_unlock(&g_spu_mx);
}

/* ================================================================== */
/* XA ADPCM                                                             */
/* ================================================================== */
#define XA_CAP (1 << 18)
#define XA_MASK (XA_CAP - 1)
static const int XA_POS[4] = { 0, 60, 115, 98 };
static const int XA_NEG[4] = { 0, 0, -52, -55 };
static int32_t xa_ring[XA_CAP];
static int xa_w, xa_r, xa_count;
static pthread_mutex_t xa_mx = PTHREAD_MUTEX_INITIALIZER;
static int xa_old_l, xa_older_l, xa_old_r, xa_older_r;
static int xa_src_rate = 37800; static bool xa_playing; static double xa_pos;
static int16_t xa_s0l, xa_s0r, xa_s1l, xa_s1r; static int xa_underrun;

void xa_reset(void)
{
    pthread_mutex_lock(&xa_mx);
    xa_old_l = xa_older_l = xa_old_r = xa_older_r = 0;
    xa_w = xa_r = xa_count = 0; xa_playing = false; xa_pos = 0;
    xa_s0l = xa_s0r = xa_s1l = xa_s1r = 0; xa_underrun = 0;
    pthread_mutex_unlock(&xa_mx);
}

static void xa_block(const uint8_t *sec, int b, int blk, int *old, int *older, int *dst)
{
    uint8_t hdr = sec[b + 4 + blk];
    int sv = hdr & 0xF; if (sv > 12) sv = 9;
    int filter = (hdr >> 4) & 3, f0 = XA_POS[filter], f1 = XA_NEG[filter];
    int col = blk >> 1, nshift = (blk & 1) * 4;
    for (int j = 0; j < 28; j++) {
        int nib = (sec[b + 16 + 4 * j + col] >> nshift) & 0xF;
        int t = nib >= 8 ? nib - 16 : nib;
        int s = clampi(((t << 12) >> sv) + ((*old * f0 + *older * f1 + 32) >> 6), -32768, 32767);
        *older = *old; *old = s; dst[j] = s;
    }
}

void xa_decode_sector(const uint8_t *sec, int off, uint8_t coding)
{
    bool stereo = (coding & 1) != 0;
    int rate = (coding & 4) ? 18900 : 37800;
    int l[28], r[28];
    int32_t frames[4032];
    int n = 0;
    /* xa_old_* are only touched by the (single) decoding thread at a time */
    for (int p = 0; p < 18; p++) {
        int b = off + p * 128;
        if (stereo) {
            for (int tb = 0; tb < 4; tb++) {
                xa_block(sec, b, tb * 2, &xa_old_l, &xa_older_l, l);
                xa_block(sec, b, tb * 2 + 1, &xa_old_r, &xa_older_r, r);
                for (int j = 0; j < 28; j++) frames[n++] = (int32_t)((uint16_t)l[j] | ((uint32_t)r[j] << 16));
            }
        } else {
            for (int blk = 0; blk < 8; blk++) {
                xa_block(sec, b, blk, &xa_old_l, &xa_older_l, l);
                for (int j = 0; j < 28; j++) frames[n++] = (int32_t)((uint16_t)l[j] | ((uint32_t)l[j] << 16));
            }
        }
    }
    pthread_mutex_lock(&xa_mx);
    xa_src_rate = rate;
    for (int i = 0; i < n; i++) {
        xa_ring[xa_w] = frames[i]; xa_w = (xa_w + 1) & XA_MASK;
        if (xa_count < XA_CAP) xa_count++; else xa_r = (xa_r + 1) & XA_MASK;
    }
    if (!xa_playing && xa_count >= 512) xa_playing = true;
    pthread_mutex_unlock(&xa_mx);
}

int xa_buffered_samples(void) { pthread_mutex_lock(&xa_mx); int n = xa_count; pthread_mutex_unlock(&xa_mx); return n; }

bool xa_next(int16_t *left, int16_t *right)
{
    pthread_mutex_lock(&xa_mx);
    if (!xa_playing) { pthread_mutex_unlock(&xa_mx); *left = *right = 0; return false; }
    while (xa_pos >= 1.0) {
        xa_s0l = xa_s1l; xa_s0r = xa_s1r;
        if (xa_count > 0) {
            int32_t packed = xa_ring[xa_r]; xa_r = (xa_r + 1) & XA_MASK; xa_count--;
            xa_s1l = (int16_t)(packed & 0xFFFF); xa_s1r = (int16_t)((uint32_t)packed >> 16); xa_underrun = 0;
        } else {
            if (++xa_underrun > 44100) { xa_playing = false; pthread_mutex_unlock(&xa_mx); *left = *right = 0; return false; }
            xa_s1l = (int16_t)(xa_s1l * 31 / 32); xa_s1r = (int16_t)(xa_s1r * 31 / 32);
        }
        xa_pos -= 1.0;
    }
    double f = xa_pos;
    *left = (int16_t)(xa_s0l + (xa_s1l - xa_s0l) * f);
    *right = (int16_t)(xa_s0r + (xa_s1r - xa_s0r) * f);
    xa_pos += (double)xa_src_rate / 44100.0;
    pthread_mutex_unlock(&xa_mx);
    return true;
}
