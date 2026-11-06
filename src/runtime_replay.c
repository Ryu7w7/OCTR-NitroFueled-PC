#define _GNU_SOURCE
/* Native race replays (spectator build): the recorder.
 *
 * Every frame of a race, every racer (player, online players, AI): where it is, how it moves, its inputs
 * and race state, 60 times a second; written when the level ends to <saves>/savedata/1/<title>/RaceReplay
 * as A<number>.crr (the last RR_KEEP_AUTO; a Time Trial personal best as K<number>.crr, kept).
 *
 * Ported from Erluinzo's Replay Mode 0.2.2 (github.com/Erluinzo/ctrnf-replay-mode, MIT, Copyright (c)
 * 2026 Onur): its recorder (rec.c, items.c) and file packing (files.c), with the same file format. Here it
 * runs as OCTR's own code: the game's functions are called through pointers into the image, and the game
 * jumps to it through small trampolines in the mod code area after its code segment (scripts/patches.py
 * code_extension). Battle modes and the adventure hub are not recorded. */
#include "runtime_replay.h"
#include "runtime_replay_internal.h"
#include "runtime.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define AT(object, offset, type) (*(type *)((char *)(object) + (offset)))
static inline int count(void *list) { return list ? AT(list, 0xc, int32_t) : 0; }
static inline void *item(void *list, int i) { return AT(list, 0x20, void **)[i]; }

/* ---- the game (USA CUSA13795 1.21 addresses; EU 1.21 functions sit 0x10 lower where noted) ---- */
static unsigned char *image;
#define G(offset) (image + (offset))
#define PTR(offset) (*(void **)G(offset))

typedef void ABI handle_resolve_fn(void **slot);
typedef void *ABI find_component_fn(void *entity, void *meta, int zero);
typedef void ABI entity_angles_fn(float *out, void *entity);
typedef float ABI maintained_fn(void *boost);
typedef void *ABI current_zone_fn(void *zones);
typedef int32_t ABI now_fn(void *timer);
typedef void *ABI meta_fn(void *component);
static handle_resolve_fn *game_handle_resolve;
static find_component_fn *game_find_component;
static entity_angles_fn *game_entity_angles;
static maintained_fn *game_maintained;
static current_zone_fn *game_current_zone;

enum {
    D_GAME = 0x2acde48, D_FRAME = 0x2ab4100, D_KART_MANAGER = 0x2ac57c0, D_KART_CONTROLLER_META = 0x2ac4580,
    D_RACER_MANAGER = 0x2ad4b70, D_CUSTOMIZATION = 0x2ac4ec8, D_BOOST_META = 0x2ac4018, D_POWER_UP_META = 0x2addd90,
    D_HORN_META = 0x2add660, D_DAMAGE_META = 0x2ac5018, D_RESPAWN_META = 0x2ac5948, D_ZONE_SYSTEM = 0x2ae3c18,
    D_RACE_SETTINGS = 0x2ad4e30, D_RACE_CLOCK_META = 0x2ad3ec8, D_WORLD_READY = 0x2a92868, D_WORLD_ID = 0x2a92888,
    D_WORLD_NO_ID = 0x2a922d0, D_ENTITY_TABLE = 0x2a92880, D_SYSTEM_DATA = 0x2ad9c38, D_GHOST_MANAGER = 0x2abc3d0,
    D_SPAWNED_META = 0x2ad70d8,
};
#define RR_DRIVERS 0x130
#define RR_KARTS 0x150

/* ---- state ---- */
#define RR_MADE 32
#define RR_CRATES 12
static struct {
    struct rr_header header;
    struct rr_frame *frames;
    void *race;
    uint8_t uses[RR_RACERS];
    uint8_t pressed[RR_RACERS];         /* the item button with nothing stored since the last sample */
    uint8_t launch[RR_RACERS];          /* the direction of each racer's last use + 1 (RS_LAUNCH_SHIFT), 0 not known */
    struct { void *object; int32_t type; } made[RR_MADE];
    int made_next;
    float time, next, end_at, end_local;
    int done, recording;
    uint16_t *seen;
    uint32_t pulse_buttons[RR_RACERS];
    uint16_t pulse_bits[RR_RACERS];
    struct { int slot; uint16_t generation; void *component; } crates[RR_CRATES];
    int crate_next;
} rr;

static void copy_text(char *out, const char *in, size_t size) {
    size_t i = 0;
    for (; in && in[i] && i + 1 < size; i++) out[i] = in[i];
    out[i] = 0;
}

static void *world(void) {
    const uint32_t id = AT(image, D_WORLD_ID, uint32_t);
    char *table = PTR(D_ENTITY_TABLE);
    if (!AT(image, D_WORLD_READY, uint8_t) || id == AT(image, D_WORLD_NO_ID, uint32_t) || !(id & 0x8000) || !table) return 0;
    const int i = id & 0x7fff;
    void *generations = AT(table, 0x28, void *);
    if (i >= count(generations) || AT(generations, 0x20, uint16_t *)[i] != (i | 0x8000)) return 0;
    return AT(AT(table, 0x20, void *), 8 * i, void *);
}

/* The object a handle field points to, or 0 (as the game reads them) */
static void *handle_object(void **slot) {
    for (int depth = 0; depth < 8; depth++) {
        int32_t *h = *slot;
        if (!h) return 0;
        if (*h & 0x10000000) {
            game_handle_resolve(slot);
            h = *slot;
            if (!h) return 0;
        }
        void *object = AT(h, 0x28, void *);
        if (!(*h & 0x20000000) || !object) return object;
        slot = (void **)((char *)object + 0x10);
    }
    return 0;
}

static float timer_seconds(void *timer) {
    if (!timer || !AT(timer, 0, void *) || !AT(timer, 0x14, uint8_t)) return 0.0f;
    const int32_t now = !AT(timer, 0x15, uint8_t) ? AT(*(void **)timer, 0xb0, now_fn *)(timer) : AT(timer, 0x10, int32_t);
    return (float)(now - AT(timer, 0xc, int32_t)) / 8192.0f;
}

static float race_clock(void) {
    void *w = world(), *meta = PTR(D_RACE_CLOCK_META);
    void *clock = w && meta ? game_find_component(w, meta, 0) : 0;
    return clock ? timer_seconds(AT(clock, 0x28, void *)) : 0.0f;
}

static void *kart_of(int i, int *player_id) {
    void *manager = PTR(D_KART_MANAGER);
    void *slot = manager ? AT(manager, 0x10 + 8 * i, void *) : 0;
    void *data = slot ? handle_object(&AT(slot, 0x18, void *)) : 0;
    if (player_id) *player_id = data ? AT(data, 0x28, int32_t) : -1;
    return slot ? handle_object(&AT(slot, 0x10, void *)) : 0;
}

static void *component(void *kart, int meta_at) {
    void *meta = PTR(meta_at);
    return meta ? game_find_component(kart, meta, 0) : 0;
}

static void *racer_of(void *kart) {
    char *manager = PTR(D_RACER_MANAGER);
    void *racers = manager ? AT(manager, 0x10, void *) : 0;
    for (int i = 0; i < count(racers); i++) {
        void *r = item(racers, i);
        if (r && handle_object(&AT(r, 0x20, void *)) == kart) return r;
    }
    return 0;
}

static int16_t angle(float radians) {
    const float v = radians * 10430.06f;
    return (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : v);
}
static uint16_t clamp16(float v) { return (uint16_t)(v <= 0.0f ? 0 : v >= 65535.0f ? 65535 : v); }

static void *running_boost(void *boosts) {
    void *list = AT(boosts, 0x28, void *), *active = 0;
    float speed = 0.0f;
    for (int i = 0; i < count(list); i++) {
        void *boost = item(list, i), *data = boost ? AT(boost, 0x10, void *) : 0;
        if (!data || AT(boost, 0x48, uint8_t)) continue;
        if (!active || AT(data, 0xc, float) >= speed) active = boost, speed = AT(data, 0xc, float);
    }
    return active;
}

static float reserves(void *boosts) {
    void *active = running_boost(boosts);
    const float length = active ? AT(AT(active, 0x10, void *), 0x1c, float) + AT(active, 0x28, float) + game_maintained(active) : 0.0f;
    const float left = active ? length - AT(active, 0x18, float) : 0.0f;
    return left > 0.0f ? left : 0.0f;
}

/* The type of a stored item (a handle): the one the factory made it with; else (a kart another PC
 * simulates) from its data, the type's entry of the power-up table */
int rr_item_type(void *handle) {
    static const uint8_t FIELD[12] = {0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x80, 0x70, 0x78};
    for (int i = 1; i <= RR_MADE; i++) {
        const int k = (rr.made_next - i + RR_MADE) % RR_MADE;
        if (rr.made[k].object == handle) return rr.made[k].type;
    }
    void *object = handle_object(&handle);
    char *system = PTR(D_SYSTEM_DATA);
    char *table = system ? AT(system, 0xf8, char *) : 0;
    for (int k = 0; object && table && k < 12; k++)
        if (AT(table, FIELD[k], void *) == AT(object, 0x10, void *))
            return k == 9 ? 9 + !!AT(object, 0x54, int32_t) : k < 9 ? k : k + 1;
    return -1;
}

static void sample(struct rr_sample *s, void *kart, int slot) {
    memset(s, 0, sizeof *s);
    s->item = -1;
    if (!kart) return;
    s->flags = RS_PRESENT;
    for (int k = 0; k < 3; k++) {
        s->position[k] = (int32_t)(AT(kart, 0x20 + 4 * k, float) * RR_POSITION);
        const float v = AT(kart, 0x60 + 4 * k, float) * RR_VELOCITY;
        s->velocity[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : v);
    }
    float a[3];
    game_entity_angles(a, kart);
    for (int k = 0; k < 3; k++) s->angles[k] = angle(a[k]);
    void *input = AT(kart, 0x1a0, void *);
    void *command = input ? AT(input, 0x10, void *) : 0;
    if (command) {
        s->buttons = (uint32_t)AT(command, 0x10, uint64_t) | rr.pulse_buttons[slot];
        s->steer = (int16_t)(AT(command, 0x68, float) * RR_STEER);
    }
    void *controller = component(kart, D_KART_CONTROLLER_META);
    if (controller) {
        void *snapshot = AT(controller, 0x268, void *);
        if (snapshot) s->ghost_bits = AT(snapshot, 0xc, uint16_t) | rr.pulse_bits[slot];
        const int drive = AT(controller, 0xe0, int32_t);
        s->drift_state = (uint8_t)drive;
        if (drive == 3 || drive == 4) s->flags |= RS_DRIFTING;
        if (AT(controller, 0x214, uint8_t)) s->flags |= RS_ON_GROUND;
        if (AT(controller, 0x255, uint8_t)) s->flags |= RS_WRONG_WAY;
        s->slide_timer = clamp16(timer_seconds(AT(controller, 0xf8, void *)) * 1000.0f);
        s->air_time = clamp16(AT(controller, 0x248, float) * 1000.0f);
    }
    void *boosts = component(kart, D_BOOST_META);
    if (boosts) {
        s->reserves = clamp16(reserves(boosts) * 100.0f);
        s->boost_speed = clamp16(AT(boosts, 0x38, float));
        void *list = AT(boosts, 0x28, void *);
        for (int i = 0; i < count(list); i++) {
            void *data = item(list, i) ? AT(item(list, i), 0x10, void *) : 0;
            if (!data) continue;
            s->flags |= RS_BOOSTING;
            if (AT(data, 0x3c, uint8_t)) s->flags |= RS_PAD_BOOST;
            if (AT(data, 0x3e, uint8_t)) s->flags |= RS_DRIFT_BOOST;
        }
    }
    void *items = component(kart, D_POWER_UP_META);
    if (items) {
        void *stored = handle_object(&AT(items, 0x30, void *));
        if (AT(items, 0x40, uint8_t)) s->flags |= RS_SHUFFLING;
        if (stored) {
            s->item = (int8_t)rr_item_type(AT(items, 0x30, void *));
            const int32_t charges = s->item == 9 || s->item == 10 ? AT(stored, 0x50, int32_t) : 1;
            s->item_count = (uint8_t)(charges > 0 && charges < 10 ? charges : 1);
        }
    }
    s->item_uses = rr.uses[slot];
    s->flags |= (uint16_t)((rr.launch[slot] & 3u) << RS_LAUNCH_SHIFT);
    if (rr.pressed[slot]) s->flags |= RS_PRESSED, rr.pressed[slot] = 0;
    void *horn = component(kart, D_HORN_META);
    if (horn && AT(horn, 0x30, uint8_t)) s->flags |= RS_HONK;
    void *damage = component(kart, D_DAMAGE_META);
    if (damage) s->damage = (uint8_t)(AT(damage, 0x40, int32_t) + 1);
    void *respawn = component(kart, D_RESPAWN_META);
    if (respawn) s->respawn = (uint8_t)AT(respawn, 0x5c, int32_t);
    void *racer = racer_of(kart);
    if (racer) {
        s->rank = (uint8_t)AT(racer, 0x58, int32_t);
        s->lap = (uint8_t)AT(racer, 0x4c, int32_t);
        s->wumpa = (uint8_t)AT(racer, 0x9c, int32_t);
        s->juiced = s->item >= 0 && s->wumpa > 9;
        if (AT(racer, 0x8c, uint8_t)) {
            s->flags |= RS_FINISHED;
            struct rr_racer *r = &rr.header.racer[slot];
            if (!r->finished) r->finished = s->rank, r->finish_time = (float)AT(racer, 0x6c, int32_t) / 8192.0f;
        }
    }
}

static void *game_data(int list, const char *name) {
    char *system = PTR(D_SYSTEM_DATA);
    void *entries = system && name[0] ? AT(system, list, void *) : 0;
    for (int i = 0; i < count(entries); i++) {
        const char *a = item(entries, i) ? AT(item(entries, i), 0x58, const char *) : 0;
        if (a && !strcmp(a, name)) return item(entries, i);
    }
    return 0;
}

static void record_outfit(struct rr_racer *r, void *outfit) {
    void *data = outfit ? game_data(RR_DRIVERS, r->driver) : 0;
    void *skins = data ? AT(data, 0x98, void *) : 0;
    for (int k = 0; k < count(skins); k++)
        if (item(skins, k) && AT(item(skins, k), 0x68, void *) == outfit) r->look[5] = (uint16_t)(k + 1);
}

static void identify(int slot, void *kart) {
    struct rr_racer *r = &rr.header.racer[slot];
    char *manager = PTR(D_RACER_MANAGER);
    void *racer = racer_of(kart);
    void *player = racer ? handle_object(&AT(racer, 0x10, void *)) : 0;
    r->player_id = racer ? (int8_t)AT(racer, 0x164, int32_t) : -1;
    char *custom = PTR(D_CUSTOMIZATION);
    if (custom) {
        const int64_t *look = (const int64_t *)(custom + 0x10 + slot * 0x88);
        for (int k = 0; k < 5; k++) r->look[k] = (uint16_t)look[k * 4];
        r->look_filled = 1;
    }
    if (player) {
        r->kind = (uint32_t)AT(player, 0x20, int32_t) <= 7 ? RR_LOCAL : RR_REMOTE;
        copy_text(r->name, AT(player, 0x68, const char *), sizeof r->name);
        if (r->kind == RR_LOCAL && !rr.header.player[0]) copy_text(rr.header.player, r->name, sizeof rr.header.player);
        copy_text(r->driver, AT(player, 0x88, const char *), sizeof r->driver);
        copy_text(r->kart, AT(player, 0x98, const char *), sizeof r->kart);
        record_outfit(r, handle_object(&AT(player, 0x90, void *)));
        return;
    }
    r->kind = RR_AI;
    void *infos = AT(manager, 0x28, void *);
    for (int i = 0; i < count(infos); i++) {
        void *n = item(infos, i);
        if (!n || AT(n, 0x38, void *) != kart) continue;
        copy_text(r->driver, AT(n, 0x18, const char *), sizeof r->driver);
        copy_text(r->kart, AT(n, 0x28, const char *), sizeof r->kart);
        void *persistent = AT(manager, 0x30, void *);
        for (int k = 0; k < count(persistent); k++)
            if (item(persistent, k) && AT(item(persistent, k), 0x58, int32_t) == AT(n, 0xc, int32_t))
                record_outfit(r, handle_object(&AT(item(persistent, k), 0x20, void *)));
    }
    char *ghost_manager = PTR(D_GHOST_MANAGER);
    void *ghosts = ghost_manager ? AT(ghost_manager, 0x40, void *) : 0;
    for (int i = 0; i < count(ghosts) && !r->driver[0]; i++) {
        void *g = item(ghosts, i);
        if (!g || handle_object(&AT(g, 0x10, void *)) != kart) continue;
        copy_text(r->driver, AT(g, 0x20, const char *), sizeof r->driver);
        copy_text(r->kart, AT(g, 0x18, const char *), sizeof r->kart);
        copy_text(r->name, "GHOST", sizeof r->name);
    }
}

/* Items of karts another PC simulates: each new spawned power-up entity is a use by its owner; TNT on a
 * kart's head and shields as flags (items.c of the mod). */
static void record_items(struct rr_frame *f) {
    char *table = PTR(D_ENTITY_TABLE);
    void *spawned_meta = PTR(D_SPAWNED_META);
    void *gens = table ? AT(table, 0x28, void *) : 0;
    void **entities = table ? AT(table, 0x20, void **) : 0;
    const int n = count(gens);
    if (!entities || !spawned_meta || n > 4096) return;
    if (!rr.seen) rr.seen = calloc(4096, sizeof *rr.seen);
    if (!rr.seen) return;
    uint16_t *generation = AT(gens, 0x20, uint16_t *);
    for (int i = 0; i < n; i++) {
        if (generation[i] == rr.seen[i]) continue;
        rr.seen[i] = generation[i];
        void *list = (generation[i] & 0x8000) && entities[i] && rr.header.frames > 1 ? AT(entities[i], 0x18, void *) : 0;
        for (int k = 0; k < count(list); k++) {
            void *c = item(list, k);
            void *meta = c ? (*(meta_fn ***)c)[0](c) : 0;
            if (!meta || AT(meta, 0x40, void *) != spawned_meta) continue;
            void *owner = handle_object(&AT(c, 0x28, void *));
            const int type = AT(c, 0x34, int32_t);
            if (type == 2 || type == 8) {
                rr.crates[rr.crate_next].slot = i, rr.crates[rr.crate_next].generation = generation[i];
                rr.crates[rr.crate_next].component = c;
                rr.crate_next = (rr.crate_next + 1) % RR_CRATES;
            }
            for (int r = 0; owner && !(AT(owner, 0x3b, uint8_t) & 1) && type >= 0 && type < 13 && r < RR_RACERS; r++)
                if (kart_of(r, 0) == owner) {
                    f[-1].racer[r].item = (int8_t)type;
                    f->racer[r].item_uses = ++rr.uses[r];
                }
        }
    }
    for (int c = 0; c < RR_CRATES; c++) {
        void *comp = rr.crates[c].component;
        if (!comp || generation[rr.crates[c].slot] != rr.crates[c].generation) continue;
        void *list = entities[rr.crates[c].slot] ? AT(entities[rr.crates[c].slot], 0x18, void *) : 0;
        int alive = 0;
        for (int k = 0; k < count(list); k++) alive |= item(list, k) == comp;
        if (!alive) {
            rr.crates[c].component = 0;
            continue;
        }
        const int shield = AT(comp, 0x34, int32_t) == 8;
        void *kart = shield ? handle_object(&AT(comp, 0x28, void *))
                   : AT(comp, 0xb0, int32_t) == 2 ? handle_object(&AT(comp, 0xa8, void *)) : 0;
        for (int r = 0; kart && r < RR_RACERS; r++)
            if (kart_of(r, 0) == kart) f->racer[r].flags |= shield ? RS_SHIELD : RS_TNT;
    }
}

static void date_of(int64_t t, uint8_t *date) {
    int64_t days = t / 86400, rest = t % 86400;
    date[3] = (uint8_t)(rest / 3600), date[4] = (uint8_t)(rest / 60 % 60), date[5] = (uint8_t)(rest % 60);
    days += 719468;
    const int64_t era = days / 146097, doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2);
    date[0] = (uint8_t)(year - 2000), date[1] = (uint8_t)month, date[2] = (uint8_t)day;
}

/* The race being recorded, for the live stream (runtime_spectate.c reads it on its own thread) */
static struct {
    pthread_mutex_t lock;
    uint32_t race_id;           /* counts up with every race recorded, 0 none yet */
    struct rr_header header;
    uint32_t frames;            /* frames that will not change any more (the newest one still can) */
    int ended;
} live = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void start(void *race) {
    memset(&rr.header, 0, sizeof rr.header);
    memcpy(rr.header.magic, RR_MAGIC, 8);
    rr.header.version = RR_VERSION;
    rr.header.header_size = sizeof rr.header;
    rr.header.frame_size = sizeof(struct rr_frame);
    char *game = PTR(D_GAME), *settings = PTR(D_RACE_SETTINGS);
    rr.header.mode = game ? AT(game, 0xd4c, int32_t) : -1;
    rr.header.go_time = -1.0f;
    rr.header.laps = settings ? AT(settings, 0xc, int32_t) : 0;
    rr.header.mirror = settings && AT(settings, 0x3c, uint8_t);
    rr.header.no_crates = rr.header.mode == 2 || runtime_speed_race_active();
    void *zones = PTR(D_ZONE_SYSTEM);
    void *zone = zones ? game_current_zone(zones) : 0;
    if (zone) copy_text(rr.header.level, AT(zone, 0x10, const char *), sizeof rr.header.level);
    if (zone) copy_text(rr.header.track, AT(zone, 0xa0, const char *), sizeof rr.header.track);
    date_of((int64_t)time(NULL), rr.header.date);
    rr.race = race;
    rr.time = rr.next = rr.end_at = rr.end_local = 0.0f;
    rr.done = 0;
    rr.recording = 1;
    memset(rr.uses, 0, sizeof rr.uses);
    memset(rr.launch, 0, sizeof rr.launch);
    memset(rr.pressed, 0, sizeof rr.pressed);
    pthread_mutex_lock(&live.lock);
    live.race_id++;
    live.header = rr.header;
    live.frames = 0;
    live.ended = 0;
    pthread_mutex_unlock(&live.lock);
}

/* ---- the file ---- */
#define REST_OFFSET 24

static uint8_t *put_varint(uint8_t *o, int32_t v) {
    uint32_t u = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
    while (u >= 0x80) *o++ = (uint8_t)(u | 0x80), u >>= 7;
    *o++ = (uint8_t)u;
    return o;
}

static int get_varint(const uint8_t **in, const uint8_t *end, int32_t *v) {
    uint32_t u = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (*in >= end) return 0;
        const uint8_t c = *(*in)++;
        u |= (uint32_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *v = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            return 1;
        }
    }
    return 0;
}

static uint8_t *put_bytes(uint8_t *o, const uint8_t *now, const uint8_t *before, int size) {
    int i = 0;
    while (i < size) {
        int zeros = 0;
        while (i + zeros < size && zeros < 128 && now[i + zeros] == before[i + zeros]) zeros++;
        if (zeros) {
            *o++ = (uint8_t)(0x7f + zeros);
            i += zeros;
            continue;
        }
        uint8_t *first = o++;
        int n = 0;
        while (i < size && n < 128 && now[i] != before[i]) *o++ = now[i] ^ before[i], i++, n++;
        *first = (uint8_t)(n - 1);
    }
    return o;
}

static int get_bytes(const uint8_t **in, const uint8_t *end, uint8_t *now, const uint8_t *before, int size) {
    for (int i = 0; i < size; i++) now[i] = before[i];
    int o = 0;
    while (o < size) {
        if (*in >= end) return 0;
        const int c = *(*in)++;
        if (c >= 0x80) {
            o += c - 0x7f;
            continue;
        }
        if (*in + c + 1 > end || o + c + 1 > size) return 0;
        for (int k = 0; k <= c; k++) now[o++] ^= *(*in)++;
    }
    return o == size;
}

uint8_t *rr_pack_frame(uint8_t *o, const struct rr_frame *f, const struct rr_frame *p1, const struct rr_frame *p2) {
    o = put_bytes(o, (const uint8_t *)f, (const uint8_t *)p1, 8);
    for (int r = 0; r < RR_RACERS; r++) {
        const struct rr_sample *s = &f->racer[r], *a = &p1->racer[r], *b = &p2->racer[r];
        for (int k = 0; k < 3; k++)
            o = put_varint(o, (int32_t)((uint32_t)s->position[k] - 2u * (uint32_t)a->position[k] + (uint32_t)b->position[k]));
        for (int k = 0; k < 3; k++) o = put_varint(o, (int16_t)(s->angles[k] - 2 * a->angles[k] + b->angles[k]));
        for (int k = 0; k < 3; k++) o = put_varint(o, (int16_t)(s->velocity[k] - a->velocity[k]));
        o = put_varint(o, (int16_t)(s->steer - a->steer));
        o = put_varint(o, (int16_t)(s->ghost_bits ^ a->ghost_bits));
        struct rr_sample rest = *s;
        rest.steer = a->steer;
        rest.ghost_bits = a->ghost_bits;
        o = put_bytes(o, (const uint8_t *)&rest + REST_OFFSET, (const uint8_t *)a + REST_OFFSET, (int)sizeof *s - REST_OFFSET);
    }
    return o;
}

int rr_unpack_frame(struct rr_frame *f, const struct rr_frame *p1, const struct rr_frame *p2,
                    const uint8_t **in, const uint8_t *end, int version) {
    if (!get_bytes(in, end, (uint8_t *)f, (const uint8_t *)p1, 8)) return 0;
    for (int r = 0; r < RR_RACERS; r++) {
        struct rr_sample *s = &f->racer[r];
        const struct rr_sample *a = &p1->racer[r], *b = &p2->racer[r];
        int32_t v;
        for (int k = 0; k < 3; k++) {
            if (!get_varint(in, end, &v)) return 0;
            s->position[k] = (int32_t)((uint32_t)v + 2u * (uint32_t)a->position[k] - (uint32_t)b->position[k]);
        }
        for (int k = 0; k < 3; k++) {
            if (!get_varint(in, end, &v)) return 0;
            s->angles[k] = (int16_t)(v + 2 * a->angles[k] - b->angles[k]);
        }
        for (int k = 0; k < 3 && version > 4; k++) {
            if (!get_varint(in, end, &v)) return 0;
            s->velocity[k] = (int16_t)(v + a->velocity[k]);
        }
        int32_t steer, bits;
        if (!get_varint(in, end, &steer) || !get_varint(in, end, &bits)) return 0;
        if (!get_bytes(in, end, (uint8_t *)s + REST_OFFSET, (const uint8_t *)a + REST_OFFSET, (int)sizeof *s - REST_OFFSET)) return 0;
        s->steer = (int16_t)(steer + a->steer);
        s->ghost_bits = (uint16_t)(bits ^ a->ghost_bits);
        const float dt = f->time - p1->time;
        for (int k = 0; k < 3 && version == 4; k++) {
            const float speed = dt > 0.0f ? (float)(s->position[k] - a->position[k]) / RR_POSITION / dt * RR_VELOCITY : 0.0f;
            s->velocity[k] = (int16_t)(speed > 32767.0f ? 32767 : speed < -32767.0f ? -32767 : speed);
        }
    }
    return 1;
}

static void race_dir(char *out, size_t size) {
    snprintf(out, size, "%s/savedata/1/%s/RaceReplay", runtime_file_progress_dir(), runtime_savedata_title());
}

static void make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer, sizeof buffer, "%s", path);
    for (char *p = buffer + 1; *p; ++p)
        if (*p == '/' || *p == '\\') {
            const char c = *p;
            *p = 0;
#ifdef _WIN32
            _mkdir(buffer);
#else
            mkdir(buffer, 0755);
#endif
            *p = c;
        }
#ifdef _WIN32
    _mkdir(buffer);
#else
    mkdir(buffer, 0755);
#endif
}

static int auto_number(const char *name) {
    if (strlen(name) != 10 || name[0] != 'A' || strcmp(name + 6, ".crr")) return -1;
    int n = 0;
    for (int i = 1; i < 6; i++) {
        if (name[i] < '0' || name[i] > '9') return -1;
        n = n * 10 + name[i] - '0';
    }
    return n;
}

/* A finished Time Trial's time (the local racer's), or 0 */
static float trial_time(const struct rr_header *h) {
    for (int i = 0; h->mode == 2 && i < RR_RACERS; i++)
        if (h->racer[i].kind == RR_LOCAL && h->racer[i].finished && h->racer[i].finish_time > 0.0f) return h->racer[i].finish_time;
    return 0.0f;
}

static void save(void) {
    char dir[700], path[800];
    race_dir(dir, sizeof dir);
    make_dirs(dir);
    int newest = 0, oldest = -1, total = 0;
    float best = trial_time(&rr.header);
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        const int n = auto_number(e->d_name);
        if (n >= 0) {
            total++;
            if (n > newest) newest = n;
            if (oldest < 0 || n < oldest) oldest = n;
        }
        /* a Time Trial personal best: faster than every recording of that track */
        const size_t len = strlen(e->d_name);
        if (best > 0.0f && len > 4 && !strcmp(e->d_name + len - 4, ".crr")) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            FILE *f = fopen(path, "rb");
            struct rr_header h;
            if (f && fread(&h, 1, sizeof h, f) == sizeof h && !memcmp(h.magic, RR_MAGIC, 8)) {
                h.level[sizeof h.level - 1] = 0;
                const float t = trial_time(&h);
                if (t > 0.0f && t <= best && !strcmp(h.level, rr.header.level)) best = 0.0f;
            }
            if (f) fclose(f);
        }
    }
    if (d) closedir(d);
    FILE *f = NULL;
    char name[16];
    for (int n = newest + 1, tries = 0; !f && tries < 100; n++, tries++) {
        snprintf(name, sizeof name, "%c%05d.crr", best > 0.0f ? 'K' : 'A', n % 100000);
        snprintf(path, sizeof path, "%s/%s", dir, name);
        struct stat st;
        if (stat(path, &st) == 0) continue;
        f = fopen(path, "wb");
    }
    if (!f) {
        printf("Replay: could not write in %s\n", dir);
        return;
    }
    enum { CHUNK = 65536 };
    uint8_t *out = malloc(CHUNK + 4 * sizeof(struct rr_frame));
    static const struct rr_frame ZERO;
    int ok = out && fwrite(&rr.header, 1, sizeof rr.header, f) == sizeof rr.header;
    int used = 0;
    rr.header.packed = 0;
    for (uint32_t i = 0; ok && i < rr.header.frames; i++) {
        used = (int)(rr_pack_frame(out + used, &rr.frames[i], i > 0 ? &rr.frames[i - 1] : &ZERO,
                                   i > 1 ? &rr.frames[i - 2] : &ZERO) - out);
        if (used >= CHUNK || i + 1 == rr.header.frames) {
            ok = fwrite(out, 1, (size_t)used, f) == (size_t)used;
            rr.header.packed += (uint32_t)used;
            used = 0;
        }
    }
    if (ok) ok = !fseek(f, 0, SEEK_SET) && fwrite(&rr.header, 1, sizeof rr.header, f) == sizeof rr.header;
    free(out);
    if (fclose(f)) ok = 0;
    if (!ok) {
        remove(path);
        printf("Replay: could not write %s\n", path);
        return;
    }
    printf("Replay: saved %s (%u frames, %u KB, %s)\n", name, rr.header.frames, (rr.header.packed + (unsigned)sizeof rr.header) / 1024,
           rr.header.track);
    for (int n = oldest, left = total + 1 - RR_KEEP_AUTO; best <= 0.0f && n >= 0 && n <= newest && left > 0; n++) {
        snprintf(path, sizeof path, "%s/A%05d.crr", dir, n);
        if (!remove(path)) left--;
    }
    fflush(stdout);
}

/* ---- shared with the player (runtime_replay_internal.h) ---- */
unsigned char *rr_game(size_t offset) { return G(offset); }
void *rr_ptr(size_t offset) { return PTR(offset); }
void *rr_handle_object(void **slot) { return handle_object(slot); }
void *rr_kart_of(int i, int *player_id) { return kart_of(i, player_id); }
void *rr_component(void *kart, int meta_at) { return component(kart, meta_at); }
void *rr_find_component(void *entity, void *meta) { return game_find_component(entity, meta, 0); }
void *rr_running_boost(void *boosts) { return running_boost(boosts); }
float rr_boost_length(void *boost) {
    return AT(AT(boost, 0x10, void *), 0x1c, float) + AT(boost, 0x28, float) + game_maintained(boost);
}
void *rr_world(void) { return world(); }
float rr_race_clock(void) { return race_clock(); }
void *rr_game_data(int list, const char *name) { return game_data(list, name); }
void rr_race_dir(char *out, size_t size) { race_dir(out, size); }

/* The world entity's id: another level's world has another one */
uint32_t rr_world_id(void) { return AT(image, D_WORLD_ID, uint32_t); }

int runtime_replay_local_finished(void) { return rr.recording && rr.end_local != 0.0f; }

/* The level loaded now (its zone's name), 0 none */
const char *rr_current_level(void) {
    void *zones = PTR(D_ZONE_SYSTEM);
    void *zone = zones && game_current_zone ? game_current_zone(zones) : 0;
    return zone ? AT(zone, 0x10, const char *) : 0;
}

/* ---- hooks: the game calls these on its own threads ---- */
void ABI runtime_replay_level_end(void) {
    runtime_replay_play_level_end();
    if (!rr.recording) return;
    rr.recording = 0;
    rr.race = 0;
    pthread_mutex_lock(&live.lock);
    live.header = rr.header;
    live.frames = rr.header.frames;
    live.ended = 1;
    pthread_mutex_unlock(&live.lock);
    if (rr.header.frames <= 60 || rr.header.go_time < 0.0f) return;
    save();
}

void ABI runtime_replay_race_frame(void *race) {
    if (!race) return;
    if (runtime_replay_playing()) {             /* a replay is shown: nothing is recorded */
        runtime_replay_play_frame();
        return;
    }
    char *game = PTR(D_GAME), *frame = PTR(D_FRAME);
    const int mode = game ? AT(game, 0xd4c, int32_t) : -1;
    if ((mode >= 9 && mode <= 13) || mode == 7 || mode <= 0) return;   /* battle modes, the adventure hub */
    if (!rr.frames && !(rr.frames = malloc(sizeof(struct rr_frame) * RR_MAX_FRAMES))) return;
    if (race != rr.race) {
        runtime_replay_level_end();
        start(race);
    }
    const float dt = frame && !AT(frame, 0x28, uint8_t) ? AT(frame, 0x24, float) : 0.0f;
    if (dt <= 0.0f || rr.header.frames >= RR_MAX_FRAMES || rr.done) return;
    rr.time += dt;
    for (int i = 0; i < RR_RACERS; i++) {
        void *kart = kart_of(i, 0), *input = kart ? AT(kart, 0x1a0, void *) : 0, *command = input ? AT(input, 0x10, void *) : 0;
        void *controller = kart ? component(kart, D_KART_CONTROLLER_META) : 0;
        void *snapshot = controller ? AT(controller, 0x268, void *) : 0;
        if (command) rr.pulse_buttons[i] |= (uint32_t)AT(command, 0x10, uint64_t);
        if (snapshot) rr.pulse_bits[i] |= AT(snapshot, 0xc, uint16_t) & 0xff00;
    }
    if (rr.time < rr.next) return;
    rr.next += 1.0f / RR_RATE;
    if (rr.next < rr.time) rr.next = rr.time;
    struct rr_frame *f = &rr.frames[rr.header.frames++];
    f->time = rr.time;
    f->race_time = race_clock();
    if (f->race_time > 0.0f && rr.header.go_time < 0.0f) rr.header.go_time = rr.time - f->race_time;
    int racers = 0, finished = 0, local_finished = 0;
    for (int i = 0; i < RR_RACERS; i++) {
        void *kart = kart_of(i, 0);
        /* (again twice a second while a player has no name yet: online it can come after the racer) */
        const struct rr_racer *known = &rr.header.racer[i];
        const int unnamed = known->kind && known->kind != RR_AI && !known->name[0] && rr.header.frames % 30 == 0;
        if (kart && (!known->kind || !known->driver[0] || unnamed) && PTR(D_RACER_MANAGER)) identify(i, kart);
        sample(&f->racer[i], kart, i);
        racers += !!kart;
        finished += !!(f->racer[i].flags & RS_FINISHED);
        local_finished |= rr.header.racer[i].kind == RR_LOCAL && (f->racer[i].flags & RS_FINISHED);
    }
    record_items(f);
    memset(rr.pulse_buttons, 0, sizeof rr.pulse_buttons);
    memset(rr.pulse_bits, 0, sizeof rr.pulse_bits);
    rr.header.duration = rr.time;
    if (!rr.end_at && racers && finished == racers) rr.end_at = rr.time + 3.0f;
    if (!rr.end_local && local_finished) rr.end_local = rr.time + 60.0f;
    if ((rr.end_at && rr.time >= rr.end_at) || (rr.end_local && rr.time >= rr.end_local)) rr.done = 1;
    pthread_mutex_lock(&live.lock);
    live.header = rr.header;
    live.frames = rr.done ? rr.header.frames : rr.header.frames - 1;
    live.ended = rr.done;
    pthread_mutex_unlock(&live.lock);
}

/* ---- the live stream (runtime_replay.h) ---- */
int runtime_replay_live_state(uint32_t *race_id, struct rr_header *header, uint32_t *frames, int *ended) {
    pthread_mutex_lock(&live.lock);
    *race_id = live.race_id;
    if (header) *header = live.header;
    *frames = live.frames;
    *ended = live.ended;
    pthread_mutex_unlock(&live.lock);
    return *race_id != 0;
}

size_t runtime_replay_live_pack(uint32_t race_id, uint32_t from, uint32_t to, uint8_t *out, size_t cap) {
    static const struct rr_frame ZERO;
    size_t used = 0;
    pthread_mutex_lock(&live.lock);
    if (race_id == live.race_id && rr.frames && to <= live.frames)
        for (uint32_t i = from; i < to && used + 2 * sizeof(struct rr_frame) < cap; i++)
            used = (size_t)(rr_pack_frame(out + used, &rr.frames[i], i > 0 ? &rr.frames[i - 1] : &ZERO,
                                          i > 1 ? &rr.frames[i - 2] : &ZERO) - out);
    pthread_mutex_unlock(&live.lock);
    return used;
}

/* A kart uses its item (counted with an item stored). 0: a replay refuses it (only the recorded uses are made,
 * runtime_replay_play.c), the game's function returns at once. */
int ABI runtime_replay_item_gate(void);
int ABI runtime_replay_item_used(void *controller) {
    if (!runtime_replay_item_gate()) return 0;
    if (!rr.recording || !controller) return 1;
    const int stored = handle_object(&AT(controller, 0x30, void *)) != 0;
    for (int i = 0; i < RR_RACERS; i++) {
        void *kart = kart_of(i, 0);
        if (kart && component(kart, D_POWER_UP_META) == controller) {
            if (stored) rr.uses[i]++;
            else rr.pressed[i] = 1;
            const int32_t direction = AT(controller, 0x28, int32_t);   /* _launchDirection, set by the use input */
            rr.launch[i] = direction >= 0 && direction <= 2 ? (uint8_t)(direction + 1) : 0;
            return 1;
        }
    }
    return 1;
}

/* The item factory made `object` of `type` */
void ABI runtime_replay_item_made(void *object, int type) {
    if (!object) return;
    rr.made[rr.made_next].object = object;
    rr.made[rr.made_next].type = type;
    rr.made_next = (rr.made_next + 1) % RR_MADE;
}

/* Stubs the trampolines jump to. Entry hooks keep every register a C call may change (the vector ones
 * too), call the handler with the game's rdi, then run the 6 bytes they replaced ("push rbp; mov rbp, rsp;
 * push r15") and go on in the game's function. The factory hook calls the factory with the caller's
 * arguments (one on the stack), then tells the handler the new item. */
void *runtime_replay_cont_race_frame, *runtime_replay_cont_level_exit, *runtime_replay_cont_item_used,
     *runtime_replay_item_factory, *runtime_replay_cont_level_start, *runtime_replay_native_pad_read,
     *runtime_replay_ai_body, *runtime_replay_ai_no_entity, *runtime_replay_cont_item_cancel,
     *runtime_replay_cont_item_respawn, *runtime_replay_cont_item_finish, *runtime_replay_cont_item_update,
     *runtime_replay_cont_item_crate, *runtime_replay_cont_item_wumpa, *runtime_replay_cont_hud_lap,
     *runtime_replay_cont_crate, *runtime_replay_cont_hero, *runtime_replay_cont_damage, *runtime_replay_cont_power_event,
     *runtime_replay_stack_guard;
int ABI runtime_replay_on(void), runtime_replay_crate(void), runtime_replay_hud_lap(void);   /* the player */
int ABI runtime_replay_hero(void *entities, int index, char **hero, void **outfit);
int ABI runtime_replay_take_damage(void *damage, int type);        /* runtime_replay_play.c */
int ABI runtime_replay_power_event(void *controller, void *event);
extern char *runtime_replay_hero_name;     /* runtime_replay_play.c: what changeHeroAndOutfit gets instead */
extern void *runtime_replay_hero_outfit;
void ABI runtime_replay_tick(void);         /* runtime_replay_play.c */

#define RR_SAVE                                                                                       \
    "push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n push %r8\n push %r9\n push %r10\n"   \
    "push %r11\n push %rbx\n mov %rsp, %rbx\n and $-16, %rsp\n sub $256, %rsp\n"                     \
    "movdqu %xmm0, 0(%rsp)\n movdqu %xmm1, 16(%rsp)\n movdqu %xmm2, 32(%rsp)\n movdqu %xmm3, 48(%rsp)\n" \
    "movdqu %xmm4, 64(%rsp)\n movdqu %xmm5, 80(%rsp)\n movdqu %xmm6, 96(%rsp)\n movdqu %xmm7, 112(%rsp)\n" \
    "movdqu %xmm8, 128(%rsp)\n movdqu %xmm9, 144(%rsp)\n movdqu %xmm10, 160(%rsp)\n movdqu %xmm11, 176(%rsp)\n" \
    "movdqu %xmm12, 192(%rsp)\n movdqu %xmm13, 208(%rsp)\n movdqu %xmm14, 224(%rsp)\n movdqu %xmm15, 240(%rsp)\n"
#define RR_RESTORE                                                                                    \
    "movdqu 0(%rsp), %xmm0\n movdqu 16(%rsp), %xmm1\n movdqu 32(%rsp), %xmm2\n movdqu 48(%rsp), %xmm3\n" \
    "movdqu 64(%rsp), %xmm4\n movdqu 80(%rsp), %xmm5\n movdqu 96(%rsp), %xmm6\n movdqu 112(%rsp), %xmm7\n" \
    "movdqu 128(%rsp), %xmm8\n movdqu 144(%rsp), %xmm9\n movdqu 160(%rsp), %xmm10\n movdqu 176(%rsp), %xmm11\n" \
    "movdqu 192(%rsp), %xmm12\n movdqu 208(%rsp), %xmm13\n movdqu 224(%rsp), %xmm14\n movdqu 240(%rsp), %xmm15\n" \
    "mov %rbx, %rsp\n pop %rbx\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n pop %rdi\n pop %rsi\n pop %rdx\n" \
    "pop %rcx\n pop %rax\n"
#define RR_GATE_STUB(stub, handler, cont)                                                             \
    ".globl " #stub "\n" #stub ":\n" RR_SAVE "call " #handler "\n test %eax, %eax\n" RR_RESTORE      \
    "jz 1f\n push %rbp\n mov %rsp, %rbp\n push %r15\n jmp *" #cont "(%rip)\n 1: ret\n"
#define RR_ENTRY_STUB(stub, handler, cont)                                                            \
    ".globl " #stub "\n" #stub ":\n" RR_SAVE "call " #handler "\n" RR_RESTORE                        \
    "push %rbp\n mov %rsp, %rbp\n push %r15\n jmp *" #cont "(%rip)\n"

__asm__(".text\n"
        RR_ENTRY_STUB(runtime_replay_stub_race_frame, runtime_replay_race_frame, runtime_replay_cont_race_frame)
        RR_ENTRY_STUB(runtime_replay_stub_level_exit, runtime_replay_level_end, runtime_replay_cont_level_exit)
        RR_GATE_STUB(runtime_replay_stub_item_used, runtime_replay_item_used, runtime_replay_cont_item_used)
        RR_ENTRY_STUB(runtime_replay_stub_level_start, runtime_replay_level_start, runtime_replay_cont_level_start)
        /* replaces "call pad_read" (every frame): the game's call, then the player's tick */
        ".globl runtime_replay_stub_pad_read\n"
        "runtime_replay_stub_pad_read:\n"
        "push %rdi\n call *runtime_replay_native_pad_read(%rip)\n pop %rdi\n push %rax\n"
        RR_SAVE "call runtime_replay_tick\n" RR_RESTORE "pop %rax\n ret\n"
        /* replaces "test rax, rax; je +6" in an AI kart's input delegate (rdi = its CKartAIComponent): in a
         * replay (handler 0) the function returns at once, the kart gets the recorded inputs */
        ".globl runtime_replay_stub_ai_input\n"
        "runtime_replay_stub_ai_input:\n"
        RR_SAVE "call runtime_replay_ai_input\n test %eax, %eax\n" RR_RESTORE
        "jz 1f\n test %rax, %rax\n jz 2f\n jmp *runtime_replay_ai_body(%rip)\n"
        "2: jmp *runtime_replay_ai_no_entity(%rip)\n"
        "1: pop %rbx\n pop %r14\n pop %rbp\n ret\n"
        /* Gates in a replay (handler 0: the game's function returns at once): a kart through an item crate
         * (items come from the recording), the HUD's lap (ours) */
        RR_GATE_STUB(runtime_replay_stub_crate, runtime_replay_crate, runtime_replay_cont_crate)
        RR_GATE_STUB(runtime_replay_stub_hud_lap, runtime_replay_hud_lap, runtime_replay_cont_hud_lap)
        /* changeHeroAndOutfit(entities, index, const string &hero, const handle &outfit, ...): in a replay the
         * player's kart gets the recorded racer's (handler 1: the saved rdx and rcx, at rbx + 56 and + 64,
         * become our string and handle; the game copies both with their reference counts) */
        /* Replaces "mov rax, [stack guard]" at 0xe330f1 (after the 17 bytes the online mode wraps): a kart takes damage
         * (rdi its damage component, esi the type). Handler 0 (a replay, not a recorded hit): the function returns
         * 0, "no damage", undoing its prologue (push rbp, r15, r14, r13, r12, rbx; sub rsp, 0x48). */
        ".globl runtime_replay_stub_damage\n runtime_replay_stub_damage:\n"
        RR_SAVE "call runtime_replay_take_damage\n test %eax, %eax\n" RR_RESTORE
        "jz 1f\n mov runtime_replay_stack_guard(%rip), %rax\n mov (%rax), %rax\n jmp *runtime_replay_cont_damage(%rip)\n"
        "1: add $0x48, %rsp\n pop %rbx\n pop %r12\n pop %r13\n pop %r14\n pop %r15\n pop %rbp\n xor %eax, %eax\n ret\n"
        /* 0xe368a0(controller, event): a kart's item events (shuffle, item, charges, show, hide) to its own HUD.
         * Handler 0: a replay sent it where it goes (the watched racer's to the HUD), the game's function returns. */
        ".globl runtime_replay_stub_power_event\n runtime_replay_stub_power_event:\n"
        RR_SAVE "call runtime_replay_power_event\n test %eax, %eax\n" RR_RESTORE
        "jz 1f\n push %rbp\n mov %rsp, %rbp\n push %r14\n jmp *runtime_replay_cont_power_event(%rip)\n 1: ret\n"
        ".globl runtime_replay_stub_hero\n runtime_replay_stub_hero:\n"
        RR_SAVE "call runtime_replay_hero\n test %eax, %eax\n jz 1f\n"
        "lea runtime_replay_hero_name(%rip), %rax\n mov %rax, 56(%rbx)\n"
        "lea runtime_replay_hero_outfit(%rip), %rax\n mov %rax, 64(%rbx)\n"
        "1:\n" RR_RESTORE "push %rbp\n mov %rsp, %rbp\n push %r15\n jmp *runtime_replay_cont_hero(%rip)\n"
        /* A rewind clears the items: their components stay on the karts' message lists. An old item with no
         * entity left (+0x18) ends or updates as nothing; what items do on a respawn, finish, crate or wumpa
         * is skipped in a replay (it would run on freed memory). */
        ".globl runtime_replay_stub_item_cancel\n runtime_replay_stub_item_cancel:\n"
        "cmpq $0, 0x18(%rdi)\n je 1f\n push %rbp\n mov %rsp, %rbp\n push %r15\n jmp *runtime_replay_cont_item_cancel(%rip)\n 1: ret\n"
        ".globl runtime_replay_stub_item_update\n runtime_replay_stub_item_update:\n"
        "cmpq $0, 0x18(%rdi)\n je 1f\n push %rbp\n mov %rsp, %rbp\n push %r15\n jmp *runtime_replay_cont_item_update(%rip)\n 1: ret\n"
        "runtime_replay_replay_on:\n" RR_SAVE "call runtime_replay_on\n test %eax, %eax\n" RR_RESTORE "ret\n"
        ".globl runtime_replay_stub_item_respawn\n runtime_replay_stub_item_respawn:\n"
        "call runtime_replay_replay_on\n jnz 1f\n mov 0xd0(%rdi), %eax\n jmp *runtime_replay_cont_item_respawn(%rip)\n 1: ret\n"
        ".globl runtime_replay_stub_item_finish\n runtime_replay_stub_item_finish:\n"
        "call runtime_replay_replay_on\n jnz 1f\n mov 0xd0(%rdi), %eax\n jmp *runtime_replay_cont_item_finish(%rip)\n 1: ret\n"
        ".globl runtime_replay_stub_item_crate\n runtime_replay_stub_item_crate:\n"
        "call runtime_replay_replay_on\n jnz 1f\n push %rbp\n mov %rsp, %rbp\n push %r15\n jmp *runtime_replay_cont_item_crate(%rip)\n 1: ret\n"
        ".globl runtime_replay_stub_item_wumpa\n runtime_replay_stub_item_wumpa:\n"
        "call runtime_replay_replay_on\n jnz 1f\n push %rbp\n mov %rsp, %rbp\n push %r15\n jmp *runtime_replay_cont_item_wumpa(%rip)\n 1: ret\n"
        ".globl runtime_replay_stub_item_made\n"
        "runtime_replay_stub_item_made:\n"
        "push %rbx\n push %r12\n push %r13\n mov %rdi, %rbx\n mov %ecx, %r12d\n sub $8, %rsp\n"
        "pushq 40(%rsp)\n call *runtime_replay_item_factory(%rip)\n add $16, %rsp\n mov %rax, %r13\n"
        "mov (%rbx), %rdi\n mov %r12d, %esi\n call runtime_replay_item_made\n mov %r13, %rax\n"
        "pop %r13\n pop %r12\n pop %rbx\n ret\n");
extern void runtime_replay_stub_race_frame(void), runtime_replay_stub_level_exit(void),
    runtime_replay_stub_item_used(void), runtime_replay_stub_item_made(void), runtime_replay_stub_level_start(void),
    runtime_replay_stub_pad_read(void), runtime_replay_stub_ai_input(void), runtime_replay_stub_crate(void),
    runtime_replay_stub_hud_lap(void), runtime_replay_stub_item_cancel(void), runtime_replay_stub_item_update(void),
    runtime_replay_stub_item_respawn(void), runtime_replay_stub_item_finish(void), runtime_replay_stub_item_crate(void),
    runtime_replay_stub_item_wumpa(void), runtime_replay_stub_hero(void), runtime_replay_stub_damage(void),
    runtime_replay_stub_power_event(void);

static int writable(void *address, size_t length) {
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &old) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *first = (void *)((uintptr_t)address & ~(uintptr_t)(page - 1));
    return !mprotect(first, (size_t)((unsigned char *)address + length - (unsigned char *)first),
                     PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

/* Trampolines in the mod code area (within 2 GB of every call site): 16 bytes each, "jmp [rip]" + address. */
#define TRAMPOLINES 0x23f0000
static unsigned char *next_trampoline;
static unsigned char *trampoline(void (*stub)(void)) {
    unsigned char *t = next_trampoline;
    if (!writable(t, 16)) return NULL;
    memcpy(t, "\xff\x25\0\0\0\0", 6);
    memcpy(t + 6, &stub, 8);
    next_trampoline += 16;
    return t;
}

static int rel32(unsigned char *at, unsigned char opcode, const unsigned char *target) {
    const int64_t d = target - (at + 5);
    if (d < INT32_MIN || d > INT32_MAX || !writable(at, 5)) return 0;
    const int32_t v = (int32_t)d;
    at[0] = opcode;
    memcpy(at + 1, &v, 4);
    return 1;
}

static const unsigned char PROLOGUE[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

/* The function at USA offset `usa` (EU: 0x10 lower when its prologue is there instead). */
static unsigned char *function_at(size_t usa, int eu) {
    if (!memcmp(G(usa), PROLOGUE, 6)) return G(usa);
    if (eu && !memcmp(G(usa - 0x10), PROLOGUE, 6)) return G(usa - 0x10);
    return NULL;
}

int rr_redirect_call(unsigned char *site, const unsigned char *expected, void (*target)(void)) {
    int32_t d;
    memcpy(&d, site + 1, 4);
    if (site[0] != 0xe8 || site + 5 + d != expected) return 0;
    unsigned char *t = trampoline(target);
    return t && rel32(site, 0xe8, t);
}

static int hook_entry(size_t usa, void (*stub)(void), void **cont) {
    unsigned char *at = function_at(usa, 1), *t = at ? trampoline(stub) : NULL;
    if (!t) return 0;
    *cont = at + 6;
    return rel32(at, 0xe9, t);
}

/* Replaces 6 known bytes at `at` (the same in both editions) by a jump to `stub`; it goes on at at + 6. */
static int hook_bytes(size_t at, const unsigned char *expected, void (*stub)(void), void **cont) {
    unsigned char *p = G(at), *t;
    if (memcmp(p, expected, 6) || !(t = trampoline(stub))) return 0;
    *cont = p + 6;
    return rel32(p, 0xe9, t);
}

int runtime_replay_start(unsigned char *base, size_t size) {
    const char *off = getenv("BB_REPLAY");
    if (off && *off == '0') return 0;
    image = base;
    if (size < 0x2bee66d) return 0;
    /* the mod code area must be mapped as code (boot.bin from this build's prepare.py) */
    next_trampoline = G(TRAMPOLINES);
    game_find_component = (find_component_fn *)G(0x3dbf90);
    game_entity_angles = (entity_angles_fn *)G(0x3dcfb0);
    game_maintained = (maintained_fn *)G(0xe2f050);
    unsigned char *resolve = !memcmp(G(0x149fca0), "\x55\x48\x89\xe5", 4) ? G(0x149fca0) : G(0x149fc90);
    unsigned char *zone = !memcmp(G(0x102e040), "\x55\x48\x89\xe5", 4) ? G(0x102e040) : G(0x102e030);
    game_handle_resolve = (handle_resolve_fn *)resolve;
    game_current_zone = (current_zone_fn *)zone;
    unsigned char *factory = function_at(0xe36a00, 1);
    if (!factory) return 0;
    const int eu = memcmp(G(0xea2d90), PROLOGUE, 6) != 0;  /* EU 1.21: most functions 0x10 lower */
    runtime_replay_item_factory = factory;
    int hooks = hook_entry(0xea2d90, runtime_replay_stub_race_frame, &runtime_replay_cont_race_frame) &&
                hook_entry(0xf9b450, runtime_replay_stub_level_exit, &runtime_replay_cont_level_exit) &&
                hook_entry(0xf0b070, runtime_replay_stub_item_used, &runtime_replay_cont_item_used);
    if (!hooks) {
        puts("Replay: the game's code is not the expected one: no recording");
        return 0;
    }
    /* the factory's callers (USA; EU a few sit 0x10 lower): each call must reach the factory */
    static const size_t SITES[] = {0xe367a7, 0xe37ec6, 0xf2363c, 0x1042b2c};
    unsigned char *made = trampoline(runtime_replay_stub_item_made);
    int sites = 0;
    for (size_t i = 0; made && i < sizeof SITES / sizeof *SITES; i++)
        for (size_t shift = 0; shift <= 0x10; shift += 0x10) {
            unsigned char *at = G(SITES[i] - shift);
            int32_t d;
            memcpy(&d, at + 1, 4);
            if (at[0] == 0xe8 && at + 5 + d == factory) {
                sites += rel32(at, 0xe8, made);
                break;
            }
        }
    /* the player: its functions and call sites, the ghost manager's level start, an AI kart's input, the pad read */
    static const unsigned char AI_INPUT[5] = {0x48, 0x85, 0xc0, 0x74, 0x06};   /* test rax, rax; je +6 */
    unsigned char *ai_site = G(0xdcb641), *pad_site = G(0x3d4954), *pad_fn = G(0x3d4dd0);
    int32_t pad_d;
    memcpy(&pad_d, pad_site + 1, 4);
    int player = runtime_replay_play_install(eu) && !memcmp(ai_site, AI_INPUT, 5) && pad_site[0] == 0xe8 &&
                 pad_site + 5 + pad_d == pad_fn &&
                 hook_entry(0xf9b250, runtime_replay_stub_level_start, &runtime_replay_cont_level_start);
    unsigned char *ai_t = player ? trampoline(runtime_replay_stub_ai_input) : NULL;
    if (ai_t) {
        runtime_replay_ai_body = ai_site + 5;
        runtime_replay_ai_no_entity = ai_site + 11;
        runtime_replay_native_pad_read = pad_fn;
        player = rel32(ai_site, 0xe9, ai_t) && rr_redirect_call(pad_site, pad_fn, runtime_replay_stub_pad_read);
    }
    /* the player's gates and the item guards for rewinds */
    static const unsigned char ITEM_STATE[6] = {0x8b, 0x87, 0xd0, 0x00, 0x00, 0x00};   /* mov eax, [rdi + 0xd0] */
    if (player && ai_t)
        player = hook_bytes(0xe36230, PROLOGUE, runtime_replay_stub_crate, &runtime_replay_cont_crate) &&
                 hook_bytes(0xe63f20, PROLOGUE, runtime_replay_stub_hud_lap, &runtime_replay_cont_hud_lap) &&
                 hook_bytes(0xe35190, PROLOGUE, runtime_replay_stub_item_cancel, &runtime_replay_cont_item_cancel) &&
                 hook_bytes(0xe38b00, PROLOGUE, runtime_replay_stub_item_update, &runtime_replay_cont_item_update) &&
                 hook_bytes(0xe35990, ITEM_STATE, runtime_replay_stub_item_respawn, &runtime_replay_cont_item_respawn) &&
                 hook_bytes(0xe359b0, ITEM_STATE, runtime_replay_stub_item_finish, &runtime_replay_cont_item_finish) &&
                 hook_bytes(0xe359e0, PROLOGUE, runtime_replay_stub_item_crate, &runtime_replay_cont_item_crate) &&
                 hook_bytes(0xe35b80, PROLOGUE, runtime_replay_stub_item_wumpa, &runtime_replay_cont_item_wumpa);
    /* the player's kart: changeHeroAndOutfit (its callers, the map load among them, read the save's choice) */
    if (player && ai_t && !hook_entry(0xbd5b90, runtime_replay_stub_hero, &runtime_replay_cont_hero))
        printf("Native Replay: changeHeroAndOutfit not found: the player's kart keeps the viewer's driver\n");
    /* the items: the recorded hits only, and the watched racer's item events to the HUD */
    static const unsigned char GUARD_LOAD[7] = {0x48, 0x8b, 0x05, 0xa0, 0x3f, 0x9d, 0x01};   /* mov rax, [0x2807098] */
    static const unsigned char EVENT_PROLOGUE[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};
    unsigned char *damage_at = G(0xe330f1), *event_at = G(0xe368a0), *t;
    int items = 0;
    if (player && ai_t && !memcmp(damage_at, GUARD_LOAD, 7) && !memcmp(event_at, EVENT_PROLOGUE, 6)) {
        runtime_replay_stack_guard = G(0x2807098);
        runtime_replay_cont_damage = damage_at + 7;
        runtime_replay_cont_power_event = event_at + 6;
        items = (t = trampoline(runtime_replay_stub_damage)) && rel32(damage_at, 0xe9, t) &&
                (t = trampoline(runtime_replay_stub_power_event)) && rel32(event_at, 0xe9, t);
    }
    if (player && ai_t && !items) printf("Native Replay: the item hooks are not there: items in replays are the game's own\n");
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    printf("Native Replay: races are recorded (%d of 4 item sites)%s (%s)\n", sites,
           player && ai_t ? ", replays play from the menu's Replays tab" : ", the player could not be installed", eu ? "EU" : "USA");
    return 1;
}

/* ---- the menu's list (runtime_replay_list.h) ---- */
#include "runtime_replay_list.h"

static long long seconds_of(const uint8_t *date) {
    const long long month = date[1], year = 2000 + date[0] - (month <= 2), era = year / 400, yoe = year - era * 400;
    const long long doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + date[2] - 1;
    const long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + date[3] * 3600 + date[4] * 60 + date[5];
}

static int newer_first(const void *a, const void *b) {
    const long long x = ((const RrListEntry *)a)->sort, y = ((const RrListEntry *)b)->sort;
    return x < y ? 1 : x > y ? -1 : 0;
}

int runtime_replay_list(RrListEntry *out, int max) {
    char dir[700], path[800];
    race_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    int n = 0;
    RrListEntry *all = d ? calloc(RR_LIST_MAX * 2, sizeof *all) : NULL;
    for (struct dirent *e; all && n < RR_LIST_MAX * 2 && (e = readdir(d));) {
        const size_t len = strlen(e->d_name);
        if (len < 5 || len >= sizeof all->file || strcmp(e->d_name + len - 4, ".crr")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        struct rr_header h;
        const int ok = f && fread(&h, 1, sizeof h, f) == sizeof h && !memcmp(h.magic, RR_MAGIC, 8);
        if (f) fclose(f);
        if (!ok) continue;
        RrListEntry *x = &all[n++];
        snprintf(x->file, sizeof x->file, "%s", e->d_name);
        h.track[sizeof h.track - 1] = h.player[sizeof h.player - 1] = 0;
        copy_text(x->track, h.track[0] ? h.track : h.level, sizeof x->track);
        copy_text(x->player, h.player, sizeof x->player);
        snprintf(x->when, sizeof x->when, "20%02u-%02u-%02u %02u:%02u", h.date[0], h.date[1], h.date[2], h.date[3], h.date[4]);
        for (int r = 0; r < RR_RACERS; r++) {
            x->racers += h.racer[r].kind != RR_EMPTY;
            if (h.racer[r].kind == RR_LOCAL && h.player[0] && !strncmp(h.racer[r].name, h.player, sizeof h.racer[r].name))
                x->place = h.racer[r].finished;
        }
        x->laps = h.laps;
        x->mode = h.mode;
        x->duration = h.duration;
        x->sort = seconds_of(h.date);
    }
    if (d) closedir(d);
    if (all) qsort(all, (size_t)n, sizeof *all, newer_first);
    if (n > max) n = max;
    if (all) memcpy(out, all, (size_t)n * sizeof *out);
    free(all);
    return n;
}

/* ---- managing the list (the menu's Replays tab): export, import, delete ----
 * The shared folder is "replays" next to bbport.ini (BB_CONFIG's folder, else the game's folder): exported races go
 * there with a readable name, and the races put there are imported (each once: a race already in RaceReplay, the same
 * date, track and length, is not copied again). */
void runtime_replay_folder(char *out, size_t size) {
    char base[600] = ".";
    const char *config = getenv("BB_CONFIG");
    if (config && *config) {
        snprintf(base, sizeof base, "%s", config);
        char *slash = strrchr(base, '/'), *back = strrchr(base, '\\');
        if (back > slash) slash = back;
        if (slash) *slash = 0;
        else snprintf(base, sizeof base, ".");
    }
    char folder[700];
    snprintf(folder, sizeof folder, "%s/replays", base);
#ifdef _WIN32
    _mkdir(folder);
    if (!_fullpath(out, folder, size)) snprintf(out, size, "%s", folder);
#else
    mkdir(folder, 0755);
    char *full = realpath(folder, NULL);
    snprintf(out, size, "%s", full ? full : folder);
    free(full);
#endif
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return 0;
    FILE *out = fopen(to, "wb");
    int ok = out != NULL;
    char buffer[65536];
    for (size_t n; ok && (n = fread(buffer, 1, sizeof buffer, in)) > 0;) ok = fwrite(buffer, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    if (out && fclose(out)) ok = 0;
    if (!ok) remove(to);
    return ok;
}

static int read_header(const char *path, struct rr_header *h) {
    FILE *f = fopen(path, "rb");
    const int ok = f && fread(h, 1, sizeof *h, f) == sizeof *h && !memcmp(h->magic, RR_MAGIC, 8);
    if (f) fclose(f);
    return ok;
}

/* a name of the list (not a path) */
static int plain_name(const char *name) {
    const size_t n = name ? strlen(name) : 0;
    return n > 4 && n < 64 && !strchr(name, '/') && !strchr(name, '\\') && !strstr(name, "..") && !strcmp(name + n - 4, ".crr");
}

static void safe_part(char *out, size_t size, const char *in) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < size; i++) {
        const unsigned char c = (unsigned char)in[i];
        out[o++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ? (char)c
                   : c >= 0x80 ? (char)c : '_';
    }
    out[o] = 0;
}

int runtime_replay_export(const char *name, char *saved_as, size_t size) {
    if (!plain_name(name)) return 0;
    char dir[700], from[800], folder[700], to[900], track[48], player[40];
    race_dir(dir, sizeof dir);
    snprintf(from, sizeof from, "%s/%s", dir, name);
    struct rr_header h;
    if (!read_header(from, &h)) return 0;
    h.track[sizeof h.track - 1] = h.player[sizeof h.player - 1] = 0;
    safe_part(track, sizeof track, h.track[0] ? h.track : "race");
    safe_part(player, sizeof player, h.player[0] ? h.player : "player");
    runtime_replay_folder(folder, sizeof folder);
    snprintf(to, sizeof to, "%s/20%02u-%02u-%02u_%02u%02u%02u_%s_%s.crr", folder, h.date[0], h.date[1], h.date[2], h.date[3],
             h.date[4], h.date[5], track, player);
    const int ok = copy_file(from, to);
    if (ok && saved_as) snprintf(saved_as, size, "%s", strrchr(to, '/') + 1);
    printf("Replay: %s %s to %s\n", name, ok ? "exported" : "could not be exported", to);
    fflush(stdout);
    return ok;
}

int runtime_replay_delete(const char *name) {
    if (!plain_name(name)) return 0;
    char dir[700], path[800];
    race_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    const int ok = !remove(path);
    printf("Replay: %s %s\n", name, ok ? "deleted" : "could not be deleted");
    fflush(stdout);
    return ok;
}

static int same_race(const struct rr_header *a, const struct rr_header *b) {
    return !memcmp(a->date, b->date, sizeof a->date) && a->frames == b->frames && !strncmp(a->level, b->level, sizeof a->level);
}

/* The races in the shared folder that RaceReplay does not have: copied in as I_<name>. The number copied, -1 none there */
int runtime_replay_import(void) {
    char folder[700], dir[700], path[1000], to[1000];
    runtime_replay_folder(folder, sizeof folder);
    race_dir(dir, sizeof dir);
    make_dirs(dir);
    struct rr_header *have = calloc(512, sizeof *have);
    int had = 0, found = 0, copied = 0;
    DIR *d = have ? opendir(dir) : NULL;
    for (struct dirent *e; d && had < 512 && (e = readdir(d));) {
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (plain_name(e->d_name) && read_header(path, &have[had])) had++;
    }
    if (d) closedir(d);
    d = have ? opendir(folder) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        struct rr_header h;
        snprintf(path, sizeof path, "%s/%s", folder, e->d_name);
        if (!plain_name(e->d_name) || !read_header(path, &h)) continue;
        found++;
        int known = 0;
        for (int i = 0; i < had && !known; i++) known = same_race(&have[i], &h);
        if (known) continue;
        snprintf(to, sizeof to, "%s/I_%s", dir, e->d_name);
        if (copy_file(path, to)) {
            copied++;
            if (had < 512) have[had++] = h;
        }
    }
    if (d) closedir(d);
    free(have);
    printf("Replay: imported %d of %d races from %s\n", copied, found, folder);
    fflush(stdout);
    return found ? copied : -1;
}
