/* Native race replays (spectator build): the player.
 *
 * A replay is shown as a Single Race on its track: the recording player is the player's own kart (locked),
 * the other recorded racers are the AI karts, with the recorded drivers, outfits, karts and looks. Each kart
 * is driven by a ghost made from its recording (the game moves ghost-driven karts from keys) and gets the
 * recorded inputs. The camera follows one racer; time can be paused, slowed, sped up and rewound:
 *   L1 / R1 lap before / next, L2 / R2 racer before / next, Square pause, Up / Down speed, Left / Right rewind
 *   and fast forward. The race has 99 laps, so the player never finishes; nothing of a replay is saved.
 *
 * Ported from Erluinzo's Replay Mode 0.2.2 (github.com/Erluinzo/ctrnf-replay-mode, MIT, Copyright (c) 2026
 * Onur), play.c: this first part has the race, the ghosts, the inputs, the camera and the controls; the HUD
 * lines, sounds and the recorded items and hits come next. Started from the in-game menu's Replays tab
 * (runtime_replay_play_file), run on the game's thread by the pad read hook (runtime_replay_play_tick). */
#include "runtime_replay.h"
#include "runtime_replay_internal.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RACE_START 3.80f        /* ghost time of GO, as the game's own ghosts */
#define KEY_STEP 2              /* move keys every 2/100 s */
#define ANGLE_UNIT 10430.06f
static const float SPEEDS[] = {0.1f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
#define NORMAL_SPEED 2
#define SCRUB 4.0f
#define MAX_LAPS 9

/* the game's functions the player calls */
typedef void *ABI ghost_new_fn(int pool);
typedef void ABI list_fn(void *list, int n, int item_size);
typedef void ABI ghost_manager_fn(void *manager, void *ghost);
typedef void ABI ghost_attach_fn(void *manager, void *kart, int player_id);
typedef void ABI manager_fn(void *manager);
typedef int ABI ai_count_fn(void **out);
typedef void *ABI racer_of_fn(void *racer_manager, void *entity);
typedef void *ABI void_fn(void);
typedef void *ABI active_camera_fn(void *boxes);
typedef void ABI camera_refresh_fn(void *camera);
typedef void ABI make_handle_fn(void *slot, void *object);
typedef void *ABI pool_new_fn(void *pool);
typedef void ABI send_message_fn(void *message, void *target, void *sender);
typedef void *ABI player_kart_fn(void *kart_manager, int player, int one);
typedef void ABI void0_fn(void);
typedef char *ABI string_fn(void *pool, const char *text);
typedef void ABI string_release_fn(void *pool, char *header);
typedef void ABI release_fn(void *object);
typedef void ABI race_mode_fn(void *game, int mode);
typedef void ABI next_map_fn(const char *level);
typedef void ABI apply_choice_fn(void *choice, int index);
typedef void ABI player_count_fn(void *race_settings, int force);
typedef void *ABI player_at_fn(void *players, int index);
typedef void ABI customization_fn(char **back_menu, int players);
typedef void *ABI zone_by_name_fn(void *zones, const char *level);
typedef unsigned char ABI decal_find_fn(void *table, const char **kart, uint32_t hash, void ***out);
typedef void ABI player_input_fn(void *actor, void *command);
typedef void *ABI meta_of_fn(void *object);

static struct {
    ghost_new_fn *ghost_new, *move_key_new, *input_key_new;
    list_fn *list_reserve, *list_set_count;
    ghost_manager_fn *ghost_reset;
    ghost_attach_fn *ghost_attach;
    manager_fn *ghost_update, *ai_load;
    ai_count_fn *ai_count;
    racer_of_fn *racer_of;
    void_fn *camera_instance;
    active_camera_fn *active_camera;
    camera_refresh_fn *camera_refresh;
    make_handle_fn *make_handle;
    pool_new_fn *ghost_look_end_new;
    send_message_fn *send_message;
    player_kart_fn *player_kart;
    void0_fn *despawn_items, *load_next_map;
    string_fn *string;
    string_release_fn *string_release;
    release_fn *release;
    race_mode_fn *set_next_race_mode;
    next_map_fn *set_next_map;
    apply_choice_fn *apply_choice;
    player_count_fn *apply_player_count;
    player_at_fn *player_at;
    customization_fn *enter_customization;
    zone_by_name_fn *zone_by_name;
    decal_find_fn *decal_find;
} game;

enum {
    D_PLAYERS = 0x2ad1160, D_INPUT_MANAGER = 0x2ac2e90, D_KART_CAMERA_META = 0x2ac42e8, D_PLAYER_ENTITIES = 0x2ad0730,
    D_STRING_POOL = 0x2af2600,
};

/* ---- state ---- */
static struct {
    int pending;                /* 1: the replay's level is loading; 2: ghosts added, playing */
    struct rr_header header;
    struct rr_frame *frames;
    uint32_t count;             /* frames loaded */
    int player_slot;
    int kart_slot[RR_RACERS];   /* each racer's kart slot in the replay's race (0 the player's), -1 none */
    void *ghost[RR_RACERS];
    uint8_t attached[RR_RACERS];
    uint32_t fed[RR_RACERS];
    int32_t accolade_player;
    int rewound;
    void *kart;
    int set_up, started;
    float level_time;
    int watch;
    void *camera, *target;
    int speed, paused, scrubbing;
    float time;                 /* play time, s since GO */
    uint64_t buttons;
    int unghost_frames;
    void *player;               /* the local CPlayer whose character the replay changed */
    char *driver, *kart_name;   /* its own character and kart (game strings) */
    void *outfit;
    int give_back;
    char *choice;
    void *choice_driver, *choice_kart;
    int32_t choice_numbers[7];
    int laps_was, mirror_was;
    int32_t look[4];
    /* start sequence (the mod's menu.c) */
    enum { STEP_NONE, STEP_KART, STEP_LOAD } step;
    int wait, frames_waited, cast_in;
    int last_frame;
    char request[64];           /* a file asked for by the menu (any thread), taken by the game's thread */
    pthread_mutex_t lock;
} P = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void *string_pool(void) { return rr_ptr(D_STRING_POOL); }

static float ghost_time(const struct rr_frame *f) { return f->time - P.header.go_time + RACE_START; }
static int ended(void) { return P.time >= ghost_time(&P.frames[P.count - 1]) - RACE_START - 0.01f; }

int runtime_replay_playing(void) { return P.pending != 0; }

/* ---- loading ---- */
#define GAP 6
#define STILL (RR_RATE / 2)
static void clean(int r) {
    for (uint32_t i = 1; i < P.count; i++)
        for (int steering = 0; steering < 2 && P.header.racer[r].kind == RR_REMOTE; steering++) {
            const struct rr_sample *a = &P.frames[i - 1].racer[r];
#define EMPTY(s) (steering ? (s)->steer == -64 : !(s)->buttons)
            if (!EMPTY(&P.frames[i].racer[r]) || EMPTY(a)) continue;
            uint32_t k = i;
            while (k < P.count && k < i + GAP && EMPTY(&P.frames[k].racer[r])) k++;
            if (k == i + GAP || k >= P.count) continue;
            for (uint32_t j = i; j < k; j++) {
                struct rr_sample *s = &P.frames[j].racer[r];
                s->buttons = a->buttons, s->steer = a->steer, s->ghost_bits = a->ghost_bits;
            }
#undef EMPTY
        }
    for (uint32_t i = 0, k; i < P.count; i = k) {
        const struct rr_sample *s = &P.frames[i].racer[r];
        for (k = i + 1; k < P.count; k++) {
            const struct rr_sample *b = &P.frames[k].racer[r];
            int d = 0;
            for (int c = 0; c < 3; c++) d += b->position[c] > s->position[c] ? b->position[c] - s->position[c] : s->position[c] - b->position[c];
            if (d > 2 * (int)RR_POSITION) break;
        }
        for (uint32_t j = i; k - i >= STILL && j < k && P.frames[i].time > P.header.go_time; j++) {
            struct rr_sample *b = &P.frames[j].racer[r];
            b->velocity[0] = b->velocity[1] = b->velocity[2] = 0;
            if (P.header.racer[r].kind != RR_LOCAL) b->buttons = 0, b->steer = 0, b->ghost_bits = 0;
        }
    }
}

static void clean_text(char *t, size_t size) {
    t[size - 1] = 0;
    for (size_t i = 0; t[i]; i++)
        if ((unsigned char)t[i] < 0x20) t[i] = ' ';
}

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fread(&P.header, 1, sizeof P.header, f) == sizeof P.header && !memcmp(P.header.magic, RR_MAGIC, 8) &&
             (P.header.version == RR_VERSION || P.header.version == 4) && P.header.frame_size == sizeof(struct rr_frame) &&
             P.header.frames <= RR_MAX_FRAMES && P.header.packed <= (uint64_t)RR_MAX_FRAMES * sizeof(struct rr_frame);
    uint8_t *packed = ok ? malloc((size_t)P.header.packed + 16) : NULL;
    if (ok) ok = packed && !fseek(f, P.header.header_size, SEEK_SET) && fread(packed, 1, P.header.packed, f) == P.header.packed;
    fclose(f);
    if (ok && !P.frames) P.frames = malloc(sizeof(struct rr_frame) * RR_MAX_FRAMES);
    ok = ok && P.frames;
    P.count = 0;
    static const struct rr_frame ZERO;
    const uint8_t *in = packed, *end = packed + (ok ? P.header.packed : 0);
    for (uint32_t i = 0; ok && i < P.header.frames; i++) {
        ok = rr_unpack_frame(&P.frames[i], i > 0 ? &P.frames[i - 1] : &ZERO, i > 1 ? &P.frames[i - 2] : &ZERO, &in, end,
                             (int)P.header.version);
        if (ok) P.count = i + 1;
    }
    free(packed);
    P.header.level[sizeof P.header.level - 1] = 0;
    clean_text(P.header.player, sizeof P.header.player);
    clean_text(P.header.track, sizeof P.header.track);
    for (int i = 0; i < RR_RACERS; i++) {
        struct rr_racer *r = &P.header.racer[i];
        clean_text(r->name, sizeof r->name);
        clean_text(r->driver, sizeof r->driver);
        clean_text(r->kart, sizeof r->kart);
    }
    void *zones = rr_ptr(RR_D_ZONE_SYSTEM);
    ok = ok && zones && game.zone_by_name(zones, P.header.level);   /* a track this game has */
    for (int r = 0; ok && r < RR_RACERS; r++) clean(r);
    return ok && P.count > 1 && P.header.go_time >= 0.0f;
}

/* ---- the player's character ---- */
static void *local_player(void) {
    char *players = rr_ptr(D_PLAYERS);
    void *list = players ? RR_AT(players, 0x10, void *) : 0;
    for (int i = 0; i < rr_count(list); i++)
        if (rr_item(list, i) && (uint32_t)RR_AT(rr_item(list, i), 0x20, int32_t) <= 7) return rr_item(list, i);
    return 0;
}

static void set_string(void *object, int offset, const char *text) {
    char *old = RR_AT(object, offset, char *);
    RR_AT(object, offset, char *) = game.string(string_pool(), text);
    if (old) game.string_release(string_pool(), old - 0x10);
}

static void keep_choice(int index) {
    char *g = rr_ptr(RR_D_GAME);
    char *save = g ? RR_AT(g, 0xdb0, char *) : 0;
    void *list = save ? RR_AT(save, 0x10, void *) : 0;
    P.choice = index >= 0 && index < rr_count(list) ? rr_item(list, index) : 0;
    if (!P.choice) return;
    P.choice_driver = rr_handle_object(&RR_AT(P.choice, 0x10, void *));
    P.choice_kart = rr_handle_object(&RR_AT(P.choice, 0x20, void *));
    P.choice_numbers[0] = RR_AT(P.choice, 0x18, int32_t);
    for (int k = 1; k < 7; k++) P.choice_numbers[k] = RR_AT(P.choice, 0x24 + 4 * k, int32_t);
}

static void put_choice_back(void) {
    if (!P.choice) return;
    if (rr_handle_object(&RR_AT(P.choice, 0x10, void *)) != P.choice_driver) game.make_handle(&RR_AT(P.choice, 0x10, void *), P.choice_driver);
    if (rr_handle_object(&RR_AT(P.choice, 0x20, void *)) != P.choice_kart) game.make_handle(&RR_AT(P.choice, 0x20, void *), P.choice_kart);
    RR_AT(P.choice, 0x18, int32_t) = P.choice_numbers[0];
    for (int k = 1; k < 7; k++) RR_AT(P.choice, 0x24 + 4 * k, int32_t) = P.choice_numbers[k];
}

static void request_character(void *player, const char *driver, void *outfit, const char *kart) {
    const int index = RR_AT(player, 0xb4, int32_t);
    char *entities = rr_ptr(D_PLAYER_ENTITIES);
    if (entities && (uint32_t)index < 8) {
        char *entry = entities + 0x10 + 0x78 * index;
        set_string(entry, 0x08, driver);
        game.make_handle(entry + 0x10, outfit);
    }
    char *karts = rr_ptr(RR_D_KART_MANAGER);
    void *slot = kart && karts ? RR_AT(karts, 0x10 + 8 * index, void *) : 0;
    if (slot) set_string(slot, 0x30, kart);
}

static void give_back(void) {
    void *player = local_player();
    put_choice_back();
    char *settings = rr_ptr(RR_D_RACE_SETTINGS);
    if (settings && P.mirror_was >= 0) {
        RR_AT(settings, 0x3c, uint8_t) = (uint8_t)P.mirror_was;
        RR_AT(settings, 0xc, int32_t) = P.laps_was;
    }
    P.mirror_was = -1;
    if (!player) return;                                /* no player yet: next frame */
    if (player == P.player) {
        set_string(player, 0x88, P.driver);
        set_string(player, 0x98, P.kart_name);
        game.make_handle(&RR_AT(player, 0x90, void *), P.outfit);
        for (int k = 0; k < 4; k++) RR_AT(player, 0x74 + 4 * k, int32_t) = P.look[k];
    }
    request_character(player, P.driver, P.outfit, 0);
    if (P.driver) game.string_release(string_pool(), P.driver - 0x10);
    if (P.kart_name) game.string_release(string_pool(), P.kart_name - 0x10);
    P.driver = P.kart_name = 0;
    P.player = 0;
    P.give_back = 0;
    P.choice = 0;
    puts("Replay: the player's own character is back");
}

static void play_end(int now) {
    if (!P.player) return;
    put_choice_back();
    if (now) give_back();
    else P.give_back = 1;
}

static void drop_ghosts(void) {
    for (int i = 0; i < RR_RACERS; i++) {
        if (P.ghost[i]) game.release(P.ghost[i]);
        P.ghost[i] = 0;
        P.attached[i] = 0;
    }
}

static void accolades(int hide) {
    char *g = rr_ptr(RR_D_GAME);
    char *data = g ? RR_AT(g, 0xdb0, char *) : 0;
    if (!data) return;
    if (hide && P.accolade_player == -2) P.accolade_player = RR_AT(data, 0x40, int32_t);
    if (!hide && P.accolade_player != -2) RR_AT(data, 0x40, int32_t) = P.accolade_player, P.accolade_player = -2;
}

static void *outfit_of(void *driver, const struct rr_racer *r) {
    void *skins = RR_AT(driver, 0x98, void *);
    const int k = r->look[5] && r->look[5] <= rr_count(skins) ? r->look[5] - 1 : 0;
    return rr_count(skins) && rr_item(skins, k) ? RR_AT(rr_item(skins, k), 0x68, void *) : 0;
}

static int32_t look_value(uint16_t v) { return v == 0xffff ? -1 : v; }
static void *nth(void *list, uint16_t i) { return i < rr_count(list) ? rr_item(list, i) : 0; }

static void set_look(void *preset, const char **kart, const struct rr_racer *r) {
    char *system = rr_ptr(RR_D_SYSTEM_DATA);
    char *global = system ? RR_AT(system, 0x110, char *) : 0;
    if (!global || !*kart) return;
    uint32_t hash = 0x811c9dc5;
    for (const char *c = *kart; *c; c++) hash = (hash ^ (uint8_t)*c) * 0x1000193;
    void **decals = 0;
    if (!(game.decal_find(RR_AT(global, 0x18, void *), kart, hash, &decals) & 1)) decals = 0;
    game.make_handle(&RR_AT(preset, 0x30, void *), nth(RR_AT(global, 0x28, void *), r->look[0]));
    game.make_handle(&RR_AT(preset, 0x18, void *), nth(RR_AT(global, 0x10, void *), r->look[1]));
    game.make_handle(&RR_AT(preset, 0x20, void *), decals ? nth(*decals, r->look[2]) : 0);
    game.make_handle(&RR_AT(preset, 0x28, void *), nth(RR_AT(global, 0x20, void *), r->look[3]));
}

/* The player's kart shows the recording player (its CPlayer gets that racer's driver, outfit, kart, look) */
static void cast_player(int ask) {
    if (P.pending != 1) return;
    void *player = local_player();
    const struct rr_racer *rec = P.player_slot >= 0 ? &P.header.racer[P.player_slot] : 0;
    void *driver = rec && player ? rr_game_data(RR_DRIVERS, rec->driver) : 0, *kart = driver ? rr_game_data(RR_KARTS, rec->kart) : 0;
    if (!kart || (P.player && P.player != player)) return;
    if (!P.player) {
        const int index = RR_AT(player, 0xb4, int32_t);
        char *entities = rr_ptr(D_PLAYER_ENTITIES), *karts = rr_ptr(RR_D_KART_MANAGER);
        char *entry = entities && (uint32_t)index < 8 ? entities + 0x10 + 0x78 * index : 0;
        void *slot = karts && (uint32_t)index < 8 ? RR_AT(karts, 0x10 + 8 * index, void *) : 0;
        const char *own_driver = entry && RR_AT(entry, 0x08, char *) ? RR_AT(entry, 0x08, char *) : RR_AT(player, 0x88, char *);
        const char *own_kart = slot && RR_AT(slot, 0x30, char *) ? RR_AT(slot, 0x30, char *) : RR_AT(player, 0x98, char *);
        if (!own_driver || !own_kart) return;
        keep_choice(index);
        P.player = player;
        P.driver = game.string(string_pool(), own_driver);
        P.kart_name = game.string(string_pool(), own_kart);
        P.outfit = entry && rr_handle_object((void **)(entry + 0x10)) ? rr_handle_object((void **)(entry + 0x10))
                                                                      : rr_handle_object(&RR_AT(player, 0x90, void *));
        for (int k = 0; k < 4; k++) P.look[k] = RR_AT(player, 0x74 + 4 * k, int32_t);
    }
    set_string(player, 0x88, rec->driver);
    set_string(player, 0x98, rec->kart);
    void *outfit = outfit_of(driver, rec);
    if (outfit) game.make_handle(&RR_AT(player, 0x90, void *), outfit);
    for (int k = 0; k < 4 && rec->look_filled; k++) RR_AT(player, 0x74 + 4 * k, int32_t) = look_value(rec->look[k]);
    if (ask) {
        request_character(player, rec->driver, outfit, rec->kart);
        char *settings = rr_ptr(RR_D_RACE_SETTINGS);
        if (settings && P.mirror_was < 0) {
            P.mirror_was = RR_AT(settings, 0x3c, uint8_t);
            P.laps_was = RR_AT(settings, 0xc, int32_t);
            RR_AT(settings, 0x3c, uint8_t) = P.header.mirror;
        }
    }
}

/* Direct start, without the menus, as the race settings and character screens do it */
static void direct_start(void) {
    char *settings = rr_ptr(RR_D_RACE_SETTINGS), *g = rr_ptr(RR_D_GAME);
    if (!settings || !g) return;
    game.set_next_race_mode(g, 1);                      /* Single Race */
    int racers = 0;
    for (int i = 0; i < RR_RACERS; i++) racers += P.kart_slot[i] >= 0;
    RR_AT(settings, 0x10, uint8_t) = racers > 1;        /* _wantAI */
    RR_AT(settings, 0x14, int32_t) = racers - 1;        /* _desiredAICount */
    RR_AT(settings, 0x20, int32_t) = 1;                 /* one local player */
    game.apply_player_count(settings, 1);
    game.set_next_map(P.header.level);
    char *back = game.string(string_pool(), "Octane_MainMenu_RaceSetting");
    game.enter_customization(&back, 1);
    game.string_release(string_pool(), back - 0x10);
}

static int same_text(const char *a, const char *b) { return a && b && !strcmp(a, b); }

static int direct_ready(void) {
    void *player = local_player();
    const int index = player ? RR_AT(player, 0xb4, int32_t) : -1;
    char *entities = rr_ptr(D_PLAYER_ENTITIES);
    char *entry = entities && (uint32_t)index < 8 ? entities + 0x10 + 0x78 * index : 0;
    void *loaded = entry ? RR_AT(entry, 0x38, void *) : 0;
    return loaded && !RR_AT(entry, 4, int32_t) && same_text(RR_AT(entry, 8, const char *), RR_AT(loaded, 0x10, const char *)) &&
           rr_handle_object(&RR_AT(player, 0x10, void *));
}

static void direct_go(void) {
    char *g = rr_ptr(RR_D_GAME), *players = rr_ptr(D_PLAYERS);
    char *save = g ? RR_AT(g, 0xdb0, char *) : 0;
    void *choices = save ? RR_AT(save, 0x10, void *) : 0;
    if (rr_count(choices) && rr_item(choices, 0)) game.apply_choice(rr_item(choices, 0), 0);
    void *player = players ? game.player_at(RR_AT(players, 0x10, void *), 0) : 0;
    if (player) RR_AT(g, 0x4c, int32_t) = RR_AT(player, 0x84, int32_t);
    game.load_next_map();
}

/* ---- the race ---- */
static int pose(int slot, float t, float *position, float *angles) {
    uint32_t lo = 0, hi = P.count - 1;
    if (t <= ghost_time(&P.frames[0])) hi = 0;
    while (hi - lo > 1) {
        const uint32_t mid = (lo + hi) / 2;
        if (ghost_time(&P.frames[mid]) <= t) lo = mid;
        else hi = mid;
    }
    const struct rr_sample *a = &P.frames[lo].racer[slot], *b = &P.frames[hi].racer[slot];
    if (!(a->flags & RS_PRESENT)) return 0;
    if (!(b->flags & RS_PRESENT)) b = a;
    const float ta = ghost_time(&P.frames[lo]), tb = ghost_time(&P.frames[hi]);
    float f = tb > ta ? (t - ta) / (tb - ta) : 0.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    for (int k = 0; k < 3; k++) {
        position[k] = ((float)a->position[k] + ((float)b->position[k] - (float)a->position[k]) * f) / RR_POSITION;
        const int d = (int16_t)(b->angles[k] - a->angles[k]);
        angles[k] = ((float)a->angles[k] + (float)d * f) / ANGLE_UNIT;
    }
    return 1;
}

static void list_append(void *list, void *object) {
    const int n = RR_AT(list, 0xc, int32_t);
    __atomic_fetch_add(&RR_AT(object, 8, int32_t), 1, __ATOMIC_SEQ_CST);
    if (n >= RR_AT(list, 0x10, int32_t)) game.list_set_count(list, n + 1, 8);
    else RR_AT(list, 0xc, int32_t) = n + 1;
    RR_AT(list, 0x20, void **)[n] = object;
}

static void *build_ghost(int slot) {
    struct rr_racer *r = &P.header.racer[slot];
    void *ghost = game.ghost_new(0);
    if (!ghost) return 0;
    RR_AT(ghost, 0xc, int32_t) = 3;
    set_string(ghost, 0x18, r->kart);
    set_string(ghost, 0x20, r->driver);
    float end = ghost_time(&P.frames[P.count - 1]);
    if (end > 650.0f) end = 650.0f;
    RR_AT(ghost, 0x28, float) = end + 60.0f;
    RR_AT(ghost, 0x48, int32_t) = -1;
    void *moves = RR_AT(ghost, 0x30, void *), *inputs = RR_AT(ghost, 0x38, void *);
    game.list_reserve(moves, (int)(end * 100.0f) / KEY_STEP + 2, 8);
    for (int cs = 0; cs <= (int)(end * 100.0f); cs += KEY_STEP) {
        float position[3], angles[3];
        if (!pose(slot, cs / 100.0f, position, angles)) continue;
        void *key = game.move_key_new(0);
        if (!key) break;
        for (int k = 0; k < 3; k++) {
            RR_AT(key, 0xc + 4 * k, float) = position[k];
            RR_AT(key, 0x1a + 2 * k, int16_t) = (int16_t)(angles[k] * ANGLE_UNIT);
        }
        RR_AT(key, 0x18, uint16_t) = (uint16_t)cs;
        list_append(moves, key);
        game.release(key);
    }
    int last = -1;
    for (uint32_t i = 0; i < P.count; i++) {
        const float t = ghost_time(&P.frames[i]);
        if (t < 0.0f || t > end) continue;
        const int bits = P.frames[i].racer[slot].ghost_bits;
        if (bits == last) continue;
        void *key = game.input_key_new(0);
        if (!key) break;
        RR_AT(key, 0xc, uint16_t) = (uint16_t)bits;
        RR_AT(key, 0xe, uint16_t) = (uint16_t)(t * 100.0f + 0.5f);
        list_append(inputs, key);
        game.release(key);
        last = bits;
    }
    if (rr_count(moves) < 2 || !rr_count(inputs)) {
        game.release(ghost);
        return 0;
    }
    return ghost;
}

static void *kart_of_racer(int slot) {
    return slot >= 0 && slot < RR_RACERS && P.kart_slot[slot] >= 0 ? rr_kart_of(P.kart_slot[slot], 0) : 0;
}

static void *with_ghosts(int on) {
    char *manager = rr_ptr(RR_D_GHOST_MANAGER);
    void *list = manager ? RR_AT(manager, 0x40, void *) : 0;
    if (!list) return 0;
    int n = 0;
    for (int i = 0; i < RR_RACERS && on; i++)
        if (P.ghost[i]) n++;
    if (n > RR_AT(list, 0x10, int32_t)) game.list_set_count(list, n, 8);
    RR_AT(list, 0xc, int32_t) = n;
    for (int i = 0, k = 0; i < RR_RACERS && on; i++)
        if (P.ghost[i]) RR_AT(list, 0x20, void **)[k++] = P.ghost[i];
    return list;
}

static const struct rr_frame *frame_now(void) {
    const float t = P.time + RACE_START;
    uint32_t lo = 0, hi = P.count - 1;
    while (hi - lo > 1) {
        const uint32_t mid = (lo + hi) / 2;
        if (ghost_time(&P.frames[mid]) <= t) lo = mid;
        else hi = mid;
    }
    return &P.frames[ghost_time(&P.frames[hi]) <= t ? hi : lo];
}

static void drive(float time) {
    char *manager = rr_ptr(RR_D_GHOST_MANAGER), *frame = rr_ptr(RR_D_FRAME), *racers = rr_ptr(RR_D_RACER_MANAGER);
    accolades(1);
    if (!manager || !with_ghosts(1)) return;
    for (int i = 0; i < RR_RACERS; i++) {
        void *kart = P.ghost[i] && !P.attached[i] ? kart_of_racer(i) : 0;
        void *racer = kart && racers ? game.racer_of(racers, kart) : 0;
        if (!racer) continue;
        const int id = RR_AT(racer, 0x164, int32_t);
        RR_AT(P.ghost[i], 0x48, int32_t) = id;
        game.ghost_attach(manager, kart, id);
        P.attached[i] = 1;
    }
    if (P.rewound) game.despawn_items();
    for (int i = 0; i < RR_RACERS && P.rewound; i++) {
        if (!P.ghost[i]) continue;
        RR_AT(P.ghost[i], 0x40, int64_t) = 0;
        game.ghost_reset(manager, P.ghost[i]);
    }
    P.rewound = 0;
    for (int i = 0; i < RR_RACERS; i++) {
        void *kart = P.attached[i] ? kart_of_racer(i) : 0;
        void *move = kart ? rr_component(kart, RR_D_KART_CONTROLLER_META) : 0;
        if (move) RR_AT(move, 0x260, uint8_t) = 0;      /* not a ghost: items aim at it and hit it */
    }
    RR_AT(manager, 0x14, uint8_t) = 1;                  /* _isPlaying */
    RR_AT(manager, 0x0c, uint8_t) = 0;                  /* _isRecording */
    RR_AT(manager, 0x18, float) = time - (frame ? RR_AT(frame, 0x24, float) : 0.0f);
    game.ghost_update(manager);
    with_ghosts(0);
    const struct rr_frame *now = frame_now();
    const float scale = P.paused || P.scrubbing || ended() ? 0.0f : SPEEDS[P.speed];
    for (int i = 0; i < RR_RACERS; i++) {
        void *kart = P.attached[i] ? kart_of_racer(i) : 0;
        void *move = kart ? rr_component(kart, RR_D_KART_CONTROLLER_META) : 0;
        if (!move) continue;
        for (int k = 0; k < 3; k++)
            RR_AT(kart, 0x60 + 4 * k, float) = RR_AT(move, 0xcc + 4 * k, float) = now->racer[i].velocity[k] / RR_VELOCITY * scale;
        void *boosts = rr_component(kart, RR_D_BOOST_META);
        void *boost = boosts ? rr_running_boost(boosts) : 0;
        const float elapsed = boost ? rr_boost_length(boost) - now->racer[i].reserves / 100.0f : 0.0f;
        if (boost) RR_AT(boost, 0x18, float) = elapsed > 0.0f ? elapsed : 0.0f;
    }
}

static void *local_controller(int *id_out) {
    char *players = rr_ptr(D_PLAYERS), *input = rr_ptr(D_INPUT_MANAGER);
    void *list = players ? RR_AT(players, 0x10, void *) : 0;
    for (int i = 0; i < rr_count(list); i++) {
        void *player = rr_item(list, i);
        if (!player || (uint32_t)RR_AT(player, 0x20, int32_t) > 7) continue;
        const int id = RR_AT(player, 0xb0, int32_t);
        if (id_out) *id_out = id;
        if (id < 0 || id > 7 || !input) return 0;
        const int index = RR_AT(input, 0x38 + 4 * id, int32_t);
        void *controllers = RR_AT(input, 0x28, void *);
        return index >= 0 && index < rr_count(controllers) ? rr_item(controllers, index) : 0;
    }
    return 0;
}

static int bit_of(void *ctrl, int signal) {
    for (int b = 0; b < 51; b++)
        if (RR_AT(ctrl, 0x18 + 4 * b, int32_t) == signal) return b;
    return -1;
}

static void set_up(void) {
    int id = -1;
    void *ctrl = local_controller(&id);
    char *karts = rr_ptr(RR_D_KART_MANAGER);
    P.kart = id >= 0 && karts ? game.player_kart(karts, id, 1) : 0;
    if (!P.kart) return;
    P.set_up = 1;
    if (ctrl) P.buttons = RR_AT(ctrl, 0xe8, uint64_t);
}

static void follow(void) {
    void *target = kart_of_racer(P.watch);
    void *instance = target ? game.camera_instance() : 0;
    void *boxes = instance ? RR_AT(instance, 0x58, void *) : 0;
    void *camera = boxes ? game.active_camera(boxes) : 0;
    void *meta = rr_ptr(D_KART_CAMERA_META);
    if (!camera || !meta) return;
    if ((*(meta_of_fn ***)camera)[0](camera) != meta) return;  /* intro and countdown cameras */
    if (camera == P.camera && target == P.target) return;
    game.make_handle((char *)camera + 0x278, target);
    game.camera_refresh(camera);
    RR_AT(camera, 0x11d, uint8_t) = 1;
    P.camera = camera;
    P.target = target;
}

static void unghost(void) {
    if (++P.unghost_frames < 60) return;
    P.unghost_frames = 0;
    for (int i = 0; i < RR_RACERS; i++) {
        void *kart = P.attached[i] ? kart_of_racer(i) : 0;
        void *message = kart ? game.ghost_look_end_new(0) : 0;
        if (!message) continue;
        game.send_message(message, kart, kart);
        game.release(message);
    }
}

enum { S_UP = 0, S_DOWN = 1, S_LEFT = 2, S_RIGHT = 3, S_SQUARE = 5, S_TRIANGLE = 6, S_L1 = 8, S_L2 = 9, S_R1 = 10, S_R2 = 11 };

static void lap_skip(int step) {
    const float now = P.time + 0.05f;
    float current = 0.0f, target = step < 0 ? 0.0f : -1.0f;
    for (uint32_t i = 1; i < P.count; i++) {
        if (P.frames[i].racer[P.watch].lap <= P.frames[i - 1].racer[P.watch].lap || P.frames[i - 1].racer[P.watch].lap < 1) continue;
        const float t = ghost_time(&P.frames[i]) - RACE_START;
        if (step < 0 && t <= now) target = current, current = t;
        if (step > 0 && t > now && target < 0.0f) target = t;
    }
    if (target < 0.0f) return;
    P.rewound = 1;
    P.time = target;
}

static void read_buttons(float dt) {
    void *ctrl = local_controller(NULL);
    char *frame = rr_ptr(RR_D_FRAME);
    if (!ctrl) return;
    const uint64_t now = RR_AT(ctrl, 0xe8, uint64_t), pressed = now & ~P.buttons;
    P.buttons = now;
#define PRESSED(s) (bit_of(ctrl, s) >= 0 && (pressed >> bit_of(ctrl, s) & 1))
#define HELD(s) (bit_of(ctrl, s) >= 0 && (now >> bit_of(ctrl, s) & 1))
    if (PRESSED(S_L1)) lap_skip(-1);
    if (PRESSED(S_R1)) lap_skip(1);
    if (PRESSED(S_L2) || PRESSED(S_R2)) {
        const int step = PRESSED(S_R2) ? 1 : -1;
        for (int n = 1; n <= RR_RACERS; n++) {
            const int slot = ((P.watch + step * n) % RR_RACERS + RR_RACERS) % RR_RACERS;
            if (kart_of_racer(slot)) {
                P.watch = slot;
                break;
            }
        }
    }
    if (PRESSED(S_SQUARE)) P.paused = !P.paused;
    if (PRESSED(S_UP) && P.speed < (int)(sizeof SPEEDS / sizeof SPEEDS[0]) - 1) P.speed++;
    if (PRESSED(S_DOWN) && P.speed > 0) P.speed--;
    const float end = ghost_time(&P.frames[P.count - 1]) - RACE_START;
    P.scrubbing = HELD(S_LEFT) || HELD(S_RIGHT);
    if (HELD(S_LEFT)) {
        P.time -= dt * SCRUB;
        if (P.time < 0.0f) P.time = 0.0f;
        P.rewound = 1;
    } else if (HELD(S_RIGHT)) {
        P.time += dt * SCRUB;
        P.rewound = 1;
    } else if (!P.paused) {
        P.time += dt * SPEEDS[P.speed];
    }
    if (P.time > end) P.time = end;
    if (frame) RR_AT(frame, 0x2c, float) = HELD(S_LEFT) || ended() ? 1.0f : HELD(S_RIGHT) ? SCRUB : P.paused ? 0.000001f : SPEEDS[P.speed];
#undef PRESSED
#undef HELD
}

static void *race_timer(void) {
    void *w = rr_world(), *meta = rr_ptr(RR_D_RACE_CLOCK_META);
    void *clock = w && meta ? rr_find_component(w, meta) : 0;
    void *timer = clock ? RR_AT(clock, 0x28, void *) : 0;
    return timer && RR_AT(timer, 0, void *) ? timer : 0;
}

static void set_race_clock(float seconds) {
    void *timer = race_timer();
    if (!timer || !RR_AT(timer, 0x14, uint8_t)) return;
    const int32_t now = !RR_AT(timer, 0x15, uint8_t) ? RR_AT(*(void **)timer, 0xb0, rr_now_fn *)(timer) : RR_AT(timer, 0x10, int32_t);
    RR_AT(timer, 0xc, int32_t) = now - (int32_t)(seconds * 8192.0f);
}

/* ---- the recorded inputs ---- */
static void feed(void *kart) {
    void *input = kart ? RR_AT(kart, 0x1a0, void *) : 0;
    char *command = input ? RR_AT(input, 0x10, char *) : 0;
    if (!command) return;
    for (int i = 0; i < RR_RACERS; i++) {
        if (kart_of_racer(i) != kart) continue;
        const struct rr_frame *f = frame_now();
        if (f + 1 < P.frames + P.count) f++;
        const struct rr_sample *s = &f->racer[i];
        const int still = P.paused || P.scrubbing || ended();
        uint32_t now = still ? 0 : s->buttons;
        if ((now & 0x2000) && s->drift_state) now |= 0x08000000;
        RR_AT(command, 0x10, uint64_t) = now;
        RR_AT(command, 0x18, uint64_t) = now & ~P.fed[i];
        RR_AT(command, 0x20, uint64_t) = P.fed[i] & ~now;
        RR_AT(command, 0x68, float) = still ? 0.0f : s->steer / RR_STEER;
        P.fed[i] = now;
        return;
    }
    RR_AT(command, 0x10, uint64_t) = RR_AT(command, 0x18, uint64_t) = RR_AT(command, 0x20, uint64_t) = 0;
    RR_AT(command, 0x68, float) = 0.0f;
}

static int running(void) { return P.pending == 2 && P.started; }

/* ---- hooks (the game's threads) ---- */
static player_input_fn *native_player_input;
static void ABI player_input(void *actor, void *command) {
    native_player_input(actor, command);
    if (running() && actor == P.kart) feed(actor);
}

/* An AI kart's input: in a replay the AI does not drive, its kart gets the recorded inputs (0: skip the AI) */
int ABI runtime_replay_ai_input(void *ai) {
    if (!running()) return 1;
    feed(RR_AT(ai, 0x18, void *));
    return 0;
}

static ai_count_fn *native_ai_count;
static int ABI ai_count(void **out) {
    const int n = native_ai_count(out);
    int racers = 0;
    for (int i = 0; P.pending == 1 && i < RR_RACERS; i++) racers += P.kart_slot[i] > 0;
    return P.pending == 1 && racers < n ? racers : n;
}

static void ABI ai_load(void *manager) {
    if (P.pending == 1 && manager) {
        void *infos = RR_AT(manager, 0x30, void *);
        int r = 0;
        for (int host = 1; host < RR_RACERS; host++) {
            int found = 0;
            for (int i = 0; i < rr_count(infos); i++) found |= rr_item(infos, i) && RR_AT(rr_item(infos, i), 0x58, int32_t) == host;
            while (found && r < RR_RACERS && (r == P.player_slot || P.kart_slot[r] < 0)) r++;
            if (found && r < RR_RACERS) P.kart_slot[r++] = host;
        }
        for (int i = 0; i < rr_count(infos); i++) {
            void *p = rr_item(infos, i);
            int k = 0;
            while (k < RR_RACERS && (!p || P.kart_slot[k] != RR_AT(p, 0x58, int32_t))) k++;
            const struct rr_racer *rec = &P.header.racer[k < RR_RACERS ? k : 0];
            void *driver = k < RR_RACERS ? rr_game_data(RR_DRIVERS, rec->driver) : 0, *kart = driver ? rr_game_data(RR_KARTS, rec->kart) : 0;
            if (!kart) continue;
            set_string(p, 0x10, rec->driver);
            game.make_handle(&RR_AT(p, 0x18, void *), driver);
            game.make_handle(&RR_AT(p, 0x20, void *), outfit_of(driver, rec));
            set_string(p, 0x28, rec->kart);
            game.make_handle(&RR_AT(p, 0x30, void *), kart);
            void *preset = RR_AT(p, 0x38, void *);
            if (preset) game.make_handle(&RR_AT(preset, 0x10, void *), kart);
            if (preset && rec->look_filled) set_look(preset, &RR_AT(p, 0x28, const char *), rec);
            if (rec->look_filled) {
                RR_AT(p, 0x40, int32_t) = look_value(rec->look[0]);
                for (int n = 0; n < 3; n++) RR_AT(p, 0x68 + 4 * n, int32_t) = look_value(rec->look[1 + n]);
            }
        }
        cast_player(0);
    }
    game.ai_load(manager);
}

/* The ghost manager's level start: in a replay, a ghost is made for every recorded racer */
void ABI runtime_replay_level_start(void *manager) {
    if (!P.pending) return;
    char *g = rr_ptr(RR_D_GAME), *settings = rr_ptr(RR_D_RACE_SETTINGS);
    if (P.pending != 1 || !manager || !g || RR_AT(g, 0xd4c, int32_t) != 1) {
        P.pending = 0;                                  /* another level: the replay is over */
        drop_ghosts();
        play_end(1);
        return;
    }
    cast_player(0);
    P.pending = 2;
    P.level_time = 0.0f;
    if (settings) RR_AT(settings, 0xc, int32_t) = 99;   /* laps: the player's kart never finishes */
    drop_ghosts();
    int ghosts = 0;
    for (int i = 0; i < RR_RACERS; i++)
        if (P.kart_slot[i] >= 0) ghosts += !!(P.ghost[i] = build_ghost(i));
    P.watch = P.player_slot >= 0 ? P.player_slot : 0;
    P.speed = NORMAL_SPEED;
    printf("Replay: playing %s (%d racers)\n", P.header.track, ghosts);
    fflush(stdout);
}

void runtime_replay_play_level_end(void) {
    char *frame = rr_ptr(RR_D_FRAME);
    if (P.pending && frame) RR_AT(frame, 0x2c, float) = 1.0f;
    accolades(0);
    if (P.pending != 2) return;
    P.pending = 0;
    drop_ghosts();
    play_end(0);
}

void runtime_replay_play_frame(void) {
    char *frame = rr_ptr(RR_D_FRAME);
    if (P.pending != 2) return;
    if (frame && !RR_AT(frame, 0x28, uint8_t)) P.level_time += RR_AT(frame, 0x24, float);
    put_choice_back();
    if (!P.set_up) set_up();
    const float go = P.started ? 0.0f : rr_race_clock();
    if (go > 0.0f && P.kart) {
        P.started = 1;
        P.time = go;
    }
    const float dt = frame ? RR_AT(frame, 0x18, float) : 0.0f;
    if (P.started && !(frame && RR_AT(frame, 0x28, uint8_t))) read_buttons(dt);
    drive(P.started ? P.time + RACE_START : RACE_START - 0.2f);
    unghost();
    follow();
    const float finish = P.header.racer[P.watch].finish_time;
    set_race_clock((frame_now()->racer[P.watch].flags & RS_FINISHED) && finish > 0.0f && finish < P.time ? finish : P.time);
}

/* Every frame (the pad read, runtime_replay.c): the menu's request, the start sequence, the character given back */
void ABI runtime_replay_tick(void) {
    char *frame = rr_ptr(RR_D_FRAME);
    const int number = frame ? RR_AT(frame, 0x20, int32_t) : 0;
    if (number == P.last_frame) return;
    P.last_frame = number;
    char path[sizeof P.request];
    pthread_mutex_lock(&P.lock);
    snprintf(path, sizeof path, "%s", P.request);
    P.request[0] = 0;
    pthread_mutex_unlock(&P.lock);
    if (path[0] && !P.pending && P.step == STEP_NONE) {
        char full[800];
        rr_race_dir(full, sizeof full - 80);
        strncat(full, "/", sizeof full - strlen(full) - 1);
        strncat(full, path, sizeof full - strlen(full) - 1);
        accolades(0);
        memset(P.ghost, 0, sizeof P.ghost);
        P.accolade_player = -2;
        P.mirror_was = -1;
        P.set_up = P.started = P.paused = P.scrubbing = P.rewound = 0;
        P.camera = P.target = P.kart = 0;
        P.time = 0.0f;
        memset(P.attached, 0, sizeof P.attached);
        memset(P.fed, 0, sizeof P.fed);
        if (!load(full)) {
            printf("Replay: %s cannot be played\n", path);
        } else {
            P.player_slot = -1;
            for (int i = 0; i < RR_RACERS && P.player_slot < 0; i++)
                if (P.header.racer[i].kind == RR_LOCAL && P.header.racer[i].driver[0] && same_text(P.header.racer[i].name, P.header.player)) P.player_slot = i;
            for (int i = 0; i < RR_RACERS && P.player_slot < 0; i++)
                if (P.header.racer[i].kind == RR_LOCAL && P.header.racer[i].driver[0]) P.player_slot = i;
            for (int i = 0; i < RR_RACERS && P.player_slot < 0; i++)
                if (P.header.racer[i].kind && P.header.racer[i].driver[0]) P.player_slot = i;
            int next = 1;
            for (int i = 0; i < RR_RACERS; i++)
                P.kart_slot[i] = i == P.player_slot ? 0 : P.header.racer[i].kind && P.header.racer[i].driver[0] && next < RR_RACERS ? next++ : -1;
            P.pending = 1;
            printf("Replay: starting %s (%s, %u frames)\n", path, P.header.track, P.count);
            direct_start();
            P.step = STEP_KART;
            P.wait = 3;
            P.frames_waited = 0;
        }
        fflush(stdout);
    }
    int ai = 0;
    for (int i = 1; i < RR_RACERS; i++) ai |= rr_kart_of(i, 0) != 0;
    if (P.give_back && !P.pending && !ai) give_back();
    if (P.step != STEP_NONE) {
        /* the map loads 1.5 s after the player's kart is made (at most 10 s) */
        if (P.wait) P.wait--;
        else if (P.step == STEP_LOAD) direct_go(), P.step = STEP_NONE, P.cast_in = 5;
        else if (direct_ready() || ++P.frames_waited > 600) P.step = STEP_LOAD, P.wait = 90;
        return;
    }
    if (P.cast_in && !--P.cast_in) cast_player(1);
}

/* The menu (any thread): play a file of RaceReplay. 0 when a replay is already starting or playing. */
int runtime_replay_play_file(const char *name) {
    if (!name || !*name || strlen(name) >= sizeof P.request || strchr(name, '/') || strchr(name, '\\')) return 0;
    pthread_mutex_lock(&P.lock);
    const int ok = !P.pending && P.step == STEP_NONE;
    if (ok) snprintf(P.request, sizeof P.request, "%s", name);
    pthread_mutex_unlock(&P.lock);
    return ok;
}

/* ---- install ---- */
static void *fn(size_t usa, int eu_moved, int eu) { return rr_game(usa - (eu && eu_moved ? 0x10 : 0)); }

int runtime_replay_play_install(int eu) {
    game.ghost_new = fn(0x10e1050, 1, eu);
    game.move_key_new = fn(0x10e0cb0, 1, eu);
    game.input_key_new = fn(0x10e0d80, 1, eu);
    game.list_reserve = fn(0x14a4ed0, 1, eu);
    game.list_set_count = fn(0x14a6f20, 1, eu);
    game.ghost_reset = fn(0xfce410, 1, eu);
    game.ghost_attach = fn(0xfce080, 1, eu);
    game.ghost_update = fn(0xfce570, 1, eu);
    game.ai_load = fn(0xffd780, 1, eu);
    game.ai_count = fn(0xff9580, 1, eu);
    game.racer_of = fn(0xfd4230, 1, eu);
    game.camera_instance = fn(0xc4b110, 0, eu);
    game.active_camera = fn(0xc4b230, 0, eu);
    game.camera_refresh = fn(0xc8ade0, 0, eu);
    game.make_handle = fn(0x14bea20, 1, eu);
    game.ghost_look_end_new = fn(0x112da10, 1, eu);
    game.send_message = fn(0xd32160, 0, eu);
    game.player_kart = fn(0xe50f70, 0, eu);
    game.despawn_items = fn(0x1056bb0, 1, eu);
    game.string = fn(0x1574080, 1, eu);
    game.string_release = fn(0x1518e30, 1, eu);
    game.release = fn(0x149d260, 1, eu);
    game.set_next_race_mode = fn(0xfdeb70, 1, eu);
    game.set_next_map = fn(0x1045e90, 1, eu);
    game.load_next_map = fn(0x1046580, 1, eu);
    game.apply_choice = fn(0xc1d8e0, 0, eu);
    game.apply_player_count = fn(0xc2e400, 0, eu);
    game.player_at = fn(0xc02600, 0, eu);
    game.enter_customization = fn(0xe1e7a0, 0, eu);
    game.zone_by_name = fn(0x102e320, 1, eu);
    game.decal_find = fn(0xa0b5a0, 0, eu);
    /* every function but player_at starts "push rbp; mov rbp, rsp" */
    void *const check[] = {game.ghost_new, game.move_key_new, game.input_key_new, game.list_reserve, game.list_set_count,
                           game.ghost_reset, game.ghost_attach, game.ghost_update, game.ai_load, game.ai_count, game.racer_of,
                           game.camera_instance, game.active_camera, game.camera_refresh, game.make_handle,
                           game.ghost_look_end_new, game.send_message, game.player_kart, game.despawn_items, game.string,
                           game.string_release, game.release, game.set_next_race_mode, game.set_next_map, game.load_next_map,
                           game.apply_choice, game.apply_player_count, game.enter_customization, game.zone_by_name,
                           game.decal_find};
    for (size_t i = 0; i < sizeof check / sizeof *check; i++)
        if (memcmp(check[i], "\x55\x48\x89\xe5", 4)) {
            printf("Replay: player function %zu is not the expected code: no replays\n", i);
            return 0;
        }
    unsigned char *input_fn_at = fn(0xbc3830, 0, eu);
    native_player_input = (player_input_fn *)input_fn_at;
    native_ai_count = game.ai_count;
    /* call sites: the player's input, the AI count, both AI loads (the pad read: runtime_replay.c) */
    int ok = rr_redirect_call(fn(0xbc30aa, 0, eu), input_fn_at, (void (*)(void))player_input) &&
             rr_redirect_call(fn(0xffb24e, 1, eu), (unsigned char *)game.ai_count, (void (*)(void))ai_count) &&
             rr_redirect_call(fn(0xffc451, 1, eu), (unsigned char *)game.ai_load, (void (*)(void))ai_load) &&
             rr_redirect_call(fn(0xe9b7ec, 1, eu), (unsigned char *)game.ai_load, (void (*)(void))ai_load);
    if (!ok) {
        puts("Replay: the player's call sites are not the expected code: no replays");
        return 0;
    }
    return 1;
}

/* The menu may start a replay: none is starting or playing, and no race is on (no player kart). */
int runtime_replay_can_start(void) {
    return !P.pending && P.step == STEP_NONE && rr_game(0) && !rr_kart_of(0, 0);
}
