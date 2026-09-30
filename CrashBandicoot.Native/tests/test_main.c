/* test_main.c - desktop smoke tests: rasterizer, GTE. */
#include "psx.h"

static void gp0(uint32_t w) { gpu_write_gp0(w); }

static int test_gpu(void)
{
    mem_init(); gpu_init(); spu_init(); mdec_init();
    gp0(0x020000FFu); gp0(0x00000000u); gp0(0x00400040u);                 /* fill 64x64 red   */
    gp0(0x300000FFu); gp0(0x00100010u); gp0(0x0000FF00u); gp0(0x00100030u);
    gp0(0x00FF0000u); gp0(0x00300010u);                                   /* gouraud triangle */
    gp0(0x60FFFFFFu); gp0(0x00200020u); gp0(0x00040004u);                 /* white 4x4 rect   */
    int red = g_vram[1 * VRAM_W + 1] & 0x1F;
    int tri = g_vram[0x14 * VRAM_W + 0x14];
    int rect = g_vram[0x21 * VRAM_W + 0x21];
    printf("fill r5=%d tri=%04x rect=%04x\n", red, tri, rect);
    if (red != 31) return 1;
    if (tri == 0 || tri == g_vram[1]) return 2;
    if (rect != 0x7FFF) return 3;
    gp0(0xA0000000u); gp0(0x00800080u); gp0(0x00010002u); gp0(0x12345678u);
    if (g_vram[0x80 * VRAM_W + 0x80] != 0x5678 || g_vram[0x80 * VRAM_W + 0x81] != 0x1234) return 4;
    return 0;
}

static int test_gte(void)
{
    Gte_WriteControl(0, 0x1000); Gte_WriteControl(1, 0); Gte_WriteControl(2, 0x1000); Gte_WriteControl(3, 0); Gte_WriteControl(4, 0x1000);
    Gte_WriteControl(5, 0); Gte_WriteControl(6, 0); Gte_WriteControl(7, 0x100);
    Gte_WriteControl(24, 160u << 16); Gte_WriteControl(25, 120u << 16); Gte_WriteControl(26, 200);
    Gte_Write(0, (20u << 16) | 10u); Gte_Write(1, 0);
    Gte_Execute(0x0180001u);
    uint32_t sxy = Gte_Read(14);
    int16_t sx = (int16_t)sxy, sy = (int16_t)(sxy >> 16);
    printf("rtps: sx=%d sy=%d sz=%u\n", sx, sy, Gte_Read(19));
    return (Gte_Read(19) == 0x100 && sx > 160 && sy > 120) ? 0 : 1;
}

int main(void)
{
    int r = test_gpu(); if (r) { printf("gpu test FAILED (%d)\n", r); return 1; }
    r = test_gte(); if (r) { printf("gte test FAILED (%d)\n", r); return 1; }
    printf("all smoke tests passed\n");
    return 0;
}
