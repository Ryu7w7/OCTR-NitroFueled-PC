/* Script types a port track left registered after it unloaded are unregistered; a game level's only told.
 * SPDX-License-Identifier: GPL-2.0-or-later */
volatile unsigned runtime_memory_changes;   /* runtime_memory.c */
#include "../src/runtime_hero_guard.c"
struct T { char pad[0x10]; const char *name; char rest[0x80]; };
static struct T t[4];
static int count, order[4], unregistered;
static int ABI fake_count(void) { return count; }
static void *ABI fake_at(int i) { return &t[order[i]]; }
static void ABI fake_unregister(void *type) {
    int instances;   /* as the game's (0x14a9c90): a type with instances stays */
    memcpy(&instances, (char *)type + 0x30, sizeof instances);
    if (instances) return;
    int k = 0;
    for (int i = 0; i < count; ++i) if (&t[order[i]] != type) order[k++] = order[i];
    unregistered += count - k;
    count = k;
}
#define LEVEL level
int main(void) {
    const char *level = "octane/octane_startscreen/octane_startscreen";
    setenv("BB_CONTENT_FILES", "archives/t929_neo_bowser_city.pak=/x|archives/b.bk2=/y", 1);
    const int game_level = getenv("GAME_LEVEL") != NULL;   /* the left level is not one of the port's */
    if (game_level) setenv("BB_CONTENT_FILES", "archives/t999_other.pak=/x", 1);
    t[0].name = "CEngineType"; t[1].name = "Scripts.Graph.common_TurboPadData"; t[2].name = "Scripts.Graph.Fine";
    t[3].name = "Scripts.Graph.Startscreen";
    type_count = fake_count; type_at = fake_at; type_unregister = fake_unregister;
    static const char vtable[8];
    for (int i = 0; i < 4; ++i) { const char *v = vtable; memcpy(t[i].pad, &v, sizeof v); }
    const int instances = 3;   /* the turbo pad data type still has instances (the objects Neo never gave back) */
    memcpy((char *)&t[1] + 0x30, &instances, sizeof instances);
    count = 2; order[0] = 0; order[1] = 3;
    runtime_type_diag("level loaded", "");                /* the start screen */
    runtime_type_diag("leaving the level", LEVEL);
    count = 1; order[0] = 0;
    level = "octane/t929_neo_bowser_city/t929_neo_bowser_city";
    runtime_type_diag("the next level loads", LEVEL);        /* start screen gone: nothing left */
    if (unregistered) { puts("FAIL: released after a clean unload"); return 1; }
    level = "octane/t929_neo_bowser_city/t929_neo_bowser_city";
    count = 3; order[1] = 1; order[2] = 2;
    runtime_type_diag("level loaded", LEVEL);
    runtime_type_diag("the next level loads", LEVEL);        /* a retry: nothing released */
    runtime_type_diag("level loaded", LEVEL);
    runtime_type_diag("leaving the level", LEVEL);
    count = 2; order[0] = 0; order[1] = 1;
    const int at_memory = getenv("AT_MEMORY") != NULL;   /* the game says the level's memory is given back first */
    if (at_memory) {
        runtime_type_diag("memory given back", "");
        if (!game_level && unregistered != 1) { puts("FAIL: not released when the memory was given back"); return 1; }
    }
    runtime_type_diag("the next level loads", "octane/octane_startscreen/octane_startscreen");        /* Fine gone, TurboPad left behind */
    if (game_level) { if (unregistered || count != 2) { puts("FAIL: a game level's types released"); return 1; } puts("game level PASS"); return 0; }
    if (unregistered != 1 || count != 1 || order[0] != 0) { printf("FAIL %d %d\n", unregistered, count); return 1; }
    puts("release PASS");
    return 0;
}
