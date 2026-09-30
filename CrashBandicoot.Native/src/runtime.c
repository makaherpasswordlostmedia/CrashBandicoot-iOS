/* runtime.c - Runtime.Initialize equivalent. Called once by the host on the
 * emulation thread, then game_entry_run() (generated) never returns. */
#include "psx.h"

void runtime_init(void)
{
    if (!mem_init()) plat_fatal("out of memory");
    gpu_init();
    if (!g_vram) plat_fatal("out of memory (vram)");
    spu_init();
    mdec_init();
    bios_init();

    static MemoryCard card_a, card_b;
    char path[600];
    snprintf(path, sizeof path, "%s/card1.mcr", plat_save_dir());
    card_init(&card_a, path);
    snprintf(path, sizeof path, "%s/card2.mcr", plat_save_dir());
    card_init(&card_b, path);
    g_card_a = &card_a; g_card_b = &card_b;

    g_disc = disc_open(plat_disc_path());
    if (!g_disc) plat_fatal("cannot open disc image (cue/bin)");
    cd_init(g_disc);
    LOG("[RUNTIME] initialised, disc=%s", plat_disc_path());
}

void runtime_run_game(void)
{
    runtime_init();
    game_entry_run();
    LOG("[RUNTIME] game returned");
}
