/* gpu.c - selects the GPU backend.
 *
 *   PSX_GLES2 (set by the Theos Makefile): GLES 2.0 renderer, VRAM lives on the GPU.
 *   PSX_ALLOW_SOFT_GPU: portable software rasterizer, ONLY for the desktop CI smoke
 *       test (gpu_soft.inc lives in tests/, it is not on the iOS include path).
 *
 * The iOS app has no software rendering path at all: without PSX_GLES2 the build fails. */
#if defined(PSX_GLES2)
#include "gpu_gles2.inc"
#elif defined(PSX_ALLOW_SOFT_GPU)
#include "gpu_soft.inc"
#else
#error "No GPU backend selected: build with -DPSX_GLES2 (iOS) or -DPSX_ALLOW_SOFT_GPU (desktop tests only)"
#endif
