/* Online: other players' missiles and bombs drawn ahead, where they really are (v0.8.6), and a log of
 * the race's timing on each PC.
 *
 * How the game plays them (USA 1.21, read 2026-11-12): only the PC that threw a missile or a bomb
 * simulates it. Its init (missile 0xf2a9f0, bomb 0xf1a4e0) makes the collision trigger
 * (CTriggerVolumeComponent) only where the entity has local authority ([entity + 0x3b] bit 0), and
 * only there does its update move it (missile 0xf28bc0: position += direction * speed * dt). Every
 * other PC holds a copy that the network moves, a round trip and the network's smoothing behind,
 * and the thrower's PC decides the hit. So at 200 ms a missile hits while it still looks far away,
 * and a shield comes too late when it is raised on sight.
 *
 * Here, this PC's copies of other players' missiles and bombs are drawn ahead along their own velocity
 * (capped at the missile's data speed: a spawn or removal teleport is no velocity). How far: what the
 * hits measured — when one of this thrower's missiles hits this PC's kart, how far behind the network
 * still had it, in seconds of its speed (the network's delay plus the game's smoothing, ~0.1 s more
 * than the round trip in tests) — and before the first hit, the round trip to the thrower (the RyuNET
 * ping, runtime_net_ping.c) plus BB_ITEM_LEAD_EXTRA ms (default 100). A missile chasing this PC's kart
 * goes toward it and stops just behind it until the hit comes. Only the drawing: the copy decides
 * nothing, and nothing goes over the network. The position the network gave is put back at the start
 * of the next frame (the pad read, runtime_replay_tick), so the game's own smoothing never sees ours.
 *   where:  after the item's own per-frame update (CEntityUpdateMessage handlers missile 0xf28bc0,
 *           bomb 0xf1b990: their registrations are pointed here, runtime_replay.c rr_redirect_lea)
 *   items:  the entity's position is entity + 0x20 (what replays' item guide moves as well)
 * BB_ITEM_LEAD=0 turns it off; BB_ITEM_LEAD_MS=n draws them n ms ahead whatever the ping.
 *
 * Race timing: when the race clock starts and when each racer's finish shows up on this PC, with
 * this PC's wall clock, so that two PCs' logs say whose clock started late ("Race timing:"). */
#include "runtime_replay_internal.h"
#include "runtime_net_ping.h"
#include "runtime_http.h"
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <strings.h>
#define _stricmp strcasecmp
#endif

#define D_ENTITY_TABLE 0x2a92880
#define TRACKED 32
#define SAMPLES 16
#define MAX_LEAD 0.8f      /* seconds */
#define EASE_IN 0.15       /* seconds: the lead grows in, no jump when an item appears */
#define KART_GAP 150.0f    /* a missile chasing this kart stops this far behind it (game units: cm) */
#define RECENT 8
#define THROWERS 8
/* Bombs (players' report 2026-11-17: "the bomb lags online, the missile is fine"): they were led like
 * missiles, by the thrower's missile calibration (300-500 ms, plus up to 200 ms of a stale network
 * position) along a velocity capped only at 6400 while a bomb rolls at ~4000, and logs showed the
 * estimate pinned at that cap: drawn 20-34 m ahead, jumping by half that whenever the estimate
 * moved. Now a bomb rolls at its own speed (CSpawnedLargeBowlingBombComponent +0x8c, set from its
 * data +0x48 at 0xf1c9cc; negative when thrown backwards), along a direction taken over a longer
 * stretch, by the round trip + BOMB_EXTRA_MS (no missile calibration), and the drawn offset follows
 * smoothly. BB_ITEM_LEAD_BOMB=0 draws bombs where the network has them (tests: compare). */
#define BOMB_MAX_LEAD 0.35f    /* seconds */
#define BOMB_EXTRA_MS 50
#define BOMB_STALE 0.1         /* seconds: no further on a network position older than this */
#define BOMB_WINDOW 0.2        /* seconds of network positions its direction is taken over */
#define BOMB_SMOOTH 0.08       /* seconds: how quickly the drawn offset follows a new one */
#define BOMB_FALLBACK_CAP 5200.0f   /* a bomb's speed unknown: no faster than this (juiced ~4600) */

typedef void ABI update_fn(void *component, void *a, void *b, void *c);
static update_fn *original_missile, *original_bomb;

enum { MISSILE, BOMB };
static const char *const KIND[] = {"missile", "bomb"};

typedef struct {
    void *component, *entity;
    int slot, kind;
    uint16_t generation;
    double first, last_seen, rtt_at;
    float base[3], lead[3];
    int has_base, written;
    struct { double t; float p[3]; } sample[SAMPLES];
    int samples;
    float rtt;                  /* ms, -1 unknown */
    float speed;                /* its usual speed (units/s), 0 not known yet */
    float cap;                  /* a missile's top speed from its data (x1.6, juiced), 0 none */
    float rolls;                /* a bomb's own speed (+0x8c), 0 not known */
    float shown[3];             /* a bomb's drawn offset, following the wanted one smoothly */
    int has_shown;
    char owner[PING_NAME];
    int updates, moves, checks, kept, chasing, jumps;
    float lead_ms, farthest;
} Tracked;

static Tracked items[TRACKED];
static struct { int on, fixed_ms, extra_ms, bombs; } cfg = {-1, -1, 100, 1};

/* items just gone (a hit often removes the missile in the frame its damage shows up) */
static struct { char owner[PING_NAME]; int kind; float base[3], speed, cap, lead_ms; double last_seen; } recent[RECENT];
static int recent_next;

/* What each thrower's items need, measured at the hits they gave this PC: how far behind the network still
 * had the item when the hit came, in seconds of its own speed. Before the first hit: round trip + extra. */
static struct { char owner[PING_NAME]; float lead; int hits; } calibration[THROWERS];

static float calibrated(const char *owner) {
    for (int i = 0; i < THROWERS; i++)
        if (calibration[i].hits && !strcmp(calibration[i].owner, owner)) return calibration[i].lead;
    return 0.0f;
}

static float calibrate(const char *owner, float needed) {
    int at = -1;
    for (int i = 0; i < THROWERS && at < 0; i++)
        if (calibration[i].hits && !strcmp(calibration[i].owner, owner)) at = i;
    for (int i = 0; i < THROWERS && at < 0; i++)
        if (!calibration[i].hits) at = i;
    if (at < 0) at = 0;
    if (!calibration[at].hits || strcmp(calibration[at].owner, owner)) {
        snprintf(calibration[at].owner, sizeof calibration[at].owner, "%s", owner);
        calibration[at].lead = needed, calibration[at].hits = 0;
    }
    calibration[at].lead += (needed - calibration[at].lead) * (calibration[at].hits ? 0.4f : 1.0f);
    calibration[at].hits++;
    return calibration[at].lead;
}

static double now_s(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER count;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static long long wall_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (long long)(t / 10000ULL) - 11644473600000LL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int enabled(void) {
    if (cfg.on < 0) {
        const char *off = getenv("BB_ITEM_LEAD"), *fixed = getenv("BB_ITEM_LEAD_MS"), *extra = getenv("BB_ITEM_LEAD_EXTRA");
        const char *bombs = getenv("BB_ITEM_LEAD_BOMB");
        cfg.on = !(off && *off == '0');
        cfg.bombs = !(bombs && *bombs == '0');
        if (fixed && *fixed) cfg.fixed_ms = atoi(fixed);
        if (extra && *extra) cfg.extra_ms = atoi(extra);
        if (cfg.on && !cfg.bombs) puts("Item lead: bombs drawn where the network has them (BB_ITEM_LEAD_BOMB=0)");
        if (cfg.on && cfg.fixed_ms >= 0) printf("Item lead: other players' missiles and bombs drawn %d ms ahead (BB_ITEM_LEAD_MS)\n", cfg.fixed_ms);
        else if (cfg.on) printf("Item lead: other players' missiles and bombs drawn ahead by the round trip + %d ms\n", cfg.extra_ms);
        else puts("Item lead: off (BB_ITEM_LEAD=0)");
        fflush(stdout);
    }
    return cfg.on;
}

/* ---- the game's entities ---- */

static int table_of(void ***entities, uint16_t **generation) {
    char *table = rr_ptr(D_ENTITY_TABLE);
    void *gens = table ? RR_AT(table, 0x28, void *) : 0;
    *entities = table ? RR_AT(table, 0x20, void **) : 0;
    *generation = gens ? RR_AT(gens, 0x20, uint16_t *) : 0;
    const int n = rr_count(gens);
    return *entities && *generation ? (n < 4096 ? n : 4096) : 0;
}

static int alive(const Tracked *t) {
    void **entities;
    uint16_t *generation;
    const int n = table_of(&entities, &generation);
    return t->slot >= 0 && t->slot < n && entities[t->slot] == t->entity && generation[t->slot] == t->generation;
}

static void *player_of(void *kart) {
    void *racer = kart ? rr_racer_of(kart) : 0;
    return racer ? rr_handle_object(&RR_AT(racer, 0x10, void *)) : 0;
}

/* a kart one of this PC's players drives (CPlayer +0x20: 0..7 local, 8 and up remote) */
static int local_player_kart(void *kart) {
    void *player = player_of(kart);
    return player && (uint32_t)RR_AT(player, 0x20, int32_t) <= 7;
}

static void owner_name(void *component, char out[PING_NAME]) {
    void *player = player_of(rr_handle_object(&RR_AT(component, 0x28, void *)));
    const char *name = player ? RR_AT(player, 0x68, const char *) : 0;
    size_t n = 0;
    for (; name && n < PING_NAME - 1 && name[n]; n++) out[n] = name[n];
    out[n] = 0;
}

/* the round trip to the thrower (ms): its nickname in the ping list, else the room's average; -1 none */
static float round_trip(const char *name) {
    PingPeer peers[PING_MAX_PEERS];
    const int n = runtime_net_ping_view(peers, PING_MAX_PEERS);
    float sum = 0.0f;
    int known = 0;
    for (int i = 0; i < n; i++) {
        if (peers[i].ms <= 0) continue;
        if (name[0] && !_stricmp(peers[i].name, name)) return (float)peers[i].ms;
        sum += (float)peers[i].ms;
        known++;
    }
    return known ? sum / known : -1.0f;
}

/* ---- the items ---- */

static void finish(Tracked *t, const char *why) {
    if (t->updates > 1)
        printf("Item lead: %s from %s %s after %.2f s: round trip %.0f ms, drawn %.0f ms ahead, speed %.0f, %d frames, "
               "the network moved it in %d (%d jumps), ours kept to the next frame %d/%d, %s, farthest ahead %.0f\n",
               KIND[t->kind], t->owner[0] ? t->owner : "?", why, t->last_seen - t->first, t->rtt, t->lead_ms, t->speed,
               t->updates, t->moves, t->jumps, t->kept, t->checks, t->chasing ? "chased this kart" : "not at this kart",
               t->farthest);
    if (t->updates > 1 && t->kind == BOMB)
        printf("Item lead: that bomb rolled at %.0f (its own speed%s)\n", t->rolls > 0.0f ? t->rolls : t->cap,
               t->rolls > 0.0f ? "" : " unknown: capped");
    if (t->has_base && t->speed > 0.0f) {
        __typeof__(recent[0]) *r = &recent[recent_next++ % RECENT];
        memcpy(r->owner, t->owner, sizeof r->owner);
        r->kind = t->kind, r->speed = t->speed, r->cap = t->cap, r->lead_ms = t->lead_ms, r->last_seen = t->last_seen;
        memcpy(r->base, t->base, sizeof r->base);
    }
    memset(t, 0, sizeof *t);
}

static Tracked *track(void *component, void *entity, int kind, double now) {
    Tracked *free_slot = 0;
    for (int i = 0; i < TRACKED; i++) {
        if (items[i].component == component && items[i].entity == entity) return &items[i];
        if (!items[i].component && !free_slot) free_slot = &items[i];
    }
    if (!free_slot) return 0;
    void **entities;
    uint16_t *generation;
    const int n = table_of(&entities, &generation);
    int slot = -1;
    for (int i = 0; i < n && slot < 0; i++)
        if (entities[i] == entity) slot = i;
    if (slot < 0) return 0;
    Tracked *t = free_slot;
    memset(t, 0, sizeof *t);
    t->component = component, t->entity = entity, t->kind = kind, t->slot = slot, t->generation = generation[slot];
    t->first = t->last_seen = now;
    owner_name(component, t->owner);
    t->rtt = round_trip(t->owner);
    t->rtt_at = now;
    /* a tracking missile's speed is its data's +0x14 (the thrower's update moves it by that, 0xf2a200) */
    void *data = RR_AT(component, 0x10, void *);
    const float speed = kind == MISSILE && data ? RR_AT(data, 0x14, float) : 0.0f;
    t->cap = speed > 1000.0f && speed < 20000.0f ? speed * 1.6f : 6400.0f;
    if (kind == BOMB) t->cap = BOMB_FALLBACK_CAP;   /* until its own speed is read (bomb_speed) */
    return t;
}

/* A bomb's own rolling speed (+0x8c, units/s; negative thrown backwards), 0 not a plausible one */
static float bomb_speed(void *component) {
    const float speed = fabsf(RR_AT(component, 0x8c, float));
    return speed > 500.0f && speed < 20000.0f ? speed : 0.0f;
}

static float norm(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* A position from the network: kept as a sample, unless it jumped (spawned at the thrower, teleported,
 * moved away to be removed): then the samples start again from it, and nothing is led until they agree. */
static void sample(Tracked *t, const float *at, double now) {
    if (t->samples) {
        /* a step longer than twice the top speed allows over the time passed (a frame at least: the
         * network's steps come unevenly) is a jump */
        const int last = (t->samples - 1) % SAMPLES;
        double dt = now - t->sample[last].t;
        if (dt < 1.0 / 30.0) dt = 1.0 / 30.0;
        float d[3];
        for (int c = 0; c < 3; c++) d[c] = at[c] - t->sample[last].p[c];
        /* a bomb's cap is its own speed (x1.15), too tight here: the network's bursts would count as
         * jumps (13-16 a bomb at 200 ms in tests); a spawn or removal teleport is far longer anyway */
        const float limit = t->kind == BOMB && t->cap < 6400.0f ? 6400.0f : t->cap;
        if (norm(d) > limit * 2.0f * (float)dt + 300.0f) {
            t->samples = 0;
            t->jumps++;
        }
    }
    const int i = t->samples++ % SAMPLES;
    t->sample[i].t = now;
    memcpy(t->sample[i].p, at, sizeof t->sample[i].p);
}

static int same(const float *a, const float *b) { return !memcmp(a, b, 3 * sizeof(float)); }

/* its velocity (units per second) over the network's positions of the last `window` s (~0.1 s missiles,
 * longer bombs), and how old the newest of them is (s); 0 not known yet */
static int velocity(const Tracked *t, double now, double window, float v[3], double *age) {
    if (t->samples < 2) return 0;
    const int newest = (t->samples - 1) % SAMPLES, oldest = t->samples > SAMPLES ? t->samples - SAMPLES : 0;
    int pick = -1;
    for (int k = t->samples - 2; k >= oldest; k--) {
        const double dt = t->sample[newest].t - t->sample[k % SAMPLES].t;
        if (dt > 0.5) break;
        pick = k % SAMPLES;
        if (dt >= window) break;
    }
    const double dt = pick >= 0 ? t->sample[newest].t - t->sample[pick].t : 0.0;
    if (dt < 0.03) return 0;
    for (int c = 0; c < 3; c++) v[c] = (float)((t->sample[newest].p[c] - t->sample[pick].p[c]) / dt);
    const float speed = norm(v);
    if (speed > t->cap)
        for (int c = 0; c < 3; c++) v[c] *= t->cap / speed;
    *age = now - t->sample[newest].t;
    if (*age > 0.2) *age = 0.2;     /* the network stopped moving it: no further than this */
    return 1;
}

static void lead_item(void *component, int kind) {
    void *entity = component ? RR_AT(component, 0x18, void *) : 0;
    if (!entity || (RR_AT(entity, 0x3b, uint8_t) & 1) || !enabled() || runtime_replay_playing()) return;
    const double now = now_s();
    Tracked *t = track(component, entity, kind, now);
    if (!t) return;
    float *at = &RR_AT(entity, 0x20, float);
    const double dt = now - t->last_seen;
    t->updates++;
    t->last_seen = now;
    if (!(t->written && same(at, t->lead)) && !(t->has_base && same(at, t->base))) {
        /* the network moved it */
        memcpy(t->base, at, sizeof t->base);
        t->has_base = 1;
        t->moves++;
        sample(t, at, now);
    }
    t->written = 0;
    if (!t->has_base) return;
    memcpy(at, t->base, sizeof t->base);    /* drawn where the network has it, unless led below */
    const int bomb = kind == BOMB;
    if (bomb && !cfg.bombs) return;
    if (bomb) {
        const float rolls = bomb_speed(component);
        if (rolls > 0.0f) t->rolls = rolls, t->cap = rolls * 1.15f;
    }
    float v[3];
    double stale = 0.0;
    if (!velocity(t, now, bomb ? BOMB_WINDOW : 0.08, v, &stale)) {
        /* no velocity this frame (its samples started again): a bomb keeps the offset it was drawn
         * at for a moment instead of falling back onto the network's position and jumping ahead again */
        if (bomb && t->has_shown && now - t->first > EASE_IN) {
            for (int c = 0; c < 3; c++) t->lead[c] = t->base[c] + t->shown[c];
            memcpy(at, t->lead, sizeof t->lead);
            t->written = 1;
        }
        return;
    }
    /* a rolling bomb goes at its own speed: the network's steps give only the direction (a slowing or
     * stopped one, about to go off, keeps what they show) */
    const float moving = norm(v);
    if (bomb && t->rolls > 0.0f && moving > t->rolls * 0.3f)
        for (int c = 0; c < 3; c++) v[c] *= t->rolls / moving;
    /* Preserve the 30 FPS response when item updates arrive more often. */
    const float blend = dt >= 0.0 && dt < 1.0 / 30.0 ? (float)(1.0 - pow(0.8, dt * 30.0)) : 0.2f;
    t->speed = t->speed > 0.0f ? t->speed + (norm(v) - t->speed) * blend : norm(v);
    if (now - t->rtt_at > 0.5) t->rtt = round_trip(t->owner), t->rtt_at = now;
    const float measured = bomb ? 0.0f : calibrated(t->owner);   /* missile hits measure missiles only */
    const int extra_ms = bomb ? BOMB_EXTRA_MS : cfg.extra_ms;
    float lead = cfg.fixed_ms >= 0 ? cfg.fixed_ms / 1000.0f
               : measured > 0.0f ? measured
               : t->rtt > 0.0f ? (t->rtt + extra_ms) / 1000.0f : 0.0f;
    const float most = bomb ? BOMB_MAX_LEAD : MAX_LEAD;
    if (lead > most) lead = most;
    if (lead <= 0.0f) return;
    t->lead_ms = lead * 1000.0f;
    const double age = now - t->first;
    if (age < EASE_IN) lead *= (float)(age / EASE_IN);
    if (bomb && stale > BOMB_STALE) stale = BOMB_STALE;
    lead += (float)stale;               /* the network's newest position is that old already */
    float off[3], length = 0.0f;
    for (int c = 0; c < 3; c++) off[c] = v[c] * lead;
    if (bomb) {
        /* the drawn offset follows the wanted one over ~BOMB_SMOOTH s: a network correction or a turn
         * bends the bomb's path instead of moving it at once */
        if (!t->has_shown) {
            memcpy(t->shown, off, sizeof t->shown);
            t->has_shown = 1;
        } else {
            const double step = dt > 0.0 ? (dt < 0.1 ? dt : 0.1) : 0.0;
            const float follow = (float)(1.0 - exp(-step / BOMB_SMOOTH));
            for (int c = 0; c < 3; c++) t->shown[c] += (off[c] - t->shown[c]) * follow;
        }
        memcpy(off, t->shown, sizeof off);
    }
    for (int c = 0; c < 3; c++) length += off[c] * off[c];
    length = sqrtf(length);
    /* a missile after this PC's kart (the tracking missile's target, +0xb0) goes at it and waits just behind */
    void *target = kind == MISSILE ? rr_handle_object(&RR_AT(component, 0xb0, void *)) : 0;
    if (target && local_player_kart(target)) {
        const float *kart = &RR_AT(target, 0x20, float);
        float d[3], dist = 0.0f;
        for (int c = 0; c < 3; c++) d[c] = kart[c] - t->base[c], dist += d[c] * d[c];
        dist = sqrtf(dist);
        float go = dist - KART_GAP;
        if (go < 0.0f) go = 0.0f;
        if (length < go) go = length;
        for (int c = 0; c < 3 && dist > 0.001f; c++) off[c] = d[c] / dist * go;
        length = go;
        t->chasing = 1;
    }
    for (int c = 0; c < 3; c++) t->lead[c] = t->base[c] + off[c];
    memcpy(at, t->lead, sizeof t->lead);
    t->written = 1;
    if (length > t->farthest) t->farthest = length;
}

static void ABI missile_update(void *component, void *a, void *b, void *c) {
    original_missile(component, a, b, c);
    lead_item(component, MISSILE);
}

static void ABI bomb_update(void *component, void *a, void *b, void *c) {
    original_bomb(component, a, b, c);
    lead_item(component, BOMB);
}

/* ---- race timing ---- */

static struct {
    int active;
    long long origin;           /* the wall clock (ms) at race clock 0 */
    void *finished[8];
} RT;

static void timing_tick(int racing) {
    if (!racing) {
        if (RT.active) memset(&RT, 0, sizeof RT);
        return;
    }
    RT.active = 1;
    const float clock = rr_race_clock();
    const long long wall = wall_ms();
    /* BB_KART_TRACE=1 (tests, "Prueba ping.bat"): every kart's position as drawn here, 10 times a second.
     * Two instances on one PC share the wall clock, so their logs say how far behind each one draws the other. */
    static int trace = -1;
    static long long traced;
    if (trace < 0) trace = getenv("BB_KART_TRACE") && *getenv("BB_KART_TRACE") == '1';
    if (trace && clock > 0.0f && wall - traced >= 100) {
        traced = wall;
        printf("Kart trace: wall %lld clock %.3f", wall, clock);
        for (int i = 0; i < 8; i++) {
            void *kart = rr_kart_of(i, 0);
            void *player = player_of(kart);
            if (!kart || !player) continue;
            const float *p = &RR_AT(kart, 0x20, float);
            printf(" | %s %s %.0f %.0f %.0f", RR_AT(player, 0x68, const char *) ? RR_AT(player, 0x68, const char *) : "?",
                   (uint32_t)RR_AT(player, 0x20, int32_t) <= 7 ? "here" : "remote", p[0], p[1], p[2]);
        }
        putchar('\n');
    }
    if (!RT.origin && clock > 1.0f) {
        RT.origin = wall - (long long)(clock * 1000.0f);
        printf("Race timing: the race clock started at wall %lld ms\n", RT.origin);
        fflush(stdout);
    }
    char *manager = rr_ptr(RR_D_RACER_MANAGER);
    void *list = manager ? RR_AT(manager, 0x10, void *) : 0;
    for (int i = 0; i < rr_count(list) && i < 8; i++) {
        void *racer = rr_item(list, i);
        if (!racer || !RR_AT(racer, 0x8c, uint8_t)) continue;
        int seen = 0, free_at = -1;
        for (int k = 0; k < 8; k++) {
            seen |= RT.finished[k] == racer;
            if (!RT.finished[k] && free_at < 0) free_at = k;
        }
        if (seen || free_at < 0) continue;
        RT.finished[free_at] = racer;
        void *player = rr_handle_object(&RR_AT(racer, 0x10, void *));
        const char *name = player ? RR_AT(player, 0x68, const char *) : "AI";
        void *laps = RR_AT(racer, 0x90, void *);
        const int count = rr_count(laps);
        const int32_t *times = laps && count > 0 && count < 100 ? RR_AT(laps, 0x20, int32_t *) : 0;
        printf("Race timing: %s finished (%s): race clock here %.3f s, wall %lld ms (clock start + %lld), "
               "end of race %.3f, laps %d, last lap record %d, position %d, official %d\n",
               name ? name : "?", RR_AT(racer, 0xb0, uint8_t) ? "this PC" : "another PC", clock, wall,
               RT.origin ? wall - RT.origin : -1, RR_AT(racer, 0x6c, float), count, times ? times[count - 1] : -1,
               RR_AT(racer, 0x58, int32_t), RR_AT(racer, 0x64, int32_t));
        fflush(stdout);
    }
}

/* ---- the hits: how far behind the network still had the item ---- */

#define HIT_RADIUS 150.0f       /* the item hits this far from the kart's centre (game units) */
static struct { void *kart; int state; } hurt[8];

static void hits_tick(double now) {
    for (int i = 0; i < 8; i++) {
        void *kart = rr_kart_of(i, 0);
        if (!kart || !local_player_kart(kart)) {
            hurt[i].kart = 0;
            continue;
        }
        void *damage = rr_component(kart, RR_D_DAMAGE_META);
        const int state = damage ? RR_AT(damage, 0x40, int32_t) : -1;   /* _damageStateInProgress */
        const int was = hurt[i].kart == kart ? hurt[i].state : state;
        hurt[i].kart = kart, hurt[i].state = state;
        if (state != 2 || was == 2) continue;                            /* eKDT_Explosive starts */
        const float *at = &RR_AT(kart, 0x20, float);
        float best = 1e30f, speed = 0.0f, cap = 0.0f, lead_ms = 0.0f;
        const char *owner = 0;
        int kind = MISSILE;
        for (int k = 0; k < TRACKED + RECENT; k++) {
            const int live = k < TRACKED;
            const Tracked *t = live ? &items[k] : 0;
            const __typeof__(recent[0]) *r = live ? 0 : &recent[k - TRACKED];
            if (live ? !t->component || !t->has_base || t->speed <= 0.0f || now - t->last_seen > 0.35
                     : r->speed <= 0.0f || now - r->last_seen > 0.35) continue;
            const float *base = live ? t->base : r->base;
            float d[3];
            for (int c = 0; c < 3; c++) d[c] = base[c] - at[c];
            const float dist = norm(d);
            if (dist >= best) continue;
            best = dist, speed = live ? t->speed : r->speed, cap = live ? t->cap : r->cap;
            lead_ms = live ? t->lead_ms : r->lead_ms;
            owner = live ? t->owner : r->owner, kind = live ? t->kind : r->kind;
        }
        if (!owner) {
            puts("Item lead: this kart was hit by an explosion, no other player's missile or bomb near");
            continue;
        }
        /* a missile flies at its data's speed at least (cap = 1.6 x that): a stalled estimate is not its speed */
        if (kind == MISSILE && speed < cap / 1.6f) speed = cap / 1.6f;
        const float gap = (best > HIT_RADIUS ? best - HIT_RADIUS : 0.0f) / speed;
        const float known = calibrated(owner);
        if (kind != MISSILE || gap > MAX_LEAD || (known > 0.0f && gap > known * 2.0f)) {
            printf("Item lead: hit by %s's %s: the network had it %.0f units (%.0f ms) away, drawn %.0f ms ahead "
                   "(not used to measure)\n", owner, KIND[kind], best, gap * 1000.0f, lead_ms);
            continue;
        }
        const float lead = calibrate(owner, gap);
        printf("Item lead: hit by %s's missile: the network had it %.0f units (%.0f ms) behind, drawn %.0f ms ahead; "
               "%s's items now drawn %.0f ms ahead\n", owner, best, gap * 1000.0f, lead_ms, owner, lead * 1000.0f);
    }
    fflush(stdout);
}

/* ---- the game's network config (tests) ----
 * CNetworkConfig (meta 0x2acb688; the one in use at [[0x2acb3d8] + 0x40]: Demonware's NetConfig file
 * when one downloads, which RyuNET does not serve, else the code's defaults, 0x196ae96):
 *   +0x28 _sendFrequency 15.0 (Hz): the session's local peer is made with it (0xa216f1, igNetLocalPeer
 *         _expectedSendRate); measured ~12 datagrams a second to a peer in a race
 *   +0x2c _lowBandwidthPingThreshold 0.25 (s): above it the low caps below apply
 *   +0x30/+0x34 low caps per connection / total 500 / 1500; +0x38/+0x3c high caps 1236 / 6180 (0xa4830f..)
 * The rate comes from RyuNET (GET /ryunet/net-rate, server net_rate.go: "hz=60 bandwidth=4"; hz=0 the
 * game's own), asked at start and every minute online, so that a room's players all send alike; a
 * session's local peer takes the frequency when it is made. BB_NET_SEND_HZ=n / BB_NET_BANDWIDTH=x
 * (tests) win over the server's. The caps are always the game's own times the multiplier. */
#define D_NETWORK 0x2acb3d8
int runtime_net_is_online(void);                /* runtime_net.h (its winsock headers go before windows.h) */

static struct {
    pthread_mutex_t lock;
    float hz, bandwidth;        /* the server's: 0 / 1 the game's own */
    int asked;
} RATE = {PTHREAD_MUTEX_INITIALIZER, 0.0f, 1.0f, 0};

static void *ask_rate(void *unused) {
    (void)unused;
    for (;;) {
        char body[128] = "";
        float hz = 0.0f, bandwidth = 1.0f;
        const int status = runtime_ryunet_request("GET", "/ryunet/net-rate", body, sizeof body);
        const int ok = status == 200 && !strncmp(body, "ok\n", 3) &&
                       sscanf(body, "ok\nhz=%f\nbandwidth=%f", &hz, &bandwidth) == 2;
        pthread_mutex_lock(&RATE.lock);
        if (ok) RATE.hz = hz >= 5.0f && hz <= 60.0f ? hz : 0.0f, RATE.bandwidth = bandwidth >= 0.5f && bandwidth <= 8.0f ? bandwidth : 1.0f;
        pthread_mutex_unlock(&RATE.lock);
#ifdef _WIN32
        Sleep(60000);
#else
        sleep(60);
#endif
    }
    return NULL;
}

static void net_config_tick(void) {
    static int env = -1;
    static float env_hz, env_bandwidth;
    static void *seen;
    static float caps_of_game[4], applied_hz = -1.0f, applied_bandwidth = -1.0f;
    if (env < 0) {
        const char *h = getenv("BB_NET_SEND_HZ"), *b = getenv("BB_NET_BANDWIDTH");
        env_hz = h ? (float)atof(h) : 0.0f, env_bandwidth = b ? (float)atof(b) : 0.0f;
        env = (env_hz >= 5.0f && env_hz <= 60.0f) || (env_bandwidth >= 0.5f && env_bandwidth <= 8.0f);
    }
    if (!RATE.asked && runtime_net_is_online()) {
        pthread_t thread;
        RATE.asked = 1;
        if (pthread_create(&thread, NULL, ask_rate, NULL) == 0) pthread_detach(thread);
    }
    char *network = rr_ptr(D_NETWORK);
    char *config = network ? RR_AT(network, 0x40, char *) : 0;
    if (!config) return;
    if (config != seen) {
        /* a config the game made (or downloaded): its own values, before ours */
        seen = config, applied_hz = applied_bandwidth = -1.0f;
        for (int k = 0; k < 4; k++) caps_of_game[k] = (float)RR_AT(config, 0x30 + 4 * k, int32_t);
        printf("Net config: send frequency %.1f Hz, low bandwidth from %.0f ms ping (caps %d per connection, %d in all), "
               "else caps %d / %d\n", RR_AT(config, 0x28, float), RR_AT(config, 0x2c, float) * 1000.0f,
               RR_AT(config, 0x30, int32_t), RR_AT(config, 0x34, int32_t), RR_AT(config, 0x38, int32_t),
               RR_AT(config, 0x3c, int32_t));
        fflush(stdout);
    }
    static float game_hz;
    if (applied_hz < 0.0f) game_hz = RR_AT(config, 0x28, float);
    pthread_mutex_lock(&RATE.lock);
    float hz = RATE.hz, bandwidth = RATE.bandwidth;
    pthread_mutex_unlock(&RATE.lock);
    const char *source = "RyuNET";
    if (env) {
        hz = env_hz >= 5.0f && env_hz <= 60.0f ? env_hz : 0.0f;
        bandwidth = env_bandwidth >= 0.5f && env_bandwidth <= 8.0f ? env_bandwidth : 1.0f;
        source = "BB_NET_SEND_HZ / BB_NET_BANDWIDTH";
    }
    if (hz <= 0.0f) hz = game_hz;
    if (hz == applied_hz && bandwidth == applied_bandwidth) return;
    RR_AT(config, 0x28, float) = hz;
    for (int k = 0; k < 4; k++) RR_AT(config, 0x30 + 4 * k, int32_t) = (int32_t)(caps_of_game[k] * bandwidth);
    if (applied_hz >= 0.0f || hz != game_hz || bandwidth != 1.0f)
        printf("Net config (%s): now %.1f Hz, caps %d / %d low, %d / %d high (from the next session)\n", source,
               RR_AT(config, 0x28, float), RR_AT(config, 0x30, int32_t), RR_AT(config, 0x34, int32_t),
               RR_AT(config, 0x38, int32_t), RR_AT(config, 0x3c, int32_t));
    fflush(stdout);
    applied_hz = hz, applied_bandwidth = bandwidth;
}

/* ---- item-box rules while the game runs ----
 * At start run.py writes the server's rules into systemData.igz (scripts/item_rules.py). A preset chosen in the
 * admin panel afterwards reaches a running game too: every 20 s online it asks GET /ryunet/items/live?have=<the
 * rules id it plays with>; the server answers "same", or every table in full (the preset over the game's own
 * odds), and the values go into the loaded tables between races (a race keeps the odds it started with). The
 * same walk as item_rules.py, in memory: systemData (0x9b9ec0, global 0x2ad9c38) +0xf8 CPowerUpData; +0x18 the
 * online tables by race mode (an igHashTable: +0x18 values, +0x20 their byte size, +0x28 keys, +0x30 count;
 * EOctaneRaceModes -> a list, +0xc count, +0x20 items, of weight tables EPowerUpType -> float), +0x20 the
 * cooldowns, +0x88 the item names. The online draw (0xe38030) reads the table at the network config +0x138,
 * written too when it is another one. Logins then say the new rules' id (X-RyuNET-Items).
 * Ranked has its own preset (25/11: public chaotic, Ranked competitive): in a Ranked room the game asks with
 * mode=ranked, and asks again at once when it enters or leaves one (checked every second), so the rules of the
 * mode it plays are in before its next race. */
#define LIVE_EMPTY 0xFFFFFFFFu
static struct {
    pthread_mutex_t lock;
    char *pending;              /* the server's answer, applied by the game's thread */
    char id[48];                /* the rules applied while running ("" the startup's) */
    int asked;
    int ranked;                 /* in a Ranked room now (the game's thread, live_items_tick) */
} LIVE = {PTHREAD_MUTEX_INITIALIZER, 0, "", 0, 0};

static void *ask_items(void *unused) {
    (void)unused;
    int asked_ranked = 0;
    for (;;) {
        /* every 20 s, at once when the mode changes */
        for (int second = 0; second < 20 && __atomic_load_n(&LIVE.ranked, __ATOMIC_ACQUIRE) == asked_ranked; ++second) {
#ifdef _WIN32
            Sleep(1000);
#else
            sleep(1);
#endif
        }
        asked_ranked = __atomic_load_n(&LIVE.ranked, __ATOMIC_ACQUIRE);
        char have[48] = "";
        pthread_mutex_lock(&LIVE.lock);
        snprintf(have, sizeof have, "%s", LIVE.id);
        pthread_mutex_unlock(&LIVE.lock);
        if (!have[0]) {
            const char *start = getenv("BB_ITEM_RULES_ID");
            snprintf(have, sizeof have, "%s", start && *start ? start : "-");
        }
        for (char *c = have; *c; c++)
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || *c == '-')) *c = '-';
        char target[96], *body = malloc(32768);
        if (!body) continue;
        snprintf(target, sizeof target, "/ryunet/items/live?have=%s%s", have, asked_ranked ? "&mode=ranked" : "");
        const int status = runtime_ryunet_request("GET", target, body, 32768);
        if (status == 200 && !strncmp(body, "id ", 3) && asked_ranked == __atomic_load_n(&LIVE.ranked, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&LIVE.lock);
            free(LIVE.pending);
            LIVE.pending = body, body = 0;
            pthread_mutex_unlock(&LIVE.lock);
        }
        free(body);
    }
    return NULL;
}

/* The value slot of key in an igHashTable (step: the values' size), 0 none */
static char *live_slot(char *table, uint32_t key, int step) {
    char *values = table ? RR_AT(table, 0x18, char *) : 0;
    uint32_t *keys = table ? RR_AT(table, 0x28, uint32_t *) : 0;
    const uint32_t n = table ? RR_AT(table, 0x20, uint32_t) / 4 : 0;
    for (uint32_t i = 0; values && keys && n <= 4096 && i < n; i++)
        if (keys[i] == key) return values + (size_t)step * i;
    return 0;
}

/* The EPowerUpType of an item named as the server names it (the game's names table), -1 none */
static int live_item_key(char *data, const char *name) {
    char *table = RR_AT(data, 0x88, char *);
    char *values = table ? RR_AT(table, 0x18, char *) : 0;
    uint32_t *keys = table ? RR_AT(table, 0x28, uint32_t *) : 0;
    const uint32_t n = table ? RR_AT(table, 0x20, uint32_t) / 4 : 0;
    for (uint32_t i = 0; values && keys && n <= 4096 && i < n; i++) {
        const char *s = keys[i] != LIVE_EMPTY ? *(const char **)(values + 8 * (size_t)i) : 0;
        if ((uintptr_t)s > 0x10000 && !strcmp(s, name)) return (int)keys[i];   /* a loaded name, not an index */
    }
    return -1;
}

/* The weight table at index of a race mode's list in an online table, 0 none */
static char *live_weights(char *online, int mode, int index) {
    char **slot = (char **)live_slot(online, (uint32_t)mode, 8);
    char *list = slot ? *slot : 0;
    if (!list || index < 0 || index >= RR_AT(list, 0xc, int32_t)) return 0;
    char **items = RR_AT(list, 0x20, char **);
    return items ? items[index] : 0;
}

/* A weight into a table: in its slot, or (an item the table lacks, weight above 0) in an empty one, as
 * item_rules.py does (experimental). 1 written, 0 not. */
static int live_put(char *table, int key, float weight, int *added) {
    float *slot = (float *)live_slot(table, (uint32_t)key, 4);
    if (slot) {
        *slot = weight;
        return 1;
    }
    if (weight <= 0.0f) return 1;
    float *free_value = (float *)live_slot(table, LIVE_EMPTY, 4);
    uint32_t *keys = RR_AT(table, 0x28, uint32_t *);
    char *values = RR_AT(table, 0x18, char *);
    if (!free_value || !keys || !values) return 0;
    keys[((char *)free_value - values) / 4] = (uint32_t)key;
    *free_value = weight;
    RR_AT(table, 0x30, uint32_t) += 1;
    ++*added;
    return 1;
}

static void live_apply(char *text) {
    char *system = rr_ptr(RR_D_SYSTEM_DATA);
    char *data = system ? RR_AT(system, 0xf8, char *) : 0;
    char *network = rr_ptr(D_NETWORK);
    char *config = network ? RR_AT(network, 0x40, char *) : 0;
    char *online[2] = {data ? RR_AT(data, 0x18, char *) : 0, config ? RR_AT(config, 0x138, char *) : 0};
    if (online[1] == online[0]) online[1] = 0;
    if (!data || !online[0] || !live_weights(online[0], 1, 7)) {
        puts("Live item rules: the game's item tables are not there; asked again in 20 s");
        return;
    }
    char id[48] = "";
    int written = 0, added = 0, failed = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "id ", 3)) {
            snprintf(id, sizeof id, "%s", line + 3);
            continue;
        }
        char section[16];
        int position = 0, consumed = 0;
        float value = 0.0f;
        if (sscanf(line, "%15[a-z]\t%d\t%f\t%n", section, &position, &value, &consumed) != 3 || !consumed) continue;
        const char *name = line + consumed;
        const int key = live_item_key(data, name);
        if (key < 0 || !(value >= 0.0f && value <= 1e6f)) {
            failed++;
            continue;
        }
        if (!strcmp(section, "cooldown")) {
            float *slot = (float *)live_slot(RR_AT(data, 0x20, char *), (uint32_t)key, 4);
            if (slot) *slot = value, written++;
            else failed++;
            continue;
        }
        const int mode = !strcmp(section, "race") ? 1 : !strcmp(section, "battle") ? 9 : !strcmp(section, "bacon") ? 12 : -1;
        const int index = mode == 1 ? position - 1 : 0;
        for (int t = 0; t < 2 && mode >= 0; t++) {
            char *table = online[t] ? live_weights(online[t], mode, index) : 0;
            if (!table) continue;
            if (live_put(table, key, value, &added)) written += t == 0;
            else failed++;
        }
    }
    pthread_mutex_lock(&LIVE.lock);
    snprintf(LIVE.id, sizeof LIVE.id, "%s", id);
    pthread_mutex_unlock(&LIVE.lock);
    runtime_items_id_set(id);
    printf("Live item rules: %s applied while running (%d values%s, %d added, %d not taken)\n", id, written,
           online[1] ? ", the network config's tables too" : "", added, failed);
    fflush(stdout);
}

static void live_items_tick(int racing) {
    __atomic_store_n(&LIVE.ranked, runtime_ranked_active(), __ATOMIC_RELEASE);
    if (!LIVE.asked && runtime_net_is_online() && getenv("BB_ONLINE_SERVER")) {
        const char *off = getenv("BB_LIVE_ITEM_RULES");
        LIVE.asked = 1;
        pthread_t thread;
        if (!(off && *off == '0') && pthread_create(&thread, NULL, ask_items, NULL) == 0) pthread_detach(thread);
    }
    if (racing) return;                     /* a race keeps the odds it started with */
    pthread_mutex_lock(&LIVE.lock);
    char *text = LIVE.pending;
    LIVE.pending = 0;
    pthread_mutex_unlock(&LIVE.lock);
    if (!text) return;
    live_apply(text);
    free(text);
}

/* ---- voice chat's karts (gpu/shim/bbport_voice.cpp): where each player of the race is, copied
 * every frame on the game's thread, so the voice thread never reads the game's own objects ---- */

static pthread_mutex_t voice_lock = PTHREAD_MUTEX_INITIALIZER;
static struct RuntimeVoiceKart voice_karts[8];
static int voice_kart_count;

/* What an item does to a kart, for the voice's effects (RUNTIME_VOICE_*; TNT on its head and the bubble also from
 * the spawned power-ups, rr_spawned_voice_effects):
 *   bubble:    the kart's CPowerShieldComponent (meta global 0x2ad1b60) +0x58 ShieldState is eSS_Shielded (1),
 *              or the bubble it holds (+0x50, an entity with CSpawnedPowerShieldComponent, meta 0x2ad7560) is
 *              eSSS_Spawned (2) at +0xd0 (0xe34ff0 reads them so when the kart is hit)
 *   invisible: CKartEffectsComponent (0x2ac5438) +0x48, the viewports that see the kart, is -1 but while the
 *              Invisibility item lasts (the invisibility look, runtime_replay_play.c)
 *   clock, squashed: CKartDamageComponent (fields 0x1da44a0) +0x40 _damageStateInProgress and +0x4c
 *              _damageAddedEffectInProgress (the effect that outlasts the hit), EKartDamageType: eKDT_Ntropied 7,
 *              eKDT_Crushed 8 / eKDT_CrushedAndStopped 9; squashed also while the kart controller's +0xa8 object
 *              +0x64 (set to 1.0 for damage types 8 and 9 at 0xe3aa90, back at 0xe3abd0) is up. The clock's
 *              voice lasts at least kClockHold after it was last seen (the hit alone is ~4 s). */
static int voice_effects(void *kart) {
    int fx = 0;
    void *shield = rr_component(kart, 0x2ad1b60);
    if (shield) {
        void *bubble = rr_handle_object(&RR_AT(shield, 0x50, void *));
        void *spawned = bubble ? rr_component(bubble, 0x2ad7560) : 0;
        if (RR_AT(shield, 0x58, int32_t) == 1 || (spawned && RR_AT(spawned, 0xd0, int32_t) == 2)) fx |= RUNTIME_VOICE_BUBBLE;
    }
    void *controller = rr_component(kart, RR_D_KART_CONTROLLER_META);
    void *flat = controller ? rr_handle_object(&RR_AT(controller, 0xa8, void *)) : 0;
    if (flat && RR_AT(flat, 0x64, float) > 0.5f) fx |= RUNTIME_VOICE_CRUSHED;
    void *effects = rr_component(kart, 0x2ac5438);
    if (effects && RR_AT(effects, 0x48, int32_t) != -1) fx |= RUNTIME_VOICE_INVISIBLE;
    void *damage = rr_component(kart, RR_D_DAMAGE_META);
    for (int k = 0; damage && k < 2; k++) {
        const int type = RR_AT(damage, k ? 0x4c : 0x40, int32_t);
        if (type == 7) fx |= RUNTIME_VOICE_CLOCK;
        if (type == 8 || type == 9) fx |= RUNTIME_VOICE_CRUSHED;
    }
    return fx;
}

static const double kClockHold = 4.0;   /* seconds */

static void voice_karts_tick(int racing) {
    struct RuntimeVoiceKart now[8];
    void *karts[8];
    static int spawned_fx[8], frames;
    static struct { char name[24]; int fx; } told[8];
    int n = 0;
    for (int i = 0; racing && i < 8; i++) {
        void *kart = rr_kart_of(i, 0);
        void *player = player_of(kart);
        const char *name = player ? RR_AT(player, 0x68, const char *) : 0;
        if (!kart || !name || !*name) continue;
        snprintf(now[n].name, sizeof now[n].name, "%s", name);
        memcpy(now[n].pos, &RR_AT(kart, 0x20, float), sizeof now[n].pos);
        now[n].here = (uint32_t)RR_AT(player, 0x20, int32_t) <= 7;
        now[n].effects = voice_effects(kart);
        karts[n++] = kart;
    }
    /* the spawned power-ups (TNT on a head, a bubble up): every PC has them for every kart; a few times a second */
    if (racing && ++frames % 4 == 1) {
        memset(spawned_fx, 0, sizeof spawned_fx);
        rr_spawned_voice_effects(karts, n, spawned_fx);
    }
    static void *clock_kart[8];
    static double clock_until[8];
    const double t = now_s();
    for (int i = 0; i < n; i++) {
        now[i].effects |= spawned_fx[i];
        /* the clock's slowdown outlasts its hit: its voice stays a while after it was last seen */
        if (now[i].effects & RUNTIME_VOICE_CLOCK) clock_kart[i] = karts[i], clock_until[i] = t + kClockHold;
        else if (clock_kart[i] == karts[i] && t < clock_until[i]) now[i].effects |= RUNTIME_VOICE_CLOCK;
        if (strcmp(told[i].name, now[i].name) || told[i].fx != now[i].effects) {
            if (told[i].name[0] && !strcmp(told[i].name, now[i].name))
                printf("Voice effects: %s%s%s%s%s%s\n", now[i].name, now[i].effects & RUNTIME_VOICE_BUBBLE ? " bubble" : "",
                       now[i].effects & RUNTIME_VOICE_CLOCK ? " clock" : "",
                       now[i].effects & RUNTIME_VOICE_INVISIBLE ? " invisible" : "",
                       now[i].effects & RUNTIME_VOICE_TNT ? " tnt" : "",
                       now[i].effects & RUNTIME_VOICE_CRUSHED ? " squashed" : now[i].effects ? "" : " none");
            snprintf(told[i].name, sizeof told[i].name, "%.*s", (int)sizeof told[i].name - 1, now[i].name);
            told[i].fx = now[i].effects;
        }
    }
    if (!racing) memset(spawned_fx, 0, sizeof spawned_fx);
    if (!racing) memset(told, 0, sizeof told);
    pthread_mutex_lock(&voice_lock);
    memcpy(voice_karts, now, sizeof(now[0]) * (size_t)n);
    voice_kart_count = n;
    pthread_mutex_unlock(&voice_lock);
}

int runtime_voice_karts(struct RuntimeVoiceKart *out, int max) {
    pthread_mutex_lock(&voice_lock);
    const int n = voice_kart_count < max ? voice_kart_count : max;
    memcpy(out, voice_karts, sizeof(out[0]) * (size_t)(n > 0 ? n : 0));
    pthread_mutex_unlock(&voice_lock);
    return n;
}

/* ---- every frame, and the install ---- */

void runtime_item_lead_tick(int racing) {
    net_config_tick();
    live_items_tick(racing);
    timing_tick(racing);
    voice_karts_tick(racing);
    runtime_voice_markers_tick(racing);
    const double now = now_s();
    for (int i = 0; i < TRACKED; i++) {
        Tracked *t = &items[i];
        if (!t->component) continue;
        if (!racing || !alive(t)) {
            finish(t, "gone");
            continue;
        }
        if (now - t->last_seen > 0.5) {
            finish(t, "ended");
            continue;
        }
        if (!t->written) continue;
        /* the network's position back before anything reads it this frame */
        float *at = &RR_AT(t->entity, 0x20, float);
        t->checks++;
        if (same(at, t->lead)) {
            t->kept++;
            memcpy(at, t->base, sizeof t->base);
        }
        t->written = 0;
    }
    if (!racing) {
        memset(recent, 0, sizeof recent);           /* the calibration stays: the same room races again */
        memset(hurt, 0, sizeof hurt);
        return;
    }
    if (enabled()) hits_tick(now);
}

void runtime_item_lead_install(int eu) {
    (void)eu;
    const char *off = getenv("BB_ITEM_LEAD");
    if (off && *off == '0') return;
    void *missile = 0, *bomb = 0;
    /* the remote copy's registration (0xf2adee) and the thrower's (0xf28493); bombs 0xf1b07c, 0xf1cf75 */
    int ok = rr_redirect_lea(0xf2adee, 0xf28bc0, (void (*)(void))missile_update, &missile);
    ok += rr_redirect_lea(0xf28493, 0xf28bc0, (void (*)(void))missile_update, &missile);
    original_missile = (update_fn *)missile;
    int bombs = rr_redirect_lea(0xf1b07c, 0xf1b990, (void (*)(void))bomb_update, &bomb);
    bombs += rr_redirect_lea(0xf1cf75, 0xf1b990, (void (*)(void))bomb_update, &bomb);
    original_bomb = (update_fn *)bomb;
    printf("Item lead: %d of 2 missile and %d of 2 bomb update sites hooked\n", ok, bombs);
}
