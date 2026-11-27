/* Elimination mode (test, offline Arcade single races only): every so often the racer in last place is out.
 *
 * The race menu's lap selector picks the time between cuts (laps do not end the race here). With the mode on, its
 * 3/5/7 list becomes 30, 45, 60, 80, 90, 120, 150, 180 (seconds), as GlisinSR's "Custom lap number selector" swaps
 * that list (its two menu functions 0x1268440 / 0x128d380, its XML says +0x400000: selector +0x1c0 the list, +0x190 / +0x1a0 the choice),
 * and the 1-digit formatter the menu prints with gets room for three digits (0x9b5be1 2 -> 4, guard 0x9b5c01).
 * The race settings [0x2ad4e30] +0xc then hold the seconds; 1..7 (the old list) still map to 30 s .. 2:30.
 *
 * Notices ("X ELIMINADO", "X GANO") are drawn by the in-game overlay (gpu/shim/bbport_overlay.cpp,
 * runtime_elimination_banner): the race HUD hides what is added to it once the local player is finished.
 * Last place is the game's own race position (CRacer +0x58), the one the HUD shows.
 *
 * While the player out watches another racer, the track's occlusion triggers (parts shown or hidden by where the
 * player's kart is) use the camera's place instead, as the replay player does (runtime_replay_camera_triggers:
 * COcclusionTrigger +0x5a _useCameraPosition).
 *
 * Laps do not end the race: crossing the line on the last lap is ignored, so everyone keeps going until a single
 * racer is left, who then finishes first. An eliminated racer is finished by the game's own code (as when a racer
 * crosses the line) in the place it had (8th, 7th...): its kart goes on driving by itself. While anyone is still
 * racing, the race does not end when the local player is out.
 *
 * The lap counter shows only the lap you are on: the HUD's total (its component +0x84, the game's lap cap and its
 * "final lap" check) becomes 99, and the total's text is formatted from "" instead of "%d" (0xe63f20, the
 * second format at 0xe64059: lea rsi,"%d" -> the same string's end). runtime_replay.c hooks 0xe63f20 and calls
 * runtime_elimination_hud_lap.
 *
 * Warning: the race timer blinks red (twice a second) in the last 10 s before each cut (runtime_replay_timer_warning,
 * runtime_replay_play.c), and the race clock's low time alarm (Crystal Challenge's) starts then
 * (runtime_replay_low_time_alarm: the game's own COnRaceClockLowTimeRemainingMessage).
 *
 * The local player out is not finished then (finishing a human opens the end of race menu and, offline, ends the race
 * for everyone): it is finished in the place it went out in once a single racer is left, just before the winner.
 *
 * Every kart out stays where it was: each frame it is put back at the place it went out (0x3dc8e0, an entity's
 * position; 0x3dc720 reads it) with no speed (0xbb5060, the kart's velocity), whatever its driver does.
 *
 * Notices: "<name> ELIMINADO" for 4 s at each cut and "<name> GANO" at the end (runtime_replay_banner); the alarm is
 * the HUD's own "UI_Timer_Low_Warning" sound on the player's kart (0x1024340, as runtime_online_mode.c plays the
 * results' sounds).
 *
 * The player out only stops (no flag faked: online the others must see the same race); just before the winner it
 * is finished for real. It can no longer use an item (runtime_elimination_blocks_item, runtime_replay.c's gate at
 * 0xf0b070).
 *
 * Spectator: once the local player is out, the race camera follows the leader; R2 or right / L2 or left go to the
 * next / previous racer still in the race (runtime_replay_spectate, runtime_replay_play.c, moves the camera as the
 * replay player does).
 *
 * Chosen in a private room's Game Mode (index 4, room mode 3). Native functions (USA 1.21; in the European 1.21
 * the ones marked EU-0x10 sit 0x10 lower, found by comparing the two executables, tools/port_patch_eu.py):
 *   0xf351b0 (rules, racer)       a racer reached the finish: finished flag, finish place, end of race check;
 *                                 hands a human's kart to the AI (0xff32e0). Called by 0xf34fd0 on the last lap.
 *   0xf34fd0 (rules, racer, lap)  a racer started a lap (gives us the race rules object)
 *   0xf34430 (rules)              are all the racers that matter finished? (offline: the humans). Hooked by
 *                                 runtime_online_mode.c (race_finished), which asks runtime_elimination_holds_race.
 * CRacer: +0x40 AI, +0x44 ratio on the track (0..1), +0x48 spline distance, +0x4c lap, +0x58 race position,
 * +0x8c finished. Racers: [0x2ad4b70]+0x10 (igVector: +0xc count, +0x20 items). Client mode [0x2ab3cd8]+0x144
 * (8 Arcade), race kind [0x2acde48]+0xd4c (1 single race). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

#define ELIMINATION_SECONDS 80.0
#define MAX_RACERS 8
#define WARNING_SECONDS 10.0
#define ELIMINATION_PACKET 24

typedef void (ABI *ReachedFinish)(void *, unsigned char *);
typedef void (ABI *LapStarted)(void *, unsigned char *, int);
typedef void (ABI *ResolveHandle)(void **);
typedef void (ABI *SetPosition)(void *, const float *);
typedef void (ABI *GetPosition)(float *, void *);
typedef void (ABI *SetVelocity)(void *, const float *);
typedef void (ABI *PlaySound)(void *, const char *, void **, void **);
int runtime_replay_spectate(void *kart);  /* runtime_replay_play.c */
int runtime_replay_spectate_wide(int on);  /* L3: the wide view, 1 = on */
float rr_race_clock(void);                /* runtime_replay.c: the race clock, seconds (0 none) */
void runtime_replay_timer_warning(int red);
void runtime_replay_hide_lap_total(int hide);
void runtime_replay_hud_racer(void *racer);
void runtime_replay_banner(const char *text);
void runtime_replay_camera_triggers(void);

static ReachedFinish native_reached_finish;
static LapStarted native_lap_started;
static ResolveHandle resolve_handle;
static SetPosition set_position;
static GetPosition get_position;
static SetVelocity set_velocity;
static PlaySound play_sound;
static void **racer_manager, **client, **manager;
static unsigned char *game_image;
static int installed;
static unsigned char *total_format;       /* 0xe6405c: the low byte of lea rsi,"%d" for the lap total */
static unsigned char total_low;           /* its own value: USA 0xea, EU 0xaa ("%d" 0x40 lower there) */
/* The European 1.21 game: the functions marked EU-0x10 are 0x10 lower; the data at the same addresses. */
static int game_eu;
static unsigned char *at(unsigned char *image, size_t usa, int moved) { return image + usa - (game_eu && moved ? 0x10 : 0); }
static int total_hidden;

/* the current race, on the game's thread */
static void *rules;                       /* from the game's own lap and finish calls */
static int allowing;                      /* our own finish call is going through */
static double race_clock, next_cut;
static unsigned long long last_ms;
static float last_distance[MAX_RACERS];
static int eliminated;
static int race_over;                     /* the winner is finished: nothing more to do in this race */
static double alarm_cut;                  /* the cut whose alarm was sent */
static int alarm_second = -1;             /* the second (to the cut) it sounded last */
static char banner[96];                   /* the overlay's notice: its title */
static int spectating;                    /* the local player is out */
static unsigned char *out_player;         /* the local player out, still unfinished for the game */
static int out_place;
static int out_finished;                  /* the player out finished for real (end of the race) */
static struct { unsigned char *kart; float at[3]; int place; } stopped[MAX_RACERS];  /* the karts out */
static int stopped_count;
static int out_stopped = -1;              /* the player's entry in stopped[], -1 none */
static void **race_settings;
static double interval = ELIMINATION_SECONDS;   /* the time between cuts, from the lap count */
static unsigned char *watched;            /* the racer the camera follows */

static unsigned long long now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
#endif
}

static void *ptr(const void *p, size_t off) { void *v; memcpy(&v, (const unsigned char *)p + off, sizeof v); return v; }
static int integer(const void *p, size_t off) { int v; memcpy(&v, (const unsigned char *)p + off, sizeof v); return v; }
static float real(const void *p, size_t off) { float v; memcpy(&v, (const unsigned char *)p + off, sizeof v); return v; }

/* A game handle's object (as runtime_nsane.c's handle_object). */
static unsigned char *handle_object(void **slot) {
    for (int depth = 0; slot && depth < 16 && (uintptr_t)*slot >= 0x10000; ++depth) {
        unsigned flags;
        memcpy(&flags, *slot, 4);
        if (flags & 0x10000000) resolve_handle(slot);
        if (!*slot) return NULL;
        memcpy(&flags, *slot, 4);
        unsigned char *object = ptr(*slot, 0x28);
        if (!(flags & 0x20000000) || !object) return object;
        slot = (void **)(object + 0x10);
    }
    return NULL;
}

/* A racer out stays where it is now. */
static void stop(unsigned char *racer, int place) {
    unsigned char *kart = handle_object((void **)(racer + 0x20));
    if (!kart || stopped_count >= MAX_RACERS) return;
    stopped[stopped_count].kart = kart;
    stopped[stopped_count].place = place;
    get_position(stopped[stopped_count].at, kart);
    ++stopped_count;
}

/* Every frame: the karts out back in their place, still. Only karts of the race on screen now: a retry or another
 * track frees the old ones, and moving a freed kart crashes. */
static void keep_stopped(unsigned char **items, int count) {
    static const float still[3] = {0, 0, 0};
    for (int i = 0; i < stopped_count; ++i) {
        int current = 0;
        for (int k = 0; k < count && !current; ++k)
            current = items[k] && handle_object((void **)(items[k] + 0x20)) == stopped[i].kart;
        if (!current) continue;
        set_position(stopped[i].kart, stopped[i].at);
        set_velocity(stopped[i].kart, still);
    }
}

/* The game's language (BB_LANGUAGE, PS4 ids): 0 English, 1 Spanish (3, 20), 2 French (2, 22), 3 German (4),
 * 4 Italian (5), 5 Portuguese (7, 17). */
static int language(void) {
    const char *lang = getenv("BB_LANGUAGE");
    const int id = lang ? atoi(lang) : 1;
    return id == 3 || id == 20 ? 1 : id == 2 || id == 22 ? 2 : id == 4 ? 3 : id == 5 ? 4 : id == 7 || id == 17 ? 5 : 0;
}

/* A racer's name: the player's, or the AI's driver (racer manager +0x28: +0x38 kart, +0x18 driver). */
static void racer_name(unsigned char *racer, char *out, size_t size) {
    const char *name = NULL;
    unsigned char *player = handle_object((void **)(racer + 0x10));
    if (player) name = ptr(player, 0x68);
    if (!name || (uintptr_t)name < 0x10000) {
        /* the driver's name as the game shows it: CRacer +0x30 _driverData, a CMarketplaceItem: +0x28
         * _storeDisplayName (set from systemData_<language>.lng) */
        name = NULL;
        unsigned char *driver = handle_object((void **)(racer + 0x30));
        const char *shown = driver ? ptr(driver, 0x28) : NULL;
        if (shown && (uintptr_t)shown >= 0x10000 && !((uintptr_t)shown & 1) && shown[0] && shown[1])
            for (int i = 0; i < 40; ++i) {
                if (!shown[i]) { name = shown; break; }
                if ((unsigned char)shown[i] < 0x20) break;
            }
    }
    if (!name || (uintptr_t)name < 0x10000) {
        unsigned char *kart = handle_object((void **)(racer + 0x20));
        void *infos = racer_manager && *racer_manager ? ptr(*racer_manager, 0x28) : NULL;
        const int count = infos ? integer(infos, 0xc) : 0;
        void **items = infos ? ptr(infos, 0x20) : NULL;
        for (int i = 0; items && i < count && i < 64; ++i)
            if (items[i] && ptr(items[i], 0x38) == kart) name = ptr(items[i], 0x18);
    }
    static const char *const racer_word[] = {"Racer", "Corredor", "Pilote", "Fahrer", "Pilota", "Piloto"};
    if (!name || (uintptr_t)name < 0x10000 || !*name) name = racer_word[language()];
    if (!strncmp(name, "Driver", 6) && name[6]) name += 6;   /* the AI's: "DriverPinstripe" */
    size_t n = 0;
    for (; name[n] && n + 1 < size; ++n) out[n] = (char)(name[n] == '_' ? ' ' : name[n]);
    out[n] = 0;
}

/* The overlay's banner: title, line under it, shown from banner_shown (ms) for banner_ms. */
static pthread_mutex_t banner_lock = PTHREAD_MUTEX_INITIALIZER;
static char banner_detail[128];
static unsigned long long banner_shown, banner_ms;
static int banner_won;

static void notice(unsigned char *racer, int won, int place) {
    char name[48];
    racer_name(racer, name, sizeof name);
    pthread_mutex_lock(&banner_lock);
    static const char *const wins[] = {"%s WINS", "%s GAN\xc3\x93", "%s GAGNE", "%s GEWINNT", "%s VINCE", "%s VENCEU"};
    static const char *const out[] = {"%s IS OUT", "%s ELIMINADO", "%s \xc3\x89LIMIN\xc3\x89", "%s IST RAUS", "%s ELIMINATO",
                                      "%s ELIMINADO"};
    static const char *const last[] = {"The last one left in the race.", "El \xc3\xbaltimo que qued\xc3\xb3 en carrera.",
                                       "Le dernier encore en course.", "Der Letzte im Rennen.", "L'ultimo rimasto in gara.",
                                       "O \xc3\xbaltimo que restou na corrida."};
    static const char *const place_text[] = {"Out in place %d.", "Qued\xc3\xb3 en el lugar %d.", "\xc3\x89limin\xc3\xa9 \xc3\xa0 la place %d.",
                                             "Raus auf Platz %d.", "Eliminato al posto %d.", "Ficou no lugar %d."};
    const int lang = language();
    snprintf(banner, sizeof banner, won ? wins[lang] : out[lang], name);
    if (won) snprintf(banner_detail, sizeof banner_detail, "%s", last[lang]);
    else snprintf(banner_detail, sizeof banner_detail, place_text[lang], place);
    banner_shown = now_ms();
    banner_ms = won ? 8000 : 4500;
    banner_won = won;
    pthread_mutex_unlock(&banner_lock);
    printf("Elimination: notice \"%s\"\n", banner);
}

/* For the overlay (any thread): 1 while a notice shows, its texts and how long it has shown (s), *won 1 for the
 * winner's. NULL buffers: only whether one shows. */
int runtime_elimination_banner(char *title, size_t title_size, char *detail, size_t detail_size, float *age, int *won) {
    pthread_mutex_lock(&banner_lock);
    const unsigned long long now = now_ms();
    const int on = banner_shown && now >= banner_shown && now - banner_shown < banner_ms;
    if (on && title) snprintf(title, title_size, "%s", banner);
    if (on && detail) snprintf(detail, detail_size, "%s", banner_detail);
    if (on && age) *age = (float)(now - banner_shown) / 1000.0f;
    if (on && won) *won = banner_won;
    pthread_mutex_unlock(&banner_lock);
    return on;
}

/* CRacer +0xb0: this PC drives it (online: its own player, and the host's AI); +0x10 the player (none for an AI). */
static int local_human(const unsigned char *r) { return r && r[0xb0] && !r[0x40] && ptr(r, 0x10); }
static int local_ai(const unsigned char *r) { return r && r[0xb0] && !ptr(r, 0x10); }

/* The local player's racer (the human one), NULL none. */
static unsigned char *local_racer(unsigned char **items, int count) {
    for (int i = 0; i < count; ++i)
        if (items[i] && (items[i] == out_player || local_human(items[i]))) return items[i];
    return NULL;
}

/* The racers of the race on screen; 0 when there is none. */
static int racers(unsigned char ***items) {
    void *list = racer_manager && *racer_manager ? ptr(*racer_manager, 0x10) : NULL;
    const int count = list ? integer(list, 0xc) : 0;
    *items = list ? ptr(list, 0x20) : NULL;
    return *items && count >= 2 && count <= MAX_RACERS ? count : 0;
}

/* ---- online: the private room's mode, chosen by its host (runtime_online_mode.c, runtime_room_mode.c) ---- */
static int mode_enabled;   /* this room's mode is Elimination (the host's pick, or the host's room for a guest) */
void runtime_elimination_mode_set(int on) { __atomic_store_n(&mode_enabled, !!on, __ATOMIC_RELEASE); }
int runtime_elimination_mode_enabled(void) { return __atomic_load_n(&mode_enabled, __ATOMIC_ACQUIRE); }

/* The racers out in this race, in the order they went out (the host's list online). */
static unsigned char *out_racers[MAX_RACERS];
static int out_count;
static int is_out(const unsigned char *racer) {
    for (int i = 0; i < out_count; ++i) if (out_racers[i] == racer) return 1;
    return 0;
}

/* The karts of the racers out, for the collision filter (physics threads) and the blink (runtime_replay_play.c):
 * set on the game's thread each frame of an elimination race, empty otherwise and once the race is over. */
static void *ghost_karts[MAX_RACERS];
int runtime_elimination_ghost_kart(void *entity) {
    if (!entity) return 0;
    for (int i = 0; i < MAX_RACERS; ++i)
        if (__atomic_load_n(&ghost_karts[i], __ATOMIC_RELAXED) == entity) return 1;
    return 0;
}
static void set_ghost_karts(int on) {
    for (int i = 0; i < MAX_RACERS; ++i) {
        void *kart = on && i < out_count && out_racers[i] ? handle_object((void **)(out_racers[i] + 0x20)) : NULL;
        __atomic_store_n(&ghost_karts[i], kart, __ATOMIC_RELAXED);
    }
}

/* Its hooks are in (USA or EU 1.21, no other patch there): the selector offers ELIMINATION only then. */
int runtime_elimination_available(void) { return installed; }

static int elimination_race(void) {
    if (!installed) return 0;
    unsigned char **items;
    return runtime_elimination_mode_enabled() && runtime_online_private_single_race() && racers(&items);
}

/* The online race-end countdown ("ENDS IN", CNetworkConfig._endRaceTimeout: 20 s in the game, the server's choice
 * through scripts/patches.py) is 10 s in an elimination race (the user, 26/11) and back to its own after. The network
 * config is [0x2acb3d8] +0x40 (runtime_item_lead.c); the field's offset comes from the game's reflection (type at
 * vtable[0], field by name with 0x14a7250, its offset at +0x14, as runtime_public_itemless.c reads them). */
#define ELIMINATION_END_RACE 10.0f
typedef void *(ABI *MetaField)(void *, const char *);
static MetaField meta_field;
static float *end_race_field(void) {
    static unsigned short offset;
    unsigned char *network = NULL, *config = NULL;
    if (game_image) memcpy(&network, game_image + 0x2acb3d8, sizeof network);
    if (network) memcpy(&config, network + 0x40, sizeof config);
    if (!config || !meta_field) return NULL;
    if (!offset) {
        typedef void *(ABI *GetType)(void *);
        void *meta = (*(GetType **)config)[0](config);
        unsigned char *field = meta ? meta_field(meta, "_endRaceTimeout") : NULL;
        if (field) memcpy(&offset, field + 0x14, sizeof offset);
        if (!offset) return NULL;
    }
    return (float *)(config + offset);
}

static void short_end_race(int on) {
    static int active;
    static float saved;
    if (on == active) return;
    float *field = end_race_field();
    if (!field) return;
    if (on) {
        saved = *field;
        *field = ELIMINATION_END_RACE;
    } else if (saved > 0.0f) {
        *field = saved;
    }
    active = on;
    printf("Elimination: race-end countdown %.0f s%s\n", *field, on ? " (elimination race)" : " (back to the room's)");
}

/* Online: the host decides the cuts (its game's race positions) and sends its list; a guest only applies it. */
static int guest(void) { return runtime_room_mode_following(); }

static int still_racing(void) {
    unsigned char **items;
    const int count = racers(&items);
    int left = 0;
    for (int i = 0; i < count; ++i)
        if (items[i] && !items[i][0x8c] && items[i] != out_player && !is_out(items[i])) ++left;
    return left;
}

/* ---- the game's calls ---------------------------------------------------------------------- */
static void ABI reached_finish(void *self, unsigned char *racer) {
    if (self) rules = self;
    const uintptr_t from = (uintptr_t)__builtin_return_address(0);
    /* 0xd29fd0 finishes everyone at once (the race being ended): always let it through. So does a finish of a kart
     * this PC does not drive (its own PC finished it: 26/11 test, every PC blocked the others' finishes, nobody was
     * finished anywhere but at home and the end-of-race countdown stayed at 00), and any finish once it is over. */
    if (allowing || race_over || !elimination_race() || from == (uintptr_t)(game_image + 0xd2a026) ||
        (racer && !local_human(racer) && !local_ai(racer))) {
        native_reached_finish(self, racer);
        return;
    }
    /* the last lap's line: no finish in this mode, the racer keeps going */
}

/* 0xf0b070 (item used), through runtime_replay.c's gate: 1 = refuse, for the player out. The power-up controller's
 * +0x18 is its kart. */
int runtime_elimination_blocks_item(void *controller) {
    if (!controller || !out_player || !elimination_race()) return 0;
    return ptr(controller, 0x18) == handle_object((void **)(out_player + 0x20));
}

/* 0xf34430, through runtime_online_mode.c: while anyone is still racing, the local player out does not end it */
int runtime_elimination_holds_race(void *self) {
    if (self) rules = self;
    return elimination_race() && !race_over && still_racing() > 0;   /* the winner known: nothing to wait for */
}

static void ABI lap_started(void *self, unsigned char *racer, int lap) {
    if (self) rules = self;
    native_lap_started(self, racer, lap);
}

static void new_race(void);

/* ---- every frame --------------------------------------------------------------------------- */
static void finish(unsigned char *racer, int place) {
    memcpy(racer + 0x58, &place, 4);  /* the finish place is taken from the race position */
    /* the player out shows the finish flag (+0x8c) without being finished: while another racer's finish runs,
     * the game would see every local player finished and open the results; off for that call */
    /* not for the winner's finish (the player out is finished for real by then): that one ends the race, and the
     * game asks "every local player finished?" only on a finish (0xf353b0 -> 0xf34430, rules +0x25) */
    unsigned char *flagged = out_player && !out_finished && racer != out_player && out_player[0x8c] ? out_player : NULL;
    if (flagged) flagged[0x8c] = 0;
    allowing = 1;
    native_reached_finish(rules, racer);
    allowing = 0;
    if (flagged) flagged[0x8c] = 1;
}

/* The lap total's text: "%d" normally, "" (the same string's end) in an elimination race. */
static void hide_total(int hide) {
    if (!total_format || hide == total_hidden) return;
    total_hidden = hide;
    *total_format = (unsigned char)(hide ? total_low + 2 : total_low);
}

/* 0xe63f20 (hud, lap), from runtime_replay.c: no lap cap and no "final lap" in an elimination race. */
void runtime_elimination_hud_lap(unsigned char *hud) {
    if (!hud || !elimination_race()) return;
    hide_total(1);
    const int no_cap = 99;
    if (integer(hud, 0x84) < no_cap) memcpy(hud + 0x84, &no_cap, 4);
}

/* The racers still in the race, by race position (the leader first). */
static int in_race(unsigned char **items, int count, unsigned char **out) {
    int n = 0;
    for (int i = 0; i < count; ++i)
        if (items[i] && !items[i][0x8c] && items[i] != out_player && !is_out(items[i])) out[n++] = items[i];
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && integer(out[j], 0x58) < integer(out[j - 1], 0x58); --j) {
            unsigned char *t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

/* The game ranks by where the karts are, so a racer still in that drives back behind a kart out would show a
 * place under it. Every frame: a racer out keeps the place it went out in, the ones still in take 1..n in the
 * game's order. */
static void hold_places(unsigned char **items, int count) {
    if (!out_count) return;
    unsigned char *left[MAX_RACERS];
    const int n = in_race(items, count, left);
    int fixed = 0;
    char what[256];
    size_t used = 0;
    what[0] = 0;
#define NOTE_FIX(r, was, now_place)                                                                            \
    do {                                                                                                       \
        ++fixed;                                                                                               \
        if (used + 40 < sizeof what)                                                                           \
            used += (size_t)snprintf(what + used, sizeof what - used, " [%s%s %d->%d]", (r)[0x40] ? "AI" : "player", \
                                     (r)[0x8c] ? " finished" : "", was, now_place);                            \
    } while (0)
    for (int i = 0; i < n; ++i) {
        const int place = i + 1, was = integer(left[i], 0x58);
        if (was != place) { memcpy(left[i] + 0x58, &place, 4); NOTE_FIX(left[i], was, place); }
    }
    for (int k = 0; k < out_count; ++k) {   /* the k-th out went out in place count - k */
        unsigned char *r = out_racers[k];
        int current = 0;
        for (int i = 0; i < count && !current; ++i) current = items[i] == r;
        const int place = count - k, was = current ? integer(r, 0x58) : place;
        if (was != place) {
            memcpy(r + 0x58, &place, 4);
            NOTE_FIX(r, was, place);
        }
    }
#undef NOTE_FIX
    static unsigned long long said;
    const unsigned long long now = now_ms();
    if (fixed && now - said > 2000) {
        said = now;
        printf("Elimination: %d race positions put back:%s\n", fixed, what);
    }
}

/* ---- the wide view turned around the racer with the right stick ----
 * The kart camera's update (0xc890c0, CKartCamera: rdi the camera, xmm0 the frame time) ends with the camera's place
 * at +0x100 (x, y, z; z up) and its angles at +0x10c (roll, pitch, yaw, radians: atan2 / asin / atan2 of its
 * rotation, wrapped by 0xa98310). Right after it, while the wide view is on, the place turns around the racer watched
 * by `orbit` (about z) and the yaw by the same angle, so the camera keeps looking at the racer. The yaw's sense is
 * learnt from the game's own frames (how the yaw follows the direction to the racer). */
void *runtime_replay_wide_camera(void);   /* runtime_replay_play.c */
static void jump(unsigned char *at, const void *target);
static int writable(void *address, size_t length);
static double orbit;                       /* radians, the right stick adds to it */
static int yaw_score;                      /* > 0: the yaw turns as atan2(dy, dx) does */
static void *orbit_kart;                   /* the kart the camera looks at */

static float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static void ABI camera_after(unsigned char *camera) {
    if (!camera || camera != runtime_replay_wide_camera() || !orbit_kart) return;
    float kart[3], *at = (float *)(camera + 0x100), *angles = (float *)(camera + 0x10c);
    get_position(kart, orbit_kart);
    const float dx = kart[0] - at[0], dy = kart[1] - at[1];
    /* the yaw's sense, from the game's own camera (before we turn it) */
    static float last_dir, last_yaw;
    static int seeded;
    const float dir = atan2f(dy, dx), yaw = angles[2];
    if (seeded) {
        const float ddir = wrap_pi(dir - last_dir), dyaw = wrap_pi(yaw - last_yaw);
        if (fabsf(ddir) > 0.002f && fabsf(dyaw) > 0.002f && fabsf(ddir) < 0.5f && yaw_score > -200 && yaw_score < 200)
            yaw_score += ddir * dyaw > 0 ? 1 : -1;
    }
    last_dir = dir, last_yaw = yaw, seeded = 1;
    if (orbit == 0.0) return;
    const float c = cosf((float)orbit), s = sinf((float)orbit);
    const float rx = at[0] - kart[0], ry = at[1] - kart[1];
    at[0] = kart[0] + rx * c - ry * s;
    at[1] = kart[1] + rx * s + ry * c;
    angles[2] = wrap_pi(yaw + (yaw_score >= 0 ? 1.0f : -1.0f) * (float)orbit);
}

static void install_camera_turn(unsigned char *image, unsigned char *stub) {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x158 */
    static const unsigned char code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,
                                         0x48,0x81,0xec,0x58,0x01,0x00,0x00};
    unsigned char *at = image + 0xc890c0;
    if (memcmp(at, code, sizeof code) || !writable(at, 14)) {
        puts("Elimination: the kart camera's update is not as expected: no turning the wide view");
        return;
    }
    /* the original: its first instructions, then back */
    unsigned char *original = stub;
    memcpy(original, code, sizeof code);
    jump(original + sizeof code, at + sizeof code);
    /* the wrapper: push rdi; call original; pop rdi; push rax; call camera_after(rdi); pop rax; ret */
    unsigned char *w = stub + 64;
    size_t k = 0;
    const void *after = (const void *)camera_after;
    w[k++] = 0x57;
    w[k++] = 0x48; w[k++] = 0xb8; memcpy(w + k, &original, 8); k += 8;
    w[k++] = 0xff; w[k++] = 0xd0;
    w[k++] = 0x5f;
    w[k++] = 0x50;
    w[k++] = 0x48; w[k++] = 0xb8; memcpy(w + k, &after, 8); k += 8;
    w[k++] = 0xff; w[k++] = 0xd0;
    w[k++] = 0x58;
    w[k++] = 0xc3;
    jump(at, w);
    puts("Elimination: the wide view turns with the right stick (kart camera update hooked)");
}

/* The spectator's state for the overlay's button legend (runtime_elimination_spectator), any thread. */
static pthread_mutex_t spectator_lock = PTHREAD_MUTEX_INITIALIZER;
static char spectator_name[48];
static int spectator_place, spectator_lap, spectator_wide, spectator_on;

static void spectator_show(int on, unsigned char *racer, int wide_on) {
    char name[48] = "";
    if (on && racer) racer_name(racer, name, sizeof name);
    if (strlen(name) > 18) memcpy(name + 15, "...", 4);   /* a long name: cut, the box stays small */
    pthread_mutex_lock(&spectator_lock);
    spectator_on = on;
    snprintf(spectator_name, sizeof spectator_name, "%s", name);
    spectator_place = on && racer ? integer(racer, 0x58) : 0;
    spectator_lap = on && racer ? integer(racer, 0x4c) : 0;
    spectator_wide = wide_on;
    pthread_mutex_unlock(&spectator_lock);
}

/* For the overlay: 1 while the local player out watches the race; the racer watched, its place and lap, and whether
 * the wide view (L3) is on. NULL buffers: only whether it shows. */
int runtime_elimination_spectator(char *name, size_t size, int *place, int *lap, int *wide_on) {
    pthread_mutex_lock(&spectator_lock);
    const int on = spectator_on && !runtime_elimination_banner(NULL, 0, NULL, 0, NULL, NULL);
    if (on && name) snprintf(name, size, "%s", spectator_name);
    if (on && place) *place = spectator_place;
    if (on && lap) *lap = spectator_lap;
    if (on && wide_on) *wide_on = spectator_wide;
    pthread_mutex_unlock(&spectator_lock);
    return on;
}

static void spectate(unsigned char **items, int count) {
    unsigned char *left[MAX_RACERS];
    const int n = in_race(items, count, left);
    if (!n) return;
    /* L3: the wide view of the racer watched, on and off */
    static uint32_t buttons_seen;
    const uint32_t buttons = runtime_pad_host_buttons();
    static int wide_wanted;
    if ((buttons & 0x2) && !(buttons_seen & 0x2)) {
        wide_wanted = !wide_wanted;
        printf("Elimination: L3, wide view %s\n", wide_wanted ? "on" : "off");
    }
    buttons_seen = buttons;
    if (!wide_wanted) runtime_replay_spectate_wide(0);
    /* the right stick turns the wide view (a full turn in about 2.5 s); off, it comes back behind the racer */
    {
        static unsigned long long stick_ms;
        const unsigned long long now = now_ms();
        const double dt = stick_ms && now > stick_ms && now - stick_ms < 250 ? (double)(now - stick_ms) / 1000.0 : 0.0;
        stick_ms = now;
        const int x = (int)(runtime_pad_host_right_stick() & 0xff) - 128;
        const double v = x > 24 || x < -24 ? x / 127.0 : 0.0;
        if (wide_wanted) orbit += v * dt * 2.5;
        else orbit = 0.0;
        while (orbit > 3.14159265) orbit -= 6.2831853;
        while (orbit < -3.14159265) orbit += 6.2831853;
    }
    int at = -1;
    for (int i = 0; i < n; ++i) if (left[i] == watched) at = i;
    if (at < 0) at = 0;  /* the start, or the one watched is out: the leader */
    const int step = runtime_replay_spectate(handle_object((void **)(left[at] + 0x20)));
    if (step) {
        at = ((at + step) % n + n) % n;
        runtime_replay_spectate(handle_object((void **)(left[at] + 0x20)));
    }
    /* after the camera is on the racer (a new camera for a new kart): the wide view on it */
    const int wide_now = wide_wanted ? runtime_replay_spectate_wide(1) : 0;
    orbit_kart = handle_object((void **)(left[at] + 0x20));
    runtime_replay_hud_racer(left[at]);
    if (watched != left[at]) printf("Elimination: spectating racer in place %d\n", integer(left[at], 0x58));
    watched = left[at];
    spectator_show(1, watched, wide_now);
}

/* A racer goes out in `place`. Only the PC that drives a kart stops it and finishes it (its own player at the end of
 * the race, an AI now); the others see that through the race. */
static void eliminate(unsigned char *r, int place) {
    if (!r || is_out(r) || out_count >= MAX_RACERS) return;
    out_racers[out_count++] = r;
    const int mine = local_human(r), ai = local_ai(r);
    printf("Elimination: racer out in place %d at %.1f s (%s)\n", place, race_clock,
           mine ? "this PC's player" : ai ? "this PC's AI" : "another PC's");
    if (mine) out_stopped = stopped_count;
    if (mine || ai) stop(r, place);
    notice(r, 0, place);
    if (mine) {  /* the local player: out, no end of race yet */
        out_player = r;
        out_place = place;
        spectating = 1;
        watched = NULL;
        puts("Elimination: the player is out, spectator camera (d-pad right next, left previous)");
    } else if (ai && !r[0x8c]) {
        finish(r, place);
    }
    ++eliminated;
}

/* ---- online: the host's list of the racers out, to the guests --------------------------------------------------
 * A RYUPN1 packet (as runtime_ai_catchup.c's) next to the game's own packets to each player (runtime_net.c):
 *   "RYUPN1", kind 6, version 1, race (4, big endian), count, ids[8] (runtime_online_racer_id, the host's numbering)
 * The host sends it to each player every 150 ms while the race lasts; a guest takes it only from its room's host, and
 * only a few seconds into its own race (an old race's list must not cut anyone at the start). */
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t race; int count, active; signed char ids[MAX_RACERS]; } net_out, net_in;
static int net_ready;             /* the guest's race is running: lists are taken */
static int net_applied;           /* the guest: entries of the list applied */
static struct { char name[24]; unsigned long long sent; } net_peers[8];

static void net_publish(void) {
    if (guest()) return;
    unsigned char **items;
    racers(&items);
    pthread_mutex_lock(&net_lock);
    net_out.count = 0;
    for (int i = 0; i < out_count && i < MAX_RACERS; ++i) net_out.ids[net_out.count++] = (signed char)runtime_online_racer_id(out_racers[i]);
    net_out.active = 1;
    pthread_mutex_unlock(&net_lock);
}

size_t runtime_elimination_net_send(const char *peer, uint8_t *out, uint64_t now) {
    size_t n = 0;
    if (!peer || !*peer) return 0;
    pthread_mutex_lock(&net_lock);
    if (net_out.active) {
        int slot = -1;
        for (int i = 0; i < 8 && slot < 0; ++i) if (!strcmp(net_peers[i].name, peer)) slot = i;
        for (int i = 0; i < 8 && slot < 0; ++i) if (!net_peers[i].name[0]) { slot = i; snprintf(net_peers[i].name, 24, "%s", peer); }
        if (slot >= 0 && (!net_peers[slot].sent || now - net_peers[slot].sent >= 150)) {
            net_peers[slot].sent = now;
            memset(out, 0, ELIMINATION_PACKET);
            memcpy(out, "RYUPN1", 6);
            out[6] = 6;
            out[7] = 1;
            out[8] = (uint8_t)(net_out.race >> 24), out[9] = (uint8_t)(net_out.race >> 16);
            out[10] = (uint8_t)(net_out.race >> 8), out[11] = (uint8_t)net_out.race;
            out[12] = (uint8_t)net_out.count;
            memcpy(out + 13, net_out.ids, MAX_RACERS);
            n = ELIMINATION_PACKET;
        }
    }
    pthread_mutex_unlock(&net_lock);
    return n;
}

int runtime_elimination_net_receive(const char *peer, const uint8_t *in, size_t len, uint64_t now) {
    (void)now;
    if (len < 8 || memcmp(in, "RYUPN1", 6) || in[6] != 6) return 0;
    if (len != ELIMINATION_PACKET || in[7] != 1 || !peer || !*peer || in[12] > MAX_RACERS) return 1;
    char host[24];
    runtime_room_mode_host_name(host);
    if (!guest() || strcasecmp(peer, host)) return 1;
    const uint32_t race = (uint32_t)in[8] << 24 | (uint32_t)in[9] << 16 | (uint32_t)in[10] << 8 | in[11];
    pthread_mutex_lock(&net_lock);
    if (net_ready && (race != net_in.race || in[12] > net_in.count)) {
        if (race != net_in.race) printf("Elimination: the host's race list (%08x)\n", race);
        net_in.race = race;
        net_in.count = in[12];
        memcpy(net_in.ids, in + 13, MAX_RACERS);
        net_in.active = 1;
    }
    pthread_mutex_unlock(&net_lock);
    return 1;
}

/* Every frame of an elimination race. The guest applies the host's list (place = racers - its order). */
static void net_tick(unsigned char **items, int count) {
    if (!guest()) return;
    pthread_mutex_lock(&net_lock);
    if (!net_ready && race_clock > 3.0) net_ready = 1;
    const int listed = net_in.active ? net_in.count : 0;
    signed char ids[MAX_RACERS];
    memcpy(ids, net_in.ids, sizeof ids);
    pthread_mutex_unlock(&net_lock);
    while (net_applied < listed) {
        unsigned char *r = NULL;
        for (int i = 0; i < count && !r; ++i)
            if (items[i] && runtime_online_racer_id(items[i]) == ids[net_applied]) r = items[i];
        if (!r) {   /* not known here (yet): try again next frame */
            static unsigned long long told;
            if (now_ms() - told > 3000) told = now_ms(), printf("Elimination: the host's racer id %d is not in this race\n", ids[net_applied]);
            return;
        }
        eliminate(r, count - net_applied);
        ++net_applied;
    }
}

/* A racer out whose kart this PC drives now (a player who left the race: the game gives its kart to the host's AI)
 * stays where it is too: stopped here, and finished as an AI out is. */
static void keep_out_still(unsigned char **items, int count) {
    for (int k = 0; k < out_count; ++k) {
        unsigned char *r = out_racers[k];
        if (!r || r == out_player) continue;
        int current = 0;
        for (int i = 0; i < count && !current; ++i) current = items[i] == r;
        if (!current || !r[0xb0]) continue;   /* gone from the race, or still another PC's */
        unsigned char *kart = handle_object((void **)(r + 0x20));
        int held = 0;
        for (int i = 0; i < stopped_count && !held; ++i) held = stopped[i].kart == kart;
        if (held || !kart) continue;
        const int place = count - k;
        printf("Elimination: a racer out (place %d) is driven by this PC now (it left?): stopped here\n", place);
        stop(r, place);
        if (local_ai(r) && !r[0x8c] && rules) finish(r, place);
    }
}

void runtime_elimination_tick(void) {
    if (!installed) return;
    hide_total(elimination_race());
    short_end_race(elimination_race());
    unsigned char **items;
    const int count = racers(&items);
    static void *race_seen;
    void *race = count ? (void *)items : NULL;
    if (race_over && count) {   /* a retry can keep the same racers: nobody finished any more = a new race */
        int finished = 0;
        for (int i = 0; i < count; ++i) finished |= items[i] && items[i][0x8c];
        if (!finished) race_seen = NULL;
    }
    /* a restart from the pause menu keeps the racers too: the game's race clock goes back. The player out gets
     * its own camera and HUD back, and the mode starts over. */
    static float clock_seen;
    const float game_clock = count ? rr_race_clock() : 0.0f;
    if (count && clock_seen > 2.0f && game_clock < clock_seen - 1.0f) {
        printf("Elimination: the race restarted (clock %.1f -> %.1f s)\n", clock_seen, game_clock);
        if (out_player) {
            runtime_replay_spectate(handle_object((void **)(out_player + 0x20)));
            runtime_replay_hud_racer(out_player);
        }
        race_seen = NULL;
    }
    clock_seen = game_clock;
    if (race != race_seen) {   /* a race started or ended */
        race_seen = race;
        new_race();
        if (race && elimination_race()) net_publish();
        if (race) {
            static const int seconds[] = {30, 45, 60, 80, 90, 120, 150};
            const int laps = race_settings && *race_settings ? integer(*race_settings, 0xc) : 0;
            interval = laps >= 1 && laps <= 7 ? seconds[laps - 1] : laps >= 10 && laps <= 600 ? laps : ELIMINATION_SECONDS;
            next_cut = interval;
            printf("Elimination: %d laps in the menu -> one out every %.0f s\n", laps, interval);
            printf("Elimination: race with %d racers, private room mode %s, %s -> %s\n", count,
                   runtime_elimination_mode_enabled() ? "elimination" : "other", guest() ? "guest" : "host",
                   elimination_race() ? "elimination" : "normal");
        }
    }
    if (!elimination_race()) {
        set_ghost_karts(0);
        return;
    }
    set_ghost_karts(!race_over);
    /* the clock runs while karts move: not before the start, not while paused */
    int moving = 0;
    for (int i = 0; i < count; ++i) {
        if (!items[i] || items[i][0x8c]) continue;
        const float distance = real(items[i], 0x48);
        if (distance != last_distance[i]) moving = 1;
        last_distance[i] = distance;
    }
    const unsigned long long now = now_ms();
    const double dt = last_ms && now > last_ms ? (double)(now - last_ms) / 1000.0 : 0;
    last_ms = now;
    if (moving && dt < 0.25) race_clock += dt;
    if (race_over) {   /* results, retry: the karts are the game's again */
        static unsigned long long told;
        const unsigned long long t = now_ms();
        if (t - told > 3000) {   /* for the log: what the end of the race is waiting for */
            told = t;
            char what[160];
            size_t used = 0;
            what[0] = 0;
            for (int i = 0; i < count && used + 24 < sizeof what; ++i)
                if (items[i]) used += (size_t)snprintf(what + used, sizeof what - used, " [%s %s %d]",
                                                       items[i][0x40] ? "AI" : "player", items[i][0x8c] ? "finished" : "racing",
                                                       integer(items[i], 0x60));
            printf("Elimination: race over, the game still has the race:%s\n", what);
        }
        return;
    }
    keep_stopped(items, count);
    hold_places(items, count);
    if (spectating) {
        spectate(items, count);
        runtime_replay_camera_triggers();   /* every frame: triggers made later get it too */
    }
    /* a guest does not cut: its next cut is the one after the host's cuts it has (the alarm before each one) */
    if (guest()) next_cut = interval * (double)(out_count + 1);
    /* the timer blinks red before a cut, while there is someone left to cut */
    const double to_cut = next_cut - race_clock;
    const int warn = still_racing() >= 2 && to_cut > 0 && to_cut <= WARNING_SECONDS;
    runtime_replay_timer_warning(warn && (int)(to_cut * 4.0) % 2 == 0);
    const int second = (int)to_cut;   /* 9 .. 0: the alarm once a second through the last 10 s */
    if (warn && (alarm_cut != next_cut || second != alarm_second)) {
        alarm_cut = next_cut;
        alarm_second = second;
        unsigned char *me = local_racer(items, count);
        unsigned char *kart = me ? handle_object((void **)(me + 0x20)) : NULL;
        void *sound = NULL, *callback[2] = {NULL, NULL};
        if (kart) play_sound(kart, "UI_Timer_Low_Warning", &sound, callback);
        printf("Elimination: low time alarm, sound %s (%.0f s to the cut)\n", sound ? "playing" : "not found", to_cut);
    }
    runtime_replay_hide_lap_total(1);   /* the "/" before the empty lap total */
    net_tick(items, count);
    keep_out_still(items, count);
    if (!rules) return;

    const int left = still_racing();
    /* racers still in, finished or not (a player who left the race is not in the list any more) */
    int in_race_now = 0;
    for (int i = 0; i < count; ++i) if (items[i] && !is_out(items[i]) && items[i] != out_player) ++in_race_now;
    if (out_count && in_race_now <= 1) {  /* the survivor wins */
        if (out_player && !out_finished) {
            printf("Elimination: the player finishes in place %d\n", out_place);
            /* the end of the race is the local player's: its HUD and camera back on its own racer first (the HUD
             * watched another racer, and the game's end of race waits for its own racer's finish) */
            runtime_replay_spectate_wide(0);
            runtime_replay_spectate(handle_object((void **)(out_player + 0x20)));
            runtime_replay_hud_racer(out_player);
            spectating = 0;
            out_player[0x8c] = 0;   /* the game's finish runs only for a racer not finished yet */
            out_finished = 1;
            finish(out_player, out_place);
        }
        for (int i = 0; i < count; ++i)
            if (items[i] && !is_out(items[i]) && items[i] != out_player) {
                printf("Elimination: racer %d wins at %.1f s (%s)\n", i, race_clock,
                       local_human(items[i]) ? "this PC's player" : local_ai(items[i]) ? "this PC's AI" : "another PC's");
                notice(items[i], 1, 1);
                /* every PC finishes the winner: the game does not send a guest's finish to the host (a finish is
                 * each PC's own line check, which this mode turns off), and the host ends the race online */
                if (!items[i][0x8c]) finish(items[i], 1);
                printf("Elimination: the game %s the race\n", rules && ((unsigned char *)rules)[0x25] ? "ended" : "did NOT end");
            }
        race_over = 1;
        spectator_show(0, NULL, 0);
        runtime_replay_spectate_wide(0);
        return;
    }
    if (guest() || left < 2 || race_clock < next_cut) return;   /* a guest takes the host's cuts (net_tick) */
    next_cut += interval;
    /* last place: the game's race position (the HUD's); on a tie, the least progress (laps + part of the lap) */
    unsigned char *last = NULL;
    int worst = -1, last_index = -1;
    double least = 1e30;
    for (int i = 0; i < count; ++i) {
        unsigned char *r = items[i];
        if (!r || r[0x8c] || r == out_player || is_out(r)) continue;
        const int place = integer(r, 0x58);
        const float ratio = real(r, 0x44);
        const double progress = integer(r, 0x4c) + (ratio >= 0 && ratio <= 1 ? ratio : 0);
        if (place > worst || (place == worst && progress < least)) {
            worst = place;
            least = progress;
            last = r;
            last_index = i;
        }
    }
    if (!last) return;
    printf("Elimination: last place by the game: position %d (racer %d)\n", worst, last_index);
    eliminate(last, count - out_count);
    net_publish();
}

static void new_race(void) {
    rules = NULL;
    allowing = 0;
    race_clock = 0;
    interval = ELIMINATION_SECONDS;
    next_cut = interval;
    out_stopped = -1;
    last_ms = 0;
    eliminated = 0;
    race_over = 0;
    alarm_cut = 0;
    alarm_second = -1;
    pthread_mutex_lock(&banner_lock);
    banner[0] = 0;
    banner_shown = 0;
    pthread_mutex_unlock(&banner_lock);
    spectating = 0;
    spectator_show(0, NULL, 0);
    runtime_replay_spectate_wide(0);
    orbit = 0.0;
    orbit_kart = NULL;
    out_player = NULL;
    out_place = 0;
    out_finished = 0;
    stopped_count = 0;
    watched = NULL;
    memset(last_distance, 0, sizeof last_distance);
    memset(out_racers, 0, sizeof out_racers);
    out_count = 0;
    pthread_mutex_lock(&net_lock);
    memset(&net_out, 0, sizeof net_out);
    memset(&net_in, 0, sizeof net_in);
    net_out.race = (uint32_t)now_ms() ^ 0x5a17e11u;
    memset(net_peers, 0, sizeof net_peers);
    net_ready = 0;
    net_applied = 0;
    pthread_mutex_unlock(&net_lock);
}

/* A new track or the menus: nothing carries over (the tick also notices a new race by itself). */
void runtime_elimination_map_loading(const char *map) {
    (void)map;
    new_race();
}

static void jump(unsigned char *at, const void *target);
static int writable(void *address, size_t length);

/* ---- the lap selector: seconds ------------------------------------------------------------- */
static int lap_seconds[] = {30, 45, 60, 80, 90, 120, 150, 180};

/* The lap selector shows seconds in a private room whose mode is Elimination (the host's settings). */
static int switch_on(void) { return runtime_elimination_mode_enabled() && runtime_online_mode_in_private(); }

typedef void (ABI *SetText)(void *, const char *);
static SetText set_text;   /* 0x268ef0 (placeable, text), as the replay player shows its texts */
typedef char *(ABI *Intern)(void *, const char *);
static Intern intern;      /* 0x1574080 (pool, text): a game string; released with 0x1518e30(pool, string - 0x10) */
static void **string_pool; /* [0x2af2600] */

static int same_word(const char *text, const char *word);
static const char *text_of(unsigned char *placeable);
static int readable(const void *p, size_t n);

/* CGuiBehaviorBaseSettingSelector: _optionName (the title, a game string) and right after it _nameTextPlaceable
 * (a handle to the text it is drawn in). Found by the title's own text among the selector's fields. */
static const char *const lap_words[] = {"Vueltas", "Laps", "Tours", "Voltas", "Runden", "Giri"};
static const char *const second_words[] = {"Segundos", "Seconds", "Secondes", "Segundos", "Sekunden", "Secondi"};
#define TITLE_WORDS 6
static int to_seconds = 1;   /* the titles' direction: laps -> seconds (1) or back (0) */

static int retitle_selector(unsigned char *selector) {
    const char *const *laps = to_seconds ? lap_words : second_words;
    const char *const *seconds = to_seconds ? second_words : lap_words;
    for (size_t off = 0x40; off < 0x1c0; off += 8) {
        unsigned char *field = selector + off;
        const char *text = text_of(field - 0x30);   /* the string at selector + off */
        for (int i = 0; text && i < TITLE_WORDS; ++i) {
            if (!same_word(text, laps[i])) continue;
            char *title = string_pool && *string_pool ? intern(*string_pool, seconds[i]) : NULL;
            /* the old string is left as it is (a few bytes): freeing what is not a pooled string would crash */
            if (title) memcpy(field, &title, 8);   /* the new string keeps the reference intern gave it */
            void *slot = ptr(field, 8);
            unsigned char *shown = slot && readable(slot, 0x30) ? handle_object((void **)(field + 8)) : NULL;
            if (shown && !readable(shown, 0x40)) shown = NULL;
            /* only a text that shows the same title: anything else is not the title's placeable */
            const char *now = shown ? text_of(shown) : NULL;
            if (now && same_word(now, laps[i])) set_text(shown, seconds[i]);
            else shown = NULL;
            printf("Elimination: the selector's title \"%s\" -> \"%s\" (+0x%zx, text %p)\n", laps[i], seconds[i], off,
                   (void *)shown);
            return 1;
        }
    }
    return 0;
}

static int same_word(const char *text, const char *word) {
    for (; *word; ++text, ++word)
        if (!*text || ((*text | 0x20) != (*word | 0x20))) return 0;
    return !*text;
}

/* 1 when [p, p + n) is mapped and readable (a field read as a pointer may hold a number). */
static int readable(const void *p, size_t n) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT) return 0;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                       PAGE_EXECUTE_WRITECOPY))) return 0;
    return (const unsigned char *)p + n <= (const unsigned char *)m.BaseAddress + m.RegionSize;
#else
    (void)n;
    return p != NULL;
#endif
}

/* A text placeable's text (+0x30), NULL when it does not read as one. */
static const char *text_of(unsigned char *placeable) {
    const char *text = placeable ? ptr(placeable, 0x30) : NULL;
    if (!text || (uintptr_t)text < 0x10000 || ((uintptr_t)text & 1) || !readable(text, 24)) return NULL;
    for (int i = 0; i < 24; ++i) {
        if (!text[i]) return text;
        if ((unsigned char)text[i] < 0x20) return NULL;
    }
    return NULL;
}

/* Under `placeable` (children +0x48), the lap title: "Vueltas", "Laps", "Tours", "Voltas" -> seconds. */
static int retitle(unsigned char *placeable, int depth) {
    if (!placeable || depth > 4) return 0;
    const char *const *laps = to_seconds ? lap_words : second_words;
    const char *const *seconds = to_seconds ? second_words : lap_words;
    const char *text = text_of(placeable);
    for (int i = 0; text && i < TITLE_WORDS; ++i)
        if (same_word(text, laps[i])) {
            set_text(placeable, seconds[i]);
            printf("Elimination: the selector's title \"%s\" -> \"%s\"\n", laps[i], seconds[i]);
            return 1;
        }
    void *children = ptr(placeable, 0x48);
    const int count = children ? integer(children, 0xc) : 0;
    unsigned char **items = children ? ptr(children, 0x20) : NULL;
    for (int i = 0; items && i < count && i < 64; ++i)
        if (retitle(items[i], depth + 1)) return 1;
    return 0;
}

/* For the log when the title is not found: the texts under `placeable`. */
static void list_texts(unsigned char *placeable, int depth, int *left) {
    if (!placeable || depth > 4 || *left <= 0) return;
    const char *text = text_of(placeable);
    if (text && *text) {
        printf("Elimination:   text \"%s\" at depth %d\n", text, depth);
        --*left;
    }
    void *children = ptr(placeable, 0x48);
    const int count = children ? integer(children, 0xc) : 0;
    unsigned char **items = children ? ptr(children, 0x20) : NULL;
    for (int i = 0; items && i < count && i < 64; ++i) list_texts(items[i], depth + 1, left);
}

/* The placeable a behavior belongs to: igGuiBehavior +0x10 _owner (a handle); else, among its first fields, the
 * object whose behaviour list (+0xe8) holds it. */
static unsigned char *owner_of(unsigned char *behavior) {
    unsigned char *owner = handle_object((void **)(behavior + 0x10));
    if (owner && (uintptr_t)owner >= 0x10000 && !((uintptr_t)owner & 7)) return owner;
    for (size_t off = 0x10; off <= 0x40; off += 8) {
        unsigned char *p = ptr(behavior, off);
        if (!p || (uintptr_t)p < 0x10000 || ((uintptr_t)p & 7)) continue;
        void *list = ptr(p, 0xe8);
        if (!list || (uintptr_t)list < 0x10000 || ((uintptr_t)list & 7)) continue;
        const int count = integer(list, 0xc);
        void **items = ptr(list, 0x20);
        for (int i = 0; items && count > 0 && count <= 8 && i < count; ++i)
            if (items[i] == behavior) return p;
    }
    return NULL;
}

/* The selector's title, laps <-> seconds (to_seconds): its own string, or a text placeable around it. */
static void retitle_all(unsigned char *selector) {
    if (retitle_selector(selector)) return;
    unsigned char *owner = owner_of(selector);
    unsigned char *parent = owner ? ptr(owner, 0x40) : NULL;
    if (!owner || !(retitle(owner, 0) || retitle(parent, 0) || retitle(parent ? ptr(parent, 0x40) : NULL, 0))) {
        printf("Elimination: the selector's title was not found (owner %p)\n", (void *)owner);
        int left = 30;
        list_texts(parent ? ptr(parent, 0x40) : owner, 0, &left);
    }
}

/* Before 0x1268440 / 0x128d380 (the lap selector reads its list): in an Elimination room the game's laps (3/5/7)
 * become our seconds; another mode gets its laps back (the list, the choice, the race settings' laps and the title). */
static struct { unsigned char *selector; void *data; int n, bytes, choice; int values[16]; } lap_saved;
static void ABI lap_menu(unsigned char *selector) {
    if (!selector) return;
    unsigned char *list = ptr(selector, 0x1c0);
    const int *values = list ? ptr(list, 0x20) : NULL;
    if (!values) return;
    const int count = integer(list, 0xc);
    const int ours = values == lap_seconds;
    if (switch_on() && !ours) {
        if (count < 2 || count > 16) return;
        for (int i = 0; i < count; ++i) if (values[i] < 1 || values[i] > 9) return;   /* not a lap list */
        lap_saved.selector = selector;
        lap_saved.data = (void *)values;
        lap_saved.n = count;
        lap_saved.bytes = integer(list, 0x18);
        lap_saved.choice = integer(selector, 0x190);
        memcpy(lap_saved.values, values, (size_t)count * 4);
        const int n = (int)(sizeof lap_seconds / sizeof *lap_seconds), bytes = n * 4, chosen = 3;   /* 80 s */
        void *data = lap_seconds;
        memcpy(list + 0x20, &data, 8);
        memcpy(list + 0xc, &n, 4);
        memcpy(list + 0x10, &n, 4);
        memcpy(list + 0x18, &bytes, 4);
        memcpy(selector + 0x190, &chosen, 4);
        memcpy(selector + 0x1a0, &chosen, 4);
        if (race_settings && *race_settings) memcpy((unsigned char *)*race_settings + 0xc, &lap_seconds[chosen], 4);
        printf("Elimination: the lap selector shows seconds (30 .. 180; it had %d laps)\n", count);
        to_seconds = 1;
        retitle_all(selector);
    } else if (!switch_on() && ours && lap_saved.selector == selector && lap_saved.data) {
        memcpy(list + 0x20, &lap_saved.data, 8);
        memcpy(list + 0xc, &lap_saved.n, 4);
        memcpy(list + 0x10, &lap_saved.n, 4);
        memcpy(list + 0x18, &lap_saved.bytes, 4);
        const int choice = lap_saved.choice >= 0 && lap_saved.choice < lap_saved.n ? lap_saved.choice : 0;
        memcpy(selector + 0x190, &choice, 4);
        memcpy(selector + 0x1a0, &choice, 4);
        if (race_settings && *race_settings) memcpy((unsigned char *)*race_settings + 0xc, &lap_saved.values[choice], 4);
        printf("Elimination: the lap selector shows laps again (%d)\n", lap_saved.values[choice]);
        to_seconds = 0;
        retitle_all(selector);
        lap_saved.selector = NULL;
    }
}

/* A call to fn(rdi) before `at`'s first `n` bytes, which are moved into the stub; then back into the game. */
static size_t gate(unsigned char *stub, unsigned char *at, size_t n, void (ABI *fn)(unsigned char *)) {
    static const unsigned char save[] = {0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x50,0x41,0x52,0x41,0x53};
    static const unsigned char restore[] = {0x41,0x5b,0x41,0x5a,0x58,0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f};
    size_t k = 0;
    memcpy(stub + k, save, sizeof save); k += sizeof save;           /* 9 pushes: the call is 16-aligned */
    stub[k++] = 0x48; stub[k++] = 0xb8;
    memcpy(stub + k, &fn, 8); k += 8;                                  /* movabs rax, fn */
    stub[k++] = 0xff; stub[k++] = 0xd0;                                /* call rax */
    memcpy(stub + k, restore, sizeof restore); k += sizeof restore;
    memcpy(stub + k, at, n); k += n;                                   /* the game's first instructions */
    memcpy(stub + k, "\xff\x25\0\0\0\0", 6);
    const void *back = at + n;
    memcpy(stub + k + 6, &back, 8);
    return k + 14;
}

static void install_lap_menu(unsigned char *image, unsigned char *stub) {
    /* push rbp; mov rbp,rsp; push r15..rbx; sub rsp,48 / 18 (then a rip-relative load, left in place) */
    static const unsigned char first[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48};
    static const unsigned char second[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    static const unsigned char digits_room[] = {0x48,0x85,0xf6,0x48,0x8d,0x57,0x18,0x48,0xb9,0,0,0,0,0x02,0,0,0,0x48,0x8b,0x03};
    static const unsigned char digits_guard[] = {0xc6,0x47,0x18,0x00,0xc6,0x47,0x1a,0xfd};
    unsigned char *a = at(image, 0x1268440, 1), *b = at(image, 0x128d380, 1);   /* EU-0x10 */
    if (memcmp(a, first, sizeof first) || memcmp(b, second, sizeof second) ||
        memcmp(a + 0x11, "\x4c\x8b\x25", 3) || memcmp(b + 0x11, "\x4c\x8b\x2d", 3)) {
        puts("Elimination: the lap selector is patched already (another lap mod?): its list is left as it is");
        return;
    }
    if (!writable(a, 14) || !writable(b, 14)) return;
    const size_t used = gate(stub, a, sizeof first, lap_menu);
    gate(stub + used, b, sizeof second, lap_menu);
    jump(a, stub);
    jump(b, stub + used);
    /* the menu's 1-digit number formatter: room for 3 digits (as the lap mod does) */
    unsigned char *room = image + 0x9b5bd4, *guard = image + 0x9b5bfb;
    if (!memcmp(room, digits_room, sizeof digits_room) && !memcmp(guard, digits_guard, sizeof digits_guard) &&
        writable(room, sizeof digits_room) && writable(guard, sizeof digits_guard)) {
        room[0xd] = 0x04;
        guard[6] = 0x1c;
    }
    puts("Elimination: lap selector hooks installed");
}

/* ---- install ------------------------------------------------------------------------------- */
static void jump(unsigned char *at, const void *target) {
    memcpy(at, "\xff\x25\0\0\0\0", 6);
    memcpy(at + 6, &target, 8);
}

static int writable(void *address, size_t length) {
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &old) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *start = (void *)((uintptr_t)address & ~(uintptr_t)(page - 1));
    return !mprotect(start, (unsigned char *)address + length - (unsigned char *)start,
                     PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

int runtime_elimination_start(unsigned char *image, size_t size) {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov rbx,rsi */
    static const unsigned char finish_code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x89,0xf3};
    /* push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; push rax */
    static const unsigned char lap_code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
    if (size < 0x2ad4b78) return 1;
    /* USA 1.21, or the European 1.21 (both functions 0x10 lower) */
    game_eu = memcmp(image + 0xf351b0, finish_code, sizeof finish_code) &&
              !memcmp(image + 0xf351a0, finish_code, sizeof finish_code);
    unsigned char *finish_at = at(image, 0xf351b0, 1), *lap_at = at(image, 0xf34fd0, 1);   /* EU-0x10 */
    if (memcmp(finish_at, finish_code, sizeof finish_code) || memcmp(lap_at, lap_code, sizeof lap_code)) {
        printf("Elimination mode: unexpected code at %s (another patch, or not USA/EU 1.21); mode off\n",
               memcmp(finish_at, finish_code, sizeof finish_code) ? "the finish (0xf351b0)" : "the lap start (0xf34fd0)");
        return 1;  /* not fatal: the mode stays off */
    }
#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 512, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 512, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) stub = NULL;
#endif
    if (!stub || !writable(finish_at, 14) || !writable(lap_at, 14)) return 1;
    /* the originals: their first instructions, then back into the game */
    memcpy(stub, finish_code, sizeof finish_code);
    jump(stub + sizeof finish_code, finish_at + sizeof finish_code);
    memcpy(stub + 64, lap_code, sizeof lap_code);
    jump(stub + 64 + sizeof lap_code, lap_at + sizeof lap_code);
    /* the lap total's format: lea rsi,[rip+0xf36eea] -> "%d" at 0x1d9af4a ("%d\0") */
    /* EU: the same site, lea rsi,[rip+0xf36eaa] -> "%d" at 0x1d9af0a */
    unsigned char total_code[] = {0x48,0x8d,0x35,0xea,0x6e,0xf3,0x00};
    if (game_eu) total_code[3] = 0xaa;
    if (!memcmp(image + 0xe64059, total_code, sizeof total_code) &&
        !memcmp(image + (game_eu ? 0x1d9af0a : 0x1d9af4a), "%d", 3) && writable(image + 0xe6405c, 1)) {
        total_format = image + 0xe6405c;
        total_low = total_code[3];
    } else
        puts("Elimination mode: the lap counter keeps its total (unexpected code at 0xe64059)");
    native_reached_finish = (ReachedFinish)stub;
    native_lap_started = (LapStarted)(stub + 64);
    resolve_handle = (ResolveHandle)at(image, 0x149fca0, 1);   /* EU-0x10 */
    meta_field = (MetaField)at(image, 0x14a7250, 1);           /* EU-0x10, as runtime_public_itemless.c */
    set_position = (SetPosition)(image + 0x3dc8e0);
    race_settings = (void **)(image + 0x2ad4e30);
    get_position = (GetPosition)(image + 0x3dc720);
    set_velocity = (SetVelocity)(image + 0xbb5060);
    play_sound = (PlaySound)at(image, 0x1024340, 1);   /* EU-0x10 */
    game_image = image;
    racer_manager = (void **)(image + 0x2ad4b70);
    client = (void **)(image + 0x2ab3cd8);
    manager = (void **)(image + 0x2acde48);
    jump(finish_at, (void *)reached_finish);
    jump(lap_at, (void *)lap_started);
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    set_text = (SetText)(image + 0x268ef0);
    intern = (Intern)at(image, 0x1574080, 1);   /* EU-0x10 */
    string_pool = (void **)(image + 0x2af2600);
    install_lap_menu(image, stub + 256);
#ifdef _WIN32
    unsigned char *turn = VirtualAlloc(NULL, 256, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *turn = mmap(NULL, 256, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (turn == MAP_FAILED) turn = NULL;
#endif
    if (turn) install_camera_turn(image, turn);
    installed = 1;
    printf("Elimination mode: installed (%s 1.21; a private room's Game Mode ELIMINATION)\n", game_eu ? "EU" : "USA");
    return 1;
}
