/* Pack Tacho's reserves bar (scripts/tacho_pack/patches/BarraReservas.xml) starts off.
 *
 * The bar shows while the game's _showBoostCount option is on (COctaneGlobalOptionsData +0x28,
 * reached as [[[0x2acde48] + 0xdb0] + 0x78], as the pack's own code reads it), and the pack's page
 * of Options > Game ("Barra de reservas", the pack's variable 12) switches it. That option starts
 * on in every save, so the bar would too. Once per player (a marker next to the saves), after the
 * game has read its save the first time (the save closed again: runtime_savedata.c), this turns the
 * option off; the game keeps it in its save from then on, and the player turns the bar on in
 * Options > Game. Nothing is done where the pack's bar is not in the executable (another edition).
 *
 * "Hud Power Charged" (the 8-place leaders list, HudOriginal.xml at VA 0x2334000) starts off the
 * same way: it reads _showObjective (+0x29, the pack's variable 11), turned off once with its own
 * marker, so a player who already had the bar's marker still gets it.
 *
 * "Barra de derrape" (the drift bar, in BarraReservas.xml with the reserves bar) starts off the same way: it reads
 * _showPlayerTags (+0x2b, the pack's variable 13; no game code reads it), with its own marker.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"

#include <stdio.h>
#include <string.h>

static unsigned char *image;
static size_t image_size;
static int save_closed, done;

/* The pack's code at that VA (BarraReservas.xml 0x2330000, HudOriginal.xml 0x2334000): zeros when
 * the pack is not applied. */
static int patched(size_t va) {
    static const unsigned char zeros[16];
    return image && image_size > va + sizeof zeros && memcmp(image + va, zeros, sizeof zeros) != 0;
}

/* Turns the option at options + field off once per player (the marker file says it was). */
static void start_off(unsigned char *options, size_t field, size_t code, const char *name, const char *what) {
    if (!patched(code)) return;
    char marker[1024];
    snprintf(marker, sizeof marker, "%s/%s", runtime_file_progress_dir(), name);
    FILE *f = fopen(marker, "rb");
    if (f) {
        fclose(f);
        return;  /* applied before: the player's own choice is in the save */
    }
    options[field] = 0;
    f = fopen(marker, "wb");
    if (f) {
        fprintf(f, "%s started off once; Options > Game turns it on.\n", what);
        fclose(f);
    }
    printf("Pack Tacho: %s starts off (Options > Game turns it on)\n", what);
    fflush(stdout);
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
    start_off(options, 0x28, 0x2330000, "reserves_bar_default.txt", "Barra de reservas");
    start_off(options, 0x29, 0x2334000, "hud_power_charged_default.txt", "Hud Power Charged");
    start_off(options, 0x2b, 0x2330000, "drift_bar_default.txt", "Barra de derrape");
}
