/* host_stub.c - platform hooks for building/testing the runtime on a desktop. */
#include "psx.h"
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

void plat_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); fputc('\n', stderr); }
uint64_t plat_time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void plat_sleep_us(unsigned us) { usleep(us); }
void plat_present(const uint16_t *v, int x, int y, int w, int h, bool r24, bool en) { (void)v; (void)x; (void)y; (void)w; (void)h; (void)r24; (void)en; }
void plat_fatal(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(1); }
const char *plat_disc_path(void) { return getenv("PSX_CUE") ? getenv("PSX_CUE") : "game.cue"; }
const char *plat_save_dir(void) { return "/tmp"; }
void plat_input_poll(void) {}
void plat_rumble(uint8_t a, uint8_t b) { (void)a; (void)b; }
void game_entry_run(void) {}
