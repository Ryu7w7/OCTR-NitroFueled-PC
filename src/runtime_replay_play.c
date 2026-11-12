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
#include "runtime_replay_list.h"
#include "runtime_net_ping.h"
#include "runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
typedef void ABI set_text_fn(void *placeable, const char *text);
typedef void ABI set_hidden_fn(void *placeable, int hidden);
typedef void ABI hud_set_lap_fn(void *hud, int lap);
typedef void *ABI find_driver_fn(void *drivers, const char *name);
typedef void *ABI spawn_fn(void *project, void *source, void *parent);
typedef void ABI vector_fn(void *placeable, const float *v);
typedef void ABI opacity_fn(void *placeable, float opacity);
typedef void ABI color_fn(void *placeable, const uint8_t *rgba);
typedef void ABI hud_style_fn(void *hud, int player);

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
    pool_new_fn *ghost_look_end_new, *ghost_look_start_new;
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
    set_text_fn *set_text;
    set_hidden_fn *set_hidden;
    hud_set_lap_fn *hud_set_lap;
    find_driver_fn *find_driver;
    spawn_fn *spawn_placeable;
    vector_fn *set_position, *set_pivot, *set_scale, *set_rotation;
    opacity_fn *set_opacity;
    color_fn *set_color;
    hud_style_fn *hud_style_next;
} game;

#define D_HUD_META 0x2ad4940
#define COMPOSITION_VTABLE 0x2431b18

enum {
    D_PLAYERS = 0x2ad1160, D_INPUT_MANAGER = 0x2ac2e90, D_KART_CAMERA_META = 0x2ac42e8, D_PLAYER_ENTITIES = 0x2ad0730,
    D_STRING_POOL = 0x2af2600,
};

#define HELP_LINES 6
#define LINES 3

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
    void *ghost_look[RR_RACERS];  /* the kart that got the ghost look (Time Trial ghosts), 0 none */
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
    /* the HUD and the panel */
    char names[RR_RACERS][48];
    void *hud_root, *total_lap, *place, *place_suffix, *wumpa;
    int shown_lap, setting_lap, lap_refresh;
    float speed_clock;
    int shown_speed, speed_watch, scrub_dir;
    RrView view;                /* the panel's copy, under lock */
    char drivers[RR_RACERS][48];   /* each racer's driver as the status line shows it */
    void *line[LINES], *help[HELP_LINES], *help_arrow[4], *lap_line[7], *line_copy[7];
    float lap_start[RR_RACERS][MAX_LAPS];
    char shown_text[200];
    void *pad_piece[12], *pad_seen, *pad_root;
    int pad_frames;
    uint32_t pad_shown;
    /* live: a friend's race while it is sent (runtime_spectate.c appends its frames on its own thread) */
    int live, live_ended, live_request, buffering, live_header_new;
    struct rr_header live_header;
    int key_cs[RR_RACERS];          /* the next move key's time (1/100 s) of each ghost */
    uint32_t input_frame[RR_RACERS];    /* the next frame to make input keys from */
    int input_last[RR_RACERS];
    uint32_t laps_scanned;
    int hero_told;                      /* runtime_replay_hero() logged for this replay */
    int skip_set, flyby_seen;          /* skip_intro(): the cutscene manager's flag is ours; the fly-over came anyway */
    uint8_t skip_was;
    uint32_t world_id;                  /* the replay's level's world (give_back waits for another) */
    int back_frames;
    int ghosts_later;                   /* live, loaded before the friend's GO: the ghosts are made at GO */
    int option_set;                     /* skip_intro(): the game's auto-skip fly-by option is ours */
    uint8_t option_was;
} P = {.lock = PTHREAD_MUTEX_INITIALIZER};
static int drop_ghosts_at_end;          /* a chained live race: the level that ends still has the last race's ghosts */
/* The friend's next live race while the last one is still on screen (ended): kept for the game's thread,
 * which swaps the races (P.lock) */
static struct {
    struct rr_header header;
    uint8_t *packed;
    size_t size;
    uint32_t frames;
} NEXT;
static int take_live(const struct rr_header *header, const uint8_t *packed, size_t size, uint32_t frames);
static void items_reset(void);
/* live: the friend's GO time from the newest header the stream brought, once it is there */
static void live_go(void);
static void left_level(void);
static int items_restore;            /* drive cleared the track: the recent items come back (track_items) */

static void *string_pool(void) { return rr_ptr(D_STRING_POOL); }

static float ghost_time(const struct rr_frame *f) { return f->time - P.header.go_time + RACE_START; }
/* live: the play time stays this far behind the newest frame. It grows by 1 s (up to 6 s) each time the stream
 * ran dry (a sender with a slow or uneven connection), and comes back down by 0.5 s after 90 s without that. */
#define LIVE_DELAY 2.0f
#define LIVE_DELAY_MAX 6.0f
static float live_delay = LIVE_DELAY, live_smooth;
static uint32_t frames_now(void) { return __atomic_load_n(&P.count, __ATOMIC_ACQUIRE); }
/* the last play time there is: live, a little behind the newest frame until the race is over */
static float last_time(void) {
    const float end = ghost_time(&P.frames[frames_now() - 1]) - RACE_START;
    return P.live && !__atomic_load_n(&P.live_ended, __ATOMIC_ACQUIRE) ? end - live_delay : end;
}
static int ended(void) {
    if (P.live && !__atomic_load_n(&P.live_ended, __ATOMIC_ACQUIRE)) return 0;
    return P.time >= ghost_time(&P.frames[frames_now() - 1]) - RACE_START - 0.01f;
}

int runtime_replay_playing(void) { return P.pending != 0; }
/* read by the file thread too (runtime_file.c): the track's archive without crates */
int runtime_replay_no_crates(void) {
    return __atomic_load_n(&P.pending, __ATOMIC_ACQUIRE) && (P.header.no_crates || P.header.mode == 2);
}
/* live: a friend's race still going on (the play time cannot go past what has come) */
static int live_open(void) { return P.live && !__atomic_load_n(&P.live_ended, __ATOMIC_ACQUIRE); }

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
    P.live = 0;
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

/* The kart and its look on the player's kart. The map start (guest 0xe73820) gives each vehicle (kart manager
 * +0x10 + 8 * slot) the kart named at +0x38 (+0x30 is the current one, +0x40 its wheels) and the look comes
 * from the customization table (+0x10 + 0x88 * slot: wheels, paint, decal, sticker, ... every 0x20, as the
 * recorder reads them). Both get the recorded racer's while a replay loads or plays, and the viewer's own
 * back afterwards (give_back). */
static int32_t look_value(uint16_t v);
static struct {
    int saved;
    int64_t look[5];
    char *start_kart;
    int32_t wheels;
    int told;
} OWN;

static char *vehicle_of(int index) {
    char *karts = rr_ptr(RR_D_KART_MANAGER);
    return karts && (uint32_t)index < 8 ? RR_AT(karts, 0x10 + 8 * index, char *) : 0;
}

static void apply_look(void) {
    void *player = P.pending && P.player_slot >= 0 ? local_player() : 0;
    const struct rr_racer *rec = player ? &P.header.racer[P.player_slot] : 0;
    if (!rec || !rec->kart[0] || !rr_game_data(RR_KARTS, rec->kart)) return;
    const int index = RR_AT(player, 0xb4, int32_t);
    char *custom = rr_ptr(RR_D_CUSTOMIZATION), *vehicle = vehicle_of(index);
    int64_t *look = custom && (uint32_t)index < 8 ? (int64_t *)(custom + 0x10 + 0x88 * index) : 0;
    if (!OWN.saved && look && vehicle) {
        OWN.saved = 1;
        for (int k = 0; k < 5; k++) OWN.look[k] = look[k * 4];
        const char *own = RR_AT(vehicle, 0x38, const char *);
        OWN.start_kart = own ? game.string(string_pool(), own) : 0;
        OWN.wheels = RR_AT(vehicle, 0x40, int32_t);
    }
    if (!OWN.saved) return;
    const char *start = vehicle ? RR_AT(vehicle, 0x38, const char *) : 0;
    if (vehicle && (!start || strcmp(start, rec->kart))) set_string(vehicle, 0x38, rec->kart);
    if (vehicle && rec->look_filled) RR_AT(vehicle, 0x40, int32_t) = look_value(rec->look[0]);
    for (int k = 0; look && rec->look_filled && k < 5; k++) look[k * 4] = look_value(rec->look[k]);
    if (!OWN.told) {
        OWN.told = 1;
        printf("Replay: the player's kart is %s, look %d %d %d %d %d (own: %s)\n", rec->kart, look_value(rec->look[0]),
               look_value(rec->look[1]), look_value(rec->look[2]), look_value(rec->look[3]), look_value(rec->look[4]),
               OWN.start_kart ? OWN.start_kart : "-");
        fflush(stdout);
    }
}

static void restore_look(void *player) {
    if (!OWN.saved || !player) return;
    const int index = RR_AT(player, 0xb4, int32_t);
    char *custom = rr_ptr(RR_D_CUSTOMIZATION), *vehicle = vehicle_of(index);
    if (custom && (uint32_t)index < 8)
        for (int k = 0; k < 5; k++) ((int64_t *)(custom + 0x10 + 0x88 * index))[k * 4] = OWN.look[k];
    if (vehicle && OWN.start_kart) set_string(vehicle, 0x38, OWN.start_kart);
    if (vehicle && P.kart_name) set_string(vehicle, 0x30, P.kart_name);
    if (vehicle) RR_AT(vehicle, 0x40, int32_t) = OWN.wheels;
    if (OWN.start_kart) game.string_release(string_pool(), OWN.start_kart - 0x10);
    OWN.start_kart = 0;
    OWN.saved = OWN.told = 0;
}

static void give_back(void) {
    void *player = local_player();
    put_choice_back();
    restore_look(player);
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
    P.back_frames = 0;
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
        P.ghost_look[i] = 0;
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

/* changeHeroAndOutfit (runtime_replay.c's stub, guest 0xbd5b90): whatever its caller read (the save's choice, the
 * profile), during a replay the player's kart gets the recorded racer's driver and outfit. 1: the stub hands the
 * game these two instead of its own. */
char *runtime_replay_hero_name;
void *runtime_replay_hero_outfit;
int ABI runtime_replay_hero(void *entities, int index, char **hero, void **outfit) {
    (void)outfit;
    /* an empty driver is the game clearing the slot before a map load: kept, or the next driver looks loaded
     * already and the map's own hero load never comes (a kart without its driver, the race never starting) */
    if (!hero || !*hero || !**hero) return 0;
    if (!P.pending || P.player_slot < 0 || !entities || entities != rr_ptr(D_PLAYER_ENTITIES)) return 0;
    void *player = local_player();
    if (!player || RR_AT(player, 0xb4, int32_t) != index) return 0;
    const struct rr_racer *rec = &P.header.racer[P.player_slot];
    void *driver = rec->driver[0] ? rr_game_data(RR_DRIVERS, rec->driver) : 0;
    if (!driver) return 0;
    if (!runtime_replay_hero_name || strcmp(runtime_replay_hero_name, rec->driver)) {
        char *old = runtime_replay_hero_name;
        runtime_replay_hero_name = game.string(string_pool(), rec->driver);
        if (old) game.string_release(string_pool(), old - 0x10);
    }
    game.make_handle(&runtime_replay_hero_outfit, outfit_of(driver, rec));
    if (!P.hero_told) {
        P.hero_told = 1;
        printf("Replay: changeHeroAndOutfit asked for %s on the player's kart: %s instead\n", hero && *hero ? *hero : "-",
               rec->driver);
        fflush(stdout);
    }
    return runtime_replay_hero_name != 0;
}

/* The player's kart shows the recording player (its CPlayer gets that racer's driver, outfit, kart, look) */
static void cast_player(int ask) {
    if (P.pending != 1) return;
    void *player = local_player();
    const struct rr_racer *rec = P.player_slot >= 0 ? &P.header.racer[P.player_slot] : 0;
    void *driver = rec && player ? rr_game_data(RR_DRIVERS, rec->driver) : 0, *kart = driver ? rr_game_data(RR_KARTS, rec->kart) : 0;
    if (!kart || (P.player && P.player != player)) {
        if (ask) printf("Replay: the player's kart cannot show %s / %s (player %p, driver %p, kart %p, own %p)\n",
                        rec ? rec->driver : "-", rec ? rec->kart : "-", player, driver, kart, P.player);
        return;
    }
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
        printf("Replay: the player's kart shows %s / %s (outfit %p)\n", rec->driver, rec->kart, outfit);
        request_character(player, rec->driver, outfit, rec->kart);
        char *settings = rr_ptr(RR_D_RACE_SETTINGS);
        if (settings && P.mirror_was < 0) {             /* the player's own settings (a chained race keeps them) */
            P.mirror_was = RR_AT(settings, 0x3c, uint8_t);
            P.laps_was = RR_AT(settings, 0xc, int32_t);
        }
        if (settings) RR_AT(settings, 0x3c, uint8_t) = P.header.mirror;
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
    P.key_cs[slot] = ((int)(end * 100.0f) / KEY_STEP + 1) * KEY_STEP;
    P.input_frame[slot] = P.count;
    P.input_last[slot] = last;
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
    if (P.rewound) game.despawn_items(), items_restore = 1;
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
    const float scale = P.paused || P.scrubbing || P.buffering || ended() ? 0.0f : SPEEDS[P.speed];
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
        /* A replay plays as a Single Race, where karts have no ghost look. In a Time Trial the others were
         * ghosts: they get the look once (a second Start would add its effects twice). */
        const int ghost = P.header.mode == 2 && i != P.player_slot;
        if (ghost && P.ghost_look[i] == kart) continue;
        void *message = kart ? (ghost ? game.ghost_look_start_new : game.ghost_look_end_new)(0) : 0;
        if (ghost) P.ghost_look[i] = kart;
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
    if (target < 0.0f || (step > 0 && target > last_time())) return;   /* live: a lap not here yet */
    P.rewound = 1;
    P.time = target;
}

static void read_buttons(float dt) {
    void *ctrl = local_controller(NULL);
    char *frame = rr_ptr(RR_D_FRAME);
    if (!ctrl) return;
    const uint64_t now = RR_AT(ctrl, 0xe8, uint64_t), pressed = now & ~P.buttons;
    P.buttons = now;
    const float before = P.time;
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
    {
        int id = 0;
        local_controller(&id);
        void *meta = rr_ptr(D_HUD_META);
        void *hud_component = PRESSED(S_TRIANGLE) && P.kart && meta ? rr_find_component(P.kart, meta) : 0;
        if (hud_component && RR_AT(hud_component, 0x80, uint8_t)) game.hud_style_next(hud_component, id);
    }
    /* live: no faster than the race itself, and forward only back up to the live point (after a rewind) */
    const int open = live_open();
    const int fastest = open ? NORMAL_SPEED : (int)(sizeof SPEEDS / sizeof SPEEDS[0]) - 1;
    if (PRESSED(S_UP) && P.speed < fastest) P.speed++;
    if (PRESSED(S_DOWN) && P.speed > 0) P.speed--;
    if (P.speed > fastest) P.speed = fastest;
    const float end = last_time();
    const int forward = HELD(S_RIGHT) && !HELD(S_LEFT) && (!open || P.time < end - 0.05f);
    P.scrubbing = HELD(S_LEFT) || forward;
    P.scrub_dir = HELD(S_LEFT) ? -1 : forward ? 1 : 0;
    if (HELD(S_LEFT)) {
        P.time -= dt * SCRUB;
        if (P.time < 0.0f) P.time = 0.0f;
        P.rewound = 1;
    } else if (forward) {
        P.time += dt * SCRUB;
        P.rewound = 1;
    } else if (!P.paused) {
        P.time += dt * SPEEDS[P.speed];
    }
    const int was_buffering = P.buffering;
    P.buffering = P.live && !P.paused && !P.scrubbing && P.time >= end;
    if (P.buffering && !was_buffering && P.started && live_delay < LIVE_DELAY_MAX) {
        live_delay += 1.0f;
        printf("Replay: the live stream ran dry: now %.0f s behind\n", live_delay);
        fflush(stdout);
    }
    live_smooth = P.buffering ? 0.0f : live_smooth + dt;
    if (live_smooth > 90.0f && live_delay > LIVE_DELAY) live_delay -= 0.5f, live_smooth = 0.0f;
    /* live, past the point (the delay just grew): it waits there, it does not jump back */
    if (P.time > end) P.time = live_open() && before > end && !P.scrubbing ? before : end;
    if (frame) RR_AT(frame, 0x2c, float) = HELD(S_LEFT) || ended() ? 1.0f : forward ? SCRUB : P.paused ? 0.000001f : SPEEDS[P.speed];
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

/* Parts of a track are shown or hidden by occlusion triggers where the player's kart is; the camera follows
 * another kart in a replay, so they use the camera's place instead (COcclusionTrigger +0x5a
 * _useCameraPosition, a level setting the game already has). COcclusionTriggerManager: +0x10 triggers. */
#define D_OCCLUSION_TRIGGERS 0x2acdb48
static void camera_triggers(void) {
    char *manager = rr_ptr(D_OCCLUSION_TRIGGERS);
    void *list = manager ? RR_AT(manager, 0x10, void *) : 0;
    for (int i = 0; i < rr_count(list); i++)
        if (rr_item(list, i)) RR_AT(rr_item(list, i), 0x5a, uint8_t) = 1;
}

/* In a replay the items come from the recording: what old items do after a rewind cleared them would run on
 * freed memory (runtime_replay.c's item stubs), crates give nothing, the HUD's lap is ours. */
int ABI runtime_replay_on(void) { return P.pending == 2; }
int ABI runtime_replay_crate(void) { return P.pending != 2; }
int ABI runtime_replay_hud_lap(void) { return P.pending != 2 || !P.started || P.setting_lap; }

/* ---- the race HUD shows the watched racer ---- */
static void *asset_of(void *placeable) { return rr_handle_object(&RR_AT(placeable, 0x38, void *)); }

static void *content(void *placeable) {
    void *a = asset_of(placeable);
    return a && RR_AT(a, 0, const void *) == (const void *)rr_game(COMPOSITION_VTABLE) ? RR_AT(a, 0x20, void *) : 0;
}

static void *find(void *placeable, const char *name, int depth) {
    if (!placeable || depth > 24) return 0;
    if (same_text(RR_AT(placeable, 0x28, const char *), name)) return placeable;
    void *children = RR_AT(placeable, 0x48, void *);
    for (int i = 0; i < rr_count(children); i++) {
        void *found = find(rr_item(children, i), name, depth + 1);
        if (found) return found;
    }
    return find(content(placeable), name, depth + 1);
}

static void show_text(void *piece, const char *text) {
    if (!piece) return;
    game.set_hidden(RR_AT(piece, 0x40, void *), 0);
    if (!same_text(RR_AT(piece, 0x30, const char *), text)) game.set_text(piece, text);
}

/* ---- The replay's HUD, as the Replay Mode draws it (play.c hud(), pad.c): three status lines at the bottom
 * centre (the watched racer, its speed, the replay's state), the button legend under the lap display, the
 * timer's lap lines with the watched racer's laps, and its controller. All are copies of the race HUD's own
 * pieces (the lap counter's text, the boost arrow), so they have its font and look. ---- */
static void *find_asset(void *placeable, const char *name, int depth) {
    if (!placeable || depth > 24) return 0;
    void *a = asset_of(placeable);
    if (a && same_text(RR_AT(a, 0x10, const char *), name)) return placeable;
    void *children = RR_AT(placeable, 0x48, void *);
    for (int i = 0; i < rr_count(children); i++) {
        void *found = find_asset(rr_item(children, i), name, depth + 1);
        if (found) return found;
    }
    return find_asset(content(placeable), name, depth + 1);
}

static void v3(float *v, float a, float b, float c) { v[0] = a, v[1] = b, v[2] = c; }

/* The legend; the font has the button symbols but not the D-pad: arrows drawn over the spaces of lines 1, 2 */
static const char HELP[HELP_LINES][48] = {"\xee\x80\x83 : PAUSE", "     /     : REWIND / FORWARD", "     /     : SPEED",
    "\xee\x80\x84 / \xee\x80\x86 : PREVIOUS / NEXT LAP", "\xee\x80\x87 / \xee\x80\x88 : OTHER RACER", "\xee\x80\x82 : HUD STYLE"};
static const float HELP_ARROW[4][4] = {{1, 17, -1, 90}, {1, 65, -1, 270}, {2, 16, -2, 180}, {2, 65, 0, 0}};
static const float HELP_BOX_FIX[HELP_LINES] = {0, 0, -37, 0, 0, 0};
#define HELP_X 1555.0f
#define HELP_Y 170.0f
#define HELP_STEP 32.0f
#define HELP_SCALE 0.62f
#define HELP_BOX 0.11f

static void make_legend(void *project, void *root, void *template) {
    void *source = template ? RR_AT(template, 0x18, void *) : 0;
    const float extent = template ? RR_AT(template, 0x90, float) : 0.0f;
    float scale[3], pivot[3], at[3];
    v3(scale, HELP_SCALE, HELP_SCALE, 1.0f);
    for (int i = 0; template && i < HELP_LINES; i++) {
        void *piece = P.help[i] = game.spawn_placeable(project, source ? source : template, root);
        if (!piece) continue;
        RR_AT(piece, 0xa0, int32_t) = RR_AT(piece, 0xa4, int32_t) = 0;
        v3(pivot, 0.0f, 0.5f, 0.5f);
        v3(at, HELP_X, HELP_Y + HELP_STEP * i, 0.0f);
        game.set_pivot(piece, pivot);
        game.set_position(piece, at);
        game.set_scale(piece, scale);
        RR_AT(piece, 0x90, float) = extent * HELP_BOX * (float)strlen(HELP[i]) + HELP_BOX_FIX[i];
        game.set_opacity(piece, 1.0f);
        game.set_hidden(piece, 0);
        game.set_text(piece, HELP[i]);
    }
    void *arrow = find_asset(root, "[octane_ui_hud_boost_nf_arrow.tga]", 0);
    void *from = arrow ? RR_AT(arrow, 0x18, void *) : 0;
    float small[3], turn[3];
    v3(small, 0.8f, 0.8f, 1.0f);
    for (int i = 0; arrow && i < 4; i++) {
        void *piece = P.help_arrow[i] = game.spawn_placeable(project, from ? from : arrow, root);
        if (!piece) continue;
        RR_AT(piece, 0xa0, int32_t) = RR_AT(piece, 0xa4, int32_t) = 0;
        v3(pivot, 0.5f, 0.5f, 0.5f);
        v3(turn, 0.0f, 0.0f, HELP_ARROW[i][3]);
        v3(at, HELP_X + HELP_ARROW[i][1], HELP_Y + HELP_STEP * HELP_ARROW[i][0] + HELP_ARROW[i][2], 0.0f);
        game.set_pivot(piece, pivot);
        game.set_rotation(piece, turn);
        game.set_scale(piece, small);
        game.set_position(piece, at);
        game.set_opacity(piece, 1.0f);
        game.set_hidden(piece, 0);
    }
}

static const char SPEED_TEXT[6][6] = {"x0.1", "x0.5", "", "x2", "x3", "x4"};

static void status_text(char *out, size_t size) {
    const struct rr_racer *r = &P.header.racer[P.watch];
    const struct rr_sample *s = &frame_now()->racer[P.watch];
    char first[96];
    snprintf(first, sizeof first, "%s", P.drivers[P.watch]);
    if (r->name[0]) snprintf(first + strlen(first), sizeof first - strlen(first), " (%s)", r->name);
    if ((s->flags & RS_FINISHED) && r->finished)
        snprintf(first + strlen(first), sizeof first - strlen(first), "   FINISHED %d%s", r->finished,
                 r->finished == 1 ? "ST" : r->finished == 2 ? "ND" : r->finished == 3 ? "RD" : "TH");
    const char *state = ended() ? "REPLAY ENDED" : P.paused ? "REPLAY PAUSED" : P.live && P.buffering ? "LIVE (WAITING)"
                      : P.live && !P.live_ended ? "LIVE" : "";
    char speed[16] = "";
    if (!state[0] && SPEED_TEXT[P.speed][0]) snprintf(speed, sizeof speed, "REPLAY %s", SPEED_TEXT[P.speed]);
    snprintf(out, size, "%s\n%d KM/H\n%s", first, P.shown_speed, state[0] ? state : speed);
}

/* The watched racer's controller (pad.c): bit per piece; button symbols and a D-pad of four arrows */
enum { PAD_SYMBOLS = 8, PAD_COUNT = 12 };
static const char PAD_TEXT[PAD_SYMBOLS][4] = {"\xee\x80\x87", "\xee\x80\x88", "\xee\x80\x84", "\xee\x80\x86",
    "\xee\x80\x82", "\xee\x80\x83", "\xee\x80\x81", "\xee\x80\x80"};
static const float PAD_AT[PAD_COUNT][3] = {
    {60, 30, 0.195f}, {220, 30, 0.195f}, {60, 88, 0.222f}, {222, 88, 0.222f},
    {221.5f, 130, 0.18f}, {175, 175, 0.18f}, {265, 175, 0.18f},
    {219.5f, 220, 0.164f},
    {66, 129, 180}, {63.5f, 211, 0}, {24, 170, 90}, {106, 170, 270}};
#define PAD_X 345.0f
#define PAD_Y 815.0f
#define PAD_SCALE 2.0f
#define PAD_WIDTH 270.0f
#define PRESSED_SCALE 1.2f
#define ARROW_SCALE 2.0f
#define DARK_OPACITY 0.45f

static uint32_t pad_bits(void) {
    const struct rr_frame *f = frame_now();
    const struct rr_sample *s = &f->racer[P.watch];
    uint32_t pad = 0;
    if (s->ghost_bits & 0x100) pad |= 1u << 7;      /* accelerate: Cross */
    if (s->ghost_bits & 0x200) pad |= 1u << 9;      /* reverse: down */
    if (s->ghost_bits & 0x400) pad |= 1u << 5;      /* brake: Square */
    if (s->ghost_bits & 0x800) pad |= 1u << 3;      /* first jump: R1 */
    if (s->ghost_bits & 0x1000) pad |= 1u << 2;     /* second jump: L1 */
    if (s->steer < -RR_STEER / 10) pad |= 1u << 10;
    if (s->steer > RR_STEER / 10) pad |= 1u << 11;
    const struct rr_frame *before = f - (f - P.frames > RR_RATE / 4 ? RR_RATE / 4 : f - P.frames);
    if (s->item_uses != before->racer[P.watch].item_uses) pad |= 1u << 6;   /* an item used: Circle */
    return pad;
}

static void *copy_piece(void *project, void *template, void *root) {
    if (!template) return 0;
    void *source = RR_AT(template, 0x18, void *);
    void *piece = game.spawn_placeable(project, source ? source : template, root);
    if (!piece) return 0;
    RR_AT(piece, 0xa0, int32_t) = RR_AT(piece, 0xa4, int32_t) = 0;
    game.set_opacity(piece, 1.0f);
    game.set_hidden(piece, 0);
    return piece;
}

static void show_button(int i, int pressed) {
    void *piece = P.pad_piece[i];
    if (!piece) return;
    game.set_opacity(piece, pressed ? 1.0f : DARK_OPACITY);
    const uint8_t light = pressed ? 255 : 150;
    const uint8_t rgba[4] = {light, light, light, 255};
    game.set_color(piece, rgba);
    const float size = i < PAD_SYMBOLS && pressed ? PRESSED_SCALE : 1.0f;
    float x = PAD_X + PAD_AT[i][0], v[3];
    if (i < PAD_SYMBOLS) {
        x -= PAD_AT[i][2] * PAD_WIDTH * size / 2;
        v3(v, PAD_SCALE * size, PAD_SCALE * size, 1.0f);
        game.set_scale(piece, v);
    }
    v3(v, x, PAD_Y + PAD_AT[i][1], 0.0f);
    game.set_position(piece, v);
}

static void show_pad(void *project, void *root) {
    if (root != P.pad_seen) P.pad_seen = root, P.pad_frames = 0;
    if (P.pad_frames < 3) {
        P.pad_frames++;
        return;
    }
    int there = 0;
    void *children = RR_AT(root, 0x48, void *);
    for (int i = 0; i < rr_count(children); i++)
        if (rr_item(children, i) == P.pad_piece[PAD_SYMBOLS]) there = 1;
    if (root != P.pad_root || !there) {
        P.pad_root = root;
        P.pad_shown = (uint32_t)-1;
        void *label = find(root, "Laps Label 1", 0), *arrow = find_asset(root, "[octane_ui_hud_boost_nf_arrow.tga]", 0);
        if (!label) label = find(root, "[LapCounterText]", 0);
        float v[3];
        for (int i = 0; i < PAD_COUNT; i++) {
            void *piece = P.pad_piece[i] = copy_piece(project, i < PAD_SYMBOLS ? label : arrow, root);
            if (!piece) continue;
            if (i < PAD_SYMBOLS) {
                v3(v, 0.0f, 0.5f, 0.5f);
                game.set_pivot(piece, v);
                game.set_text(piece, PAD_TEXT[i]);
                continue;
            }
            v3(v, 0.5f, 0.5f, 0.5f);
            game.set_pivot(piece, v);
            v3(v, 0.0f, 0.0f, PAD_AT[i][2]);
            game.set_rotation(piece, v);
            v3(v, ARROW_SCALE, ARROW_SCALE, 1.0f);
            game.set_scale(piece, v);
        }
    }
    const uint32_t pieces = pad_bits() & ((1u << PAD_COUNT) - 1);
    if (pieces == P.pad_shown) return;
    for (int i = 0; i < PAD_COUNT; i++)
        if (((pieces ^ P.pad_shown) >> i & 1) || P.pad_shown == (uint32_t)-1) show_button(i, pieces >> i & 1);
    P.pad_shown = pieces;
}

static void hud(void) {
    void *meta = rr_ptr(D_HUD_META);
    void *component = P.kart && meta ? rr_find_component(P.kart, meta) : 0;
    if (!component || !RR_AT(component, 0x80, uint8_t)) return;        /* _isInitialized */
    void *project = rr_handle_object(&RR_AT(component, 0x38, void *));
    void *root = project && RR_AT(project, 0x10, void *) ? content(RR_AT(project, 0x10, void *)) : 0;
    if (!root) return;
    char *racers = rr_ptr(RR_D_RACER_MANAGER);
    void *watched = kart_of_racer(P.watch);
    void *own = watched && racers ? game.racer_of(racers, watched) : 0;
    if (own && rr_handle_object(&RR_AT(component, 0x28, void *)) != own) game.make_handle(&RR_AT(component, 0x28, void *), own);
    const int laps = P.header.laps > 0 ? P.header.laps : 3;
    RR_AT(component, 0x84, int32_t) = laps;             /* _totalLap: the race has 99 */
    if (root != P.hud_root) {
        P.hud_root = root;
        P.total_lap = find(root, "[TotalLap]", 0);
        P.place = find(root, "[PositionNumber]", 0);
        P.place_suffix = find(root, "[PositionText]", 0);
        P.wumpa = find(root, "[WumpaFruitText]", 0);
        char name[] = "Lap Timer 1";
        float at[3];
        for (int i = 0; i < 7; i++, name[sizeof name - 2]++) {
            void *line = P.lap_line[i] = find(root, name, 0);
            void *parent = line ? RR_AT(line, 0x40, void *) : 0;
            void *piece = parent ? game.spawn_placeable(project, RR_AT(line, 0x18, void *) ? RR_AT(line, 0x18, void *) : line, parent) : 0;
            P.line_copy[i] = piece;
            if (!piece) continue;
            set_string(piece, 0x28, "[ReplayLapTime]");     /* the timer's script finds its lines by name */
            memcpy(at, &RR_AT(line, 0x60, float), sizeof at);
            game.set_position(piece, at);
            game.set_opacity(piece, 1.0f);
        }
        P.shown_text[0] = 0;
        void *template = find(root, "[LapCounterText]", 0);
        void *source = template ? RR_AT(template, 0x18, void *) : 0;
        for (int line = 0; line < LINES; line++) {
            P.line[line] = 0;
            if (!template) continue;
            void *piece = game.spawn_placeable(project, source ? source : template, root);
            if (!piece) break;
            RR_AT(piece, 0xa0, int32_t) = RR_AT(piece, 0xa4, int32_t) = 0;
            float pivot[3], position[3], scale[3];
            v3(pivot, 0.5f, 0.5f, 0.5f);
            v3(position, 960.0f, 952.0f + 38.0f * line, 0.0f);
            v3(scale, 0.6f, 0.6f, 1.0f);
            game.set_pivot(piece, pivot);
            game.set_position(piece, position);
            game.set_scale(piece, scale);
            RR_AT(piece, 0x90, float) *= 6.0f;               /* a wide box: one long line */
            game.set_opacity(piece, 1.0f);
            game.set_hidden(piece, 0);
            P.line[line] = piece;
        }
        make_legend(project, root, template);
        P.shown_lap = 0;
    }
    char text[200];
    status_text(text, sizeof text);
    if (!same_text(P.shown_text, text)) {
        snprintf(P.shown_text, sizeof P.shown_text, "%s", text);
        char *at = text;
        for (int line = 0; line < LINES; line++) {
            char *start = at;
            while (*at && *at != '\n') at++;
            if (*at) *at++ = 0;
            if (P.line[line]) game.set_text(P.line[line], start);
        }
    }
    const struct rr_sample *s = &frame_now()->racer[P.watch];
    char number[16];
    snprintf(number, sizeof number, "%d", s->rank);
    show_text(P.place, s->rank ? number : "");
    show_text(P.place_suffix, s->rank == 1 ? "ST" : s->rank == 2 ? "ND" : s->rank == 3 ? "RD" : s->rank ? "TH" : "");
    snprintf(number, sizeof number, "%d", s->wumpa);
    show_text(P.wumpa, number);
    snprintf(number, sizeof number, "%d", laps);
    if (P.total_lap && !same_text(RR_AT(P.total_lap, 0x30, const char *), number)) game.set_text(P.total_lap, number);
    const int lap = s->lap > laps ? laps : s->lap;
    if (lap && lap != P.shown_lap) {
        P.shown_lap = lap;
        P.setting_lap = 1;
        game.hud_set_lap(component, lap);
        P.setting_lap = 0;
    }
    /* the timer's lap lines: the player's kart's own are hidden (every frame: the game shows them again); the copies
     * show the watched racer's, each finished lap, then the one it is on */
    float lines[7];
    int shown = 0;
    const float *starts = P.lap_start[P.watch];
    float now = P.time > 0.0f ? P.time : 0.0f;
    const float finish = P.header.racer[P.watch].finish_time;
    if ((s->flags & RS_FINISHED) && finish > 0.0f && finish < now) now = finish;
    for (int k = 0; k < MAX_LAPS && shown < 7 && starts[k] >= 0.0f && starts[k] <= now; k++) {
        const float end = k + 1 < MAX_LAPS && starts[k + 1] >= 0.0f && starts[k + 1] <= now ? starts[k + 1] : now;
        if (P.header.laps > 0 && k >= P.header.laps) break;
        lines[shown++] = end - starts[k];
    }
    for (int i = 0; i < 7; i++) {
        if (P.lap_line[i]) game.set_hidden(P.lap_line[i], 1);
        if (!P.line_copy[i]) continue;
        game.set_hidden(P.line_copy[i], i >= shown || P.time <= 0.0f);
        if (i >= shown) continue;
        const int cs = (int)(lines[i] * 100.0f + 0.5f);
        snprintf(text, sizeof text, "%d:%02d:%02d", cs / 6000, cs / 100 % 60, cs % 100);
        game.set_text(P.line_copy[i], text);
    }
    show_pad(project, root);
}

/* The display name of each racer: the online name, else the driver's shown name */
static void racer_names(void) {
    char *racers = rr_ptr(RR_D_RACER_MANAGER);
    for (int i = 0; i < RR_RACERS; i++) {
        const struct rr_racer *r = &P.header.racer[i];
        P.names[i][0] = P.drivers[i][0] = 0;
        if (!r->kind) continue;
        {   /* the driver, upper case (the HUD font) */
            void *d = racers && r->driver[0] ? game.find_driver(RR_AT(racers, 0x18, void *), r->driver) : 0;
            const char *shown = d ? RR_AT(d, 0x60, const char *) : 0;
            const char *name = shown && *shown ? shown : r->driver;
            if (!strncmp(name, "Driver", 6)) name += 6;
            size_t o = 0;
            for (size_t k = 0; name[k] && o + 2 < sizeof P.drivers[i]; k++) {
                if (!shown && k && name[k] >= 'A' && name[k] <= 'Z' && name[k - 1] >= 'a' && name[k - 1] <= 'z') P.drivers[i][o++] = ' ';
                P.drivers[i][o++] = name[k] >= 'a' && name[k] <= 'z' ? (char)(name[k] - 32) : name[k];
            }
            P.drivers[i][o] = 0;
        }
        if (r->name[0] && strcmp(r->name, "GHOST")) {
            snprintf(P.names[i], sizeof P.names[i], "%s", r->name);
            continue;
        }
        void *data = racers && r->driver[0] ? game.find_driver(RR_AT(racers, 0x18, void *), r->driver) : 0;
        const char *shown = data ? RR_AT(data, 0x60, const char *) : 0;
        const char *name = shown && *shown ? shown : r->driver;
        if (!strncmp(name, "Driver", 6)) name += 6;
        size_t o = 0;
        for (size_t k = 0; name[k] && o + 2 < sizeof P.names[i]; k++) {
            if (!shown && k && name[k] >= 'A' && name[k] <= 'Z' && name[k - 1] >= 'a' && name[k - 1] <= 'z') P.names[i][o++] = ' ';
            P.names[i][o++] = name[k];
        }
        P.names[i][o] = 0;
    }
}

/* Speed over the ground in km/h (world units are centimetres): the average of the last half second, worked
 * out 3 times a second, changed only by 2 km/h or more */
static void speed_now(float dt) {
    P.speed_clock += dt;
    if (P.speed_clock < 0.33f && P.speed_watch == P.watch) return;
    const struct rr_frame *f = frame_now();
    int n = 0;
    float sum = 0.0f;
    for (const struct rr_frame *g = f; g >= P.frames && n < RR_RATE / 2; g--, n++) {
        const float x = g->racer[P.watch].velocity[0], y = g->racer[P.watch].velocity[1];
        sum += sqrtf(x * x + y * y);
    }
    const int kmh = n && !ended() && !P.paused ? (int)(sum / n / RR_VELOCITY * 0.036f + 0.5f) : 0;
    if (kmh == 0 || abs(kmh - P.shown_speed) >= 2 || P.speed_watch != P.watch) P.shown_speed = kmh;
    P.speed_clock = 0.0f;
    P.speed_watch = P.watch;
}

static void publish_view(void) {
    RrView v;
    memset(&v, 0, sizeof v);
    v.active = 1;
    snprintf(v.track, sizeof v.track, "%s", P.header.track);
    v.laps = P.header.laps;
    v.time = P.time;
    v.end = ghost_time(&P.frames[P.count - 1]) - RACE_START;
    v.speed = SPEEDS[P.speed];
    v.paused = P.paused;
    v.ended = ended();
    v.rewinding = P.scrubbing && P.scrub_dir < 0;
    v.forwarding = P.scrubbing && P.scrub_dir > 0;
    v.watch = P.watch;
    v.kmh = P.shown_speed;
    const struct rr_frame *f = frame_now();
    for (int i = 0; i < RR_RACERS; i++) {
        snprintf(v.racer[i].name, sizeof v.racer[i].name, "%s", P.names[i]);
        v.racer[i].kind = P.header.racer[i].kind;
        v.racer[i].present = kart_of_racer(i) != 0 && (f->racer[i].flags & RS_PRESENT);
        v.racer[i].rank = f->racer[i].rank;
        v.racer[i].lap = f->racer[i].lap;
        v.racer[i].finished = (f->racer[i].flags & RS_FINISHED) ? P.header.racer[i].finished : 0;
        v.racer[i].time = v.racer[i].finished ? P.header.racer[i].finish_time : 0.0f;
    }
    /* as in a race: the results come when the recording player finishes, and fill in as the others do */
    v.results = P.started && ((P.player_slot >= 0 && (f->racer[P.player_slot].flags & RS_FINISHED)) || v.ended);
    pthread_mutex_lock(&P.lock);
    P.view = v;
    pthread_mutex_unlock(&P.lock);
}

int runtime_replay_view(RrView *out) {
    pthread_mutex_lock(&P.lock);
    const int active = P.pending == 2 && P.view.active;
    if (active && out) *out = P.view;
    pthread_mutex_unlock(&P.lock);
    return active;
}

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
    const int replay = P.pending == 1 || P.pending == 3;     /* 3: a restart reloads them */
    for (int i = 0; replay && i < RR_RACERS; i++) racers += P.kart_slot[i] > 0;
    return replay && racers < n ? racers : n;
}

static void ABI ai_load(void *manager) {
    if ((P.pending == 1 || P.pending == 3) && manager) {
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
    if (!P.pending) {
        if (P.give_back) give_back();           /* not given back in the menus (no world there): now */
        return;
    }
    if (P.pending == 3) {                       /* the pause menu's Restart: the replay starts again */
        const char *level = rr_current_level();
        if (level && !strcasecmp(level, P.header.level)) {
            P.pending = 1;
            P.set_up = P.started = P.paused = P.scrubbing = P.rewound = 0;
            P.camera = P.target = P.kart = 0;
            P.time = 0.0f;
            memset(P.attached, 0, sizeof P.attached);
            memset(P.ghost_look, 0, sizeof P.ghost_look);
            memset(P.fed, 0, sizeof P.fed);
            puts("Replay: the race restarted: the replay starts again");
        } else {
            left_level();
            return;
        }
    }
    char *g = rr_ptr(RR_D_GAME), *settings = rr_ptr(RR_D_RACE_SETTINGS);
    if (P.pending != 1 || !manager || !g || RR_AT(g, 0xd4c, int32_t) != 1) {
        P.pending = 0;                                  /* another level: the replay is over */
        P.live = 0;
        drop_ghosts();
        play_end(1);
        return;
    }
    cast_player(0);
    P.pending = 2;
    drop_ghosts_at_end = 0;
    items_reset();
    P.level_time = 0.0f;
    if (settings) RR_AT(settings, 0xc, int32_t) = 99;   /* laps: the player's kart never finishes */
    drop_ghosts();
    live_go();
    int ghosts = 0;
    P.ghosts_later = P.header.go_time < 0.0f;          /* live, before the friend's GO: made when it comes */
    for (int i = 0; i < RR_RACERS && !P.ghosts_later; i++)
        if (P.kart_slot[i] >= 0) ghosts += !!(P.ghost[i] = build_ghost(i));
    if (P.ghosts_later) puts("Replay: the friend's race has not reached GO yet: its racers come at GO");
    P.watch = P.player_slot >= 0 ? P.player_slot : 0;
    P.speed = NORMAL_SPEED;
    P.hud_root = 0;
    P.pad_root = P.pad_seen = 0;
    P.shown_speed = 0;
    racer_names();
    for (int r = 0; r < RR_RACERS; r++) {      /* each racer's lap starts (race time when its lap goes up) */
        for (int k = 0; k < MAX_LAPS; k++) P.lap_start[r][k] = -1.0f;
        P.lap_start[r][0] = 0.0f;
        for (uint32_t i = 1; i < P.count; i++) {
            const int was = P.frames[i - 1].racer[r].lap, now = P.frames[i].racer[r].lap;
            if (now > was && was >= 1 && now - 1 < MAX_LAPS) P.lap_start[r][now - 1] = P.frames[i].race_time;
        }
    }
    printf("Replay: playing %s (%d racers)\n", P.header.track, ghosts);
    fflush(stdout);
}

/* A replay's level went (pending 3): the next one decides. The same level (the pause menu's Restart) starts the replay
 * again (runtime_replay_level_start); another one (Quit: the menus) ends it (left_level, from the tick). */
static void left_level(void) {
    if (P.pending != 3) return;
    const char *level = rr_current_level();
    if (!level || !strcasecmp(level, P.header.level)) return;     /* still the race's (or nothing yet): wait */
    P.pending = 0;
    P.live = 0;
    play_end(0);
    printf("Replay: left for %s: the replay is over\n", level);
    fflush(stdout);
}

void runtime_replay_play_level_end(void) {
    char *frame = rr_ptr(RR_D_FRAME);
    if (P.pending && frame) RR_AT(frame, 0x2c, float) = 1.0f;
    accolades(0);
    if (drop_ghosts_at_end) {
        drop_ghosts_at_end = 0;
        drop_ghosts();
        return;
    }
    if (P.pending != 2) return;
    P.pending = 3;                                      /* left_level or a restart decides */
    drop_ghosts();
}

static void live_go(void) {
    if (!P.live || P.header.go_time >= 0.0f) return;
    pthread_mutex_lock(&P.lock);
    const float go = P.live_header_new || P.live_header.go_time >= 0.0f ? P.live_header.go_time : -1.0f;
    pthread_mutex_unlock(&P.lock);
    if (go >= 0.0f) P.header.go_time = go;
}

/* Live: the frames that came since the last game frame become ghost keys, lap starts and names (the game's
 * thread: the ghost lists are the game's) */
static void live_extend(void) {
    if (!P.live || P.pending != 2 || P.ghosts_later) return;
    const uint32_t n = frames_now();
    struct rr_header header;
    int new_header = 0;
    pthread_mutex_lock(&P.lock);
    if (P.live_header_new) header = P.live_header, P.live_header_new = 0, new_header = 1;
    pthread_mutex_unlock(&P.lock);
    int named = 0;
    for (int i = 0; new_header && i < RR_RACERS; i++) {
        struct rr_racer *r = &P.header.racer[i];
        r->finished = header.racer[i].finished;
        r->finish_time = header.racer[i].finish_time;
        if (!r->name[0] && header.racer[i].name[0]) {
            memcpy(r->name, header.racer[i].name, sizeof r->name);
            named = 1;
        }
        r->name[sizeof r->name - 1] = 0;
    }
    if (named) racer_names();                   /* a nick that came late: the tags and the results take it */
    if (new_header) P.header.duration = header.duration;
    float end = ghost_time(&P.frames[n - 1]);
    if (end > 650.0f) end = 650.0f;
    for (int slot = 0; slot < RR_RACERS; slot++) {
        void *ghost = P.ghost[slot];
        if (!ghost) continue;
        void *moves = RR_AT(ghost, 0x30, void *), *inputs = RR_AT(ghost, 0x38, void *);
        for (; P.key_cs[slot] <= (int)(end * 100.0f); P.key_cs[slot] += KEY_STEP) {
            float position[3], angles[3];
            if (!pose(slot, P.key_cs[slot] / 100.0f, position, angles)) continue;
            void *key = game.move_key_new(0);
            if (!key) break;
            for (int k = 0; k < 3; k++) {
                RR_AT(key, 0xc + 4 * k, float) = position[k];
                RR_AT(key, 0x1a + 2 * k, int16_t) = (int16_t)(angles[k] * ANGLE_UNIT);
            }
            RR_AT(key, 0x18, uint16_t) = (uint16_t)P.key_cs[slot];
            list_append(moves, key);
            game.release(key);
        }
        for (; P.input_frame[slot] < n; P.input_frame[slot]++) {
            const float t = ghost_time(&P.frames[P.input_frame[slot]]);
            const int bits = P.frames[P.input_frame[slot]].racer[slot].ghost_bits;
            if (t < 0.0f || t > end || bits == P.input_last[slot]) continue;
            void *key = game.input_key_new(0);
            if (!key) break;
            RR_AT(key, 0xc, uint16_t) = (uint16_t)bits;
            RR_AT(key, 0xe, uint16_t) = (uint16_t)(t * 100.0f + 0.5f);
            list_append(inputs, key);
            game.release(key);
            P.input_last[slot] = bits;
        }
        RR_AT(ghost, 0x28, float) = end + 60.0f;        /* _duration */
    }
    for (uint32_t i = P.laps_scanned > 0 ? P.laps_scanned : 1; i < n; i++)
        for (int r = 0; r < RR_RACERS; r++) {
            const int was = P.frames[i - 1].racer[r].lap, now = P.frames[i].racer[r].lap;
            if (now > was && was >= 1 && now - 1 < MAX_LAPS) P.lap_start[r][now - 1] = P.frames[i].race_time;
        }
    P.laps_scanned = n;
}

static void *fn(size_t usa, int eu_moved, int eu);
/* ---- Items and hits (ported from Replay Mode's play.c). They come from the recording only: crates give nothing
 * (runtime_replay_crate), the karts' own item uses (the AI's, the pad's) and the game's hits are refused
 * (runtime_replay.c's gates at 0xf0b070 and 0xe330f1); at the recorded moments a shuffle starts that ends with the
 * recorded item (controller +0x78 _crateCollectedPowerUpType, as a crate of a given type does), the item is used
 * (the game throws the real one) and the hit is taken. The watched racer's item events go to the HUD (0xe368a0). ---- */
typedef void ABI item_give_fn(void *controller, int type, int backward, int juiced, int boss, float lifetime);
typedef void ABI controller_fn(void *controller);
typedef void ABI shuffle_start_fn(void *controller, float zero);
typedef int ABI take_damage_fn(void *damage, int type, void **attacker, int flag);
typedef void *ABI event_new_fn(int pool);
typedef int ABI faction_fn(void *entity);
typedef void ABI hud_event_fn(void *project, void *event);
static event_new_fn *TAGS_SPAWN;        /* the kart markers' spawn event: nicknames() */

static struct {
    item_give_fn *give;
    controller_fn *use, *shuffle_end;
    shuffle_start_fn *shuffle_start;
    take_damage_fn *take_damage;
    event_new_fn *show, *hide, *item, *charges, *reset;
    faction_fn *faction;
    int ready, using, hurting;
    void *slot_kart;                    /* the kart the HUD's item slot shows */
    uint32_t last;                      /* the frame the item events were made up to */
    uint32_t hit_used[RR_RACERS], hit_frame[RR_RACERS];
    int told;
} IT;

#define HIT_NEAR (RR_RATE * 2 / 5)      /* frames around a recorded hit a real one counts for: 0.4 s */
#define HIT_WAIT (RR_RATE * 7 / 20)     /* a recorded hit waits 0.35 s for its real one */

static int items_running(void) { return IT.ready && P.pending == 2; }

int ABI runtime_replay_item_gate(void) { return !items_running() || IT.using; }

static void *items_of(void *kart) { return kart ? rr_component(kart, RR_D_POWER_UP_META) : 0; }

static int hit_starts(int r, uint32_t i) {
    if (i == 0 || i >= frames_now()) return 0;
    const struct rr_sample *a = &P.frames[i - 1].racer[r], *b = &P.frames[i].racer[r];
    return b->damage && b->damage != a->damage && b->damage - 1 < 11;
}

static uint32_t frame_index(void) { return (uint32_t)(frame_now() - P.frames); }

/* A real hit (a replayed item reached a kart) is taken when its racer has a recorded hit near then: the game's own
 * hit then plays with all its effects. Recorded hits no real one took are made a little later (hurt). */
int ABI runtime_replay_take_damage(void *damage, int type) {
    (void)type;
    int ok = !items_running() || IT.hurting;
    void *meta = rr_ptr(RR_D_DAMAGE_META);
    for (int r = 0; !ok && r < RR_RACERS; r++) {
        void *kart = kart_of_racer(r);
        if (!kart || !meta || rr_find_component(kart, meta) != damage) continue;
        const uint32_t now = frame_index(), n = frames_now();
        for (uint32_t i = now > HIT_NEAR ? now - HIT_NEAR : 1; i <= now + HIT_NEAR && i < n && !ok; i++)
            if (i > IT.hit_used[r] && hit_starts(r, i)) {
                ok = 1;
                IT.hit_used[r] = i;
                if (IT.hit_frame[r] == i) IT.hit_frame[r] = 0;
            }
    }
    return ok;
}

/* The item racer r holds next (within 6 s of frame i), or -1 */
static int next_item(int r, uint32_t i) {
    const uint32_t n = frames_now();
    for (uint32_t k = i; k < n && k < i + RR_RATE * 6; k++)
        if (P.frames[k].racer[r].item >= 0) return P.frames[k].racer[r].item;
    return -1;
}

/* The kart holds this item: a running shuffle ends now with it (as a use during a shuffle does, 0xf0b403), else it is
 * given when the kart holds another or none */
static void hold(void *controller, int type) {
    if (type < 0 || type >= 13) return;
    if (RR_AT(controller, 0x40, uint8_t)) {
        RR_AT(controller, 0x78, int32_t) = type;
        IT.shuffle_end(controller);
        return;
    }
    void *stored = rr_handle_object(&RR_AT(controller, 0x30, void *));
    if (stored && rr_item_type(RR_AT(controller, 0x30, void *)) == type) return;
    /* an item it still holds goes first: the game keeps it, and the next use threw the wrong one */
    if (stored) game.make_handle(&RR_AT(controller, 0x30, void *), 0);
    IT.give(controller, type, 0, 0, 0, 0.0f);
}

static int shield_up(void *kart);
static void hurt(void *kart, int type);
static void use_item(void *kart, int type, uint16_t flags) {
    void *controller = items_of(kart);
    if (!controller) return;
    const unsigned launch = (flags & RS_LAUNCH_MASK) >> RS_LAUNCH_SHIFT;
    if (launch) RR_AT(controller, 0x28, int32_t) = (int32_t)launch - 1;     /* forward, neutral, backward as recorded */
    /* a shield still up (the replay had no hit to end it): the next use would throw it, not this item */
    if (type != 8 && shield_up(kart)) hurt(kart, 4);
    hold(controller, type);
    IT.using = 1;
    IT.use(controller);
    IT.using = 0;
}

/* A ghost takes no damage (CKartControllerComponent +0x260 _isGhost): not a ghost for the call */
static void hurt(void *kart, int type) {
    void *damage = rr_component(kart, RR_D_DAMAGE_META), *move = rr_component(kart, RR_D_KART_CONTROLLER_META);
    void *attacker = 0;
    if (!damage || !move) return;
    const uint8_t ghost = RR_AT(move, 0x260, uint8_t);
    RR_AT(move, 0x260, uint8_t) = 0;
    IT.hurting = 1;
    IT.take_damage(damage, type, &attacker, 0);
    IT.hurting = 0;
    RR_AT(move, 0x260, uint8_t) = ghost;
}

/* The item slot: the game sends a kart's item events to that kart's own HUD (the HUD component's project +0x38,
 * vtable +0xb0). In a replay the watched kart's go to the HUD (the player's kart's), the others' nowhere. */
static void to_hud(void *kart, void *event) {
    void *meta = rr_ptr(D_HUD_META);
    void *hud = kart && meta ? rr_find_component(kart, meta) : 0;
    void *project = hud ? rr_handle_object(&RR_AT(hud, 0x38, void *)) : 0;
    if (project) RR_AT(*(void **)project, 0xb0, hud_event_fn *)(project, event);
}

int ABI runtime_replay_power_event(void *controller, void *event) {
    if (!items_running()) return 1;
    void *kart = controller ? RR_AT(controller, 0x18, void *) : 0;
    if (kart && P.kart && kart == kart_of_racer(P.watch)) to_hud(P.kart, event);
    return 0;
}

static void send_event(void *event) {
    if (!event) return;
    to_hud(P.kart, event);
    game.release(event);
}

/* A new watched racer: its item at once */
static void item_slot(void) {
    void *kart = kart_of_racer(P.watch), *controller = items_of(kart);
    if (kart == IT.slot_kart || !controller || !P.kart) return;
    IT.slot_kart = kart;
    send_event(IT.reset(0));
    void *stored = rr_handle_object(&RR_AT(controller, 0x30, void *));
    if (!stored) {
        send_event(IT.hide(0));
        return;
    }
    send_event(IT.show(0));
    char *racers = rr_ptr(RR_D_RACER_MANAGER);
    void *racer = racers ? game.racer_of(racers, kart) : 0;
    void *event = IT.item(0);
    if (event) {
        RR_AT(event, 0x18, int32_t) = RR_AT(stored, 0x54, int32_t);         /* as the store (0xe371a0) does */
        RR_AT(event, 0x1c, uint8_t) = racer && RR_AT(racer, 0x9c, int32_t) > 9;
        RR_AT(event, 0x20, int32_t) = IT.faction(kart);
        send_event(event);
    }
    const int charges = frame_now()->racer[P.watch].item_count;
    if (charges > 1 && (event = IT.charges(0))) {
        RR_AT(event, 0x18, int32_t) = charges;
        send_event(event);
    }
}

#define D_ENTITY_TABLE 0x2a92880
#define DROPPED 16
typedef void *ABI meta_fn(void *object);
typedef void ABI crate_attach_fn(void *crate);

static struct {
    crate_attach_fn *crate_attach;
    struct { void *kart; int type; float at[3]; } dropped[DROPPED];
    int dropped_count, dropped_wait;
} TRACK;

static void tags_reset(void);
static void items_reset(void) {
    tags_reset();
    items_restore = 0;
    TRACK.dropped_count = TRACK.dropped_wait = 0;
    IT.last = 0;
    IT.slot_kart = 0;
    memset(IT.hit_used, 0, sizeof IT.hit_used);
    memset(IT.hit_frame, 0, sizeof IT.hit_frame);
}

/* ---- Items on the track (ported from Replay Mode's play.c): a rewind clears them (drive: despawn_items), so the
 * ones thrown, dropped or worn not long before come back; a recorded TNT goes on its racer's head; a recorded shield's
 * end is a hit. Each item on the track is an entity with a spawned power-up component (CSpawnedPowerUpComponent's
 * classes: their metas have its meta at +0x40; +0x28 its owner's handle, +0x34 its EPowerUpType). ---- */
/* Each item on the track: its entity slot and component, -1 at the end */
static int next_spawned(int from, void **component_out) {
    char *table = rr_ptr(D_ENTITY_TABLE);
    void *spawned = rr_ptr(RR_D_SPAWNED_META);
    void *gens = table ? RR_AT(table, 0x28, void *) : 0;
    void **entities = table ? RR_AT(table, 0x20, void **) : 0;
    uint16_t *generation = gens ? RR_AT(gens, 0x20, uint16_t *) : 0;
    for (int i = from; entities && generation && spawned && i < rr_count(gens) && i < 4096; i++) {
        void *list = (generation[i] & 0x8000) && entities[i] ? RR_AT(entities[i], 0x18, void *) : 0;
        for (int k = 0; k < rr_count(list); k++) {
            void *component = rr_item(list, k);
            void *meta = component ? (**(meta_fn ***)component)(component) : 0;
            if (meta && RR_AT(meta, 0x40, void *) == spawned) {
                *component_out = component;
                return i;
            }
        }
    }
    return -1;
}

static void *entity_at(int slot) {
    char *table = rr_ptr(D_ENTITY_TABLE);
    return table ? RR_AT(table, 0x20, void **)[slot] : 0;
}

static float distance2(void *entity, const float *at) {
    float d = 0.0f;
    for (int c = 0; c < 3; c++) {
        const float x = RR_AT(entity, 0x20 + 4 * c, float) - at[c];
        d += x * x;
    }
    return d;
}

/* The kart's active power-up (controller +0x38, it stays on the last thrown item) is a shield, and its spawned shield is there */
static int shield_up(void *kart) {
    void *controller = items_of(kart), *active = controller ? rr_handle_object(&RR_AT(controller, 0x38, void *)) : 0, *component;
    void *meta = active ? (**(meta_fn ***)active)(active) : 0;
    const char *name = meta ? RR_AT(meta, 0x10, const char *) : 0;
    if (!name || strcmp(name, "CPowerShieldComponent")) return 0;
    for (int i = next_spawned(0, &component); i >= 0; i = next_spawned(i + 1, &component))
        if (RR_AT(component, 0x34, int32_t) == 8 && rr_handle_object(&RR_AT(component, 0x28, void *)) == kart) return 1;
    return 0;
}

/* What racer r has at frame i, on its kart: a shuffle that ends with its next item, the item it holds, or none */
static void sync_item(void *controller, int r, uint32_t i) {
    const struct rr_sample *s = &P.frames[i].racer[r];
    const int shuffling = RR_AT(controller, 0x40, uint8_t), next = next_item(r, i);
    if (s->flags & RS_SHUFFLING) {
        if (shuffling) {
            if (next >= 0) RR_AT(controller, 0x78, int32_t) = next;
        } else if (next >= 0) {
            if (rr_handle_object(&RR_AT(controller, 0x30, void *))) game.make_handle(&RR_AT(controller, 0x30, void *), 0);
            RR_AT(controller, 0x78, int32_t) = next;
            IT.shuffle_start(controller, 0.0f);
        }
        return;
    }
    if (s->item >= 0) {
        hold(controller, s->item);          /* a running shuffle ends now with it */
        return;
    }
    /* none: a shuffle still running (a jump back from after it) would give an item of its own */
    if (shuffling) IT.shuffle_end(controller);
    if (rr_handle_object(&RR_AT(controller, 0x30, void *))) game.make_handle(&RR_AT(controller, 0x30, void *), 0);
}

static int thrown_seconds(int type) {
    return type == 0 || type == 9 ? 3 : type == 1 || type == 10 ? 4 : type == 2 || type == 3 ? 90 : type == 4 || type == 8 || type == 12 ? 12 : 0;
}

/* A crate or beaker dropped by racer `owner` at frame k at `at` went off before frame now: a racer took a hit near it,
 * or a racer drove over it (a mask or a shield takes it without a hit; the owner only after it left it behind) */
#define HIT_RANGE 450.0f
#define OVER_RANGE 220.0f
static int went_off(const float *at, uint32_t k, uint32_t now, int owner) {
    for (uint32_t j = k + 1; j <= now; j++)
        for (int r = 0; r < RR_RACERS; r++) {
            if (!(P.frames[j].racer[r].flags & RS_PRESENT)) continue;
            float d = 0.0f;
            for (int c = 0; c < 3; c++) {
                const float x = (float)P.frames[j].racer[r].position[c] / RR_POSITION - at[c];
                d += x * x;
            }
            if (hit_starts(r, j) && d < HIT_RANGE * HIT_RANGE) return 1;
            if (d < OVER_RANGE * OVER_RANGE && (r != owner || j > k + RR_RATE * 2)) return 1;
        }
    return 0;
}

/* After a jump (the track cleared): the items thrown, dropped or worn not long before come back. Thrown ones start
 * from the kart; dropped ones are put where they were dropped a few frames later (an item comes out after its use). */
static void restore_items(uint32_t now) {
    TRACK.dropped_count = 0;
    for (int r = 0; r < RR_RACERS; r++) {
        void *kart = kart_of_racer(r), *controller = items_of(kart);
        if (!controller) continue;
        for (uint32_t k = now > 90 * RR_RATE ? now - 90 * RR_RATE : 1; k <= now; k++) {
            const struct rr_sample *a = &P.frames[k - 1].racer[r], *b = &P.frames[k].racer[r];
            if (b->item_uses == a->item_uses || a->item < 0 || a->item >= 13 ||
                (now - k) / RR_RATE >= (uint32_t)thrown_seconds(a->item))
                continue;
            const int dropped = a->item == 2 || a->item == 3;
            float at[3];
            for (int c = 0; c < 3; c++) at[c] = (float)b->position[c] / RR_POSITION;
            /* thrown forward (launch 1: forward + 1), it landed somewhere ahead the recording does not tell: only while
             * it can still be in the air */
            const unsigned launch = (b->flags & RS_LAUNCH_MASK) >> RS_LAUNCH_SHIFT;
            if (dropped && launch == 1 && now - k > (uint32_t)RR_RATE * 2) continue;
            if (dropped && (went_off(at, k, now, r) || TRACK.dropped_count >= DROPPED)) continue;
            use_item(kart, a->item, b->flags);
            if (dropped && launch != 1) {
                TRACK.dropped[TRACK.dropped_count].kart = kart;
                TRACK.dropped[TRACK.dropped_count].type = a->item;
                for (int c = 0; c < 3; c++) TRACK.dropped[TRACK.dropped_count].at[c] = at[c];
                TRACK.dropped_count++;
            }
        }
        sync_item(controller, r, now);
    }
    TRACK.dropped_wait = 4;
    IT.slot_kart = 0;       /* the item slot shows what the watched racer holds now */
}

static void place_dropped(void) {
    void *component;
    for (int d = 0; d < TRACK.dropped_count; d++) {
        void *best = 0;
        float near = 2000.0f * 2000.0f;
        for (int i = next_spawned(0, &component); i >= 0; i = next_spawned(i + 1, &component)) {
            void *entity = entity_at(i);
            if (!entity || RR_AT(component, 0x34, int32_t) != TRACK.dropped[d].type ||
                rr_handle_object(&RR_AT(component, 0x28, void *)) != TRACK.dropped[d].kart)
                continue;
            const float gone = distance2(entity, &RR_AT(TRACK.dropped[d].kart, 0x20, float));
            if (gone < near) near = gone, best = entity;
        }
        for (int c = 0; best && c < 3; c++) RR_AT(best, 0x20 + 4 * c, float) = TRACK.dropped[d].at[c];
    }
    TRACK.dropped_count = 0;
}

/* A recorded TNT on a racer's head: the nearest crate on the track goes on its head, as a crate's own hit does (+0xa8 the
 * kart, +0xb0 state 2, 0xe40d60). A recorded shield that ended: the hit that took it. */
static void tnt(uint32_t now) {
    void *component;
    for (int r = 0; r < RR_RACERS && now && TRACK.crate_attach; r++) {
        void *kart = kart_of_racer(r);
        const int on = !!(P.frames[now].racer[r].flags & RS_TNT), was = !!(P.frames[now - 1].racer[r].flags & RS_TNT);
        if (kart && (P.frames[now - 1].racer[r].flags & RS_SHIELD) && !(P.frames[now].racer[r].flags & RS_SHIELD) && shield_up(kart))
            hurt(kart, 4);
        if (!kart || on == was || !on) continue;
        void *best = 0;
        float near = 4000.0f * 4000.0f;
        for (int i = next_spawned(0, &component); i >= 0; i = next_spawned(i + 1, &component)) {
            void *entity = entity_at(i);
            if (!entity || RR_AT(component, 0x34, int32_t) != 2 || RR_AT(component, 0xb0, int32_t) == 2) continue;
            const float d = distance2(entity, &RR_AT(kart, 0x20, float));
            if (d < near) near = d, best = component;
        }
        if (!best) continue;
        game.make_handle(&RR_AT(best, 0xa8, void *), kart);
        RR_AT(best, 0xb0, int32_t) = 2;
        TRACK.crate_attach(best);
    }
}


/* A recorded explosive hit (a missile's or a bomb's): online it flew on the other PC's path, here it can be far from the
 * kart at the hit. In the last GUIDE_TIME the nearest thrown missile or bomb (not the racer's own) flies to the kart and
 * arrives at the recorded time, with the hit (ported from Replay Mode). */
#define GUIDE_TIME 1.0f
#define GUIDE_RANGE 9000.0f
static struct {
    struct { int slot; uint16_t generation; uint32_t hit; float t0, from[3]; } racer[RR_RACERS];
    uint32_t hit[RR_RACERS], last;
} GUIDE;

static int guide_start(int r, void *kart, uint32_t hit) {
    char *table = rr_ptr(D_ENTITY_TABLE);
    float best = GUIDE_RANGE * GUIDE_RANGE;
    int found = -1;
    void *component;
    for (int i = next_spawned(0, &component); i >= 0; i = next_spawned(i + 1, &component)) {
        const int type = RR_AT(component, 0x34, int32_t);
        int taken = 0;
        if ((type != 0 && type != 1 && type != 9 && type != 10) || rr_handle_object(&RR_AT(component, 0x28, void *)) == kart) continue;
        for (int g = 0; g < RR_RACERS; g++) taken |= GUIDE.racer[g].slot == i + 1;
        void *entity = entity_at(i);
        if (!entity || taken) continue;
        const float d = distance2(entity, &RR_AT(kart, 0x20, float));
        if (d < best) best = d, found = i;
    }
    if (found < 0 || !table) return 0;
    void *entity = entity_at(found);
    GUIDE.racer[r].slot = found + 1;
    GUIDE.racer[r].generation = RR_AT(RR_AT(table, 0x28, void *), 0x20, uint16_t *)[found];
    GUIDE.racer[r].hit = hit;
    GUIDE.racer[r].t0 = P.time;
    for (int c = 0; c < 3; c++) GUIDE.racer[r].from[c] = RR_AT(entity, 0x20 + 4 * c, float);
    return 1;
}

static void guide_items(uint32_t now) {
    char *table = rr_ptr(D_ENTITY_TABLE);
    if (now < GUIDE.last) memset(&GUIDE, 0, sizeof GUIDE);
    GUIDE.last = now;
    if (P.paused || P.scrubbing || !P.started || !table) return;
    const uint32_t n = frames_now();
    for (int r = 0; r < RR_RACERS; r++) {
        void *kart = kart_of_racer(r);
        if (!kart) continue;
        if (!GUIDE.racer[r].slot) {
            for (uint32_t k = now + 1; k < n && k < now + (uint32_t)(GUIDE_TIME * RR_RATE); k++)
                if (hit_starts(r, k) && P.frames[k].racer[r].damage == 2 + 1 && k > GUIDE.hit[r] && k > IT.hit_used[r]) {
                    GUIDE.hit[r] = k;
                    guide_start(r, kart, k);
                    break;
                }
            continue;
        }
        const int slot = GUIDE.racer[r].slot - 1;
        void *gens = RR_AT(table, 0x28, void *);
        void *entity = entity_at(slot);
        if (slot >= rr_count(gens) || RR_AT(gens, 0x20, uint16_t *)[slot] != GUIDE.racer[r].generation || !entity) {
            GUIDE.racer[r].slot = 0;        /* it went off on the way: the hit comes by itself */
            continue;
        }
        const uint32_t hit = GUIDE.racer[r].hit;
        const float end = P.frames[hit].time - P.header.go_time, span = end - GUIDE.racer[r].t0;
        float part = span > 0.0f ? (P.time - GUIDE.racer[r].t0) / span : 1.0f;
        if (part > 1.0f) part = 1.0f;
        for (int c = 0; c < 3; c++)
            RR_AT(entity, 0x20 + 4 * c, float) = GUIDE.racer[r].from[c] + (RR_AT(kart, 0x20 + 4 * c, float) - GUIDE.racer[r].from[c]) * part;
        if (part < 1.0f) continue;
        GUIDE.racer[r].slot = 0;
        if (hit > IT.hit_used[r]) {
            IT.hit_used[r] = hit;
            IT.hit_frame[r] = 0;
            hurt(kart, P.frames[hit].racer[r].damage - 1);
        }
    }
}

/* After the frame's item events (items_frame): the items back after a jump, the dropped ones in place, TNT and shields */
static void track_items(void) {
    if (!IT.ready || !P.started) return;
    const uint32_t now = frame_index();
    if (items_restore && !P.scrubbing && !P.rewound) {
        items_restore = 0;
        restore_items(now);
    }
    if (TRACK.dropped_wait && !--TRACK.dropped_wait) place_dropped();
    if (!P.paused && !P.scrubbing && now) tnt(now);
    guide_items(now);
}

/* Every frame, each racer: its wumpa count (CRacer +0x9c: the HUD's, and an item is juiced from 10) and, while the
 * replay goes forward, the item events of the frames passed since the last one; after a jump (rewind, forward, a live
 * catch-up) each kart holds what the racer holds there */
static void items_frame(void) {
    if (!IT.ready || !P.started) return;
    const struct rr_frame *f = frame_now();
    const uint32_t now = (uint32_t)(f - P.frames), last = IT.last;
    IT.last = now;
    const int forward = !P.paused && !P.scrubbing && now > last && now - last <= RR_RATE / 2;
    char *racers = rr_ptr(RR_D_RACER_MANAGER);
    for (int r = 0; r < RR_RACERS; r++) {
        void *kart = kart_of_racer(r);
        void *racer = kart && racers ? game.racer_of(racers, kart) : 0;
        void *controller = items_of(kart);
        if (!racer || !controller) continue;
        RR_AT(racer, 0x9c, int32_t) = f->racer[r].wumpa;
        if (!forward) {
            if (now == last) continue;
            IT.hit_used[r] = now, IT.hit_frame[r] = 0;
            sync_item(controller, r, now);
            IT.slot_kart = 0;
            continue;
        }
        for (uint32_t i = last + 1; i <= now; i++) {
            const struct rr_sample *a = &P.frames[i - 1].racer[r], *b = &P.frames[i].racer[r];
            if (b->item_uses != a->item_uses && a->item >= 0 && a->item < 13) use_item(kart, a->item, b->flags);
            if ((b->flags & RS_PRESSED) && shield_up(kart)) {       /* a bubble up: the same use throws it */
                const unsigned launch = (b->flags & RS_LAUNCH_MASK) >> RS_LAUNCH_SHIFT;
                if (launch) RR_AT(controller, 0x28, int32_t) = (int32_t)launch - 1;
                IT.using = 1;
                IT.use(controller);
                IT.using = 0;
            }
            const int type = next_item(r, i);
            if ((b->flags & RS_SHUFFLING) && !(a->flags & RS_SHUFFLING) && type >= 0 && !RR_AT(controller, 0x40, uint8_t) &&
                !rr_handle_object(&RR_AT(controller, 0x30, void *))) {
                RR_AT(controller, 0x78, int32_t) = type;
                IT.shuffle_start(controller, 0.0f);
            }
            if (b->item >= 0 && a->item < 0 && !(b->flags & RS_SHUFFLING)) hold(controller, b->item);
            if (hit_starts(r, i) && i > IT.hit_used[r]) IT.hit_frame[r] = i;
        }
        const uint32_t wait = IT.hit_frame[r];
        if (wait && now >= wait + HIT_WAIT) {
            IT.hit_frame[r] = 0;
            if (wait > IT.hit_used[r]) {
                IT.hit_used[r] = wait;
                hurt(kart, P.frames[wait].racer[r].damage - 1);
            }
        }
    }
    track_items();
    item_slot();
}

static int items_install(int eu) {
    IT.give = (item_give_fn *)fn(0xe366b0, 0, eu);
    IT.use = (controller_fn *)fn(0xf0b070, 1, eu);
    IT.shuffle_start = (shuffle_start_fn *)fn(0xe363e0, 0, eu);
    IT.shuffle_end = (controller_fn *)fn(0xe37b50, 0, eu);
    IT.take_damage = (take_damage_fn *)fn(0xe330e0, 0, eu);
    IT.show = (event_new_fn *)fn(0x117ee70, 1, eu);
    IT.hide = (event_new_fn *)fn(0x117ef90, 1, eu);
    IT.item = (event_new_fn *)fn(0x117f1d0, 1, eu);
    IT.charges = (event_new_fn *)fn(0x117f530, 1, eu);
    IT.reset = (event_new_fn *)fn(0x117f410, 1, eu);
    IT.faction = (faction_fn *)fn(0xfe5790, 1, eu);
    if (!memcmp(fn(0x1198390, 1, eu), "\x55\x48\x89\xe5", 4)) TAGS_SPAWN = (event_new_fn *)fn(0x1198390, 1, eu);
    TRACK.crate_attach = (crate_attach_fn *)fn(0xe40d60, 0, eu);
    if (memcmp(TRACK.crate_attach, "\x55\x48\x89\xe5", 4)) TRACK.crate_attach = 0;      /* no TNT on heads */
    /* (0xf0b070 and 0xe330e0 start with jumps: the recorder's gate, the online mode's damage wrap) */
    void *const check[] = {IT.give, IT.shuffle_start, IT.shuffle_end, IT.show, IT.hide, IT.item, IT.charges, IT.reset, IT.faction};
    for (size_t i = 0; i < sizeof check / sizeof *check; i++)
        if (memcmp(check[i], "\x55\x48\x89\xe5", 4)) {
            printf("Replay: item function %zu is not the expected code: replays keep the game's own items\n", i);
            return 0;
        }
    IT.ready = 1;
    return 1;
}

/* ---- Nicknames over the karts: the game's own kart markers. The HUD's project has a CGuiBehaviorKartMarkerManager (+0x18
 * _markers: handles to spawned placeables, each with a CGuiBehaviorKartMarker). On the spawn event (0x1198390, sent by the
 * HUD in the races that show them, 0xe5bd19) it makes a marker for every racer but the screen's own, AI too (0x9d82d0).
 * A replay sends that event; each marker (+0x70 _racerHandle) then gets its racer's name in +0x50 _playerTagPlaceable, a
 * text. The watched racer's stays empty: the camera is on it. ---- */
#define KART_MARKER_MANAGER_VTABLE 0x2545970
#define KART_MARKER_VTABLE 0x2545878

static struct {
    event_new_fn *spawn;
    int sent, tries, told;
    void *manager, *owner;
    uint32_t frames;
} TAGS;

static void *behavior_owner;            /* find_behavior: the placeable that holds what it found */
static void *find_behavior(void *placeable, size_t vtable, int depth) {
    if (!placeable || depth > 24) return 0;
    void *list = RR_AT(placeable, 0xe8, void *);
    for (int i = 0; i < rr_count(list); i++) {
        void *b = rr_item(list, i);
        if (b && RR_AT(b, 0, const void *) == (const void *)rr_game(vtable)) {
            behavior_owner = placeable;
            return b;
        }
    }
    void *children = RR_AT(placeable, 0x48, void *);
    for (int i = 0; i < rr_count(children); i++) {
        void *found = find_behavior(rr_item(children, i), vtable, depth + 1);
        if (found) return found;
    }
    return find_behavior(content(placeable), vtable, depth + 1);
}

static void nicknames(void) {
    TAGS.spawn = TAGS_SPAWN;
    if (!TAGS.spawn || !P.kart || !P.started) return;
    if (++TAGS.frames % 15) return;                     /* 4 times a second are enough */
    void *meta = rr_ptr(D_HUD_META);
    void *hud = meta ? rr_find_component(P.kart, meta) : 0;
    void *project = hud ? rr_handle_object(&RR_AT(hud, 0x38, void *)) : 0;
    if (!project) return;
    if (!TAGS.manager) {
        if (TAGS.tries > 12) return;
        if (!TAGS.tries++ ) printf("Replay: looking for the HUD's kart markers\n");
        TAGS.manager = find_behavior(RR_AT(project, 0x10, void *), KART_MARKER_MANAGER_VTABLE, 0);
        TAGS.owner = TAGS.manager ? behavior_owner : 0;
        if (!TAGS.manager) {
            if (TAGS.tries > 12) printf("Replay: this HUD has no kart markers: no nicknames over the karts\n");
            fflush(stdout);
            return;
        }
    }
    if (!TAGS.sent) {
        TAGS.sent = 1;
        send_event(TAGS.spawn(0));
        printf("Replay: kart markers asked for\n");
        fflush(stdout);
        return;
    }
    /* the manager hides itself outside online races and battles (0x9d85c5: its placeable's hidden bit, +0xc 0x400) */
    if (TAGS.owner) game.set_hidden(TAGS.owner, 0);
    char *racers = rr_ptr(RR_D_RACER_MANAGER);
    void *list = RR_AT(TAGS.manager, 0x18, void *);
    const int n = rr_count(list);
    int named = 0;
    for (int i = 0; racers && i < n; i++) {
        void *placeable = rr_handle_object(&RR_AT(list, 0x20, void **)[i]);
        void *marker = placeable ? find_behavior(placeable, KART_MARKER_VTABLE, 0) : 0;
        void *racer = marker ? rr_handle_object(&RR_AT(marker, 0x70, void *)) : 0;
        void *tag = marker ? RR_AT(marker, 0x50, void *) : 0;
        if (!racer || !tag) continue;
        game.set_hidden(placeable, 0);
        for (int r = 0; r < RR_RACERS; r++) {
            void *kart = kart_of_racer(r);
            if (!kart || game.racer_of(racers, kart) != racer) continue;
            const char *want = r == P.watch ? "" : P.names[r];
            const char *now = RR_AT(tag, 0x30, const char *);
            if (!now || strcmp(now, want)) game.set_text(tag, want);
            RR_AT(marker, 0x8a, uint8_t) = 0;           /* _keepDisplayTextUpdated: the text stays ours */
            named++;
            break;
        }
    }
    if (TAGS.told != n) {
        TAGS.told = n;
        printf("Replay: %d kart markers, %d named\n", n, named);
        fflush(stdout);
    }
}

static void tags_reset(void) {
    TAGS.sent = TAGS.tries = TAGS.told = 0;
    TAGS.manager = TAGS.owner = 0;
    TAGS.frames = 0;
}

void runtime_replay_play_frame(void) {
    char *frame = rr_ptr(RR_D_FRAME);
    if (P.pending != 2) return;
    if (frame && !RR_AT(frame, 0x28, uint8_t)) P.level_time += RR_AT(frame, 0x24, float);
    put_choice_back();
    if (!P.set_up) set_up();
    P.world_id = rr_world_id();
    camera_triggers();
    if (P.ghosts_later) {                       /* live, before the friend's GO: wait for it on the grid */
        live_go();
        if (P.header.go_time < 0.0f) return;
        P.ghosts_later = 0;
        int ghosts = 0;
        for (int i = 0; i < RR_RACERS; i++)
            if (P.kart_slot[i] >= 0) ghosts += !!(P.ghost[i] = build_ghost(i));
        for (int r = 0; r < RR_RACERS; r++)
            for (uint32_t i = 1; i < P.count; i++) {
                const int was = P.frames[i - 1].racer[r].lap, now = P.frames[i].racer[r].lap;
                if (now > was && was >= 1 && now - 1 < MAX_LAPS) P.lap_start[r][now - 1] = P.frames[i].race_time;
            }
        printf("Replay: the friend's race reached GO: %d racers\n", ghosts);
        fflush(stdout);
    }
    const float go = P.started ? 0.0f : rr_race_clock();
    if (go > 0.0f && P.kart) {
        P.started = 1;
        P.time = go;
        const float newest = last_time() - 1.0f;   /* live: close to the race, not from its start */
        if (P.live && newest > go + 1.0f) P.time = newest, P.rewound = 1;
        else if (P.live && newest < go) P.time = newest > 0.0f ? newest : 0.0f;   /* our GO came first: from the friend's */
    }
    live_extend();
    const float dt = frame ? RR_AT(frame, 0x18, float) : 0.0f;
    if (P.started && !(frame && RR_AT(frame, 0x28, uint8_t))) read_buttons(dt);
    drive(P.started ? P.time + RACE_START : RACE_START - 0.2f);
    unghost();
    follow();
    items_frame();
    nicknames();
    hud();
    speed_now(dt);
    publish_view();
    const float finish = P.header.racer[P.watch].finish_time;
    set_race_clock((frame_now()->racer[P.watch].flags & RS_FINISHED) && finish > 0.0f && finish < P.time ? finish : P.time);
}

/* A replay's race starts without the camera fly-over over the track: CCutsceneManager's own _skipCutscenes
 * (+0x7a) is on while a replay loads or plays, and goes back to what it was after. _Instance and the field
 * offsets: the game's reflection data (scripts/patches.py cutscene_state; the same in the EU edition). */
#define D_CUTSCENE_MANAGER 0x2ab67f0
static void skip_intro(void) {
    char *manager = rr_ptr(D_CUTSCENE_MANAGER);
    if (!manager) return;
    /* the game's own option, Auto-skip fly-by (COctaneGlobalOptionsData +0x2a, reached by the game +0xdb0 global save
     * data, +0x78 _octaneGlobalOptions): on from the replay's level start until GO, then back as the player had it
     * (only that long: the options are saved now and then, and the player's own choice must stay theirs) */
    char *g = rr_ptr(RR_D_GAME);
    char *save = g ? RR_AT(g, 0xdb0, char *) : 0;
    char *options = save ? RR_AT(save, 0x78, char *) : 0;
    const int want = P.pending == 2 && !P.started;
    if (options && want && !P.option_set) {
        P.option_set = 1, P.option_was = RR_AT(options, 0x2a, uint8_t);
        RR_AT(options, 0x2a, uint8_t) = 1;
    } else if (P.option_set && !want) {
        P.option_set = 0;
        if (options) RR_AT(options, 0x2a, uint8_t) = P.option_was;
    }
    if (P.pending) {
        if (!P.skip_set) P.skip_set = 1, P.skip_was = RR_AT(manager, 0x7a, uint8_t), P.flyby_seen = 0;
        RR_AT(manager, 0x7a, uint8_t) = 1;
        char *active = RR_AT(manager, 0xa0, char *);   /* _activeCutsceneData, _isFlybyIntro +0x30 */
        if (active && RR_AT(active, 0x30, uint8_t) && !P.flyby_seen) {
            P.flyby_seen = 1;
            printf("Replay: the track's fly-over plays (auto-skip %s)\n", options ? "on" : "not found");
            fflush(stdout);
        }
    } else if (P.skip_set) {
        P.skip_set = 0;
        RR_AT(manager, 0x7a, uint8_t) = P.skip_was;
    }
}

static int racing;                              /* the last frame: a kart and a race clock */
int runtime_replay_chat_allowed(void) {
    return !__atomic_load_n(&racing, __ATOMIC_RELAXED) || runtime_replay_local_finished();
}

/* Every frame (the pad read, runtime_replay.c): the menu's request, the start sequence, the character given back */
void ABI runtime_replay_tick(void) {
    left_level();
    skip_intro();
    __atomic_store_n(&racing, rr_kart_of(0, 0) && race_timer(), __ATOMIC_RELAXED);
    runtime_online_mode_note_frame();
    apply_look();                               /* every frame, loads too: the map start reads them */
    char *frame = rr_ptr(RR_D_FRAME);
    const int number = frame ? RR_AT(frame, 0x20, int32_t) : 0;
    if (number == P.last_frame) return;
    P.last_frame = number;
    char path[sizeof P.request];
    pthread_mutex_lock(&P.lock);
    snprintf(path, sizeof path, "%s", P.request);
    P.request[0] = 0;
    pthread_mutex_unlock(&P.lock);
    pthread_mutex_lock(&P.lock);
    const int live = P.live_request;
    P.live_request = 0;
    struct rr_header next_header;
    uint8_t *next = 0;
    size_t next_size = 0;
    uint32_t next_frames = 0;
    if (live == 2) {
        next_header = NEXT.header, next = NEXT.packed, next_size = NEXT.size, next_frames = NEXT.frames;
        NEXT.packed = 0;
    }
    pthread_mutex_unlock(&P.lock);
    /* the followed friend's next race in place of the one that ended on screen: this level goes (its ghosts
     * with it, runtime_replay_play_level_end) and the next race starts as from the menus */
    int chain = 0;
    if (live == 2 && next && runtime_replay_can_chain()) {
        drop_ghosts_at_end = 1;
        P.pending = 1;
        if (take_live(&next_header, next, next_size, next_frames)) {
            chain = 1;
            if (frame) RR_AT(frame, 0x2c, float) = 1.0f;
            printf("Replay: the friend's next race: leaving this one for it\n");
        } else {
            P.pending = 2;
            drop_ghosts_at_end = 0;
        }
    }
    free(next);
    if (chain || ((path[0] || live == 1) && !P.pending && P.step == STEP_NONE)) {
        char full[800];
        rr_race_dir(full, sizeof full - 80);
        strncat(full, "/", sizeof full - strlen(full) - 1);
        strncat(full, path, sizeof full - strlen(full) - 1);
        accolades(0);
        if (!chain) {                   /* a chain keeps this level's ghosts until it goes, and the saved settings */
            memset(P.ghost, 0, sizeof P.ghost);
            memset(P.attached, 0, sizeof P.attached);
            if (!P.give_back) P.mirror_was = -1;        /* (the last replay's not given back yet: still the player's) */
        }
        P.accolade_player = -2;
        P.hero_told = 0;
        P.set_up = P.started = P.paused = P.scrubbing = P.rewound = 0;
        P.camera = P.target = P.kart = 0;
        P.time = 0.0f;
        memset(P.fed, 0, sizeof P.fed);
        if (live) {
            P.live_ended = 0;
            P.laps_scanned = 0;
            snprintf(path, sizeof path, "%s", "(live)");
        }
        if (!chain && !live && !load(full)) {
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
    /* not on the title screen (no world there): the driver given back loads its hero, whose load reads the
     * world's components (a null world: guest 0x3dbfad called from 0xda8eb1, code 3 on leaving a replay) */
    if (P.give_back && !P.pending && !ai && rr_world() && rr_world_id() != P.world_id) {
        /* (the race's world stays until the next level is loaded: not while it goes, the same crash) */
        if (++P.back_frames >= 30) give_back();
    }
    if (P.step != STEP_NONE) {
        /* the map loads 1.5 s after the player's kart is made (at most 10 s) */
        if (P.wait) P.wait--;
        else if (P.step == STEP_LOAD) {
            /* the recording player's character is asked as the map starts loading (the frames do not count
             * while it loads: 5 frames later came after the level start), and again 5 frames later */
            cast_player(1);
            direct_go();
            cast_player(1);
            apply_look();                       /* after direct_go, which applies the save's choice */
            fflush(stdout);
            P.step = STEP_NONE, P.cast_in = 5;
        }
        else if (direct_ready() || ++P.frames_waited > 600) P.step = STEP_LOAD, P.wait = 90;
        return;
    }
    if (P.cast_in && !--P.cast_in) {
        if (P.pending == 1) cast_player(1);
        fflush(stdout);
    }
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
    game.ghost_look_start_new = fn(0x112d8b0, 1, eu);
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
    game.set_text = fn(0x268ef0, 0, eu);
    game.set_hidden = fn(0x2694d0, 0, eu);
    game.hud_set_lap = fn(0xe63f20, 0, eu);
    game.find_driver = fn(0xf9c610, 1, eu);
    game.spawn_placeable = fn(0x253c00, 0, eu);
    game.set_position = fn(0x268f90, 0, eu);
    game.set_pivot = fn(0x269190, 0, eu);
    game.set_scale = fn(0x269120, 0, eu);
    game.set_rotation = fn(0x2690b0, 0, eu);
    game.set_opacity = fn(0x2694a0, 0, eu);
    game.set_color = fn(0x269460, 0, eu);
    game.hud_style_next = fn(0xe31f80, 0, eu);
    /* every function but player_at starts "push rbp; mov rbp, rsp" */
    void *const check[] = {game.ghost_new, game.move_key_new, game.input_key_new, game.list_reserve, game.list_set_count,
                           game.ghost_reset, game.ghost_attach, game.ghost_update, game.ai_load, game.ai_count, game.racer_of,
                           game.camera_instance, game.active_camera, game.camera_refresh, game.make_handle,
                           game.ghost_look_end_new, game.ghost_look_start_new, game.send_message, game.player_kart, game.despawn_items, game.string,
                           game.string_release, game.release, game.set_next_race_mode, game.set_next_map, game.load_next_map,
                           game.apply_choice, game.apply_player_count, game.enter_customization, game.zone_by_name,
                           game.decal_find, game.set_text, game.hud_set_lap, game.find_driver, game.spawn_placeable, game.hud_style_next};
    for (size_t i = 0; i < sizeof check / sizeof *check; i++)
        if (memcmp(check[i], "\x55\x48\x89\xe5", 4)) {
            printf("Replay: player function %zu is not the expected code: no replays\n", i);
            return 0;
        }
    items_install(eu);
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

/* The menu may start a replay: none is starting or playing, no race is on (no player kart), and not in the online mode:
 * its lobby is the game's online session even alone in a room, and a local race started there came back with "not all
 * players are ready" (the followed race loads once the online menus are left). That is the session, not the level:
 * octane_onlinelobby is also where a race or a replay left outside the online mode comes back to.
 * What stops it goes to the log when it changes. */
/* The title screen and the lobby: menus, never a race. Giving the player's own character back after a
 * replay (give_back) has the game load that character's kart there, in the player's kart slot: it is
 * the menu's, not a race's (the second replay was refused "the player has a kart" until a restart). */
static int has_word(const char *text, const char *word) {
    const size_t n = strlen(word);
    for (; *text; text++)
        if (!strncasecmp(text, word, n)) return 1;
    return 0;
}
static int menu_level(void) {
    const char *level = rr_current_level();
    return level && (has_word(level, "octane_startscreen") || has_word(level, "octane_onlinelobby"));
}

static const char *start_blocked;              /* runtime_replay_can_start's last "why" */
const char *runtime_replay_blocked_reason(void) { return __atomic_load_n(&start_blocked, __ATOMIC_RELAXED); }
int runtime_replay_can_start(void) {
    PingPeer peers[PING_MAX_PEERS];
    const char *why = P.pending || P.step != STEP_NONE ? "a replay is on"
                      : !rr_game(0) ? "the game is not up"
                      : rr_kart_of(0, 0) && !menu_level() ? "the player has a kart"
                      : race_timer() ? "a race clock is on"
                      : runtime_online_mode_lobby_open() ? "an online lobby is on screen"
                      : runtime_public_match_active() ? "a public match"
                      : runtime_private_match_active() || runtime_online_mode_room_on() ? "a private room"
                      : runtime_net_ping_view(peers, PING_MAX_PEERS) > 0 ? "players in the room"
                                                                         : 0;
    if (__atomic_exchange_n(&start_blocked, why, __ATOMIC_RELAXED) != why) {
        const char *level = rr_current_level();
        printf("Replay: %s%s (level %s)\n", why ? "cannot start now: " : "can start", why ? why : "", level ? level : "-");
        fflush(stdout);
    }
    return !why;
}

/* ---- live (runtime_replay.h): runtime_spectate.c's thread gives the frames ---- */
static uint32_t unpack_more(const uint8_t *packed, size_t size, uint32_t frames) {
    static const struct rr_frame ZERO;
    const uint8_t *in = packed, *end = packed + size;
    uint32_t n = frames_now();
    for (; n < frames && n < RR_MAX_FRAMES; n++)
        if (!rr_unpack_frame(&P.frames[n], n > 0 ? &P.frames[n - 1] : &ZERO, n > 1 ? &P.frames[n - 2] : &ZERO, &in, end,
                             (int)P.header.version))
            break;
    return n;
}

/* 1 when a replay is over on screen: a friend's next live race can take its place */
int runtime_replay_can_chain(void) { return P.pending == 2 && P.step == STEP_NONE && ended(); }

static int take_live(const struct rr_header *header, const uint8_t *packed, size_t size, uint32_t frames) {
    if (!P.frames && !(P.frames = malloc(sizeof(struct rr_frame) * RR_MAX_FRAMES))) return 0;
    P.header = *header;
    P.header.level[sizeof P.header.level - 1] = 0;
    clean_text(P.header.player, sizeof P.header.player);
    clean_text(P.header.track, sizeof P.header.track);
    for (int i = 0; i < RR_RACERS; i++) {
        clean_text(P.header.racer[i].name, sizeof P.header.racer[i].name);
        clean_text(P.header.racer[i].driver, sizeof P.header.racer[i].driver);
        clean_text(P.header.racer[i].kart, sizeof P.header.racer[i].kart);
    }
    __atomic_store_n(&P.count, 0, __ATOMIC_RELEASE);
    const uint32_t n = unpack_more(packed, size, frames);
    if (n < 2) return 0;
    __atomic_store_n(&P.count, n, __ATOMIC_RELEASE);
    P.live = 1;
    __atomic_store_n(&P.live_ended, 0, __ATOMIC_RELEASE);
    return 1;
}

int runtime_replay_play_live(const struct rr_header *header, const uint8_t *packed, size_t size, uint32_t frames) {
    if (!header || memcmp(header->magic, RR_MAGIC, 8)) return 0;      /* (live: GO may come later, ghosts_later) */
    if (runtime_replay_can_chain()) {
        uint8_t *copy = malloc(size ? size : 1);
        if (!copy) return 0;
        memcpy(copy, packed, size);
        pthread_mutex_lock(&P.lock);
        free(NEXT.packed);
        NEXT.header = *header, NEXT.packed = copy, NEXT.size = size, NEXT.frames = frames;
        P.live_request = 2;
        pthread_mutex_unlock(&P.lock);
        return 1;
    }
    if (P.pending || P.step != STEP_NONE || !take_live(header, packed, size, frames)) return 0;
    pthread_mutex_lock(&P.lock);
    P.live_request = 1;
    pthread_mutex_unlock(&P.lock);
    return 1;
}

int runtime_replay_live_append(const struct rr_header *header, const uint8_t *packed, size_t size, uint32_t frames, int ended) {
    if (!P.live) return 0;
    if (packed && size) __atomic_store_n(&P.count, unpack_more(packed, size, frames), __ATOMIC_RELEASE);
    if (header) {
        pthread_mutex_lock(&P.lock);
        P.live_header = *header;
        P.live_header_new = 1;
        pthread_mutex_unlock(&P.lock);
    }
    if (ended) __atomic_store_n(&P.live_ended, 1, __ATOMIC_RELEASE);
    return 1;
}

uint32_t runtime_replay_live_count(void) { return P.live ? frames_now() : 0; }
