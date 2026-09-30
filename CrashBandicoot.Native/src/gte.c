/* gte.c - Hardware/Gte.cs ported to C (without the HLE widescreen / screen
 * cache hooks, which only matter for the GL backend). */
#include "psx.h"

static int16_t V[9];
static uint8_t RGBC_R, RGBC_G, RGBC_B, RGBC_CODE;
static uint16_t OTZ;
static int IR0, IR1, IR2, IR3;
static int16_t SX[3], SY[3];
static uint16_t SZ[4];
static uint32_t RGB[3], RES1;
static int MAC0, MAC1, MAC2, MAC3;
static uint32_t LZCS, LZCR;
static int16_t RT[9], LLM[9], LCM[9];
static int TR[3], BK[3], FC[3];
static int OFX, OFY;
static uint16_t H;
static int16_t DQA;
static int DQB;
static int16_t ZSF3, ZSF4;
static uint32_t FLAG;
static uint8_t Unr[0x101];
static bool unr_ready;

static void build_unr(void)
{
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        Unr[i] = (uint8_t)(v < 0 ? 0 : v > 0xFF ? 0xFF : v);
    }
    unr_ready = true;
}

static inline void flag(int bit) { FLAG |= 1u << bit; }

static int sat_ir(int n, int v, bool lm)
{
    int mn = lm ? 0 : -0x8000;
    if (v < mn) { v = mn; flag(25 - n); }
    else if (v > 0x7FFF) { v = 0x7FFF; flag(25 - n); }
    return v;
}
static int sat_ir0(int v) { if (v < 0) { flag(12); return 0; } if (v > 0x1000) { flag(12); return 0x1000; } return v; }
static int sat_color(int n, int v) { if (v < 0) { flag(21 - n); return 0; } if (v > 0xFF) { flag(21 - n); return 0xFF; } return v; }
static int sat_sz(int v) { if (v < 0) { flag(18); return 0; } if (v > 0xFFFF) { flag(18); return 0xFFFF; } return v; }
static int sat_x(int v) { if (v < -0x400) { flag(14); return -0x400; } if (v > 0x3FF) { flag(14); return 0x3FF; } return v; }
static int sat_y(int v) { if (v < -0x400) { flag(13); return -0x400; } if (v > 0x3FF) { flag(13); return 0x3FF; } return v; }
static int64_t check_mac0(int64_t v) { if (v > 0x7FFFFFFFLL) flag(16); else if (v < -0x80000000LL) flag(15); return v; }
static void check_mac(int n, int64_t v)
{
    if (v >= (1LL << 43)) flag(31 - n);
    else if (v < -(1LL << 43)) flag(28 - n);
}

static void set_mac(int n, int64_t v, int sf, bool lm)
{
    check_mac(n, v);
    int m = (int)(v >> sf);
    if (n == 1) { MAC1 = m; IR1 = sat_ir(1, m, lm); }
    else if (n == 2) { MAC2 = m; IR2 = sat_ir(2, m, lm); }
    else { MAC3 = m; IR3 = sat_ir(3, m, lm); }
}

static void mat_vec(const int16_t *mx, int t0, int t1, int t2, int vx, int vy, int vz, int sf, bool lm)
{
    set_mac(1, ((int64_t)t0 << 12) + (int64_t)mx[0] * vx + (int64_t)mx[1] * vy + (int64_t)mx[2] * vz, sf, lm);
    set_mac(2, ((int64_t)t1 << 12) + (int64_t)mx[3] * vx + (int64_t)mx[4] * vy + (int64_t)mx[5] * vz, sf, lm);
    set_mac(3, ((int64_t)t2 << 12) + (int64_t)mx[6] * vx + (int64_t)mx[7] * vy + (int64_t)mx[8] * vz, sf, lm);
}

static void push_color(void)
{
    int r = sat_color(0, MAC1 >> 4), g = sat_color(1, MAC2 >> 4), b = sat_color(2, MAC3 >> 4);
    RGB[0] = RGB[1]; RGB[1] = RGB[2];
    RGB[2] = (uint32_t)(r | (g << 8) | (b << 16) | ((uint32_t)RGBC_CODE << 24));
}

static void interp(int64_t in1, int64_t in2, int64_t in3, int sf, bool lm)
{
    IR1 = sat_ir(1, (int)((((int64_t)FC[0] << 12) - in1) >> sf), false);
    IR2 = sat_ir(2, (int)((((int64_t)FC[1] << 12) - in2) >> sf), false);
    IR3 = sat_ir(3, (int)((((int64_t)FC[2] << 12) - in3) >> sf), false);
    set_mac(1, (int64_t)IR1 * IR0 + in1, sf, lm);
    set_mac(2, (int64_t)IR2 * IR0 + in2, sf, lm);
    set_mac(3, (int64_t)IR3 * IR0 + in3, sf, lm);
    push_color();
}

static void modulate(int sf, bool lm)
{
    set_mac(1, ((int64_t)RGBC_R * IR1) << 4, sf, lm);
    set_mac(2, ((int64_t)RGBC_G * IR2) << 4, sf, lm);
    set_mac(3, ((int64_t)RGBC_B * IR3) << 4, sf, lm);
    push_color();
}

static int clz16(uint32_t v) { int n = 0; for (int i = 15; i >= 0 && !(v & (1u << i)); i--) n++; return n; }

static uint32_t divide(uint32_t h, uint32_t sz3)
{
    if (h >= sz3 * 2) { flag(17); return 0x1FFFF; }
    int z = clz16(sz3);
    uint64_t n = (uint64_t)h << z, d = (uint64_t)sz3 << z;
    int idx = (int)((d - 0x7FC0) >> 7);
    if (idx < 0) idx = 0; else if (idx > 0x100) idx = 0x100;
    uint64_t u = (uint64_t)Unr[idx] + 0x101;
    d = (0x2000080ULL - d * u) >> 8;
    d = (0x0000080ULL + d * u) >> 8;
    uint64_t res = (n * d + 0x8000) >> 16;
    return res > 0x1FFFF ? 0x1FFFFu : (uint32_t)res;
}

static void rtp(int vx, int vy, int vz, int sf, bool lm, bool last)
{
    int64_t m1 = ((int64_t)TR[0] << 12) + (int64_t)RT[0] * vx + (int64_t)RT[1] * vy + (int64_t)RT[2] * vz;
    int64_t m2 = ((int64_t)TR[1] << 12) + (int64_t)RT[3] * vx + (int64_t)RT[4] * vy + (int64_t)RT[5] * vz;
    int64_t m3 = ((int64_t)TR[2] << 12) + (int64_t)RT[6] * vx + (int64_t)RT[7] * vy + (int64_t)RT[8] * vz;
    check_mac(1, m1); check_mac(2, m2); check_mac(3, m3);
    MAC1 = (int)(m1 >> sf); MAC2 = (int)(m2 >> sf); MAC3 = (int)(m3 >> sf);
    IR1 = sat_ir(1, MAC1, lm);
    IR2 = sat_ir(2, MAC2, lm);
    int ir3flag = (int)(m3 >> 12);
    if (ir3flag < -0x8000 || ir3flag > 0x7FFF) flag(22);
    int lo = lm ? 0 : -0x8000;
    IR3 = MAC3 < lo ? lo : MAC3 > 0x7FFF ? 0x7FFF : MAC3;
    int sz = sat_sz((int)(m3 >> 12));
    SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3]; SZ[3] = (uint16_t)sz;
    uint32_t div = divide(H, SZ[3]);
    int64_t sx = check_mac0((int64_t)div * IR1 + OFX); MAC0 = (int)sx;
    int64_t sy = check_mac0((int64_t)div * IR2 + OFY); MAC0 = (int)sy;
    int nx = sat_x((int)(sx >> 16)), ny = sat_y((int)(sy >> 16));
    SX[0] = SX[1]; SX[1] = SX[2]; SX[2] = (int16_t)nx;
    SY[0] = SY[1]; SY[1] = SY[2]; SY[2] = (int16_t)ny;
    if (last) {
        int64_t dp = check_mac0((int64_t)div * DQA + DQB);
        MAC0 = (int)dp;
        IR0 = sat_ir0((int)(dp >> 12));
    }
}

static void ncs(int vec, int sf, bool lm)
{
    mat_vec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
    mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
    push_color();
}
static void ncds(int vec, int sf, bool lm)
{
    mat_vec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
    mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
    interp(((int64_t)RGBC_R * IR1) << 4, ((int64_t)RGBC_G * IR2) << 4, ((int64_t)RGBC_B * IR3) << 4, sf, lm);
}
static void nccs(int vec, int sf, bool lm)
{
    mat_vec(LLM, 0, 0, 0, V[vec * 3], V[vec * 3 + 1], V[vec * 3 + 2], sf, lm);
    mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
    modulate(sf, lm);
}

static void mvmva(int sf, bool lm, int mx, int vn, int cv)
{
    const int16_t *mat = mx == 0 ? RT : mx == 1 ? LLM : mx == 2 ? LCM : RT;
    int vx, vy, vz;
    if (vn < 3) { vx = V[vn * 3]; vy = V[vn * 3 + 1]; vz = V[vn * 3 + 2]; }
    else { vx = IR1; vy = IR2; vz = IR3; }
    if (cv == 2) {
        sat_ir(1, (int)((((int64_t)FC[0] << 12) + (int64_t)mat[0] * vx) >> sf), lm);
        sat_ir(2, (int)((((int64_t)FC[1] << 12) + (int64_t)mat[3] * vx) >> sf), lm);
        sat_ir(3, (int)((((int64_t)FC[2] << 12) + (int64_t)mat[6] * vx) >> sf), lm);
        set_mac(1, (int64_t)mat[1] * vy + (int64_t)mat[2] * vz, sf, lm);
        set_mac(2, (int64_t)mat[4] * vy + (int64_t)mat[5] * vz, sf, lm);
        set_mac(3, (int64_t)mat[7] * vy + (int64_t)mat[8] * vz, sf, lm);
        return;
    }
    int t0 = 0, t1 = 0, t2 = 0;
    if (cv == 0) { t0 = TR[0]; t1 = TR[1]; t2 = TR[2]; }
    else if (cv == 1) { t0 = BK[0]; t1 = BK[1]; t2 = BK[2]; }
    mat_vec(mat, t0, t1, t2, vx, vy, vz, sf, lm);
}

void Gte_Execute(uint32_t cmd)
{
    if (!unr_ready) build_unr();
    FLAG = 0;
    int sf = (cmd & (1u << 19)) ? 12 : 0;
    bool lm = (cmd & (1u << 10)) != 0;
    int mx = (int)((cmd >> 17) & 3), vn = (int)((cmd >> 15) & 3), cv = (int)((cmd >> 13) & 3);
    switch (cmd & 0x3F) {
    case 0x01: rtp(V[0], V[1], V[2], sf, lm, true); break;
    case 0x30:
        rtp(V[0], V[1], V[2], sf, lm, false);
        rtp(V[3], V[4], V[5], sf, lm, false);
        rtp(V[6], V[7], V[8], sf, lm, true);
        break;
    case 0x06:
        MAC0 = (int)check_mac0((int64_t)SX[0] * (SY[1] - SY[2]) + (int64_t)SX[1] * (SY[2] - SY[0]) + (int64_t)SX[2] * (SY[0] - SY[1]));
        break;
    case 0x2D:
        MAC0 = (int)check_mac0((int64_t)ZSF3 * (SZ[1] + SZ[2] + SZ[3]));
        OTZ = (uint16_t)sat_sz(MAC0 >> 12);
        break;
    case 0x2E:
        MAC0 = (int)check_mac0((int64_t)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]));
        OTZ = (uint16_t)sat_sz(MAC0 >> 12);
        break;
    case 0x12: mvmva(sf, lm, mx, vn, cv); break;
    case 0x28:
        set_mac(1, (int64_t)IR1 * IR1, sf, lm); set_mac(2, (int64_t)IR2 * IR2, sf, lm); set_mac(3, (int64_t)IR3 * IR3, sf, lm);
        break;
    case 0x0C:
        set_mac(1, (int64_t)RT[4] * IR3 - (int64_t)RT[8] * IR2, sf, lm);
        set_mac(2, (int64_t)RT[8] * IR1 - (int64_t)RT[0] * IR3, sf, lm);
        set_mac(3, (int64_t)RT[0] * IR2 - (int64_t)RT[4] * IR1, sf, lm);
        break;
    case 0x3D:
        set_mac(1, (int64_t)IR0 * IR1, sf, lm); set_mac(2, (int64_t)IR0 * IR2, sf, lm); set_mac(3, (int64_t)IR0 * IR3, sf, lm);
        push_color();
        break;
    case 0x3E:
        set_mac(1, ((int64_t)MAC1 << sf) + (int64_t)IR0 * IR1, sf, lm);
        set_mac(2, ((int64_t)MAC2 << sf) + (int64_t)IR0 * IR2, sf, lm);
        set_mac(3, ((int64_t)MAC3 << sf) + (int64_t)IR0 * IR3, sf, lm);
        push_color();
        break;
    case 0x10: interp((int64_t)RGBC_R << 16, (int64_t)RGBC_G << 16, (int64_t)RGBC_B << 16, sf, lm); break;
    case 0x2A:
        for (int i = 0; i < 3; i++)
            interp((int64_t)(RGB[0] & 0xFF) << 16, (int64_t)((RGB[0] >> 8) & 0xFF) << 16, (int64_t)((RGB[0] >> 16) & 0xFF) << 16, sf, lm);
        break;
    case 0x11: interp((int64_t)IR1 << 12, (int64_t)IR2 << 12, (int64_t)IR3 << 12, sf, lm); break;
    case 0x29: interp(((int64_t)RGBC_R * IR1) << 4, ((int64_t)RGBC_G * IR2) << 4, ((int64_t)RGBC_B * IR3) << 4, sf, lm); break;
    case 0x1E: ncs(0, sf, lm); break;
    case 0x20: ncs(0, sf, lm); ncs(1, sf, lm); ncs(2, sf, lm); break;
    case 0x13: ncds(0, sf, lm); break;
    case 0x16: ncds(0, sf, lm); ncds(1, sf, lm); ncds(2, sf, lm); break;
    case 0x1B: nccs(0, sf, lm); break;
    case 0x3F: nccs(0, sf, lm); nccs(1, sf, lm); nccs(2, sf, lm); break;
    case 0x1C:
        mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        modulate(sf, lm);
        break;
    case 0x14:
        mat_vec(LCM, BK[0], BK[1], BK[2], IR1, IR2, IR3, sf, lm);
        interp(((int64_t)RGBC_R * IR1) << 4, ((int64_t)RGBC_G * IR2) << 4, ((int64_t)RGBC_B * IR3) << 4, sf, lm);
        break;
    }
    if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u;
}

static uint32_t pk(int16_t lo, int16_t hi) { return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16); }

uint32_t Gte_Read(int reg)
{
    switch (reg) {
    case 0: return pk(V[0], V[1]);
    case 1: return (uint32_t)(int32_t)V[2];
    case 2: return pk(V[3], V[4]);
    case 3: return (uint32_t)(int32_t)V[5];
    case 4: return pk(V[6], V[7]);
    case 5: return (uint32_t)(int32_t)V[8];
    case 6: return RGBC_R | ((uint32_t)RGBC_G << 8) | ((uint32_t)RGBC_B << 16) | ((uint32_t)RGBC_CODE << 24);
    case 7: return OTZ;
    case 8: return (uint32_t)IR0;
    case 9: return (uint32_t)IR1;
    case 10: return (uint32_t)IR2;
    case 11: return (uint32_t)IR3;
    case 12: return pk(SX[0], SY[0]);
    case 13: return pk(SX[1], SY[1]);
    case 14: case 15: return pk(SX[2], SY[2]);
    case 16: return SZ[0]; case 17: return SZ[1]; case 18: return SZ[2]; case 19: return SZ[3];
    case 20: return RGB[0]; case 21: return RGB[1]; case 22: return RGB[2]; case 23: return RES1;
    case 24: return (uint32_t)MAC0; case 25: return (uint32_t)MAC1; case 26: return (uint32_t)MAC2; case 27: return (uint32_t)MAC3;
    case 28: case 29: {
        int r = clampi(IR1 >> 7, 0, 0x1F), g = clampi(IR2 >> 7, 0, 0x1F), b = clampi(IR3 >> 7, 0, 0x1F);
        return (uint32_t)(r | (g << 5) | (b << 10)); }
    case 30: return LZCS;
    case 31: return LZCR;
    default: return 0;
    }
}

void Gte_Write(int reg, uint32_t val)
{
    switch (reg) {
    case 0: V[0] = (int16_t)val; V[1] = (int16_t)(val >> 16); break;
    case 1: V[2] = (int16_t)val; break;
    case 2: V[3] = (int16_t)val; V[4] = (int16_t)(val >> 16); break;
    case 3: V[5] = (int16_t)val; break;
    case 4: V[6] = (int16_t)val; V[7] = (int16_t)(val >> 16); break;
    case 5: V[8] = (int16_t)val; break;
    case 6: RGBC_R = (uint8_t)val; RGBC_G = (uint8_t)(val >> 8); RGBC_B = (uint8_t)(val >> 16); RGBC_CODE = (uint8_t)(val >> 24); break;
    case 7: OTZ = (uint16_t)val; break;
    case 8: IR0 = (int16_t)val; break;
    case 9: IR1 = (int16_t)val; break;
    case 10: IR2 = (int16_t)val; break;
    case 11: IR3 = (int16_t)val; break;
    case 12: SX[0] = (int16_t)val; SY[0] = (int16_t)(val >> 16); break;
    case 13: SX[1] = (int16_t)val; SY[1] = (int16_t)(val >> 16); break;
    case 14: SX[2] = (int16_t)val; SY[2] = (int16_t)(val >> 16); break;
    case 15:
        SX[0] = SX[1]; SY[0] = SY[1]; SX[1] = SX[2]; SY[1] = SY[2];
        SX[2] = (int16_t)val; SY[2] = (int16_t)(val >> 16);
        break;
    case 16: SZ[0] = (uint16_t)val; break; case 17: SZ[1] = (uint16_t)val; break;
    case 18: SZ[2] = (uint16_t)val; break; case 19: SZ[3] = (uint16_t)val; break;
    case 20: RGB[0] = val; break; case 21: RGB[1] = val; break; case 22: RGB[2] = val; break; case 23: RES1 = val; break;
    case 24: MAC0 = (int)val; break; case 25: MAC1 = (int)val; break; case 26: MAC2 = (int)val; break; case 27: MAC3 = (int)val; break;
    case 28:
        IR1 = (int)((val & 0x1F) << 7); IR2 = (int)(((val >> 5) & 0x1F) << 7); IR3 = (int)(((val >> 10) & 0x1F) << 7);
        break;
    case 29: break;
    case 30: {
        LZCS = val;
        uint32_t test = (val & 0x80000000u) ? ~val : val;
        LZCR = test == 0 ? 32u : (uint32_t)__builtin_clz(test);
        break; }
    case 31: break;
    }
}

uint32_t Gte_ReadControl(int reg)
{
    switch (reg) {
    case 0: return pk(RT[0], RT[1]); case 1: return pk(RT[2], RT[3]); case 2: return pk(RT[4], RT[5]); case 3: return pk(RT[6], RT[7]);
    case 4: return (uint32_t)(int32_t)RT[8];
    case 5: return (uint32_t)TR[0]; case 6: return (uint32_t)TR[1]; case 7: return (uint32_t)TR[2];
    case 8: return pk(LLM[0], LLM[1]); case 9: return pk(LLM[2], LLM[3]); case 10: return pk(LLM[4], LLM[5]); case 11: return pk(LLM[6], LLM[7]);
    case 12: return (uint32_t)(int32_t)LLM[8];
    case 13: return (uint32_t)BK[0]; case 14: return (uint32_t)BK[1]; case 15: return (uint32_t)BK[2];
    case 16: return pk(LCM[0], LCM[1]); case 17: return pk(LCM[2], LCM[3]); case 18: return pk(LCM[4], LCM[5]); case 19: return pk(LCM[6], LCM[7]);
    case 20: return (uint32_t)(int32_t)LCM[8];
    case 21: return (uint32_t)FC[0]; case 22: return (uint32_t)FC[1]; case 23: return (uint32_t)FC[2];
    case 24: return (uint32_t)OFX; case 25: return (uint32_t)OFY;
    case 26: return (uint32_t)(int32_t)(int16_t)H;
    case 27: return (uint32_t)(int32_t)DQA; case 28: return (uint32_t)DQB;
    case 29: return (uint32_t)(int32_t)ZSF3; case 30: return (uint32_t)(int32_t)ZSF4;
    case 31: return FLAG;
    default: return 0;
    }
}

void Gte_WriteControl(int reg, uint32_t val)
{
    switch (reg) {
    case 0: RT[0] = (int16_t)val; RT[1] = (int16_t)(val >> 16); break;
    case 1: RT[2] = (int16_t)val; RT[3] = (int16_t)(val >> 16); break;
    case 2: RT[4] = (int16_t)val; RT[5] = (int16_t)(val >> 16); break;
    case 3: RT[6] = (int16_t)val; RT[7] = (int16_t)(val >> 16); break;
    case 4: RT[8] = (int16_t)val; break;
    case 5: TR[0] = (int)val; break; case 6: TR[1] = (int)val; break; case 7: TR[2] = (int)val; break;
    case 8: LLM[0] = (int16_t)val; LLM[1] = (int16_t)(val >> 16); break;
    case 9: LLM[2] = (int16_t)val; LLM[3] = (int16_t)(val >> 16); break;
    case 10: LLM[4] = (int16_t)val; LLM[5] = (int16_t)(val >> 16); break;
    case 11: LLM[6] = (int16_t)val; LLM[7] = (int16_t)(val >> 16); break;
    case 12: LLM[8] = (int16_t)val; break;
    case 13: BK[0] = (int)val; break; case 14: BK[1] = (int)val; break; case 15: BK[2] = (int)val; break;
    case 16: LCM[0] = (int16_t)val; LCM[1] = (int16_t)(val >> 16); break;
    case 17: LCM[2] = (int16_t)val; LCM[3] = (int16_t)(val >> 16); break;
    case 18: LCM[4] = (int16_t)val; LCM[5] = (int16_t)(val >> 16); break;
    case 19: LCM[6] = (int16_t)val; LCM[7] = (int16_t)(val >> 16); break;
    case 20: LCM[8] = (int16_t)val; break;
    case 21: FC[0] = (int)val; break; case 22: FC[1] = (int)val; break; case 23: FC[2] = (int)val; break;
    case 24: OFX = (int)val; break; case 25: OFY = (int)val; break;
    case 26: H = (uint16_t)val; break;
    case 27: DQA = (int16_t)val; break; case 28: DQB = (int)val; break;
    case 29: ZSF3 = (int16_t)val; break; case 30: ZSF4 = (int16_t)val; break;
    case 31: FLAG = val & 0x7FFFF000u; if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u; break;
    }
}
