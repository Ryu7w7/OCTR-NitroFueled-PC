/* BB_META_DUMP=1 (lab): the game's own Alchemy field metadata, logged once it is registered.
 *
 * Each reflected class keeps a table of its fields' igMetaField objects in .bss, filled while
 * the game starts (the tables' names are in its RELRO data). Their objects carry each field's
 * offset in the instance, which no file has: this logs the first bytes of the objects of the
 * fields OCTR needs (USA 1.21 addresses), so the offsets can be read off the log. igGuiPlaceable's
 * _position is at +0x60 (scripts/aspect_hud.py), which shows where the offset is kept.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct { const char *klass, *field; uint64_t slot; } MetaSlot;

/* scratchpad meta_tables.py on CUSA13795 1.21: table -> slot of each field's igMetaField */
static const MetaSlot slots[] = {
    {"igGuiPlaceable", "_name", 0x2a20b50},          {"igGuiPlaceable", "_displayText", 0x2a20b58},
    {"igGuiPlaceable", "_parent", 0x2a20b68},        {"igGuiPlaceable", "_children", 0x2a20b70},
    {"igGuiPlaceable", "_position", 0x2a20b88},      {"igGuiPlaceable", "_pivot", 0x2a20b90},
    {"igGuiPlaceable", "_rotation", 0x2a20b98},      {"igGuiPlaceable", "_scale", 0x2a20ba0},
    {"igGuiPlaceable", "_extent", 0x2a20bb0},        {"igGuiPlaceable", "_color", 0x2a20c18},
    {"igGuiPlaceable", "_alpha", 0x2a20c28},         {"igGuiPlaceable", "_hidden", 0x2a20c30},
    {"igGuiPlaceable", "_behaviors", 0x2a20c40},
    {"igGuiText", "_font", 0x2a21790},               {"igGuiText", "_colorMode", 0x2a217b0},
    {"igGuiText", "_modifierColorA", 0x2a217b8},     {"igGuiText", "_modifierColorB", 0x2a217c0},
    {"igGuiText", "_renderText", 0x2a21820},         {"igGuiText", "_previousDisplayText", 0x2a21830},
    {"igText", "_string", 0x2a28400},                {"igText", "_color", 0x2a28450},
    {"igText", "_colorMode", 0x2a28458},             {"igText", "_colorA", 0x2a28460},
    {"LobbySlot", "_playerIndex", 0x2abeed8},        {"LobbySlot", "_namePlaceable", 0x2abeee0},
    {"LobbySlot", "_playerNumberText", 0x2abeee8},   {"LobbySlot", "_teamIndex", 0x2abef30},
    {"LobbySlot", "_player", 0x2abef38},             {"LobbySlot", "_ready", 0x2abef40},
    {"RacePosition", "_playerTagText", 0x2abf3a0},   {"RacePosition", "_localPlayerTextColor", 0x2abf3f8},
    {"RacePosition", "_remotePlayerTextColor", 0x2abf400}, {"RacePosition", "_racer", 0x2abf418},
    {"RacePosition", "_isOnlineRace", 0x2abf470},
};

static unsigned char *g_image;
static size_t g_size;

static int readable(const void *p) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    return p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
#else
    return p != NULL && (uintptr_t)p > 0x10000;
#endif
}

static void *dump_thread(void *unused) {
    (void)unused;
    /* the tables fill while the game starts: wait for the first and the last */
    for (int waited = 0; waited < 240; ++waited) {
        uint64_t first, last;
        memcpy(&first, g_image + slots[0].slot, 8);
        memcpy(&last, g_image + slots[sizeof slots / sizeof *slots - 1].slot, 8);
        if (first && last) break;
        sleep(1);
    }
    sleep(5);
    printf("Meta dump: %zu fields (USA 1.21 slots)\n", sizeof slots / sizeof *slots);
    for (size_t i = 0; i < sizeof slots / sizeof *slots; ++i) {
        uint64_t object;
        memcpy(&object, g_image + slots[i].slot, 8);
        char hex[3 * 64 + 1] = "";
        if (readable((void *)(uintptr_t)object)) {
            const unsigned char *o = (const unsigned char *)(uintptr_t)object;
            for (int b = 0; b < 64; ++b) snprintf(hex + 3 * b, 4, "%02x ", o[b]);
        }
        printf("Meta dump: %s.%s @%llx: %s\n", slots[i].klass, slots[i].field, (unsigned long long)object,
               hex[0] ? hex : "(not set)");
    }
    /* the classes' igMetaObjects, in the slots just before their fields' (their vtables) */
    static const MetaSlot classes[] = {
        {"igGuiText", "meta", 0x2a21780},        {"igGuiText", "meta-8", 0x2a21788},
        {"TeamSelectSlot", "meta", 0x2abeec8},   {"TeamSelectSlot", "meta-8", 0x2abeed0},
        {"LobbyMemberItem?", "meta", 0x2abecc8}, {"LobbyMemberItem?", "meta-8", 0x2abecd0},
        {"LobbyMemberItem?", "meta-16", 0x2abecd8},
        {"Table262afd0", "meta", 0x2abec48},     {"Table262afd0", "meta+8", 0x2abec50},
        {"RacePosition", "meta", 0x2abf380},     {"RacePosition", "meta-8", 0x2abf388},
        {"PlayerTag", "meta", 0x2abe0d8},        {"PlayerTag", "meta-8", 0x2abe0e0},
    };
    for (size_t i = 0; i < sizeof classes / sizeof *classes; ++i) {
        uint64_t object;
        memcpy(&object, g_image + classes[i].slot, 8);
        printf("Meta class: %s %s slot %llx -> %llx\n", classes[i].klass, classes[i].field,
               (unsigned long long)classes[i].slot, (unsigned long long)object);
        if (!readable((void *)(uintptr_t)object)) continue;
        const uint64_t *q = (const uint64_t *)(uintptr_t)object;
        for (int w = 0; w < 40; ++w) {
            const uint64_t v = q[w];
            const uint64_t rel = v - (uint64_t)(uintptr_t)g_image;
            const char *s = (const char *)(uintptr_t)v;
            int text = readable(s) && s[0] >= 'A' && s[0] <= 'z';
            for (int c = 0; text && c < 48 && s[c]; ++c) text = s[c] >= 32 && s[c] < 127;
            if (text)
                printf("Meta class:   +%02x \"%.48s\"\n", w * 8, s);
            else if (v >= (uint64_t)(uintptr_t)g_image && rel < g_size)
                printf("Meta class:   +%02x image+%llx\n", w * 8, (unsigned long long)rel);
            else
                printf("Meta class:   +%02x %llx\n", w * 8, (unsigned long long)v);
        }
    }
    /* Classes by name (BB_META_FIND, default the lobby/results ones): an igMetaObject keeps its
     * name at +0x10 and its vtable at +0x38; its fields' igMetaField objects follow in the next
     * slots, each with its offset at +0x14 and its name at +0x28. */
    const char *find = getenv("BB_META_FIND");
    if (!find || !*find) find = "LobbyMember,RaceRanking,Podium,LobbyPlayer,Matchmaking,RaceLeaders,KartMarker,TeamSelectPlayer,OnlinePlayer,Results";
    printf("Meta find: scanning for %s (image size %zx)\n", find, g_size);
    fflush(stdout);
    int classes_seen = 0;
    for (uint64_t slot = 0x2a00000; slot + 8 <= 0x2bee000 && slot + 8 <= g_size; slot += 8) {
        uint64_t object;
        memcpy(&object, g_image + slot, 8);
        if (object < 0x10000 || (object & 7) || !readable((void *)(uintptr_t)object) ||
            !readable((void *)(uintptr_t)(object + 0x40))) continue;
        const char *name = *(const char *const *)(uintptr_t)(object + 0x10);
        if ((uintptr_t)name < 0x10000 || !readable(name) || (name[0] != 'C' && name[0] != 'i')) continue;
        ++classes_seen;
        int printable = 1;
        for (int c = 0; printable && c < 64 && name[c]; ++c) printable = name[c] >= 32 && name[c] < 127;
        if (!printable) continue;
        int hit = 0;
        for (const char *f = find; *f && !hit;) {
            const char *comma = strchr(f, ',');
            const size_t n = comma ? (size_t)(comma - f) : strlen(f);
            char part[64];
            snprintf(part, sizeof part, "%.*s", (int)n, f);
            hit = n && strstr(name, part) != NULL;
            f += n + (comma ? 1 : 0);
        }
        if (!hit) continue;
        const uint64_t vtable = *(const uint64_t *)(uintptr_t)(object + 0x38);
        printf("Meta find: %s slot %llx vtable image+%llx\n", name, (unsigned long long)slot,
               (unsigned long long)(vtable - (uint64_t)(uintptr_t)g_image));
        for (int k = 0; k < 48; ++k) {
            uint64_t field;
            memcpy(&field, g_image + slot + 0x10 + 8 * (uint64_t)k, 8);
            if (!readable((void *)(uintptr_t)field)) break;
            const char *fname = *(const char *const *)(uintptr_t)(field + 0x28);
            if (!readable(fname) || fname[0] != '_') break;
            uint32_t offset;
            memcpy(&offset, (const unsigned char *)(uintptr_t)field + 0x14, 4);
            printf("Meta find:   [%d] %-32.32s +%x\n", k, fname, offset);
        }
        fflush(stdout);
    }
    printf("Meta find: done, %d class names seen\n", classes_seen);
    fflush(stdout);
    return NULL;
}

void runtime_meta_dump_start(unsigned char *image, size_t size) {
    const char *on = getenv("BB_META_DUMP");
    if (!on || on[0] != '1' || size < 0x2ac0000) return;
    g_image = image;
    g_size = size;
    pthread_t thread;
    if (pthread_create(&thread, NULL, dump_thread, NULL) == 0) pthread_detach(thread);
}
