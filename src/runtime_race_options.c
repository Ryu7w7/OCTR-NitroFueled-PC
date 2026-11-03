/* Shared native race options, including offline menus. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_tt_reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef void (ABI *Event)(unsigned char *, unsigned char *, void *);
typedef void *(ABI *Type)(void *);
typedef void (ABI *DropString)(void *, void *);
typedef void (ABI *Refresh)(void *);
static Event team_event_original;
static DropString drop_string;
static Refresh refresh;
static void **manager, **settings, **pool, **changed_type;
static _Thread_local int refreshing;

static void *ptr(const void *p, size_t offset) {
    void *v;
    memcpy(&v, (const unsigned char *)p + offset, sizeof v);
    return v;
}
static int integer(const void *p, size_t offset) {
    int v;
    memcpy(&v, (const unsigned char *)p + offset, sizeof v);
    return v;
}
static void put_int(void *p, size_t offset, int value) {
    memcpy((unsigned char *)p + offset, &value, sizeof value);
}
static void put_ptr(void *p, size_t offset, void *value) {
    memcpy((unsigned char *)p + offset, &value, sizeof value);
}

static void ABI team_event(unsigned char *self, unsigned char *event, void *owner) {
    const int mode = *manager ? integer(*manager, 0xd40) : 0;
    if (!runtime_online_mode_private_single_options() || (mode >= 9 && mode <= 13)) {
        team_event_original(self, event, owner);
        return;
    }
    void *options = ptr(self, 0x1a8);
    const int compact = options && integer(options, 12) == 3;
    Type type = (Type)ptr(ptr(event, 0), 0);
    const int remap = compact && type(event) == *changed_type && integer(event, 0x18) == 2;
    if (*settings && integer(*settings, 0x28) == 3) put_int(*settings, 0x28, 2);
    if (remap) put_int(event, 0x18, 3); /* Visible index 2 means native FOUR teams. */
    team_event_original(self, event, owner);
    if (remap) put_int(event, 0x18, 2);
    options = ptr(self, 0x1a8);
    void *available = ptr(self, 0x1b0);
    if (!options || !available) return;
    const int count = integer(options, 12);
    if (count != 3 && count != 4) return;
    void *labels = ptr(options, 0x20);
    unsigned char *enabled = ptr(available, 0x20);
    if (!labels || !enabled || integer(available, 12) < count) return;
    if (count == 4) {
        char *removed = ptr(labels, 16);
        if (removed) drop_string(*pool, removed - 16);
        put_ptr(labels, 16, ptr(labels, 24)); /* Transfer the existing string reference. */
        put_ptr(labels, 24, NULL);
        enabled[2] = enabled[3];
        put_int(options, 12, 3);
        put_int(available, 12, 3);
    } else {
        /* The private CUP wrapper may disable team choices on each event. */
        enabled[2] = !runtime_online_mode_private_cup();
    }
    if (*settings) {
        const int teams = integer(*settings, 0x28);
        put_int(self, 0x190, teams == 4 ? 2 : teams == 2 ? 1 : 0);
    }
    if (!refreshing && (count == 4 || remap) && ptr(self, 0x198)) {
        refreshing = 1;
        refresh(self);
        refreshing = 0;
    }
}

typedef char *(ABI *InternString)(void *, const char *);
typedef void (ABI *AppendChoice)(void *, char **, void *);
typedef int (ABI *Difficulty)(void *);
typedef void *(ABI *DataGetter)(void);
static Event difficulty_event_original;
static InternString intern_string;
static AppendChoice append_choice;
static Difficulty native_difficulty;
static DataGetter cold_ai_data;
static void **difficulty_manager, **ai_data;
static void **client;
static _Atomic int ai_profile;
/* Learned only from native replicated AI input, never from a guest menu. */
static _Atomic int followed_ai_profile = -1;
static _Atomic int followed_ai_base = -1, lobby_personal_difficulty;
static _Thread_local int peer_decision;
typedef void (ABI *AiInput)(void *, void *);
static AiInput native_ai_input, native_apply_ai_input;
static void apply_tt_technique(void *self, void *input);
static void apply_item_during_slide(void *self, void *input);
static _Thread_local void *pending_item_use;
static void trace_native_controls(void *self);
static void register_ai_physics(void *self);
static void reset_ai_physics(void);
typedef void *(ABI *NetContext)(void);
typedef unsigned char (ABI *OwnsObject)(void *, void *);
static NetContext net_context;
static OwnsObject owns_object;
typedef void (ABI *ResolveHandle)(void **);
static ResolveHandle race_resolve_handle;

static void tune_ai(void *root);
static int custom_ai_allowed(void) {
    const int mode = client && *client ? integer(*client, 0x144) : 0;
    const int race = manager && *manager ? integer(*manager, 0xd4c) : 0;
    return (mode == 8 && (race == 1 || race == 6)) ||
           (mode == 16 && runtime_private_match_active() && (race == 1 || race == 6));
}

static int custom_ai_menu_allowed(void) {
    const int mode = client && *client ? integer(*client, 0x144) : 0;
    if (mode == 16)
        return runtime_private_match_active() &&
               (runtime_online_mode_private_single_options() || runtime_online_mode_private_cup());
    /* The menu edits the next race; the previous race can still be Battle. */
    const int next = manager && *manager ? integer(*manager, 0xd40) : 0;
    return mode == 8 && (next == 1 || next == 6);
}

void runtime_race_options_leave_room(void) {
    followed_ai_profile = followed_ai_base = -1;
    lobby_personal_difficulty = 0;
}

void runtime_race_options_map_loading(const char *map) {
    if (!map) return;
    reset_ai_physics();
    const int lobby = !strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby");
    const int was_lobby = atomic_exchange(&lobby_personal_difficulty, lobby);
    if (lobby || was_lobby) followed_ai_profile = followed_ai_base = -1;
    if (lobby) {
        puts("Native AI: lobby restored personal difficulty; race authority override cleared");
    }
}

/* c0a3b0 selects personal +0c or replicated +10 by native ownership. Keep the
 * received base across takeover without overwriting either native field. */
static int ABI room_difficulty(void *self) {
    const int online = client && *client && integer(*client, 0x144) == 16;
    const int base = followed_ai_base;
    if (online && lobby_personal_difficulty && custom_ai_menu_allowed()) return integer(self, 12);
    if (online && custom_ai_allowed() && base >= 0 && base <= 2) return base;
    return integer(self, owns_object(net_context(), self) ? 12 : 16);
}

int runtime_private_difficulty_modified(void) {
    /* Hard and the three extended profiles all use native Hard (2). */
    return runtime_private_match_active() &&
           (runtime_online_mode_private_single_options() || runtime_online_mode_private_cup()) &&
           settings && *settings &&
           ((unsigned char *)*settings)[0x10] && difficulty_manager && *difficulty_manager &&
           native_difficulty && native_difficulty(*difficulty_manager) == 2;
}

static int effective_profile(void) {
    if (!custom_ai_allowed()) return 0;
    const int online = client && *client && integer(*client, 0x144) == 16;
    const int followed = followed_ai_profile;
    return online && !lobby_personal_difficulty && followed >= 0 ? followed : ai_profile;
}

typedef void (ABI *RaceTargets)(unsigned char *, unsigned char *);
static RaceTargets native_race_targets;
typedef void (ABI *WorldPosition)(float *, void *);
static WorldPosition world_position;
static _Thread_local unsigned reference_intent, previous_ai_actions, reference_native_hop;
struct SlideDecision {
    int entry, continuation, direction, complete, oversteer, reference_hold;
    float entry_angle, curve, continue_angle;
};
static _Thread_local struct SlideDecision slide_decision;
static _Thread_local int reference_slot = -1, observed_slot = -1;
static struct { void *owner; unsigned active, phase, boost_reports; int boosts; } tt_jumps[8];
static void **collectible_manager, **power_use_type, **racer_manager;
typedef unsigned char (ABI *ScanTargets)(void *, void *, const float *, const float *);
static ScanTargets native_scan_targets;
typedef unsigned char (ABI *EnemyRacer)(void *, void *);
static EnemyRacer native_enemy_racer;
static void *box_claims[8];
static int box_kind[8], racer_wumpa[8], racer_perfect_chain[8], racer_crates[8];


static atomic_flag tuning_lock = ATOMIC_FLAG_INIT;
static int applied_profile = -1;
typedef unsigned char (ABI *BoostReady)(void *, float);
typedef void *(ABI *Component)(void *, void *, int);
typedef float (ABI *BoostDuration)(void *);
static BoostReady native_boost_ready, native_drift_oversteer;
typedef unsigned char (ABI *DriftComplete)(void *);
static DriftComplete native_drift_complete;
static unsigned char boost_decision(void *controller, float threshold, int ai_call);
static float fire_remaining(void *controller);
static unsigned char bank_slide_boost(void *controller);
static Component component;
static BoostDuration maintained_duration, normalized_kart_speed;
static void **boost_type;
static void *ai_boost_return;
typedef void *(ABI *ChooseLine)(void *, unsigned char, float);
static ChooseLine native_choose_line;
static void *ai_line_return;
typedef unsigned char (ABI *StartDrift)(void *, float);
static StartDrift native_start_drift;
typedef unsigned char (ABI *ContinueDrift)(void *, int, float);
static ContinueDrift native_continue_drift;
typedef void *(ABI *ChooseBranch)(void *, void **, unsigned char, unsigned char);
static ChooseBranch native_choose_branch;

static float real(const void *p, size_t offset) {
    float value;
    memcpy(&value, (const unsigned char *)p + offset, 4);
    return value;
}

static int selected_driving_level(void) {
    if (!custom_ai_allowed() || !difficulty_manager || !*difficulty_manager) return -1;
    const int level = native_difficulty(*difficulty_manager);
    if (runtime_speed_race_active() && level < 2) return -1;
    const int profile = effective_profile();
    return level >= 0 && level <= 2 ? level + (level == 2 && profile >= 1 && profile <= 3 ? profile : 0) : -1;
}

static int driving_level(void) { return peer_decision ? -1 : selected_driving_level(); }

/* dcb630 owns the authority/follower split. Keep it native: only the owner
 * produces actions. The already-replicated byte reserves bits 6..7; readers of
 * the six native actions mask bits 0..5. No new packet or native enum value. */
static void ABI ai_input(void *self, void *input) {
    if (!custom_ai_allowed()) {
        followed_ai_profile=followed_ai_base=-1;
        if (ai_data && *ai_data) tune_ai(*ai_data); /* Restore cached private tuning before public AI runs. */
        native_ai_input(self,input);
        return;
    }
    unsigned char *entity = ptr(self, 0x18), *wire = ptr(self, 0x70);
    const int online = client && *client && integer(*client, 0x144) == 16;
    const int owner = !entity || (entity[0x3b] & 1);
    const int saved_peer = peer_decision;
    if (online && wire && !owner) {
        const int profile = wire[0x10] >> 6;
        const int base = profile ? 2 : difficulty_manager && *difficulty_manager ?
                         integer(*difficulty_manager, 16) : -1;
        if (base >= 0 && base <= 2) followed_ai_base = base;
        const int previous = atomic_exchange(&followed_ai_profile, profile);
        if (previous != profile)
            printf("Native AI: adopted authority profile=%d from replicated input\n", profile);
    }
    if (online && wire && owner) {
        const int level = driving_level();
        const unsigned profile = level >= 3 && level <= 5 ? (unsigned)(level - 2) : 0;
        wire[0x10] = (wire[0x10] & 0x3f) | (profile << 6);
    }
    peer_decision = online && !owner;
    const unsigned saved_intent = reference_intent, saved_actions = previous_ai_actions, saved_hop = reference_native_hop;
    previous_ai_actions = wire ? wire[0x10] : 0;
    const int saved_slot = reference_slot, saved_observed = observed_slot;
    reference_intent = 0; reference_slot = observed_slot = -1; reference_native_hop = 0;
    /* dcae80 binds driving at +48; +58 is item-use, not steering. */
    unsigned char *control = ptr(self, 0x48);
    unsigned char drift_allowed = control ? control[0x50] : 0, boost_allowed = control ? control[0x51] : 0;
    struct SlideDecision saved_slide = slide_decision;
    memset(&slide_decision, 0, sizeof slide_decision);
    slide_decision.entry = slide_decision.continuation = -1;
    void *saved_item_use = pending_item_use;
    pending_item_use = NULL;
    if (ai_data && *ai_data) tune_ai(*ai_data);
    native_ai_input(self, input);
    register_ai_physics(self);
    if (!peer_decision) {
        apply_tt_technique(self, input);
        apply_item_during_slide(self, input);
        if (reference_slot < 0 || driving_level() < 3) trace_native_controls(self);
    }
    pending_item_use = saved_item_use;
    if (control) { control[0x50] = drift_allowed; control[0x51] = boost_allowed; }
    reference_intent = saved_intent; reference_slot = saved_slot; observed_slot = saved_observed;
    previous_ai_actions = saved_actions; reference_native_hop = saved_hop;
    slide_decision = saved_slide;
    peer_decision = saved_peer;
}

static void *race_handle(void **slot) {
    for (int depth = 0; slot && *slot && depth < 16; ++depth) {
        unsigned flags = (unsigned)integer(*slot, 0);
        if (flags & 0x10000000) {
            if (!race_resolve_handle) return NULL;
            race_resolve_handle(slot);
        }
        if (!*slot) return NULL;
        flags = (unsigned)integer(*slot, 0);
        unsigned char *object = ptr(*slot, 0x28);
        if (!(flags & 0x20000000) || !object) return object;
        slot = (void **)(object + 0x10);
    }
    return NULL;
}

static struct { void *kart, *entity; unsigned id; } ai_physics[8];
typedef float (ABI *KartSpeed)(void *, void *, int, float, float);
typedef void (ABI *SpeedModifiers)(void *, float);
static KartSpeed native_kart_speed;
static SpeedModifiers native_speed_modifiers;

static void reset_ai_physics(void) { memset(ai_physics,0,sizeof ai_physics); }
static void register_ai_physics(void *self) {
    unsigned char *control=ptr(self,0x48);
    void *kart=control ? race_handle((void **)(control+0x58)) : NULL;
    void *entity=ptr(self,0x18);
    if (!kart || !entity || ptr(kart,0x18)!=entity) return;
    unsigned free_slot=8;
    for (unsigned i=0;i<8;++i) {
        if (ai_physics[i].kart==kart || ai_physics[i].entity==entity) { free_slot=i; break; }
        if (!ai_physics[i].kart && free_slot==8) free_slot=i;
    }
    if (free_slot<8) {
        ai_physics[free_slot].kart=kart; ai_physics[free_slot].entity=entity;
        /* AI+28 holds steering targets. Match the live racer by entity instead. */
        void *list=racer_manager && *racer_manager ? ptr(*racer_manager,0x10) : NULL;
        const int count=list ? integer(list,12) : 0;
        void **racers=list ? ptr(list,0x20) : NULL;
        ai_physics[free_slot].id=0;
        if (racers && count>=0 && count<=8) for (int i=0;i<count;++i) {
            unsigned char *racer=racers[i];
            if (!racer || race_handle((void **)(racer+0x20))!=entity) continue;
            void *player=race_handle((void **)(racer+0x10));
            const int id=player ? integer(player,0xc8) : integer(racer,0x164);
            if (id>=0 && id<8) ai_physics[free_slot].id=(unsigned)id;
            break;
        }
    }
}
static int physics_ai_level(void *kart) {
    if (!kart || !client || !*client || !manager || !*manager ||
        (integer(*manager,0xd4c)!=1 && integer(*manager,0xd4c)!=6)) return -1;
    int mode=integer(*client,0x144);
    if (mode!=8 && (mode!=16 || !runtime_private_match_active())) return -1;
    for (unsigned i=0;i<8;++i)
        if (ai_physics[i].kart==kart && ai_physics[i].entity==ptr(kart,0x18))
            return selected_driving_level(); /* Replicated AI physics needs the same profile on peers. */
    return -1;
}
static float ai_speed_scale(int level, int itemless, uint32_t ticks, unsigned id) {
    static const float middle[]={0,0,.04f,.06f,.08f,.105f};
    if (level<0 || level>5 || !level || (level==1 && !itemless)) return 1;
    const float center=level==1 ? .015f : middle[level];
    const float amplitude=level==1 ? .005f : level==5 ? .015f : .01f;
    /* Sixteen simulation seconds per cycle; wrapping and pauses stay continuous.
     * Network racer identity offsets each kart, never process pointers or RNG. */
    const float phase=(ticks%131072)/131072.f+(id%8)/8.f;
    return 1+center+amplitude*sinf(6.28318530718f*phase);
}
static float kart_speed_scale(void *kart, int level, int itemless) {
    void *timer=ptr(kart,0xf8), *vtable=timer ? ptr(timer,0) : NULL;
    typedef uint32_t (ABI *Now)(void *);
    Now now_fn=vtable ? (Now)ptr(vtable,0xb0) : NULL;
    unsigned id=0;
    for (unsigned i=0;i<8;++i)
        if (ai_physics[i].kart==kart) { id=ai_physics[i].id; break; }
    return ai_speed_scale(level,itemless,now_fn ? now_fn(timer) : 0,id);
}
/* Native e4a220 reads speed-data +c and curve pointers +18/+20/+28.
 * A call-local view changes AI base speed without mutating a shared archetype. */
static float ABI kart_speed(void *kart, void *data, int kind, float speed, float dt) {
    int level=physics_ai_level(kart);
    const int itemless=runtime_speed_race_active();
    float scale=level>=0 ? kart_speed_scale(kart,level,itemless) : 1;
    static int reported_policy=-1;
    const int policy=level+(itemless ? 16 : 0);
    if (level>=0 && reported_policy!=policy) {
        reported_policy=policy;
        printf("Native AI: driving policy route=%s level=%d dynamic_base_speed_scale=%.3f cycle_seconds=16 ignore_slow_surfaces=%d\n",
               itemless ? "ghost-traffic" : "native",level,scale,itemless);
    }
    if (scale==1 || !data || !isfinite(real(data,12)) || real(data,12)<=0)
        return native_kart_speed(kart,data,kind,speed,dt);
    unsigned char tuned[0x30]; memcpy(tuned,data,sizeof tuned);
    float top=real(data,12)*scale; memcpy(tuned+12,&top,4);
    return native_kart_speed(kart,tuned,kind,speed,dt);
}
/* e452c0 expires native modifiers and combines positive multipliers with the
 * strongest slowdown. Itemless AI retains positives and ignores that slowdown. */
static void ABI speed_modifiers(void *kart, float dt) {
    const int slow_time=integer(kart,0x194);
    native_speed_modifiers(kart,dt);
    if (physics_ai_level(kart)<0 || !runtime_speed_race_active()) return;
    void *list=ptr(kart,0x188);
    int count=list ? integer(list,12) : 0;
    void **items=list ? ptr(list,0x20) : NULL;
    if (count<0 || count>256 || (count && !items)) return;
    float multiplier=1;
    for (int i=0;i<count;++i) {
        void *data=items[i] ? ptr(items[i],0x10) : NULL;
        if (!data) continue;
        float value=real(data,0x10);
        if (value<0 || ((unsigned char *)kart)[0x214]) value=real(data,0xc);
        if (value>1 && isfinite(value)) multiplier*=value;
    }
    if (isfinite(multiplier)) {
        memcpy((unsigned char *)kart+0x190,&multiplier,4);
        put_int(kart,0x194,slow_time); /* Do not arm native post-surface damping. */
    }
}

static uintptr_t route_identity(void *self) {
    unsigned char *racer = self ? race_handle((void **)((unsigned char *)self + 0x28)) : NULL;
    if (!racer) return 0;
    /* Native network/AI identity and lap, not process-specific pointers. */
    unsigned char *player = race_handle((void **)(racer + 0x10));
    const int id = player ? integer(player, 0xc8) : integer(racer, 0x164);
    return (uint32_t)id +
           UINT64_C(0x9e3779b9) * (uint32_t)integer(racer, 0x4c);
}

/* The native manager contains live components, including respawning crates.
 * A short corridor around the current route avoids detours through scenery. */
static float crate_score(const float position[3], const float target[3], const float crate[3], int level, int retained) {
    float dx = target[0] - position[0], dy = target[1] - position[1];
    float length = sqrtf(dx * dx + dy * dy);
    if (!isfinite(length) || length < 1 || level < 0 || level > 5) return INFINITY;
    float x = crate[0] - position[0], y = crate[1] - position[1];
    float forward = (x * dx + y * dy) / length;
    float side = fabsf((y * dx - x * dy) / length);
    if (!isfinite(forward) || !isfinite(side) || !isfinite(crate[2]) ||
        forward < (retained ? 0 : 30) || forward > fminf(1500, length + 350) ||
        fabsf(crate[2] - position[2]) > 180 || side > 90 + level * 28 || side > fmaxf(retained ? 120.f : 0,forward * .4f))
        return INFINITY;
    return forward + side * (4.f - level * .45f);
}

static int aim_at_box(unsigned char *ai, unsigned char *entity, unsigned id, int level,
                      const float position[3], float targets[2][3], unsigned intent, int wumpa) {
    void *previous = box_claims[id];
    box_claims[id] = NULL; box_kind[id] = -1;
    if (runtime_speed_race_active()) return 0; /* Itemless keeps the racing line and boost intent. */
    const unsigned takeoff=(intent & TT_REFERENCE_DRIFT_HOP) ? 0 : intent & (2u|512u);
    if (takeoff || intent & (TT_REFERENCE_BOX_BLOCK|TT_REFERENCE_BELOW_ROUTE) || !collectible_manager || !*collectible_manager || !power_use_type || !*power_use_type) return 0;
    unsigned char *driving=ptr(ai,0x48);
    unsigned char *kart=driving ? race_handle((void **)(driving+0x58)) : NULL;
    if (kart && ptr(kart,0x18)==entity && !kart[0x214]) return 0;
    unsigned char *use = component(entity, *power_use_type, 0);
    unsigned char *controller = use ? race_handle((void **)(use + 0x28)) : NULL;
    const int needs_item = controller && !ptr(controller, 0x30) && !controller[0x40];
    const int needs_wumpa = wumpa >= 0 && wumpa < 10;
    if ((!needs_item && !needs_wumpa) || ai[0x7b]) return 0;
    void *list = ptr(*collectible_manager, 0x10);
    int count = list ? integer(list, 12) : 0;
    void **items = list ? ptr(list, 32) : NULL;
    if (!items || count < 0 || count > 4096) return 0;
    float best = INFINITY, best_position[3] = {0};
    void *chosen = NULL;
    int chosen_kind = -1, live[2]={0}, reachable[2]={0};
    for (int i = 0; i < count; ++i) {
        unsigned char *crate = items[i];
        void *data = crate ? ptr(crate, 0x10) : NULL;
        void *object = crate ? ptr(crate, 0x18) : NULL;
        if (!object || !data || !crate[0x2c] || crate[0x2d]) continue;
        const int kind = integer(data, 0x18); /* Native CrateContent:0 item,1 Wumpa crate. */
        if (kind<0 || kind>1) continue;
        ++live[kind];
        if (!((kind == 0 && needs_item) || (kind == 1 && needs_wumpa))) continue;
        float where[3]; world_position(where, object);
        float score = crate_score(position, targets[1], where, level, object==previous);
        if (isfinite(score)) ++reachable[kind];
        if (kind == 1) score *= 1.f - (10 - wumpa) * .03f;

        for (unsigned j = 0; j < 8; ++j) if (j != id && box_claims[j] == object) score += 600;
        /* Commit while the chosen box remains valid and ahead; occupancy
         * changes must not switch steering to a competing box every frame. */
        if (object == previous && isfinite(score)) score = -1;
        if (score < best) { best = score; chosen = object; chosen_kind = kind; memcpy(best_position, where, sizeof where); }
    }
    static unsigned scans[8];
    if (scans[id]++ % 180==0 && scans[id]<=4320)
        printf("Native AI: pickup scan id=%u needs_item=%d wumpa=%d live_item=%d live_wumpa=%d reachable_item=%d reachable_wumpa=%d selected=%d\n",
               id,needs_item,wumpa,live[0],live[1],reachable[0],reachable[1],chosen_kind);
    if (!chosen) return 0;
    box_claims[id] = chosen; box_kind[id] = chosen_kind;
    static unsigned reports[8];
    if (chosen != previous && reports[id]++ < 48)
        printf("Native AI: pickup target id=%u kind=%s wumpa=%d needs_item=%d retained=0\n",
               id,chosen_kind ? "wumpa-box" : "item-box",wumpa,needs_item);
    memcpy(targets[0], best_position, sizeof best_position);
    /* Keep looking down the route after contact, not backwards toward the box. */
    float crate_distance = 0, target_distance = 0;
    for (int k = 0; k < 2; ++k) {
        float a = best_position[k] - position[k], b = targets[1][k] - position[k];
        crate_distance += a * a; target_distance += b * b;
    }
    if (crate_distance > target_distance) memcpy(targets[1], best_position, sizeof best_position);
    return 1;
}

static int shot_in_cone(const float position[3], const float direction[3], const float target[3],
                        float range, float radius, float degrees) {
    if (!isfinite(range) || range <= 0 || !isfinite(radius) || radius < 0 ||
        !isfinite(degrees) || degrees < 0 || degrees > 360) return 0;
    float dx = target[0] - position[0], dy = target[1] - position[1], dz = target[2] - position[2];
    float length = sqrtf(direction[0] * direction[0] + direction[1] * direction[1]);
    if (!isfinite(length) || length < .01f || !isfinite(dz) || fabsf(dz) > 350) return 0;
    float forward = (dx * direction[0] + dy * direction[1]) / length;
    float side = fabsf((dy * direction[0] - dx * direction[1]) / length);
    return forward > 0 && dx * dx + dy * dy < range * range &&
           atan2f(fmaxf(0, side - radius), forward) < degrees * .00872664626f;
}

static unsigned char ABI scan_targets(void *self, void *racer, const float *direction, const float *angles) {
    const unsigned char result = native_scan_targets(self, racer, direction, angles);
    const int level = driving_level();
    if (!result || level < 0 || !manager || !*manager ||
        integer(*manager, 0xd4c) - 9u < 5 || !native_enemy_racer || !racer_manager || !*racer_manager)
        return result; /* Battle keeps its native policy. */
    unsigned char *entity = ptr(self, 0x18);
    if (!entity || (client && *client && integer(*client, 0x144) == 16 && !(entity[0x3b] & 1))) return result;
    void *root = ai_data ? *ai_data : NULL;
    if (!root) return 0;
    float limits[3];
    int column = level < 2 ? level : 2;
    for (int i = 0; i < 3; ++i) {
        void *value = ptr(root, 0x10 + (size_t)i * 8);
        if (!value) return 0;
        limits[i] = real(value, 12 + (size_t)column * 4);
    }
    void *list = ptr(*racer_manager, 0x10);
    int count = list ? integer(list, 12) : 0;
    void **racers = list ? ptr(list, 32) : NULL;
    if (!racers || count < 0 || count > 64) return 0;
    float position[3]; world_position(position, entity);
    for (int i = 0; i < count; ++i) {
        unsigned char *other = racers[i];
        if (!other || other == racer || other[0x8c]) continue;
        /* FFA racers can share a faction; native enemy filtering is for teams. */
        if (settings && *settings && integer(*settings, 0x28) > 1 &&
            !native_enemy_racer(racer, other)) continue;
        void *target = race_handle((void **)(other + 0x20));
        if (!target || target == entity) continue;
        float where[3]; world_position(where, target);
        if (shot_in_cone(position, direction, where, limits[0], limits[1], limits[2])) return 1;
    }
    return 0;
}

struct StuckRecovery {
    void *owner;
    int lap, initialized, active, moving, below;
    uint32_t since, entered, moving_since, below_since;
    float anchor[3];
};
static struct StuckRecovery stuck_recovery[8];
static _Thread_local int native_recovery_context;

/* Simulation-clock deadlines, not render-frame counts. Hysteresis prevents
 * alternating controllers at a blocked shortcut or while still reversing. */
static int recover_stuck(unsigned id, void *owner, int lap, uint32_t now,
                         int driving, const float position[3], const float velocity[3], unsigned intent) {
    struct StuckRecovery *r = stuck_recovery + id;
    if (!r->initialized || r->owner != owner || r->lap != lap || !driving) {
        *r = (struct StuckRecovery){.owner=owner,.lap=lap,.initialized=1,.since=now};
        memcpy(r->anchor,position,sizeof r->anchor);
        return 0;
    }
    const float moved = hypotf(position[0]-r->anchor[0],position[1]-r->anchor[1]);
    const float speed = hypotf(velocity[0],velocity[1]);
    if (!r->active) {
        if (moved >= 100) { memcpy(r->anchor,position,sizeof r->anchor); r->since=now; }
        if (intent & TT_REFERENCE_BELOW_ROUTE) {
            if (!r->below) { r->below=1; r->below_since=now; }
        } else r->below=0;
        const int missed_landing=r->below && speed<300 && (uint32_t)(now-r->below_since)>=2048;
        if (missed_landing || ((uint32_t)(now-r->since) >= 8192 && speed < 100)) {
            r->active=1; r->entered=now; r->moving=0;
            printf("Native AI: stuck id=%u native_recovery=1 speed=%.1f moved=%.1f missed_landing=%d lap=%d position=%.1f,%.1f,%.1f\n",
                   id,speed,moved,missed_landing,lap,position[0],position[1],position[2]);
        }
    } else {
        if (speed > 300 && moved > 250 && (intent & TT_REFERENCE_ALIGNED)) {
            if (!r->moving) { r->moving=1; r->moving_since=now; }
            if ((uint32_t)(now-r->entered)>=16384 && (uint32_t)(now-r->moving_since)>=6144 && (intent & TT_REFERENCE_REJOIN_READY) && !(intent & TT_REFERENCE_BELOW_ROUTE)) {
                r->active=0; r->below=0; r->since=now; memcpy(r->anchor,position,sizeof r->anchor);
                printf("Native AI: stuck id=%u native_recovery=0 resumed_ghost=1 speed=%.1f lap=%d recovery_seconds=%.2f\n",
                       id,speed,lap,(uint32_t)(now-r->entered)/8192.f);
            }
        } else r->moving=0;
    }
    return r->active;
}

/* TT supplies terrain takeoff timing; native dcf1c0 owns slide-entry hops.
 * Never mix native route-hop requests with a different guided trajectory. */
static unsigned guide_hop(unsigned char *ai, unsigned intent) {
    ai[0x7d] = 0; /* A native spline hop cannot own a ghost route. */
    if (!(intent & TT_REFERENCE_ALIGNED)) return intent;
    if (intent & TT_REFERENCE_DRIFT_HOP) intent &= ~(2u | 512u);
    ai[0x7d] = (intent & 2) != 0;
    return intent;
}

/* Itemless can pass nearby racers on a straight instead of sharing one lane.
 * Keep the recorded line through bends and terrain jumps. */
static void avoid_reference_traffic(void *entity, unsigned id, const float position[3],
                                    float targets[2][3], unsigned intent) {
    if ((intent & (4u|2u|512u)) || !racer_manager || !*racer_manager || !world_position) return;
    float dx=targets[0][0]-position[0], dy=targets[0][1]-position[1];
    float fx=targets[1][0]-targets[0][0], fy=targets[1][1]-targets[0][1];
    float length=hypotf(dx,dy), far_length=hypotf(fx,fy);
    if (length<1 || far_length<1 || dx*fx+dy*fy<.96f*length*far_length) return;
    dx/=length; dy/=length;
    void *list=ptr(*racer_manager,0x10);
    int count=list ? integer(list,12) : 0;
    void **items=list ? ptr(list,0x20) : NULL;
    if (!items || count<0 || count>8) return;
    float offset=0;
    for (int i=0;i<count;++i) {
        unsigned char *other=items[i];
        if (!other || other[0x8c]) continue;
        void *other_entity=race_handle((void **)(other+0x20));
        if (!other_entity || other_entity==entity) continue;
        float p[3]; world_position(p,other_entity);
        float x=p[0]-position[0], y=p[1]-position[1];
        float ahead=x*dx+y*dy, side=y*dx-x*dy;
        if (ahead<=-250 || ahead>=900 || fabsf(side)>=240 || fabsf(p[2]-position[2])>120) continue;
        unsigned char *player=race_handle((void **)(other+0x10));
        int other_id=player ? integer(player,0xc8) : integer(other,0x164);
        if (other_id<0 || other_id>=8) continue;
        float weight=(ahead>=0 ? 1-ahead/900 : 1+ahead/250)*(1-fabsf(side)/240);
        offset+=(id<(unsigned)other_id ? -90.f : 90.f)*weight;
    }
    offset=fmaxf(-90,fminf(90,offset));
    for (int i=0;i<2;++i) { targets[i][0]-=dy*offset; targets[i][1]+=dx*offset; }
}

/* Native steering consumes AI+28/+34 after this tactical update. Keep its
 * controller, physics, checkpoints, collision recovery and input replication. */
static float shortcut_approach(const unsigned char *segment);

static void ABI race_targets(unsigned char *self, unsigned char *ai) {
    if (driving_level() < 0) { native_race_targets(self,ai); return; }
    const int saved_recovery_context = native_recovery_context;
    const int ghost=runtime_speed_race_active() && client && *client && integer(*client,0x144)==16;
    if (!ghost) memset(stuck_recovery,0,sizeof stuck_recovery);
    void *route_owner = ptr(self,0x18);
    unsigned char *route_racer=race_handle((void **)(self+0x28));
    for (unsigned i=0;i<8;++i) {
        if (stuck_recovery[i].owner!=route_owner) continue;
        if (route_racer && stuck_recovery[i].lap!=integer(route_racer,0x4c))
            memset(stuck_recovery+i,0,sizeof stuck_recovery[i]);
        if (stuck_recovery[i].active) native_recovery_context=1;
    }
    native_race_targets(self, ai);
    native_recovery_context = saved_recovery_context;
    reference_intent = 0; reference_slot=-1;
    const int level = driving_level();
    if (level < 0 || !client || !*client || !world_position) return;
    unsigned char *entity = ptr(self, 0x18);
    if (!entity || (integer(*client, 0x144) == 16 && !(entity[0x3b] & 1))) return;
    unsigned char *racer = race_handle((void **)(self + 0x28));
    if (!racer || racer[0x8c]) return;
    unsigned char *player = race_handle((void **)(racer + 0x10));
    const int id = player ? integer(player, 0xc8) : integer(racer, 0x164);
    if (id < 0 || id >= 8) return;
    observed_slot = id;
    racer_wumpa[id] = integer(racer, 0x9c);
    racer_perfect_chain[id] = integer(racer, 0x10c);
    racer_crates[id] = integer(racer, 0x104);
    float position[3], velocity[3], targets[2][3];
    world_position(position, entity);
    memcpy(velocity, entity + 0x60, sizeof velocity);
    memcpy(targets, ai + 0x28, sizeof targets);
    unsigned intent = 0;
    int guided = ghost &&
                 runtime_tt_reference_guide(ptr(*client, 0xc0), (uintptr_t)entity, (unsigned)id,
                               integer(racer, 0x4c), level, position, velocity, targets, &intent);
    racer_wumpa[id] = integer(racer, 0x9c);
    racer_perfect_chain[id] = integer(racer, 0x10c);
    void *timer = ptr(self,0x68);
    typedef uint32_t (ABI *SimulationNow)(void *);
    SimulationNow now_fn = timer ? (SimulationNow)ptr(ptr(timer,0),0xb0) : NULL;
    /* A slower airborne trajectory is not a failed landing. */
    if (intent & TT_REFERENCE_BELOW_ROUTE) {
        unsigned char *control=ptr(ai,0x48);
        unsigned char *kart=control ? race_handle((void **)(control+0x58)) : NULL;
        if (!kart || ptr(kart,0x18)!=entity || !kart[0x214]) intent &= ~TT_REFERENCE_BELOW_ROUTE;
    }
    if (guided && now_fn && recover_stuck((unsigned)id,entity,integer(racer,0x4c),now_fn(timer),
                                         ai[0x78] != 0,position,velocity,intent)) {
        box_claims[id]=NULL; box_kind[id]=-1;
        reference_slot=-1;
        return; /* Keep native targets, hop and reversing recovery intact. */
    }
    if (guided) ai[0x7b] = 0; /* Rejoin the ghost instead of reversing along a native spline. */
    int box = aim_at_box(ai, entity, (unsigned)id, level, position, targets,
                         intent, racer_wumpa[id]);
    /* A pickup approach temporarily owns steering; don't start a ghost slide
     * whose geometry assumes a different target. Native active slides are kept. */
    if (box) intent &= ~(1u | TT_REFERENCE_ALIGNED);
    if (!guided && !box) return;
    if (guided) avoid_reference_traffic(entity,(unsigned)id,position,targets,intent);
    memcpy(ai + 0x28, targets, sizeof targets);
    reference_native_hop = ai[0x7d];
    if (guided) intent = guide_hop(ai, intent);
    reference_intent = intent; reference_slot = guided ? id : -1;
    unsigned char *control = ptr(ai, 0x48);
    if (guided && level >= 3 && control) {
        control[0x50] = 1; control[0x51] = 1; /* Native drift/boost permission; outer call restores it. */
    }
    float ax = targets[0][0] - position[0], ay = targets[0][1] - position[1];
    float bx = targets[1][0] - targets[0][0], by = targets[1][1] - targets[0][1];
    const float denom = sqrtf((ax * ax + ay * ay) * (bx * bx + by * by));
    if (denom > 1) {
        const float curve = (ax * by - ay * bx) / denom;
        memcpy(ai + 0x40, &curve, sizeof curve);
    }
    /* guide_hop has selected the sole terrain-hop source for guided poses. */
    static unsigned reports;
    if (reports++ < 32) printf("Native AI: guidance id=%d lap=%d tt_reference=%d box=%d drift=%d jump=%d\n",
                              id, integer(racer, 0x4c), guided, box, !!(intent & 1), !!(intent & 2));
}

/* Bounded diagnostics compare decisions before/after the adapter. State and
 * action transitions plus periodic samples expose cancellations, not just intent. */
static void trace_ai_controls(void *self, unsigned char *kart, unsigned char *wire,
                              unsigned native_actions, float native_turn) {
    static struct { void *owner; unsigned tick, reports, actions, native_actions;
                    int state, boosts; } trace[8];
    const unsigned id = (unsigned)observed_slot;
    if (id >= 8) return;
    if (trace[id].owner != self) {
        memset(&trace[id], 0, sizeof trace[id]); trace[id].owner = self;
        trace[id].state = -1;
    }
    const int state = integer(kart, 0xe0), boosts = integer(kart, 0xec);
    const unsigned actions = wire[0x10] & 0x3f;
    const unsigned tick = trace[id].tick++;
    const int changed = trace[id].state != state || trace[id].boosts != boosts ||
        trace[id].actions != actions || trace[id].native_actions != (native_actions & 0x3f);
    trace[id].state=state; trace[id].boosts=boosts; trace[id].actions=actions;
    trace[id].native_actions=native_actions & 0x3f;
    if (!changed && tick % 60) return;
    if (trace[id].reports >= 600) return;
    ++trace[id].reports;
    unsigned char *control = ptr(self,0x48);
    printf("Native AI: control id=%u tick=%u state=%d grounded=%u native=%02x final=%02x "
           "turn_native=%.3f turn_final=%.3f intent=%x hop_native=%u hop_final=%u "
           "recovery=%u throttle_alt=%u coast_delay=%.3f slide_mode=%d box=%d box_kind=%d wumpa=%d perfect_chain=%d crates=%d guided=%d speed=%.3f boosts=%d reserve=%.3f ready97=%u entry=%d entry_angle=%.3f curve=%.3f continue=%d continue_angle=%.3f direction=%d complete=%d oversteer=%d reference_hold=%d\n",
           id,tick,state,kart[0x214],native_actions & 0x3f,actions,native_turn,real(wire,0xc),
           reference_intent,reference_native_hop,((unsigned char *)self)[0x7d],
           ((unsigned char *)self)[0x7b],((unsigned char *)self)[0x79],real(self,0x84),
           control ? integer(control,0x74) : -1,
           box_claims[id]!=NULL,box_kind[id],racer_wumpa[id],racer_perfect_chain[id],racer_crates[id],reference_slot >= 0,normalized_kart_speed ? normalized_kart_speed(kart) : 0,
           boosts,fire_remaining(kart),native_boost_ready ? native_boost_ready(kart,.97f) : 0,
           slide_decision.entry,slide_decision.entry_angle,slide_decision.curve,
           slide_decision.continuation,slide_decision.continue_angle,slide_decision.direction,
           slide_decision.complete,slide_decision.oversteer,slide_decision.reference_hold);
    if (trace[id].reports == 600) printf("Native AI: control trace limit id=%u\n",id);
}

/* Observe native recovery and pickup-only updates too; these were invisible in
 * the guided-only trace. Never adapt follower input or detached components. */
static void trace_native_controls(void *self) {
    if (observed_slot < 0) return;
    unsigned char *control = ptr(self, 0x48), *wire = ptr(self, 0x70);
    unsigned char *kart = control ? race_handle((void **)(control + 0x58)) : NULL;
    void *entity = ptr(self, 0x18);
    if (kart && wire && entity && ptr(kart, 0x18) == entity)
        trace_ai_controls(self, kart, wire, wire[0x10], real(wire, 0xc));
}

/* Ghost inputs describe the ghost's pose, not this kart's. Preserve native
 * steering/throttle/braking/item decisions; adapt only hop edges and boost timing. */
static void apply_tt_technique(void *self, void *input) {
    if (driving_level() < 3 || reference_slot < 0 || reference_slot >= 8 || !native_apply_ai_input) return;
    unsigned char *control = ptr(self, 0x48), *wire = ptr(self, 0x70);
    unsigned char *kart = control ? race_handle((void **)(control + 0x58)) : NULL;
    /* A resolved handle can outlive/detach from its entity. Native speed and
     * drift helpers dereference kart+18 without checking it. */
    void *entity = ptr(self, 0x18);
    if (!kart || !wire || !entity || ptr(kart, 0x18) != entity) {
        static unsigned skipped[8];
        if (skipped[reference_slot]++ < 2)
            printf("Native AI: technique skipped id=%d kart=%d wire=%d entity_match=%d\n",
                   reference_slot, kart != NULL, wire != NULL, kart && entity && ptr(kart, 0x18) == entity);
        return;
    }
    /* dcf1c0 marks a native slide with +74=1; dcf740 marks a hop with 2.
     * A recorded shoulder hold must not turn a hop or rejected slide into one. */
    const unsigned native_actions = wire[0x10];
    const float native_turn = real(wire, 0xc);
    const int native_slide = (wire[0x10] & 8) && integer(control, 0x74) == 1;
    int changed = 0;
    unsigned slot = (unsigned)reference_slot;
    if (tt_jumps[slot].owner != self) {
        tt_jumps[slot].owner = self; tt_jumps[slot].active = tt_jumps[slot].phase = 0;
        tt_jumps[slot].boost_reports = 0; tt_jumps[slot].boosts = integer(kart, 0xec);
    }
    const int boosts = integer(kart, 0xec);
    if (boosts > tt_jumps[slot].boosts && boosts <= 3 && tt_jumps[slot].boost_reports++ < 12)
        printf("Native AI: consumed slide boost id=%u count=%d fire_remaining=%.3f\n",
               slot, boosts, fire_remaining(kart));
    tt_jumps[slot].boosts = boosts;
    if (!(reference_intent & (2 | 512)) && tt_jumps[slot].phase != 3)
        tt_jumps[slot].active = tt_jumps[slot].phase = 0;
    const int terrain_hop = (reference_intent & (2 | 512)) || tt_jumps[slot].phase == 3;
    if (tt_jumps[slot].phase == 3) {
        /* Finish one release/press pair on the next tick, not another spatial
         * trigger. Cancel if airborne or no longer aligned with the reference. */
        if (kart[0x214] && (reference_intent & TT_REFERENCE_ALIGNED)) {
            wire[0x10] |= 8; changed = 1;
        }
        tt_jumps[slot].active = 1; tt_jumps[slot].phase = 0;
    }
    /* The native hop needs a rising edge. Release in the recorded short
     * pre-jump window, so the actual takeoff tick can press without a delay. */
    if ((reference_intent & 512) && !(reference_intent & 2) &&
        !tt_jumps[slot].active && kart[0x214]) {
        if (!tt_jumps[slot].phase && (reference_intent & TT_REFERENCE_BANK_BEFORE_HOP) &&
            native_slide && !(previous_ai_actions & 16) &&
            native_boost_ready && boost_decision(kart,.8f,1)) {
            wire[0x10] |= 24; /* Bank once, before the separate release frame. */
            tt_jumps[slot].phase = 4;
        } else {
            wire[0x10] &= (unsigned char)~24;
            tt_jumps[slot].phase = 1;
        }
        changed = 1;
    }
    if ((reference_intent & 2) && !tt_jumps[slot].active) {
        if (!kart[0x214]) {
            tt_jumps[slot].active = 1; tt_jumps[slot].phase = 0;
        } else if (!(previous_ai_actions & 8)) {
            wire[0x10] |= 8; tt_jumps[slot].active = 1; tt_jumps[slot].phase = 0; changed = 1;
        } else {
            wire[0x10] &= (unsigned char)~8; tt_jumps[slot].phase = 3; changed = 1;
        }
    }
    /* Native steering owns entry, landing and countersteer in the actual pose.
     * Ghost steering belongs to a different pose and must never replace it. */
    const int state = integer(kart, 0xe0);
    const float speed = native_slide && normalized_kart_speed ? normalized_kart_speed(kart) : 0;
    /* Native may release a slide to correct its heading. Bank a physically
     * valid charge once before that release, instead of throwing it away. */
    if (!native_slide && !terrain_hop && !tt_jumps[slot].phase &&
        (previous_ai_actions & 24) == 8 && integer(control,0x74)==1 &&
        (state==3 || state==4) && !(reference_intent & 16) &&
        native_drift_complete && !native_drift_complete(kart) &&
        native_drift_oversteer && !native_drift_oversteer(kart,1.f/30.f) &&
        bank_slide_boost(kart)) {
        wire[0x10] |= 24;
        changed=1;
    }
    const int drift = native_slide && !(reference_intent & 16) &&
        ((reference_intent & 384) || state >= 3) && isfinite(speed) && speed > real(control, 0x4c) &&
        state >= 0 && state <= 4;
    if (drift && !terrain_hop && !tt_jumps[slot].phase) {
        if (state >= 3 && ((native_drift_complete && native_drift_complete(kart)) ||
                          (native_drift_oversteer && native_drift_oversteer(kart, 1.f / 30.f)))) {
            /* Release an exhausted/oversteered slide before starting the next cycle. */
            wire[0x10] &= (unsigned char)~24;
        } else if (state >= 3 && native_boost_ready) {
            wire[0x10] &= (unsigned char)~16;
            if (boost_decision(kart, .8f, 1)) wire[0x10] |= 16;
        }
        changed = 1;
    }
    if (changed) {
        native_apply_ai_input(self, input);
        static unsigned boost_reports[8];
        if (drift && (wire[0x10] & 24) == 24 && boost_reports[slot]++ < 8)
            printf("Native AI: power slide id=%u state=%d normalized_speed=%.3f boosts=%d actions=%02x\n",
                   slot, state, speed, integer(kart, 0xec), wire[0x10] & 0x3f);
        static unsigned takeoff_reports[8];
        if ((reference_intent & 2) && takeoff_reports[slot]++ < 16)
            printf("Native AI: takeoff input id=%u state=%d grounded=%d prepared=%u actions=%02x\n",
                   slot, state, kart[0x214] != 0, tt_jumps[slot].phase, wire[0x10] & 0x3f);
    }
    trace_ai_controls(self, kart, wire, native_actions, native_turn);
}

/* Read the remaining duration of the fastest fire, using native reserve carry
 * arithmetic (e2e6b0). Never refill reserves or suppress boost cancellation. */
static float fire_remaining(void *controller) {
    void *entity = ptr(controller, 0x18);
    void *boost = entity && boost_type && *boost_type && component ? component(entity, *boost_type, 0) : NULL;
    void *list = boost ? ptr(boost, 0x28) : NULL;
    const int count = list ? integer(list, 12) : 0;
    void **items = list ? ptr(list, 0x20) : NULL;
    float fastest = -1, remaining = INFINITY;
    if (count < 0 || count > 256) return 0;
    for (int i = 0; items && i < count; ++i) {
        unsigned char *entry = items[i];
        void *data = entry ? ptr(entry, 0x10) : NULL;
        if (!data || entry[0x48]) continue;
        const float speed = real(data, 0xc);
        /* Maintained fire is not an infinite bank: predict what survives
         * leaving the pad, as native f4b770/e2e6b0 do. */
        float left = maintained_duration(entry) + real(data, 0x1c) + real(entry, 0x28) - real(entry, 0x18);
        if (isnan(left) || left <= 0 || !isfinite(speed)) continue;
        if (speed > fastest) { fastest = speed; remaining = left; }
        else if (speed == fastest && left > remaining) remaining = left;
    }
    return fastest >= 0 ? remaining : 0;
}

/* e32850 increments the count even for an empty boost. Only drift-data +24
 * gates actual turbo creation; +c4 on the kart is the separate Perfect rating. */
static unsigned char bank_slide_boost(void *controller) {
    unsigned char *archetype=race_handle((void **)((unsigned char *)controller+0xb8));
    void *data=archetype ? ptr(archetype,0xa8) : NULL;
    const float minimum=data ? real(data,0x24) : NAN;
    return native_boost_ready && isfinite(minimum) && minimum>=0 && minimum<1 &&
           native_boost_ready(controller,minimum);
}

static unsigned char boost_decision(void *controller, float threshold, int ai_call) {
    const int level = ai_call ? driving_level() : -1;
    if (level >= 0 && reference_slot >= 0) {
        unsigned char *archetype=race_handle((void **)((unsigned char *)controller+0xb8));
        unsigned char *data=archetype ? ptr(archetype,0xa8) : NULL;
        void *timer=ptr(controller,0xf8);
        const float maximum=data ? real(data,0x20) : 0;
        if (isfinite(maximum) && maximum>0 && maximum<10 && timer) {
            const float pattern=runtime_tt_reference_boost_interval((unsigned)reference_slot,
                                                                   (unsigned)integer(controller,0xec));
            float desired=pattern>0 ? pattern : threshold*maximum;
            if (((unsigned char *)timer)[0x14]) {
                typedef uint32_t (ABI *Now)(void *);
                Now now_fn=(Now)ptr(ptr(timer,0),0xb0);
                if (now_fn) {
                    const uint32_t now=((unsigned char *)timer)[0x15] ? (uint32_t)integer(timer,0x10) : now_fn(timer);
                    const float charged=(uint32_t)(now-(uint32_t)integer(timer,0xc))/8192.f;
                    const float reserve=fire_remaining(controller);
                    /* Keep the fastest fire alive; no requirement that this boost
                     * be perfect, and no synthetic reserve/speed refill. */
                    if (reserve>0 && isfinite(reserve))
                        desired=fminf(desired,charged+fmaxf(0,reserve-1.f/30.f));
                    if (reference_intent&(2u|512u)) desired=fminf(desired,charged);
                }
            }
            const float minimum=real(data,0x24);
            if (isfinite(minimum) && minimum>=0 && minimum<1)
                threshold=fmaxf(minimum,fminf(.98f,desired/maximum));
        }
    }
    return native_boost_ready(controller, threshold);
}

static unsigned char ABI boost_ready(void *controller, float threshold) {
    return boost_decision(controller, threshold, __builtin_return_address(0) == ai_boost_return);
}

static float line_target(void *list, unsigned char avoid_hazards, float target, int level) {
    const int count = list ? integer(list, 12) : 0;
    void **items = list ? ptr(list, 0x20) : NULL;
    if (level < 0 || level > 5 || count < 2 || count > 4096 || !items || !isfinite(target)) return target;
    float fastest = INFINITY;
    for (int i = 0; i < count; ++i) {
        void *entry = items[i];
        unsigned char *segment = entry ? ptr(entry, 0x10) : NULL;
        if (!segment || (avoid_hazards && segment[0x65])) continue;
        const float time = real(entry, 12);
        if (isfinite(time) && time > 0 && time < fastest) fastest = time;
    }
    if (!isfinite(fastest) || target <= fastest) return target;
    static const float preference[] = {.10f,.20f,.35f,.55f,.80f,1.0f};
    return target + (fastest - target) * preference[level];
}

static void *ABI choose_line(void *list, unsigned char avoid_hazards, float target) {
    /* Keep the native branch/checkpoint selection. Bias only its eligible
     * authored racing lines toward faster recorded traversal times. */
    if (runtime_speed_race_active() && !native_recovery_context && __builtin_return_address(0) == ai_line_return)
        target = line_target(list, avoid_hazards, target, driving_level());
    return native_choose_line(list, avoid_hazards, target);
}

/* dcfb10 is the AI-only drift-entry predicate, not a steering command.
 * Adjust its two angular gates only for the duration of the predicate call.
 * Native curve direction and minimum speed gates
 * remain authoritative, including the raised speed gate after getting stuck. */
static float drift_entry_angle(float angle, int level) {
    static const float sensitivity[] = {.90f, 1.0f, 1.15f, 1.35f, 1.60f, 1.90f};
    if (level < 0 || level > 5 || !isfinite(angle)) return angle;
    const float adjusted = angle * sensitivity[level];
    return isfinite(adjusted) ? adjusted : angle;
}

static unsigned char ABI start_drift(void *self, float angle) {
    const int level = driving_level();
    const float curve = real(self, 0x44);
    slide_decision.entry_angle = angle; slide_decision.curve = curve;
    /* A guided line needs its own slide windows, not the stock route's
     * opportunistic hop/slide starts while recovering or approaching a jump. */
    if (level >= 3 && reference_slot >= 0 &&
        (reference_intent & (1u | TT_REFERENCE_ALIGNED)) != (1u | TT_REFERENCE_ALIGNED)) {
        slide_decision.entry = -2;
        return 0;
    }
    /* The native heading test has the opposite sign to controller steering.
     * Do not restart a slide against the recorded turn just to correct its lane. */
    if (level >= 3 && reference_slot >= 0 &&
        (((reference_intent & 128) && angle > 0) ||
         ((reference_intent & 256) && angle < 0))) {
        slide_decision.entry = -3;
        return 0;
    }
    float entry = drift_entry_angle(angle, level);
    float adjusted_curve = drift_entry_angle(curve, level);
    /* The native gate tests BOTH target curvature and heading angle. Scaling
     * only the latter left shallow recorded powerslides permanently blocked. */
    if (level >= 3 && (reference_intent & 1) && curve * angle > 0 &&
        fabsf(curve) > .025f && fabsf(angle) > .025f) {
        entry = copysignf(fmaxf(fabsf(entry), .16f), entry);
        adjusted_curve = copysignf(fmaxf(fabsf(adjusted_curve), .16f), curve);
    }
    memcpy((unsigned char *)self + 0x44, &adjusted_curve, 4);
    const unsigned char accepted = native_start_drift(self, entry);
    memcpy((unsigned char *)self + 0x44, &curve, 4);
    slide_decision.entry = accepted;
    return accepted;
}

/* Finish the actual boost chain across short ghost button-release gaps.
 * Terrain takeoff, U-turn and physical slide limits remain gates. */
static unsigned char ABI continue_drift(void *self, int direction, float angle) {
    const int guided = driving_level() >= 3 && reference_slot >= 0 &&
        !(reference_intent & (2u | 16u | 512u));
    slide_decision.continue_angle = angle; slide_decision.direction = direction;
    unsigned char accepted = native_continue_drift(self, direction, angle);
    slide_decision.continuation = accepted;
    unsigned char *kart = race_handle((void **)((unsigned char *)self + 0x58));
    if (!accepted && kart && ptr(kart,0x18) && ptr(kart,0x18) == ptr(self,0x18)) {
        slide_decision.complete = native_drift_complete ? native_drift_complete(kart) : 1;
        slide_decision.oversteer = native_drift_oversteer ? native_drift_oversteer(kart,1.f/30.f) : 1;
    }
    if (!accepted && guided && isfinite(angle) && fabsf(angle) <= .35f &&
        (direction == 0 || direction == 1)) {
        /* Bridge small heading jitter only. Large route corrections need the
         * native release, otherwise the held slide fights the ghost target. */
        accepted = native_continue_drift(self, direction, 0);
        slide_decision.reference_hold = accepted;
    }
    return accepted;
}

/* Native e0efa0 balances current segment occupancy (+60), then route time.
 * Nightmare prioritizes authored shortcuts (+64), regardless of occupancy. */
static void *shortcut_branch(void *list, int level) {
    const int count = list ? integer(list, 12) : 0;
    void **items = list ? ptr(list, 32) : NULL;
    if (level < 2 || level > 5 || count < 2 || count > 64 || !items) return NULL;
    int least_occupied = 0x7fffffff;
    for (int i = 0; i < count; ++i) {
        unsigned char *segment = items[i];
        if (!segment || segment[0x65]) continue;
        const int occupancy = integer(segment, 0x60);
        if (occupancy >= 0 && occupancy < least_occupied) least_occupied = occupancy;
    }
    if (least_occupied == 0x7fffffff) return NULL;
    void *best = NULL, *shortcut = NULL;
    float best_time = INFINITY, native_time = INFINITY, shortcut_time = INFINITY;
    for (int i = 0; i < count; ++i) {
        unsigned char *segment = items[i];
        if (!segment || segment[0x65]) continue;
        const int occupancy = integer(segment, 0x60);
        if (occupancy < 0) continue;
        void *lines = ptr(segment, 0x28);
        const int n = lines ? integer(lines, 12) : 0;
        void **records = lines ? ptr(lines, 32) : NULL;
        if (n < 1 || n > 4096 || !records || !records[0]) continue;
        const float time = real(records[0], 12);
        if (!isfinite(time) || time <= 0) continue;
        if (level == 5 && segment[0x64] && time < shortcut_time) {
            shortcut = segment; shortcut_time = time;
        }
        if (level != 5 && (int64_t)occupancy - least_occupied > level - 1) continue;
        if (occupancy == least_occupied && time < native_time) native_time = time;
        if (time < best_time) { best = segment; best_time = time; }
    }
    if (shortcut) return shortcut;
    return isfinite(native_time) && best_time < native_time ? best : NULL;
}

static float route_time(const unsigned char *segment) {
    if (!segment || segment[0x65] || integer(segment, 0x60) < 0) return INFINITY;
    void *lines = ptr(segment, 0x28);
    const int count = lines ? integer(lines, 12) : 0;
    void **records = lines ? ptr(lines, 32) : NULL;
    if (count < 1 || count > 4096 || !records || !records[0]) return INFINITY;
    const float time = real(records[0], 12);
    return isfinite(time) && time > 0 ? time : INFINITY;
}

/* Crash Cove's jump approaches are ordinary splines. The shortcut flag is on
 * the following segment. Plan one connected transition ahead, using native
 * world-space endpoints, rather than steering sideways only after the jump. */
static float shortcut_approach(const unsigned char *segment) {
    void *transition = ptr(segment, 0x18);
    void *next = transition ? ptr(transition, 0x48) : NULL;
    const int count = next ? integer(next, 12) : 0;
    void **items = next ? ptr(next, 32) : NULL;
    float closest = INFINITY;
    if (count < 1 || count > 64 || !items) return closest;
    for (int i = 0; i < count; ++i) {
        unsigned char *target = items[i];
        if (!target || !target[0x64] || !isfinite(route_time(target))) continue;
        float distance = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const float delta = real(segment, 0x48 + axis * 4) - real(target, 0x3c + axis * 4);
            distance += delta * delta;
        }
        if (isfinite(distance) && distance < closest) closest = distance;
    }
    return closest;
}

/* Only choose among authored connected routes. Spread comparable lanes using
 * live occupancy and an AI/transition-specific tie break, never per-frame noise.
 * A sole Nightmare shortcut remains shared: do not invent an off-track lane. */
static void *varied_branch(void *list, int level, uintptr_t identity) {
    const int count = list ? integer(list, 12) : 0;
    void **items = list ? ptr(list, 32) : NULL;
    if (level < 0 || level > 5 || count < 2 || count > 64 || !items) return NULL;
    unsigned char *preferred = shortcut_branch(list, level);
    float fastest = INFINITY, approach = INFINITY;
    int direct = 0;
    for (int i = 0; i < count; ++i) {
        unsigned char *segment = items[i];
        const float time = route_time(segment);
        if (!isfinite(time)) continue;
        if (time < fastest) fastest = time;
        if (level == 5) {
            if (segment[0x64]) direct = 1;
            const float gap = shortcut_approach(segment);
            if (gap < approach) approach = gap;
        }
    }
    if (!isfinite(fastest)) return NULL;
    if (preferred) fastest = route_time(preferred);
    static const float allowance[] = {.20f, .16f, .12f, .10f, .08f, .05f};
    unsigned char *best = NULL;
    int occupancy = 0x7fffffff;
    uintptr_t tie = UINTPTR_MAX;
    for (int i = 0; i < count; ++i) {
        unsigned char *segment = items[i];
        const float time = route_time(segment);
        if (!isfinite(time)) continue;
        if (level == 5 && direct) {
            if (!segment[0x64] || time > fastest * (1.0f + allowance[level])) continue;
        } else if (level == 5 && isfinite(approach)) {
            /* Equivalent approach endpoints may use different authored lanes. */
            if (shortcut_approach(segment) > approach + .01f) continue;
        } else if (time > fastest * (1.0f + allowance[level])) continue;
        const int used = integer(segment, 0x60);
        uintptr_t rank = identity ^ ((uintptr_t)(unsigned)i * UINT64_C(0x85ebca6b));
        /* World endpoints distinguish transitions without memory addresses. */
        rank ^= (uint32_t)integer(segment, 0x48);
        rank ^= (uintptr_t)(uint32_t)integer(segment, 0x50) << 16;
        rank ^= rank >> 16; rank *= UINT64_C(0x9e3779b97f4a7c15); rank ^= rank >> 29;
        if (used < occupancy || (used == occupancy && rank < tie)) {
            best = segment; occupancy = used; tie = rank;
        }
    }
    return best;
}

static void *choose_branch_impl(void *self, void **list, unsigned char lap, unsigned char force) {
    if (!custom_ai_allowed() || !runtime_speed_race_active() || native_recovery_context) return native_choose_branch(self, list, lap, force);
    const int level = driving_level();
    const uintptr_t identity = route_identity(self);
    void *selected = list ? varied_branch(*list, level, identity) : NULL;
    if (!selected) return native_choose_branch(self, list, lap, force);
    /* Native only reads this list during the call. Records stored on the AI
     * remain the original segment's records, not pointers to this stack view. */
    unsigned char view[40];
    memcpy(view, *list, sizeof view);
    put_int(view, 12, 1); put_int(view, 16, 1); put_int(view, 24, 8);
    put_ptr(view, 32, &selected);
    void *filtered = view;
    void *actual = native_choose_branch(self, &filtered, lap, force);
    static atomic_uint route_logs;
    if (atomic_fetch_add_explicit(&route_logs, 1, memory_order_relaxed) < 48)
        printf("Native AI: route level=%d candidates=%d shortcut=%d approach=%d accepted=%d\n",
               level, integer(*list, 12), ((unsigned char *)selected)[0x64] != 0,
               level == 5 && isfinite(shortcut_approach(selected)), actual == selected);
    return actual;
}

static void *ABI choose_branch(void *self, void **list, unsigned char lap, unsigned char force) {
    unsigned char *entity = self ? ptr(self, 0x18) : NULL;
    const int saved_peer = peer_decision;
    if (client && *client && integer(*client, 0x144) == 16 && entity && !(entity[0x3b] & 1))
        peer_decision = 1;
    void *result = choose_branch_impl(self, list, lap, force);
    peer_decision = saved_peer;
    return result;
}

/* Native dcb6d0 skips item execution whenever driving +48 holds hop/drift.
 * The decision has already passed inventory, roulette, grounded and kart-state
 * checks. Preserve the slide and put its independent item press on the wire. */
static void apply_item_during_slide(void *self, void *input) {
    unsigned char *control = ptr(self, 0x48), *use = ptr(self, 0x58), *wire = ptr(self, 0x70);
    const int level = driving_level();
    if (level < 0 || level > 5 || runtime_speed_race_active() || !manager || !*manager ||
        (integer(*manager, 0xd4c) != 1 && integer(*manager, 0xd4c) != 6) ||
        !control || !control[0x48] || !use || pending_item_use != use || !use[0x30] || !wire || (wire[0x10] & 32)) return;
    /* A held item button needs a release frame before another press. */
    if (previous_ai_actions & 32) return;
    wire[0x10] |= 32;
    native_apply_ai_input(self, input);
    static unsigned reports;
    if (reports++ < 96) printf("Native AI: item press during slide level=%d actions=%02x\n", level, wire[0x10]);
}

typedef unsigned char (ABI *ItemDecision)(unsigned char *, unsigned char *, int);
static ItemDecision native_item_decision;
struct ItemHold { void *owner, *item; uint32_t since; };
static struct ItemHold item_holds[8];

/* Called only after native inventory, roulette, grounded and kart-state checks.
 * Keep tactical opportunities, but do not hoard any item category indefinitely. */
static unsigned char ABI item_decision(unsigned char *self, unsigned char *racer, int category) {
    unsigned char result = native_item_decision(self, racer, category);
    const int level = driving_level();
    unsigned char *entity = ptr(self, 0x18);
    if (client && *client && integer(*client,0x144)==16 && !runtime_private_match_active()) return result;
    if (level < 0 || level > 5 || !racer || racer[0x8c] || runtime_speed_race_active() ||
        !manager || !*manager || (integer(*manager, 0xd4c) != 1 && integer(*manager, 0xd4c) != 6) ||
        !entity || (integer(*client, 0x144) == 16 && !(entity[0x3b] & 1))) return result;
    unsigned char *player = race_handle((void **)(racer + 0x10));
    const int id = player ? integer(player, 0xc8) : integer(racer, 0x164);
    if (id < 0 || id >= 8) return result;
    unsigned char *controller = race_handle((void **)(self + 0x28));
    void *item = controller ? race_handle((void **)(controller + 0x30)) : NULL;
    void *timer = ptr(self, 0x38);
    if (!item || !timer) { item_holds[id] = (struct ItemHold){0}; return result; }
    typedef uint32_t (ABI *TimerNow)(void *);
    TimerNow now_fn = (TimerNow)ptr(ptr(timer, 0), 0xb0);
    if (!now_fn) return result;
    const uint32_t now = now_fn(timer);
    struct ItemHold *hold = item_holds + id;
    if (hold->owner != self || hold->item != item)
        *hold = (struct ItemHold){self, item, now};
    static const float reaction[] = {.6f, .45f, .3f, .2f, .15f, .1f};
    static const float maximum[] = {6.f, 4.f, 1.5f, .9f, .6f, .3f};
    const float elapsed = (uint32_t)(now - hold->since) / 8192.f;
    if (elapsed < reaction[level]) return 0;
    if (!result && elapsed < maximum[level]) return 0;
    *hold = (struct ItemHold){0};
    pending_item_use = self;
    printf("Native AI: use item id=%d level=%d category=%d held=%.2f native=%d\n",
           id, level, category, elapsed, result != 0);
    return 1;
}

/* Profiles refine supported Hard parameters; native difficulty stays in 0..2.
 * In particular, Adventure boss data never receives the incomplete index 3. */
static const char *const profile_names[] = {"Very Hard", "Extreme", "Nightmare"};
struct AiTuning { size_t offset; int integral; float scale[3]; float ceiling; };
static const struct AiTuning tuning[] = {
    {0x28, 0, {0.80f, 0.60f, 0.40f}, 0}, /* Racer target lock delay. */
    {0x48, 0, {0.80f, 0.60f, 0.40f}, 0}, /* Battle target lock delay. */
    {0x50, 0, {0.80f, 0.60f, 0.45f}, 0}, /* Battle attack interval. */
    {0x78, 0, {0.80f, 0.60f, 0.40f}, 0}, /* Cap on hurt catch-up compensation. */
    {0x80, 1, {2.00f, 2.34f, 2.67f}, 8}, /* Racers eligible for a successful start boost. */
    {0x88, 0, {1.15f, 1.29f, 1.38f}, 97}, /* Start boost success probability. */
    {0x90, 0, {0.72f, 0.40f, 0.16f}, 0}, /* Start boost late-failure probability. */
    {0x98, 1, {1.20f, 1.40f, 1.60f}, 8}, /* Racers allowed to avoid hazards. */
    {0xc0, 0, {0.80f, 0.60f, 0.40f}, 0}, /* Interrupt item roulette sooner. */
};
static void *baseline_root;
static void *baseline_objects[sizeof tuning / sizeof tuning[0]];
static unsigned baseline_bits[sizeof tuning / sizeof tuning[0]][3];
static int applied_driving = -1;

/* Custom tuning belongs to Arcade/Private Single and Cup. Other modes restore
 * every original column, including public matchmaking after a private race. */
static const float start_boost_chance[] = {.40f, .60f, .80f, .87f, .93f, .97f};
static const float start_late_failure[] = {.40f, .05f, .12f, .08f, .04f, .01f};
static const int driving_hazard_racers[] = {2, 4, 6, 7, 8, 8};
static const int start_boost_racers[] = {2, 3, 5, 6, 7, 8};

static void tune_ai(void *root) {
    if (!root) return;
    const int driving = custom_ai_allowed() ? (runtime_speed_race_active() ? 2 : 1) : 0;
    const int active = driving && difficulty_manager && *difficulty_manager && native_difficulty(*difficulty_manager) == 2 ? effective_profile() : 0;
    while (atomic_flag_test_and_set_explicit(&tuning_lock, memory_order_acquire)) { }
    if (baseline_root == root && applied_profile == active && applied_driving == driving) {
        atomic_flag_clear_explicit(&tuning_lock, memory_order_release);
        return;
    }
    const int changed_root = baseline_root != root;
    for (size_t i = 0; i < sizeof tuning / sizeof tuning[0]; ++i) {
        unsigned char *value = ptr(root, tuning[i].offset);
        if (!value) continue;
        if (changed_root || baseline_objects[i] != value) {
            baseline_objects[i] = value;
            memcpy(baseline_bits[i], value + 0xc, 12);
        }
        for (int column = 0; column < 3; ++column) {
            unsigned bits = baseline_bits[i][column];
            if (column == 2 && active >= 1 && active <= 3) {
                float original;
                if (tuning[i].integral) {
                    int n; memcpy(&n, &bits, 4); original = (float)n;
                } else memcpy(&original, &bits, 4);
                float result = original * tuning[i].scale[active - 1];
                if (tuning[i].ceiling && result > tuning[i].ceiling) result = tuning[i].ceiling;
                if (tuning[i].integral) { int n = (int)(result + 0.5f); memcpy(&bits, &n, 4); }
                else memcpy(&bits, &result, 4);
            }
            if (driving && (driving == 1 || column == 2)) {
                const int level = column == 2 ? 2 + active : column;
                if (tuning[i].offset == 0x80) memcpy(&bits, &start_boost_racers[level], 4);
                if (tuning[i].offset == 0x88) memcpy(&bits, &start_boost_chance[level], 4);
                if (tuning[i].offset == 0x90) memcpy(&bits, &start_late_failure[level], 4);
                if (tuning[i].offset == 0x98) memcpy(&bits, &driving_hazard_racers[level], 4);
                if (tuning[i].offset == 0x78) {
                    float cap; memcpy(&cap, &baseline_bits[i][column], 4);
                    cap *= .8f; memcpy(&bits, &cap, 4);
                }
            }
            memcpy(value + 0xc + column * 4, &bits, 4);
        }
    }
    baseline_root = root;
    applied_profile = active;
    applied_driving = driving;
    atomic_flag_clear_explicit(&tuning_lock, memory_order_release);
}

static void *ABI get_ai_data(void) {
    void *root = *ai_data;
    if (!root) root = cold_ai_data();
    tune_ai(root);
    return root;
}

static void ABI difficulty_event(unsigned char *self, unsigned char *event, void *owner) {
    /* Remove appended choices when leaving the supported race menus. */
    if (!custom_ai_menu_allowed() || !self[0x1b9]) {
        const int count=3+(self[0x1b9]!=0);
        void *options=ptr(self,0x1a8), *available=ptr(self,0x1b0);
        if (options && available && integer(options,12)==count+3 && integer(available,12)==count+3) {
            void **names=ptr(options,0x20);
            for (int i=count; names && i<count+3; ++i) {
                if (names[i]) drop_string(*pool,(char *)names[i]-16);
                names[i]=NULL;
            }
            put_int(options,12,count); put_int(available,12,count);
            if (integer(self,0x190)>=count) put_int(self,0x190,count-1);
            if (integer(self,0x1a0)>=count) put_int(self,0x1a0,count-1);
        }
        Type event_type=(Type)ptr(ptr(event,0),0);
        const int selected=event_type(event)==*changed_type ? integer(event,0x18) : 0;
        const int remap=selected>=count;
        if (remap) put_int(event,0x18,count-1);
        difficulty_event_original(self, event, owner);
        if (remap) put_int(event,0x18,selected);
        if (ai_data && *ai_data) tune_ai(*ai_data);
        return;
    }
    Type type = (Type)ptr(ptr(event, 0), 0);
    const int no_ai = self[0x1b9] != 0;
    const int changing = type(event) == *changed_type;
    if (changing && client && *client && integer(*client, 0x144) == 16 &&
        (!net_context || !owns_object || !difficulty_manager || !*difficulty_manager ||
         !owns_object(net_context(), *difficulty_manager))) return;
    const int selected = changing ? integer(event, 0x18) : 0;
    if (changing) {
        const int level = selected - no_ai;
        ai_profile = level >= 3 && level <= 5 ? level - 2 : 0;
        followed_ai_profile = followed_ai_base = -1;
        if (ai_profile) put_int(event, 0x18, 2 + no_ai);
    }
    difficulty_event_original(self, event, owner);
    if (changing) put_int(event, 0x18, selected);
    void *options = ptr(self, 0x1a8), *available = ptr(self, 0x1b0);
    if (!options || !available) return;
    int added = 0;
    if (integer(options, 12) == 3 + no_ai) {
        for (size_t i = 0; i < 3; ++i) {
            char *label = intern_string(*pool, profile_names[i]);
            append_choice(self, &label, owner);
            if (label) drop_string(*pool, label - 16);
        }
        added = 1;
    }
    if (integer(options, 12) != 6 + no_ai || integer(available, 12) != 6 + no_ai) return;
    unsigned char *enabled = ptr(available, 0x20);
    if (enabled) for (int i = 3 + no_ai; i < 6 + no_ai; ++i) enabled[i] = enabled[2 + no_ai];
    /* +190 is the initialization index, +1a0 is the live cursor. Native
     * navigation owns the live cursor; reinitializing it after each choice
     * snaps it back to the stale initialization index and replays animations. */
    if (changing) put_int(self, 0x190, selected);
    else if (added && ai_profile && *difficulty_manager && native_difficulty(*difficulty_manager) == 2)
        put_int(self, 0x190, ai_profile + 2 + no_ai);
    if (changing) {
        tune_ai(*ai_data);
        printf("Native AI: profile=%s native difficulty=%d\n", ai_profile ? profile_names[ai_profile - 1] : "native",
               *difficulty_manager ? native_difficulty(*difficulty_manager) : -1);
    }
    if (!refreshing && added && ptr(self, 0x198)) {
        refreshing = 1; refresh(self); refreshing = 0;
    }
}

static void jump(unsigned char *at, const void *target) {
    memcpy(at, "\xff\x25\0\0\0\0", 6);
    memcpy(at + 6, &target, sizeof target);
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

int runtime_race_options_start(unsigned char *image, size_t size) {
    if (size < 0x2af2608) return 0;
    const uintptr_t team_native = (uintptr_t)ptr(image, 0x2778150);
    /* Online may already wrap this slot; detect the region from the native code. */
    const unsigned char signature[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56};
    size_t shift = memcmp(image + 0x1293030, signature, sizeof signature) ? 0x10 : 0;
    if (memcmp(image + 0x1293030 - shift, signature, sizeof signature)) return 0;
    if (ptr(image, 0x2770de0) != image + 0x125fb30 - shift) return 0;
    /* Native initialization binds driving (+48) and item use (+58) separately. */
    if (memcmp(image + 0xdcaea8, "\x48\x8b\x35\x21\x8b\xcf\x01", 7) ||
        memcmp(image + 0xdcaebb, "\x49\x89\x47\x48", 4) ||
        memcmp(image + 0xdcaf00, "\x48\x8b\x35\x31\x8b\xcf\x01", 7) ||
        memcmp(image + 0xdcaf15, "\x49\x89\x47\x58", 4)) return 0;
    const unsigned char difficulty_prefix[] = {0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0xe8,0x72,0x80,0x7d,0xff};
    if (memcmp(image + 0xc0a3b0, difficulty_prefix, sizeof difficulty_prefix)) return 0;
    const unsigned char ai_getter[] = {0x48,0x8b,0x05,0x61,0xa0,0xe6,0x01,0x48,0x85,0xc0,0x74,0x01,0xc3,0x55};
    if (memcmp(image + 0xc59b10, ai_getter, sizeof ai_getter)) return 0;
    const unsigned char boost_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x10};
    if (memcmp(image + 0xf4c840 - shift, boost_prefix, sizeof boost_prefix)) return 0;
    int32_t boost_call;
    memcpy(&boost_call, image + 0xdcf478, 4);
    if (image[0xdcf477] != 0xe8 || (int64_t)0xdcf47c + boost_call != (int64_t)(0xf4c840 - shift)) return 0;
    const unsigned char line_prefix[] = {0x4c,0x63,0x47,0x0c,0x49,0x83,0xf8,0x01,0x0f,0x8e,0x21,0x01,0,0};
    const unsigned char line_call[] = {0xe8,0x50,0x07,0,0};
    if (memcmp(image + 0xe0f9b0, line_prefix, sizeof line_prefix) ||
        memcmp(image + 0xe0f25b, line_call, sizeof line_call)) return 0;
    const unsigned char drift_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x83,0xec,0x10,0x48,0x89,0xfb};
    if (memcmp(image + 0xdcfb10, drift_prefix, sizeof drift_prefix) ||
        memcmp(image + 0xdcfc10, drift_prefix, sizeof drift_prefix)) return 0;
    const unsigned char input_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,
        0x48,0x89,0xfb,0x49,0x89,0xf6,0x48,0x8b,0x43,0x18};
    if (memcmp(image + 0xdcb630, input_prefix, sizeof input_prefix)) return 0;
    const unsigned char item_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x98,0,0,0};
    if (memcmp(image + 0xdcd930, item_prefix, sizeof item_prefix)) return 0;
    const unsigned char scan_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x28,0x01,0,0};
    if (memcmp(image + 0xdcdd30, scan_prefix, sizeof scan_prefix)) return 0;
    const unsigned char targets_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x88,0,0,0};
    if (memcmp(image + 0xdd0210, targets_prefix, sizeof targets_prefix)) return 0;
    const unsigned char branch_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28};
    if (memcmp(image + 0xe0efa0, branch_prefix, sizeof branch_prefix)) return 0;
    const unsigned char modifier_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    const unsigned char speed_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28};
    if (memcmp(image+0xe452c0,modifier_prefix,sizeof modifier_prefix) ||
        memcmp(image+0xe4a220,speed_prefix,sizeof speed_prefix)) return 0;
#ifdef _WIN32
    unsigned char *cold = VirtualAlloc(NULL, 512, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *cold = mmap(NULL, 512, PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cold == MAP_FAILED) cold = NULL;
#endif
    if (!cold || !writable(image+0xe452c0,17) || !writable(image+0xe4a220,17) || !writable(image + 0xdcd930, 20) || !writable(image + 0xc0a3b0, 14) || !writable(image + 0xdcdd30, 20) || !writable(image + 0xdd0210, 20) || !writable(image + 0xc59b10, 14) || !writable(image + 0x2770de0, sizeof(void *)) ||
        !writable(image + 0xf4c840 - shift, 14) || !writable(image + 0xe0f9b0, 14) ||
        !writable(image + 0xdcfc10, 14) || !writable(image + 0xdcb630, 17) || !writable(image + 0xdcfb10, 14) || !writable(image + 0xe0efa0, 14)) return 0;
    cold[0] = 0x55; /* Replaced native cold-path push rbp; rest stays in place. */
    jump(cold + 1, image + 0xc59b1e);
    cold_ai_data = (DataGetter)cold;
    memcpy(cold + 32, boost_prefix, sizeof boost_prefix);
    jump(cold + 32 + sizeof boost_prefix, image + 0xf4c840 - shift + sizeof boost_prefix);
    native_boost_ready = (BoostReady)(cold + 32);
    component = (Component)(image + 0x3dbf90);
    maintained_duration = (BoostDuration)(image + 0xe2f050);
    normalized_kart_speed = (BoostDuration)(image + 0xf4afc0 - shift);
    native_drift_complete = (DriftComplete)(image + 0xf4c520 - shift);
    native_drift_oversteer = (BoostReady)(image + 0xf4c780 - shift);
    boost_type = (void **)(image + 0x2ac4018);
    ai_boost_return = image + 0xdcf47c;
    memcpy(cold + 64, line_prefix, 8);
    cold[72] = 0x7f; cold[73] = 14; /* Relocate native jle without clobbering registers. */
    jump(cold + 74, image + 0xe0fadf);
    jump(cold + 88, image + 0xe0f9be);
    native_choose_line = (ChooseLine)(cold + 64);
    ai_line_return = image + 0xe0f260;
    memcpy(cold + 112, drift_prefix, sizeof drift_prefix);
    jump(cold + 126, image + 0xdcfb1e);
    native_start_drift = (StartDrift)(cold + 112);
    memcpy(cold + 352, drift_prefix, sizeof drift_prefix);
    jump(cold + 366, image + 0xdcfc1e);
    native_continue_drift = (ContinueDrift)(cold + 352);
    memcpy(cold + 160, branch_prefix, sizeof branch_prefix);
    jump(cold + 177, image + 0xe0efb1);
    native_choose_branch = (ChooseBranch)(cold + 160);
    memcpy(cold + 208, input_prefix, sizeof input_prefix);
    jump(cold + 225, image + 0xdcb641);
    native_ai_input = (AiInput)(cold + 208);
    native_apply_ai_input = (AiInput)(image + 0xdcb8e0);
    memcpy(cold + 256, targets_prefix, sizeof targets_prefix);
    jump(cold + 276, image + 0xdd0224);
    native_race_targets = (RaceTargets)(cold + 256);
    memcpy(cold + 304, scan_prefix, sizeof scan_prefix);
    jump(cold + 324, image + 0xdcdd44);
    native_scan_targets = (ScanTargets)(cold + 304);
    memcpy(cold + 400, item_prefix, sizeof item_prefix);
    jump(cold + 420, image + 0xdcd944);
    native_item_decision = (ItemDecision)(cold + 400);
    memcpy(cold+448,modifier_prefix,sizeof modifier_prefix);
    jump(cold+465,image+0xe452c0+sizeof modifier_prefix);
    native_speed_modifiers=(SpeedModifiers)(cold+448);
    memcpy(cold+480,speed_prefix,sizeof speed_prefix);
    jump(cold+497,image+0xe4a220+sizeof speed_prefix);
    native_kart_speed=(KartSpeed)(cold+480);
    native_enemy_racer = (EnemyRacer)(image + 0xdc9db0);
    collectible_manager = (void **)(image + 0x2acdc40);
    power_use_type = (void **)(image + 0x2ac3a38);
    racer_manager = (void **)(image + 0x2ad4b70);
    world_position = (WorldPosition)(image + 0x3dc720);
    race_resolve_handle = (ResolveHandle)(image + 0x149fca0 - shift);
    difficulty_event_original = (Event)ptr(image, 0x2770de0);
    intern_string = (InternString)(image + 0x1574080 - shift);
    append_choice = (AppendChoice)(image + 0x125e750 - shift);
    native_difficulty = room_difficulty;
    net_context = (NetContext)(image + 0x3e2430);
    owns_object = (OwnsObject)(image + 0x3ee590);
    difficulty_manager = (void **)(image + 0x2ab7ee0);
    ai_data = (void **)(image + 0x2ac3b78);
    client = (void **)(image + 0x2ab3cd8);
    manager = (void **)(image + 0x2acde48);
    settings = (void **)(image + 0x2ad4e30);
    pool = (void **)(image + 0x2af2600);
    changed_type = (void **)(image + 0x2ae4a38);
    drop_string = (DropString)(image + 0x1518e30 - shift);
    refresh = (Refresh)(image + 0x125da80 - shift);
    team_event_original = (Event)team_native;
    if (!writable(image + 0x2778150, sizeof(void *))) return 0;
    put_ptr(image, 0x2778150, team_event);
    put_ptr(image, 0x2770de0, difficulty_event);
    jump(image + 0xc59b10, get_ai_data);
    jump(image + 0xf4c840 - shift, boost_ready);
    jump(image + 0xe0f9b0, choose_line);
    jump(image + 0xc0a3b0, room_difficulty);
    jump(image + 0xdcfb10, start_drift);
    jump(image + 0xdcfc10, continue_drift);
    jump(image + 0xe0efa0, choose_branch);
    jump(image + 0xdcb630, ai_input);
    jump(image + 0xdd0210, race_targets);
    jump(image + 0xdcdd30, scan_targets);
    jump(image + 0xdcd930, item_decision);
    jump(image+0xe452c0,speed_modifiers);
    jump(image+0xe4a220,kart_speed);
    printf("Native AI: loaded %d TT track references\n",
           runtime_tt_reference_load(getenv("BB_TT_REFERENCE")));
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    puts("Native race options: Private Single No Teams / 2 / 4; Very Hard / Extreme / Nightmare AI profiles");
    return 1;
}
