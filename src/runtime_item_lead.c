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
    char owner[PING_NAME];
    int updates, moves, checks, kept, chasing, jumps;
    float lead_ms, farthest;
} Tracked;

static Tracked items[TRACKED];
static struct { int on, fixed_ms, extra_ms; } cfg = {-1, -1, 100};

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
        cfg.on = !(off && *off == '0');
        if (fixed && *fixed) cfg.fixed_ms = atoi(fixed);
        if (extra && *extra) cfg.extra_ms = atoi(extra);
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
    t->cap = speed > 1000.0f && speed < 20000.0f ? speed * 1.6f : 6400.0f;   /* bombs: 4000 at most, x1.6 */
    return t;
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
        if (norm(d) > t->cap * 2.0f * (float)dt + 300.0f) {
            t->samples = 0;
            t->jumps++;
        }
    }
    const int i = t->samples++ % SAMPLES;
    t->sample[i].t = now;
    memcpy(t->sample[i].p, at, sizeof t->sample[i].p);
}

static int same(const float *a, const float *b) { return !memcmp(a, b, 3 * sizeof(float)); }

/* its velocity (units per second) over the network's positions of the last ~0.1 s, and how old the newest
 * of them is (s); 0 not known yet */
static int velocity(const Tracked *t, double now, float v[3], double *age) {
    if (t->samples < 2) return 0;
    const int newest = (t->samples - 1) % SAMPLES, oldest = t->samples > SAMPLES ? t->samples - SAMPLES : 0;
    int pick = -1;
    for (int k = t->samples - 2; k >= oldest; k--) {
        const double dt = t->sample[newest].t - t->sample[k % SAMPLES].t;
        if (dt > 0.5) break;
        pick = k % SAMPLES;
        if (dt >= 0.08) break;
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
    float v[3];
    double stale = 0.0;
    if (!velocity(t, now, v, &stale)) return;
    t->speed = t->speed > 0.0f ? t->speed + (norm(v) - t->speed) * 0.2f : norm(v);
    if (now - t->rtt_at > 0.5) t->rtt = round_trip(t->owner), t->rtt_at = now;
    const float measured = calibrated(t->owner);
    float lead = cfg.fixed_ms >= 0 ? cfg.fixed_ms / 1000.0f
               : measured > 0.0f ? measured
               : t->rtt > 0.0f ? (t->rtt + cfg.extra_ms) / 1000.0f : 0.0f;
    if (lead > MAX_LEAD) lead = MAX_LEAD;
    if (lead <= 0.0f) return;
    t->lead_ms = lead * 1000.0f;
    const double age = now - t->first;
    if (age < EASE_IN) lead *= (float)(age / EASE_IN);
    lead += (float)stale;               /* the network's newest position is that old already */
    float off[3], length = 0.0f;
    for (int c = 0; c < 3; c++) off[c] = v[c] * lead, length += off[c] * off[c];
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

/* ---- every frame, and the install ---- */

void runtime_item_lead_tick(int racing) {
    net_config_tick();
    timing_tick(racing);
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
