#define _POSIX_C_SOURCE 200809L
#include "runtime_clan_tags.h"
#include "runtime_name_colors.h"
#include "runtime_http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *runtime_np_get_online_name(void) { return "ClanHost"; }

/* The real transport runs on the worker. A rejected write keeps the last
 * approved label; repeated draw calls must never stack suffixes. */
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t cap) {
    assert(!strncmp(target, "/ryunet/clan-tag?name=ClanHost", 30));
    const char *tag = "Ryu7";
    if (!strcmp(method, "POST")) {
        tag = strstr(target, "&tag="); assert(tag); tag += 5;
        if (!strcmp(tag, "Denied")) return 400;
        if (!strcmp(tag, "%E6%9D%B1%E4%BA%AC%E7%AB%B6%E8%B5%B0%E9%9A%8A%E5%93%A1")) tag = "東京競走隊員";
        else assert(!strchr(tag, '%')); /* a different UTF-8 request is a test failure */
    }
    // Also exercise an older server until the first explicit swatch edit.
    static int saved_swatch = -1;
    const char *selection = strstr(target, "&swatch=");
    if (selection) saved_swatch = atoi(selection + 8);
    if (saved_swatch >= 0) snprintf(body, cap, "ok\t%s\t%d\n", tag, saved_swatch);
    else snprintf(body, cap, "ok\t%s\n", tag);
    return 200;
}
static void tick(void) {
    const struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
}
/* Supply the real color worker with a local server response. New holders
 * need only appear in this list, regardless of the clan tag they choose. */
int runtime_ryunet_transfer(const char *method, const char *target, const void *data,
                            size_t size, char **reply, size_t *reply_size, size_t limit) {
    (void)data; (void)size;
    const char colors[] = "ok\nvenvs\trainbow:7\nNewHolder\trainbow:30\nAmber\t#ff8000\n";
    assert(!strcmp(method, "GET") && !strcmp(target, "/ryunet/names"));
    assert(sizeof colors <= limit);
    *reply = strdup(colors);
    assert(*reply);
    *reply_size = sizeof colors - 1;
    return 200;
}

static void check_name_colors(void) {
    float rgb[3];
    runtime_name_colors_start();
    for (int n = 0; n < 2000 && !runtime_name_color("NewHolder", rgb); ++n) tick();
    float first[3];
    assert(runtime_clan_color("Venvs [OMEN]", first));
    for (int n = 0; n < 2000; ++n) {
        tick();
        assert(runtime_clan_color("Venvs [OMEN]", rgb));
        if (memcmp(first, rgb, sizeof rgb)) break;
    }
    assert(memcmp(first, rgb, sizeof rgb)); /* the whole label animates */
    assert(runtime_clan_color("NewHolder [OTHER]" RUNTIME_NAME_SPEAKER_SUFFIX, rgb));
    assert(runtime_clan_color("NewHolder" RUNTIME_NAME_SPEAKER_SUFFIX, rgb));
    assert(runtime_clan_color("Amber [OTHER]", rgb));
    assert(rgb[0] == 1 && rgb[1] == 128 / 255.0f && rgb[2] == 0);
    assert(!runtime_clan_color("ClanHost [venvs]", rgb));
    assert(!runtime_clan_color("ClanHost [Amber]" RUNTIME_NAME_SPEAKER_SUFFIX, rgb));
}

static void save(const char *tag, enum ClanSave result) {
    assert(runtime_clan_save(tag) == CLAN_SAVING);
    for (int n = 0; n < 2000 && runtime_clan_status() == CLAN_SAVING; ++n) tick();
    assert(runtime_clan_status() == result);
}
/* The overlay uses the whole shaped run. The native font encodes that same
 * coverage in four levels. A hard threshold makes connected strokes look dotted. */
static void check_clan_raster(void) {
    unsigned char alpha[CLAN_RASTER_HEIGHT * CLAN_RASTER_WIDTH] = {0};
    const char *tag = "Équipe";
    const int width = bbgpu_clan_raster(tag, alpha);
    assert(width > 0 && width <= CLAN_RASTER_WIDTH);
    char tiles[CLAN_TILES_CAP];
    assert(bbgpu_clan_tiles(tag, tiles));
    const unsigned char *in = (const unsigned char *)tiles;
    int partial = 0;
    for (int x = 0; x < width; x += 2) {
        for (int y = 0; y < CLAN_RASTER_HEIGHT; y += 2) {
            unsigned mask = 0;
            for (int bit = 0; bit < 4; ++bit) {
                unsigned value = (alpha[(y + bit / 2)*CLAN_RASTER_WIDTH + x + bit%2] + 42u) / 85u;
                partial += value == 1 || value == 2;
                mask |= value << (2 * bit);
            }
            if (mask) {
                unsigned code = ((in[0]&15u)<<12) | ((in[1]&63u)<<6) | (in[2]&63u);
                assert(code == CLAN_TILE_FIRST + (unsigned)(y/2)*CLAN_TILE_LEVELS + mask);
                in += 3;
            }
        }
        unsigned advance = ((in[0]&15u)<<12) | ((in[1]&63u)<<6) | (in[2]&63u);
        assert(advance == CLAN_TILE_ADVANCE);
        in += 3;
    }
    assert(!*in && partial > 0);
}
int main(void) {
    char tag[CLAN_TAG_CAP], label[CLAN_LABEL_CAP], account[17];
    for (int n = 0; n < 2000 && !runtime_clan_lookup("ClanHost", tag); ++n) tick();
    assert(!strcmp(tag, "Ryu7"));
    runtime_clan_label("ClanHost", label, sizeof label);
    assert(!strcmp(label, "ClanHost [Ryu7]"));
    assert(!strcmp(runtime_clan_identity(label, account), "ClanHost"));
    assert(!strcmp(runtime_clan_identity("[Rank]", account), "[Rank]"));
    assert(!strcmp(runtime_clan_identity("SixteenCharsHere [ABC123]", account), "SixteenCharsHere"));
    assert(!strcmp(runtime_clan_identity("ClanHost [ABCDE77]", account), "ClanHost [ABCDE77]"));
    assert(!strcmp(runtime_clan_identity("ClanHost []", account), "ClanHost []"));
    runtime_clan_label("ClanHost [Ryu7]", label, sizeof label);
    assert(!strcmp(label, "ClanHost [Ryu7]"));
    save("ABC123", CLAN_SAVED);
    save("Denied", CLAN_INVALID);
    assert(runtime_clan_save("1234567") == CLAN_INVALID);
    runtime_clan_label("ClanHost [Ryu7]", label, sizeof label);
    assert(!strcmp(label, "ClanHost [ABC123]"));
    save("", CLAN_SAVED);
    runtime_clan_label("ClanHost [ABC123]", label, sizeof label);
    assert(!strcmp(label, "ClanHost"));
    save("東京競走隊員", CLAN_SAVED);
    runtime_clan_label("ClanHost", label, sizeof label);
    assert(!strcmp(label, "ClanHost [東京競走隊員]"));
    assert(!strcmp(runtime_clan_identity(label, account), "ClanHost"));
    char visual[CLAN_VISUAL_CAP];
    runtime_clan_visual_label("ClanHost", visual, sizeof visual);
    assert(strcmp(visual, label) && !strcmp(runtime_clan_identity(visual, account), "ClanHost"));
    char again[CLAN_VISUAL_CAP];
    runtime_clan_visual_label(visual, again, sizeof again);
    assert(!strcmp(visual, again));
    char narrow[16];
    runtime_clan_label("ClanHost", narrow, sizeof narrow);
    assert(!strcmp(narrow, "ClanHost"));
    assert(runtime_clan_save("\xc0\x80") == CLAN_INVALID);
    assert(runtime_clan_save("A\xe2\x80\xae") == CLAN_INVALID);
    assert(runtime_clan_swatch("ClanHost") == -1);
    assert(runtime_clan_save_profile("TEAM", NAME_SWATCH_COUNT) == CLAN_INVALID);
    assert(runtime_clan_save_profile("TEAM", 18) == CLAN_SAVING);
    for (int n = 0; n < 2000 && runtime_clan_status() == CLAN_SAVING; ++n) tick();
    assert(runtime_clan_status() == CLAN_SAVED && runtime_clan_swatch("ClanHost") == 18);
    runtime_clan_visual_label("ClanHost", visual, sizeof visual);
    assert((unsigned char)visual[0] == 0xee && !strcmp(runtime_clan_identity(visual, account), "ClanHost"));
    runtime_clan_visual_label(visual, again, sizeof again);
    assert(!strcmp(again, visual));
    runtime_clan_label(visual,label,sizeof label);
    assert(!strcmp(label,"ClanHost [TEAM]"));
    assert(runtime_clan_save_profile("TEAM", 0) == CLAN_SAVING);
    for (int n = 0; n < 2000 && runtime_clan_status() == CLAN_SAVING; ++n) tick();
    assert(runtime_clan_status() == CLAN_SAVED && runtime_clan_swatch("ClanHost") == 0);
    check_clan_raster();
    check_name_colors();
    puts("Clan-tag client tests passed");
}
