/* Pack Tacho's reserves bar (scripts/tacho_pack/patches/BarraReservas.xml) starts off.
 *
 * The bar shows while the game's _showBoostCount option is on (COctaneGlobalOptionsData +0x28,
 * reached as [[[0x2acde48] + 0xdb0] + 0x78], as the pack's own code reads it), and the pack's page
 * of Options > Game ("Barra de reservas", the pack's variable 12) switches it. That option starts
 * on in every save, so the bar would too. Once per player (a marker next to the saves), after the
 * game has read its save the first time (the save closed again: runtime_savedata.c), this turns the
 * option off; the game keeps it in its save from then on, and the player turns the bar on in
 * Options > Game. Nothing is done where the pack's bar is not in the executable (another edition).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"

#include <stdio.h>
#include <string.h>

static unsigned char *image;
static size_t image_size;
static int save_closed, done;

/* The bar's code at VA 0x2330000 (BarraReservas.xml): zeros when the pack is not applied. */
static int bar_patched(void) {
    static const unsigned char zeros[16];
    return image && image_size > 0x2330010 && memcmp(image + 0x2330000, zeros, sizeof zeros) != 0;
}

void runtime_reserves_bar_start(unsigned char *base, size_t size) {
    image = base;
    image_size = size;
}

void runtime_reserves_bar_save_closed(void) {
    __atomic_store_n(&save_closed, 1, __ATOMIC_RELEASE);
}

void runtime_reserves_bar_tick(void) {
    if (done || !__atomic_load_n(&save_closed, __ATOMIC_ACQUIRE) || !image || image_size < 0x2acde50) return;
    unsigned char *manager = *(unsigned char **)(image + 0x2acde48);
    unsigned char *owner = manager ? *(unsigned char **)(manager + 0xdb0) : NULL;
    unsigned char *options = owner ? *(unsigned char **)(owner + 0x78) : NULL;
    if (!options) return;
    done = 1;
    if (!bar_patched()) return;
    char marker[1024];
    snprintf(marker, sizeof marker, "%s/reserves_bar_default.txt", runtime_file_progress_dir());
    FILE *f = fopen(marker, "rb");
    if (f) {
        fclose(f);
        return;  /* applied before: the player's own choice is in the save */
    }
    options[0x28] = 0;
    f = fopen(marker, "wb");
    if (f) {
        fputs("The reserves bar started off once; Options > Game > Barra de reservas turns it on.\n", f);
        fclose(f);
    }
    puts("Pack Tacho: the reserves bar starts off (Options > Game > Barra de reservas turns it on)");
    fflush(stdout);
}
