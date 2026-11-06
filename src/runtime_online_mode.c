/* Native Normal / Speed / Battle selector; Speed uses the game's race event. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_name_colors.h"
#include "runtime_ai_catchup.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern void runtime_speed_mode_set(int enabled);
typedef void (ABI *EventHandler)(unsigned char *, void *, void *);
typedef void (ABI *EmitChoice)(void *, int);
typedef void (ABI *SetSeason)(void *, int);
typedef void *(ABI *GetType)(void *);
typedef void (ABI *PrivateState)(unsigned char *, unsigned char *);
static EventHandler original_event, base_event;
static EmitChoice original_choice;
static SetSeason original_season;
static PrivateState private_state, native_can_start;
typedef void (ABI *NativeNotice)(const char *, void *);
static NativeNotice native_notice;
static void **initialize_type;
static unsigned char **network;
static _Thread_local int customized_selector;
static int private_speed_selected;
static int selector_mode_index;
static int private_cup_selected;
static int remote_cup_selected = -1;
static int cup_reset_pending;
static int private_cup_race_active;
static int cup_transition_pending;
static int cup_final_results_ready;
static void **racer_manager;
static unsigned char *end_timer_hud_mode;
static unsigned char *finish_timer_counter, *human_finish_shortcut;
static unsigned char *ai_reuse_branch, *ai_asset_reuse_branch;
static const unsigned char native_ai_asset_reuse[] = {0x0f,0x85,0xf2,0x0b,0,0};
static const unsigned char native_ai_reuse[] = {0x0f,0x85,0xd6,0x06,0,0};
typedef void (ABI *ObjectPin)(void *, void *, void **);
static ObjectPin object_pin;
static SetSeason set_race_mode;
static void **race_manager;
static unsigned char **race_settings;
static int picker_previous_mode = -1;
static EventHandler original_cup_item;
static EventHandler original_level_button;
static EventHandler original_lobby_event, original_timer_event;
typedef void (ABI *BehaviorUpdate)(unsigned char *, unsigned char *, float);
static BehaviorUpdate original_lobby_update, original_selector_update, original_team_slot_update;
typedef char *(ABI *InternString)(void *, const char *);
typedef void (ABI *DropString)(void *, void *);
typedef void (ABI *HandleOperation)(void **);
static InternString intern_string;
static DropString drop_string;
static HandleOperation resolve_handle, destroy_handle;
static void **string_pool;
static GetType sprite_asset;
static void *composition_vtable;
typedef unsigned char *(ABI *FindZone)(void *, const char *);
static FindZone find_zone;
static void **zone_manager;
static void **system_data;
typedef void (ABI *NativeEvent)(unsigned char *);
static NativeEvent receive_private_settings;
static NativeEvent publish_private_track;
static NativeEvent broadcast_race_settings;
typedef void (ABI *ResizeList)(void *, int, int);
static ResizeList resize_list;
static NativeEvent clear_zone_list;
static NativeEvent refresh_selector;
static NativeEvent original_clear_participants;
static NativeEvent original_reset_racer_pool;
static NativeEvent original_spawn_cup_ai;
static NativeEvent original_place_racers;
static NativeEvent original_race_ui, original_lobby_countdown, original_lobby_start, original_countdown_init;
typedef unsigned char (ABI *NetworkQuery)(void);
static NetworkQuery in_network_game;
typedef void (ABI *RunGraphNode)(void *, void *);
static RunGraphNode run_graph_node;
static NativeEvent finish_project_context;
typedef void (ABI *ProjectEvent)(void *, void *);
typedef void (ABI *OpenProject)(void **, void *, const char *, void *, int, void *, float);
static ProjectEvent original_project_event;
static OpenProject open_project;
static NativeEvent enable_project_controls;
static NativeEvent activate_project_input;
static _Thread_local int opening_team_race_ui, opening_cup_finish_ui;
static SetSeason hide_placeable;
static void **gui_manager, **race_finished_type, **local_racers_finished_type;
static void *cup_ranking_project, *cup_finish_project;
static void *team_ranking_project;
static void *lobby_members_project;
static void *hidden_lobby_members_root;
static int lobby_members_was_hidden;
static unsigned char *team_race_ui_owner;
static int team_score_opened, team_score_finished, team_effects_played;
static NativeEvent native_win_confetti, release_result_sound;
typedef void (ABI *PlayResultSound)(void *, const char *, void **, void **);
static PlayResultSound play_result_sound;
typedef void (ABI *AnimationSelector)(void *, const char *, int);
typedef void (ABI *AnimationEvent)(void *, const char *);
static AnimationSelector set_animation_selector;
static AnimationEvent send_animation_event;
static void play_team_result_effects(void);
static unsigned team_score_trace_events;
static void open_team_score(void);
static void *team_leaders_project;
static int private_single_race_active;
static int private_team_selector_active;
static void *active_team_selector_project;
static void **team_confirm_type, **team_unconfirm_type;
static ProjectEvent original_gui_broadcast;
static _Thread_local void *ready_delivery_event;
static _Thread_local int ready_delivery_seen;
typedef void (ABI *ReadyInsert)(void *, void *, unsigned char *, unsigned);
static ReadyInsert original_ready_insert;
static int team_result_count, team_totals[4];
typedef unsigned char (ABI *RacerAhead)(unsigned char *, unsigned char *);
static RacerAhead original_racer_ahead;
static struct { int id, position; } private_grid[8];
static int private_grid_count;
static unsigned team_finished_mask;
static int team_results_pending;
static unsigned char pending_team_results[0x40];
static int team_winner = -1, team_tiebreak;
static EventHandler original_ranking_row_event, original_team_selector_event, original_team_count_event;
static void **selector_changed_type;
static void **gui_input_type, **gui_uninitialize_type;
static unsigned char **player_manager;
typedef int (ABI *ControllerPlayer)(int);
typedef unsigned char *(ABI *FindPlayer)(void *, int);
typedef unsigned char (ABI *ReadyFind)(void *, void *, unsigned, unsigned char **);
static ControllerPlayer controller_player;
static FindPlayer find_player;
static ReadyFind ready_find;
typedef void (ABI *AddLegend)(void *, int, char **, unsigned char);
static AddLegend add_legend;
static int team_legend_state = -1, team_legend_controller = 1;

static void *ranking_row_vtable;
static EventHandler original_leader_event, original_marker_event;
typedef void *(ABI *LookupHero)(void *, const char *);
static LookupHero lookup_hero;
static BehaviorUpdate original_leader_update;
static void **marker_initialize_type;
static const char *team_names[] = {"BLUE", "RED", "GREEN", "YELLOW"};
typedef int (ABI *RacerFaction)(void *);
static RacerFaction racer_faction;
static NativeEvent original_race_setup;
static int cup_panel_done;
static SetSeason queue_pool_assets, wait_pool_assets;
static unsigned char **native_racer_pool;
static char *cup_ai_assets[8][2];
static NativeEvent original_race_results;
typedef void (ABI *SortCupRacers)(void **, void **, size_t);
static SortCupRacers sort_cup_racers;
static _Thread_local void *podium_order[8];
static _Thread_local int podium_count;
typedef int (ABI *RaceFinished)(unsigned char *);
static RaceFinished original_race_finished;
static SetSeason original_finish_racer;
static int private_team_race(void);
typedef void (ABI *RecordLap)(void *, unsigned char *, int);
typedef void (ABI *CompleteRacer)(void *, unsigned char *);
static RecordLap original_record_lap;
static CompleteRacer complete_racer;
struct CupTotal { uint64_t online_id; int id, human, points, finish_position; };
static struct CupTotal finish_order[8];
static int finish_order_count;
static struct CupTotal team_results[8]; /* Indexed by validated host finish order. */
static struct CupTotal cup_totals[8];
static int cup_total_count;
static struct { struct CupTotal key; int position, award; } cup_results[8];
static int cup_result_count;
static int cup_timer_pending;
typedef void (ABI *MakeHandle)(void **, void *);
static MakeHandle make_handle;

typedef void (ABI *LoadMap)(const char *, int);
static LoadMap original_load_map;
static char pending_private_map[256];
static int pending_private_reason;
typedef void (ABI *NoArgument)(void);
static NoArgument assign_ai_teams;
static NoArgument return_to_lobby;
static NativeEvent next_cup_map, original_guest_return_timer;
typedef void (ABI *ReturnDeadline)(void);
typedef void (ABI *ScheduleReturn)(void *, ReturnDeadline, void *);
static ScheduleReturn schedule_guest_return;
typedef void *(ABI *NetworkState)(void);
typedef int (ABI *NetworkRace)(int);
static NetworkState network_state;
static NetworkRace network_race;

static int write_code(void *target, const void *data, size_t length);
static void *pointer_at(const void *object, size_t offset);
static void update_private_cup_track(void);
static void ensure_default_cup(void);
static int battle_selected(void);
static int is_private(void);
static int current_cup_index(void);
static void snapshot_cup_totals(void);
static void ABI private_race_timer_done(void);
static void snapshot_cup_ai_assets(void);
static void restore_cup_total(unsigned char *racer);
static void refresh_lobby_card(unsigned char *owner);
static void clear_cup_finish_panel(void);
static void refresh_team_results(void);
static int racer_team(unsigned char *racer);
static void team_color(unsigned char *placeable, int team);
static void clear_team_selector(void);

static int private_team_selector_allowed(void) {
    int mode = 0, teams = 1;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    return mode == 1 && (teams == 2 || teams == 4) && !runtime_public_match_active() &&
        !runtime_online_mode_private_cup() && !battle_selected() &&
        (private_team_selector_active || is_private());
}

static void reset_team_count(void) {
    const int single = 1;
    if (race_settings && *race_settings) memcpy(*race_settings + 0x28, &single, 4);
    unsigned char *settings = race_manager && *race_manager ? pointer_at(*race_manager, 0xde8) : NULL;
    if (settings) memcpy(settings + 0x28, &single, 4);
}

static void normalize_race_teams(void) {
    if (!race_settings || !*race_settings) return;
    int teams, mode = 0;
    memcpy(&teams, *race_settings + 0x28, 4);
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    if (runtime_public_match_active() || runtime_online_mode_private_cup() ||
        (mode == 1 && !is_private() && !private_team_selector_active)) teams = 1;
    else if (mode == 1 && is_private() && !battle_selected() && teams == 3) teams = 2;
    memcpy(*race_settings + 0x28, &teams, 4);
    if (teams == 1) clear_team_selector();
}

static void ABI team_count_event(unsigned char *self, void *event, void *owner) {
    const int cup = runtime_online_mode_private_cup();
    const int single = is_private() && !battle_selected() && !cup;
    normalize_race_teams();
    GetType type = (GetType)pointer_at(pointer_at(event, 0), 0);
    int selected = 0;
    if (type(event) == *selector_changed_type) memcpy(&selected, (unsigned char *)event + 0x18, 4);
    if (!(type(event) == *selector_changed_type && (cup || (single && selected == 2))))
        original_team_count_event(self, event, owner);
    normalize_race_teams();
    if (cup || single) {
        int teams = 1;
        if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
        const int index = teams - 1;
        memcpy(self + 0x190, &index, 4);
    }
    unsigned char *available = pointer_at(self, 0x1b0);
    int count = 0;
    if (available) memcpy(&count, available + 12, 4);
    unsigned char *items = available ? pointer_at(available, 0x20) : NULL;
    if (items && count > 0 && count <= 4 && (cup || single)) {
        for (int i = 0; i < count; ++i) items[i] = cup ? i == 0 : i != 2;
    }
}

static void ABI menu_project(void *context, void *pin, void **result, unsigned char *args) {
    ensure_default_cup();
    unsigned count;
    memcpy(&count, args, sizeof count);
    const int cup = runtime_online_mode_private_cup();
    if (count >= 10 && count <= 12) {
        void **pins;
        memcpy(&pins, args + 0x10, sizeof pins);
        if (cup) {
            pin = pins[8];
            if (*race_manager && count == 11) {
                memcpy(&picker_previous_mode, (unsigned char *)*race_manager + 0xd40, 4);
                set_race_mode(*race_manager, 6);
            } else if (*race_manager && picker_previous_mode >= 0) {
                set_race_mode(*race_manager, picker_previous_mode);
                picker_previous_mode = -1;
                if (count == 12) update_private_cup_track();
            }
            puts("Native private CUP: opening native menu route");
        }
    }
    object_pin(context, pin, result);
    if (cup && count >= 10 && count <= 12)
        printf("Native private CUP: menu project %s (%p)\n", *result ? "resolved" : "MISSING", *result);
}
/* Every pick in the selector (set by runtime_room_mode.c; NULL in the unit test, which links this
 * file alone): in a private match the host's choice goes to RyuNET for the players who join, and
 * a player picking a mode stops following another host's room. */
void (*runtime_online_mode_chosen)(int speed, int private_room);

/* A joining player takes the room's mode from its host (runtime_room_mode.c): Speed or not,
 * whatever this player picked in the selector. Their own choice stays for their own room. */
void runtime_online_mode_adopt(int speed) {
    runtime_speed_mode_set(speed != 0);
    printf("Native Online mode: %s (the host's private room)\n", speed ? "ITEMLESS" : "NORMAL");
}

int runtime_online_mode_private_speed(void) { return private_speed_selected; }

static void *pointer_at(const void *object, size_t offset) {
    void *pointer;
    memcpy(&pointer, (const unsigned char *)object + offset, sizeof pointer);
    return pointer;
}

static void set_string(unsigned char *field, const char *text) {
    char *old = pointer_at(field, 0);
    if (!text || (old && !strcmp(old, text))) return;
    char *value = intern_string(*string_pool, text);
    memcpy(field, &value, sizeof value);
    if (old) drop_string(*string_pool, old - 0x10);
}

/* Another player's racer (the lobby's customization message: +0x20 field count, +0x88 the
 * racer's name, +0x90 its outfit, +0xb0 the player's slot). A racer from a mod this game lacks
 * (DriverSonic) loads from an archive that is not here, and the lobby waits for it forever:
 * the countdown stops at 0 for everyone. It is shown as Crash here instead.
 * BB_KNOWN_HEROES: the game's own racers (scripts/online_mode.py known_heroes); a racer this
 * player's own mods add has its archive here and stays. */
typedef void (ABI *PeerCustomization)(void *, unsigned char *);
static PeerCustomization original_peer_customization;

static char lower_ascii(char c) { return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }

static int known_hero(const char *name) {
    const char *list = getenv("BB_KNOWN_HEROES");
    if (!name || !*name || !list || !*list) return 1;
    const size_t length = strlen(name);
    for (const char *at = list; *at;) {
        const char *end = strchr(at, ',');
        const size_t n = end ? (size_t)(end - at) : strlen(at);
        size_t same = 0;
        while (n == length && same < n && lower_ascii(at[same]) == lower_ascii(name[same])) ++same;
        if (n == length && same == n) return 1;
        if (!end) break;
        at = end + 1;
    }
    char path[128], lower[64];
    if (length >= sizeof lower) return 0;
    for (size_t i = 0; i <= length; ++i) lower[i] = lower_ascii(name[i]);
    snprintf(path, sizeof path, "/app0/archives/%s.pak", lower);
    unsigned char stat[256];
    return runtime_file_stat(path, stat) == 0;
}

static void ABI peer_customization(void *self, unsigned char *message) {
    uint32_t fields = 0;
    if (message) memcpy(&fields, message + 0x20, sizeof fields);
    const char *hero = fields >= 8 ? pointer_at(message, 0x88) : NULL;
    if (hero && !known_hero(hero)) {
        int slot;
        memcpy(&slot, message + 0xb0, sizeof slot);
        printf("Native Online: player %d's racer %s is not in this game (a mod); shown as Crash\n",
               slot, hero);
        fflush(stdout);
        set_string(message + 0x88, "DriverCrash");
        void *no_outfit = NULL; /* the mod's outfit does not exist either */
        memcpy(message + 0x90, &no_outfit, sizeof no_outfit);
    }
    original_peer_customization(self, message);
}

static void set_text(unsigned char *placeable, const char *text) {
    if (!placeable || !text) return;
    const char *old = pointer_at(placeable, 0x30);
    if (old && !strcmp(old, text)) return;
    set_string(placeable + 0x30, text);
    unsigned flags;
    memcpy(&flags, placeable + 12, 4);
    flags |= 0x1000;
    memcpy(placeable + 12, &flags, 4);
}

void runtime_online_mode_set_text(void *placeable, const char *text) {
    set_text(placeable, text);
}

/* The native handle may lazily resolve or redirect through another handle. */
static unsigned char *handle_object(void **slot) {
    for (int depth = 0; depth < 16 && *slot; ++depth) {
        unsigned flags;
        memcpy(&flags, *slot, 4);
        if (flags & 0x10000000) resolve_handle(slot);
        if (!*slot) return NULL;
        memcpy(&flags, *slot, 4);
        unsigned char *object = pointer_at(*slot, 0x28);
        if (!(flags & 0x20000000) || !object) return object;
        slot = (void **)(object + 0x10);
    }
    return NULL;
}

typedef int (ABI *KartDamage)(unsigned char *, unsigned, void **, unsigned char);
static KartDamage original_kart_damage;

static int friendly_race_entities(unsigned char *target, unsigned char *attacker) {
    int teams = 1;
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    if (!private_single_race_active || private_cup_race_active ||
        runtime_online_mode_private_cup() || (teams != 2 && teams != 4) ||
        !target || !attacker || target == attacker) return 0;
    /* Native entity factions cover humans and AI; preserve self/world hazards. */
    unsigned target_bits, attacker_bits;
    memcpy(&target_bits, target + 0x78, 4);
    memcpy(&attacker_bits, attacker + 0x78, 4);
    const unsigned mask = ((1u << teams) - 1) << 22;
    target_bits &= mask;
    attacker_bits &= mask;
    return target_bits && !(target_bits & (target_bits - 1)) && target_bits == attacker_bits;
}

static int ABI kart_damage(unsigned char *component, unsigned kind, void **source, unsigned char flags) {
    if (private_single_race_active &&
        friendly_race_entities(pointer_at(component, 0x18), source ? handle_object(source) : NULL))
        return 1; /* Native damage-rejected result, before any hit side effects. */
    return original_kart_damage(component, kind, source, flags);
}

typedef void (ABI *ClockTarget)(unsigned char *, unsigned char *, void *);
static ClockTarget original_clock_target;
static void ABI clock_target(unsigned char *component, unsigned char *target, void *effect) {
    if (!private_single_race_active ||
        !friendly_race_entities(target, handle_object((void **)(component + 0x28))))
        original_clock_target(component, target, effect);
}

static unsigned char *current_cup(void) {
    return race_manager && *race_manager ?
        handle_object((void **)((unsigned char *)*race_manager + 0xd88)) : NULL;
}

static void assign_handle(unsigned char *field, void *value) {
    void *old = pointer_at(field, 0);
    if (old == value) return;
    if (value) __atomic_fetch_add((unsigned *)value, 1, __ATOMIC_SEQ_CST);
    if (old) {
        unsigned before = __atomic_fetch_sub((unsigned *)old, 1, __ATOMIC_SEQ_CST);
        if ((before & 0xffff) == ((before & 0x90000000) ? 3u : 2u))
            destroy_handle(&old);
    }
    memcpy(field, &value, sizeof value);
}

static void refresh_lobby_members_visibility(void) {
    unsigned char *project = handle_object(&lobby_members_project);
    unsigned char *root = project ? pointer_at(project, 0x10) : NULL;
    if (!root) { hidden_lobby_members_root = NULL; return; }
    if (private_team_selector_active) {
        if (hidden_lobby_members_root != root) {
            unsigned flags;
            memcpy(&flags, root + 12, 4);
            lobby_members_was_hidden = !!(flags & 0x400);
            hidden_lobby_members_root = root;
        }
        hide_placeable(root, 1);
    } else if (hidden_lobby_members_root == root) {
        hide_placeable(root, lobby_members_was_hidden);
        hidden_lobby_members_root = NULL;
    }
}

static void clear_team_selector(void) {
    unsigned char *project = handle_object(&active_team_selector_project);
    if (project) {
        unsigned char *root = pointer_at(project, 0x10);
        if (root) hide_placeable(root, 1);
        project[0x34] &= ~0x20; /* A retired selector must not consume the next screen's input. */
    }
    private_team_selector_active = 0;
    team_legend_state = -1; team_legend_controller = 1;
    assign_handle((unsigned char *)&active_team_selector_project, NULL);
    refresh_lobby_members_visibility();
}

/* The native private-settings event carries mode 6 for CUP. Its map is the
 * selected first track, unique among Arcade cups; no invented playlist is sent. */
static unsigned char *cup_for_first_track(const char *map) {
    unsigned char *system = system_data ? *system_data : NULL;
    unsigned char *cups = system ? pointer_at(system, 0x188) : NULL;
    if (!cups) return NULL;
    int count;
    memcpy(&count, cups + 12, 4);
    void **items = pointer_at(cups, 0x20);
    for (int i = 0; items && i < count; ++i) {
        unsigned char *cup = items[i];
        int tracks;
        memcpy(&tracks, cup + 12, 4);
        if (tracks != 4 || cup[0x38]) continue; /* exclude Gem cups */
        void **levels = pointer_at(cup, 0x20);
        const char *first = levels && levels[0] ? pointer_at(levels[0], 0x10) : NULL;
        if (first && (!map || !strcasecmp(first, map))) return cup;
    }
    return NULL;
}

static int adopt_native_cup(unsigned char *cup) {
    unsigned char *manager = race_manager ? *race_manager : NULL;
    unsigned char *selected = manager ? pointer_at(manager, 0xd80) : NULL;
    void **tracks = cup ? pointer_at(cup, 0x20) : NULL;
    if (!selected || !tracks) return 0;
    void *zones[4];
    for (int i = 0; i < 4; ++i) {
        zones[i] = find_zone(*zone_manager, pointer_at(tracks[i], 0x10));
        if (!zones[i]) return 0;
    }
    if (current_cup() == cup) return 1;
    clear_zone_list(selected);
    resize_list(selected, 4, 8);
    void **handles = pointer_at(selected, 0x20);
    for (int i = 0; i < 4; ++i) {
        handles[i] = NULL;
        make_handle(handles + i, zones[i]);
    }
    /* Cup + 0x78 is a string, not an igHandle. Register the actual object. */
    make_handle((void **)(manager + 0xd88), cup);
    const int first = 0;
    memcpy(manager + 0xd68, &first, 4);
    return current_cup() == cup;
}

void runtime_online_mode_leave_cup(void) {
    clear_team_selector();
    cup_timer_pending = 0;
    reset_team_count();
    private_single_race_active = 0;
    /* Host loss can leave the native standings open. Keep their CUP mode until
     * the level actually changes; otherwise native total-point queries return 0. */
    if (race_manager && *race_manager) {
        int mode;
        memcpy(&mode, (unsigned char *)*race_manager + 0xd4c, 4);
        cup_reset_pending = mode == 6;
    }
    remote_cup_selected = -1;
}

static void ensure_default_cup(void) {
    if (remote_cup_selected >= 0 || !runtime_online_mode_private_cup() || current_cup()) return;
    unsigned char *cup = cup_for_first_track(NULL);
    if (cup && adopt_native_cup(cup)) update_private_cup_track();
}

static void ABI private_settings_received(unsigned char *event) {
    int mode, type;
    memcpy(&type, event + 0x20, 4);
    memcpy(&mode, event + 0x3c, 4);
    unsigned char *cup = type == 1 && mode == 6 && pointer_at(event, 0x28) ?
        cup_for_first_track(pointer_at(event, 0x28)) : NULL;
    /* Private settings use the racing UI mode; native CUP mode is for the race. */
    if (type == 1 && mode == 6) {
        const int single = 1;
        memcpy(event + 0x3c, &single, 4);
    }
    remote_cup_selected = cup && adopt_native_cup(cup);
    receive_private_settings(event);
    if (type == 1 && (mode == 1 || mode == 6)) normalize_race_teams();
    memcpy(event + 0x3c, &mode, 4);
    printf("Native private CUP: received %s\n", remote_cup_selected ?
           (const char *)pointer_at(cup, 0x28) : "SINGLE");
}

static void ABI private_settings_sending(unsigned char *event) {
    int mode = 1;
    if (race_manager && *race_manager)
        memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    if (runtime_online_mode_private_cup() && current_cup()) mode = 6;
    normalize_race_teams();
    memcpy(event + 0x3c, &mode, 4);
    int type;
    memcpy(&type, event + 0x20, sizeof type);
    /* Native lobby changes broadcast this payload only for Battle. Racing
     * and CUP need the same native event to carry settings to existing/joining peers. */
    if (type == 1 && (mode == 1 || mode == 6) && is_private() && race_settings && *race_settings)
        broadcast_race_settings(*race_settings);
}

/* The map loader runs after settings synchronization, on host and peers. Keep
 * the lobby's racing UI mode until this boundary, then use native CUP scoring. */
static void ABI load_private_map(const char *map, int reason) {
    runtime_private_party_cancel();
    private_grid_count = 0;
    const int lobby = map && (!strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby") ||
                             !strcasecmp(map, "octane/octane_startscreen/octane_startscreen"));
    if (lobby) { pending_private_map[0]=0; runtime_room_mode_end_race(); }
    else if (map && !runtime_public_match_active() && (runtime_online_mode_in_private() || runtime_private_match_active()) && !battle_selected()) {
        int verified=runtime_room_mode_begin_race();
        if (verified<0) {
            if (strlen(map)>=sizeof pending_private_map) return;
            if (!pending_private_map[0])
                printf("Native private race: awaiting asynchronous mode verification map=%s reason=%d\n",map,reason);
            snprintf(pending_private_map,sizeof pending_private_map,"%s",map);
            pending_private_reason=reason;
            return;
        }
        pending_private_map[0]=0;
        if (!verified) {
        puts("Native private race: mode verification failed; cancelling before track loading");
        return_to_lobby();
        native_notice("Cannot synchronize the host's race mode. Return to the lobby and retry.", NULL);
        return;
    }
    }
    finish_order_count = 0;
    cup_timer_pending = 0;
    runtime_race_options_map_loading(map);
    clear_team_selector();
    normalize_race_teams();
    const int team_return = team_ranking_project && map &&
        !strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby");
    int next_mode = 0;
    if (race_manager && *race_manager) memcpy(&next_mode, (unsigned char *)*race_manager + 0xd40, 4);
    private_single_race_active = map && !runtime_public_match_active() && is_private() && next_mode == 1 &&
        strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby") &&
        strcasecmp(map, "octane/octane_startscreen/octane_startscreen");
    if (private_single_race_active)
        printf("Native private race: loading %s mode=%s teams=%d\n", map,
               runtime_speed_race_active() ? "ITEMLESS" : "NORMAL",
               race_settings && *race_settings ? *(int *)(*race_settings + 0x28) : 1);
    const int incomplete_cup_return = private_cup_race_active && !cup_final_results_ready &&
        map && !strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby");
    clear_cup_finish_panel();
    team_race_ui_owner = NULL;
    team_score_opened = team_score_finished = team_effects_played = 0;
    team_score_trace_events = 0;
    assign_handle((unsigned char *)&team_ranking_project, NULL);
    assign_handle((unsigned char *)&team_leaders_project, NULL);
    team_result_count = team_results_pending = 0;
    if (cup_reset_pending && map &&
        (!strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby") ||
         !strcasecmp(map, "octane/octane_startscreen/octane_startscreen"))) {
        const int single = 1;
        set_race_mode(*race_manager, single);
        memcpy((unsigned char *)*race_manager + 0xd4c, &single, 4);
        unsigned char *settings = pointer_at(*race_manager, 0xde8);
        if (settings) memcpy(settings + 0x20, &single, 4);
        picker_previous_mode = -1;
        cup_reset_pending = 0;
    }
    unsigned char *cup = runtime_online_mode_private_cup() ? current_cup() : NULL;
    private_cup_race_active = 0;
    void **tracks = cup ? pointer_at(cup, 0x20) : NULL;
    for (int i = 0; tracks && map && i < 4; ++i) {
        const char *path = pointer_at(tracks[i], 0x10);
        if (path && !strcasecmp(path, map)) {
            private_cup_race_active = 1;
            cup_final_results_ready = 0;
            set_race_mode(*race_manager, 6);
            memcpy((unsigned char *)*race_manager + 0xd68, &i, 4);
            printf("Native private CUP: loading race %d/4 (%s)\n", i + 1, map);
            break;
        }
    }
    cup_transition_pending = private_cup_race_active && current_cup_index() > 0;
    if (cup_transition_pending) snapshot_cup_totals();
    else cup_total_count = 0;
    cup_result_count = 0;
    snapshot_cup_ai_assets();
    /* Arcade CUP omits the online timer HUD; its behavior also expires the
     * race timeout. Select that same native HUD for private CUP loads only. */
    const unsigned char hud_mode = private_cup_race_active ? 6 : 1;
    if (end_timer_hud_mode && *end_timer_hud_mode != hud_mode &&
        !write_code(end_timer_hud_mode, &hud_mode, 1))
        fputs("Native private CUP: could not enable race timer HUD\n", stderr);
    /* Include AI in Private Single (solo/teams, Normal/Itemless) and CUP.
     * Only the timer trigger changes; native completion/replication stays intact. */
    const unsigned char counter = private_cup_race_active || private_single_race_active ? 0xff : 0xed;
    if (finish_timer_counter && *finish_timer_counter != counter &&
        !write_code(finish_timer_counter, &counter, 1))
        fputs("Native private race: could not enable AI finish countdown\n", stderr);
    /* All humans finishing must not cut off nearby AI before the timeout. */
    const unsigned char finish_gate[2] = {counter == 0xff ? 0x90 : 0x74,
                                          counter == 0xff ? 0x90 : 0x54};
    if (human_finish_shortcut && memcmp(human_finish_shortcut, finish_gate, 2) &&
        !write_code(human_finish_shortcut, finish_gate, 2))
        fputs("Native private race: could not disable human-only finish shortcut\n", stderr);
    /* Online teardown removes racer objects. Arcade's subsequent-track lookup
     * assumes they survive; use native lookup-or-create for private CUP instead. */
    const unsigned char new_racer[] = {0x90,0x90,0x90,0x90,0x90,0x90};
    const unsigned char *reuse = private_cup_race_active ? new_racer : native_ai_reuse;
    if (ai_reuse_branch && memcmp(ai_reuse_branch, reuse, sizeof native_ai_reuse) &&
        !write_code(ai_reuse_branch, reuse, sizeof native_ai_reuse))
        fputs("Native private CUP: could not enable native AI racer creation\n", stderr);
    reuse = private_cup_race_active ? new_racer : native_ai_asset_reuse;
    if (ai_asset_reuse_branch && memcmp(ai_asset_reuse_branch, reuse, sizeof native_ai_asset_reuse) &&
        !write_code(ai_asset_reuse_branch, reuse, sizeof native_ai_asset_reuse))
        fputs("Native private CUP: could not enable fresh AI driver loading\n", stderr);
    original_load_map(map, reason);
    if (incomplete_cup_return || team_return) {
        unsigned char *zone = find_zone(*zone_manager, map);
        unsigned char *data = zone ? pointer_at(zone, 0x1b0) : NULL;
        /* Online podium data can still contain track 1/2/3 after host loss. */
        if (data) {
            data[0x71] = 0;
            puts(team_return ? "Native private TEAM: no custom podium; returning to lobby" :
                 "Native private CUP: skipped stale podium (no final host results)");
        }
    }
}

static void hide_cup_finish_panel(void) {
    unsigned char *project = handle_object(&cup_finish_project);
    unsigned char *root = project ? pointer_at(project, 0x10) : NULL;
    if (root) hide_placeable(root, 1);
}

static void clear_cup_finish_panel(void) {
    hide_cup_finish_panel();
    assign_handle((unsigned char *)&cup_finish_project, NULL);
    assign_handle((unsigned char *)&cup_ranking_project, NULL);
    cup_panel_done = 0;
}

/* Trace loading separately from graph delivery/visibility; handles alone do
 * not prove that a streaming project has resolved. */
static void trace_team_score(const char *stage, void *receiver, void *kind) {
    unsigned char *score = handle_object(&team_ranking_project);
    unsigned char *root = score ? pointer_at(score, 0x10) : NULL;
    unsigned flags = 0;
    if (root) memcpy(&flags, root + 12, sizeof flags);
    printf("Native private TEAM: scoreboard trace stage=%s handle=%p project=%p root=%p "
           "root_flags=%x finish=%p battle_slot=%p opened=%d finished=%d rows=%d receiver=%p event_type=%p\n",
           stage, team_ranking_project, score, root, flags, handle_object(&cup_finish_project),
           team_race_ui_owner ? handle_object((void **)(team_race_ui_owner + 0x68)) : NULL,
           team_score_opened, team_score_finished, team_result_count, receiver, kind);
}

static void ABI open_race_project(void **result, void *manager, const char *name,
                                 void *options, int player, void *context, float delay) {
    if (name && opening_cup_finish_ui && !strcmp(name, "Octane_CupRace_Ranking")) {
        /* Open the interim panel at the native ranking point, BEFORE the intro
         * HUD. Opening it after race_ui steals the intro's input-stack order. */
        unsigned char finish_context[0x10] = {0};
        open_project(&cup_finish_project, manager, "Octane_Race_Ranking",
                     options, player, finish_context, delay);
        finish_project_context(finish_context);
        assign_handle(finish_context + 8, NULL);
        assign_handle(finish_context, NULL);

    }
    int mode = 0;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    if (name && !strcasecmp(name, "Octane_TeamSelect")) {
        normalize_race_teams();
        const int allowed = private_team_selector_allowed();
        int teams = 1;
        if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
        if (!allowed && !(!runtime_public_match_active() && mode >= 9 && mode <= 13 && teams >= 2 && teams <= 4)) {
            clear_team_selector();
            if (result) assign_handle((unsigned char *)result, NULL);
            return;
        }
        if (!allowed) clear_team_selector();
        private_team_selector_active = allowed;
        printf("Native private TEAM: opening selector mode=%d private=%d\n", mode, private_team_selector_active);
    }
    open_project(result, manager, name, options, player, context, delay);
    if (name && !strcasecmp(name, "Octane_LobbyMembers")) {
        assign_handle((unsigned char *)&lobby_members_project, result ? *result : NULL);
    }
    refresh_lobby_members_visibility();
    if (opening_team_race_ui && name && !strcmp(name, "Octane_Race_Ranking")) {
        /* Use the native ranking creation point, before the intro/input layer.
         * A resolved project opened inside RaceFinished is not proof its VSC
         * initialization and animation bindings have completed. */
        unsigned char score_context[16] = {0};
        open_project(&team_ranking_project, manager, "Octane_BattleTeam_Ranking",
                     options, player, score_context, delay);
        trace_team_score("open-return", NULL, NULL);
        finish_project_context(score_context);
        assign_handle(score_context + 8, NULL);
        assign_handle(score_context, NULL);
    }
    /* open_project may finish before initialization/streaming. Enable input
     * in the selector's initialize handler, after native initialization. */
}

static void ABI race_ui(unsigned char *self) {
    clear_cup_finish_panel();
    team_race_ui_owner = NULL;
    team_score_opened = team_score_finished = team_effects_played = 0;
    team_score_trace_events = 0;
    assign_handle((unsigned char *)&team_ranking_project, NULL);
    assign_handle((unsigned char *)&team_leaders_project, NULL);
    team_result_count = team_results_pending = 0;
    int mode = 0, teams = 0;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd4c, 4);
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    opening_team_race_ui = mode == 1 && !runtime_online_mode_private_cup() && teams >= 2 && teams <= 4 && runtime_online_mode_in_private();
    opening_cup_finish_ui = private_cup_race_active;
    original_race_ui(self);
    if (opening_team_race_ui) {
        team_race_ui_owner = self;
        /* ea4540 owns Battle results population and dispatch through +68. */
        assign_handle(self + 0x68, team_ranking_project);
        assign_handle((unsigned char *)&cup_finish_project, pointer_at(self, 0x60));
        assign_handle((unsigned char *)&team_leaders_project, pointer_at(self, 0x58));
        trace_team_score("ui-owned", NULL, NULL);
        printf("Native private TEAM: race UI teams=%d ranking=%s leaders=%s\n", teams,
               team_ranking_project ? "requested" : "missing", team_leaders_project ? "requested" : "missing");
    }
    opening_team_race_ui = opening_cup_finish_ui = 0;
    if (private_cup_race_active || team_ranking_project) {
        assign_handle((unsigned char *)&cup_ranking_project, pointer_at(self, 0x60));
        puts(cup_finish_project ? "Native private race: prepared local finish time panel before intro" :
             "Native private race: local finish time panel unavailable");
    }
}

/* The online guest may not receive the Arcade-only cup-title event. */
static int refresh_cup_title(unsigned char *row, const char *title, int depth) {
    if (!row || depth>16) return 0;
    const char *name=pointer_at(row,0x28);
    if (name && !strcmp(name,"[CupName]")) { set_text(row,title); return 1; }
    unsigned char *composition=handle_object((void **)(row+0x38));
    if (composition && pointer_at(composition,0)==composition_vtable &&
        refresh_cup_title(pointer_at(composition,0x20),title,depth+1)) return 1;
    unsigned char *list=pointer_at(row,0x48);
    int count=0;
    if (list) memcpy(&count,list+12,4);
    void **items=list ? pointer_at(list,0x20) : NULL;
    if (items && count>0 && count<=512) for (int i=0;i<count;++i)
        if (refresh_cup_title(items[i],title,depth+1)) return 1;
    return 0;
}

static void synchronize_cup_finish_time(unsigned char *racer);
static void name_color_project(unsigned char *project);
static void ABI cup_project_event(void *self, void *event) {
    if (private_cup_race_active &&
        (self==handle_object(&cup_finish_project) || self==handle_object(&cup_ranking_project))) {
        unsigned char *root=racer_manager ? *racer_manager : NULL;
        unsigned char *list=root ? pointer_at(root,0x10) : NULL;
        int count=0;
        if (list) memcpy(&count,list+12,4);
        void **items=list ? pointer_at(list,0x20) : NULL;
        for (int i=0;items && count<=8 && i<count;++i)
            if (items[i]) synchronize_cup_finish_time(items[i]);
    }
    GetType event_type = (GetType)pointer_at(pointer_at(event, 0), 0);
    const int team_finish = team_race_ui_owner && event_type(event) == *race_finished_type;
    const int trace_score_event = team_race_ui_owner &&
        (self == handle_object(&team_ranking_project) || team_finish) && team_score_trace_events++ < 32;
    if (trace_score_event) trace_team_score("event-before", self, event_type(event));
    if (team_race_ui_owner && self == handle_object(&team_ranking_project)) refresh_team_results();
    /* The interim project's RaceFinished graph starts its own end-screen flow.
     * Transfer ownership BEFORE dispatch, otherwise it can advance to Wumpa
     * while the team scoreboard is still entering. */
    if (team_finish && self == handle_object(&cup_finish_project)) {
        open_team_score();
        void *score = handle_object(&team_ranking_project);
        if (team_score_opened && score && score != self) {
            if (!team_score_finished) {
                team_score_finished = 1;
                original_project_event(score, event);
                trace_team_score("finish-forwarded", score, event_type(event));
                puts("Native private TEAM: RaceFinished owned by team scoreboard; interim flow suppressed");
            }
            refresh_team_results();
            return;
        }
        /* If loading failed, retain the native finish flow as a fallback. */
    }
    if (team_finish && self == handle_object(&team_ranking_project)) {
        open_team_score();
        if (team_score_finished) return;
        team_score_finished = 1;
    }
    if ((private_cup_race_active || team_ranking_project) && cup_ranking_project &&
        self == handle_object(&cup_ranking_project)) {
        GetType type = (GetType)pointer_at(pointer_at(event, 0), 0);
        void *kind = type(event);
        if (kind == *race_finished_type) {
            /* The native CUP project owns the full standings and next-race flow. */
            cup_panel_done = 1;
            hide_cup_finish_panel();
        } else if (!cup_panel_done && kind == *local_racers_finished_type) {
            /* Only the native finish notification crosses projects. Forwarding
             * init/input/animation events duplicates or resets their UI state. */
            unsigned char *panel = handle_object(&cup_finish_project);
            if (panel && panel != self) original_project_event(panel, event);
        }
    }
    original_project_event(self, event);
    if (private_cup_race_active && self==handle_object(&cup_ranking_project)) {
        unsigned char *cup=current_cup();
        const char *title=cup ? pointer_at(cup,0x30) : NULL;
        if (title && *title) refresh_cup_title(pointer_at(self,0x10),title,0);
    }
    name_color_project(self);
    refresh_lobby_members_visibility();
    if (trace_score_event) trace_team_score("event-after", self, event_type(event));
    if (team_ranking_project && self == handle_object(&team_ranking_project)) refresh_team_results();
}

static int current_cup_index(void) {
    int index = -1;
    if (race_manager && *race_manager)
        memcpy(&index, (unsigned char *)*race_manager + 0xd68, sizeof index);
    return index;
}

/* Retain character/kart selections, not spawned drivers or their map resources.
 * Native loading compares active and selected counts; retaining both skips reload. */
static void ABI clear_participants(unsigned char *manager) {
    unsigned char *selected = pointer_at(manager, 0x30);
    int count = 0;
    const int keep = cup_transition_pending && !cup_reset_pending && selected;
    if (keep) {
        memcpy(&count, selected + 12, 4);
        memset(selected + 12, 0, 4); /* Native cleanup must not release selections. */
    }
    original_clear_participants(manager);
    if (keep) {
        memcpy(selected + 12, &count, 4);
        printf("Native private CUP: released old AI drivers; retained %d selections\n", count);
    }
}

static void snapshot_cup_ai_assets(void) {
    for (int slot = 0; slot < 8; ++slot)
        for (int j = 0; j < 2; ++j) {
            free(cup_ai_assets[slot][j]);
            cup_ai_assets[slot][j] = NULL;
        }
    if (!cup_transition_pending || cup_reset_pending) return;
    unsigned char *pool = native_racer_pool ? *native_racer_pool : NULL;
    unsigned char *manager = racer_manager ? *racer_manager : NULL;
    unsigned char *list = manager ? pointer_at(manager, 0x38) : NULL;
    int count = 0, saved = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    if (!pool || !items || count < 0 || count > 8) return;
    for (int i = 0; i < count; ++i) {
        if (!items[i]) continue;
        int slot;
        memcpy(&slot, (unsigned char *)items[i] + 0x40, 4);
        if (slot < 0 || slot >= 8 || cup_ai_assets[slot][0]) continue;
        unsigned char *record = pointer_at(pool, 0x10 + slot * 8);
        if (!record) continue;
        const char *asset = pointer_at(record, 0x38);
        /* AI can be loaded directly, with no pending streaming request. */
        if (!asset || !*asset) asset = pointer_at(record, 0x28);
        if (!asset || !*asset) asset = pointer_at(record, 0x30);
        if (!asset || !*asset) continue;
        cup_ai_assets[slot][0] = strdup(asset);
        const char *override = pointer_at(record, 0x48);
        if (override && *override) cup_ai_assets[slot][1] = strdup(override);
        if (cup_ai_assets[slot][0]) ++saved;
    }
    printf("Native private CUP: saved %d AI asset requests before map teardown\n", saved);
}

static void restore_cup_ai_assets(unsigned char *pool) {
    if (!cup_transition_pending || cup_reset_pending) return;
    int restored = 0;
    /* Reload through native streaming; never retain old entities or resources. */
    for (int slot = 0; slot < 8; ++slot) {
        unsigned char *record = pointer_at(pool, 0x10 + slot * 8);
        if (!record || !cup_ai_assets[slot][0]) continue;
        set_string(record + 0x38, cup_ai_assets[slot][0]);
        if (cup_ai_assets[slot][1]) set_string(record + 0x48, cup_ai_assets[slot][1]);
        ++restored;
    }
    printf("Native private CUP: restored %d AI asset requests\n", restored);
}

static void ABI reset_racer_pool(unsigned char *pool) {
    original_reset_racer_pool(pool);
    restore_cup_ai_assets(pool);
}

static void ABI spawn_cup_ai(unsigned char *manager) {
    unsigned char *pool = native_racer_pool ? *native_racer_pool : NULL;
    if (cup_transition_pending && !cup_reset_pending && pool) {
        restore_cup_ai_assets(pool);
        queue_pool_assets(pool, 1);
        wait_pool_assets(pool, 1);
        int requested = 0, loaded = 0;
        for (int slot = 0; slot < 8; ++slot) {
            if (!cup_ai_assets[slot][0]) continue;
            ++requested;
            unsigned char *record = pointer_at(pool, 0x10 + slot * 8);
            if (record && pointer_at(record, 0x20)) ++loaded;
        }
        printf("Native private CUP: AI resources before spawn %d/%d\n", loaded, requested);
    }
    original_spawn_cup_ai(manager);
}

static struct CupTotal racer_identity(unsigned char *racer) {
    struct CupTotal result = {0};
    unsigned char *player = handle_object((void **)(racer + 0x10));
    result.human = player != NULL;
    if (player) {
        memcpy(&result.online_id, player + 0xa8, 8);
        memcpy(&result.id, player + 0xc8, 4); /* native network index, not grid position */
    } else {
        memcpy(&result.id, racer + 0x164, 4);
    }
    memcpy(&result.points, racer + 0xb8, 4);
    memcpy(&result.finish_position, racer + 0x60, 4);
    return result;
}

/* Native a299c1 maps an AI spawn slot through the network roster before
 * comparing result IDs. Spawn slots and network IDs need not be identical. */
static int result_racer_id(unsigned char *racer) {
    const struct CupTotal key = racer_identity(racer);
    if (key.human) return key.id;
    unsigned char *list = racer_manager && *racer_manager ? pointer_at(*racer_manager, 0x30) : NULL;
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    if (!items || count < 0 || count > 8) return -1;
    for (int i = 0; i < count; ++i) {
        if (!items[i]) continue;
        int slot, id;
        memcpy(&slot, (unsigned char *)items[i] + 0x5c, 4);
        memcpy(&id, (unsigned char *)items[i] + 0x58, 4);
        if (slot == key.id) return id;
    }
    return -1;
}

static unsigned char ABI racer_ahead(unsigned char *a, unsigned char *b) {
    const unsigned char ahead = original_racer_ahead(a, b);
    if (ahead || a == b || !private_grid_count ||
        (!private_single_race_active && !private_cup_race_active) || original_racer_ahead(b, a)) return ahead;
    /* Equal native lap/checkpoint/distance must not reshuffle the starting grid. */
    const int aid = result_racer_id(a), bid = result_racer_id(b);
    int ap = 0, bp = 0;
    for (int i = 0; i < private_grid_count; ++i) {
        if (private_grid[i].id == aid) ap = private_grid[i].position;
        if (private_grid[i].id == bid) bp = private_grid[i].position;
    }
    return ap && bp ? ap < bp : ahead;
}

static void snapshot_private_grid(void) {
    private_grid_count = 0;
    if (!private_single_race_active && !private_cup_race_active) return;
    unsigned char *list = racer_manager && *racer_manager ? pointer_at(*racer_manager, 0x10) : NULL;
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    if (!items || count < 1 || count > 8) return;
    unsigned occupied = 0;
    for (int i = 0; i < count; ++i) {
        if (!items[i]) return;
        const int id = result_racer_id(items[i]);
        int position;
        memcpy(&position, (unsigned char *)items[i] + 0x58, 4);
        if (id < 0 || position < 1 || position > count || (occupied & (1u << position))) return;
        for (int j = 0; j < i; ++j) if (private_grid[j].id == id) return;
        private_grid[i].id = id;
        private_grid[i].position = position;
        occupied |= 1u << position;
    }
    private_grid_count = count;
}

/* Native finish-time rows require all configured laps and a valid final time.
 * Race teardown sets the finished flag even for racers that timed out. */
static int team_finish_time(unsigned char *racer) {
    unsigned char *laps = pointer_at(racer, 0x90);
    int required = 0, completed = 0, time = -1;
    if (race_settings && *race_settings) memcpy(&required, *race_settings + 12, 4);
    if (laps) memcpy(&completed, laps + 12, 4);
    const int *times = laps ? pointer_at(laps, 0x20) : NULL;
    if (!racer[0x8c] || required <= 0 || completed < required || !times) return 0;
    memcpy(&time, times + required - 1, sizeof time);
    return time > 0 ? time : 0;
}

static int team_racer_finished(unsigned char *racer) {
    return team_finish_time(racer) > 0;
}

static int corrected_race_order(void) {
    return private_single_race_active || private_cup_race_active ||
        runtime_public_match_active() || runtime_speed_race_active();
}

static int ABI team_race_order(unsigned char *a, unsigned char *b) {
    if (finish_order_count) {
        const struct CupTotal keys[] = {racer_identity(a), racer_identity(b)};
        int positions[] = {-1, -1};
        for (int i = 0; i < finish_order_count; ++i)
            for (int j = 0; j < 2; ++j)
                if (keys[j].human == finish_order[i].human && keys[j].id == finish_order[i].id &&
                    keys[j].online_id == finish_order[i].online_id) positions[j] = i;
        if (positions[0] >= 0 && positions[1] >= 0) return positions[0] < positions[1];
    }
    if (corrected_race_order()) {
        const int a_time = team_finish_time(a), b_time = team_finish_time(b);
        /* Native sorting trusts stale positions once both finished flags are set. */
        if (a_time != b_time) return a_time && (!b_time || a_time < b_time);
    }
    return racer_ahead(a, b);
}

/* Freeze the validated host permutation before native callbacks can re-sort local state. */
static int capture_finish_order(unsigned char *event) {
    if (!corrected_race_order()) return 1;
    unsigned char *list = racer_manager && *racer_manager ? pointer_at(*racer_manager, 0x10) : NULL;
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    if (!items || count < 1 || count > 8) return 0;
    struct CupTotal order[8] = {0};
    unsigned occupied = 0;
    for (int i = 0; i < count; ++i) {
        if (!items[i]) return 0;
        const struct CupTotal key = racer_identity(items[i]);
        const int network_id = result_racer_id(items[i]);
        if (network_id < 0) return 0;
        int position = -1;
        for (int rank = 0; rank < count; ++rank) {
            int id;
            memcpy(&id, event + 0x20 + rank * 4, 4);
            if (id != network_id) continue;
            if (position >= 0 || (occupied & (1u << rank))) return 0;
            position = rank;
        }
        if (position < 0) return 0;
        occupied |= 1u << position;
        order[position] = key;
    }
    memcpy(finish_order, order, sizeof order);
    finish_order_count = count;
    return 1;
}

/* Freeze the final snapshot once, after native result handling. Late lap or
 * finish callbacks cannot turn a timed-out racer into a scoreboard finisher. */
static void apply_team_results(unsigned char *event) {
    if (!team_ranking_project || team_result_count) return;
    unsigned char *list = racer_manager && *racer_manager ? pointer_at(*racer_manager, 0x10) : NULL;
    int count = 0, teams = 0;
    if (list) memcpy(&count, list + 12, 4);
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    if (!items || count < 1 || count > 8 || teams < 2 || teams > 4) return;
    const int guest=runtime_room_mode_following();
    const int authoritative=guest ? runtime_ai_catchup_results() : -1;
    if (guest && authoritative<0) {
        if (event!=pending_team_results) memcpy(pending_team_results,event,sizeof pending_team_results);
        team_results_pending=1;
        return;
    }
    const int awards[8] = {10, 8, 6, 5, 4, 3, 2, 1};
    int totals[4] = {0};
    struct CupTotal results[8] = {0};
    int rank_team[8] = {0};
    unsigned occupied = 0, finished_mask = 0;
    for (int i = 0; i < count; ++i) {
        unsigned char *racer = items[i];
        if (!racer) return;
        struct CupTotal key = racer_identity(racer);
        unsigned char *player = handle_object((void **)(racer + 0x10));
        int team = -1;
        if (player) memcpy(&team, player + 0xe4, 4);
        else team = racer_faction(racer) - 0x16;
        if (team < 0 || team >= teams) return;
        const int network_id = result_racer_id(racer);
        int position = -1;
        for (int rank = 0; rank < count; ++rank) {
            int id;
            memcpy(&id, event + 0x20 + rank * 4, 4);
            if (id < 0 || id != network_id) continue;
            if (position >= 0 || (occupied & (1u << rank))) return;
            position = rank;
        }
        if (position < 0) return;
        occupied |= 1u << position;
        rank_team[position] = team;
        results[position] = key;
        results[position].finish_position = position + 1;
        const int finished=guest ? !!(authoritative & (1u << position)) : team_racer_finished(racer);
        if (finished) finished_mask |= 1u << position;
        results[position].points = runtime_speed_mode_enabled() && !finished ? 0 : awards[position];
        totals[team] += results[position].points;
    }
    int maximum = 0, leaders = 0;
    for (int team = 0; team < teams; ++team)
        if (totals[team] > maximum) maximum = totals[team];
    for (int team = 0; team < teams; ++team)
        if (totals[team] == maximum) ++leaders;
    /* Host finish order is a validated permutation: the first tied team's
     * finisher decides head-to-head, independently of team ID or GUI order. */
    team_winner = -1;
    for (int position = 0; maximum > 0 && position < count; ++position)
        if (results[position].points > 0 && totals[rank_team[position]] == maximum) {
            team_winner = rank_team[position];
            break;
        }
    team_tiebreak = maximum > 0 && leaders > 1;
    printf("Native private TEAM: winner=%s points=%d head_to_head=%d\n",
           team_winner >= 0 ? team_names[team_winner] : "NONE", maximum, team_tiebreak);
    memcpy(team_totals, totals, sizeof totals);
    memcpy(team_results, results, sizeof results);
    team_result_count = count;
    team_finished_mask = finished_mask;
    team_results_pending = 0;
    if (!guest) runtime_ai_catchup_results_publish(finished_mask);
    for (int i = 0; i < count; ++i)
        printf("Native private TEAM: finish=%d %s=%d team=%s award=%d\n", i + 1,
               results[i].human ? "player" : "AI", results[i].id,
               team_names[rank_team[i]], results[i].points);
    printf("Native private TEAM: result panels ranking=%s finish=%s\n",
           handle_object(&team_ranking_project) ? "resolved" : "unresolved",
           handle_object(&cup_finish_project) ? "resolved" : "unresolved");
    printf("Native private TEAM: host results BLUE=%d RED=%d GREEN=%d YELLOW=%d\n",
           totals[0], totals[1], totals[2], totals[3]);
}

/* BattleTeam's serialized d8 flag binds rows to team statistics. Racing
 * keeps the native racer binding/name and overrides only finish rank/award. */
static void team_ranking_row(unsigned char *self) {
    unsigned char *racer = handle_object((void **)(self + 0x78));
    if (!racer) return;
    const struct CupTotal key = racer_identity(racer);
    int finished = team_result_count || team_results_pending ? 0 : team_racer_finished(racer);
    for (int i=0;i<team_result_count;++i) {
        const struct CupTotal *result=team_results+i;
        if (key.human==result->human && key.id==result->id && key.online_id==result->online_id) {
            finished=!!(team_finished_mask & (1u << i));
            break;
        }
    }
    unsigned char *name = pointer_at(self, 0xa8);
    unsigned char *highlight = pointer_at(self, 0xc0);
    if (highlight) {
        /* Keep native local-player visibility, but suppress its bright layer for DNF. */
        const float opacity = finished ? 1.0f : 0.0f;
        memcpy(highlight + 0xe0, &opacity, sizeof opacity);
        unsigned flags;
        memcpy(&flags, highlight + 12, 4); flags |= 0x1000;
        memcpy(highlight + 12, &flags, 4);
    }
    unsigned highlight_flags = 0x400;
    if (highlight) memcpy(&highlight_flags, highlight + 12, 4);
    float text_color[] = {1, 1, 1, 1};
    /* Native finish-time text uses this serialized RGBA color for the local row. */
    if (finished && !(highlight_flags & 0x400))
        for (int c = 0; c < 4; ++c) text_color[c] = self[0xda + c] / 255.0f;
    const int team = racer_team(racer);
    unsigned char *position = pointer_at(self, 0xb8);
    team_color(position, team);
    unsigned char *position_text = position ? sprite_asset(position) : NULL;
    if (position_text) {
        const int gradient = 0; /* Native local-player gradient otherwise masks the team tint. */
        memcpy(position_text + 0x40, &gradient, sizeof gradient);
    }
    if (name) {
        memcpy(name + 0xc0, text_color, sizeof text_color);
        unsigned flags;
        memcpy(&flags, name + 12, 4); flags |= 0x1000;
        memcpy(name + 12, &flags, 4);
    }
    unsigned char *avatar = pointer_at(self, 0x98);
    const char *avatar_name = avatar ? pointer_at(avatar, 0x28) : NULL;
    if (avatar_name && !strcmp(avatar_name, "OCTR_TEAM_AVATAR")) {
        hide_placeable(avatar, 0);
    }
    unsigned char *row = name ? pointer_at(name, 0x40) : NULL;
    if (row) {
        /* Fade the shared parent once: every icon, label, background and point inherits it. */
        const float opacity = finished ? 1.0f : 0.6f;
        memcpy(row + 0xe0, &opacity, sizeof opacity);
        unsigned flags;
        memcpy(&flags, row + 12, 4); flags |= 0x1000;
        memcpy(row + 12, &flags, 4);
    }
    unsigned char *engine = pointer_at(self, 0xa0);
    if (engine) {
        memcpy(engine + 0xc0, text_color, sizeof text_color);
        unsigned flags;
        memcpy(&flags, engine + 12, 4); flags |= 0x1000;
        memcpy(engine + 12, &flags, 4);
    }
    const char *engine_name = engine ? pointer_at(engine, 0x28) : NULL;
    if (engine_name && !strcmp(engine_name, "OCTR_TEAM_ENGINE")) {
        hide_placeable(engine, 0);
        unsigned char *name = pointer_at(self, 0xa8);
        if (name) {
            const float x = 158.0f; /* Native finish-panel name column. */
            memcpy(name + 0x60, &x, 4);
            /* PlayerTagText already enables native forceFitInBox. Its old
             * 259px box overlapped the points after adding portrait/engine. */
            const float width = 185.0f;
            memcpy(name + 0x90, &width, 4);
            unsigned flags;
            memcpy(&flags, name + 12, 4); flags |= 0x1000;
            memcpy(name + 12, &flags, 4);
        }
    }
    /* The separate finish-time panel retains native time/status columns. */
    unsigned char *score = pointer_at(self, 0xd0);
    if (!score) return;
    memcpy(score + 0xc0, text_color, sizeof text_color);
    const float opacity = 1.0f;
    memcpy(score + 0xe0, &opacity, sizeof opacity);
    unsigned char *score_asset = sprite_asset(score);
    if (score_asset) {
        const int gradient = 0;
        memcpy(score_asset + 0x40, &gradient, sizeof gradient);
    }
    unsigned score_flags;
    memcpy(&score_flags, score + 12, 4); score_flags |= 0x1000;
    memcpy(score + 12, &score_flags, 4);
    for (int i = 0; i < team_result_count; ++i) {
        const struct CupTotal *result = team_results + i;
        if (key.human != result->human || key.id != result->id || key.online_id != result->online_id)
            continue;
        char text[16];
        snprintf(text, sizeof text, "%d", result->finish_position);
        unsigned char *position = pointer_at(self, 0xb8);
        if (position) set_text(position, text);
        snprintf(text, sizeof text, "%d POINT", result->points);
        if (score) {
            const float scale[] = {.75f, .75f, 1.0f};
            memcpy(score + 0x84, scale, sizeof scale);
            set_text(score, text);
        }
        break;
    }
}

static void ABI ranking_row_event(unsigned char *self, void *event, void *owner) {
    const int team_race = opening_team_race_ui || team_ranking_project != NULL;
    if (team_race) self[0xd8] = 0;
    unsigned char *extras[2] = {NULL, NULL};
    const char *tags[] = {"OCTR_TEAM_AVATAR", "OCTR_TEAM_ENGINE"};
    if (!team_race) for (int i = 0; i < 2; ++i) {
        unsigned char *icon = pointer_at(self, 0x98 + i * 8);
        const char *name = icon ? pointer_at(icon, 0x28) : NULL;
        if (name && !strcmp(name, tags[i])) {
            /* Shared Battle assets contain our extras; native binding reveals them. */
            extras[i] = icon;
            hide_placeable(icon, 1);
            void *empty = NULL;
            memcpy(self + 0x98 + i * 8, &empty, sizeof empty);
        }
    }
    original_ranking_row_event(self, event, owner);
    for (int i = 0; i < 2; ++i)
        if (extras[i]) memcpy(self + 0x98 + i * 8, &extras[i], sizeof extras[i]);
    if (team_race) team_ranking_row(self);
}

static int local_race_team(void) {
    unsigned char *project = handle_object(&team_ranking_project);
    int controller = 1;
    if (project) memcpy(&controller, project + 0x44, 4);
    if (!controller) controller = 1;
    int id = controller_player(controller);
    if (id < 0 && controller != 1) id = controller_player(1);
    void *list = player_manager && *player_manager ? pointer_at(*player_manager, 0x10) : NULL;
    unsigned char *player = list ? find_player(list, id) : NULL;
    int team = -1;
    if (player) memcpy(&team, player + 0xe4, 4);
    return team >= 0 && team < 4 ? team : -1;
}

static void team_result_rows(unsigned char *row, int depth) {
    if (!row || depth > 16) return;
    unsigned char *composition = handle_object((void **)(row + 0x38));
    if (composition && pointer_at(composition, 0) == composition_vtable)
        team_result_rows(pointer_at(composition, 0x20), depth + 1);
    const char *own_name = pointer_at(row, 0x28);
    if (own_name && !strcmp(own_name, "OCTR_TEAM_LIST") && composition) {
        unsigned char *item = pointer_at(composition, 0x20);
        const float height = 86.0f; /* Battle row artwork needs its original 77px pitch. */
        const float y = -7.0f;
        /* Carry the clip with the list; leave space below the points heading. */
        unsigned char *parent = pointer_at(row, 0x40);
        if (parent) {
            const float parent_y = 260.0f;
            memcpy(parent + 0x64, &parent_y, 4);
            unsigned parent_flags;
            memcpy(&parent_flags, parent + 12, 4); parent_flags |= 0x1000;
            memcpy(parent + 12, &parent_flags, 4);
        }
        memcpy(row + 0x64, &y, 4);
        unsigned flags;
        memcpy(&flags, row + 12, 4); flags |= 0x1000;
        memcpy(row + 12, &flags, 4);
        const int visible = 8;
        if (item) memcpy(item + 0x94, &height, 4);
        memcpy(composition + 0x90, &visible, 4);
    }
    if (own_name && (!strcmp(own_name, "[WinnerNameText]") || !strcmp(own_name, "[WinsText]") ||
                     !strcmp(own_name, "OCTR_TEAM_POINTS"))) {
        const int best = team_winner;
        if (best >= 4 || !team_result_count) return;
        const int local = local_race_team();
        char title[128];
        float y;
        if (!strcmp(own_name, "[WinsText]")) {
            snprintf(title, sizeof title, "%s", best < 0 || local < 0 ? "RESULT" : local == best ? "WINS" : "LOSE");
            y = -19.0f;
            /* Keep the native text material's gradient, without team tint. */
        } else if (!strcmp(own_name, "OCTR_TEAM_POINTS")) {
            snprintf(title, sizeof title, "%d POINTS%s", best >= 0 ? team_totals[best] : 0, team_tiebreak ? " (H2H)" : "");
            y = 9.0f;
            const float scale[] = {0.08081815f, 0.11369143f, 1.0f};
            memcpy(row + 0x84, scale, sizeof scale);
            hide_placeable(row, 0);
        } else {
            if (best < 0) snprintf(title, sizeof title, "NO FINISHERS");
            else snprintf(title, sizeof title, "%s TEAM WINS", team_names[best]);
            y = -4.0f;
            const float white[] = {1, 1, 1, 1};
            memcpy(row + 0xc0, white, sizeof white);
        }
        memcpy(row + 0x64, &y, sizeof y);
        unsigned flags;
        memcpy(&flags, row + 12, 4);
        flags |= 0x1000;
        memcpy(row + 12, &flags, 4);
        set_text(row, title);
    }
    unsigned char *behaviors = pointer_at(row, 0xe8);
    int behavior_count = 0;
    if (behaviors) memcpy(&behavior_count, behaviors + 12, 4);
    void **behavior_items = behaviors ? pointer_at(behaviors, 0x20) : NULL;
    if (behavior_items && behavior_count >= 0 && behavior_count <= 128)
        for (int i = 0; i < behavior_count; ++i)
            if (behavior_items[i] && pointer_at(behavior_items[i], 0) == ranking_row_vtable)
                team_ranking_row(behavior_items[i]);
    unsigned char *children = pointer_at(row, 0x48);
    int count = 0;
    if (children) memcpy(&count, children + 12, 4);
    void **items = children ? pointer_at(children, 0x20) : NULL;
    if (!items || count < 0 || count > 512) return;
    for (int i = 0; i < count; ++i) team_result_rows(items[i], depth + 1);

}

static void refresh_team_results(void) {
    if (team_results_pending) apply_team_results(pending_team_results);
    unsigned char *project = handle_object(&team_ranking_project);
    if (project) team_result_rows(pointer_at(project, 0x10), 0);
    play_team_result_effects();
}

static int racer_team(unsigned char *racer) {
    unsigned char *player = handle_object((void **)(racer + 0x10));
    int team = -1;
    if (player) memcpy(&team, player + 0xe4, 4);
    else team = racer_faction(racer) - 0x16;
    return team;
}

static int private_team_race(void) {
    int teams = 1;
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    return private_single_race_active && !private_cup_race_active && (teams == 2 || teams == 4);
}

static void ABI individual_confetti(void *racer) {
    if (!private_team_race()) native_win_confetti(racer);
}

typedef void (ABI *SoundNode)(void *, unsigned char *);
static SoundNode original_team_legend_node;
static void ABI team_legend_node(void *context, unsigned char *pins) {
    int count = 0;
    if (pins) memcpy(&count, pins, 4);
    if ((count == 5 || count == 6) && private_team_selector_allowed()) return;
    original_team_legend_node(context, pins);
}

static SoundNode original_finish_sound_node;
static void ABI finish_sound_node(void *context, unsigned char *pins) {
    int count = 0;
    if (pins) memcpy(&count, pins, 4);
    /* Only the marked win/loss stings in the native race music graph. */
    if (count != 5 || !private_team_race()) original_finish_sound_node(context, pins);
}

/* Replay only native presentation after the host result and scoreboard agree.
 * f382f0 emits the confetti message; f35470 also grants rewards and is not used. */
static void play_team_result_effects(void) {
    if (team_effects_played || !team_score_finished || !team_result_count || team_winner < 0) return;
    unsigned char *project = handle_object(&team_ranking_project);
    if (!project || !racer_manager || !*racer_manager) return;
    int controller = 1;
    memcpy(&controller, project + 0x44, 4);
    if (!controller) controller = 1;
    int id = controller_player(controller);
    if (id < 0 && controller != 1) id = controller_player(1);
    void *players = player_manager && *player_manager ? pointer_at(*player_manager, 0x10) : NULL;
    unsigned char *local_player = players && id >= 0 ? find_player(players, id) : NULL;
    if (!local_player) return;
    unsigned char *list = pointer_at(*racer_manager, 0x10);
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    if (!items || count < 1 || count > 8) return;
    for (int i = 0; i < count; ++i) {
        unsigned char *racer = items[i];
        unsigned char *player = racer ? handle_object((void **)(racer + 0x10)) : NULL;
        /* Controller IDs are local; the network slot can differ on a guest. */
        if (!player || player != local_player) continue;
        const int team = racer_team(racer);
        unsigned char *kart = handle_object((void **)(racer + 0x20));
        if (team < 0 || team >= 4 || !kart) return;
        GetType animation = (GetType)pointer_at(pointer_at(kart, 0), 0x110);
        void *anim = animation ? animation(kart) : NULL;
        if (!anim) return;
        const int won = team == team_winner;
        team_effects_played = 1; /* Native messages can synchronously reenter GUI dispatch. */
        set_animation_selector(anim, "VictoryLapAnimSelector", !won);
        send_animation_event(anim, "OnVictoryLap");
        if (won) native_win_confetti(racer);
        void *sound = NULL, *callback[2] = {NULL, NULL};
        play_result_sound(kart, won ? "UI_Player_Lap_Change_End_Win" : "UI_Player_Lap_Change_End_Lose",
                          &sound, callback);
        printf("Native private TEAM: scoreboard effects player=%d team=%d winner=%d confetti=%d sound=%d\n",
               id, team, team_winner, won, sound != NULL);
        if (sound) release_result_sound(sound);
        return;
    }
}

static void team_color(unsigned char *placeable, int team) {
    if (!placeable || team < 0 || team >= 4) return;
    static const float colors[4][4] = {
        {0.1f, 0.55f, 1.0f, 1.0f}, {1.0f, 0.15f, 0.12f, 1.0f},
        {0.15f, 1.0f, 0.3f, 1.0f}, {1.0f, 0.85f, 0.1f, 1.0f}
    };
    /* igGuiPlaceable::_color, matching native 252c10's float conversion. */
    if (memcmp(placeable + 0xc0, colors[team], sizeof colors[team])) {
        memcpy(placeable + 0xc0, colors[team], sizeof colors[team]);
        unsigned flags;
        memcpy(&flags, placeable + 12, 4);
        flags |= 0x1000;
        memcpy(placeable + 12, &flags, 4);
    }
}

static void team_leader_border(unsigned char *self) {
    if (!team_leaders_project && !opening_team_race_ui) return;
    unsigned char *racer = handle_object((void **)(self + 0x88));
    unsigned char *border = pointer_at(self, 0x50);
    if (!racer || !border) return;
    const int team = racer_team(racer);
    if (team < 0 || team >= 4) return;
    team_color(border, team);
    hide_placeable(border, 0);
}

static void team_marker(unsigned char *self) {
    if (!team_leaders_project && !opening_team_race_ui) return;
    unsigned char *racer = handle_object((void **)(self + 0x70));
    unsigned char *label = pointer_at(self, 0x50), *arrow = pointer_at(self, 0x48);
    if (!racer || !label || !arrow) return;
    self[0x88] = 1; /* Use native Battle projection and faction updates. */
    team_color(arrow, racer_team(racer));
    const float opaque = 1.0f, white[] = {1, 1, 1, 1};
    memcpy(arrow + 0xe0, &opaque, 4);
    hide_placeable(arrow, 0);
    /* The native leader decoration is a second icon; Team Race uses one arrow. */
    unsigned char *children = pointer_at(arrow, 0x48);
    int count = 0;
    if (children) memcpy(&count, children + 12, 4);
    void **items = children ? pointer_at(children, 0x20) : NULL;
    for (int i = 0; items && i < count && i < 16; ++i) hide_placeable(items[i], 1);
    const int human = handle_object((void **)(racer + 0x10)) != NULL;
    hide_placeable(label, !human);
    if (!human) set_text(label, "");
    memcpy(label + 0xc0, white, sizeof white);
    /* Place the name below the existing arrow using its own native geometry. */
    float y, height, pivot, scale, text_height, text_pivot;
    memcpy(&y, arrow + 0x64, 4); memcpy(&height, arrow + 0x94, 4);
    memcpy(&pivot, arrow + 0x70, 4); memcpy(&scale, arrow + 0x88, 4);
    memcpy(&text_height, label + 0x94, 4); memcpy(&text_pivot, label + 0x70, 4);
    y += height * scale * (1.0f - pivot) + text_height * (text_pivot + 0.2f);
    memcpy(label + 0x64, &y, 4);
    unsigned flags;
    memcpy(&flags, label + 12, 4); flags |= 0x1000;
    memcpy(label + 12, &flags, 4);
}

static void ABI marker_event(unsigned char *self, void *event, void *owner) {
    const int team = team_leaders_project || opening_team_race_ui;
    GetType event_type = event ? (GetType)pointer_at(pointer_at(event, 0), 0) : NULL;
    const int initializing = team && event_type && event_type(event) == *marker_initialize_type;
    unsigned char saved = 0;
    if (initializing) {
        saved = ((unsigned char *)event)[0x28];
        ((unsigned char *)event)[0x28] = 1; /* Skip the Race layout that hides the Battle arrow. */
    }
    original_marker_event(self, event, owner);
    if (initializing) ((unsigned char *)event)[0x28] = saved;
    team_marker(self);
}

static void ABI leader_event(unsigned char *self, void *event, void *owner) {
    original_leader_event(self, event, owner);
    team_leader_border(self);
}

static void colored_name(unsigned char *placeable, int allowed);
static int name_colors_allowed(void);
static void ABI leader_update(unsigned char *self, unsigned char *owner, float dt) {
    original_leader_update(self, owner, dt);
    team_leader_border(self);
    colored_name(pointer_at(self, 0x20), name_colors_allowed()); /* _playerTagText */
}

static void ABI race_team_setup(unsigned char *self) {
    int mode = 0, teams = 0;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd4c, 4);
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    /* Native caller assigns AI factions only for Battle; the helper itself
     * enforces host authority and replicates the entity faction normally. */
    if (mode == 1 && !runtime_online_mode_private_cup() && teams >= 2 && teams <= 4 && runtime_online_mode_in_private() && (*race_settings)[0x10])
        assign_ai_teams();
    original_race_setup(self);
}

static void ABI place_cup_racers(unsigned char *self) {
    if (private_cup_race_active && current_cup_index() > 0) {
        unsigned char *manager = racer_manager ? *racer_manager : NULL;
        unsigned char *list = manager ? pointer_at(manager, 0x10) : NULL;
        int count = 0, restored = 0;
        if (list) memcpy(&count, list + 12, 4);
        void **items = list ? pointer_at(list, 0x20) : NULL;
        for (int i = 0; items && i < count && i < 8; ++i) {
            unsigned char *racer = items[i];
            if (!racer) continue;
            const struct CupTotal key = racer_identity(racer);
            for (int j = 0; j < cup_total_count; ++j) {
                const struct CupTotal *saved = cup_totals + j;
                if (key.human != saved->human || key.id != saved->id ||
                    key.online_id != saved->online_id) continue;
                /* Native later-track placement reads last finish, not total points. */
                if (saved->finish_position > 0 && saved->finish_position <= count) {
                    memcpy(racer + 0x60, &saved->finish_position, 4);
                    ++restored;
                }
                restore_cup_total(racer);
                break;
            }
        }
        printf("Native private CUP: restored previous finish grid %d/%d before placement\n", restored, count);
    }
    original_place_racers(self);
    snapshot_private_grid();
}

static void snapshot_cup_totals(void) {
    /* Local finish callbacks can still mutate racers after the host results. */
    cup_total_count = cup_result_count;
    for (int i = 0; i < cup_result_count; ++i) {
        cup_totals[i] = cup_results[i].key;
        cup_totals[i].finish_position = cup_results[i].position;
    }
}

static void restore_finish_position(unsigned char *racer) {
    const struct CupTotal key = racer_identity(racer);
    for (int i = 0; i < finish_order_count; ++i) {
        const struct CupTotal *saved = finish_order + i;
        if (key.human != saved->human || key.id != saved->id ||
            key.online_id != saved->online_id) continue;
        const int position = i + 1;
        memcpy(racer + 0x58, &position, 4);
        memcpy(racer + 0x60, &position, 4);
        memcpy(racer + 0x64, &position, 4);
        break;
    }
}

static void restore_cup_total(unsigned char *racer) {
    if (!private_cup_race_active || racer[0x8c]) return;
    struct CupTotal key = racer_identity(racer);
    for (int i = 0; i < cup_total_count; ++i) {
        struct CupTotal *saved = cup_totals + i;
        if (key.human == saved->human && key.id == saved->id && key.online_id == saved->online_id) {
            memcpy(racer + 0xb8, &saved->points, 4);
            return;
        }
    }
}

/* Network lap timestamps are authoritative. A follower's local clock can
 * differ, and its final-time callback can be skipped until race teardown. */
static void synchronize_cup_finish_time(unsigned char *racer) {
    if (!private_cup_race_active || !racer[0x8c]) return;
    unsigned char *laps=pointer_at(racer,0x90);
    int required=0, count=0;
    if (race_settings && *race_settings) memcpy(&required,*race_settings+12,4);
    if (laps) memcpy(&count,laps+12,4);
    const int *times=laps ? pointer_at(laps,0x20) : NULL;
    if (!times || required<1 || required>99 || count!=required) return;
    for (int i=0;i<required;++i)
        if (times[i]<=0 || (i && times[i]<=times[i-1])) return;
    memcpy(racer+0x68,&count,4);
    memcpy(racer+0x6c,times+required-1,4);
}

static void trace_cup_finish(const char *stage, unsigned char *racer) {
    unsigned char *laps=pointer_at(racer,0x90);
    int count=0, lap=0, required=0, final_time=0;
    if (laps) memcpy(&count,laps+12,4);
    const int *times=laps ? pointer_at(laps,0x20) : NULL;
    memcpy(&lap,racer+0x4c,4);
    memcpy(&final_time,racer+0x6c,4);
    if (race_settings && *race_settings) memcpy(&required,*race_settings+12,4);
    printf("Native private CUP finish: stage=%s id=%d local=%d finished=%d lap=%d recorded=%d required=%d last_time=%d final_time=%d\n",
           stage,result_racer_id(racer),racer[0xb0]!=0,racer[0x8c]!=0,lap,count,required,
           times && count>0 && count<=99 ? times[count-1] : -1,final_time);
}

static void ABI racer_finished(unsigned char *racer, int finished) {
    restore_cup_total(racer);
    const int first_finish=finished && !racer[0x8c];
    restore_finish_position(racer);
    original_finish_racer(racer, finished);
    restore_finish_position(racer);
    if (private_cup_race_active && first_finish) trace_cup_finish("native-callback",racer);
    if (team_result_count && team_ranking_project) {
        refresh_team_results();
    }
    if (!private_cup_race_active || !cup_result_count) return;
    synchronize_cup_finish_time(racer);
    const struct CupTotal key = racer_identity(racer);
    for (int i = 0; i < cup_result_count; ++i) {
        const struct CupTotal *result = &cup_results[i].key;
        if (key.human != result->human || key.id != result->id ||
            key.online_id != result->online_id) continue;
        /* A late finish callback must not overwrite the host's final award. */
        memcpy(racer + 0x64, &cup_results[i].position, 4);
        memcpy(racer + 0xb4, &cup_results[i].award, 4);
        memcpy(racer + 0xb8, &result->points, 4);
        break;
    }
}

/* Native lap recording is the checkpoint-validated boundary. Cup AI may
 * keep driving after that final record without the human finish callback. */
static void ABI record_private_cup_lap(void *self, unsigned char *racer, int lap) {
    original_record_lap(self, racer, lap);
    if (!private_cup_race_active || !racer || racer[0x8c] || !racer[0xb0] ||
        pointer_at(racer, 0x10)) return;
    unsigned char *entity = handle_object((void **)(racer + 0x20));
    if (!entity || !(entity[0x3b] & 1)) return;
    unsigned char *laps = pointer_at(racer, 0x90);
    int required = 0, completed = 0, current = 0;
    if (race_settings && *race_settings) memcpy(&required, *race_settings + 12, 4);
    if (laps) memcpy(&completed, laps + 12, 4);
    memcpy(&current, racer + 0x4c, 4);
    const int *times = laps ? pointer_at(laps, 0x20) : NULL;
    if (required < 1 || required > 99 || lap != required || current != required + 1 ||
        completed != required || !times) return;
    for (int i=0; i<required; ++i)
        if (times[i] <= 0 || (i && times[i] <= times[i-1])) return;
    printf("Native private CUP: completing recorded AI finish id=%d laps=%d time=%d\n",
           result_racer_id(racer), completed, times[required-1]);
    /* Native completion publishes the racer event and performs race-end
     * bookkeeping. Never set the flag/position or invent a lap time here. */
    complete_racer(self, racer);
}

/* Online results carry host player IDs in finishing order. Arcade awards made
 * before this event are provisional: replace them, never add a second award. */
static int apply_cup_results(unsigned char *event) {
    if (!private_cup_race_active) return 0;
    unsigned char *manager = racer_manager ? *racer_manager : NULL;
    unsigned char *list = manager ? pointer_at(manager, 0x10) : NULL;
    unsigned char *system = system_data ? *system_data : NULL;
    unsigned char *table = system ? pointer_at(system, 0x180) : NULL;
    int count = 0, awards = 0;
    if (list) memcpy(&count, list + 12, 4);
    if (table) memcpy(&awards, table + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    const int *points = table ? pointer_at(table, 0x20) : NULL;
    if (!items || !points || count < 1 || count > 8 || awards < 0 || awards > 8) return 0;
    int positions[8];
    struct CupTotal keys[8];
    unsigned occupied = 0;
    for (int i = 0; i < count; ++i) {
        if (!items[i]) return 0;
        keys[i] = racer_identity(items[i]);
        positions[i] = -1;
        for (int rank = 0; rank < count; ++rank) {
            int id;
            memcpy(&id, event + 0x20 + rank * 4, 4);
            if (id < 0 || id != result_racer_id(items[i])) continue;
            if (positions[i] >= 0 || (occupied & (1u << rank))) return 0;
            positions[i] = rank;
            occupied |= 1u << rank;
        }
        if (positions[i] < 0) return 0;
    }
    for (int i = 0; i < count; ++i) {
        unsigned char *racer = items[i];
        int previous = 0;
        for (int j = 0; j < cup_total_count; ++j) {
            const struct CupTotal *saved = cup_totals + j;
            if (keys[i].human == saved->human && keys[i].id == saved->id &&
                keys[i].online_id == saved->online_id) {
                previous = saved->points;
                break;
            }
        }
        synchronize_cup_finish_time(racer);
        if (!cup_result_count) trace_cup_finish("host-results",racer);
        const int position = positions[i] + 1;
        const int award = positions[i] < awards ? points[positions[i]] : 0;
        const int total = previous + award;
        memcpy(racer + 0x58, &position, 4);
        memcpy(racer + 0x60, &position, 4);
        memcpy(racer + 0x64, &position, 4);
        memcpy(racer + 0xb4, &award, 4);
        memcpy(racer + 0xb8, &total, 4);
        cup_results[i].key = keys[i];
        cup_results[i].key.points = total;
        cup_results[i].position = position;
        cup_results[i].award = award;
    }
    cup_result_count = count;
    return 1;
}

/* Only the podium slot changes; native online code still builds the driver,
 * customization and player name, without changing the race's finish order. */
static int ABI podium_position(void *racer, int finish_position) {
    for (int i = 0; i < podium_count; ++i)
        if (podium_order[i] == racer) return i + 1;
    return finish_position;
}

/* Activate the scoreboard prepared with the native race UI. Do not create a
 * project from inside its first RaceFinished broadcast. */
static void open_team_score(void) {
    if (!team_race_ui_owner || team_score_opened) return;
    trace_team_score("activate", NULL, NULL);
    unsigned char *score = handle_object(&team_ranking_project);
    if (!score) {
        puts("Native private TEAM: prepared scoreboard unresolved; retaining native finish flow");
        return;
    }
    assign_handle((unsigned char *)&cup_ranking_project, team_ranking_project);
    /* Native Battle dispatch already uses +68. Retire the race-only end-flow
     * slot so ea4540 does not run its separate results pipeline afterward. */
    assign_handle(team_race_ui_owner + 0x60, NULL);
    team_score_opened = 1;
    hide_cup_finish_panel();
    cup_panel_done = 1;
    puts("Native private TEAM: activated native Battle scoreboard slot; race end-flow slot retired");
}

static void ABI race_results_received(unsigned char *event) {
    podium_count = 0;
    if (!capture_finish_order(event)) return;
    open_team_score();
    const int applied = apply_cup_results(event);
    if (applied && current_cup_index() == 3) {
        unsigned char *list = pointer_at(*racer_manager, 0x10);
        podium_count = cup_result_count;
        memcpy(podium_order, pointer_at(list, 0x20), podium_count * sizeof(void *));
        /* Use the native CUP sorter, including its human/finish tie breakers. */
        sort_cup_racers(podium_order, podium_order + podium_count, podium_count);
        for (int i = 0; i < podium_count && i < 3; ++i) {
            const struct CupTotal key = racer_identity(podium_order[i]);
            printf("Native private CUP: podium %d player=%d points=%d\n", i + 1, key.id, key.points);
        }
    }
    original_race_results(event);
    unsigned char *list = racer_manager && *racer_manager ? pointer_at(*racer_manager, 0x10) : NULL;
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    for (int i = 0; items && i < count && i < 8; ++i)
        if (items[i]) restore_finish_position(items[i]);
    /* Native result handling can complete remote racers; score that final state. */
    apply_team_results(event);
    if (team_race_ui_owner) trace_team_score("results-return", NULL, NULL);
    refresh_team_results();
    if (applied && podium_count) cup_final_results_ready = 1;
    podium_count = 0;
    if (applied) {
        apply_cup_results(event);
        printf("Native private CUP: host results applied for race %d/4 (%d racers)\n",
               current_cup_index() + 1, cup_result_count);
        if (cup_timer_pending) {
            cup_timer_pending = 0;
            private_race_timer_done();
        }
    }
}

static int ABI race_finished(unsigned char *self) {
    unsigned char *manager = racer_manager ? *racer_manager : NULL;
    unsigned char *list = manager ? pointer_at(manager, 0x10) : NULL;
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    int finished = 0;
    for (int i = 0; items && i < count; ++i) {
        unsigned char *racer = items[i];
        finished += racer[0x8c] != 0;
        if (!racer[0x8c]) cup_transition_pending = 0;
        restore_cup_total(racer);
    }
    if ((private_cup_race_active || private_single_race_active) &&
        items && count > 0 && count <= 8 && finished == count) return 1;
    return original_race_finished(self);
}

/* This is the host's native end-race timer callback, not the disconnect or
 * leave-race handlers. The fourth race retains the native lobby transition. */
static void ABI private_race_timer_done(void) {
    unsigned char *state = network_state();
    int state_id;
    memcpy(&state_id, state + 0x144, 4);
    if (!network_race(state_id)) return;
    if (runtime_online_mode_private_cup() && current_cup()) {
        if (private_cup_race_active && !cup_result_count) {
            cup_timer_pending = 1;
            return;
        }
        const int index = current_cup_index();
        if (index >= 0 && index < 3) {
            unsigned char *levels = pointer_at(*race_manager, 0xd80);
            int count = 0;
            if (levels) memcpy(&count, levels + 12, 4);
            void **items = levels ? pointer_at(levels, 0x20) : NULL;
            unsigned char *zone = items && index < count ? handle_object(items + index) : NULL;
            if (zone) {
                printf("Native private CUP: advancing from race %d/4 through native CUP map loader\n", index + 1);
                next_cup_map(zone);
                return;
            }
            fputs("Native private CUP: current track zone is missing\n", stderr);
        }
    }
    return_to_lobby();
}

/* The native guest first asks the host to return to the lobby, then drops
 * the host when its grace timer expires. Cup needs the same end-of-results
 * deadline as the host, without that single-race disconnect handshake. */
static void ABI private_guest_return_timer(unsigned char *self) {
    if (private_cup_race_active && current_cup()) {
        puts("Native private CUP: guest following host countdown; retaining session");
        schedule_guest_return(self, private_race_timer_done, self);
        return;
    }
    original_guest_return_timer(self);
}

static void update_private_cup_track(void) {
    unsigned char *manager = *race_manager;
    unsigned char *settings = pointer_at(manager, 0xde8);
    unsigned char *levels = pointer_at(manager, 0xd80);
    int count = 0;
    if (levels) memcpy(&count, levels + 12, 4);
    if (!settings || count != 4) return;
    void **items = pointer_at(levels, 0x20);
    unsigned char *zone = items ? handle_object(items) : NULL;
    if (zone) {
        set_string(settings + 0x10, pointer_at(zone, 0x10));
        /* The Arcade picker does not emit the private lobby's settings event. */
        publish_private_track(settings + 0x10);
    }
}

static void ABI level_button_event(unsigned char *self, void *event, void *owner) {
    ensure_default_cup();
    original_level_button(self, event, owner);
    GetType *vtable = *(GetType **)event;
    if (!runtime_online_mode_private_cup() || vtable[0](event) != *initialize_type) return;
    unsigned char *cup = current_cup();
    set_text(pointer_at(self, 0x10), cup ? pointer_at(cup, 0x30) : "SELECT CUP");
    unsigned char *icon = pointer_at(self, 0x18);
    unsigned char *sprite = icon ? sprite_asset(icon) : NULL;
    if (cup && sprite) assign_handle(sprite + 0x20, pointer_at(cup, 0x40));
    unsigned char *children = owner ? pointer_at(owner, 0x48) : NULL;
    int count = 0;
    if (children) memcpy(&count, children + 12, 4);
    void **items = children ? pointer_at(children, 0x20) : NULL;
    for (int i = 0; items && i < count; ++i) {
        unsigned char *child = items[i];
        const char *name = child ? pointer_at(child, 0x28) : NULL;
        if (name && !strcmp(name, "SelectedTrack")) set_text(child, "CUP");
    }
}

static void update_card(unsigned char *row, const char *cup_name, const char *mode, void *preview, int depth) {
    if (!row || depth > 16) return;
    const char *name = pointer_at(row, 0x28);
    if (name && (!strcmp(name, "[ModeName]") || !strcmp(name, "MapVoteResultModeName"))) set_text(row, mode);
    else if (name && !strcmp(name, "[MapName]") && cup_name) set_text(row, cup_name);
    else if (name && !strcmp(name, "OCTR_PRIVATE_PREVIEW") && preview) {
        unsigned char *sprite = sprite_asset(row);
        if (sprite && pointer_at(sprite, 0x20) != preview) {
            assign_handle(sprite + 0x20, preview);
            unsigned flags;
            memcpy(&flags, row + 12, 4);
            flags |= 0x1000; /* rebuild the existing native sprite draw state */
            memcpy(row + 12, &flags, 4);
        }
    }
    unsigned char *children = pointer_at(row, 0x48);
    int count = 0;
    if (children) memcpy(&count, children + 12, 4);
    void **items = children ? pointer_at(children, 0x20) : NULL;
    for (int i = 0; items && i < count; ++i)
        update_card(items[i], cup_name, mode, preview, depth + 1);
}

static void ABI lobby_event(unsigned char *self, void *event, void *owner) {
    ensure_default_cup();
    original_lobby_event(self, event, owner);
    refresh_lobby_card(owner);
}

/* Team selection can hide the lobby while mode verification is in flight. */
static int resume_private_map(void) {
    if (!pending_private_map[0]) return 0;
    char map[sizeof pending_private_map]; memcpy(map,pending_private_map,sizeof map);
    load_private_map(map,pending_private_reason);
    if (!pending_private_map[0]) puts("Native private race: pending map transition handled");
    return !pending_private_map[0];
}

/* The lobby's screen is on: its behaviour updated just now. The level's name cannot say it: outside the online mode too,
 * a race that is left comes back to octane_onlinelobby, the menus' backdrop. */
static uint64_t monotonic_ms(void);
static uint64_t lobby_updated_ms;

/* runtime_online_mode_in_private reads the game's entities, on the game's thread only */
static int room_on, public_on;
void runtime_online_mode_note_frame(void) {
    __atomic_store_n(&room_on, runtime_online_mode_in_private(), __ATOMIC_RELAXED);
    __atomic_store_n(&public_on, runtime_public_match_active() && runtime_online_mode_lobby_open(), __ATOMIC_RELAXED);
}
int runtime_online_mode_room_on(void) { return __atomic_load_n(&room_on, __ATOMIC_RELAXED); }
int runtime_online_mode_public_on(void) { return __atomic_load_n(&public_on, __ATOMIC_RELAXED); }

int runtime_online_mode_lobby_open(void) {
    const uint64_t at = __atomic_load_n(&lobby_updated_ms, __ATOMIC_RELAXED);
    return at && monotonic_ms() - at < 1000;
}

static void ABI lobby_update(unsigned char *self, unsigned char *owner, float dt) {
    __atomic_store_n(&lobby_updated_ms, monotonic_ms(), __ATOMIC_RELAXED);
    original_lobby_update(self, owner, dt);
    refresh_lobby_card(owner);
    runtime_public_itemless_update();
    if (runtime_private_party_update()) return;
    resume_private_map();
}

static void refresh_lobby_card(unsigned char *owner) {
    if (!owner) return;
    const char *tag = pointer_at(owner, 0x28);
    if (tag && !strcmp(tag, "OCTR_PUBLIC_LOBBY")) {
        /* Party members can retain private scene/selector state in public. */
        int type = 0;
        if (network && *network) memcpy(&type, *network + 0x13c, sizeof type);
        if (type != 2)
            update_card(owner, NULL, runtime_ranked_active() ?
                        (runtime_public_itemless_active() ? "RANKED - ITEMLESS" : "RANKED - NORMAL") :
                        (runtime_public_itemless_active() ? "SINGLE RACE - ITEMLESS" : "SINGLE RACE - NORMAL"), NULL, 0);
        return;
    }
    if (battle_selected() || !runtime_online_mode_in_private()) return;
    if (!tag || strcmp(tag, "OCTR_PRIVATE_LOBBY")) return;
    const int cup_mode = runtime_online_mode_private_cup();
    unsigned char *cup = cup_mode ? current_cup() : NULL;
    void *preview = cup ? pointer_at(cup, 0x58) : NULL;
    const char *name = cup ? pointer_at(cup, 0x30) : cup_mode ? "SELECT CUP" : NULL;
    if (!cup_mode && race_manager && *race_manager) {
        unsigned char *settings = pointer_at(*race_manager, 0xde8);
        const char *map = settings ? pointer_at(settings, 0x10) : NULL;
        unsigned char *zone = map ? find_zone(*zone_manager, map) : NULL;
        unsigned char *data = zone ? pointer_at(zone, 0x1b8) : NULL;
        if (zone) name = pointer_at(zone, 0xa0);
        if (data) preview = pointer_at(data, 0x18);
    }
    char mode[48];
    /* Show the chosen/adopted mode even before another player sends game traffic. */
    snprintf(mode, sizeof mode, "%s RACE - %s", cup_mode ? "CUP" : "SINGLE",
             runtime_speed_mode_enabled() ? "ITEMLESS" : "NORMAL");
    update_card(owner, name, mode, preview, 0);
}

/* Private settings dispatch this native GUI update after constructing its track
 * name, preview and mode. Decorate it here, after the network settings are applied. */
static void ABI private_lobby_update(unsigned char *project, unsigned char *event) {
    unsigned char *cup = runtime_online_mode_private_cup() ? current_cup() : NULL;
    if (cup) {
        set_string(event + 0x18, pointer_at(cup, 0x30));
        assign_handle(event + 0x20, pointer_at(cup, 0x58));
        const int cup_mode = 6;
        memcpy(event + 0x28, &cup_mode, 4);
    }
    typedef void (ABI *Dispatch)(void *, void *);
    Dispatch dispatch = (Dispatch)pointer_at(pointer_at(project, 0), 0xb0);
    dispatch(project, event);
    /* Native projects wrap their menu in an igGuiComposition asset. */
    unsigned char *root = pointer_at(project, 0x10);
    refresh_lobby_card(root);
    unsigned char *composition = root ? handle_object((void **)(root + 0x38)) : NULL;
    if (composition && pointer_at(composition, 0) == composition_vtable)
        refresh_lobby_card(pointer_at(composition, 0x20));
}

static void update_timer_label(unsigned char *row, const char *text, int depth) {
    if (!row || depth > 16) return;
    const char *name = pointer_at(row, 0x28);
    if (name && !strcmp(name, "TimerDesc")) set_text(row, text);
    unsigned char *children = pointer_at(row, 0x48);
    int count = 0;
    if (children) memcpy(&count, children + 12, 4);
    void **items = children ? pointer_at(children, 0x20) : NULL;
    for (int i = 0; items && i < count; ++i) update_timer_label(items[i], text, depth + 1);
}

static void ABI timer_event(unsigned char *self, void *event, void *owner) {
    original_timer_event(self, event, owner);
    if (!owner) return;
    const char *name = pointer_at(owner, 0x28);
    if (!name || strcmp(name, "OCTR_PRIVATE_RETURN_TIMER")) return;
    if (team_ranking_project) {
        /* Keep the countdown beside the result panel, clear of its winner title. */
        const float x = 1050.0f;
        memcpy((unsigned char *)owner + 0x60, &x, sizeof x);
        unsigned flags;
        memcpy(&flags, (unsigned char *)owner + 12, sizeof flags);
        flags |= 0x1000;
        memcpy((unsigned char *)owner + 12, &flags, sizeof flags);
    }
    if (!runtime_online_mode_private_cup()) return;
    const int index = current_cup_index();
    if (index < 0 || index >= 3) return;
    char title[32];
    snprintf(title, sizeof title, "NEXT RACE %d/4", index + 2);
    update_timer_label(owner, title, 0);
}

static void ABI cup_item_event(unsigned char *self, void *event, void *owner) {
    original_cup_item(self, event, owner);
    if (!owner || !runtime_online_mode_private_cup()) return;
    unsigned char *children = pointer_at(owner, 0x48);
    int count = 0;
    if (children) memcpy(&count, children + 12, 4);
    if (count != 2) return;
    unsigned char *stars = pointer_at(pointer_at(children, 0x20), 8);
    if (!stars) return;
    unsigned flags;
    memcpy(&flags, stars + 12, 4);
    flags |= 0x1400;
    memcpy(stars + 12, &flags, 4);
}

static int race_type_selector(const unsigned char *self) {
    const char *name = pointer_at(self, 0x148);
    return name && !strcmp(name, "RACE TYPE");
}

static int battle_selected(void) {
    /* The host's selector changes before the asynchronous lobby game type. */
    if (remote_cup_selected < 0) return selector_mode_index == 2;
    int type = 0;
    if (network && *network) memcpy(&type, *network + 0x13c, sizeof type);
    return type == 2;
}

int runtime_online_mode_private_cup(void) {
    if (runtime_public_match_active()) return 0;
    return private_cup_race_active || (runtime_online_mode_in_private() && !battle_selected() &&
        (remote_cup_selected >= 0 ? remote_cup_selected : private_cup_selected));
}

int runtime_online_mode_private_teams(void) {
    if (!runtime_online_mode_private_single_options() || !race_settings || !*race_settings) return 0;
    int teams = 1;
    memcpy(&teams, *race_settings + 0x28, sizeof teams);
    return teams == 2 || teams == 4;
}

int runtime_online_mode_private_single_options(void) {
    return private_state && is_private() && !battle_selected() && !runtime_online_mode_private_cup();
}

static unsigned char *row_behavior(unsigned char *row) {
    if (!row) return NULL;
    unsigned char *list = pointer_at(row, 0xe8);
    if (!list) return NULL;
    int count;
    memcpy(&count, list + 0x0c, sizeof count);
    void *items = pointer_at(list, 0x20);
    return count > 0 && items ? pointer_at(items, 0) : NULL;
}

static void race_type_visibility(unsigned char *mode, unsigned char *owner, int shown) {
    if (!owner || !pointer_at(mode, 0x118)) return;
    unsigned char *row = pointer_at(owner, 0x58);
    unsigned char *race = row_behavior(row);
    if (!race || !race_type_selector(race)) return;
    if (!shown && is_private()) {
        private_cup_selected = 0;
        remote_cup_selected = -1;
        int selected;
        memcpy(&selected, race + 0x190, sizeof selected);
        if (selected != 0) {
            const int single = 0;
            memcpy(race + 0x190, &single, sizeof single);
            const int previous = customized_selector;
            customized_selector = 2;
            if (pointer_at(race, 0x198)) refresh_selector(race);
            customized_selector = previous;
        }
    }
    unsigned flags;
    memcpy(&flags, row + 0x0c, sizeof flags);
    flags = shown ? ((flags & ~0x1400u) | 0x1000u) : (flags | 0x1400u);
    memcpy(row + 0x0c, &flags, sizeof flags);
    const float alpha = shown ? 1.0f : 0.0f;
    memcpy(row + 0xe0, &alpha, sizeof alpha);
    unsigned char *button = pointer_at(race, 0x118);
    void *next = shown ? row : button;
    memcpy(mode + 0x118, &next, sizeof next);
    unsigned char *navigation = row_behavior(button);
    if (navigation) {
        void *previous = shown ? row : owner;
        memcpy(navigation + 0x110, &previous, sizeof previous);
    }
}

static void ABI selector_update(unsigned char *self, unsigned char *owner, float dt) {
    original_selector_update(self, owner, dt);
    unsigned char *options = pointer_at(self, 0x1a8);
    int count = 0;
    if (options) memcpy(&count, options + 12, 4);
    if (count == 3) {
        race_type_visibility(self, owner, is_private() && selector_mode_index != 2);
    } else if (count == 2 && race_type_selector(self)) {
        unsigned char *mode_owner = pointer_at(self, 0x110);
        unsigned char *mode = row_behavior(mode_owner);
        if (mode) race_type_visibility(mode, mode_owner, is_private() && selector_mode_index != 2);
    }
}

static void ABI set_season(void *manager, int mode) {
    /* Evaluated on each level load; retain the incoming theme for normal races. */
    original_season(manager, runtime_speed_race_active() ? 0 : mode);
}

static int is_private(void) {
    if (runtime_public_match_active()) return 0;
    unsigned char yes = 0, no = 0;
    private_state(&yes, &no);
    return yes != 0;
}

/* Use the same active No AI flag as the native difficulty selector (125fb30). */
static int reject_private_team_start(void) {
    unsigned char *settings = race_settings ? *race_settings : NULL;
    int teams = 1;
    if (settings) memcpy(&teams, settings + 0x28, 4);
    if (is_private() && !battle_selected() && settings && !settings[0x10] &&
        (teams == 2 || teams == 4)) {
        unsigned char *list = player_manager && *player_manager ? pointer_at(*player_manager, 0x10) : NULL;
        int players = 0;
        if (list) memcpy(&players, list + 0x0c, 4);
        if (players < teams || players > 8 || players % teams) {
            char message[192];
            snprintf(message, sizeof message,
                     "%d TEAMS WITH NO AI NEED %s PLAYERS.\nCURRENT PLAYERS: %d\nADJUST PLAYER COUNT OR ENABLE AI.",
                     teams, teams == 2 ? "2, 4, 6 OR 8" : "4 OR 8", players);
            native_notice(message, NULL);
            printf("Native private TEAM: Start blocked, No AI players=%d/8 teams=%d\n", players, teams);
            return 1;
        }
    }
    return 0;
}

/* The marked host-only branch runs before readyPlayerLobby changes UI state.
 * Native canStartGame is evaluated later, after the host becomes ready. */
static void ABI private_start_query(void *context, unsigned char *pins, unsigned char *outputs) {
    int count = 0;
    if (pins) memcpy(&count, pins, 4);
    void **nodes = pointer_at(outputs, 0x20);
    if (count == 1) {
        if (!reject_private_team_start()) run_graph_node(context, nodes[0]);
        return;
    }
    unsigned char yes = 0, no = 0;
    native_can_start(&yes, &no);
    if (no) run_graph_node(context, nodes[1]);
    if (yes) run_graph_node(context, nodes[0]);
}

static void ABI private_start_gate(void *object);
static void ABI resume_private_start(void *object) {
    /* A peer/native transition may have started the race while HTTP was pending. */
    if (!((unsigned char *)object)[0x42]) private_start_gate(object);
}

/* Also protect direct/native start callers before the network broadcast. */
static void ABI private_start_gate(void *object) {
    unsigned char *self=object;
    if (!self[0x42] && is_private() && !battle_selected() && !runtime_room_mode_ready()) {
        native_notice("Room settings are still synchronizing. Please retry Play.", NULL);
        return;
    }
    if (!self[0x42]) {
        if (reject_private_team_start()) return;
        int status=runtime_private_party_check();
        if (status == -1) runtime_private_party_defer_start(self,resume_private_start);
        if (status != 1) return;
    }
    original_lobby_start(self);
}

/* The game's own private-match flag (0 before the selector hook is installed). */
int runtime_online_mode_in_private(void) {
    if (runtime_public_match_active()) return 0;
    return private_single_race_active || private_cup_race_active || (private_state && is_private());
}
/* A private room's CUP race (set by its map load, cleared by the next one): the game's private
 * flag does not hold during these races (runtime_speed_race_active, Itemless). */
int runtime_online_mode_private_cup_race(void) { return private_cup_race_active; }

static void ABI select_mode(void *sender, int index) {
    if (customized_selector == 2) {
        if (is_private() && !battle_selected() && (index == 0 || index == 1)) {
            private_cup_selected = index;
            remote_cup_selected = -1;
            clear_team_selector();
            reset_team_count();
            if (index) { ensure_default_cup(); normalize_race_teams(); }
            printf("Native private Race Type: %s\n", index ? "CUP" : "SINGLE");
        }
        return;
    }
    if (!customized_selector) {
        original_choice(sender, index);
        return;
    }
    const int private = is_private();
    if (!private && index == 2 && runtime_ranked_selected()) return;
    if (!private && index == 1 && !runtime_public_itemless_available()) return;
    selector_mode_index = index;
    clear_team_selector();
    cup_timer_pending = 0;
    reset_team_count();
    runtime_public_itemless_select(!private && index == 1);
    const int speed = index == 1 && private;
    if (private) {
        private_speed_selected = speed;
        if (index == 2) {
            private_cup_selected = 0;
            remote_cup_selected = -1;
        }
    }
    if (runtime_online_mode_chosen) runtime_online_mode_chosen(speed, private);
    runtime_speed_mode_set(speed);
    printf("Native Online mode: %s (%s)\n", index == 1 ? "ITEMLESS" : index == 2 ? "BATTLE" : "NORMAL",
           private ? "private" : "public");
    original_choice(sender, index == 2 ? 1 : 0);
}

static void ABI selector_event(unsigned char *self, void *event, void *owner) {
    unsigned char *options;
    memcpy(&options, self + 0x1a8, sizeof options);
    int count;
    memcpy(&count, options + 0x0c, sizeof count);
    const int race = count == 2 && race_type_selector(self);
    GetType *vtable = *(GetType **)event;
    if (race && vtable[0](event) == *initialize_type) {
        memcpy(self + 0x190, &private_cup_selected, sizeof private_cup_selected);
        base_event(self, event, owner);
        printf("Native Race Type selector initialized: %s\n", private_cup_selected ? "CUP" : "SINGLE");
    } else if (count == 3 && vtable[0](event) == *initialize_type) {
        unsigned char *available, *items;
        memcpy(&available, self + 0x1b0, sizeof available);
        memcpy(&items, available + 0x20, sizeof items);
        const int private = is_private();
        items[1] = (unsigned char)(private || runtime_public_itemless_available());
        items[2] = (unsigned char)(private || !runtime_ranked_selected());
        runtime_public_itemless_reset();
        int type;
        memcpy(&type, *network + 0x13c, sizeof type);
        const int private_speed = runtime_room_mode_following() ? runtime_speed_mode_enabled() : private_speed_selected;
        const int index = type == 2 && items[2] ? 2 :
                          (items[1] && (private ? private_speed : runtime_public_itemless_selected()) ? 1 : 0);
        if (!private || index == 2) {
            clear_team_selector();
            reset_team_count();
        }
        selector_mode_index = index;
        memcpy(self + 0x190, &index, sizeof index);
        runtime_speed_mode_set(private && index == 1);
        /* The asset already supplies all options. Keep the base selector initialization. */
        base_event(self, event, owner);
        race_type_visibility(self, owner, private && index != 2);
    } else {
        const int previous = customized_selector;
        customized_selector = race ? 2 : count == 3;
        original_event(self, event, owner);
        customized_selector = previous;
        if (count == 3) {
            /* The emitted choice precedes both native UI and network updates. */
            race_type_visibility(self, owner, is_private() && selector_mode_index != 2);
        }
    }
}

static int write_code(void *target, const void *data, size_t length) {
#ifdef _WIN32
    DWORD old, ignored;
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(target, data, length);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return VirtualProtect(target, length, old, &ignored) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *first = (void *)((uintptr_t)target & ~(uintptr_t)(page - 1));
    const size_t span = (size_t)((unsigned char *)target + length - (unsigned char *)first);
    if (mprotect(first, span, PROT_READ | PROT_WRITE | PROT_EXEC)) return 0;
    memcpy(target, data, length);
    __builtin___clear_cache(target, (char *)target + length);
    return mprotect(first, span, PROT_READ | PROT_EXEC) == 0;
#endif
}

static void jump_to(unsigned char *code, const void *target) {
    memcpy(code, "\xff\x25\0\0\0\0", 6);
    memcpy(code + 6, &target, sizeof target);
}

/* Initialize before the native timer creates its UI and replication state. */
static void ABI countdown_init(unsigned char *self) {
    int mode = 0, teams = 1;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    unsigned char *config = network && *network ? pointer_at(*network, 0x40) : NULL;
    if (config && mode == 1 && !runtime_online_mode_private_cup() && is_private() &&
        (teams == 2 || teams == 4)) {
        float saved[3], extended[3] = {30.f, 30.f, 30.f};
        /* +114 prepares network racers/customization: it must run before early Ready. */
        memcpy(saved, config + 0x110, sizeof saved);
        memcpy(config + 0x110, extended, sizeof extended);
        original_countdown_init(self);
        memcpy(config + 0x110, saved, sizeof saved);
        float remaining;
        memcpy(&remaining, self + 0x28, 4);
        printf("Native private TEAM: initialized selector timer=%.1f (native UI/replication)\n", remaining);
    } else original_countdown_init(self);
}

static void ABI lobby_countdown(unsigned char *self) {
    int mode = 0, teams = 0;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    float remaining, threshold;
    memcpy(&remaining, self + 0x28, 4);
    memcpy(&threshold, self + 0x2c, 4);
    if (mode == 1 && !runtime_online_mode_private_cup() && (teams == 2 || teams == 4) &&
        is_private() && remaining > 0 && threshold >= 0)
        memcpy(self + 0x2c, &remaining, 4);
    /* Never rewrite replicated remaining time or the native all-ready shortcut. */
    original_lobby_countdown(self);
}

typedef void (ABI *DisableControlsNode)(void *, unsigned char *);
static DisableControlsNode original_disable_controls_node;
static void ABI team_selector_disable_node(void *context, unsigned char *pins) {
    int count = 0;
    if (pins) memcpy(&count, pins, 4);
    if (count == 3 && private_team_selector_active && private_team_selector_allowed()) {
        puts("Native private TEAM: preserve selector controls after lobby open callback");
        return;
    }
    original_disable_controls_node(context, pins);
}

static void ABI ranking_network_branch(void *context, unsigned char *pins, unsigned char *outputs) {
    int count = 0;
    if (pins) memcpy(&count, pins, 4);
    const int manual = count == 1 && team_ranking_project && private_single_race_active;
    void **nodes = pointer_at(outputs, 0x20);
    run_graph_node(context, nodes[manual ? 1 : !in_network_game()]);
}

static int ABI team_countdown_mode(void) {
    int mode = 0;
    if (race_manager && *race_manager)
        memcpy(&mode, (unsigned char *)*race_manager + 0xd40, sizeof mode);
    if (!is_private()) clear_team_selector();
    normalize_race_teams();
    private_team_selector_active = is_private() && private_team_selector_allowed();
    if (private_team_selector_active) {
        return 9;
    }
    return mode;
}

static void ABI team_initial_ready(void *table, void *player, unsigned char *ready, unsigned hash) {
    int mode = 0;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    /* Online defaults to ready, which disables every native move request. */
    unsigned char value = private_team_selector_allowed() ? 0 : *ready;
    original_ready_insert(table, player, &value, hash);
    if (mode == 1 && private_team_selector_active)
        printf("Native private TEAM: initial player ready=%u -> %u\n", *ready, value);
}

/* A network Ready/Unready event is normally broadcast to the current GUI
 * layer. Private Single opens its selector from the lobby callback; deliver to
 * that still-live project if the native layer traversal did not reach it. */
static void ABI team_ready_broadcast(void *layer, void *event) {
    void *saved_event = ready_delivery_event;
    int saved_seen = ready_delivery_seen;
    void *kind = ((GetType)pointer_at(pointer_at(event, 0), 0))(event);
    const int ready_event = private_team_selector_allowed() && private_team_selector_active && active_team_selector_project &&
        (kind == *team_confirm_type || kind == *team_unconfirm_type);
    if (ready_event) { ready_delivery_event = event; ready_delivery_seen = 0; }
    original_gui_broadcast(layer, event);
    if (ready_event && !ready_delivery_seen) {
        unsigned char *project = handle_object(&active_team_selector_project);
        if (project) {
            original_project_event(project, event);
            printf("Native private TEAM: rerouted network %s to selector delivered=%d\n",
                   kind == *team_confirm_type ? "ready" : "unready", ready_delivery_seen);
        }
    }
    ready_delivery_event = saved_event; ready_delivery_seen = saved_seen;
}

/* Preserve the authored two-line label area when native binding leaves our
 * previous position intact. Recompute its center from current sizes/pivots. */
struct TeamNameLayout { void *slot, *name; float original_y, applied_y; };
static struct TeamNameLayout team_name_layouts[8];

/* Native PLAYER n is a local controller index, not a shared lobby identity. */

/* ---- Nicknames with a color of their own (runtime_name_colors.c) ----------------------------
 * Wherever these behaviors draw a player's name: the lobby's player slots, the race leaders list,
 * the tag over each kart and the results list. Never in Battle or a team race, where the names
 * carry their team's color. The whole name changes at once: the color is a tint of its element,
 * not of a letter.
 * Offsets from the game's own field metadata (src/runtime_meta_dump.c, USA 1.21): igGuiPlaceable
 * _displayText +0x30 (its text), _color +0xc0 (RGBA floats, a tint over the whole element). A
 * placeable a list reuses for another player gets its own color back. */
typedef struct { unsigned char *placeable; float color[4]; } ColoredName;
static ColoredName colored_names[32];
static BehaviorUpdate original_marker_update, original_results_item_update;

static int name_colors_allowed(void) {
    if (battle_selected() || private_team_race()) return 0;
    int teams = 1;
    if (race_settings && *race_settings) memcpy(&teams, *race_settings + 0x28, 4);
    return !(runtime_online_mode_in_private() && teams >= 2 && teams <= 4 && !runtime_online_mode_private_cup());
}

/* The game draws a placeable with the color it read last: its own setter (252c10) raises this bit. */
static void name_color_changed(unsigned char *placeable) {
    unsigned flags;
    memcpy(&flags, placeable + 12, 4);
    flags |= 0x1000;
    memcpy(placeable + 12, &flags, 4);
}

static void colored_name(unsigned char *placeable, int allowed) {
    if (!placeable) return;
    const char *text = (const char *)pointer_at(placeable, 0x30);
    float rgb[3];
    const int colored = allowed && text && runtime_name_color(text, rgb);
    ColoredName *entry = NULL, *free_entry = NULL;
    for (unsigned i = 0; i < sizeof colored_names / sizeof *colored_names; ++i) {
        if (colored_names[i].placeable == placeable) entry = colored_names + i;
        else if (!colored_names[i].placeable && !free_entry) free_entry = colored_names + i;
    }
    float *color = (float *)(placeable + 0xc0);
    if (colored) {
        if (!entry) {
            if (!free_entry) return;
            entry = free_entry;
            entry->placeable = placeable;
            memcpy(entry->color, color, sizeof entry->color);
        }
        static int told;
        if (!told) { told = 1; puts("Native Online: a nickname drawn in its color"); }
        if (memcmp(color, rgb, sizeof rgb)) { /* alpha kept: fades still work */
            memcpy(color, rgb, sizeof rgb);
            name_color_changed(placeable);
        }
    } else if (entry) {
        memcpy(color, entry->color, 3 * sizeof(float));
        name_color_changed(placeable);
        entry->placeable = NULL;
    }
}

/* Lists a script fills (the lobby's members, the pause menu's racers) have no behavior of
 * their own: through every placeable of a GUI project (_children +0x48: an igObjectList, count
 * +0x0c, data +0x20; a nested composition behind the handle at +0x38, its root at +0x20), the
 * texts that are such a nickname get its color (name_color_project below). */
static unsigned name_color_visited;
static int name_color_debug; /* BB_NAME_COLOR_DEBUG=1: the walked trees, logged */
static int name_color_texts; /* BB_NAME_COLOR_DEBUG=1: every text the walks meet, once */
static void name_color_log_text(const unsigned char *placeable, const char *text, int depth) {
    static char seen[512][48];
    static unsigned seen_count;
    if (!text || !*text || seen_count >= 512) return;
    for (unsigned i = 0; i < seen_count; ++i)
        if (!strncmp(seen[i], text, sizeof seen[i] - 1)) return;
    snprintf(seen[seen_count++], sizeof seen[0], "%s", text);
    const char *name = pointer_at(placeable, 0x28);
    const float *c = (const float *)(placeable + 0xc0);
    printf("Name color text: depth %d %s \"%.60s\" color %.2f %.2f %.2f\n", depth, name ? name : "-", text,
           c[0], c[1], c[2]);
}
static void name_color_tree(unsigned char *placeable, int depth, int allowed) {
    if (!placeable || depth > 32 || name_color_visited > 4096) return;
    ++name_color_visited;
    colored_name(placeable, allowed);
    /* a placeable hosting another composition (a nested menu: the members list, a row) */
    unsigned char *composition = handle_object((void **)(placeable + 0x38));
    if (name_color_texts) name_color_log_text(placeable, pointer_at(placeable, 0x30), depth);
    if (name_color_debug) {
        const char *name = pointer_at(placeable, 0x28), *text = pointer_at(placeable, 0x30);
        const float *c = (const float *)(placeable + 0xc0);
        printf("Name color tree: %*s%s \"%.40s\" color %.2f %.2f %.2f %.2f%s\n", depth * 2, "",
               name ? name : "-", text ? text : "", c[0], c[1], c[2], c[3],
               composition && pointer_at(composition, 0) == composition_vtable ? " [composition]" : "");
    }
    if (composition && pointer_at(composition, 0) == composition_vtable)
        name_color_tree(pointer_at(composition, 0x20), depth + 1, allowed);
    unsigned char *list = pointer_at(placeable, 0x48);
    if (!list) return;
    uint32_t count;
    memcpy(&count, list + 0x0c, 4);
    unsigned char **items = (unsigned char **)pointer_at(list, 0x20);
    if (!items || count > 512) return;
    for (uint32_t i = 0; i < count; ++i) name_color_tree(items[i], depth + 1, allowed);
}

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

/* Every GUI project (an igGuiProject: +0x10 its root placeable, whose +0x38 holds the menu's
 * composition) passes its events through cup_project_event: the lobby's members, the pause
 * menu's racers, the results. Each project's tree is walked at most every 150 ms. */
static void name_color_project(unsigned char *project) {
    unsigned char *root = project ? pointer_at(project, 0x10) : NULL;
    if (!root) return;
    static struct { unsigned char *root; uint64_t at; } roots[32];
    static unsigned next_root;
    const uint64_t now = monotonic_ms();
    unsigned i = 0;
    while (i < 32 && roots[i].root != root) ++i;
    if (i == 32) {
        i = next_root++ % 32;
        roots[i].root = root;
        roots[i].at = 0;
    }
    if (roots[i].at && now - roots[i].at < 150) return;
    const int first = !roots[i].at;
    roots[i].at = now ? now : 1;
    name_color_visited = 0;
    static int debug_walks;
    const char *debug = getenv("BB_NAME_COLOR_DEBUG");
    name_color_texts = debug && debug[0] == '1';
    name_color_debug = first && name_color_texts && debug_walks++ < 60;
    if (name_color_debug) printf("Name color tree: project %p\n", (void *)project);
    name_color_tree(root, 0, name_colors_allowed());
    name_color_debug = 0;
}

/* The lobby's members list and the pause menu's racers (ui/Octane_LobbyMembers): an igGuiList
 * whose rows (igGuiListItemList, not _children) each carry a CGuiBehaviorOctaneLobbyMemberItem
 * (vtable 0x2774cc0, USA 1.21). Its event handler (+0xc8) writes the player's name into the
 * placeable at +0x10 (_playerName) and plays the LocalPlayer / OtherMember animation that colors
 * it; its update (+0xd8) is the base one, called every frame. Both put the nickname's back on. */
static EventHandler original_member_event;
static BehaviorUpdate original_member_update;

static void ABI member_event(unsigned char *self, void *event, void *owner) {
    original_member_event(self, event, owner);
    colored_name(pointer_at(self, 0x10), name_colors_allowed());
}

static void ABI member_update(unsigned char *self, unsigned char *owner, float dt) {
    original_member_update(self, owner, dt);
    colored_name(pointer_at(self, 0x10), name_colors_allowed());
}

static void ABI marker_update(unsigned char *self, unsigned char *owner, float dt) {
    original_marker_update(self, owner, dt);
    colored_name(pointer_at(self, 0x50), name_colors_allowed() && !self[0x88]); /* _playerTagPlaceable, _isBattle */
}

static void ABI results_item_update(unsigned char *self, unsigned char *owner, float dt) {
    original_results_item_update(self, owner, dt);
    if (team_ranking_project) {
        if (team_results_pending) refresh_team_results();
        team_ranking_row(self);
    }
    colored_name(pointer_at(self, 0xa8), name_colors_allowed() && !self[0xd8]); /* _namePlaceable, _isTeamBattle */
}

static void ABI team_slot_update(unsigned char *self, unsigned char *owner, float dt) {
    if (runtime_private_party_update()) return;
    if (resume_private_map()) return; /* Loading may invalidate the old UI objects. */
    original_team_slot_update(self, owner, dt);
    colored_name(pointer_at(self, 0x10), name_colors_allowed()); /* _namePlaceable */
    refresh_lobby_members_visibility();
    if (!private_team_selector_allowed()) return;
    unsigned char *name = pointer_at(self, 0x10);
    unsigned char *number = pointer_at(self, 0x18);
    if (!number) return;
    hide_placeable(number, 1);
    if (!name) return;
    float y, number_y, height, number_height, pivot, number_pivot;
    memcpy(&y, name + 0x64, 4); memcpy(&number_y, number + 0x64, 4);
    memcpy(&height, name + 0x94, 4); memcpy(&number_height, number + 0x94, 4);
    memcpy(&pivot, name + 0x70, 4); memcpy(&number_pivot, number + 0x70, 4);
    if (!(height > 0 && number_height > 0)) return;
    struct TeamNameLayout *layout = NULL;
    for (unsigned i = 0; i < 8; ++i)
        if (team_name_layouts[i].slot == self) { layout = team_name_layouts + i; break; }
    if (!layout) {
        static unsigned next;
        layout = team_name_layouts + next++ % 8;
        *layout = (struct TeamNameLayout){self, name, y, y};
    }
    if (layout->name != name || y != layout->applied_y)
        *layout = (struct TeamNameLayout){self, name, y, y};
    const float name_top = layout->original_y - height * pivot;
    const float number_top = number_y - number_height * number_pivot;
    const float top = name_top < number_top ? name_top : number_top;
    const float bottom = name_top + height > number_top + number_height ?
                         name_top + height : number_top + number_height;
    const float centered = (top + bottom - height) * .5f + height * pivot;
    layout->applied_y = centered;
    if (y != centered) {
        memcpy(name + 0x64, &centered, 4);
        unsigned flags;
        memcpy(&flags, name + 12, 4); flags |= 0x1000;
        memcpy(name + 12, &flags, 4);
    }
    /* The native ready tick is the name's sole child, in text-local coordinates. */
    unsigned char *children = pointer_at(name, 0x48);
    int count = 0;
    if (children) memcpy(&count, children + 12, 4);
    unsigned char *tick = count == 1 ? pointer_at(pointer_at(children, 0x20), 0) : NULL;
    if (tick) {
        float tick_height, tick_scale, tick_pivot;
        memcpy(&tick_height, tick + 0x94, 4);
        memcpy(&tick_scale, tick + 0x88, 4);
        memcpy(&tick_pivot, tick + 0x70, 4);
        const float tick_y = height * .5f + tick_height * tick_scale * (tick_pivot - .5f);
        memcpy(tick + 0x64, &tick_y, 4);
        unsigned flags;
        memcpy(&flags, tick + 12, 4); flags |= 0x1000;
        memcpy(tick + 12, &flags, 4);
    }
}

static int selector_player_ready(unsigned char *self, unsigned char *player) {
    if (!player) return -1;
    void *key = NULL;
    make_handle(&key, player);
    if (!key) return -1;
    uint32_t low, high;
    memcpy(&low, (unsigned char *)key + 0x10, 4);
    memcpy(&high, (unsigned char *)key + 0x20, 4);
    uint64_t hash = (uint64_t)high << 32 | low;
    hash = hash * 0x40000 + ~hash;
    hash = (hash >> 31 ^ hash) * 21;
    hash = (hash >> 11 ^ hash) * 65;
    unsigned char *ready = NULL;
    const int found = ready_find(pointer_at(self, 0x60), &key,
                                (unsigned)(hash >> 22) ^ (unsigned)hash, &ready);
    assign_handle((unsigned char *)&key, NULL);
    return found && ready ? !!*ready : -1;
}

static void refresh_team_legend(unsigned char *self) {
    unsigned char *project = handle_object(&active_team_selector_project);
    void *list = player_manager && *player_manager ? pointer_at(*player_manager, 0x10) : NULL;
    unsigned char *player = list ? find_player(list, controller_player(team_legend_controller)) : NULL;
    const int ready = selector_player_ready(self, player);
    unsigned char *remove = gui_manager && *gui_manager ? pointer_at(*gui_manager, 0xf0) : NULL;
    if (!project || !remove || ready < 0 || ready == team_legend_state) return;
    ProjectEvent dispatch = (ProjectEvent)pointer_at(pointer_at(project, 0), 0xb0);
    team_legend_state = ready; /* Native legend events may reenter this behavior. */
    for (int button = 1; button <= 2; ++button) {
        memcpy(remove + 0x18, &button, 4);
        dispatch(project, remove);
    }
    char *label = intern_string(*string_pool, ready ? "UNREADY" : "READY");
    add_legend(project, ready ? 2 : 1, &label, 1);
    if (label) drop_string(*string_pool, label - 16);
}

static void ABI team_selector_event(unsigned char *self, void *event, void *owner) {
    GetType type = (GetType)pointer_at(pointer_at(event, 0), 0);
    void *kind = type(event);
    if (event == ready_delivery_event) ready_delivery_seen = 1;
    const int input = kind == *gui_input_type;
    int mode = 0;
    if (race_manager && *race_manager) memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    const int team_race = private_team_selector_allowed();
    if (!team_race && (runtime_public_match_active() || !(mode >= 9 && mode <= 13)) && kind != *gui_uninitialize_type) {
        clear_team_selector();
        return;
    }
    if (input && team_race) {
        int signal, controller;
        memcpy(&signal, (unsigned char *)event + 0x18, 4);
        memcpy(&controller, (unsigned char *)event + 0x1c, 4);
        const int id = controller_player(controller);
        void *list = player_manager && *player_manager ? pointer_at(*player_manager, 0x10) : NULL;
        unsigned char *player = list ? find_player(list, id) : NULL;
        const int ready = selector_player_ready(self, player), found = ready >= 0;
        int team = -1, teams = 0, capacity = 0;
        if (player) memcpy(&team, player + 0xe4, 4);
        if (controller > 0) team_legend_controller = controller;
        memcpy(&teams, self + 0x7c, 4);
        memcpy(&capacity, self + 0x80, 4);
        printf("Native private TEAM input: signal=%d controller=%d player=%d found=%d ready=%d team=%d teams=%d capacity=%d\n",
               signal, controller, id, found, ready, team, teams, capacity);
        if (signal == 2 || signal == 3 || signal == 6 || signal == 7) {
            if (!player || ready != 0 || teams < 2 || teams > 4 ||
                team < 0 || team >= teams || capacity < 1 || capacity > 8) return;
            int count = 0, occupancy[4] = {0}, available = 0;
            if (list) memcpy(&count, (unsigned char *)list + 12, 4);
            void **players = list ? pointer_at(list, 0x20) : NULL;
            if (!players || count < 1 || count > 8) return;
            for (int i = 0; i < count; ++i) {
                int assigned;
                if (!players[i]) return;
                memcpy(&assigned, (unsigned char *)players[i] + 0xe4, 4);
                if (assigned < 0 || assigned >= teams) return;
                ++occupancy[assigned];
            }
            for (int i = 0; i < teams; ++i)
                if (i != team && occupancy[i] < capacity) available = 1;
            /* Native search tests the unwrapped index and can loop forever when full. */
            if (!available) return;
        }
        /* Native Cancel exits when not ready. Private match selection is locked
         * to this screen: only the native ready -> unready transition is legal. */
        if (signal == 9 && ready <= 0) { refresh_team_legend(self); return; }
    }
    original_team_selector_event(self, event, owner);
    if (kind == *gui_uninitialize_type) {
        team_legend_state = -1; team_legend_controller = 1;
        private_team_selector_active = 0;
        assign_handle((unsigned char *)&active_team_selector_project, NULL);
    }
    if (kind == *initialize_type && team_race) {
        team_legend_state = -1; team_legend_controller = 1;
        private_team_selector_active = 1;
        unsigned char *project = owner ? pointer_at(owner, 0x20) : NULL;
        int disabled_depth = 0;
        if (project) {
            assign_handle((unsigned char *)&active_team_selector_project, NULL);
            make_handle(&active_team_selector_project, project);
            /* Native disableControls increments project +48; enableControls
             * decrements only once. A freshly initialized selector must not
             * inherit nested lobby/transition disables. Do this only once,
             * leaving subsequent modal/closing disables owned by the game. */
            memcpy(&disabled_depth, project + 0x48, 4);
            const int one = 1;
            memcpy(project + 0x48, &one, 4);
            enable_project_controls(project);
            /* Project input dispatch also requires an active input bit and a
             * controller mask; enableControls only clears the disable counter. */
            unsigned mask;
            memcpy(&mask, project + 0x44, 4);
            if (!mask) {
                mask = 0xff;
                memcpy(project + 0x44, &mask, 4);
            }
            if (!(project[0x34] & 0x20)) activate_project_input(project);
        }
        int players = 0, teams = 0, capacity = 0;
        memcpy(&players, self + 0x78, 4);
        memcpy(&teams, self + 0x7c, 4);
        memcpy(&capacity, self + 0x80, 4);
        printf("Native private TEAM: selector initialized players=%d teams=%d capacity=%d disabled_depth=%d controls=%s\n",
               players, teams, capacity, disabled_depth, project ? "enabled" : "missing project");
        if (project) {
            unsigned mask; memcpy(&mask, project + 0x44, 4);
            printf("Native private TEAM: input flags=%02x controller_mask=%02x\n", project[0x34], mask);
        }
    }
    if (team_race && private_team_selector_active) refresh_team_legend(self);
    refresh_lobby_members_visibility();
}

static size_t team_countdown_stub(unsigned char *code, unsigned char *image, size_t shift) {
    /* This native block starts with an aligned stack. Preserve the caller's
     * registers and floating-point state across the private-lobby query. */
    const unsigned char push[] = {0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53};
    const unsigned char pop[] = {0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f};
    size_t at = 0;
    memcpy(code + at, push, sizeof push); at += sizeof push;
    memcpy(code + at, "\x48\x81\xec\x00\x02\x00\x00\x48\x0f\xae\x04\x24\x48\xb8", 14); at += 14;
    uintptr_t fn = (uintptr_t)team_countdown_mode;
    memcpy(code + at, &fn, 8); at += 8;
    memcpy(code + at, "\xff\xd0\x48\x0f\xae\x0c\x24\x48\x81\xc4\x00\x02\x00\x00", 14); at += 14;
    memcpy(code + at, pop, sizeof pop); at += sizeof pop;
    memcpy(code + at, "\x83\xc0\xf7\x83\xf8\x04\x77\x0e", 8); at += 8;
    jump_to(code + at, image + (0xeb79e8 - shift)); at += 14;
    jump_to(code + at, image + (0xeb7996 - shift)); at += 14;
    return at;
}

/* Preserve the native sender's already prepared network arguments. */
static size_t settings_sender_stub(unsigned char *code, int joining, void *resume) {
    const unsigned char push[] = {0x50,0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x50};
    const unsigned char pop[] = {0x58,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f,0x58};
    size_t at = 0;
    memcpy(code + at, push, sizeof push); at += sizeof push;
    memcpy(code + at, joining ? "\x4c\x89\xf7" : "\x4c\x89\xe7", 3); at += 3;
    memcpy(code + at, "\x48\xb8", 2); at += 2;
    const uintptr_t fn = (uintptr_t)private_settings_sending;
    memcpy(code + at, &fn, 8); at += 8;
    memcpy(code + at, "\xff\xd0", 2); at += 2;
    memcpy(code + at, pop, sizeof pop); at += sizeof pop;
    if (!joining) { memcpy(code + at, "\x4c\x89\xe7", 3); at += 3; }
    jump_to(code + at, resume);
    return at + 14;
}

/* Replace the podium-only branch in the online result consumer. R13 is no
 * longer used as the race position here; the loop reloads it before advancing. */
static size_t podium_slot_stub(unsigned char *code, unsigned char *image) {
    const unsigned char push[] = {0x50,0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x50};
    const unsigned char pop[] = {0x58,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f,0x58};
    size_t at = 0;
    memcpy(code + at, push, sizeof push); at += sizeof push;
    memcpy(code + at, "\x4c\x89\xf7\x44\x89\xee\x48\xb8", 8); at += 8;
    const uintptr_t fn = (uintptr_t)podium_position;
    memcpy(code + at, &fn, 8); at += 8;
    memcpy(code + at, "\xff\xd0\x41\x89\xc5", 5); at += 5;
    memcpy(code + at, pop, sizeof pop); at += sizeof pop;
    /* Null podium data or rank > 3: resume native loop. */
    memcpy(code + at, "\x48\x83\x7d\xb0\x00\x74\x14\x41\x83\xfd\x03\x7f\x0e", 13); at += 13;
    jump_to(code + at, image + 0xa29ac0); at += 14;
    jump_to(code + at, image + 0xa29a9f); at += 14;
    return at;
}

int runtime_online_mode_start(unsigned char *image, size_t size) {
    const char *setting = getenv("BB_ONLINE_MODE_HOOK");
    if (!setting || !*setting) return 1;
    /* USA/EU 1.21 share globals and layouts; these EU functions start 16 bytes earlier. */
    size_t shift;
    if (!strcmp(setting, "1277e00")) shift = 0;
    else if (!strcmp(setting, "1277df0")) shift = 0x10;
    else return 0;
    if (size < 0x2af1fb0) return 0;
    const unsigned char start_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x18};
    if (memcmp(image + (0x12108f0 - shift), start_prefix, sizeof start_prefix)) return 0;
    unsigned char start_guard[] = {0x4c,0x8b,0x3d,0x14,0x8b,0x94,0x01};
    start_guard[3] += (unsigned char)shift;
    if (memcmp(image + (0xebe570 - shift), start_prefix, sizeof start_prefix) ||
        memcmp(image + (0xebe57d - shift), start_guard, sizeof start_guard)) return 0;
    const unsigned char timer_init_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58};
    const unsigned char sound_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38};
    unsigned char confetti_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x53,0x50,0x8a,0x05,0x68,0xa5,0xb5,0x01};
    confetti_prefix[12] += (unsigned char)shift;
    if (memcmp(image + (0xeb71d0 - shift), timer_init_prefix, sizeof timer_init_prefix) ||
        memcmp(image + (0x1093ca0 - shift), sound_prefix, sizeof sound_prefix) ||
        memcmp(image + (0xf382f0 - shift), confetti_prefix, sizeof confetti_prefix)) return 0;
    const unsigned char countdown_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x53,0x50,0x48,0x89,0xfb,0x48,0x8b,0x43,0x18};
    const unsigned char legend_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    const unsigned char disable_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x54,0x53,0x48,0x83,0xec,0x10};
    if (memcmp(image + (0x110b9c0 - shift), disable_prefix, sizeof disable_prefix) ||
        memcmp(image + (0x110bbc0 - shift), legend_prefix, sizeof legend_prefix)) return 0;
    unsigned char network_branch[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,
        0x48,0x89,0xd3,0x49,0x89,0xfe,0xe8,0xbe,0x63,0x93,0xff};
    network_branch[14] += (unsigned char)shift;
    if (memcmp(image + (0xeb7950 - shift), countdown_prefix, sizeof countdown_prefix) ||
        memcmp(image + (0x115a740 - shift), network_branch, sizeof network_branch)) return 0;
    if (pointer_at(image, 0x2778150) != image + (0x1293030 - shift)) return 0;
    if (pointer_at(image, 0x2544140) != image + 0x9c60b0) return 0;
    unsigned char team_gate[] = {0x48,0x8b,0x05,0x6e,0x64,0xc1,0x01,
        0x8b,0x80,0x40,0x0d,0,0,0x83,0xc0,0xf7,0x83,0xf8,0x04,0x77,0xae};
    team_gate[3] += (unsigned char)shift;
    if (memcmp(image + (0xeb79d3 - shift), team_gate, sizeof team_gate)) return 0;
    const unsigned char setup_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    if (memcmp(image + (0xe9de80 - shift), setup_prefix, sizeof setup_prefix)) return 0;
    unsigned char ready_site[] = {0xe8,0xfc,0x4c,0xae,0,0x48,0x8b,0x8d,0x68,0xff,0xff,0xff,
                                 0x4c,0x8b,0xbd,0x40,0xff,0xff,0xff};
    ready_site[1] -= (unsigned char)shift;
    if (memcmp(image + 0x9c153f, ready_site, sizeof ready_site)) return 0;
    if (memcmp(image + 0xa09190, "\x55\x48\x89\xe5\x53\x50\x48\x89\xfb\x48\x85\xdb", 12)) return 0;
    if (memcmp(image + 0x24ac80, "\x8a\x47\x34\x89\xc1\x80\xc9\x20", 8)) return 0;
    if (memcmp(image + 0x253970, "\x8b\x47\x48\x31\xd2\x8d\x48\xff\x83\xf8\x01\x0f\x4f\xd1\x89\x57\x48\xc3", 18) ||
        memcmp(image + 0x253990, "\xff\x47\x48\xc3", 4)) return 0;
    const unsigned char broadcast_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    if (memcmp(image + 0x248d10, broadcast_prefix, sizeof broadcast_prefix)) return 0;
    unsigned char open_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48};
    if (memcmp(image + 0x986820, open_prefix, sizeof open_prefix)) return 0;
    unsigned char prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x53,0x50,0x48,0x8d,0x05,0x97,0xa1,0x87,0x01};
    static const unsigned char season_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x54,0x53,0x48,0x83,0xec,0x20};
    prefix[13] += (unsigned char)shift;
    unsigned char open_pin[] = {0x49,0x8b,0x46,0x10,0x48,0x8b,0x30,0xe8,
        0x63,0x76,0x36,0xff,0x48,0xc7,0x45,0xc0,0,0,0,0};
    open_pin[8] += (unsigned char)shift;
    if (memcmp(image + (0x1277e00 - shift), prefix, sizeof prefix) ||
        memcmp(image + (0x1109231 - shift), open_pin, sizeof open_pin) ||
        memcmp(image + (0xfdeb70 - shift), "\x55\x48\x89\xe5\x41\x56\x53\x48\x89\xfb\x83\xfe\x06", 13) ||
        memcmp(image + (0xfb7710 - shift), season_prefix, sizeof season_prefix) ||
        memcmp(image + (0x125d570 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56", 8) ||
        memcmp(image + (0xf50350 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56", 8)) return 0;
    const unsigned char receive_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    unsigned char join_mode[] = {0x48,0x8b,0x05,0xf0,0xf9,0xc0,0x01,0x8b,0x80,0x40,0x0d,0,0,0x41,0x89,0x46,0x3c};
    int32_t join_displacement = 0x1c0f9f0 + (int32_t)shift;
    memcpy(join_mode + 3, &join_displacement, 4);
    const unsigned char changed_mode[] = {0x4c,0x89,0xe7,0x8b,0x80,0x40,0x0d,0,0,0x41,0x89,0x44,0x24,0x3c};
    const unsigned char lobby_dispatch[] = {0x49,0x8b,0x06,0x4c,0x89,0xf7,0x4c,0x89,0xfe,0xff,0x90,0xb0,0,0,0};
    if (memcmp(image + (0xeb9b60 - shift), receive_prefix, sizeof receive_prefix) ||
        memcmp(image + (0xebe451 - shift), join_mode, sizeof join_mode) ||
        memcmp(image + (0xf4f972 - shift), changed_mode, sizeof changed_mode) ||
        memcmp(image + (0xeb9ffb - shift), lobby_dispatch, sizeof lobby_dispatch)) return 0;
    const unsigned char damage_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x48};
    if (memcmp(image + 0xe330e0, damage_prefix, sizeof damage_prefix)) return 0;
    unsigned char clock_prefix[sizeof damage_prefix];
    memcpy(clock_prefix, damage_prefix, sizeof clock_prefix);
    clock_prefix[16] = 0x38;
    if (memcmp(image + (0xf1fb50 - shift), clock_prefix, sizeof clock_prefix)) return 0;
    const unsigned char load_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x81,0xec,0x30,0x01,0,0};
    unsigned char timer_prefix[] = {0x55,0x48,0x89,0xe5,0xe8,0x87,0x2c,0xaa,0xff,
        0x8b,0xb8,0x44,0x01,0,0,0xe8,0x0c,0xde,0xb4,0xff,0x84,0xc0,0x74,0x06,
        0x5d,0xe9,0xb2,0x08,0xbf,0xff,0x5d,0xc3};
    int32_t timer_call = (int32_t)(0x94efa0 - (0xeac319 - shift));
    memcpy(timer_prefix + 5, &timer_call, 4);
    timer_call = (int32_t)(0x9fa130 - (0xeac324 - shift));
    memcpy(timer_prefix + 16, &timer_call, 4);
    timer_call = (int32_t)(0xa9cbe0 - (0xeac32e - shift));
    memcpy(timer_prefix + 26, &timer_call, 4);
    if (memcmp(image + 0x9b1fc0, load_prefix, sizeof load_prefix) ||
        memcmp(image + (0xeac310 - shift), timer_prefix, sizeof timer_prefix)) return 0;
    if (memcmp(image + (0xf344e6 - shift), "\x44\x39\xf2\x74\x54", 5)) return 0;
    const unsigned char lap_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x54,0x53,0x48,0x8b,0x5f,0x28,0x41,0x89,0xd6};
    const unsigned char complete_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x54,0x53,0x48,0x89,0xf3};
    if (memcmp(image + (0xf350c0 - shift), lap_prefix, sizeof lap_prefix) ||
        memcmp(image + (0xf351b0 - shift), complete_prefix, sizeof complete_prefix)) return 0;
    unsigned char guest_timer_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,
        0x8a,0x05,0,0,0,0,0x49,0x89,0xfe};
    int32_t guest_global = (int32_t)(0x2a92868 - (0xf3963d - shift));
    memcpy(guest_timer_prefix + 9, &guest_global, 4);
    if (memcmp(image + (0xf39630 - shift), guest_timer_prefix, sizeof guest_timer_prefix)) return 0;
    unsigned char hud_gate[] = {0x41,0x83,0xfd,0x01,0x0f,0x85,0xe9,0x19,0,0,
        0x48,0x8b,0x35,0x54,0xa6,0xc5,0x01};
    int32_t hud_global = 0x1c5a654;
    memcpy(hud_gate + 13, &hud_global, 4);
    if (memcmp(image + (0xe6675b), hud_gate, sizeof hud_gate)) return 0;
    if (memcmp(image + (0xf34573 - shift), "\x45\x85\xed\x7e\xc7", 5) ||
        memcmp(image + (0x125da80 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56", 8)) return 0;
    const unsigned char participant_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xfe};
    unsigned char finished_prefix[] = {0x48,0x8b,0x05,0x39,0x07,0xba,0x01,0x4c,0x8b,0x40,0x10,0x4d,0x85,0xc0};
    int32_t racer_displacement = (int32_t)(0x2ad4b70 - (0xf34437 - shift));
    memcpy(finished_prefix + 3, &racer_displacement, 4);
    const unsigned char finish_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xfc};
    const unsigned char order_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50};
    if (memcmp(image + (0xff11a0 - shift), order_prefix, sizeof order_prefix)) return 0;
    if (memcmp(image + (0xfff160 - shift), participant_prefix, sizeof participant_prefix) ||
        memcmp(image + (0xf34430 - shift), finished_prefix, sizeof finished_prefix) ||
        memcmp(image + (0xff2320 - shift), finish_prefix, sizeof finish_prefix)) return 0;
    if (memcmp(image + 0xe6f830, receive_prefix, sizeof receive_prefix)) return 0;
    unsigned char place_prefix[sizeof receive_prefix];
    memcpy(place_prefix, receive_prefix, sizeof place_prefix);
    place_prefix[16] = 0x58;
    if (memcmp(image + (0xf31160 - shift), place_prefix, sizeof place_prefix)) return 0;
    unsigned char ui_prefix[sizeof receive_prefix];
    memcpy(ui_prefix, receive_prefix, sizeof ui_prefix);
    ui_prefix[16] = 0x48;
    if (memcmp(image + (0xe9ba30 - shift), ui_prefix, sizeof ui_prefix)) return 0;
    const unsigned char spawn_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0,0,0};
    if (memcmp(image + (0xfff740 - shift), spawn_prefix, sizeof spawn_prefix) ||
        memcmp(image + (0xfff9c2 - shift), native_ai_reuse, sizeof native_ai_reuse) ||
        memcmp(image + (0xffd9c7 - shift), native_ai_asset_reuse, sizeof native_ai_asset_reuse) ||
        memcmp(image + (0xe73820 - shift), receive_prefix, sizeof receive_prefix) ||
        memcmp(image + (0xe73460 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x50", 10)) return 0;
    if (memcmp(image + 0xa29a92, "\x48\x83\x7d\xb0\x00\x74\x06\x41\x83\xfd\x03\x7e\x21\x4c\x8b\x6d\x90", 17) ||
        memcmp(image + (0xeac6c0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56", 8)) return 0;
    if (memcmp(image + 0xa2a386, "\x0f\x85\x0d\xf7\xff\xff", 6) ||
        memcmp(image + 0xa2a73a, "\x0f\x85\x59\xf3\xff\xff", 6)) return 0;
    if (memcmp(image + (0xf9c610 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56", 8)) return 0;
    uintptr_t handler;
    memcpy(&handler, image + 0x2543d68, sizeof handler);
    if (handler != (uintptr_t)(image + 0x9c1250)) return 0;
    memcpy(&handler, image + 0x2545940, sizeof handler);
    if (handler != (uintptr_t)(image + 0x9d67e0)) return 0;
    memcpy(&handler, image + 0x2546008, sizeof handler);
    if (handler != (uintptr_t)(image + 0x9de1b0)) return 0;
    memcpy(&handler, image + 0x2546018, sizeof handler);
    if (handler != (uintptr_t)(image + 0x9dedb0)) return 0;
    memcpy(&handler, image + 0x25436c8, sizeof handler);
    if (handler != (uintptr_t)(image + 0x9b73f0)) return 0;
    memcpy(&handler, image + 0x2432338, sizeof handler);
    if (handler != (uintptr_t)(image + 0x253740)) return 0;
    memcpy(&handler, image + 0x254c430, sizeof handler);
    if (handler != (uintptr_t)(image + 0xa298c0)) return 0;
    memcpy(&handler, image + 0x2773b40, sizeof handler);
    if (handler != (uintptr_t)(image + (0x1277c70 - shift))) return 0;
    memcpy(&handler, image + 0x2773b50, sizeof handler);
    if (handler != (uintptr_t)(image + 0x97dfc0)) return 0;
    memcpy(&handler, image + 0x27744a8, sizeof handler);
    if (handler != (uintptr_t)(image + (0x127c6b0 - shift))) return 0;
    memcpy(&handler, image + 0x2543218, sizeof handler);
    if (handler != (uintptr_t)(image + 0x9b4940)) return 0;
    memcpy(&handler, image + 0x2778390, sizeof handler);
    if (handler != (uintptr_t)(image + (0x1293ce0 - shift))) return 0;
    memcpy(&handler, image + 0x2777748, sizeof handler);
    if (handler != (uintptr_t)(image + (0x128e3a0 - shift))) return 0;
    memcpy(&handler, image + 0x2777738, sizeof handler);
    if (handler != (uintptr_t)(image + (0x128e320 - shift))) return 0;
#ifdef _WIN32
    /* Executable from the start: write_code() puts back the protection it found, and a
     * PAGE_READWRITE trampoline crashed the first call to the original season setter (code 3). */
    unsigned char *trampoline = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *trampoline = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return 0;
#endif
    if (!trampoline) return 0;
    /* Relocate the stolen RIP-relative LEA as MOVABS; preserve the original prologue. */
    memcpy(trampoline, prefix, 10);
    trampoline[10] = 0x48; trampoline[11] = 0xb8;
    const uintptr_t global = (uintptr_t)(image + 0x2af1fa8);
    memcpy(trampoline + 12, &global, sizeof global);
    jump_to(trampoline + 20, image + (0x1277e11 - shift));
    memcpy(trampoline + 64, season_prefix, sizeof season_prefix);
    jump_to(trampoline + 64 + sizeof season_prefix, image + (0xfb771f - shift));
    /* Intercept only open2's first pin read. The entry is also hooked by the graphics menu. */
    unsigned char *route = trampoline + 128;
    memcpy(route, open_pin, 7);
    memcpy(route + 7, "\x4c\x89\xf1\x48\xb8", 5); /* fourth argument = r14; movabs rax */
    const uintptr_t project_reader = (uintptr_t)menu_project;
    memcpy(route + 12, &project_reader, 8);
    memcpy(route + 20, "\xff\xd0", 2);
    memcpy(route + 22, open_pin + 12, 8);
    jump_to(route + 30, image + (0x1109245 - shift));
    memcpy(trampoline + 192, receive_prefix, sizeof receive_prefix);
    jump_to(trampoline + 192 + sizeof receive_prefix, image + (0xeb9b71 - shift));
    settings_sender_stub(trampoline + 256, 1, image + (0xebe462 - shift));
    size_t stub_end = 352 + settings_sender_stub(trampoline + 352, 0, image + (0xf4f980 - shift));
    memcpy(trampoline + 448, load_prefix, sizeof load_prefix);
    jump_to(trampoline + 448 + sizeof load_prefix, image + 0x9b1fce);
    stub_end = 448 + sizeof load_prefix + 14;
    memcpy(trampoline + 512, "\x4c\x89\xf7\x4c\x89\xfe\x48\xb8", 8);
    uintptr_t update = (uintptr_t)private_lobby_update;
    memcpy(trampoline + 520, &update, 8);
    memcpy(trampoline + 528, "\xff\xd0", 2);
    jump_to(trampoline + 530, image + (0xeba00a - shift));
    memcpy(trampoline + 544, participant_prefix, sizeof participant_prefix);
    jump_to(trampoline + 558, image + (0xfff16e - shift));
    memcpy(trampoline + 576, "\x48\xb8", 2);
    uintptr_t racers_global = (uintptr_t)(image + 0x2ad4b70);
    memcpy(trampoline + 578, &racers_global, 8);
    memcpy(trampoline + 586, "\x48\x8b\x00\x4c\x8b\x40\x10\x4d\x85\xc0", 10);
    jump_to(trampoline + 596, image + (0xf3443e - shift));
    memcpy(trampoline + 624, finish_prefix, sizeof finish_prefix);
    jump_to(trampoline + 638, image + (0xff232e - shift));
    memcpy(trampoline + 688, receive_prefix, sizeof receive_prefix);
    jump_to(trampoline + 705, image + 0xe6f841);
    memcpy(trampoline + 736, spawn_prefix, sizeof spawn_prefix);
    jump_to(trampoline + 756, image + (0xfff754 - shift));
    memcpy(trampoline + 784, place_prefix, sizeof place_prefix);
    jump_to(trampoline + 801, image + (0xf31171 - shift));
    memcpy(trampoline + 832, ui_prefix, sizeof ui_prefix);
    jump_to(trampoline + 849, image + (0xe9ba41 - shift));
    stub_end = 880 + podium_slot_stub(trampoline + 880, image);
    /* The racer receiver's prologue (no RIP-relative operand): copied, then back after it. */
    static const unsigned char custo_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,
        0x41,0x54,0x53,0x48,0x83,0xec,0x68};
    const size_t custo_at = 0xec03c0 - shift;
    const int custo_ok = !memcmp(image + custo_at, custo_prefix, sizeof custo_prefix);
    if (custo_ok) {
        memcpy(trampoline + 1024, custo_prefix, sizeof custo_prefix);
        jump_to(trampoline + 1024 + sizeof custo_prefix, image + custo_at + sizeof custo_prefix);
        stub_end = 1024 + sizeof custo_prefix + 14;
    }
    stub_end = 1120 + team_countdown_stub(trampoline + 1120, image, shift);
    memcpy(trampoline + 1280, setup_prefix, sizeof setup_prefix);
    jump_to(trampoline + 1280 + sizeof setup_prefix, image + (0xe9de91 - shift));
    stub_end = 1280 + sizeof setup_prefix + 14;
    memcpy(trampoline + 1344, "\x48\xb8", 2);
    uintptr_t ready_fn = (uintptr_t)team_initial_ready;
    memcpy(trampoline + 1346, &ready_fn, 8);
    memcpy(trampoline + 1354, "\xff\xd0", 2);
    memcpy(trampoline + 1356, ready_site + 5, 14);
    jump_to(trampoline + 1370, image + 0x9c1552);
    stub_end = 1384;
    memcpy(trampoline + 1408, open_prefix, sizeof open_prefix);
    jump_to(trampoline + 1425, image + 0x986831);
    memcpy(trampoline + 1472, countdown_prefix, sizeof countdown_prefix);
    jump_to(trampoline + 1489, image + (0xeb7961 - shift));
    memcpy(trampoline + 1536, damage_prefix, sizeof damage_prefix);
    jump_to(trampoline + 1553, image + 0xe330f1);
    original_kart_damage = (KartDamage)(trampoline + 1536);
    memcpy(trampoline + 1600, clock_prefix, sizeof clock_prefix);
    jump_to(trampoline + 1617, image + (0xf1fb61 - shift));
    original_clock_target = (ClockTarget)(trampoline + 1600);
    memcpy(trampoline + 1664, disable_prefix, sizeof disable_prefix);
    jump_to(trampoline + 1679, image + (0x110b9cf - shift));
    original_disable_controls_node = (DisableControlsNode)(trampoline + 1664);
    stub_end = 1693;
    memcpy(trampoline + 1728, broadcast_prefix, sizeof broadcast_prefix);
    jump_to(trampoline + 1745, image + 0x248d21);
    original_gui_broadcast = (ProjectEvent)(trampoline + 1728);
    memcpy(trampoline + 1792, start_prefix, sizeof start_prefix);
    memcpy(trampoline + 1805, "\x49\xbf", 2); /* MOVABS r15, stack-guard global. */
    void *start_guard_global = image + 0x2807098;
    memcpy(trampoline + 1807, &start_guard_global, 8);
    memcpy(trampoline + 1815, "\x4d\x8b\x3f", 3); /* MOV r15, [r15]. */
    jump_to(trampoline + 1818, image + (0xebe584 - shift));
    original_lobby_start = (NativeEvent)(trampoline + 1792);
    memcpy(trampoline + 1856, timer_init_prefix, sizeof timer_init_prefix);
    jump_to(trampoline + 1873, image + (0xeb71e1 - shift));
    original_countdown_init = (NativeEvent)(trampoline + 1856);
    memcpy(trampoline + 1920, confetti_prefix, 10);
    memcpy(trampoline + 1930, "\x48\xb8", 2);
    void *confetti_init_global = image + 0x2a92868;
    memcpy(trampoline + 1932, &confetti_init_global, 8);
    memcpy(trampoline + 1940, "\x8a\x00", 2);
    jump_to(trampoline + 1942, image + (0xf38300 - shift));
    native_win_confetti = (NativeEvent)(trampoline + 1920);
    memcpy(trampoline + 1984, sound_prefix, sizeof sound_prefix);
    jump_to(trampoline + 2001, image + (0x1093cb1 - shift));
    original_finish_sound_node = (SoundNode)(trampoline + 1984);
    memcpy(trampoline + 2048, legend_prefix, sizeof legend_prefix);
    jump_to(trampoline + 2065, image + (0x110bbd1 - shift));
    original_team_legend_node = (SoundNode)(trampoline + 2048);
    memcpy(trampoline + 2112, order_prefix, sizeof order_prefix);
    jump_to(trampoline + 2112 + sizeof order_prefix, image + (0xff11ae - shift));
    original_racer_ahead = (RacerAhead)(trampoline + 2112);
    memcpy(trampoline + 2176, guest_timer_prefix, 7);
    memcpy(trampoline + 2183, "\x48\xb8", 2);
    void *guest_init_global = image + 0x2a92868;
    memcpy(trampoline + 2185, &guest_init_global, 8);
    memcpy(trampoline + 2193, "\x8a\x00\x49\x89\xfe", 5);
    jump_to(trampoline + 2198, image + (0xf39640 - shift));
    original_guest_return_timer = (NativeEvent)(trampoline + 2176);
    schedule_guest_return = (ScheduleReturn)(image + (0xf398a0 - shift));
    memcpy(trampoline + 2240, lap_prefix, sizeof lap_prefix);
    jump_to(trampoline + 2240 + sizeof lap_prefix, image + (0xf350c0 - shift) + sizeof lap_prefix);
    original_record_lap = (RecordLap)(trampoline + 2240);
    complete_racer = (CompleteRacer)(image + (0xf351b0 - shift));
    stub_end = 2240 + sizeof lap_prefix + 14;
    team_confirm_type = (void **)(image + 0x2abee50);
    team_unconfirm_type = (void **)(image + 0x2abee68);
    original_lobby_countdown = (NativeEvent)(trampoline + 1472);
    in_network_game = (NetworkQuery)(image + 0xa90b10);
    run_graph_node = (RunGraphNode)(image + 0x46efd0);
    if (!write_code(trampoline, trampoline, stub_end)) return 0;
    original_clear_participants = (NativeEvent)(trampoline + 544);
    original_reset_racer_pool = (NativeEvent)(trampoline + 688);
    original_spawn_cup_ai = (NativeEvent)(trampoline + 736);
    original_place_racers = (NativeEvent)(trampoline + 784);
    original_race_ui = (NativeEvent)(trampoline + 832);
    original_team_selector_event = (EventHandler)(image + 0x9c1250);
    original_team_slot_update = (BehaviorUpdate)(image + 0x9c60b0);
    original_team_count_event = (EventHandler)(image + (0x1293030 - shift));
    selector_changed_type = (void **)(image + 0x2ae4a38);
    gui_input_type = (void **)(image + 0x2a21d00);
    gui_uninitialize_type = (void **)(image + 0x2a21ef0);
    player_manager = (unsigned char **)(image + 0x2ad1160);
    controller_player = (ControllerPlayer)(image + 0x97f160);
    find_player = (FindPlayer)(image + 0xc02600);
    ready_find = (ReadyFind)(image + 0x9c32c0);
    add_legend = (AddLegend)(image + 0xa09d30);
    original_marker_event = (EventHandler)(image + 0x9d67e0);
    marker_initialize_type = (void **)(image + 0x2abfc40);
    lookup_hero = (LookupHero)(image + (0xf9c610 - shift));
    original_leader_event = (EventHandler)(image + 0x9de1b0);
    original_leader_update = (BehaviorUpdate)(image + 0x9dedb0);
    original_ranking_row_event = (EventHandler)(image + 0x9b73f0);
    ranking_row_vtable = image + 0x2543600;
    original_project_event = (ProjectEvent)(image + 0x253740);
    open_project = (OpenProject)(trampoline + 1408);
    enable_project_controls = (NativeEvent)(image + 0xa09190);
    activate_project_input = (NativeEvent)(image + 0x24ac80);
    finish_project_context = (NativeEvent)(image + 0x27ae10);
    hide_placeable = (SetSeason)(image + 0x252110);
    gui_manager = (void **)(image + 0x2ac0dc0);
    race_finished_type = (void **)(image + 0x2ac02a8);
    local_racers_finished_type = (void **)(image + 0x2ac02b0);
    queue_pool_assets = (SetSeason)(image + (0xe73820 - shift));
    wait_pool_assets = (SetSeason)(image + (0xe73460 - shift));
    original_race_results = (NativeEvent)(image + 0xa298c0);
    sort_cup_racers = (SortCupRacers)(image + (0xeac6c0 - shift));
    original_race_finished = (RaceFinished)(trampoline + 576);
    original_finish_racer = (SetSeason)(trampoline + 624);
    racer_manager = (void **)(image + 0x2ad4b70);
    racer_faction = (RacerFaction)(image + (0xff1810 - shift));
    play_result_sound = (PlayResultSound)(image + (0x1024340 - shift));
    release_result_sound = (NativeEvent)(image + (0x149d260 - shift));
    set_animation_selector = (AnimationSelector)(image + 0xcb0c70);
    send_animation_event = (AnimationEvent)(image + 0xc64790);
    original_race_setup = (NativeEvent)(trampoline + 1280);
    assign_ai_teams = (NoArgument)(image + (0xe9dcf0 - shift));
    original_ready_insert = (ReadyInsert)(image + (0x14a6240 - shift));
    native_racer_pool = (unsigned char **)(image + 0x2ac57c0);
    end_timer_hud_mode = image + (0xe6675e);
    finish_timer_counter = image + (0xf34575 - shift);
    human_finish_shortcut = image + (0xf344e9 - shift);
    ai_reuse_branch = image + (0xfff9c2 - shift);
    ai_asset_reuse_branch = image + (0xffd9c7 - shift);
    original_load_map = (LoadMap)(trampoline + 448);
    next_cup_map = (NativeEvent)(image + (0x1046200 - shift));
    network_state = (NetworkState)(image + 0x94efa0);
    network_race = (NetworkRace)(image + 0x9fa130);
    return_to_lobby = (NoArgument)(image + 0xa9cbe0);
    receive_private_settings = (NativeEvent)(trampoline + 192);
    publish_private_track = (NativeEvent)(image + (0xf4f7c0 - shift));
    broadcast_race_settings = (NativeEvent)(image + 0xc2ed20);
    race_settings = (unsigned char **)(image + 0x2ad4e30);
    system_data = (void **)(image + 0x2ad9c38);
    resize_list = (ResizeList)(image + (0x14a6f20 - shift));
    clear_zone_list = (NativeEvent)(image + 0x9db9a0);
    make_handle = (MakeHandle)(image + (0x14bea20 - shift));
    object_pin = (ObjectPin)(image + 0x4708a0);
    original_cup_item = (EventHandler)(image + (0x127c6b0 - shift));
    original_level_button = (EventHandler)(image + 0x9b4940);
    original_timer_event = (EventHandler)(image + (0x1293ce0 - shift));
    original_lobby_event = (EventHandler)(image + (0x128e320 - shift));
    original_selector_update = (BehaviorUpdate)(image + 0x97dfc0);
    original_lobby_update = (BehaviorUpdate)(image + (0x128e3a0 - shift));
    intern_string = (InternString)(image + (0x1574080 - shift));
    drop_string = (DropString)(image + (0x1518e30 - shift));
    resolve_handle = (HandleOperation)(image + (0x149fca0 - shift));
    destroy_handle = (HandleOperation)(image + (0x149f280 - shift));
    string_pool = (void **)(image + 0x2af2600);
    sprite_asset = (GetType)(image + 0x24c720);
    composition_vtable = image + 0x2431b18;
    find_zone = (FindZone)(image + (0x102e320 - shift));
    zone_manager = (void **)(image + 0x2ae3c18);
    set_race_mode = (SetSeason)(image + (0xfdeb70 - shift));
    race_manager = (void **)(image + 0x2acde48);
    original_choice = (EmitChoice)trampoline;
    original_season = (SetSeason)(trampoline + 64);
    original_event = (EventHandler)(image + (0x1277c70 - shift));
    base_event = (EventHandler)(image + (0x125d570 - shift));
    refresh_selector = (NativeEvent)(image + (0x125da80 - shift));
    private_state = (PrivateState)(image + (0xf50350 - shift));
    initialize_type = (void **)(image + 0x2a21ee8);
    network = (unsigned char **)(image + 0x2acb3d8);
    native_notice = (NativeNotice)(image + 0xa548e0);
    native_can_start = (PrivateState)(image + (0xf507f0 - shift));
    unsigned char jump[14];
    jump_to(jump, countdown_init);
    if (!write_code(image + (0xeb71d0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, individual_confetti);
    if (!write_code(image + (0xf382f0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, team_legend_node);
    if (!write_code(image + (0x110bbc0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, finish_sound_node);
    if (!write_code(image + (0x1093ca0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, private_start_query);
    if (!write_code(image + (0x12108f0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, private_start_gate);
    if (!write_code(image + (0xebe570 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, open_race_project);
    if (!write_code(image + 0x986820, jump, sizeof jump)) return 0;
    jump_to(jump, trampoline + 1344);
    if (!write_code(image + 0x9c153f, jump, sizeof jump)) return 0;
    jump_to(jump, race_team_setup);
    if (!write_code(image + (0xe9de80 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, trampoline + 1120);
    if (!write_code(image + (0xeb79d3 - shift), jump, sizeof jump)) return 0;
    /* This 13-byte branch ends immediately before a shared loop label.
     * R11 is unused throughout the native receiver; keep that label intact. */
    unsigned char podium_jump[13] = {0x49,0xbb};
    const uintptr_t podium_target = (uintptr_t)(trampoline + 880);
    memcpy(podium_jump + 2, &podium_target, 8);
    memcpy(podium_jump + 10, "\x41\xff\xe3", 3);
    if (!write_code(image + 0xa29a92, podium_jump, sizeof podium_jump)) return 0;
    const size_t podium_entries[] = {0xa2a386, 0xa2a73a};
    for (unsigned i = 0; i < 2; ++i) {
        const int32_t displacement = (int32_t)(0xa29a92 - (podium_entries[i] + 6));
        if (!write_code(image + podium_entries[i] + 2, &displacement, 4)) return 0;
    }
    jump_to(jump, team_ready_broadcast);
    if (!write_code(image + 0x248d10, jump, sizeof jump)) return 0;
    const BehaviorUpdate team_slot_replacement = team_slot_update;
    if (!write_code(image + 0x2544140, &team_slot_replacement, sizeof team_slot_replacement)) return 0;
    const EventHandler team_selector_replacement = team_selector_event;
    if (!write_code(image + 0x2543d68, &team_selector_replacement, sizeof team_selector_replacement)) return 0;
    const EventHandler marker_replacement = marker_event;
    if (!write_code(image + 0x2545940, &marker_replacement, sizeof marker_replacement)) return 0;
    const EventHandler leader_replacement = leader_event;
    const BehaviorUpdate leader_tick = leader_update;
    if (!write_code(image + 0x2546008, &leader_replacement, sizeof leader_replacement) ||
        !write_code(image + 0x2546018, &leader_tick, sizeof leader_tick)) return 0;
    const EventHandler ranking_replacement = ranking_row_event;
    if (!write_code(image + 0x25436c8, &ranking_replacement, sizeof ranking_replacement)) return 0;
    /* The nicknames' colors (above): the kart tags' and the results rows' own updates, each the
     * behavior's vtable +0xd8 like the slots' and leaders' (only when they are the game's). */
    unsigned char *marker_native = pointer_at(image, 0x2545950), *results_native = pointer_at(image, 0x25436d8);
    if (marker_native > image && marker_native < image + 0x2400000 &&
        results_native > image && results_native < image + 0x2400000) {
        original_marker_update = (BehaviorUpdate)marker_native;
        original_results_item_update = (BehaviorUpdate)results_native;
        const BehaviorUpdate marker_tick = marker_update, results_tick = results_item_update;
        if (!write_code(image + 0x2545950, &marker_tick, sizeof marker_tick) ||
            !write_code(image + 0x25436d8, &results_tick, sizeof results_tick)) return 0;
        puts("Native Online: nicknames in color (lobby, race, results)");
    }
    /* the members rows' behavior: only when its slots hold the game's own functions */
    unsigned char *member_event_native = pointer_at(image, 0x2774d88), *member_update_native = pointer_at(image, 0x2774d98);
    if (member_event_native == image + 0x1281810 && member_update_native == image + 0x273fc0) {
        original_member_event = (EventHandler)member_event_native;
        original_member_update = (BehaviorUpdate)member_update_native;
        const EventHandler member_handler = member_event;
        const BehaviorUpdate member_tick = member_update;
        if (!write_code(image + 0x2774d88, &member_handler, sizeof member_handler) ||
            !write_code(image + 0x2774d98, &member_tick, sizeof member_tick)) return 0;
        puts("Native Online: the lobby members list shows the nicknames' colors");
    }
    const NativeEvent results_replacement = race_results_received;
    if (!write_code(image + 0x254c430, &results_replacement, sizeof results_replacement)) return 0;
    jump_to(jump, (void *)clear_participants);
    if (!write_code(image + (0xfff160 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)reset_racer_pool);
    if (!write_code(image + 0xe6f830, jump, sizeof jump)) return 0;
    jump_to(jump, (void *)spawn_cup_ai);
    if (!write_code(image + (0xfff740 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)place_cup_racers);
    if (!write_code(image + (0xf31160 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)race_ui);
    if (!write_code(image + (0xe9ba30 - shift), jump, sizeof jump)) return 0;
    const ProjectEvent project_replacement = cup_project_event;
    if (!write_code(image + 0x2432338, &project_replacement, sizeof project_replacement)) return 0;
    jump_to(jump, clock_target);
    if (!write_code(image + (0xf1fb50 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, kart_damage);
    if (!write_code(image + 0xe330e0, jump, sizeof jump)) return 0;
    jump_to(jump, lobby_countdown);
    if (!write_code(image + (0xeb7950 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, team_selector_disable_node);
    if (!write_code(image + (0x110b9c0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, ranking_network_branch);
    if (!write_code(image + (0x115a740 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)race_finished);
    if (!write_code(image + (0xf34430 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)racer_finished);
    if (!write_code(image + (0xff2320 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)team_race_order);
    if (!write_code(image + (0xff11a0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, trampoline + 512);
    if (!write_code(image + (0xeb9ffb - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)select_mode);
    const EventHandler team_count_replacement = team_count_event;
    if (!write_code(image + 0x2778150, &team_count_replacement, sizeof team_count_replacement)) return 0;
    const EventHandler replacement = selector_event;
    const EventHandler cup_replacement = cup_item_event;
    const EventHandler level_replacement = level_button_event;
    const EventHandler lobby_replacement = lobby_event;
    const BehaviorUpdate lobby_update_replacement = lobby_update;
    const BehaviorUpdate selector_update_replacement = selector_update;
    const EventHandler timer_replacement = timer_event;
    if (!write_code(image + (0x1277e00 - shift), jump, sizeof jump) ||
        !write_code(image + 0x2773b50, &selector_update_replacement, sizeof selector_update_replacement) ||
        !write_code(image + 0x27744a8, &cup_replacement, sizeof cup_replacement) ||
        !write_code(image + 0x2543218, &level_replacement, sizeof level_replacement) ||
        !write_code(image + 0x2778390, &timer_replacement, sizeof timer_replacement) ||
        !write_code(image + 0x2777738, &lobby_replacement, sizeof lobby_replacement) ||
        !write_code(image + 0x2777748, &lobby_update_replacement, sizeof lobby_update_replacement) ||
        !write_code(image + 0x2773b40, &replacement, sizeof replacement)) return 0;
    jump_to(jump, (void *)set_season);
    if (!write_code(image + (0xfb7710 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, route);
    if (!write_code(image + (0x1109231 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, private_settings_received);
    if (!write_code(image + (0xeb9b60 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, trampoline + 256);
    if (!write_code(image + (0xebe451 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, trampoline + 352);
    if (!write_code(image + (0xf4f972 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, load_private_map);
    if (!write_code(image + 0x9b1fc0, jump, sizeof jump)) return 0;
    jump_to(jump, private_race_timer_done);
    if (!write_code(image + (0xeac310 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, record_private_cup_lap);
    if (!write_code(image + (0xf350c0 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, private_guest_return_timer);
    if (!write_code(image + (0xf39630 - shift), jump, sizeof jump)) return 0;
    if (custo_ok) {
        original_peer_customization = (PeerCustomization)(trampoline + 1024);
        jump_to(jump, (void *)peer_customization);
        if (!write_code(image + custo_at, jump, sizeof jump)) return 0;
        puts("Native Online: racers from mods this game lacks are shown as Crash");
    } else {
        puts("Native Online: racer receiver not recognized; mod racers are not replaced");
    }
    puts("Native Online mode: NORMAL / ITEMLESS / BATTLE selector installed");
    return 1;
}
