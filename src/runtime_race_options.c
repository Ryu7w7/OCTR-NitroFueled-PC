/* Shared native race options, including offline menus. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_tt_reference.h"
#include "runtime_ai_catchup.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>
#include <float.h>
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
    const char *name = ptr(self, 0x148);
    if (runtime_text_is(name, "COLLISION")) {
        team_event_original(self, event, owner);
        return;
    }
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
static atomic_uint acceleration_epoch;
static void update_ai_catchup(void *self);
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
    runtime_ai_catchup_reset(NULL,NULL,0);
}

void runtime_race_options_map_loading(const char *map) {
    if (!map) return;
    atomic_fetch_add_explicit(&acceleration_epoch,1,memory_order_relaxed);
    reset_ai_physics();
    const int lobby = !strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby");
    if (!lobby) {
        char host[24]; runtime_room_mode_host_name(host);
        runtime_ai_catchup_reset(map,host,!runtime_room_mode_following());
    }
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

static void **timestep_clock;
static float timestep_frame_dt(void) {
    return timestep_clock && *timestep_clock ? real(*timestep_clock,0x18) : 0;
}
static float timestep_frames(void) {
    const float dt=timestep_frame_dt();
    /* The native 8192 Hz clock alternates 273/274 ticks at nominal 30 FPS. */
    return dt>0 && dt<273.f/8192 ? dt*30.f : 1;
}

static int configured_driving_level(void) {
    if (!custom_ai_allowed() || !difficulty_manager || !*difficulty_manager) return -1;
    const int level = native_difficulty(*difficulty_manager);
    const int profile = effective_profile();
    return level >= 0 && level <= 2 ? level + (level == 2 && profile >= 1 && profile <= 3 ? profile : 0) : -1;
}

static int selected_driving_level(void) {
    const int level=configured_driving_level();
    return runtime_speed_race_active() && level<2 ? -1 : level;
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
    if (online && runtime_room_mode_following() && wire && (!owner || runtime_ai_catchup_level()>=0)) {
        const int canonical=runtime_ai_catchup_level();
        const int profile = canonical>=0 ? (canonical>2 ? canonical-2 : 0) : wire[0x10] >> 6;
        const int base = canonical>=0 ? (canonical>2 ? 2 : canonical) : profile ? 2 : difficulty_manager && *difficulty_manager ?
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
    update_ai_catchup(self);
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

static struct { void *kart, *entity; unsigned id; int identity_valid; float catchup, air_floor, approach_floor, jump_reference; int fire, airborne, motion_clock_valid; uint32_t motion_tick; uint64_t updated, next_report, air_until, motion_updated, jump_report, pad_report, exhaust_report; } ai_physics[8];
typedef unsigned char (ABI *RacerAhead)(void *, void *);
static RacerAhead catchup_racer_ahead;
typedef float (ABI *KartSpeed)(void *, void *, int, float, float);
typedef void (ABI *SpeedModifiers)(void *, float);
static KartSpeed native_kart_speed;
static _Thread_local float kart_motion_dt, kart_motion_speed_scale=1;
static SpeedModifiers native_speed_modifiers;
static BoostDuration native_jump_impulse;
static BoostDuration native_trick_impulse;
typedef void (ABI *KartMotion)(void *, void *, void *);
static KartMotion native_kart_motion;
typedef void (ABI *SetVelocity)(void *, const float *);
static SetVelocity native_set_velocity;

static void reset_ai_physics(void) {
    memset(ai_physics,0,sizeof ai_physics);
    runtime_ai_catchup_reset(NULL,NULL,0);
}
/* An AI component can also drive a human kart. Require a current racer with
 * no player handle; an unresolved human handle is still a human. */
static int live_ai_id(void *entity) {
    void *list=racer_manager && *racer_manager ? ptr(*racer_manager,0x10) : NULL;
    int count=list ? integer(list,12) : 0;
    void **racers=list ? ptr(list,0x20) : NULL;
    if (!entity || !racers || count<1 || count>8) return -1;
    for (int i=0;i<count;++i) {
        unsigned char *racer=racers[i];
        if (!racer || race_handle((void **)(racer+0x20))!=entity) continue;
        if (ptr(racer,0x10)) return -1;
        int id=integer(racer,0x164);
        return id>=0 && id<8 ? id : -1;
    }
    return -1;
}
static void register_ai_physics(void *self) {
    unsigned char *control=ptr(self,0x48);
    void *kart=control ? race_handle((void **)(control+0x58)) : NULL;
    void *entity=ptr(self,0x18);
    if (!kart || !entity || ptr(kart,0x18)!=entity) return;
    const int id=live_ai_id(entity);
    unsigned free_slot=8;
    for (unsigned i=0;i<8;++i) {
        if (ai_physics[i].kart==kart || ai_physics[i].entity==entity) { free_slot=i; break; }
        if (!ai_physics[i].kart && free_slot==8) free_slot=i;
    }
    if (free_slot==8) return;
    if (id<0 || ai_physics[free_slot].kart!=kart || ai_physics[free_slot].entity!=entity ||
        !ai_physics[free_slot].identity_valid || ai_physics[free_slot].id!=(unsigned)id)
        memset(ai_physics+free_slot,0,sizeof ai_physics[free_slot]);
    if (id<0) return;
    ai_physics[free_slot].kart=kart; ai_physics[free_slot].entity=entity;
    ai_physics[free_slot].id=(unsigned)id; ai_physics[free_slot].identity_valid=1;
}
static int registered_ai_level(void *kart) {
    if (!kart || !client || !*client || !manager || !*manager ||
        (integer(*manager,0xd4c)!=1 && integer(*manager,0xd4c)!=6)) return -1;
    int mode=integer(*client,0x144);
    if (mode!=8 && (mode!=16 || !runtime_private_match_active())) return -1;
    for (unsigned i=0;i<8;++i)
        if (ai_physics[i].identity_valid && ai_physics[i].kart==kart &&
            ai_physics[i].entity==ptr(kart,0x18) &&
            live_ai_id(ai_physics[i].entity)==(int)ai_physics[i].id)
            return configured_driving_level();
    return -1;
}
static int recovering_ai_kart(void *kart);
typedef void (ABI *BoostUpdate)(void *, float);
static BoostUpdate native_boost_update;

/* Run before native expiry on both peers. The native entry is the state: no
 * local colour latch, synthetic pad contact, or shared boost-data mutation. */
static unsigned char *maintained_ai_pad(void *boost) {
    if (!boost || !maintained_duration) return NULL;
    void *entity=ptr(boost,0x18), *kart=NULL;
    for (unsigned i=0;i<8;++i)
        if (ai_physics[i].entity==entity && ai_physics[i].identity_valid) {
            kart=ai_physics[i].kart; break;
        }
    const int level=registered_ai_level(kart);
    if (level<4 || level>5) return NULL;
    void *list=ptr(boost,0x28);
    int count=list ? integer(list,12) : 0;
    void **items=list ? ptr(list,0x20) : NULL;
    if (!items || count<1 || count>256) return NULL;
    unsigned char *best=NULL;
    float fastest=0;
    for (int i=0;i<count;++i) {
        unsigned char *entry=items[i], *data=entry ? ptr(entry,0x10) : NULL;
        if (!data || !data[0x3c] || entry[0x48]) continue;
        float speed=real(data,0xc);
        float left=maintained_duration(entry)+real(data,0x1c)+real(entry,0x28)-real(entry,0x18);
        if (!isfinite(speed) || !isfinite(left) || left<=0 || speed<=fastest) continue;
        best=entry; fastest=speed;
    }
    return best;
}
static void maintain_ai_pad_fire(void *boost, float dt) {
    if (!isfinite(dt) || dt<=0) return;
    unsigned char *best=maintained_ai_pad(boost);
    if (!best) return;
    void *data=ptr(best,0x10);
    float remaining=maintained_duration(best)+real(data,0x1c)+real(best,0x28)-real(best,0x18);
    if (remaining<dt+2) {
        float reserve=real(best,0x28)+(dt+2-remaining);
        if (isfinite(reserve)) memcpy(best+0x28,&reserve,4);
    }
}

typedef unsigned char (ABI *KeepBoost)(void *, void *, void *);
static KeepBoost native_keep_boost;
static unsigned char ABI keep_boost(void *boost, void *data, void *entry) {
    /* Native e2f1d0 removes fire while braking, independently of its reserve.
     * Keep the acquired pad entry; do not change the AI's braking or steering. */
    if (entry && maintained_ai_pad(boost)==entry) return 1;
    return native_keep_boost(boost,data,entry);
}

void runtime_race_options_stop_boost(void *boost, void (ABI *stop)(void *)) {
    unsigned char *entry=maintained_ai_pad(boost);
    /* e92e90 skips entries in maintained contact. Scope that flag to this
     * call so only the retained pad survives; other boosts still cancel. */
    unsigned char contact=entry ? entry[0x30] : 0;
    if (entry) entry[0x30]=1;
    stop(boost);
    if (entry) entry[0x30]=contact;
}
static void ABI boost_update(void *boost, float dt) {
    maintain_ai_pad_fire(boost,dt);
    native_boost_update(boost,dt);
    const uint64_t now=runtime_ai_catchup_now();
    for (unsigned i=0;i<8;++i) if (ai_physics[i].entity==ptr(boost,0x18) &&
        registered_ai_level(ai_physics[i].kart)>=4 && now>=ai_physics[i].pad_report) {
        unsigned char *entry=maintained_ai_pad(boost);
        void *data=entry ? ptr(entry,0x10) : NULL;
        printf("Native AI pad fire: id=%u pad_speed=%.1f applied_boost=%.1f reserve=%.3f owner=%d\n",
               ai_physics[i].id,data ? real(data,0xc) : 0,real(boost,0x38),
               fire_remaining(ai_physics[i].kart),!!(((unsigned char *)ai_physics[i].entity)[0x3b]&1));
        ai_physics[i].pad_report=now+5000;
        break;
    }
}

static int physics_ai_level(void *kart) {
    if (recovering_ai_kart(kart)) return -1;
    int level=registered_ai_level(kart);
    return runtime_speed_race_active() && level<2 ? -1 : level;
}

static float catchup_for_kart(void *kart) {
    if (registered_ai_level(kart)<0 || recovering_ai_kart(kart)) return 0;
    for (unsigned i=0;i<8;++i) if (ai_physics[i].kart==kart && ai_physics[i].identity_valid) {
        void *list=racer_manager && *racer_manager ? ptr(*racer_manager,0x10) : NULL;
        int count=list ? integer(list,12) : 0;
        unsigned char **racers=list ? ptr(list,0x20) : NULL;
        if (racers && count>0 && count<=8) for (int j=0;j<count;++j) {
            unsigned char *r=racers[j];
            if (r && !r[0x8c] && race_handle((void **)(r+0x20))==ai_physics[i].entity &&
                !race_handle((void **)(r+0x10))) return runtime_ai_catchup_bonus(ai_physics[i].id);
        }
    }
    return 0;
}

typedef int (ABI *ExhaustState)(void *);
static ExhaustState native_exhaust_state;
/* Native exhaust update copies this result to its replicated next-state field. */
static int ABI exhaust_state(void *self) {
    const int state=native_exhaust_state(self);
    void *effects=race_handle((void **)((char *)self+0x18));
    void *entity=effects ? ptr(effects,0x18) : NULL;
    if (!entity) return state;
    for (unsigned i=0;i<8;++i) if (ai_physics[i].entity==entity &&
        registered_ai_level(ai_physics[i].kart)>=0) {
        const uint64_t now=runtime_ai_catchup_now();
        if (registered_ai_level(ai_physics[i].kart)>=4 && now>=ai_physics[i].exhaust_report) {
            printf("Native AI fire visual: id=%u native_state=%d tier=%s\n",ai_physics[i].id,state,
                   state==10 || state==11 ? "blue" : state==8 || state==9 ? "red" : "other");
            ai_physics[i].exhaust_report=now+5000;
        }
        if (!runtime_speed_race_active()) return state;
        /* Seed a migrated owner from the exhaust state already replicated by native. */
        const int previous=integer(self,0xc);
        if (!ai_physics[i].fire && previous>=8 && previous<=11) ai_physics[i].fire=previous;
        /* Remember the latest actual tier, including blue -> red transitions. */
        if (state>=8 && state<=11) ai_physics[i].fire=state;
        if (state<12 && ai_physics[i].fire && catchup_for_kart(ai_physics[i].kart)>0)
            return ai_physics[i].fire;
        break;
    }
    return state;
}

/* Stable racer identity gives different surge periods and peak strengths.
 * Only the host evaluates time; peers consume its already-quantized result. */
static float catchup_pace(unsigned id, uint64_t now) {
    const unsigned trait=(id*5+3)%8;
    const uint64_t period=8000+trait*1100;
    const float wave=.5f+.5f*sinf(6.28318530718f*((now%period)/(float)period+trait/8.f));
    return .78f+.12f*(trait/7.f)+.10f*wave;
}

static float normal_catchup_cap(int level, float gap, float speed) {
    static const float caps[]={.04f,.06f,.08f,.11f,.15f,.20f};
    if (level<0 || level>5 || !isfinite(gap) || !isfinite(speed) || speed<=0) return 0;
    /* Remove assistance at bumper distance; reach the cap only several seconds behind. */
    return caps[level]*fminf(1,fmaxf(0,(gap/speed-.35f)/3.f));
}

/* The room host computes every AI bonus, even when native ownership migrates. */
static void update_ai_catchup(void *self) {
    unsigned char *entity=ptr(self,0x18);
    if (!entity || !custom_ai_allowed() ||
        (integer(*client,0x144)==16 && runtime_room_mode_following())) return;
    unsigned slot=8;
    for (unsigned i=0;i<8;++i) if (ai_physics[i].entity==entity) { slot=i; break; }
    if (slot==8 || !ai_physics[slot].identity_valid) return;
    void *kart=ai_physics[slot].kart;
    int level=registered_ai_level(kart);
    runtime_ai_catchup_level_publish(level);
    void *list=racer_manager && *racer_manager ? ptr(*racer_manager,0x10) : NULL;
    int count=list ? integer(list,12) : 0;
    unsigned char **racers=list ? ptr(list,0x20) : NULL, *me=NULL, *target=NULL;
    if (racers && count>0 && count<=8) for (int i=0;i<count;++i)
        if (racers[i] && race_handle((void **)(racers[i]+0x20))==entity) me=racers[i];
    int remote_human=0;
    if (racers && count>0 && count<=8 && integer(*client,0x144)==16) for (int i=0;i<count;++i) {
        unsigned char *r=racers[i];
        unsigned char *e=r ? race_handle((void **)(r+0x20)) : NULL;
        if (e && !(e[0x3b]&1) && race_handle((void **)(r+0x10))) ++remote_human;
    }
    runtime_ai_catchup_require_peers(remote_human);
    const int ready=runtime_ai_catchup_ready();
    const uint64_t now=runtime_ai_catchup_now();
    float desired=0, normal_limit=0;
    int target_ai=0;
    if (ready && level>=0 && me && !me[0x8c] && !race_handle((void **)(me+0x10)) &&
        catchup_racer_ahead && world_position) {
        for (int i=0;i<count;++i) {
            unsigned char *r= racers[i];
            if (!r || r==me || r[0x8c] || !catchup_racer_ahead(r,me)) continue;
            if (!target || catchup_racer_ahead(target,r)) target=r;
        }
        target_ai=target && !ptr(target,0x10);
        void *other=target ? race_handle((void **)(target+0x20)) : NULL;
        if (other) {
            float a[3],b[3]; world_position(a,entity); world_position(b,other);
            float gap=hypotf(a[0]-b[0],a[1]-b[1]);
            float human_speed=hypotf(real(other,0x60),real(other,0x64));
            float ai_speed=hypotf(real(entity,0x60),real(entity,0x64));
            if (!runtime_speed_race_active()) {
                const int other_id=live_ai_id(other);
                if (other_id>=0) human_speed/=1+runtime_ai_catchup_bonus((unsigned)other_id);
                normal_limit=normal_catchup_cap(level,gap,ai_speed);
            }
            desired=runtime_ai_catchup_target(level,gap,human_speed,ai_speed,runtime_ai_catchup_bonus(ai_physics[slot].id));
            if (!runtime_speed_race_active()) desired=fminf(desired,normal_limit);
        }
    }
    const float pace=catchup_pace(ai_physics[slot].id,now);
    desired*=pace;
    const uint64_t last=ai_physics[slot].updated;
    float dt=last && now>=last ? fminf((now-last)*.001f,.1f) : 0;
    float old=ai_physics[slot].catchup, rate=desired>old ? .55f+.40f*pace : 1.0f;
    float bonus=old+fminf(fmaxf(desired-old,-rate*dt),rate*dt);
    if (!ready || level<0 || !me || me[0x8c]) bonus=0;
    if (!runtime_speed_race_active()) bonus=fminf(bonus,normal_limit);
    /* Preserve accumulated demand through a protected section. Only the
     * applied bonus is restricted; otherwise every hop restarts catch-up at zero. */
    ai_physics[slot].catchup=bonus;
    /* Sharp turns cannot use the same closing speed as a straight. Publish
     * this restriction from the host, including normal-mode native routes. */
    const float curve=fabsf(real(self,0x40));
    const float turn_allowance=isfinite(curve) ? fmaxf(0,fminf(1,(.5f-curve)/.4f)) : 0;
    bonus=fminf(bonus,2.5f*fminf(turn_allowance,
        runtime_speed_race_active() ? runtime_tt_reference_turn_allowance(ai_physics[slot].id,(uintptr_t)entity) : 1));
    /* Publish the jump restriction with the bonus so peers use one decision. */
    const float jump_lead=runtime_tt_reference_jump_lead(ai_physics[slot].id,(uintptr_t)entity);
    const int terrain_jump=runtime_speed_race_active() &&
        (!((unsigned char *)kart)[0x214] || (jump_lead>=0 && jump_lead<=.35f));
    if (!ready || level<0 || !me || me[0x8c] || terrain_jump) bonus=0;
    if (!(entity[0x3b]&1) || !me || me[0x8c]) runtime_ai_catchup_recovery_publish(ai_physics[slot].id,0);
    ai_physics[slot].updated=now;
    runtime_ai_catchup_publish(ai_physics[slot].id,bonus);
    if (now>=ai_physics[slot].next_report && me && !me[0x8c]) {
        printf("Native AI catch-up: ai_slot=%u level=%d itemless=%d target_position=%d target_ai=%d pace=%.2f bonus=%.2f demand=%.2f protected_jump=%d turn_allowance=%.2f authority=1 ready=%d native_owner=%d\n",
               ai_physics[slot].id,level,runtime_speed_race_active(),target ? integer(target,0x58) : -1,target_ai,pace,
               runtime_ai_catchup_bonus(ai_physics[slot].id),ai_physics[slot].catchup,terrain_jump,turn_allowance,ready,!!(entity[0x3b]&1));
        ai_physics[slot].next_report=now+5000;
    }
}

static float ai_speed_scale(int level, int itemless, uint32_t ticks, unsigned id) {
    static const float middle[]={0,0,.04f,.06f,.08f,.105f};
    if (level<0 || level>5 || !level || (level==1 && !itemless)) return 1;
    const float center=level==1 ? .015f : middle[level] + (itemless ? .04f * (level-1) : 0);
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
    /* Network peers can enter simulation at different local timer values. */
    if (client && *client && integer(*client, 0x144) == 16)
        return ai_speed_scale(level, itemless, 0, id);
    return ai_speed_scale(level,itemless,now_fn ? now_fn(timer) : 0,id);
}
static float kart_base_speed_scale(void *kart,int level,int itemless) {
    float scale=(level>=0 ? kart_speed_scale(kart,level,itemless) : 1)*(1+catchup_for_kart(kart));
    if (itemless && level>=2 && !((unsigned char *)kart)[0x214]) scale*=1.10f+.03f*(level-2);
    return scale;
}

typedef struct {
    void *kart, *entity, *clock, *curve;
    unsigned epoch, frame, interval;
    int valid, state, quantized;
    double phase;
    float start, target, expected, curve_scale, parameters[4];
} AccelerationStep;
static _Thread_local AccelerationStep acceleration_steps[16], deceleration_steps[16];
static _Thread_local unsigned acceleration_next, deceleration_next;

/* Match the native clock's 28 short and two long reference intervals each second. */
static float acceleration_interval(const AccelerationStep *step) {
    return step->quantized ? ((step->interval+1)*8192/30-step->interval*8192/30)/8192.f : 1.f/30;
}

static int kart_curve_timestep(void *kart, void *data, int kind, float speed, float dt, int decelerating, float *out) {
    AccelerationStep *steps=decelerating ? deceleration_steps : acceleration_steps, *step=NULL;
    unsigned *next=decelerating ? &deceleration_next : &acceleration_next;
    for (unsigned i=0;i<16;++i) if (steps[i].kart==kart) { step=&steps[i]; break; }
    const int reverse=decelerating && speed<0;
    if (reverse) speed=-speed;
    const float frames=timestep_frames();
    void *entity=kart ? ptr(kart,0x18) : NULL;
    if (!entity || !data || kind<0 || kind>2 ||
        !(frames<1) || !(dt>0) || !isfinite(dt) || !(kart_motion_dt>0) || !isfinite(kart_motion_dt) ||
        !(speed>=0) || !isfinite(speed) || !(real(kart,0x1a0)<=0) ||
        !component || !boost_type || !*boost_type) goto native;
    if (decelerating) {
        if (((unsigned char *)kart)[0x34] || ((unsigned char *)kart)[0x36] ||
            !((unsigned char *)kart)[0x214] || integer(kart,0xe0)!=0) goto native;
    } else if (kind!=0 || !((unsigned char *)kart)[0x34] || ((unsigned char *)kart)[0x35]) goto native;
    void *boost=component(entity,*boost_type,0);
    if (boost && !(real(boost,0x38)<=0)) goto native;
    void *handle=ptr(kart,0x98), *damage=NULL;
    if (handle) {
        if ((unsigned)integer(handle,0)&0x30000000) goto native;
        damage=ptr(handle,0x28);
        if (damage && integer(damage,0x40)!=-1) goto native;
    }
    const float parameters[]={real(kart,0x190),real(kart,0x19c),real(kart,0x30),real(data,12)};
    for (unsigned i=0;i<4;++i) if (!isfinite(parameters[i])) goto native;
    const float top=parameters[0]*parameters[3], curve_scale=dt/timestep_frame_dt();
    if (!(top>0) || !isfinite(top) || (!decelerating && speed>top) || !(parameters[1]>0) ||
        !isfinite(curve_scale)) goto native;
    void *curve=ptr(data,0x18+8*kind);
    if (!curve) goto native;
    if (!step) { step=&steps[(*next)++%16]; step->valid=0; }
    void *clock=*timestep_clock;
    const unsigned frame=(unsigned)integer(clock,0x10);
    const unsigned epoch=atomic_load_explicit(&acceleration_epoch,memory_order_relaxed);
    const int state=decelerating ? kind+4*reverse : integer(kart,0xe0);
    const float ticks=timestep_frame_dt()*8192;
    const int quantized=fabsf(real(clock,0x34))<=5e-7f && ticks==floorf(ticks);
    /* Native motion reprojects velocity; tolerate its float roundoff, not external impulses. */
    const float tolerance=8*0x1p-23f*fmaxf(1,fabsf(speed));
    /* shortcut: Restart on input changes; exact mid-interval decisions need native tick scheduling. */
    if (!step->valid || step->kart!=kart || step->entity!=entity || step->clock!=clock ||
        step->epoch!=epoch || frame-step->frame!=1 || step->curve!=curve || step->state!=state ||
        step->quantized!=quantized ||
        memcmp(step->parameters,parameters,sizeof parameters) || fabsf(step->expected-speed)>tolerance ||
        fabsf(step->curve_scale-curve_scale)>8*0x1p-23f*fabsf(curve_scale)) {
        step->kart=kart; step->entity=entity; step->clock=clock; step->curve=curve;
        step->epoch=epoch; step->state=state; step->phase=0; step->start=speed; step->curve_scale=curve_scale;
        step->interval=0; step->quantized=quantized;
        memcpy(step->parameters,parameters,sizeof parameters);
        step->target=decelerating && speed<=50 ? 0 :
            native_kart_speed(kart,data,kind,speed,curve_scale*acceleration_interval(step));
    }
    step->frame=frame; step->valid=1;
    step->phase+=frames;
    float span=acceleration_interval(step)*30;
    if (step->phase>=span-8*0x1p-23) {
        step->start=step->target; step->phase-=span;
        step->interval=(step->interval+1)%30;
        span=acceleration_interval(step)*30;
        if (fabs(step->phase)<8*0x1p-23) step->phase=0;
        step->target=decelerating && step->start<=50 ? 0 :
            native_kart_speed(kart,data,kind,step->start,curve_scale*acceleration_interval(step));
    }
    /* Interpolate native30 endpoints; a partial curve evaluation repeats inverse-curve rounding. */
    *out=step->expected=step->phase ? (float)(step->start+(step->target-step->start)*(step->phase/span)) : step->start;
    if (reverse) *out=-*out;
    return 1;
native:
    if (step) step->valid=0;
    return 0;
}

/* e4a220 selects natural decay for unboosted overspeed above 1.05 times the speed limit. */
static float kart_speed_timestep(void *kart, void *data, int kind, float speed, float dt) {
    float acceleration;
    if (kart_curve_timestep(kart,data,kind,speed,dt,0,&acceleration)) return acceleration;
    const float frames=timestep_frames();
    if (kart && data && (kind==0 || kind==1) && dt>0 && isfinite(dt) &&
        kart_motion_dt>0 && isfinite(kart_motion_dt) && frames<1 && isfinite(speed) &&
        real(kart,0x190)>=1 && real(kart,0x1a0)<=0 && component && boost_type && *boost_type) {
        const float top=real(kart,0x190)*real(data,12);
        if (top>0 && speed>1.05f*top) {
            void *entity=ptr(kart,0x18);
            void *boost=entity ? component(entity,*boost_type,0) : NULL;
            /* This native branch is a pure decay; retain its 30 FPS inverse-curve rounding. */
            if (!boost || real(boost,0x38)<=0) {
                /* Native callers can scale curve time independently of the frame timestep. */
                const float reference=native_kart_speed(kart,data,kind,speed,dt/frames);
                if (reference>0 && reference<=speed)
                    return speed*powf(reference/speed,frames);
            }
        }
    }
    return native_kart_speed(kart,data,kind,speed,dt);
}

/* Native e4a220 reads speed-data +c and curve pointers +18/+20/+28.
 * A call-local view changes AI base speed without mutating a shared archetype. */
static float ABI kart_speed(void *kart, void *data, int kind, float speed, float dt) {
    int level=physics_ai_level(kart);
    const int itemless=runtime_speed_race_active();
    const float scale=kart_base_speed_scale(kart,level,itemless);
    static int reported_policy=-1;
    const int policy=level+(itemless ? 16 : 0);
    if (level>=0 && reported_policy!=policy) {
        reported_policy=policy;
        printf("Native AI: driving policy route=%s level=%d dynamic_base_speed_scale=%.3f cycle_seconds=16 ignore_slow_surfaces=%d\n",
               itemless ? "ghost-traffic" : "native",level,scale,itemless);
    }
    if (scale==1 || !data || !isfinite(real(data,12)) || real(data,12)<=0)
        return kart_speed_timestep(kart,data,kind,speed,dt);
    unsigned char tuned[0x30]; memcpy(tuned,data,sizeof tuned);
    float top=real(data,12)*scale; memcpy(tuned+12,&top,4);
    return kart_speed_timestep(kart,tuned,kind,speed,dt);
}
/* Keep native vertical impulses on both peers. Extra height changes the
 * flight time assumed by the recording, even at the correct planar speed. */
static float ABI jump_impulse(void *kart) { return native_jump_impulse(kart); }
static float ABI trick_jump_impulse(void *kart) { return native_trick_impulse(kart); }

/* e46250 reads the per-kart boost contribution and its native speed limits.
 * Scale all three for this call, then restore them; reserves never change. */
static void ABI kart_motion(void *kart, void *data, void *velocity) {
    const int level = physics_ai_level(kart);
    void *entity = kart ? ptr(kart, 0x18) : NULL;
    const float catchup=catchup_for_kart(kart);
    const float boost_scale=(runtime_speed_race_active() && level>=2 ? 1.12f+.03f*(level-2) : 1)*(1+catchup);
    unsigned char *boost = boost_scale>1 && entity &&
        component && boost_type && *boost_type ? component(entity, *boost_type, 0) : NULL;
    float saved[3];
    const size_t offsets[] = {0x38, 0x4c, 0x50};
    int tuned = boost && isfinite(real(boost, 0x38)) && real(boost, 0x38) > 0;
    if (tuned) for (int i = 0; i < 3; ++i) {
        saved[i] = real(boost, offsets[i]);
        if (!isfinite(saved[i]) || saved[i] < 0) tuned = 0;
    }
    if (tuned) for (int i = 0; i < 3; ++i) {
        const float value = saved[i] * boost_scale;
        memcpy(boost + offsets[i], &value, 4);
    }
    int slot=-1;
    const uint64_t now=runtime_ai_catchup_now();
    if (runtime_speed_race_active() && level>=2 && entity==data && native_set_velocity &&
        (integer(*client,0x144)!=16 || (((unsigned char *)entity)[0x3b]&1)))
        for (int i=0;i<8;++i) if (ai_physics[i].kart==kart) { slot=i; break; }
    float approach=0, route_speed=0, motion_dt=0;
    if (slot>=0) {
        uint64_t last=ai_physics[slot].motion_updated;
        motion_dt=last && now>=last ? fminf((now-last)*.001f,.05f) : 0;
        ai_physics[slot].motion_updated=now;
        void *timer=ptr(kart,0xf8), *vtable=timer ? ptr(timer,0) : NULL;
        typedef uint32_t (ABI *MotionNow)(void *);
        MotionNow simulation_now=vtable ? (MotionNow)ptr(vtable,0xb0) : NULL;
        if (simulation_now) {
            const uint32_t tick=simulation_now(timer);
            motion_dt=ai_physics[slot].motion_clock_valid ?
                fminf((uint32_t)(tick-ai_physics[slot].motion_tick)/8192.f,.05f) : 0;
            ai_physics[slot].motion_tick=tick; ai_physics[slot].motion_clock_valid=1;
        } else ai_physics[slot].motion_clock_valid=0;
        if (ai_physics[slot].identity_valid) {
            approach=runtime_tt_reference_jump_speed(ai_physics[slot].id,(uintptr_t)entity);
            route_speed=runtime_tt_reference_route_speed(ai_physics[slot].id,(uintptr_t)entity);
        }
        const int air=!((unsigned char *)kart)[0x214];
        if (air || approach<=0) ai_physics[slot].approach_floor=0;
        const float speed=hypotf(real(entity,0x60),real(entity,0x64));
        if (!air) {
            ai_physics[slot].air_floor=0; ai_physics[slot].air_until=0;
            ai_physics[slot].jump_reference=approach;
        }
        else if (!ai_physics[slot].airborne && real(entity,0x68)>0 && isfinite(speed)) {
            const float reference=approach>0 ? approach : ai_physics[slot].jump_reference;
            ai_physics[slot].jump_reference=reference;
            /* Catch-up can exceed the recorded landing speed. Do not lock
             * that overspeed in for the entire flight and undo native braking. */
            ai_physics[slot].air_floor=reference>0 ? fminf(speed,reference) : 0;
            if (now>=ai_physics[slot].jump_report) {
                printf("Native AI jump: ai_slot=%u takeoff_speed=%.1f reference_speed=%.1f\n",
                       ai_physics[slot].id,speed,reference);
                ai_physics[slot].jump_report=now+1000;
            }
            ai_physics[slot].air_until=reference>0 ? now+3000 : 0;
        }
        ai_physics[slot].airborne=air;
    }
    const float saved_motion_dt=kart_motion_dt, saved_speed_scale=kart_motion_speed_scale;
    kart_motion_dt=velocity ? real(velocity,0x18) : 0;
    kart_motion_speed_scale=kart_base_speed_scale(kart,level,runtime_speed_race_active());
    native_kart_motion(kart, data, velocity);
    kart_motion_dt=saved_motion_dt; kart_motion_speed_scale=saved_speed_scale;
    /* The recorded landing corridor has a speed budget. Catch-up must not
     * carry several times that speed over an edge; only the native owner writes velocity. */
    if (slot>=0) {
        const float lead=runtime_tt_reference_jump_lead(ai_physics[slot].id,(uintptr_t)entity);
        const float reference=ai_physics[slot].airborne ? route_speed :
            (lead>=0 && lead<=.35f ? approach : 0);
        if (reference>0 && (!ai_physics[slot].airborne || now<ai_physics[slot].air_until)) {
            float v[3]; memcpy(v,(char *)entity+0x60,sizeof v);
            const float speed=hypotf(v[0],v[1]), limit=reference*1.05f;
            if (isfinite(speed) && isfinite(v[2]) && speed>limit) {
                v[0]*=limit/speed; v[1]*=limit/speed;
                native_set_velocity(entity,v);
            }
        }
    }
    if (slot>=0 && !ai_physics[slot].airborne && approach>0 && motion_dt>0) {
        float v[3]; memcpy(v,(char *)entity+0x60,sizeof v);
        const float speed=hypotf(v[0],v[1]);
        /* Accelerate along the existing approach, never teleport or revive a
         * stopped kart. The recorded jump determines the target, not difficulty. */
        if (isfinite(speed) && isfinite(v[2]) && speed>100 && speed<approach) {
            float previous=ai_physics[slot].approach_floor;
            if (speed<fminf(previous,route_speed)*.75f) previous=0; /* Collision cancels accumulated assistance. */
            float target=fminf(approach,fmaxf(speed,previous)+6000.f*motion_dt);
            ai_physics[slot].approach_floor=target;
            v[0]*=target/speed; v[1]*=target/speed;
            native_set_velocity(entity,v);
        }
    }
    if (slot>=0 && world_position && motion_dt>0) {
        float position[3],v[3],guided[3]; world_position(position,entity);
        memcpy(v,(char *)entity+0x60,sizeof v);
        if (runtime_tt_reference_motion(ai_physics[slot].id,(uintptr_t)entity,position,v,
                                         ai_physics[slot].airborne,motion_dt,guided)) {
            native_set_velocity(entity,guided);
            if (ai_physics[slot].airborne && now>=ai_physics[slot].jump_report) {
                printf("Native AI flight: ai_slot=%u position=%.1f,%.1f,%.1f velocity=%.1f,%.1f,%.1f guided=%.1f,%.1f,%.1f\n",
                       ai_physics[slot].id,position[0],position[1],position[2],v[0],v[1],v[2],guided[0],guided[1],guided[2]);
                ai_physics[slot].jump_report=now+1000;
            }
        } else ai_physics[slot].air_until=0;
    }
    if (tuned) for (int i = 0; i < 3; ++i) memcpy(boost + offsets[i], &saved[i], 4);
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

/* Native recovery follows a different lane with its own braking and hop
 * decisions. Ghost-route physics must not boost it through that lane. */
static int recovering_ai_kart(void *kart) {
    if (!kart || !runtime_speed_race_active()) return 0;
    void *entity=ptr(kart,0x18);
    if (client && *client && integer(*client,0x144)==16) {
        for (unsigned i=0;entity && i<8;++i)
            if (ai_physics[i].kart==kart && ai_physics[i].entity==entity && ai_physics[i].identity_valid)
                return runtime_ai_catchup_recovery(ai_physics[i].id);
        return 0;
    }
    for (unsigned i=0;entity && i<8;++i)
        if (stuck_recovery[i].owner==entity && stuck_recovery[i].active) return 1;
    return 0;
}

/* Simulation-clock deadlines, not render-frame counts. Hysteresis prevents
 * alternating controllers at a blocked shortcut or while still reversing. */
static int recover_stuck(unsigned id, void *owner, int lap, uint32_t now,
                         int driving, const float position[3], const float velocity[3], unsigned intent) {
    struct StuckRecovery *r = stuck_recovery + id;
    if (!r->initialized || r->owner != owner || r->lap != lap || !driving) {
        *r = (struct StuckRecovery){.owner=owner,.lap=lap,.initialized=1,.since=now};
        memcpy(r->anchor,position,sizeof r->anchor);
        runtime_ai_catchup_recovery_publish(id,0);
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
            for (unsigned i=0;i<8;++i) if (ai_physics[i].entity==owner) {
                ai_physics[i].air_until=0;
                ai_physics[i].air_floor=ai_physics[i].approach_floor=ai_physics[i].jump_reference=0;
                ai_physics[i].airborne=0;
            }
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
    runtime_ai_catchup_recovery_publish(id,r->active);
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
                                    float targets[2][3], unsigned intent, int grounded) {
    const float lead=runtime_tt_reference_jump_lead(id,(uintptr_t)entity);
    if (!grounded || (intent & (2u|512u)) || ((intent & 4u) && lead<.6f) || !racer_manager || !*racer_manager || !world_position) return;
    float dx=targets[0][0]-position[0], dy=targets[0][1]-position[1];
    float fx=targets[1][0]-targets[0][0], fy=targets[1][1]-targets[0][1];
    float length=hypotf(dx,dy), far_length=hypotf(fx,fy);
    if (length<1 || far_length<1 || dx*fx+dy*fy<.96f*length*far_length) return;
    dx/=length; dy/=length;
    void *list=ptr(*racer_manager,0x10);
    int count=list ? integer(list,12) : 0;
    void **items=list ? ptr(list,0x20) : NULL;
    if (!items || count<0 || count>8) return;
    float offset=0, nearest=900;
    for (int i=0;i<count;++i) {
        unsigned char *other=items[i];
        if (!other || other[0x8c]) continue;
        void *other_entity=race_handle((void **)(other+0x20));
        if (!other_entity || other_entity==entity) continue;
        float p[3]; world_position(p,other_entity);
        float x=p[0]-position[0], y=p[1]-position[1];
        float ahead=x*dx+y*dy, side=y*dx-x*dy;
        if (ahead<0 || ahead>=nearest || fabsf(side)>=240 || fabsf(p[2]-position[2])>120) continue;
        unsigned char *player=race_handle((void **)(other+0x10));
        int other_id=player ? integer(player,0xc8) : integer(other,0x164);
        if (other_id<0 || other_id>=8) continue;
        float weight=(1-ahead/900)*(1-fabsf(side)/240);
        offset=(id<(unsigned)other_id ? -90.f : 90.f)*weight;
        nearest=ahead;
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
    if (!racer || racer[0x8c] || ptr(racer,0x10)) return;
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
    if (guided) {
        unsigned char *driving=ptr(ai,0x48);
        unsigned char *kart=driving ? race_handle((void **)(driving+0x58)) : NULL;
        avoid_reference_traffic(entity,(unsigned)id,position,targets,intent,
                                kart && ptr(kart,0x18)==entity && kart[0x214]);
    }
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
 * arithmetic (e2e6b0). This query never changes reserves. */
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
    if (level < 0 || level > 5 || !racer || racer[0x8c] || ptr(racer,0x10) || runtime_speed_race_active() ||
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

typedef struct {
    float ymm[16][8];
    uintptr_t r15,r14,r13,r12,rbp,rbx,r11,r10,r9,r8,rdi,rsi,rdx,rcx,rax,flags,resume;
} TimestepRegisters;

static void ABI rotation_timestep(TimestepRegisters *r) {
    const float dt=real((const void *)r->rcx,0x20);
    const float frames=timestep_frames();
    if (!(dt>0) || !isfinite(dt) || !(frames<1)) return;
    float *omega=r->ymm[6];
    const double length=hypot(hypot(omega[0],omega[1]),omega[2]);
    if (!(length>0) || !isfinite(length)) return;
    /* Native normalized Euler rotation advances by 2*atan(|omega|*dt/2). */
    const double scale=tan(frames*atan(length*dt/(2*frames)))/(length*dt*.5);
    for (unsigned i=0;i<3;++i) omega[i]=(float)(omega[i]*scale);
}

static size_t register_timestep_bridge(unsigned char *code,
                                      const unsigned char *displaced, size_t length,
                                      const void *resume, void (ABI *helper)(TimestepRegisters *)) {
    /* Save the continuation and every live register; helpers may select a new continuation. */
    memcpy(code,"\xff\x35\x02\0\0\0\xeb\x08",8);
    unsigned char *p=code+16;
    const unsigned char save[]={0x9c,0x50,0x51,0x52,0x56,0x57,
        0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x53,0x55,
        0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0,2,0,0};
    memcpy(p,save,sizeof save); p+=sizeof save;
    for (unsigned i=0;i<16;++i) {
        *p++=0xc5; *p++=i<8 ? 0xfc : 0x7c; *p++=0x11;
        *p++=(unsigned char)(0x84+8*(i&7)); *p++=0x24;
        const uint32_t offset=i*32; memcpy(p,&offset,4); p+=4;
    }
    const unsigned char call[]={0x48,0x89,0xe7,0x48,0x89,0xe3,
        0x48,0x83,0xe4,0xf0,0x48,0xb8};
    memcpy(p,call,sizeof call); p+=sizeof call;
    memcpy(p,&helper,8); p+=8;
    memcpy(p,"\xff\xd0\x48\x89\xdc",5); p+=5;
    for (unsigned i=0;i<16;++i) {
        *p++=0xc5; *p++=i<8 ? 0xfc : 0x7c; *p++=0x10;
        *p++=(unsigned char)(0x84+8*(i&7)); *p++=0x24;
        const uint32_t offset=i*32; memcpy(p,&offset,4); p+=4;
    }
    const unsigned char restore[]={0x48,0x81,0xc4,0,2,0,0,
        0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5d,0x5b,
        0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,0x9d,
        0x48,0x8d,0x64,0x24,0x08,0xff,0x64,0x24,0xf8};
    memcpy(p,restore,sizeof restore); p+=sizeof restore;
    memcpy(code+8,&p,8);
    memcpy(p,displaced,length); p+=length;
    jump(p,resume);
    return (size_t)(p+14-code);
}


/* A full table disables new corrections; native motion remains unchanged. */
#define FREEFLIGHT_CAPACITY 2048
struct FreeflightState {
    uintptr_t a, info, world;
    float gravity[3], velocity[3], dt, frame_dt;
    unsigned iterations, seen;
    unsigned char active, valid, touched;
};
static struct FreeflightState freeflight_states[FREEFLIGHT_CAPACITY];
static atomic_flag freeflight_lock=ATOMIC_FLAG_INIT;
static atomic_int freeflight_disabled;
static unsigned char *freeflight_image;
static void freeflight_enter(void) {
    while (atomic_flag_test_and_set_explicit(&freeflight_lock,memory_order_acquire)) {}
}
static void freeflight_leave(void) {
    atomic_flag_clear_explicit(&freeflight_lock,memory_order_release);
}
static struct FreeflightState *freeflight_find(uintptr_t a,int create) {
    struct FreeflightState *available=NULL;
    const size_t first=(a>>5)&(FREEFLIGHT_CAPACITY-1);
    /* Bound worker contention; crowded buckets retain native integration. */
    for (size_t n=0;n<16;++n) {
        struct FreeflightState *s=&freeflight_states[(first+n)&(FREEFLIGHT_CAPACITY-1)];
        if (s->a==a) return s;
        if (!s->active && !available) available=s;
        if (!s->a) break;
    }
    if (!create || !available) return NULL;
    available->a=a;
    return available;
}
static int freeflight_supported(const void *world) {
    const void *manager=ptr(world,0x150);
    if (!manager) return 0;
    for (unsigned i=0;i<8;++i) {
        const void *solver=ptr(manager,0x568+i*sizeof(void *));
        if (i>=2) { if (solver) return 0; continue; }
        if (!solver) return 0;
        const void *vtable=ptr(solver,0),*solve=vtable ? ptr(vtable,0x30) : NULL;
        if (i==0 && solve!=freeflight_image+0x81ace0 && solve!=freeflight_image+0x8c3620) return 0;
        if (i==1 && solve!=freeflight_image+0x859190) return 0;
    }
    return 1;
}
static void ABI freeflight_capture(TimestepRegisters *r) {
    /* Native final integration skips the reserved static motion (ID 0). */
    if (!r->rdi) {
        freeflight_enter();
        struct FreeflightState *s=freeflight_find(r->r8,0);
        if (s) s->active=0;
        freeflight_leave();
        return;
    }
    const void *world=(void *)r->rsi,*motion=(void *)(r->r15+r->rdi);
    const uintptr_t info=(uintptr_t)world+0x330;
    unsigned short property; memcpy(&property,(const char *)motion+0x38,sizeof property);
    const void *properties=ptr(ptr(world,0x5d0),0x28);
    const void *p=(const char *)properties+(size_t)property*0x40;
    const int iterations=integer((void *)info,0xb4);
    int valid=!atomic_load_explicit(&freeflight_disabled,memory_order_acquire) &&
        freeflight_supported(world) && iterations>0 && integer((void *)info,0xbc)==1 &&
        real(p,0x18)==0 && real(p,0x1c)==0 && real(p,0x24)==0;
    freeflight_enter();
    struct FreeflightState *s=freeflight_find(r->r8,1);
    if (s) {
        s->world=(uintptr_t)world; s->info=info; s->dt=real((void *)info,0x20); s->frame_dt=timestep_frame_dt();
        s->iterations=(unsigned)iterations; s->seen=1; s->active=1; s->touched=0;
        for (unsigned i=0;i<3;++i) {
            s->gravity[i]=r->ymm[0][i]; s->velocity[i]=real(motion,0x40+4*i);
            valid=valid && isfinite(s->gravity[i]) && isfinite(s->velocity[i]);
        }
        s->valid=(unsigned char)valid;
    }
    freeflight_leave();
}
static void ABI freeflight_force(TimestepRegisters *r) {
    freeflight_enter();
    struct FreeflightState *s=freeflight_find(r->rdx,0);
    if (s && s->active) {
        ++s->seen;
        if (s->info!=r->r9 || memcmp(s->gravity,r->ymm[0],sizeof s->gravity)) s->valid=0;
    }
    freeflight_leave();
}
static void freeflight_touch(uintptr_t a,uintptr_t b) {
    freeflight_enter();
    struct FreeflightState *s=freeflight_find(a,0);
    if (s && s->active) s->touched=1;
    s=freeflight_find(b,0);
    if (s && s->active) s->touched=1;
    freeflight_leave();
}
static uintptr_t freeflight_entry(uintptr_t frame,int offset,unsigned side,const void *row,size_t index) {
    const uintptr_t base=(uintptr_t)ptr((void *)(frame+(intptr_t)offset+(uintptr_t)side*8),0);
    unsigned id; memcpy(&id,(const char *)row+index,sizeof id);
    return base+(uintptr_t)id*32;
}
static void ABI freeflight_scalar_contact(TimestepRegisters *r) {
    const unsigned char *row=(void *)r->r13;
    if (row[0]>13) { atomic_store_explicit(&freeflight_disabled,1,memory_order_release); return; }
    const unsigned side=row[1]&1;
    freeflight_touch(freeflight_entry(r->rbp,-0x60,side,row,0x10),
                     freeflight_entry(r->rbp,-0x60,1-side,row,0x14));
}
static void ABI freeflight_contact(TimestepRegisters *r) {
    const unsigned char *row=(void *)r->r14;
    const unsigned side=row[0x1b];
    if (side>1) { atomic_store_explicit(&freeflight_disabled,1,memory_order_release); return; }
    freeflight_touch(freeflight_entry(r->rbp,-0x98,side,row,0),
                     freeflight_entry(r->rbp,-0x98,1-side,row,4));
}
static void ABI freeflight_constraint(TimestepRegisters *r) {
    const unsigned char *row=(void *)r->r14;
    const unsigned side=row[0xe];
    if (side>1 || row[0xf]<2) { atomic_store_explicit(&freeflight_disabled,1,memory_order_release); return; }
    freeflight_touch(freeflight_entry(r->rbp,-0x40,side,row,4),
                     freeflight_entry(r->rbp,-0x40,1-side,row,8));
}
static void ABI freeflight_finish(TimestepRegisters *r) {
    const uintptr_t a=(uintptr_t)ptr((void *)(r->rbp-0x230),0);
    struct FreeflightState saved={0};
    freeflight_enter();
    struct FreeflightState *s=freeflight_find(a,0);
    if (s && s->active) { saved=*s; s->active=0; }
    freeflight_leave();
    if (!saved.active || !saved.valid || saved.touched || saved.info!=r->rcx ||
        atomic_load_explicit(&freeflight_disabled,memory_order_acquire) ||
        !freeflight_supported((void *)saved.world) ||
        saved.seen!=saved.iterations || !(saved.frame_dt>0 && saved.frame_dt<273.f/8192) ||
        saved.frame_dt!=timestep_frame_dt() || !(saved.dt>0) ||
        saved.dt!=real((void *)r->rcx,0x20) ||
        saved.iterations!=(unsigned)integer((void *)r->rcx,0xb4) || integer((void *)r->rcx,0xbc)!=1) return;
    /* Explicit contact markers establish scope; this rejects additional forces and caps. */
    for (unsigned i=0;i<3;++i) {
        const double expected=saved.velocity[i]+(double)saved.iterations*saved.gravity[i];
        const double tolerance=32*FLT_EPSILON*(fabs(saved.velocity[i])+fabs(expected)+1);
        const double mean=saved.velocity[i]+.5*(saved.iterations+1.)*saved.gravity[i];
        const double mean_tolerance=32*FLT_EPSILON*(fabs(saved.velocity[i])+fabs(mean)+1);
        if (!isfinite(r->ymm[3][i]) || fabs(r->ymm[3][i]-expected)>tolerance ||
            !isfinite(r->ymm[2][i]) || fabs(r->ymm[2][i]-mean)>mean_tolerance) return;
    }
    const double ratio=.5*(1./(saved.frame_dt*30.)-1);
    float adjusted[3];
    for (unsigned i=0;i<3;++i) {
        adjusted[i]=(float)(r->ymm[2][i]+saved.gravity[i]*ratio);
        if (!isfinite(adjusted[i])) return;
    }
    memcpy(r->ymm[2],adjusted,sizeof adjusted);
}

static const struct FreeflightSite {
    size_t address, length;
    unsigned char bytes[20];
    void (ABI *helper)(TimestepRegisters *);
} freeflight_sites[]={
    {0x80eded,14,{0x49,0x8d,0x44,0x3f,0x38,0xc5,0xf8,0x28,0x4d,0xa0,0xc5,0xf0,0x58,0xc0},freeflight_capture},
    {0x878b3a,16,{0xc5,0xf0,0x58,0xc0,0xc4,0xe3,0x79,0x04,0xca,0x09,0xc4,0xe3,0x71,0x0c,0xc0,0x07},freeflight_force},
    {0x81ae17,15,{0x49,0x8b,0x5d,0x38,0x8b,0x8d,0xc8,0xfe,0xff,0xff,0xba,0x00,0x10,0x00,0x00},freeflight_scalar_contact},
    {0x8c37f0,20,{0x4c,0x89,0xb5,0x60,0xff,0xff,0xff,0x41,0x0f,0xb6,0x56,0x1b,0x48,0x8b,0x84,0xd5,0x68,0xff,0xff,0xff},freeflight_contact},
    {0x854483,17,{0x41,0x0f,0xb6,0x46,0x0e,0x48,0x8b,0x74,0xc5,0xc0,0x48,0x89,0xb5,0xb8,0xfe,0xff,0xff},freeflight_constraint},
    {0x8790b7,17,{0xc5,0xf8,0x29,0x1a,0xc4,0xc3,0x69,0x0c,0xc4,0x08,0xc4,0x81,0x78,0x29,0x44,0x30,0x60},freeflight_finish},
};
/* Native layout and the body accesses dominated by the three markers. */
static const struct FreeflightLayout {
    size_t address,length;
    unsigned char bytes[64];
} freeflight_layouts[]={
    {0x81aed8,31,{0x41,0x0f,0xb6,0x45,0x00,0x48,0x83,0xf8,0x0d,0x0f,0x87,0xc9,0x02,0x00,0x00,0x48,0x8d,0x0d,0x7a,0x27,0x00,0x00,0x48,0x63,0x04,0x81,0x48,0x01,0xc8,0xff,0xe0}},
    {0x81d668,56,{0x8f,0xd8,0xff,0xff,0x62,0xdd,0xff,0xff,0x88,0xdf,0xff,0xff,0x92,0xdf,0xff,0xff,0x9c,0xdf,0xff,0xff,0xa6,0xdf,0xff,0xff,0x48,0xdb,0xff,0xff,0x48,0xdb,0xff,0xff,0xb0,0xdf,0xff,0xff,0xb4,0xe1,0xff,0xff,0x39,0xe3,0xff,0xff,0x43,0xe3,0xff,0xff,0x4d,0xe3,0xff,0xff,0x57,0xe3,0xff,0xff}},
    {0x81af72,29,{0x41,0x0f,0xb6,0x45,0x01,0x48,0x89,0xc1,0x48,0x83,0xe1,0x01,0x48,0xc1,0xe1,0x03,0x48,0x8d,0x55,0xa0,0x48,0x89,0xd6,0x48,0x09,0xf1,0x4c,0x8b,0x11}},
    {0x81af8f,36,{0x41,0x8b,0x55,0x10,0x48,0xc1,0xe2,0x05,0xb9,0x01,0x00,0x00,0x00,0xc4,0xe2,0x78,0xf2,0xc1,0x48,0xc1,0xe0,0x03,0x48,0x09,0xf0,0x4c,0x8b,0x38,0x41,0x8b,0x7d,0x14,0x48,0xc1,0xe7,0x05}},
    {0x81b43e,31,{0x41,0x0f,0xb6,0x45,0x01,0x48,0x83,0xe0,0x01,0x48,0xc1,0xe0,0x03,0x48,0x8d,0x4d,0xa0,0x48,0x09,0xc8,0x48,0x8b,0x00,0x41,0x8b,0x4d,0x10,0x48,0xc1,0xe1,0x05}},
    {0x81b631,61,{0x41,0x0f,0xb6,0x55,0x01,0x48,0x89,0xd0,0x48,0x83,0xe0,0x01,0x48,0xc1,0xe0,0x03,0x48,0x8d,0x4d,0xa0,0x48,0x89,0xce,0x48,0x09,0xf0,0x4c,0x8b,0x10,0x41,0x8b,0x4d,0x10,0x48,0xc1,0xe1,0x05,0xb8,0x01,0x00,0x00,0x00,0xc4,0xe2,0x68,0xf2,0xd0,0x48,0xc1,0xe2,0x03,0x48,0x09,0xf2,0x4c,0x8b,0x3a,0x41,0x8b,0x75,0x14}},
    {0x81b82e,31,{0x41,0x0f,0xb6,0x45,0x01,0x48,0x83,0xe0,0x01,0x48,0xc1,0xe0,0x03,0x48,0x8d,0x4d,0xa0,0x48,0x09,0xc8,0x48,0x8b,0x00,0x41,0x8b,0x4d,0x10,0x48,0xc1,0xe1,0x05}},
    {0x6e7b16,6,{0x89,0x97,0xb4,0x00,0x00,0x00}},
    {0x6e7b3f,6,{0x89,0x8f,0xbc,0x00,0x00,0x00}},
    {0x6e7b69,5,{0xc5,0xf8,0x11,0x67,0x20}},
    {0x85446f,9,{0x45,0x0f,0xb6,0x4e,0x0f,0x49,0xc1,0xe1,0x04}},
    {0x6af550,16,{0x83,0xfe,0x08,0x7c,0x01,0xcc,0x89,0xf0,0x48,0x89,0x94,0xc7,0x68,0x05,0x00,0x00}},
    {0x6ce368,7,{0x4d,0x8b,0xa5,0x50,0x01,0x00,0x00}},
    {0x80ecc0,11,{0x48,0x8b,0x86,0xd0,0x05,0x00,0x00,0x4c,0x8b,0x58,0x28}},
    {0x80ed1e,17,{0x41,0x0f,0xb7,0x44,0x3f,0x38,0x48,0xc1,0xe0,0x06,0xc4,0xc2,0x79,0x18,0x44,0x03,0x08}},
    {0x878dde,7,{0xc4,0xe2,0x79,0x18,0x54,0x07,0x24}},
    {0x878eab,23,{0xc4,0xe2,0x79,0x18,0x44,0x07,0x1c,0xc5,0xf8,0x28,0x59,0x20,0xc5,0xe0,0x59,0xc0,0xc4,0xe2,0x79,0x18,0x6c,0x07,0x18}},
    {0x8523d0,15,{0x0f,0xb6,0x43,0x18,0x49,0x8b,0xbc,0xc7,0x68,0x05,0x00,0x00,0x48,0x8b,0x07}},
    {0x852405,3,{0xff,0x50,0x30}},
    {0x878bf2,18,{0x83,0x7a,0x18,0x00,0x75,0x0c,0xff,0xc9,0x48,0x83,0xc6,0x20,0x48,0x83,0xc2,0x20,0xff,0xc7}},
    {0x80ee57,7,{0x8b,0x45,0xbc,0x41,0x89,0x41,0x18}},
    {0x81afc1,12,{0xc4,0x41,0x78,0x28,0x04,0x12,0xc4,0x41,0x78,0x28,0x3c,0x3f}},
    {0x81b455,19,{0x41,0x8b,0x4d,0x10,0x48,0xc1,0xe1,0x05,0xc5,0xfb,0xf0,0x54,0x08,0x0c,0xc5,0x78,0x28,0x2c,0x08}},
    {0x81b66a,34,{0x41,0x8b,0x75,0x14,0x48,0xc1,0xe6,0x05,0xc4,0xc1,0x7b,0xf0,0x54,0x0a,0x0c,0xc4,0xc1,0x7b,0xf0,0x5c,0x37,0x0c,0xc4,0x41,0x78,0x28,0x14,0x0a,0xc4,0x41,0x78,0x28,0x0c,0x37}},
    {0x81b845,19,{0x41,0x8b,0x4d,0x10,0x48,0xc1,0xe1,0x05,0xc5,0xfb,0xf0,0x54,0x08,0x0c,0xc5,0x78,0x28,0x2c,0x08}},
    {0x8c3804,50,{0x41,0x8b,0x0e,0x48,0xc1,0xe1,0x05,0x48,0x8d,0x3c,0x08,0x48,0x89,0xbd,0x78,0xff,0xff,0xff,0xbe,0x01,0x00,0x00,0x00,0x48,0x29,0xd6,0x48,0x8b,0x94,0xf5,0x68,0xff,0xff,0xff,0x41,0x8b,0x76,0x04,0x48,0xc1,0xe6,0x05,0x4c,0x8d,0x04,0x32,0x4c,0x89,0x45,0x80}},
    {0x854494,61,{0x41,0x8b,0x56,0x04,0x41,0x8b,0x7e,0x08,0x48,0xc1,0xe2,0x05,0xb9,0x01,0x00,0x00,0x00,0x48,0x29,0xc1,0x4c,0x8b,0x44,0xcd,0xc0,0x48,0xc1,0xe7,0x05,0x48,0x89,0xbd,0xf8,0xfe,0xff,0xff,0x48,0x8b,0x44,0xc5,0xb0,0x48,0x8b,0x4c,0xcd,0xb0,0xc5,0xf8,0x28,0x04,0x16,0xc5,0xf8,0x29,0x85,0x20,0xff,0xff,0xff,0xc5,0xfb}},
};
static int freeflight_timestep_contract(const unsigned char *image,size_t size) {
    for (unsigned i=0;i<sizeof freeflight_sites/sizeof *freeflight_sites;++i) {
        const struct FreeflightSite *s=&freeflight_sites[i];
        if (size<s->address+s->length || memcmp(image+s->address,s->bytes,s->length)) return 0;
    }
    for (unsigned i=0;i<sizeof freeflight_layouts/sizeof *freeflight_layouts;++i) {
        const struct FreeflightLayout *s=&freeflight_layouts[i];
        if (size<s->address+s->length || memcmp(image+s->address,s->bytes,s->length)) return 0;
    }
    return 1;
}
static void install_freeflight_timestep(unsigned char *image,unsigned char *code) {
    freeflight_image=image;
    for (unsigned i=0;i<sizeof freeflight_sites/sizeof *freeflight_sites;++i) {
        const struct FreeflightSite *s=&freeflight_sites[i];
        register_timestep_bridge(code+i*512,image+s->address,s->length,image+s->address+s->length,s->helper);
        jump(image+s->address,code+i*512);
        memset(image+s->address+14,0x90,s->length-14);
    }
}

static void **angular_stack_guard;
static uintptr_t angular_acceleration_resume;

static _Thread_local struct {
    void *clock;
    uint32_t frame, raw, expected;
    unsigned interval, phase;
    float scale;
    int valid;
} scaled_clock_step;

static void ABI scaled_clock_timestep(TimestepRegisters *r) {
    void *clock=(void *)r->rbx;
    if (!timestep_clock || clock!=*timestep_clock) return;
    const float ticks=real(clock,0x18)*8192, scale=real(clock,0x2c);
    const float largest=(274.f/8192*scale)*8192;
    if (fabsf(real(clock,0x34))>5e-7f || !(ticks>=9 && ticks<273) || ticks!=floorf(ticks) ||
        !(scale>=0) || !(largest<2147483648.f)) {
        scaled_clock_step.valid=0; return;
    }
    const uint32_t frame=(uint32_t)integer(clock,0x10), raw=(uint32_t)integer(clock,0x14);
    const uint32_t previous=(uint32_t)integer(clock,0x20);
    if (!scaled_clock_step.valid || scaled_clock_step.clock!=clock || scaled_clock_step.frame+1!=frame ||
        scaled_clock_step.raw+(uint32_t)ticks!=raw || scaled_clock_step.expected!=previous || scaled_clock_step.scale!=scale) {
        scaled_clock_step.interval=scaled_clock_step.phase=0;
    }
    unsigned remaining=(unsigned)ticks;
    uint32_t delta=0;
    while (remaining) {
        const unsigned index=scaled_clock_step.interval;
        const unsigned length=(index+1)*8192/30-index*8192/30;
        const float quantum=((float)length/8192*scale)*8192;
        const uint64_t budget=(uint32_t)quantum;
        const unsigned phase=scaled_clock_step.phase;
        const unsigned take=remaining<length-phase ? remaining : length-phase;
        delta+=(uint32_t)(((phase+take)*budget)/length-(phase*budget)/length);
        remaining-=take; scaled_clock_step.phase+=take;
        if (scaled_clock_step.phase==length) {
            scaled_clock_step.phase=0; scaled_clock_step.interval=(index+1)%30;
        }
    }
    scaled_clock_step.clock=clock; scaled_clock_step.frame=frame; scaled_clock_step.raw=raw;
    scaled_clock_step.expected=previous+delta; scaled_clock_step.scale=scale; scaled_clock_step.valid=1;
    /* Native adds this increment once; never rewrite an already published clock value. */
    r->rax=delta; r->resume+=4;
}

static int scaled_clock_contract(const unsigned char *image,size_t size) {
    const unsigned char code[]={0xc5,0xfa,0x2c,0xc1,0xc5,0xf8,0x2e,0xd0,0x0f,0x92,0x43,0x31,0x01,0x43,0x20};
    return size>0xa31b0c && !memcmp(image+0xa31afd,code,sizeof code);
}

static void install_scaled_clock(unsigned char *image,unsigned char *code) {
    register_timestep_bridge(code,image+0xa31afd,15,image+0xa31b0c,scaled_clock_timestep);
    jump(image+0xa31afd,code);
}

/* Native grounded control selection, including the combined gas/brake speed limit. */
static float ground_speed_curve(void *kart,void *data,int kind,float speed,float dt) {
    unsigned char tuned[0x30];
    if(kart_motion_speed_scale!=1) {
        memcpy(tuned,data,sizeof tuned);float top=real(data,12)*kart_motion_speed_scale;
        memcpy(tuned+12,&top,4);data=tuned;
    }
    return native_kart_speed(kart,data,kind,speed,dt);
}
static float ground_speed_target(void *kart,void *archetype,void *surface,float speed,float dt,int *mode) {
    const unsigned char *k=kart;
    const int gas=k[0x34]!=0,brake=k[0x35]!=0,control=k[0x36]!=0;
    const int coast=surface && ((unsigned char *)surface)[0x28];
    int kind=0,reverse=0,stopped=0;
    if(speed>50)kind=gas ? (control && brake ? 2 : 0) : (control || brake ? 2 : 1);
    else if(speed < -50) {
        reverse=1;kind=control ? (brake || !gas ? 0 : 2) : (brake || gas ? 2 : 1);
    } else if(!gas || (control && brake)) {reverse=control;stopped=!control;}
    if(kind==2 && coast)kind=1;
    *mode=reverse ? 2 : stopped;
    if(stopped)return 0;
    void *data=ptr(archetype,reverse ? 0x78 : 0x68);
    float magnitude=reverse ? -speed : speed,result;
    if(kind==0 && brake && !coast) {
        void *both=ptr(archetype,0x98);
        if(both) {
            const float threshold=real(both,0xc)*real(data,0xc);
            result=magnitude>threshold ?
                fmaxf(threshold,ground_speed_curve(kart,data,2,magnitude,dt*real(both,0x10))) :
                fminf(threshold,ground_speed_curve(kart,data,0,magnitude,dt*real(both,0x14)));
        } else result=ground_speed_curve(kart,data,2,ground_speed_curve(kart,data,0,magnitude,dt),dt);
    } else {
        if(!reverse && brake && control)dt*=1-(1-real(archetype,0xa0))*fabsf(real(kart,0x30));
        result=ground_speed_curve(kart,data,kind,magnitude,dt);
    }
    return reverse ? -result : result;
}

typedef struct {
    AccelerationStep time;
    void *archetype,*forward,*reverse,*both,*surface;
    float parameters[18],base_start,base_target,lateral;
    int controls;
} GroundSpeedStep;
static _Thread_local GroundSpeedStep ground_speed_steps[16];
static _Thread_local unsigned ground_speed_next;
static _Thread_local int ground_speed_pending;
static _Thread_local float ground_speed_output,ground_lateral_factor;
static _Thread_local int ground_lateral_pending;
static _Thread_local float motion_lateral_cap=1;
static float ground_speed_blend(void *surface,float start,float target,int mode) {
    if(!surface)return target;
    const float retention=real(surface,(mode ? start>target : target>start) ? 0x24 : 0x20);
    return start*retention+target*(1-retention);
}
static void ground_speed_next_target(GroundSpeedStep *s,void *kart) {
    int mode;
    s->base_target=ground_speed_target(kart,s->archetype,s->surface,s->time.start,
                                s->time.curve_scale*acceleration_interval(&s->time),&mode);
    s->time.target=ground_speed_blend(s->surface,s->time.start,s->base_target+s->parameters[15],mode);
    if(s->parameters[15]>0) s->time.target=s->time.target<s->parameters[17] ? s->parameters[17] : fminf(s->time.target,s->parameters[16]);
    /* The base path divides lateral velocity by normalized speed before applying retention. */
    s->lateral=s->surface ? real(s->surface,0x1c) :
        real(mode==2 ? s->reverse : s->forward,0x14)/fmaxf(1,fabsf(s->base_target+s->parameters[15])/real(s->forward,0xc));
}
static int ground_speed_timestep(TimestepRegisters *r) {
    ground_speed_pending=ground_lateral_pending=0;
    void *kart=(void*)r->r14,*archetype=(void*)r->r12,*surface=(void*)r->rsi;
    void *entity=ptr(kart,0x18),*clock=timestep_clock ? *timestep_clock : NULL;
    GroundSpeedStep *s=NULL;
    for(unsigned i=0;i<16;++i)if(ground_speed_steps[i].time.kart==kart){s=&ground_speed_steps[i];break;}
    const float frames=timestep_frames(),speed=r->ymm[9][0];
    if(!(frames<1) || !(kart_motion_dt>0) || !isfinite(kart_motion_dt) || !isfinite(speed) ||
       !((unsigned char*)kart)[0x214] || integer(kart,0xe0)==2 || !(real(kart,0x1a0)<=0) ||
       !isfinite(r->ymm[8][0]) || r->ymm[8][0]<0 || ptr(kart,0xa0))goto native;
    const float boost=r->ymm[8][0];
    void *boost_data=boost>0 ? (void *)r->rbx : NULL;
    /* Boost transitions need a common input tick; keep their existing correction path. */
    if(real(kart,0x174)!=boost || (boost>0 && (!boost_data || real(boost_data,0x38)!=boost)))goto native;
    void *damage_handle=ptr(kart,0x98);
    if(damage_handle && (((unsigned)integer(damage_handle,0)&0x30000000) ||
       (ptr(damage_handle,0x28) && integer(ptr(damage_handle,0x28),0x40)!=-1)))goto native;
    void *forward=ptr(archetype,0x68),*reverse=ptr(archetype,0x78),*both=ptr(archetype,0x98);
    if(!forward || !reverse || !entity || !clock)goto native;
    const float parameters[]={real(kart,0x190),real(kart,0x19c),real(kart,0x30),kart_motion_speed_scale,
        real(forward,0xc),real(reverse,0xc),real(archetype,0xa0),
        surface?real(surface,0x20):0,surface?real(surface,0x24):0,
        both?real(both,0xc):0,both?real(both,0x10):0,both?real(both,0x14):0,
        real(forward,0x14),real(reverse,0x14),surface?real(surface,0x1c):0,
        boost,boost_data?real(boost_data,0x4c):0,boost_data?real(boost_data,0x50):0};
    for(unsigned i=0;i<sizeof parameters/sizeof *parameters;++i)if(!isfinite(parameters[i]))goto native;
    if(!(parameters[0]>0) || !(parameters[1]>0) || !(parameters[4]>0) || !(parameters[5]>0) ||
       parameters[7]<0 || parameters[7]>1 || parameters[8]<0 || parameters[8]>1 ||
       parameters[12]<0 || parameters[13]<0 || parameters[14]<0)goto native;
    const int controls=((unsigned char*)kart)[0x34]+4*((unsigned char*)kart)[0x35]+
        16*((unsigned char*)kart)[0x36]+64*(surface && ((unsigned char*)surface)[0x28]);
    const unsigned frame=(unsigned)integer(clock,0x10),epoch=atomic_load_explicit(&acceleration_epoch,memory_order_relaxed);
    /* A new boost limit clamps immediately; do not turn that event into a gradual response. */
    if(s && s->time.valid && s->time.entity==entity && s->time.clock==clock && s->time.epoch==epoch &&
       frame-s->time.frame==1 && (s->parameters[16]!=parameters[16] || s->parameters[17]!=parameters[17]))goto native;
    const float curve_scale=kart_motion_dt/timestep_frame_dt(),ticks=timestep_frame_dt()*8192;
    const int quantized=fabsf(real(clock,0x34))<=5e-7f && ticks==floorf(ticks);
    if(!s){s=&ground_speed_steps[ground_speed_next++%16];s->time.valid=0;}
    AccelerationStep *t=&s->time;
    /* shortcut: Reset on changed inputs or external impulses; common tick scheduling is needed across those boundaries. */
    if(!t->valid || t->kart!=kart || t->entity!=entity || t->clock!=clock || t->epoch!=epoch ||
       frame-t->frame!=1 || t->state!=integer(kart,0xe0) || t->quantized!=quantized ||
       s->archetype!=archetype || s->forward!=forward || s->reverse!=reverse || s->both!=both ||
       s->surface!=surface || s->controls!=controls || memcmp(s->parameters,parameters,sizeof parameters) ||
       fabsf(t->expected-speed)>8*0x1p-23f*fmaxf(1,fabsf(r->ymm[14][0])+fabsf(r->ymm[15][0])+fabsf(r->ymm[13][0])) ||
       fabsf(t->curve_scale-curve_scale)>8*0x1p-23f*fabsf(curve_scale)) {
        t->kart=kart;t->entity=entity;t->clock=clock;t->epoch=epoch;t->state=integer(kart,0xe0);
        t->quantized=quantized;t->curve_scale=curve_scale;t->phase=0;t->interval=0;t->start=speed;
        s->archetype=archetype;s->forward=forward;s->reverse=reverse;s->both=both;s->surface=surface;
        s->controls=controls;memcpy(s->parameters,parameters,sizeof parameters);
        s->base_start=real(kart,0x170);ground_speed_next_target(s,kart);
    }
    t->frame=frame;t->valid=1;
    float span=acceleration_interval(t)*30;
    const double remaining=span-t->phase;
    ground_lateral_factor=powf(s->lateral,(float)(fmin(frames,remaining)/span));
    t->phase+=frames;
    if(t->phase>=span-8*0x1p-23) {
        t->start=t->target;s->base_start=s->base_target;t->phase-=span;t->interval=(t->interval+1)%30;
        span=acceleration_interval(t)*30;
        if(fabs(t->phase)<8*0x1p-23)t->phase=0;
        ground_speed_next_target(s,kart);
        ground_lateral_factor*=powf(s->lateral,(float)(t->phase/span));
    }
    const double fraction=t->phase/span;
    ground_speed_output=t->expected=t->phase ? (float)(t->start+(t->target-t->start)*fraction) : t->start;
    r->ymm[6][0]=t->phase ? (float)(s->base_start+(s->base_target-s->base_start)*fraction) : s->base_start;
    ground_speed_pending=1;
    return 1;
native:
    if(s)s->time.valid=0;
    return 0;
}
static void ABI ground_speed_project(TimestepRegisters *r) {
    motion_lateral_cap=r->rsi ? 1 : fmaxf(1,r->ymm[3][0]);
    if(ground_speed_pending) {
        r->ymm[0][0]=ground_speed_output;
        /* The reference lateral factor already includes this overspeed divisor. */
        r->ymm[3][0]=fminf(r->ymm[3][0],1);
        ground_lateral_pending=1;
    }
    ground_speed_pending=0;
}

/* Correct the selected scalar speed before native motion stores or projects it. */
static void ABI deceleration_timestep(TimestepRegisters *r) {
    const unsigned char *surface=(const void *)r->rsi;
    /* Surface longitudinal blending belongs to the complete motion map, not just its speed curve. */
    if (surface && (real(surface,0x20)!=0 || real(surface,0x24)!=0)) return;
    const float speed=r->ymm[9][0];
    void *kart=(void *)r->r14, *archetype=(void *)r->r12;
    void *data=ptr(archetype,speed<0 ? 0x78 : 0x68);
    unsigned char tuned[0x30];
    if (data && kart_motion_speed_scale!=1) {
        memcpy(tuned,data,sizeof tuned);
        const float top=real(data,12)*kart_motion_speed_scale;
        memcpy(tuned+12,&top,4); data=tuned;
    }
    float out;
    const int kind=((unsigned char *)kart)[0x35] && !(surface && surface[0x28]) ? 2 : 1;
    if (kart_curve_timestep(kart,data,kind,speed,kart_motion_dt,1,&out))
        r->ymm[6][0]=out;
}

static void ABI ground_speed_selected(TimestepRegisters *r) {
    if (!ground_speed_timestep(r)) deceleration_timestep(r);
}

static int deceleration_timestep_contract(const unsigned char *image,size_t size) {
    const unsigned char code[]={0xc4,0xc1,0x7a,0x11,0xb6,0x70,0x01,0,0,
                               0xc4,0x41,0x7a,0x11,0x86,0x74,0x01,0,0};
    return size>0xe46a67 && !memcmp(image+0xe46a55,code,sizeof code);
}

static void install_deceleration_timestep(unsigned char *image,unsigned char *code) {
    register_timestep_bridge(code,image+0xe46a55,18,image+0xe46a67,ground_speed_selected);
    jump(image+0xe46a55,code);
}

static const unsigned char ground_speed_project_code[]={
    0xc5,0xfa,0x59,0xd1,0xc5,0xfa,0x59,0xce,0xc5,0x7a,0x59,0xe5,0x48,0x85,0xf6};
/* Native boost load, cap/floor order, normalization and previous-boost subtraction. */
static const struct {size_t address,length;unsigned char bytes[40];} ground_boost_layout[]={
    {0xe4678e,5,{0xc5,0x7a,0x10,0x43,0x38}},
    {0xe46e2d,39,{0xc5,0xfa,0x10,0x7b,0x4c,0xc5,0xfa,0x10,0x53,0x50,0xc5,0xf8,0x2e,0xc7,0x77,0x04,0xc5,0xf8,0x28,0xf8,0xc5,0xf8,0x2e,0xd0,0x77,0x04,0xc5,0xf8,0x28,0xd7,0xc5,0xf8,0x28,0xc2,0xe9,0x7b,0xfc,0xff,0xff}},
    {0xe46a67,4,{0xc5,0xba,0x58,0xc6}},
    {0xe46a7b,10,{0x49,0x8b,0x44,0x24,0x68,0xc5,0xf2,0x5e,0x58,0x0c}},
    {0xe4a2d6,10,{0xc4,0xc1,0x72,0x5c,0x8c,0x24,0x74,0x01,0x00,0x00}},
};
static int ground_speed_project_contract(const unsigned char *image,size_t size) {
    for(unsigned i=0;i<sizeof ground_boost_layout/sizeof *ground_boost_layout;++i) {
        if(size<ground_boost_layout[i].address+ground_boost_layout[i].length ||
           memcmp(image+ground_boost_layout[i].address,ground_boost_layout[i].bytes,ground_boost_layout[i].length))return 0;
    }
    return size>=0xe46ade && !memcmp(image+0xe46acf,ground_speed_project_code,sizeof ground_speed_project_code);
}
static void install_ground_speed_project(unsigned char *image,unsigned char *code) {
    register_timestep_bridge(code,ground_speed_project_code,sizeof ground_speed_project_code,
                             image+0xe46ade,ground_speed_project);
    jump(image+0xe46acf,code); image[0xe46add]=0x90;
}

static void ABI angular_acceleration_timestep(TimestepRegisters *r) {
    const double frames=timestep_frames();
    const double error=r->ymm[3][0], threshold=r->ymm[5][0];
    if (!(frames>0 && frames<1) || r->ymm[4][0]!=1 ||
        !(error>threshold && threshold>0) || !isfinite(error)) return;
    const double surface=r->rax ? 1-real((const void *)r->rax,0x18) : 1;
    const double reference=real((const void *)(r->rbp-0x74),0)*surface/frames;
    if (!(reference>0) || !isfinite(reference)) return;
    double remaining=0;
    if (reference<threshold) {
        /* Fractional iterations preserve the native geometric-to-linear transition. */
        const double logq=log1p(-reference/threshold), linear=threshold/reference;
        const double n=ceil(log(error/threshold)/-logq);
        const double time=n+error*exp(n*logq)/reference-frames;
        if (time>linear) {
            const double k=ceil(time-linear);
            remaining=reference*(time-k)*exp(-k*logq);
        } else remaining=fmax(0,reference*time);
    }
    memset(r->ymm[5],0,sizeof r->ymm[5]);
    r->ymm[5][0]=(float)fmax(0,error-remaining);
    memset(r->ymm[6],0,sizeof r->ymm[6]);
    r->r12=(uintptr_t)*angular_stack_guard;
    r->resume=angular_acceleration_resume;
}

static int angular_timestep_contract(const unsigned char *image,size_t size) {
    if (size<0xe4598a || memcmp(image+0xe45951,"\xc5\xe2\x5e\xed\x4c\x8b\x25",7) ||
        memcmp(image+0xe4595c,"\xc5\xc8\x57\xf6",4) ||
        memcmp(image+0xe45980,"\xc5\xf8\x2e\xdd\x0f\x87\x8a\x05\0\0",10)) return 0;
    int32_t displacement; memcpy(&displacement,image+0xe45958,4);
    const size_t guard=(size_t)(0xe4595c+(int64_t)displacement);
    return guard<=size-sizeof(void *);
}

static void install_angular_timestep(unsigned char *image,unsigned char *code) {
    int32_t displacement; memcpy(&displacement,image+0xe45958,4);
    angular_stack_guard=(void **)(image+0xe4595c+(int64_t)displacement);
    angular_acceleration_resume=(uintptr_t)(image+0xe45980);
    unsigned char displaced[22];
    memcpy(displaced,image+0xe45951,4);
    memcpy(displaced+4,"\x49\xbc",2); memcpy(displaced+6,&angular_stack_guard,8);
    memcpy(displaced+14,"\x4d\x8b\x24\x24",4);
    memcpy(displaced+18,image+0xe4595c,4);
    register_timestep_bridge(code,displaced,sizeof displaced,image+0xe45960,angular_acceleration_timestep);
    jump(image+0xe45951,code); image[0xe4595f]=0x90;
}

static unsigned char *vfx_timestep_image;
typedef void (ABI *VfxWorker)(void *,void *);
static VfxWorker native_vfx_worker;
static _Thread_local void *vfx_worker_manager;
static void ABI vfx_worker(void *owner,void *job) {
    void *previous=vfx_worker_manager;
    vfx_worker_manager=ptr(owner,0);
    native_vfx_worker(owner,job);
    vfx_worker_manager=previous;
}

/* Only the proven rebuilt-velocity stack; other operators retain native behavior. */
static int vfx_rebuilt_velocity(void *op,void *context) {
    static const size_t kinds[]={0x24700e0,0x24707c0,0x2470500,0x24604e8,0x246e560,
        0x245d1f8,0x246e560,0x24612c0,0x246ad00,0x246bd80,0x246bd80,0x24621e0,
        0x2462340,0x2462340,0x24661c8};
    if (!vfx_worker_manager || ptr(context,0x370)!=vfx_worker_manager || !ptr(context,0x3c0)) return 0;
    void *primitive=ptr(context,0x388), *instance=ptr(context,0x390), *data=ptr(context,0x380);
    if (!primitive || !instance || !data || (integer(instance,0x10)&0xa) ||
        ptr(primitive,0x10)!=data || integer(context,8)!=0 || ptr(context,0)!=(unsigned char *)context+0x10) return 0;
    /* Commit exports only track-zero quaternion/position/velocity into stream[0:40]. */
    if (integer(data,0x54)!=0xb || integer(data,0x58)!=0x30 || integer(data,0x5c)!=0xb ||
        integer(data,0x60)!=0xa || integer(data,0x64)!=0) return 0;
    void *list=ptr(data,0x70);
    if (!list || integer(list,0xc)!=15) return 0;
    void **ops=ptr(list,0x20);
    if (!ops || ops[3]!=op) return 0;
    for (unsigned i=0;i<15;++i)
        if (!ops[i] || ptr(ops[i],0)!=vfx_timestep_image+kinds[i] || !(integer(ops[i],0xc)&1)) return 0;
    if (integer(ops[0],0x10)!=2 || integer(ops[1],0x10)!=5 || integer(ops[1],0x14)!=2 ||
        integer(ops[2],0x10)!=0 || integer(ops[2],0x14)!=2 || integer(op,0x10)!=0 ||
        integer(ops[4],0x10)!=8 || integer(ops[6],0x10)!=6) return 0;
    /* The draw prelude may update its frame state, but not the committed motion. */
    if (integer(ops[14],0x30)!=0x28 || ptr(ops[14],0x28)) return 0;
    return real(op,0x14)>=0 && isfinite(real(op,0x14));
}

static void ABI vfx_drag_timestep(TimestepRegisters *r) {
    const float frames=timestep_frames();
    if (!(frames<1)) return;
    void *context=(void *)r->r15, *op=ptr((const unsigned char *)r->rbp-0xc8,0);
    const float dt=real(context,0x3dc), reference=dt/frames;
    if (!(dt>0) || !isfinite(dt) || !isfinite(reference) || !vfx_rebuilt_velocity(op,context)) return;
    for (unsigned i=0;i<4;++i) r->ymm[2][i]=reference;
    memset(r->ymm[2]+4,0,4*sizeof(float));
    r->resume+=9; /* Replace only vbroadcastss; native multiplication and store stay intact. */
}

static const unsigned char vfx_worker_code[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
    0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x68,0x01,0,0};
static const unsigned char vfx_drag_code[]={0xc4,0xc2,0x79,0x18,0x97,0xdc,0x03,0,0,
    0xc5,0xf8,0x59,0xc2,0xc5,0xf8,0x11,0x45,0x90};
static int vfx_timestep_contract(const unsigned char *image,size_t size) {
    return size>=0x2470900 && !memcmp(image+0x2b3670,vfx_worker_code,sizeof vfx_worker_code) &&
        !memcmp(image+0x2c4f98,vfx_drag_code,sizeof vfx_drag_code) &&
        !memcmp(image+0x2b3ac0,"\x49\x8b\x37\x4c\x89\xf7\x4c\x89\xe9\x49\x89\xd8\x4d\x89\xe1\xe8\x0c\xfa\xff\xff",20) &&
        !memcmp(image+0x2b2673,"\x48\x8b\x45\x10\xc4\xc1\x7a\x11\x85\xdc\x03\0\0",13) &&
        !memcmp(image+0x2b2687,"\x4d\x89\xb5\xc0\x03\0\0",7);
}
static void install_vfx_timestep(unsigned char *image,unsigned char *code) {
    vfx_timestep_image=image;
    memcpy(code+512,vfx_worker_code,sizeof vfx_worker_code);
    jump(code+512+sizeof vfx_worker_code,image+0x2b3670+sizeof vfx_worker_code);
    native_vfx_worker=(VfxWorker)(code+512);
    register_timestep_bridge(code,vfx_drag_code,sizeof vfx_drag_code,image+0x2c4faa,vfx_drag_timestep);
    jump(image+0x2b3670,vfx_worker); memset(image+0x2b3670+14,0x90,sizeof vfx_worker_code-14);
    jump(image+0x2c4f98,code); memset(image+0x2c4f98+14,0x90,sizeof vfx_drag_code-14);
}

static const unsigned char camera_height_code[]={
    0xc4,0xc1,0x7a,0x10,0x45,0x14,0xc4,0xc1,0x7a,0x10,0xaf,0x00,0x02,0x00,0x00,0xc5,
    0xfa,0x10,0x9d,0x54,0xff,0xff,0xff,0xc4,0xc1,0x7a,0x10,0xb7,0xfc,0x01,0x00,0x00,
    0xc5,0xfa,0x10,0x10,0x49,0x8d,0xbf,0xf4,0x01,0x00,0x00,0x49,0x8d,0xb7,0x04,0x02,
    0x00,0x00,0xc5,0xfa,0x59,0xc0,0xc5,0xfa,0x59,0x05,0xfa,0xed,0x01,0x01,0xc5,0xd2,
    0x5c,0xe4,0xc5,0xfa,0x59,0xe4,0xc5,0xfa,0x59,0xe4,0xc5,0xfa,0x59,0xc3,0xc5,0xfa,
    0x58,0x05,0xc6,0xed,0x01,0x01,0xc5,0xda,0x59,0xe3,0xc5,0xca,0x5c,0xe4,0xc5,0xfa,
    0x59,0xc0,0xc5,0xda,0x5e,0xc0,0xc5,0xfa,0x59,0xe3,0xc5,0xd2,0x58,0xe4,0xc4,0xc1,
    0x7a,0x11,0xa7,0x00,0x02,0x00,0x00,0xc4,0xc1,0x7a,0x11,0x87,0xfc,0x01,0x00,0x00
};

static const unsigned char camera_pitch_code[]={
    0xc5,0xea,0x59,0xc2,0xc5,0xfa,0x59,0x05,0x4d,0xed,0x01,0x01,0xc4,0xc1,0x79,0x6e,
    0xdc,0xc5,0xfa,0x10,0xa5,0x54,0xff,0xff,0xff,0xc4,0xc1,0x7a,0x10,0x8f,0x10,0x02,
    0x00,0x00,0xc5,0xd2,0x5c,0xdb,0xc5,0xfa,0x59,0xdb,0xc5,0xfa,0x59,0xdb,0xc5,0xfa,
    0x59,0xc4,0xc5,0xfa,0x58,0x05,0x03,0xed,0x01,0x01,0xc5,0xe2,0x59,0xdc,0xc5,0xf2,
    0x5c,0xcb,0xc5,0xfa,0x59,0xc0,0xc5,0xf2,0x5e,0xc0,0xc5,0xfa,0x59,0xcc,0xc5,0xd2,
    0x58,0xc9,0xc4,0xc1,0x7a,0x11,0x8f,0x0c,0x02,0x00,0x00,0xc4,0xc1,0x7a,0x11,0x87,
    0x10,0x02,0x00,0x00
};

static const unsigned char camera_planar_code[]={
    0xc5,0xfa,0x10,0x27,0xc5,0xfa,0x10,0x6f,0x04,0xc5,0xea,0x59,0xd2,0xc5,0xea,0x59,
    0x15,0xab,0xda,0x20,0x01,0xc5,0xfa,0x10,0x3d,0xa7,0xda,0x20,0x01,0xc5,0xfa,0x10,
    0x36,0xc5,0x7a,0x10,0x46,0x04,0xc5,0xda,0x5c,0xc0,0xc5,0xd2,0x5c,0xc9,0xc5,0xea,
    0x59,0xc0,0xc5,0xea,0x59,0xc9,0xc5,0xea,0x59,0xc0,0xc5,0xea,0x59,0xc9,0xc5,0xea,
    0x59,0xd3,0xc5,0xea,0x58,0xd7,0xc5,0xfa,0x59,0xc3,0xc5,0xf2,0x59,0xcb,0xc5,0xca,
    0x5c,0xc0,0xc5,0xba,0x5c,0xc9,0xc5,0xea,0x59,0xd2,0xc5,0xc2,0x5e,0xd2,0xc5,0xea,
    0x59,0xc0,0xc5,0xea,0x59,0xc9,0xc5,0xfa,0x59,0xd3,0xc5,0xf2,0x59,0xdb,0xc5,0xda,
    0x58,0xd2,0xc5,0xd2,0x58,0xdb,0xc5,0xfa,0x11,0x17,0xc5,0xfa,0x11,0x5f,0x04,0xc5,
    0xfa,0x11,0x06,0xc5,0xfa,0x11,0x4e,0x04,0xc3
};

static uintptr_t camera_height_resume, camera_planar_resume, camera_pitch_resume;

#define CAMERA_SCALAR_CAPACITY 16
struct CameraScalarState {
    uintptr_t key;
    uint32_t frame;
    float anchor_position, anchor_velocity, previous_target, stiffness, reference_dt;
    float last_position, last_velocity;
    double phase, elapsed_dt;
    int valid;
};
static _Thread_local struct CameraScalarState camera_scalar_states[CAMERA_SCALAR_CAPACITY];
static _Thread_local unsigned camera_scalar_next;

static struct CameraScalarState *camera_scalar_state(uintptr_t key) {
    for (unsigned i=0;i<CAMERA_SCALAR_CAPACITY;++i)
        if (camera_scalar_states[i].key==key)
            return &camera_scalar_states[i];
    for (unsigned i=0;i<CAMERA_SCALAR_CAPACITY;++i)
        if (!camera_scalar_states[i].valid) return &camera_scalar_states[i];
    struct CameraScalarState *state=&camera_scalar_states[camera_scalar_next++%CAMERA_SCALAR_CAPACITY];
    memset(state,0,sizeof *state);
    return state;
}

static void camera_spring_advance(float *position, float *velocity, float target,
                                  float stiffness, float reference_dt, double phase) {
    const double g=1/(1+(double)stiffness*reference_dt), decay=pow(g,phase);
    const double b=reference_dt*phase*g;
    const double error=(double)*position-target, speed=*velocity, k=stiffness;
    *position=(float)(target+decay*((1+b*k)*error+b*speed));
    *velocity=(float)(decay*((1-b*k)*speed-b*k*k*error));
}

static int camera_spring_step(float *position, float *velocity, float target,
                              float stiffness, float dt, uintptr_t key) {
    const float frames=timestep_frames();
    void *clock=timestep_clock ? *timestep_clock : NULL;
    if (!(frames>0 && frames<1) || !(dt>0) || !isfinite(dt) || !isfinite(stiffness) ||
        !isfinite(*position) || !isfinite(*velocity) || !isfinite(target) || !clock) {
        if (position && velocity) camera_scalar_state(key)->valid=0;
        return 0;
    }
    const float reference_dt=dt/frames;
    if (!(reference_dt>0) || !isfinite(reference_dt)) {
        camera_scalar_state(key)->valid=0;
        return 0;
    }
    const uint32_t frame=(uint32_t)integer(clock,0x10);
    struct CameraScalarState *state=camera_scalar_state(key);
    const int continuous=state->valid && state->frame+1==frame &&
        *position==state->last_position && *velocity==state->last_velocity &&
        state->stiffness==stiffness &&
        fabsf(state->reference_dt-reference_dt)<=8*FLT_EPSILON*fmaxf(1,fabsf(reference_dt));
    if (!continuous) {
        state->key=key;
        state->anchor_position=*position; state->anchor_velocity=*velocity;
        state->previous_target=target; state->stiffness=stiffness;
        state->reference_dt=reference_dt; state->phase=0; state->elapsed_dt=0; state->valid=1;
    }
    float next_position=state->anchor_position, next_velocity=state->anchor_velocity;
    double phase=state->phase+frames;
    const double phase_tolerance=30.0/8192.0+8*FLT_EPSILON;
    state->elapsed_dt+=dt;
    if (fabs(phase-1)<=phase_tolerance) {
        camera_spring_advance(&next_position,&next_velocity,target,stiffness,(float)state->elapsed_dt,1);
        state->anchor_position=next_position; state->anchor_velocity=next_velocity;
        state->elapsed_dt=0; phase=0;
    } else if (phase>=1) {
        const double boundary=(1-state->phase)/frames;
        const float boundary_target=state->previous_target+(target-state->previous_target)*(float)boundary;
        const float boundary_dt=(float)(state->elapsed_dt-dt*(1-boundary));
        camera_spring_advance(&next_position,&next_velocity,boundary_target,stiffness,boundary_dt,1);
        state->anchor_position=next_position; state->anchor_velocity=next_velocity;
        state->elapsed_dt=(double)dt*(1-boundary); phase-=1;
    }
    if (phase>8*FLT_EPSILON)
        camera_spring_advance(&next_position,&next_velocity,target,stiffness,reference_dt,phase);
    else phase=0;
    *position=next_position; *velocity=next_velocity;
    state->phase=phase; state->frame=frame; state->previous_target=target;
    state->last_position=next_position; state->last_velocity=next_velocity;
    return 1;
}

typedef void (ABI *CameraDirectionSpring)(float *,float *,float,float,float,float,float);
typedef void (ABI *CameraDirectionWrap)(float *);
static CameraDirectionSpring native_camera_direction;
static CameraDirectionWrap camera_direction_wrap;
static float camera_frame_phase(float frame_dt) {
    if (!(frame_dt>0) || !isfinite(frame_dt)) return 0;
    const float ticks=frame_dt*8192.f;
    if (ticks>=272.9f && ticks<274.1f) return 1.f;
    return frame_dt*30.f;
}
#define CAMERA_DIRECTION_CAPACITY 8
struct CameraDirectionState {
    uintptr_t position,velocity;
    float anchor_position[3],anchor_velocity[3],last_position[3],last_velocity[3];
    float previous_target[3],previous_error[3],damping,reference_dt;
    double phase;
    uint32_t frame;
    unsigned char valid;
};
static _Thread_local struct CameraDirectionState camera_direction_states[CAMERA_DIRECTION_CAPACITY];
static _Thread_local unsigned camera_direction_next;

static struct CameraDirectionState *camera_direction_state(uintptr_t position,uintptr_t velocity) {
    for (unsigned i=0;i<CAMERA_DIRECTION_CAPACITY;++i)
        if (camera_direction_states[i].position==position && camera_direction_states[i].velocity==velocity)
            return &camera_direction_states[i];
    for (unsigned i=0;i<CAMERA_DIRECTION_CAPACITY;++i)
        if (!camera_direction_states[i].valid) return &camera_direction_states[i];
    struct CameraDirectionState *state=&camera_direction_states[camera_direction_next++%CAMERA_DIRECTION_CAPACITY];
    memset(state,0,sizeof *state);
    return state;
}

static int camera_direction_interpolate(float position[3],float velocity[3],const float target[3],
                                        float damping,float reference_dt,double phase) {
    float error[3],equivalent_target[3];
    for (unsigned i=0;i<3;++i) {
        const float raw=position[i]-target[i];
        if (!isfinite(raw) || fabsf(raw)>2.5f || !isfinite(velocity[i])) return 0;
        error[i]=raw;
    }
    camera_direction_wrap(error);
    const double k=(float)(damping*damping*500.f),dt=reference_dt*phase;
    const double g=1/(1+k*reference_dt),decay=pow(g,phase),b=dt*g;
    for (unsigned i=0;i<3;++i) {
        equivalent_target[i]=position[i]-error[i];
        const double e=error[i],speed=velocity[i];
        position[i]=(float)(equivalent_target[i]+decay*((1+b*k)*e+b*speed));
        velocity[i]=(float)(decay*((1-b*k)*speed-b*k*k*e));
        if (!isfinite(position[i]) || !isfinite(velocity[i])) return 0;
    }
    return 1;
}

static void ABI camera_direction_timestep(float *position,float *velocity,float tx,float ty,float tz,
                                          float damping,float dt) {
    const float frame_dt=timestep_frame_dt(),frames=camera_frame_phase(frame_dt);
    void *clock=timestep_clock ? *timestep_clock : NULL;
    if (!(frames>0 && frames<=3) || frames==1 || !(dt>0) ||
        !isfinite(dt) || !isfinite(damping) || !clock || !position || !velocity) {
        if (native_camera_direction) native_camera_direction(position,velocity,tx,ty,tz,damping,dt);
        return;
    }
    const float target[3]={tx,ty,tz};
    for (unsigned i=0;i<3;++i)
        if (!isfinite(position[i]) || !isfinite(velocity[i]) || !isfinite(target[i])) {
            native_camera_direction(position,velocity,tx,ty,tz,damping,dt);
            return;
        }
    const float reference_dt=dt/frames;
    const uint32_t frame=(uint32_t)integer(clock,0x10);
    struct CameraDirectionState *state=camera_direction_state((uintptr_t)position,(uintptr_t)velocity);
    const int continuous=state->valid && state->frame+1==frame &&
        !memcmp(position,state->last_position,sizeof state->last_position) &&
        !memcmp(velocity,state->last_velocity,sizeof state->last_velocity) &&
        fabsf(reference_dt-state->reference_dt)<=8*FLT_EPSILON*fmaxf(1,fabsf(reference_dt));
    if (!continuous) {
        state->position=(uintptr_t)position; state->velocity=(uintptr_t)velocity;
        memcpy(state->anchor_position,position,sizeof state->anchor_position);
        memcpy(state->anchor_velocity,velocity,sizeof state->anchor_velocity);
        memcpy(state->previous_target,target,sizeof state->previous_target);
        state->damping=damping; state->reference_dt=reference_dt; state->phase=0;
        state->valid=1;
    }
    float raw_error[3];
    for (unsigned i=0;i<3;++i) raw_error[i]=position[i]-target[i];
    if (continuous) {
        double dot=0;
        for (unsigned i=0;i<3;++i) dot+=(double)state->previous_target[i]*target[i];
        if (dot<0.87758256 || fabsf(damping-state->damping)>8*FLT_EPSILON*fmaxf(1,fabsf(damping))) {
            state->valid=0;
            native_camera_direction(position,velocity,tx,ty,tz,damping,dt);
            return;
        }
        float wrapped_error[3]; memcpy(wrapped_error,raw_error,sizeof wrapped_error);
        camera_direction_wrap(wrapped_error);
        for (unsigned i=0;i<3;++i)
            if (fabsf(raw_error[i])>2.5f || fabsf(wrapped_error[i]-state->previous_error[i])>0.5f) {
                state->valid=0;
                native_camera_direction(position,velocity,tx,ty,tz,damping,dt);
                return;
            }
    }
    float next_position[3],next_velocity[3];
    memcpy(next_position,state->anchor_position,sizeof next_position);
    memcpy(next_velocity,state->anchor_velocity,sizeof next_velocity);
    double phase=state->phase+frames;
    if (phase>=1) {
        float boundary_target[3],norm_a=hypotf(hypotf(state->previous_target[0],state->previous_target[1]),state->previous_target[2]);
        float norm_b=hypotf(hypotf(tx,ty),tz);
        if (!(norm_a>0) || !(norm_b>0) || !isfinite(norm_a) || !isfinite(norm_b)) {
            state->valid=0;
            native_camera_direction(position,velocity,tx,ty,tz,damping,dt);
            return;
        }
        float a[3],b[3];
        for (unsigned i=0;i<3;++i) { a[i]=state->previous_target[i]/norm_a; b[i]=target[i]/norm_b; }
        double dot=0; for (unsigned i=0;i<3;++i) dot+=(double)a[i]*b[i];
        if (dot< -0.95) {
            state->valid=0;
            native_camera_direction(position,velocity,tx,ty,tz,damping,dt);
            return;
        }
        if (dot>1) dot=1;
        const double angle=acos(dot),sine=sin(angle);
for (double at=1-state->phase;at<=frames+1e-10;at+=1) {
const double sample=at/frames;
if (angle<1e-6 || fabs(sine)<1e-8) {
for (unsigned i=0;i<3;++i) boundary_target[i]=(float)(a[i]+(b[i]-a[i])*sample);
} else {
const double wa=sin((1-sample)*angle)/sine,wb=sin(sample*angle)/sine;
for (unsigned i=0;i<3;++i) boundary_target[i]=(float)(wa*a[i]+wb*b[i]);
}
const float boundary_damping=state->damping+(damping-state->damping)*(float)sample;
native_camera_direction(next_position,next_velocity,boundary_target[0],boundary_target[1],
boundary_target[2],boundary_damping,reference_dt);
phase-=1;
}
        memcpy(state->anchor_position,next_position,sizeof next_position);
        memcpy(state->anchor_velocity,next_velocity,sizeof next_velocity);
    }
    if (phase>0 && !camera_direction_interpolate(next_position,next_velocity,target,damping,reference_dt,phase)) {
        state->valid=0;
        native_camera_direction(position,velocity,tx,ty,tz,damping,dt);
        return;
    }
    memcpy(position,next_position,sizeof next_position);
    memcpy(velocity,next_velocity,sizeof next_velocity);
    state->phase=phase; state->frame=frame; state->damping=damping;
    memcpy(state->previous_target,target,sizeof state->previous_target);
    for (unsigned i=0;i<3;++i) state->previous_error[i]=position[i]-target[i];
    camera_direction_wrap(state->previous_error);
    memcpy(state->last_position,position,sizeof state->last_position);
    memcpy(state->last_velocity,velocity,sizeof state->last_velocity);
    /* Keep native stack-canary validation on every rendered camera update. */
    native_camera_direction(position,velocity,tx,ty,tz,damping,0);
}

static void camera_scalar_register(TimestepRegisters *r, unsigned reg, float value) {
    memset(r->ymm[reg],0,sizeof r->ymm[reg]);
    r->ymm[reg][0]=value;
}

static void ABI camera_height_timestep(TimestepRegisters *r) {
    unsigned char *camera=(unsigned char *)r->r15;
    const float dt=real((const unsigned char *)r->rbp-0xac,0);
    const float damping=real((const void *)r->r13,0x14);
    const float old_position=real(camera,0x200), old_velocity=real(camera,0x1fc);
    float position=old_position, velocity=old_velocity;
    if (!camera_spring_step(&position,&velocity,r->ymm[4][0],damping*damping*500.f,dt,(uintptr_t)(camera+0x200))) return;
    memcpy(camera+0x200,&position,4); memcpy(camera+0x1fc,&velocity,4);
    r->rdi=r->r15+0x1f4; r->rsi=r->r15+0x204;
    camera_scalar_register(r,0,velocity);
    camera_scalar_register(r,2,real((const void *)r->rax,0));
    camera_scalar_register(r,3,dt);
    camera_scalar_register(r,4,position);
    camera_scalar_register(r,5,old_position);
    camera_scalar_register(r,6,old_velocity);
    r->resume=camera_height_resume;
}

static void ABI camera_planar_timestep(TimestepRegisters *r) {
    float position[2], velocity[2], old_position[2], old_velocity[2];
    memcpy(position,(const void *)r->rdi,sizeof position);
    memcpy(velocity,(const void *)r->rsi,sizeof velocity);
    memcpy(old_position,position,sizeof position); memcpy(old_velocity,velocity,sizeof velocity);
    const float damping=r->ymm[2][0], dt=r->ymm[3][0];
    const float targets[]={r->ymm[7][0],r->ymm[1][0]};
    for (unsigned i=0;i<2;++i)
        if (!camera_spring_step(&position[i],&velocity[i],targets[i],damping*damping*500.f,dt,r->rdi+i*4)) return;
    memcpy((void *)r->rdi,position,sizeof position);
    memcpy((void *)r->rsi,velocity,sizeof velocity);
    const float registers[]={velocity[0],velocity[1],position[0],position[1],
                             old_position[0],old_position[1],old_velocity[0],1,old_velocity[1]};
    for (unsigned i=0;i<9;++i) camera_scalar_register(r,i,registers[i]);
    uint32_t bits; memcpy(&bits,&position[1],4); r->rax=bits;
    r->resume=camera_planar_resume;
}

static void ABI camera_pitch_timestep(TimestepRegisters *r) {
    unsigned char *camera=(unsigned char *)r->r15;
    float target; const uint32_t bits=(uint32_t)r->r12; memcpy(&target,&bits,4);
    const float dt=real((const unsigned char *)r->rbp-0xac,0), k=r->ymm[0][0];
    const float old_position=real(camera,0x20c);
    float position=old_position, velocity=real(camera,0x210);
    if (!camera_spring_step(&position,&velocity,target,k,dt,(uintptr_t)(camera+0x20c))) return;
    memcpy(camera+0x20c,&position,4); memcpy(camera+0x210,&velocity,4);
    camera_scalar_register(r,0,velocity);
    camera_scalar_register(r,1,position);
    camera_scalar_register(r,3,k*(k*(old_position-target))*dt);
    camera_scalar_register(r,4,dt);
    r->resume=camera_pitch_resume;
}

static int camera_constant(const unsigned char *image, size_t size, size_t displacement, float value) {
    int32_t relative; memcpy(&relative,image+displacement,4);
    const int64_t target=(int64_t)displacement+4+relative;
    return target>=0 && (uint64_t)target<=size-4 && !memcmp(image+target,&value,4);
}

static int camera_spring_contract(const unsigned char *image, size_t size, size_t address,
                                  const unsigned char *expected, size_t length,
                                  size_t first, size_t second) {
    return !memcmp(image+address,expected,first) &&
        !memcmp(image+address+first+4,expected+first+4,second-first-4) &&
        !memcmp(image+address+second+4,expected+second+4,length-second-4) &&
        camera_constant(image,size,address+first,500.f) &&
        camera_constant(image,size,address+second,1.f);
}

static int camera_direction_contract(const unsigned char *image,size_t size) {
    const unsigned char prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x28,0x4c,0x8b,0x3d};
    if (size<0xa997c0+20 || memcmp(image+0xa997c0,prefix,sizeof prefix)) return 0;
    int32_t displacement; memcpy(&displacement,image+0xa997c0+16,4);
    const int64_t guard=(int64_t)0xa997c0+20+displacement;
    return guard>=0 && (uint64_t)guard<=size-sizeof(void *);
}

static int camera_timestep_contract(const unsigned char *image, size_t size) {
    if (size<0x1cb0000) return 0;
    return camera_spring_contract(image,size,0xc89b54,camera_height_code,sizeof camera_height_code,58,82) &&
        camera_spring_contract(image,size,0xc89c33,camera_pitch_code,sizeof camera_pitch_code,8,54) &&
        camera_spring_contract(image,size,0xa99670,camera_planar_code,sizeof camera_planar_code,17,25) &&
        camera_direction_contract(image,size) &&
        !memcmp(image+0xc89bd4,"\xc5\xf8\x28\xc7\xe8\x93\xfa\xe0\xff\x41\x8b\x87\xf8\x01\0\0",16) &&
        !memcmp(image+0xc89be4,"\x41\x8b\x8f\0\x02\0\0\x41\x8b\x9f\xf4\x01\0\0",14) &&
        !memcmp(image+0xc89c97,"\xc5\xf2\x59\x05",4) && camera_constant(image,size,0xc89c9b,.01745329238474369f) &&
        !memcmp(image+0xc89c9f,"\xc5\xfa\x59\x05",4) && camera_constant(image,size,0xc89ca3,-.5f) &&
        !memcmp(image+0xc89ca7,"\xc5\xfa\x11\x85\x54\xff\xff\xff",8);
}

static void install_camera_timestep(unsigned char *image, unsigned char *code) {
    camera_height_resume=(uintptr_t)(image+0xc89bd4);
    camera_planar_resume=(uintptr_t)(image+0xc89be4);
    camera_pitch_resume=(uintptr_t)(image+0xc89c97);
    /* The original call moves to an absolute call literal; no RIP-relative operands move. */
    unsigned char planar[27]; memcpy(planar,image+0xc89bd4,4);
    memcpy(planar+4,"\xff\x15\x02\0\0\0\xeb\x08",8);
    const void *native=image+0xa99670; memcpy(planar+12,&native,8);
    memcpy(planar+20,image+0xc89bdd,7);
    register_timestep_bridge(code,image+0xc89b54,15,image+0xc89b63,camera_height_timestep);
    register_timestep_bridge(code+512,planar,sizeof planar,image+0xc89be4,camera_planar_timestep);
    register_timestep_bridge(code+1024,image+0xc89c44,17,image+0xc89c55,camera_pitch_timestep);
    jump(image+0xc89b54,code); image[0xc89b62]=0x90;
    jump(image+0xc89bd4,code+512); memset(image+0xc89be2,0x90,2);
    jump(image+0xc89c44,code+1024); memset(image+0xc89c52,0x90,3);
    unsigned char *trampoline=code+1536;
    memcpy(trampoline,image+0xa997c0,13);
    memcpy(trampoline+13,"\x49\xbb",2);
    int32_t displacement; memcpy(&displacement,image+0xa997c0+16,4);
    const uintptr_t guard=(uintptr_t)(image+0xa997c0+20+displacement);
    memcpy(trampoline+15,&guard,sizeof guard);
    memcpy(trampoline+23,"\x4d\x8b\x3b",3);
    jump(trampoline+26,image+0xa997d4);
    native_camera_direction=(CameraDirectionSpring)trampoline;
    camera_direction_wrap=(CameraDirectionWrap)(image+0xa98310);
    jump(image+0xa997c0,camera_direction_timestep);
}

static float ABI lateral_retention(float retention, float dt) {
    const float cap=motion_lateral_cap;motion_lateral_cap=1;
    if(ground_lateral_pending) {ground_lateral_pending=0;return ground_lateral_factor;}
    const float frames=timestep_frames();
    if (!(dt>0) || !isfinite(dt) || !(frames<1)) return retention;
    if (cap>1 && isfinite(cap) && retention>=0 && isfinite(retention))
        return cap*powf(retention/cap,frames);
    return retention>0 && retention<1 ? powf(retention,frames) : retention;
}

static float ABI wheel_blend(float weight, float dt) {
    const float steps=timestep_frames();
    if (!(dt>0) || !isfinite(dt) || !(steps<1) || !(weight>0 && weight<1))
        return weight;
    const float reference=weight/steps;
    if (reference>=1) return 1;
    return -expm1f(log1pf(-reference)*steps);
}

static void timestep_bridge(unsigned char *code, const void *resume,
                            const unsigned char displaced[15], unsigned reg,
                            signed char dt_offset, float (ABI *helper)(float,float)) {
    /* Preserve live native registers while correcting one scalar coefficient. */
    const unsigned char save[] = {0x9c,0x50,0x51,0x52,0x56,0x57,
        0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x48,0x81,0xec,0,1,0,0};
    unsigned char *p=code;
    memcpy(p,save,sizeof save); p+=sizeof save;
    for (unsigned i=0;i<16;++i) {
        if (i>=8) *p++=0x44;
        *p++=0x0f; *p++=0x11; *p++=(unsigned char)(0x84+8*(i&7)); *p++=0x24;
        uint32_t offset=i*16; memcpy(p,&offset,4); p+=4;
    }
    const unsigned char args[] = {0xf3,0x0f,0x10,0x84,0x24,0,0,0,0,
        0xf3,0x0f,0x10,0x4d,0xcc,0x48,0xb8};
    memcpy(p,args,sizeof args);
    p[13]=(unsigned char)dt_offset;
    uint32_t offset=reg*16; memcpy(p+5,&offset,4); p+=sizeof args;
    memcpy(p,&helper,8); p+=8;
    *p++=0xff; *p++=0xd0;
    const unsigned char result[] = {0xf3,0x0f,0x11,0x84,0x24};
    memcpy(p,result,sizeof result); p+=sizeof result;
    memcpy(p,&offset,4); p+=4;
    for (unsigned i=0;i<16;++i) {
        if (i>=8) *p++=0x44;
        *p++=0x0f; *p++=0x10; *p++=(unsigned char)(0x84+8*(i&7)); *p++=0x24;
        offset=i*16; memcpy(p,&offset,4); p+=4;
    }
    const unsigned char restore[] = {0x48,0x81,0xc4,0,1,0,0,
        0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,0x9d};
    memcpy(p,restore,sizeof restore); p+=sizeof restore;
    memcpy(p,displaced,15); p+=15;
    jump(p,resume);
}

static void ABI contact_timestep(TimestepRegisters *r) {
    const float dt=real((const unsigned char *)r->rbp-0x34,0), frame=timestep_frame_dt();
    /* shortcut: scaled-world position limits stay native until their contact response is proved. */
    if (!(timestep_frames()<1) || dt!=frame) return;
    r->ymm[0][0]/=dt;
    memset(r->ymm[0]+4,0,4*sizeof(float));
    r->resume+=8; /* Skip only the native multiply; retain its store and comparison. */
}

/* Only the multiply is displaced: the bridge resumes at its store (0xe47d24). The comparison at
 * 0xe47d29 must stay in place, since 0xe47b87 jumps straight to it (with the old 18-byte patch,
 * which covered it, that jump landed inside the patch's jump and crashed the race). */
static void contact_timestep_bridge(unsigned char *code, const void *resume) {
    const unsigned char displaced[]={0xc5,0xfa,0x59,0x05,0x0e,0,0,0}; /* vmulss xmm0,[rip+14]: the 30 below */
    const size_t length=register_timestep_bridge(code,displaced,sizeof displaced,resume,contact_timestep);
    const float thirty=30;
    memcpy(code+length,&thirty,4);
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
    /* Small loader images do not contain CTR's native race option hooks. */
    if (size < 0x2af2608) return 1;
    if (!camera_timestep_contract(image,size) ||
        !vfx_timestep_contract(image,size) || !angular_timestep_contract(image,size) ||
        !deceleration_timestep_contract(image,size) || !scaled_clock_contract(image,size) ||
        !ground_speed_project_contract(image,size)) return 0;
    /* The free-flight timestep checks code around it, ANDN included (0x81af9c, 0x81b65b): on a CPU without
     * BMI1 the ISA rewrite has replaced those, and the game stopped at start ("native race option callback
     * could not be installed", 20/11, a GTX 1050 Ti PC). Without its contract only that timestep is left as
     * the game's own; everything else installs. */
    const int freeflight = freeflight_timestep_contract(image,size);
    if (!freeflight)
        puts("Native free-flight: the game's own timestep (its code differs here, e.g. rewritten for this CPU)");
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
    const unsigned char hop_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x20};
    const unsigned char exhaust_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x49,0x89,0xff,0x31,0xdb};
    if (memcmp(image+0xe8ad70-shift,exhaust_prefix,sizeof exhaust_prefix)) return 0;
    const unsigned char keep_boost_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xff};
    if (memcmp(image+0xe2f1d0,keep_boost_prefix,sizeof keep_boost_prefix)) return 0;
    const unsigned char boost_update_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28};
    if (memcmp(image+0xe75250-shift,boost_update_prefix,sizeof boost_update_prefix)) return 0;
    if (memcmp(image+0xbb5060,"\xc5\xfa\x10\x06\xc5\xfa\x5c\x47\x60",9)) return 0;
    const unsigned char motion_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x08,0x01,0,0};
    if (memcmp(image+0xe33d80,hop_prefix,sizeof hop_prefix) || memcmp(image+0xe49d90,hop_prefix,sizeof hop_prefix) ||
        memcmp(image+0xe46250,motion_prefix,sizeof motion_prefix)) return 0;
    const unsigned char contact_step[] = {0xc5,0xfa,0x59,0x05,0x0c,0x26,0xe6,0,
        0xc5,0xfa,0x11,0x45,0x84,0xc5,0xf8,0x2e,0x45,0xb8};
    int32_t contact_displacement;
    memcpy(&contact_displacement,image+0xe47d20,4);
    const int64_t contact_constant=0xe47d24+(int64_t)contact_displacement;
    if (memcmp(image+0xe47d1c,contact_step,4) ||
        memcmp(image+0xe47d24,contact_step+8,sizeof contact_step-8) ||
        memcmp(image+0xe47d2e,"\x48\x8d\x45\x84",4) ||
        /* the two instructions before the multiply, which the patch now starts at */
        memcmp(image+0xe47d10,"\xc5\xfa\x10\x8d\x40\xff\xff\xff\xc5\xf2\x5c\xc0",12) ||
        memcmp(image+0xe46278,"\x48\x89\x95\x58\xff\xff\xff",7) ||
        memcmp(image+0xe252c9,"\x45\x8b\x6f\x18",4) ||
        contact_constant<0 || (uint64_t)contact_constant>size-4 ||
        memcmp(image+contact_constant,"\0\0\xf0\x41",4)) return 0;
    const unsigned char lateral_base[] = {0xc5,0xfa,0x10,0x7d,0xc0,
        0xc5,0xfa,0x10,0x5d,0xc4,0xc5,0xfa,0x10,0x45,0xc8};
    const unsigned char lateral_surface[] = {0xc5,0xfa,0x10,0x6d,0xc0,
        0xc5,0xfa,0x10,0x75,0xc4,0xc5,0x7a,0x10,0x4d,0xc8};
    if (memcmp(image+0xe46b2a,lateral_base,sizeof lateral_base) ||
        memcmp(image+0xe47237,lateral_surface,sizeof lateral_surface)) return 0;
    const unsigned char wheel_step[] = {0xc5,0xfa,0x59,0xc2,0xc5,0xf2,0x58,0xc0,
        0xc4,0xc1,0x7a,0x11,0x44,0x24,0x58};
    if (memcmp(image+0xe2f821,wheel_step,sizeof wheel_step) ||
        memcmp(image+0xe2f6a8,"\xc5\xfa\x11\x45\xac",5)) return 0;
    const unsigned char rotation_step[]={0xc5,0xf8,0x28,0x8d,0xf0,0xfd,0xff,0xff,
        0xc5,0xf0,0x59,0x49,0x20,0xc5,0xc8,0x59,0xc9};
    if (memcmp(image+0x8790eb,rotation_step,sizeof rotation_step)) return 0;
    enum { CAMERA_BRIDGE = 2912, CONTACT_BRIDGE = 10656, CONTACT_ENTRY = 11168, COLD_SIZE = 11232 };
    _Static_assert(CONTACT_BRIDGE >= CAMERA_BRIDGE + 2048, "contact bridge overlaps camera bridge");
    _Static_assert(CONTACT_ENTRY >= CONTACT_BRIDGE + 512, "contact bridge exceeds allocation");
    _Static_assert(COLD_SIZE >= CONTACT_ENTRY + 64, "contact entry exceeds allocation");
#ifdef _WIN32
    unsigned char *cold = VirtualAlloc(NULL, COLD_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *cold = mmap(NULL, COLD_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cold == MAP_FAILED) cold = NULL;
#endif
    if (!cold || !writable(image+0xe2f1d0,sizeof keep_boost_prefix) || !writable(image+0xe75250-shift,sizeof boost_update_prefix) || !writable(image+0xe8ad70-shift,sizeof exhaust_prefix) || !writable(image+0xe33d80,15) || !writable(image+0xe49d90,15) || !writable(image+0xe46250,20) || !writable(image+0xe452c0,17) || !writable(image+0xe4a220,17) || !writable(image + 0xdcd930, 20) || !writable(image + 0xc0a3b0, 14) || !writable(image + 0xdcdd30, 20) || !writable(image + 0xdd0210, 20) || !writable(image + 0xc59b10, 14) || !writable(image + 0x2770de0, sizeof(void *)) ||
        !writable(image + 0xf4c840 - shift, 14) || !writable(image + 0xe0f9b0, 14) ||
        !writable(image + 0xdcfc10, 14) || !writable(image + 0xdcb630, 17) || !writable(image + 0xdcfb10, 14) || !writable(image + 0xe0efa0, 14)) return 0;
    if (!writable(image+0xe47d10,20) ||
        !writable(image+0xe46b2a,15) || !writable(image+0xe47237,15) ||
        !writable(image+0xe2f821,15) || !writable(image+0x8790eb,sizeof rotation_step) ||
        !writable(image+0xe46acf,15) || !writable(image+0xe45951,15) || !writable(image+0xe46a55,18) || !writable(image+0xa31afd,15)) return 0;
    if (!writable(image+0xc89b54,15) || !writable(image+0xc89bd4,16) ||
        !writable(image+0xc89c44,17) || !writable(image+0xa997c0,14)) return 0;
    if (!writable(image+0x2b3670,sizeof vfx_worker_code) || !writable(image+0x2c4f98,sizeof vfx_drag_code)) return 0;
    for (unsigned i=0;freeflight && i<sizeof freeflight_sites/sizeof *freeflight_sites;++i)
        if (!writable(image+freeflight_sites[i].address,freeflight_sites[i].length)) return 0;
    timestep_clock=(void **)(image+0x2ab4100);
    install_vfx_timestep(image,cold+4960);
    install_angular_timestep(image,cold+5536);
    install_deceleration_timestep(image,cold+6048);
    install_scaled_clock(image,cold+6560);
    install_ground_speed_project(image,cold+7072);
    if (freeflight) install_freeflight_timestep(image,cold+7584);
    puts("Native steering: nonlinear angular acceleration follows the 30 FPS reference");
    /* Gameplay validation found visible shake and stutter; retain the native camera path. */
    if (0) install_camera_timestep(image,cold+CAMERA_BRIDGE);
    register_timestep_bridge(cold+2400,rotation_step,sizeof rotation_step,image+0x8790fc,rotation_timestep);
    jump(image+0x8790eb,cold+2400);
    memset(image+0x8790eb+14,0x90,sizeof rotation_step-14);
    puts("Native rotation integration: angular response matches 30 FPS above 30 FPS");
    timestep_bridge(cold+864,image+0xe46b39,lateral_base,11,-0x34,lateral_retention);
    timestep_bridge(cold+1376,image+0xe47246,lateral_surface,3,-0x34,lateral_retention);
    if (0) timestep_bridge(cold+1888,image+0xe2f830,wheel_step,2,-0x54,wheel_blend);
    jump(image+0xe46b2a,cold+864); image[0xe46b38]=0x90;
    jump(image+0xe47237,cold+1376); image[0xe47245]=0x90;
    /* Gameplay validation found visible animation stutter; retain the native wheel blend. */
    puts("Native kart lateral retention: base and surface damping follow simulation dt above 30 FPS");
    /* 0xe47d10..0xe47d23 (load, subtract, multiply) go to the entry, which runs the first two and
     * enters the bridge before the multiply; the store at 0xe47d24 and 0xe47d29's comparison stay. */
    contact_timestep_bridge(cold+CONTACT_BRIDGE,image+0xe47d24);
    memcpy(cold+CONTACT_ENTRY,image+0xe47d10,12);
    jump(cold+CONTACT_ENTRY+12,cold+CONTACT_BRIDGE);
    jump(image+0xe47d10,cold+CONTACT_ENTRY);
    memset(image+0xe47d10+14,0x90,20-14);
    puts("Native kart contact: position-limit correction follows simulation dt above 30 FPS");
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
    catchup_racer_ahead=(RacerAhead)(image+0xff11a0-shift);
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
    memcpy(cold+544,hop_prefix,sizeof hop_prefix);
    jump(cold+544+sizeof hop_prefix,image+0xe49d90+sizeof hop_prefix);
    native_jump_impulse=(BoostDuration)(cold+544);
    memcpy(cold+576,motion_prefix,sizeof motion_prefix);
    /* rbp-a8 is reused by native code. Keep dt in the Euler vector's unused fourth float. */
    memcpy(cold+576+sizeof motion_prefix,"\x8b\x42\x18\x89\x45\xcc",6);
    jump(cold+576+sizeof motion_prefix+6,image+0xe46250+sizeof motion_prefix);
    native_kart_motion=(KartMotion)(cold+576);
    memcpy(cold+624,hop_prefix,sizeof hop_prefix);
    jump(cold+624+sizeof hop_prefix,image+0xe33d80+sizeof hop_prefix);
    native_trick_impulse=(BoostDuration)(cold+624);
    jump(image+0xe33d80,trick_jump_impulse);
    jump(image+0xe49d90,jump_impulse);
    jump(image+0xe46250,kart_motion);
    native_set_velocity=(SetVelocity)(image+0xbb5060);
    memcpy(cold+672,exhaust_prefix,sizeof exhaust_prefix);
    jump(cold+672+sizeof exhaust_prefix,image+0xe8ad70-shift+sizeof exhaust_prefix);
    native_exhaust_state=(ExhaustState)(cold+672);
    jump(image+0xe8ad70-shift,exhaust_state);
    memcpy(cold+704,boost_update_prefix,sizeof boost_update_prefix);
    jump(cold+704+sizeof boost_update_prefix,image+0xe75250-shift+sizeof boost_update_prefix);
    native_boost_update=(BoostUpdate)(cold+704);
    jump(image+0xe75250-shift,boost_update);
    memcpy(cold+752,keep_boost_prefix,sizeof keep_boost_prefix);
    jump(cold+752+sizeof keep_boost_prefix,image+0xe2f1d0+sizeof keep_boost_prefix);
    native_keep_boost=(KeepBoost)(cold+752);
    jump(image+0xe2f1d0,keep_boost);
    puts("Native AI: Extreme/Nightmare retain fastest pad fire through braking/contact; players excluded");
    printf("Native AI: loaded %d TT track references\n",
           runtime_tt_reference_load(getenv("BB_TT_REFERENCE")));
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    puts("Native race options: Private Single No Teams / 2 / 4; Very Hard / Extreme / Nightmare AI profiles");
    return 1;
}
