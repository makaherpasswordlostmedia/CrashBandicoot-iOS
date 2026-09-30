/* gpu.c - selects the GPU backend.
 *   PSX_GLES2 (set by the Theos Makefile): GLES 2.0 renderer, VRAM on the GPU.
 *   otherwise: portable software rasterizer with CPU VRAM (desktop smoke tests / CI). */
#ifdef PSX_GLES2
#include "gpu_gles2.inc"
#else
#include "gpu_soft.inc"
#endif
