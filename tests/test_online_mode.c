#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_name_colors.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

int runtime_name_color(const char *nick, float rgb[3]) { (void)nick, (void)rgb; return 0; }

static int compatibility_ready = 1;
int runtime_private_party_check(void) { return compatibility_ready; }
static void *deferred_start;
static void (ABI *resume_start)(void *);
void runtime_private_party_defer_start(void *self, void (ABI *resume)(void *)) {
    deferred_start=self; resume_start=resume;
}
int runtime_private_party_update(void) { return 0; }
void runtime_private_party_cancel(void) {}
static int mode_checks;
void runtime_race_options_map_loading(const char *map) { (void)map; }
static int public_available, public_selected, public_active, ranked_selected;
static int public_normal_active;
int runtime_public_match_active(void) { return public_normal_active || public_active; }
int runtime_public_itemless_active(void) { return public_active; }
int runtime_ranked_active(void) { return 0; }
int runtime_ranked_selected(void) { return ranked_selected; }
int runtime_public_itemless_available(void) { return public_available; }
int runtime_public_itemless_selected(void) { return public_selected; }
void runtime_public_itemless_reset(void) {}
void runtime_public_itemless_update(void) {}
void runtime_public_itemless_select(int enabled) { public_selected = enabled; }
static int synced_finished_mask=-1;
int runtime_ai_catchup_results(void) { return synced_finished_mask; }
void runtime_ai_catchup_results_publish(unsigned mask) {
    if (synced_finished_mask<0) synced_finished_mask=(int)mask;
}
static int speed = -1, private = 1, base_calls;
static int clock_calls;
static void ABI clock_mock(void *component, void *target, void *effect) {
    (void)component; (void)target; (void)effect;
    ++clock_calls;
}
static int damage_calls;
static int ABI damage_mock(unsigned char *component, unsigned kind, void **source, unsigned char flags) {
    (void)component; (void)kind; (void)source; (void)flags;
    ++damage_calls;
    return 0;
}
static int confetti_calls, animation_calls, animation_selection;
static unsigned char result_anim[8];
static int result_sounds, released_result_sounds, result_sound_won;
static void ABI result_sound_mock(void *kart, const char *name, void **sound, void **callback) {
    assert(kart && !callback[0] && !callback[1]);
    result_sound_won = !strcmp(name, "UI_Player_Lap_Change_End_Win");
    assert(result_sound_won || !strcmp(name, "UI_Player_Lap_Change_End_Lose"));
    *sound = result_anim; ++result_sounds;
}
static void ABI release_result_sound_mock(void *sound) {
    assert(sound == result_anim); ++released_result_sounds;
}
static void *ABI result_animation_mock(void *kart) { assert(kart); return result_anim; }
static void ABI confetti_mock(void *racer) { assert(racer); ++confetti_calls; }
static void ABI animation_selector_mock(void *anim, const char *name, int value) {
    assert(anim == result_anim && !strcmp(name, "VictoryLapAnimSelector"));
    animation_selection = value;
}
static void ABI animation_event_mock(void *anim, const char *name) {
    assert(anim == result_anim && !strcmp(name, "OnVictoryLap")); ++animation_calls;
}
static int team_settings_broadcasts, broadcast_team_count;
static int mode_ready = 1, mode_verified = 1, following_host;
int runtime_private_match_active(void) { return 0; }
int runtime_room_mode_following(void) { return following_host; }
int runtime_room_mode_ready(void) { return mode_ready; }
int runtime_room_mode_begin_race(void) { ++mode_checks; return mode_verified; }
void runtime_room_mode_end_race(void) {}
static int ai_team_calls;
static int inserted_ready, controls_enabled;
static void ABI activate_input_mock(unsigned char *project) { project[0x34] |= 0x20; }
static void ABI enable_controls_mock(void *project) {
    assert(project);
    int *disabled_depth = (int *)((unsigned char *)project + 0x48);
    if (*disabled_depth > 0) --*disabled_depth;
    ++controls_enabled;
}
static void ABI ready_insert_mock(void *table, void *player, unsigned char *ready, unsigned hash) {
    assert(table == (void *)1 && player == (void *)2 && hash == 3);
    inserted_ready = *ready;
}
static void ABI assign_ai_teams_mock(void) { ++ai_team_calls; }
static void ABI broadcast_teams_mock(unsigned char *settings) {
    ++team_settings_broadcasts;
    memcpy(&broadcast_team_count, settings + 0x28, 4);
}
static int ABI racer_faction_mock(unsigned char *racer) {
    int faction;
    memcpy(&faction, racer + 0x174, 4);
    return faction;
}
static int native_battle_row, native_row_icons;
static void ABI ranking_row_mock(unsigned char *self, void *event, void *owner) {
    (void)event; (void)owner;
    assert(self[0xd8] == native_battle_row); /* Preserve native Battle team binding. */
    unsigned char *position = *(void **)(self + 0xb8);
    unsigned char *asset = position ? *(void **)(position + 0x38) : NULL;
    if (asset) *(int *)(asset + 0x40) = 1; /* Native local-player gradient. */
    for (int i = 0; i < 2; ++i) {
        unsigned char *icon;
        memcpy(&icon, self + 0x98 + i * 8, sizeof icon);
        if (icon) { ++native_row_icons; *(unsigned *)(icon + 12) &= ~0x400u; }
    }
}
static int marker_native_flag = -1;
static void ABI marker_event_mock(unsigned char *self, unsigned char *event, void *owner) {
    (void)self; (void)owner;
    if (event) marker_native_flag = event[0x28];
}
static int legend_buttons[3], legend_adds;
static char current_legend[16];
static void ABI legend_dispatch_mock(void *project, unsigned char *event) {
    (void)project;
    int button = *(int *)(event + 0x18);
    assert(button == 1 || button == 2); legend_buttons[button] = 0;
}
static void ABI legend_add_mock(void *project, int button, char **label, unsigned char force) {
    assert(project && force && (button == 1 || button == 2));
    legend_buttons[button] = 1; ++legend_adds;
    snprintf(current_legend, sizeof current_legend, "%s", *label);
}
static int keep_slot_position;
static void ABI team_slot_update_mock(unsigned char *self, unsigned char *owner, float dt) {
    (void)owner; assert(dt == .25f);
    unsigned char *name, *number;
    memcpy(&name, self+0x10, 8); memcpy(&number, self+0x18, 8);
    if (!keep_slot_position) *(float *)(name+0x64)=85.f;
    *(unsigned *)(number+12) &= ~0x400u; /* Native binding can reveal the label again. */
}
static int selector_native_events;
static void ABI leader_event_mock(void *self, void *event, void *owner) {
    ++selector_native_events;
    (void)self; (void)event; (void)owner;
}
static void ABI leader_update_mock(void *self, void *owner, float dt) {
    (void)self; (void)owner; (void)dt;
}
static unsigned char hero_record[0x20];
static void *ABI lookup_hero_mock(void *catalog, const char *hero) {
    assert(catalog && !strcmp(hero, "hero_crash"));
    return hero_record;
}
static int room_active, lobby_updates;
static void ABI base_update(void *self, void *owner, float dt) {
    (void)self;
    assert(owner && dt == 0.25f);
    ++lobby_updates;
}
void runtime_speed_mode_set(int enabled) { speed = enabled; }
int runtime_speed_mode_enabled(void) { return speed == 1; }
int runtime_speed_race_active(void) { return public_active || (speed == 1 && room_active); }
static void ABI private_state(unsigned char *yes, unsigned char *no) {
    *yes = (unsigned char)private; *no = (unsigned char)!private;
}
static unsigned char *timer_config;
static float timer_ui_initial;
static int individual_sounds;
static void ABI timer_init_mock(unsigned char *self) {
    unsigned char *owner = *(void **)(self + 0x18);
    if (!owner || (owner[0x3b] & 1))
        memcpy(self + 0x28, timer_config + (private ? 0x110 : 0x11c), 4);
    memcpy(self + 0x2c, timer_config + 0x118, 4);
    memcpy(self + 0x30, timer_config + 0x114, 4);
    memcpy(&timer_ui_initial, self + 0x28, 4);
}
static void ABI individual_sound_mock(void *context, unsigned char *pins) {
    (void)context; (void)pins; ++individual_sounds;
}
static int gate_notices, gate_native_calls, gate_ready = 1;
static void ABI can_start_mock(unsigned char *yes, unsigned char *no) {
    *yes = (unsigned char)gate_ready; *no = (unsigned char)!gate_ready;
}
static void ABI lobby_start_mock(unsigned char *self) {
    ++gate_native_calls;
    self[0x42] = 1; /* This is the broadcast's native one-shot latch. */
}
static void ABI gate_notice_mock(const char *text, void *callback) {
    assert(!callback && ((strstr(text, "TEAMS WITH NO AI NEED") && strstr(text, "ENABLE AI")) ||
                        strstr(text, "synchroniz")));
    ++gate_notices;
}
static void ABI base_event(void *self, void *event, void *owner) {
    (void)self; (void)event; (void)owner; base_calls++;
}
static void *ABI event_type(void *event) {
    int index; memcpy(&index, (unsigned char *)event + 8, 4);
    return (void *)(uintptr_t)(index == -1 ? 123 : 456);
}
static void (ABI *choice)(void *, int);
static int selector_refreshes;
static void ABI refresh_selector_mock(unsigned char *self) {
    assert(*(int *)(self + 0x190) == 0);
    ++selector_refreshes;
    choice(NULL, 0); /* Must not dispatch a game-mode change while resetting Race Type. */
}
static void ABI original_handler(void *self, void *event, void *owner) {
    int index; memcpy(&index, (unsigned char *)event + 8, 4);
    /* Selection event precedes the native stored-index update. */
    choice(owner, index);
    memcpy((unsigned char *)self + 0x190, &index, 4);
}
static void stub(unsigned char *at, void *function) {
    /* Preserve the signature used by the installer, then tail-call the mock. */
    memcpy(at, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x5e\x41\x5f\x5d\x48\xb8", 15);
    memcpy(at + 15, &function, 8);
    memcpy(at + 23, "\xff\xe0", 2);
}
static void *local_player_mock;
static int local_controller_id;
static int ABI local_controller_mock(int controller) { assert(controller != 0); return local_controller_id; }
static void *ABI local_find_mock(void *list, int id) { (void)list; (void)id; return local_player_mock; }

static unsigned char selector_ready;
static int selector_ready_found = 1;
static int ABI selector_ready_find_mock(void *table, void *key, unsigned hash, unsigned char **value) {
    (void)table; (void)key; (void)hash;
    *value = &selector_ready;
    return selector_ready_found;
}

static int network_query_value = 1;
static void *last_graph_node;
static unsigned char ABI network_query_mock(void) { return (unsigned char)network_query_value; }
static void ABI run_node_mock(void *context, void *node) { (void)context; last_graph_node = node; }

static void put_pointer(unsigned char *at, size_t offset, const void *pointer) {
    memcpy(at + offset, &pointer, sizeof pointer);
}
static void ABI pin_value(void *context, void *pin, void **result) {
    (void)context;
    *result = pin;
}
static char *ABI make_string(void *pool, const char *text) {
    (void)pool;
    char *p = calloc(1, 16 + strlen(text) + 1);
    assert(p);
    strcpy(p + 16, text);
    return p + 16;
}
static void ABI free_string(void *pool, void *text) { (void)pool; free(text); }
static void *ABI get_sprite(void *row) {
    void *asset;
    memcpy(&asset, (char *)row + 0x38, sizeof asset);
    return asset;
}
static void direct_stub(unsigned char *at, void *function) {
    memcpy(at, "\x48\xb8", 2);
    memcpy(at + 2, &function, 8);
    memcpy(at + 10, "\xff\xe0", 2);
}
static int received_mode;
static void ABI receive_settings(unsigned char *event) { memcpy(&received_mode, event + 0x3c, 4); }
static unsigned char test_zones[4][0x1c0], test_handles[4][48];
static unsigned char lobby_zone[0x1b8], lobby_data[0x78];
static void *test_cup, *test_cup_handle;
static int published_tracks;
static int gui_updates;
static void ABI dispatch_lobby(void *project, void *event) {
    (void)project;
    unsigned char *e = event;
    assert(!strcmp(*(char **)(e + 0x18), "WUMPA CUP"));
    assert(*(int *)(e + 0x28) == 6);
    ++gui_updates;
}
static void ABI publish_track(unsigned char *slot) {
    assert(*(char **)slot && !strcmp(*(char **)slot, "0track"));
    ++published_tracks;
}
static void *ABI lookup_zone(void *manager, const char *map) {
    (void)manager;
    if (map && !strcmp(map, "octane/octane_onlinelobby/octane_onlinelobby")) return lobby_zone;
    return map && map[0] >= '0' && map[0] <= '3' ? test_zones[map[0] - '0'] : NULL;
}
static void ABI clear_list(unsigned char *list) { memset(list + 12, 0, 4); }
static void ABI resize_zone_list(unsigned char *list, int n, int width) {
    assert(n == 4 && width == 8);
    memcpy(list + 12, &n, 4);
}
static void ABI zone_handle(void **slot, void *zone) {
    if (zone == test_cup) { *slot = test_cup_handle; return; }
    for (int i = 0; i < 4; ++i) if (zone == test_zones[i]) {
        *slot = test_handles[i];
        put_pointer(test_handles[i], 0x28, zone);
        return;
    }
    static unsigned char project_handles[2048][0x30];
    static unsigned next;
    assert(next < sizeof project_handles / sizeof project_handles[0]);
    unsigned char *handle = project_handles[next++];
    *(unsigned *)handle = 4; put_pointer(handle, 0x28, zone); *slot = handle;
}
static int cleared_participants, forced_finishes;
static int pool_queued, pool_waited, ai_spawns;
static void ABI queue_pool_mock(unsigned char *pool, int wait) {
    assert(wait == 1);
    unsigned char *record = *(void **)(pool + 0x18);
    assert(!strcmp(*(char **)(record + 0x38), "hero-asset"));
    ++pool_queued;
}
static void ABI wait_pool_mock(unsigned char *pool, int wait) {
    assert(pool && wait == 1 && pool_queued == pool_waited + 1);
    ++pool_waited;
}
static void ABI spawn_ai_mock(void *manager) {
    assert(manager && pool_waited == pool_queued);
    ++ai_spawns;
}
static int disable_node_calls;
static void ABI disable_node_mock(void *context, unsigned char *pins) {
    (void)context; assert(pins); ++disable_node_calls;
}
static int expected_grid, grid_calls;
static unsigned char ranking_handle[0x30], panel_handle[0x30];
static unsigned char members_handle[0x30], members_project[0x20], members_root[0x120];
static unsigned char leaders_handle[0x30], leaders_project[0x20];
static unsigned char ranking_project[0x20], panel_project[0x20], panel_root[0x20];
static int panel_opens, panel_events, ranking_events, context_finishes, team_score_opens;
static const char *expected_ranking;
static void (ABI *open_race_ui_project)(void **, void *, const char *, void *, int, void *, float);
static void ABI race_ui_mock(unsigned char *self) {
    unsigned char options[40] = {0};
    *(int *)(options + 8) = 2; *(int *)(options + 12) = -1;
    *(int *)(options + 24) = 8; *(int *)(options + 28) = -1;
    const int previous = panel_opens;
    open_race_ui_project((void **)(self + 0x60), NULL,
                        runtime_online_mode_private_cup() ? "Octane_CupRace_Ranking" : "Octane_Race_Ranking",
                        options, -1, NULL, 0);
    if (runtime_online_mode_private_cup())
        assert(panel_opens == previous + 1); /* Before native intro would open. */
    put_pointer(self, 0x58, leaders_handle);
}
static void *ABI panel_event_type(unsigned char *event) { return *(void **)(event + 8); }
static void *ready_target, *ready_behavior, *ready_owner;
static void (ABI *ready_handler)(void *, void *, void *);
static int broadcast_routes, ready_project_events;
static void ABI project_event_mock(void *self, void *event) {
    if (self == ready_target && ready_handler) {
        ++ready_project_events; ready_handler(ready_behavior, event, ready_owner); return;
    }
    assert(event);
    if (self == panel_project) ++panel_events;
    else ++ranking_events;
}
static void ABI broadcast_mock(void *layer, void *event) {
    (void)layer;
    if (broadcast_routes) project_event_mock(ready_target, event);
}
static void ABI open_panel_mock(void **result, void *manager, const char *name,
                               unsigned char *options, int player, void *context, float delay) {
    if (!strcmp(name, "Octane_LobbyMembers")) {
        *result = members_handle;
        return;
    }
    if (context && !strcmp(name, "Octane_BattleTeam_Ranking")) {
        ++team_score_opens;
        ++*(unsigned *)ranking_handle;
        *result = ranking_handle;
        return;
    }
    if (!context) {
        assert(!strcmp(name, expected_ranking && strcmp(expected_ranking, "Octane_BattleTeam_Ranking") ? expected_ranking : runtime_online_mode_private_cup() ? "Octane_CupRace_Ranking" : "Octane_Race_Ranking"));
        *result = expected_ranking && !strcmp(expected_ranking, "Octane_BattleTeam_Ranking") ? panel_handle : ranking_handle;
        return;
    }
    (void)manager;
    assert(!strcmp(name, "Octane_Race_Ranking") && player == -1 && delay == 0.0f);
    assert(*(int *)(options + 8) == 2 && *(int *)(options + 12) == -1);
    assert(*(int *)(options + 24) == 8 && *(int *)(options + 28) == -1);
    assert(!*(void **)context);
    ++*(unsigned *)panel_handle;
    *result = panel_handle;
    panel_root[12] = 0;
    ++panel_opens;
}
static void ABI finish_context_mock(void *context) { assert(context); ++context_finishes; }
static void ABI hide_panel_mock(unsigned char *root, int hidden) {
    if (root == panel_root) {
        assert(hidden == 1);
        root[12] = 1;
    } else {
        unsigned flags;
        memcpy(&flags, root + 12, 4);
        if (hidden) flags |= 0x400u; else flags &= ~0x400u;
        memcpy(root + 12, &flags, 4);
    }
}
static void ABI place_racers_mock(unsigned char *manager) {
    unsigned char *list = *(void **)(manager + 0x10);
    unsigned char **items = *(void **)(list + 0x20);
    for (int i = 0; i < *(int *)(list + 12); ++i) {
        unsigned char *handle = *(void **)(items[i] + 0x10);
        unsigned char *player = handle ? *(void **)(handle + 0x28) : NULL;
        int id = *(int *)(player ? player + 0xc8 : items[i] + 0x164);
        assert(*(int *)(items[i] + 0x60) == (expected_grid == 2 ? (id == 7 ? 1 : id == 0 ? 8 : id + 1) : expected_grid ? id + 1 : 0));
    }
    ++grid_calls;
}
static int race_order_calls;
static unsigned char ABI native_ahead_mock(unsigned char *a, unsigned char *b) {
    ++race_order_calls;
    if (a[0x8c] && b[0x8c]) return *(int *)(a + 0x60) < *(int *)(b + 0x60);
    return *(float *)(a + 0x44) > *(float *)(b + 0x44);
}
static unsigned char **result_racers;
static unsigned char *complete_result_racer;
static int expected_animations_at_result = -1;
static void *podium_slots[3];
static int (ABI *podium_branch)(void *, int, void *);
static void *podium_hook;
static int cup_sorts;
static int compare_points(const void *a, const void *b) {
    const unsigned char *x = *(void *const *)a, *y = *(void *const *)b;
    int points = *(int *)(y + 0xb8) - *(int *)(x + 0xb8);
    return points ? points : *(int *)(x + 0x60) - *(int *)(y + 0x60);
}
static void ABI sort_cup_mock(void **begin, void **end, size_t count) {
    assert(end == begin + count && count == 8);
    qsort(begin, count, sizeof(void *), compare_points);
    ++cup_sorts;
}
static void ABI results_mock(void *event) {
    assert(event);
    if (expected_animations_at_result >= 0) assert(animation_calls == expected_animations_at_result);
    if (complete_result_racer) complete_result_racer[0x8c] = 1;
    if (!result_racers) return;
    memset(podium_slots, 0, sizeof podium_slots);
    for (int i = 0; i < 8; ++i) {
        int rank = podium_branch(result_racers[i], *(int *)(result_racers[i] + 0x60), podium_hook);
        assert(rank == podium_branch(result_racers[i], *(int *)(result_racers[i] + 0x60), (char *)podium_hook + (0xa2a37d - 0xa29a92)));
        assert(rank == podium_branch(result_racers[i], *(int *)(result_racers[i] + 0x60), (char *)podium_hook + (0xa2a735 - 0xa29a92)));
        if (rank > 0 && rank <= 3) podium_slots[rank - 1] = result_racers[i];
    }
}
static void ABI clear_participant_mock(void *manager) {
    assert(manager);
    unsigned char *active = *(void **)((char *)manager + 0x38);
    unsigned char *selected = *(void **)((char *)manager + 0x30);
    if (active) {
        void **items = *(void ***)(active + 0x20);
        for (int i = 0; i < *(int *)(active + 12); ++i) {
            put_pointer(items[i], 0x20, NULL);
            put_pointer(items[i], 0x28, NULL);
        }
        *(int *)(active + 12) = 0;
    }
    if (selected) *(int *)(selected + 12) = 0;
    ++cleared_participants;
}
static void ABI reset_pool_mock(unsigned char *pool) {
    for (int slot = 0; slot < 8; ++slot) {
        unsigned char *record = *(void **)(pool + 0x10 + slot * 8);
        if (!record) continue;
        const int offsets[] = {0x28, 0x30, 0x38, 0x48};
        for (unsigned j = 0; j < sizeof offsets / sizeof offsets[0]; ++j) {
            const int offset = offsets[j];
            char *asset = *(void **)(record + offset);
            if (asset) free_string(NULL, asset - 16);
            put_pointer(record, offset, NULL);
        }
        put_pointer(record, 0x10, NULL);
        put_pointer(record, 0x20, NULL);
    }
}
static void ABI finish_mock(unsigned char *racer, int yes) {
    assert(yes == 1 && racer[0xb0] && !racer[0x8c]);
    racer[0x8c] = 1;
    memcpy(racer + 0x60, racer + 0x58, 4); /* Native ff23cc. */
    const int points[] = {9,6,3,1,0,0,0,0};
    const int award = points[*(int *)(racer + 0x64) - 1];
    *(int *)(racer + 0xb4) = award;
    *(int *)(racer + 0xb8) += award;
    ++forced_finishes;
}
static int map_loads, cup_advances, lobby_returns, cup_index;
static unsigned char test_network_state[0x148];
static void ABI loaded_map(const char *map, int reason) { assert(map && reason == 8); ++map_loads; }
static void ABI advance_cup(void *zone) {
    assert(cup_index >= 0 && cup_index < 3 && zone == test_zones[cup_index]);
    ++cup_advances;
}
static void ABI return_lobby(void) { ++lobby_returns; }
static int recorded_laps, completed_ai;
static void ABI record_lap_mock(void *self, unsigned char *racer, int lap) {
    (void)self; (void)racer; (void)lap; ++recorded_laps;
}
static void ABI complete_ai_mock(void *self, unsigned char *racer) {
    (void)self; ++completed_ai; racer[0x8c]=1;
}
static int guest_native_returns, guest_schedules;
static void (*guest_deadline)(void);
static void ABI guest_native_return(void *self) { (void)self; ++guest_native_returns; }
static void ABI guest_schedule(void *self, void (*callback)(void), void *context) {
    assert(self == context);
    ++guest_schedules;
    guest_deadline = callback;
}
static void *ABI get_network_state(void) { return test_network_state; }
static int ABI in_network_race(int state) { return state == 16; }
static char received_hero[32];
static void *received_outfit;
static void ABI received_customization(void *self, unsigned char *message) {
    (void)self;
    const char *hero; memcpy(&hero, message + 0x88, 8);
    snprintf(received_hero, sizeof received_hero, "%s", hero);
    memcpy(&received_outfit, message + 0x90, 8);
}
static void customization(unsigned char *image, size_t shift, const char *hero) {
    unsigned char message[0xc0] = {0};
    const uint32_t fields = 8; memcpy(message + 0x20, &fields, 4);
    const int slot = 6; memcpy(message + 0xb0, &slot, 4);
    char *name = make_string(NULL, hero); memcpy(message + 0x88, &name, 8);
    void *outfit = (void *)0x1234; memcpy(message + 0x90, &outfit, 8);
    ((void (ABI *)(void *, unsigned char *))(image + (0xec03c0 - shift)))(NULL, message);
    char *now; memcpy(&now, message + 0x88, 8);
    if (now) free_string(NULL, now - 16);
}

int64_t runtime_file_stat(const char *path, void *guest_stat) {
    (void)guest_stat;
    return strstr(path, "/drivermymod.pak") ? 0 : -2;
}

static void test_region(size_t shift) {
    expected_ranking = NULL;
    team_score_opens = 0;
    speed = -1; private = 1; base_calls = 0;
    result_racers = NULL; cup_sorts = 0;
    const size_t size = 0x3000000;
    unsigned char *image = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    unsetenv("BB_ONLINE_MODE_HOOK");
    assert(runtime_online_mode_start(image, size));
    setenv("BB_ONLINE_MODE_HOOK", shift ? "1277df0" : "1277e00", 1);
    assert(!runtime_online_mode_start(image, size));
    memcpy(image + (0xebe570 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x48\x83\xec\x18\x4c\x8b\x3d\x14\x8b\x94\x01", 20);
    image[0xebe580 - shift] += (unsigned char)shift;
    memcpy(image + (0xebe584 - shift), "\x48\x83\xc4\x18\x5b\x41\x5e\x41\x5f\x5d", 10);
    direct_stub(image + (0xebe58e - shift), lobby_start_mock);
    memcpy(image + (0x12108f0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x48\x83\xec\x18", 13);
    direct_stub(image + (0xf507f0 - shift), can_start_mock);
    direct_stub(image + 0xa548e0, gate_notice_mock);
    memcpy(image + (0xf1fb50 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x38", 17);
    memcpy(image + (0xf1fb61 - shift), "\x48\x83\xc4\x38\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xf1fb6f - shift), clock_mock);
    memcpy(image + 0xe330e0, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x48", 17);
    memcpy(image + 0xe330f1, "\x48\x83\xc4\x48\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + 0xe330ff, damage_mock);
    memcpy(image + (0x110bbc0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x18", 17);
    memcpy(image + (0x110bbd1 - shift), "\x48\x83\xc4\x18\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0x110bbdf - shift), disable_node_mock);
    memcpy(image + (0x110b9c0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x10", 15);
    memcpy(image + (0x110b9cf - shift), "\x48\x83\xc4\x10\x5b\x41\x5c\x41\x5e\x41\x5f\x5d", 12);
    direct_stub(image + (0x110b9db - shift), disable_node_mock);
    memcpy(image + (0xeb7950 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x50\x48\x89\xfb\x48\x8b\x43\x18", 17);
    memcpy(image + (0xeb7961 - shift), "\x58\x5b\x41\x5e\x41\x5f\x5d\xc3", 8);
    memcpy(image + (0x115a740 - shift), "\x55\x48\x89\xe5\x41\x56\x53\x48\x89\xd3\x49\x89\xfe\xe8\xbe\x63\x93\xff", 18);
    image[0x115a74e - shift] += (unsigned char)shift;
    direct_stub(image + 0xa90b10, network_query_mock);
    direct_stub(image + 0x46efd0, run_node_mock);
    memcpy(image + (0xeb79d3 - shift), "\x48\x8b\x05\x6e\x64\xc1\x01\x8b\x80\x40\x0d\0\0\x83\xc0\xf7\x83\xf8\x04\x77\xae", 21);
    image[0xeb79d6 - shift] += (unsigned char)shift;
    memcpy(image + 0x9c153f, "\xe8\xfc\x4c\xae\0\x48\x8b\x8d\x68\xff\xff\xff\x4c\x8b\xbd\x40\xff\xff\xff", 19);
    image[0x9c1540] -= (unsigned char)shift;
    direct_stub(image + (0x14a6240 - shift), ready_insert_mock);
    memcpy(image + (0xe9de80 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x18", 17);
    memcpy(image + (0xe9de91 - shift), "\x48\x83\xc4\x18\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d\xc3", 15);
    direct_stub(image + (0xe9dcf0 - shift), assign_ai_teams_mock);
    memcpy(image + 0x2000, "\x48\x83\xec\x08", 4);
    stub(image + 0x2004, image + (0xeb79d3 - shift));
    memcpy(image + (0xeb79e8 - shift), "\xb8\x01\0\0\0\x48\x83\xc4\x08\xc3", 10);
    memcpy(image + (0xeb7996 - shift), "\x31\xc0\x48\x83\xc4\x08\xc3", 7);
    memcpy(image + (0x1277e00 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x50\x48\x8d\x05\x97\xa1\x87\x01", 17);
    image[0x1277e00 - shift + 13] += (unsigned char)shift;
    /* Record the remapped index and relocated LEA result, then undo the prologue. */
    memcpy(image + (0x1277e11 - shift), "\x89\x37\x48\x89\x47\x08\x48\x83\xc4\x08\x5b\x41\x5e\x41\x5f\x5d\xc3", 17);
    stub(image + (0x125d570 - shift), (void *)base_event);
    stub(image + (0x125da80 - shift), (void *)refresh_selector_mock);
    memcpy(image + (0xf34573 - shift), "\x45\x85\xed\x7e\xc7", 5);
    memcpy(image + (0xffd9c7 - shift), "\x0f\x85\xf2\x0b\0\0", 6);
    memcpy(image + (0xfff9c2 - shift), "\x0f\x85\xd6\x06\0\0", 6);
    stub(image + (0xf50350 - shift), (void *)private_state);
    direct_stub(image + 0xc2ed20, broadcast_teams_mock);
    direct_stub(image + (0xff1810 - shift), racer_faction_mock);
    memcpy(image + (0xf382f0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x50\x8a\x05\x68\xa5\xb5\x01", 16);
    image[0xf382fc - shift] += (unsigned char)shift;
    memcpy(image + (0xf38300 - shift), "\x48\x83\xc4\x08\x5b\x41\x5e\x41\x5f\x5d", 10);
    direct_stub(image + (0xf3830a - shift), confetti_mock);
    memcpy(image + (0xeb71d0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x58", 17);
    memcpy(image + (0xeb71e1 - shift), "\x48\x83\xc4\x58\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xeb71ef - shift), timer_init_mock);
    memcpy(image + (0x1093ca0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x38", 17);
    memcpy(image + (0x1093cb1 - shift), "\x48\x83\xc4\x38\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0x1093cbf - shift), individual_sound_mock);
    direct_stub(image + (0x1024340 - shift), result_sound_mock);
    direct_stub(image + (0x149d260 - shift), release_result_sound_mock);
    direct_stub(image + 0xcb0c70, animation_selector_mock);
    direct_stub(image + 0xc64790, animation_event_mock);
    stub(image + (0x1277c70 - shift), (void *)original_handler);
    direct_stub(image + 0x97dfc0, base_update);
    put_pointer(image, 0x2773b50, image + 0x97dfc0);
    void *handler = image + (0x1277c70 - shift);
    memcpy(image + 0x2773b40, &handler, 8);
    memcpy(image + (0xfff160 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x49\x89\xfe", 14);
    memcpy(image + (0xf34430 - shift), "\x48\x8b\x05\x39\x07\xba\x01\x4c\x8b\x40\x10\x4d\x85\xc0", 14);
    memcpy(image + (0xfff16e - shift), "\x5b\x41\x5c\x41\x5e\x41\x5f\x5d", 8);
    direct_stub(image + (0xfff176 - shift), clear_participant_mock);
    memcpy(image + (0xf344e6 - shift), "\x44\x39\xf2\x74\x54", 5);
    memcpy(image + (0xf3443e - shift), "\x31\xc0\xc3", 3);
    memcpy(image + (0xff2320 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x49\x89\xfc", 14);
    memcpy(image + (0xff232e - shift), "\x5b\x41\x5c\x41\x5e\x41\x5f\x5d", 8);
    direct_stub(image + (0xff2336 - shift), finish_mock);
    memcpy(image + 0xe6f830, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x18", 17);
    memcpy(image + 0xe6f841, "\x48\x83\xc4\x18\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + 0xe6f84f, reset_pool_mock);
    memcpy(image + (0xfff740 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x81\xec\xd8\0\0\0", 20);
    memcpy(image + (0xfff754 - shift), "\x48\x81\xc4\xd8\0\0\0\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 17);
    direct_stub(image + (0xfff765 - shift), spawn_ai_mock);
    memcpy(image + (0xff11a0 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x50", 14);
    memcpy(image + (0xff11ae - shift), "\x48\x83\xc4\x08\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xff11bc - shift), native_ahead_mock);
    memcpy(image + (0xf31160 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x58", 17);
    memcpy(image + (0xf31171 - shift), "\x48\x83\xc4\x58\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xf3117f - shift), place_racers_mock);
    memcpy(image + (0xe9ba30 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x48", 17);
    memcpy(image + (0xe9ba41 - shift), "\x48\x83\xc4\x48\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xe9ba4f - shift), race_ui_mock);
    direct_stub(image + 0x253740, project_event_mock);
    memcpy(image + 0x248d10, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x18", 17);
    memcpy(image + 0x248d21, "\x48\x83\xc4\x18\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + 0x248d2f, broadcast_mock);
    put_pointer(image, 0x2abee50, (void *)42); put_pointer(image, 0x2abee68, (void *)43);
    memcpy(image + 0x986820, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x48", 17);
    memcpy(image + 0x986831, "\x48\x83\xc4\x48\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + 0x98683f, open_panel_mock);
    open_race_ui_project = (void *)(image + 0x986820);
    direct_stub(image + 0x27ae10, finish_context_mock);
    direct_stub(image + 0x252110, hide_panel_mock);
    put_pointer(image, 0x2432338, image + 0x253740);
    put_pointer(image, 0x2ac02b0, (void *)11);
    put_pointer(image, 0x2ac02a8, (void *)12);
    put_pointer(image, 0x2ac02b0, (void *)11);
    memcpy(image + (0xe73820 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x18", 17);
    memcpy(image + (0xe73831 - shift), "\x48\x83\xc4\x18\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xe7383f - shift), queue_pool_mock);
    memcpy(image + (0xe73460 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x50", 10);
    memcpy(image + (0xe7346a - shift), "\x58\x5b\x41\x5e\x41\x5f\x5d", 7);
    direct_stub(image + (0xe73471 - shift), wait_pool_mock);
    memcpy(image + 0xa29a92, "\x48\x83\x7d\xb0\x00\x74\x06\x41\x83\xfd\x03\x7e\x21\x4c\x8b\x6d\x90", 17);
    memcpy(image + 0xa2a386, "\x0f\x85\x0d\xf7\xff\xff", 6);
    memcpy(image + 0xa2a73a, "\x0f\x85\x59\xf3\xff\xff", 6);
    stub(image + (0xeac6c0 - shift), sort_cup_mock);
    /* Reproduce the native registers/stack at the patched branch, executing
     * the installed trampoline and both its real continuation addresses. */
    const unsigned char branch_entry[] = {0x55,0x48,0x89,0xe5,0x41,0x55,0x41,0x56,
        0x48,0x83,0xec,0x60,0x49,0x89,0xfe,0x41,0x89,0xf5,
        0x48,0xc7,0x45,0xb0,1,0,0,0,0x48,0xc7,0x45,0x90,0,0,0,0,0xff,0xe2};
    memcpy(image + 0x1000, branch_entry, sizeof branch_entry);
    const unsigned char branch_exit[] = {0x48,0x83,0xc4,0x60,0x41,0x5e,0x41,0x5d,0x5d,0xc3};
    memcpy(image + 0xa29ac0, "\x44\x89\xe8", 3);
    memcpy(image + 0xa29ac3, branch_exit, sizeof branch_exit);
    memcpy(image + 0xa29aa3, "\x31\xc0", 2);
    memcpy(image + 0xa29aa5, branch_exit, sizeof branch_exit);
    memcpy(image + 0xa2a37d, "\x45\x89\x6e\x58\x48\x83\x7d\xb0\x00", 9);
    memcpy(image + 0xa2a38c, "\xe9\x0e\xf7\xff\xff", 5);
    memcpy(image + 0xa2a735, "\x48\x83\x7d\xb0\x00", 5);
    memcpy(image + 0xa2a740, "\xe9\x5a\xf3\xff\xff", 5);
    podium_branch = (void *)(image + 0x1000);
    podium_hook = image + 0xa29a92;
    direct_stub(image + 0xa298c0, results_mock);
    put_pointer(image, 0x254c430, image + 0xa298c0);
    put_pointer(image, 0x2778150, image + (0x1293030 - shift));
    stub(image + (0x1293030 - shift), (void *)base_event);
    put_pointer(image, 0x2ae4a38, (void *)999);
    direct_stub(image + 0x97f160, local_controller_mock);
    direct_stub(image + 0xc02600, local_find_mock);
    put_pointer(image, 0x2543d68, image + 0x9c1250);
    direct_stub(image + 0x9c1250, leader_event_mock);
    put_pointer(image, 0x2544140, image + 0x9c60b0);
    direct_stub(image + 0x9c60b0, team_slot_update_mock);
    put_pointer(image, 0x25436c8, image + 0x9b73f0);
    direct_stub(image + 0x9b73f0, ranking_row_mock);
    put_pointer(image,0x2545950,image+0x100300);
    put_pointer(image,0x25436d8,image+0x100320);
    direct_stub(image+0x100300,leader_update_mock);
    direct_stub(image+0x100320,leader_update_mock);
    memcpy(image + 0x253970, "\x8b\x47\x48\x31\xd2\x8d\x48\xff\x83\xf8\x01\x0f\x4f\xd1\x89\x57\x48\xc3", 18);
    memcpy(image + 0x253990, "\xff\x47\x48\xc3", 4);
    memcpy(image + 0xa09190, "\x55\x48\x89\xe5\x53\x50\x48\x89\xfb\x48\x85\xdb", 12);
    memcpy(image + 0xa0919c, "\x58\x5b\x5d", 3);
    direct_stub(image + 0xa0919f, enable_controls_mock);
    memcpy(image + 0x24ac80, "\x8a\x47\x34\x89\xc1\x80\xc9\x20", 8);
    direct_stub(image + 0x24ac88, activate_input_mock);
    put_pointer(image, 0x2546008, image + 0x9de1b0);
    put_pointer(image, 0x2546018, image + 0x9dedb0);
    direct_stub(image + 0x9de1b0, leader_event_mock);
    put_pointer(image, 0x2545940, image + 0x9d67e0);
    direct_stub(image + 0x9d67e0, marker_event_mock);
    stub(image + (0xf9c610 - shift), lookup_hero_mock);
    direct_stub(image + 0x9dedb0, leader_update_mock);
    int32_t roster_disp = 0x1ba0739 + (int32_t)shift;
    memcpy(image + (0xf34433 - shift), &roster_disp, 4);
    memcpy(image + (0xe9b9f9 - shift), "\x48\x8b\x05\x70\x91\xc3\x01\x48\x8b\x78\x28\xe8\xc7\xc4\x65\0", 16);
    roster_disp = 0x1c39170 + (int32_t)shift;
    memcpy(image + (0xe9b9fc - shift), &roster_disp, 4);
    image[0xe9ba09 - shift] = 0xc3;
    unsigned char racers_manager[0x50] = {0}, racers_list[40] = {0};
    unsigned char racers[8][0x180] = {{0}}, players[8][0x100] = {{0}}, handles[8][48] = {{0}};
    void *racer_items[8];
    for (int i = 0; i < 8; ++i) {
        racer_items[i] = racers[i];
        put_pointer(racers[i], 0x10, handles[i]);
        put_pointer(handles[i], 0x28, players[i]);
        *(int *)(players[i] + 0xc8) = i;
        *(uint64_t *)(players[i] + 0xa8) = 100 + i;
        *(int *)(racers[i] + 0x64) = i + 1;
    }
    *(int *)(racers_list + 12) = 8;
    result_racers = (unsigned char **)racer_items;
    put_pointer(racers_list, 0x20, racer_items);
    put_pointer(racers_manager, 0x10, racers_list);
    put_pointer(racers_manager, 0x28, racers_list);
    put_pointer(image, 0x2ad4b70, racers_manager);
    unsigned char state[0x148] = {0};
    void *ptr = state;
    memcpy(image + 0x2acb3d8, &ptr, 8);
    ptr = (void *)123;
    memcpy(image + 0x2a21ee8, &ptr, 8);
    /* Missing/mismatched seasonal code must reject installation before any writes. */
    assert(!runtime_online_mode_start(image, size));
    memcpy(image + (0xfb7710 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x20", 15);
    memcpy(image + (0xfb771f - shift), "\x89\x37\x48\x83\xc4\x20\x5b\x41\x5c\x41\x5e\x41\x5f\x5d\xc3", 15);
    memcpy(image + (0x1109231 - shift), "\x49\x8b\x46\x10\x48\x8b\x30\xe8\x63\x76\x36\xff\x48\xc7\x45\xc0\0\0\0\0", 20);
    image[0x1109239 - shift] += (unsigned char)shift;
    memcpy(image + (0xfdeb70 - shift), "\x55\x48\x89\xe5\x41\x56\x53\x48\x89\xfb\x83\xfe\x06", 13);
    memcpy(image + (0xfdeb7d - shift), "\x89\xb7\x40\x0d\0\0\x5b\x41\x5e\x5d\xc3", 11);
    direct_stub(image + 0x4708a0, (void *)pin_value);
    stub(image + (0x127c6b0 - shift), (void *)base_event);
    put_pointer(image, 0x27744a8, image + (0x127c6b0 - shift));
    stub(image + 0x9b4940, (void *)base_event);
    put_pointer(image, 0x2543218, image + 0x9b4940);
    stub(image + (0x128e320 - shift), (void *)base_event);
    put_pointer(image, 0x2777738, image + (0x128e320 - shift));
    stub(image + (0x128e3a0 - shift), (void *)base_update);
    put_pointer(image, 0x2777748, image + (0x128e3a0 - shift));
    stub(image + (0x1293ce0 - shift), (void *)base_event);
    put_pointer(image, 0x2778390, image + (0x1293ce0 - shift));
    direct_stub(image + (0x1574080 - shift), (void *)make_string);
    direct_stub(image + (0x1518e30 - shift), (void *)free_string);
    direct_stub(image + 0x24c720, (void *)get_sprite);
    unsigned char manager[0xe00] = {0};
    int *race_mode = (int *)(manager + 0xd40);
    *race_mode = 1;
    put_pointer(image, 0x2acde48, manager);
    memcpy(image + (0xeb9b60 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x18", 17);
    memcpy(image + (0xeb9b71 - shift), "\x48\x83\xc4\x18\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xeb9b7f - shift), receive_settings);
    memcpy(image + (0xeb9ffb - shift), "\x49\x8b\x06\x4c\x89\xf7\x4c\x89\xfe\xff\x90\xb0\0\0\0", 15);
    memcpy(image + (0xebe451 - shift), "\x48\x8b\x05\xf0\xf9\xc0\x01\x8b\x80\x40\x0d\0\0\x41\x89\x46\x3c", 17);
    int32_t displacement = 0x1c0f9f0 + (int32_t)shift;
    memcpy(image + (0xebe454 - shift), &displacement, 4);
    memcpy(image + (0xf4f972 - shift), "\x4c\x89\xe7\x8b\x80\x40\x0d\0\0\x41\x89\x44\x24\x3c", 14);
    direct_stub(image + (0x102e320 - shift), lookup_zone);
    direct_stub(image + (0x14bea20 - shift), zone_handle);
    direct_stub(image + (0xf4f7c0 - shift), publish_track);
    direct_stub(image + (0x14a6f20 - shift), resize_zone_list);
    direct_stub(image + 0x9db9a0, clear_list);
    memcpy(image + 0x9b1fc0, "\x55\x48\x89\xe5\x41\x56\x53\x48\x81\xec\x30\x01\0\0", 14);
    memcpy(image + 0x9b1fce, "\x48\x81\xc4\x30\x01\0\0\x5b\x41\x5e\x5d", 11);
    direct_stub(image + 0x9b1fd9, loaded_map);
    unsigned char *hud_gate = image + (0xe6675b);
    memcpy(hud_gate, "\x41\x83\xfd\x01\x0f\x85\xe9\x19\0\0\x48\x8b\x35\x54\xa6\xc5\x01", 17);
    displacement = 0x1c5a654;
    memcpy(hud_gate + 13, &displacement, 4);
    unsigned char *timer = image + (0xeac310 - shift);
    memcpy(timer, "\x55\x48\x89\xe5\xe8\x87\x2c\xaa\xff\x8b\xb8\x44\x01\0\0\xe8\x0c\xde\xb4\xff\x84\xc0\x74\x06\x5d\xe9\xb2\x08\xbf\xff\x5d\xc3", 32);
    int32_t call = (int32_t)(0x94efa0 - (0xeac319 - shift));
    memcpy(timer + 5, &call, 4);
    call = (int32_t)(0x9fa130 - (0xeac324 - shift)); memcpy(timer + 16, &call, 4);
    call = (int32_t)(0xa9cbe0 - (0xeac32e - shift)); memcpy(timer + 26, &call, 4);
    unsigned char *lap_hook=image+(0xf350c0-shift);
    memcpy(lap_hook,"\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x8b\x5f\x28\x41\x89\xd6",18);
    memcpy(lap_hook+18,"\x5b\x41\x5c\x41\x5e\x41\x5f\x5d",8);
    direct_stub(lap_hook+26,record_lap_mock);
    unsigned char *complete_hook=image+(0xf351b0-shift);
    memcpy(complete_hook,"\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x89\xf3",14);
    memcpy(complete_hook+14,"\x5b\x41\x5c\x41\x5e\x41\x5f\x5d",8);
    direct_stub(complete_hook+22,complete_ai_mock);
    unsigned char *guest_timer = image + (0xf39630 - shift);
    memcpy(guest_timer, "\x55\x48\x89\xe5\x41\x56\x53\x8a\x05\0\0\0\0\x49\x89\xfe", 16);
    call = (int32_t)(0x2a92868 - (0xf3963d - shift));
    memcpy(guest_timer + 9, &call, 4);
    memcpy(guest_timer + 16, "\x5b\x41\x5e\x5d", 4);
    direct_stub(guest_timer + 20, guest_native_return);
    direct_stub(image + (0xf398a0 - shift), guest_schedule);
    direct_stub(image + 0x94efa0, get_network_state);
    direct_stub(image + 0x9fa130, in_network_race);
    direct_stub(image + 0xa9cbe0, return_lobby);
    direct_stub(image + (0x1046200 - shift), advance_cup);
    /* The racer receiver: its prologue, then undo it and hand the message to the mock. */
    unsigned char *receiver = image + (0xec03c0 - shift);
    memcpy(receiver, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x68", 17);
    memcpy(receiver + 17, "\x48\x83\xc4\x68\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d\x48\xb8", 16);
    void *receiver_mock = (void *)received_customization;
    memcpy(receiver + 33, &receiver_mock, 8);
    memcpy(receiver + 41, "\xff\xe0", 2);
    assert(!runtime_online_mode_start(image, 0x2af1fa8));
    setenv("BB_ONLINE_MODE_HOOK", "unsupported", 1);
    assert(!runtime_online_mode_start(image, size));
    setenv("BB_ONLINE_MODE_HOOK", shift ? "1277e00" : "1277df0", 1);
    assert(!runtime_online_mode_start(image, size));
    setenv("BB_ONLINE_MODE_HOOK", shift ? "1277df0" : "1277e00", 1);
    assert(runtime_online_mode_start(image, size));
    /* A racer from a mod this game lacks shows as Crash; the game's own and this player's mod
     * racers stay; no list (an older launcher) changes nothing. */
    int (ABI *team_gate)(void) = (void *)(image + 0x2000);
    unsigned char *ready_trampoline = *(void **)(image + 0x9c153f + 6);
    void (ABI *initialize_ready)(void *, void *, unsigned char *, unsigned) = *(void **)(ready_trampoline + 2);
    unsigned char gate_settings[0x40] = {0};
    put_pointer(image, 0x2ad4e30, gate_settings);
    for (int room = 0; room <= 1; ++room)
        for (int mode = 0; mode <= 15; ++mode)
            for (int teams = 1; teams <= 4; ++teams) {
                runtime_online_mode_leave_cup();
                private = room;
                *race_mode = mode;
                *(int *)(gate_settings + 0x28) = teams;
                const int team_race = room && mode == 1 && teams > 1;
                assert(team_gate() == ((mode >= 9 && mode <= 13) || team_race));
                assert(*race_mode == mode);
                unsigned char ready = 1;
                initialize_ready((void *)1, (void *)2, &ready, 3);
                assert(inserted_ready == !team_race);
                assert(ready == 1);
            }
    runtime_online_mode_leave_cup();
    put_pointer(image, 0x2ad4e30, NULL);
    *race_mode = 1;
    private = 1;
    setenv("BB_KNOWN_HEROES", "DriverCrash,DriverCoco,DriverSmallNorm", 1);
    customization(image, shift, "DriverSonic");
    assert(!strcmp(received_hero, "DriverCrash") && !received_outfit);
    customization(image, shift, "drivercoco");
    assert(!strcmp(received_hero, "drivercoco") && received_outfit == (void *)0x1234);
    customization(image, shift, "DriverMyMod");
    assert(!strcmp(received_hero, "DriverMyMod") && received_outfit);
    unsetenv("BB_KNOWN_HEROES");
    customization(image, shift, "DriverSonic");
    assert(!strcmp(received_hero, "DriverSonic"));
    void (ABI *season)(void *, int) = (void (ABI *)(void *, int))(image + (0xfb7710 - shift));
    choice = (void (ABI *)(void *, int))(image + (0x1277e00 - shift));
    uintptr_t result[2] = {0};
    unsigned char self[0x1d0] = {0}, options[40] = {0}, flags[40] = {0}, items[3] = {1,1,1};
    ptr = options; memcpy(self + 0x1a8, &ptr, 8);
    ptr = flags; memcpy(self + 0x1b0, &ptr, 8);
    ptr = items; memcpy(flags + 0x20, &ptr, 8);
    const int count = 3; memcpy(options + 12, &count, 4);
    void *vtable[] = {(void *)event_type};
    struct { void **vtable; int index; } event = {vtable, -1};
    void (ABI *handle)(void *, void *, void *);
    memcpy(&handle, image + 0x2773b40, sizeof handle);
    private = 0;
    handle(self, &event, NULL);
    assert(base_calls == 1 && items[1] == 0 && speed == 0);
    private = 1;
    handle(self, &event, NULL);
    assert(base_calls == 2 && items[1] == 1);
    event.index = 1; handle(self, &event, result);
    assert(speed == 1 && result[0] == 0 && result[1] == (uintptr_t)(image + 0x2af1fa8));
    event.index = -1; handle(self, &event, NULL);
    int selected; memcpy(&selected, self + 0x190, 4);
    assert(selected == 1 && speed == 1); /* Return from the private lobby. */
    following_host = 1;
    runtime_online_mode_adopt(0); /* Guest follows Normal despite a personal Itemless choice. */
    handle(self, &event, NULL);
    memcpy(&selected, self + 0x190, 4);
    assert(selected == 0 && speed == 0 && runtime_online_mode_private_speed() == 1);
    runtime_online_mode_adopt(1);
    handle(self, &event, NULL);
    memcpy(&selected, self + 0x190, 4);
    assert(selected == 1 && speed == 1);
    following_host = 0;
    private = 0; handle(self, &event, NULL);
    memcpy(&selected, self + 0x190, 4);
    assert(selected == 0 && speed == 0 && items[1] == 0);
    private = 1; handle(self, &event, NULL);
    memcpy(&selected, self + 0x190, 4);
    assert(selected == 1 && speed == 1); /* Public navigation doesn't erase private choice. */
    event.index = 2; handle(self, &event, result);
    assert(speed == 0 && result[0] == 1);
    event.index = 0; handle(self, &event, result);
    assert(speed == 0 && result[0] == 0);
    private = 0;
    event.index = 1; handle(self, &event, result);
    assert(speed == 0 && result[0] == 0);
    public_available = 1;
    event.index = -1; handle(self, &event, NULL);
    assert(items[1] == 1 && speed == 0);
    event.index = 1; handle(self, &event, result);
    assert(public_selected == 1 && speed == 0 && result[0] == 0);
    event.index = -1; handle(self, &event, NULL);
    memcpy(&selected, self + 0x190, 4);
    assert(selected == 1 && speed == 0);
    event.index = 2; handle(self, &event, result);
    assert(public_selected == 0 && result[0] == 1);
    event.index = 0; handle(self, &event, result);
    ranked_selected = 1;
    event.index = -1; handle(self, &event, NULL);
    assert(items[2] == 0); /* Ranked offers only Normal and Itemless. */
    result[0] = 99;
    event.index = 2; handle(self, &event, result);
    assert(result[0] == 99);
    ranked_selected = 0;
    public_available = 0;
    event.index = 1;
    /* Other native two-option selectors retain their original index meanings. */
    const int ordinary_count = 2; memcpy(options + 12, &ordinary_count, 4);
    handle(self, &event, result);
    assert(speed == 0 && result[0] == 1);
    for (int theme = 0; theme <= 2; ++theme) {
        int applied = -1;
        speed = 0; room_active = 1;
        season(&applied, theme); assert(applied == theme);
        speed = 1;
        season(&applied, theme); assert(applied == 0);
        /* Remembered Speed must not suppress Adventure/Arcade/public seasons. */
        room_active = 0;
        season(&applied, theme); assert(applied == theme);
        /* Switching back to Normal restores the theme without a restart. */
        speed = 0; room_active = 1;
        season(&applied, theme); assert(applied == theme);
    }
    /* Race Type uses the same native selector without emitting a game-mode change. */
    unsigned char race[0x1d0] = {0}, race_options[40] = {0};
    unsigned char mode_row[0x120] = {0}, race_row[0x120] = {0}, button[0x120] = {0};
    unsigned char race_list[40] = {0}, button_list[40] = {0}, navigation[0x150] = {0};
    void *race_items[] = {race}, *button_items[] = {navigation};
    put_pointer(race, 0x148, "RACE TYPE");
    put_pointer(race, 0x1a8, race_options);
    memcpy(race_options + 12, &ordinary_count, 4);
    const int one = 1;
    memcpy(race_list + 12, &one, 4);
    memcpy(button_list + 12, &one, 4);
    put_pointer(race_list, 0x20, race_items);
    put_pointer(button_list, 0x20, button_items);
    put_pointer(race_row, 0xe8, race_list);
    put_pointer(button, 0xe8, button_list);
    put_pointer(race, 0x118, button);
    put_pointer(mode_row, 0x58, race_row);
    put_pointer(self, 0x118, button);
    memcpy(options + 12, &count, 4);
    private = 1;
    int normal = 0, battle = 2;
    memcpy(state + 0x13c, &normal, 4);
    event.index = 0;
    handle(race, &event, race_row);
    assert(!runtime_online_mode_private_cup());
    event.index = 1;
    const int saved_speed = speed;
    handle(race, &event, race_row);
    assert(runtime_online_mode_private_cup() && speed == saved_speed);
    unsigned char *route;
    memcpy(&route, image + (0x1109237 - shift), sizeof route);
    void (ABI *project)(void *, void *, void **, void *);
    memcpy(&project, route + 12, sizeof project);
    unsigned char args[24] = {0};
    int original_project, cup_project;
    void *pins[11] = {&original_project};
    pins[8] = &cup_project;
    put_pointer(args, 0x10, pins);
    const unsigned entry_count = 11, exit_count = 10, ordinary_pins = 8;
    memcpy(args, &entry_count, 4);
    void *opened = NULL;
    project(NULL, pins[0], &opened, args);
    assert(opened == &cup_project && *race_mode == 6);
    memcpy(args, &exit_count, 4);
    project(NULL, pins[0], &opened, args);
    assert(opened == &cup_project && *race_mode == 1);
    private = 0;
    memcpy(args, &entry_count, 4);
    project(NULL, pins[0], &opened, args);
    assert(opened == &original_project && *race_mode == 1);
    unsigned char title[0x120] = {0}, label[0x120] = {0};
    private = 1;
    unsigned char settings_owner[0x120] = {0}, settings_children[40] = {0};
    unsigned char level_behavior[40] = {0}, cup_data[0x90] = {0}, cup_handle[48] = {0};
    put_pointer(cup_handle, 0x28, cup_data);
    put_pointer(manager, 0xd88, cup_handle);
    put_pointer(cup_data, 0x30, "WUMPA CUP");
    put_pointer(level_behavior, 0x10, label);
    put_pointer(title, 0x28, "SelectedTrack");
    void *title_items[] = {title};
    put_pointer(settings_owner, 0x48, settings_children);
    put_pointer(settings_children, 0x20, title_items);
    memcpy(settings_children + 12, &one, 4);
    void (ABI *level_event)(void *, void *, void *);
    memcpy(&level_event, image + 0x2543218, sizeof level_event);
    event.index = -1;
    level_event(level_behavior, &event, settings_owner);
    assert(!strcmp(*(char **)(label + 0x30), "WUMPA CUP"));
    assert(!strcmp(*(char **)(title + 0x30), "CUP"));
    unsigned char card[0x120] = {0}, card_children[40] = {0};
    unsigned char card_name[0x120] = {0}, card_mode[0x120] = {0}, card_image[0x120] = {0};
    unsigned char image_asset[64] = {0}, image_handle[48] = {0};
    unsigned image_refs = 10;
    memcpy(image_handle, &image_refs, 4);
    put_pointer(cup_data, 0x58, image_handle);
    put_pointer(card_image, 0x38, image_asset);
    put_pointer(card, 0x28, "OCTR_PRIVATE_LOBBY");
    put_pointer(card, 0x48, card_children);
    put_pointer(card_name, 0x28, "[MapName]");
    put_pointer(card_mode, 0x28, "[ModeName]");
    put_pointer(card_image, 0x28, "OCTR_PRIVATE_PREVIEW");
    void *card_items[] = {card_name, card_mode, card_image};
    put_pointer(card_children, 0x20, card_items);
    memcpy(card_children + 12, &count, 4);
    void (ABI *card_event)(void *, void *, void *);
    memcpy(&card_event, image + 0x2777738, sizeof card_event);
    speed = 0; room_active = 1;
    card_event(NULL, &event, card);
    assert(!strcmp(*(char **)(card_name + 0x30), "WUMPA CUP"));
    assert(!strcmp(*(char **)(card_mode + 0x30), "CUP RACE - NORMAL"));
    assert(*(void **)(image_asset + 0x20) == image_handle && *(unsigned *)image_handle == 11);
    speed = 1;
    card_event(NULL, &event, card);
    assert(!strcmp(*(char **)(card_mode + 0x30), "CUP RACE - ITEMLESS"));
    room_active = 0; /* Empty lobby, or no recent peer traffic. */
    card_event(NULL, &event, card);
    assert(!runtime_speed_race_active());
    assert(!strcmp(*(char **)(card_mode + 0x30), "CUP RACE - ITEMLESS"));
    runtime_online_mode_adopt(0);
    card_event(NULL, &event, card);
    assert(!strcmp(*(char **)(card_mode + 0x30), "CUP RACE - NORMAL"));
    runtime_online_mode_adopt(1);
    assert(*(unsigned *)image_handle == 11); /* Repeated updates retain one reference. */
    /* A later native binding overwrites the card, with no input event afterward. */
    free_string(NULL, *(char **)(card_name + 0x30) - 16);
    free_string(NULL, *(char **)(card_mode + 0x30) - 16);
    put_pointer(card_name, 0x30, make_string(NULL, "CRASH COVE"));
    put_pointer(card_mode, 0x30, make_string(NULL, "SINGLE RACE"));
    put_pointer(image_asset, 0x20, NULL);
    --*(unsigned *)image_handle;
    void (ABI *card_update)(void *, void *, float);
    memcpy(&card_update, image + 0x2777748, sizeof card_update);
    lobby_updates = 0;
    card_update(NULL, card, 0.25f);
    assert(lobby_updates == 1);
    assert(!strcmp(*(char **)(card_name + 0x30), "WUMPA CUP"));
    assert(!strcmp(*(char **)(card_mode + 0x30), "CUP RACE - ITEMLESS"));
    assert(*(void **)(image_asset + 0x20) == image_handle && *(unsigned *)image_handle == 11);
    *(unsigned *)(card_name + 12) = *(unsigned *)(card_mode + 12) = 0;
    card_update(NULL, card, 0.25f);
    assert(lobby_updates == 2 && *(unsigned *)image_handle == 11);
    assert(!*(unsigned *)(card_name + 12) && !*(unsigned *)(card_mode + 12));
    room_active = 1;
    /* Public vote cards use confirmed lobby mode, including a party guest's mode. */
    private = 0;
    put_pointer(card, 0x28, "OCTR_PUBLIC_LOBBY");
    public_selected = 1; public_active = 0;
    card_update(NULL, card, 0.25f);
    assert(!strcmp(*(char **)(card_mode + 0x30), "SINGLE RACE - NORMAL"));
    /* Party members can retain the private scene flag after entering public. */
    private = 1;
    public_selected = 0; public_active = 1;
    card_update(NULL, card, 0.25f);
    assert(!strcmp(*(char **)(card_mode + 0x30), "SINGLE RACE - ITEMLESS"));
    put_pointer(card_mode, 0x28, "MapVoteResultModeName");
    public_active = 0;
    card_update(NULL, card, 0.25f);
    assert(!strcmp(*(char **)(card_mode + 0x30), "SINGLE RACE - NORMAL"));
    assert(!strcmp(*(char **)(card_name + 0x30), "WUMPA CUP"));
    /* Public Battle keeps its native label regardless of the private selector. */
    memcpy(state + 0x13c, &battle, 4);
    free_string(NULL, *(char **)(card_mode + 0x30) - 16);
    put_pointer(card_mode, 0x30, make_string(NULL, "BATTLE"));
    card_update(NULL, card, 0.25f);
    assert(!strcmp(*(char **)(card_mode + 0x30), "BATTLE"));
    memcpy(state + 0x13c, &normal, 4);
    private = 1;
    put_pointer(card, 0x28, "OCTR_PRIVATE_LOBBY");
    put_pointer(card_mode, 0x28, "[ModeName]");
    unsigned char system[0x190] = {0}, cup_list[40] = {0}, tracks[4][32] = {{0}};
    unsigned char selected_list[40] = {0}, packet[80] = {0};
    void *cup_items[] = {cup_data}, *track_items[4], *zone_items[4] = {0};
    const char *maps[] = {"0track", "1track", "2track", "3track"};
    for (int i = 0; i < 4; ++i) { track_items[i] = tracks[i]; put_pointer(tracks[i], 16, maps[i]); }
    int four = 4, racing = 1, cup_mode = 6, single_mode = 1;
    memcpy(cup_list + 12, &racing, 4);
    memcpy(cup_data + 12, &four, 4);
    put_pointer(cup_list, 32, cup_items);
    put_pointer(cup_data, 32, track_items);
    put_pointer(cup_data, 0x28, "wumpaCup");
    /* This native field is a string; interpreting it as a handle crashed peers. */
    put_pointer(cup_data, 0x78, "cup reference text, never a handle");
    test_cup = cup_data;
    test_cup_handle = cup_handle;
    put_pointer(system, 0x188, cup_list);
    unsigned char points_table[40] = {0};
    int native_points[] = {9, 6, 3, 1};
    *(int *)(points_table + 12) = 4;
    put_pointer(points_table, 0x20, native_points);
    put_pointer(system, 0x180, points_table);
    put_pointer(image, 0x2ad9c38, system);
    put_pointer(manager, 0xd80, selected_list);
    put_pointer(selected_list, 32, zone_items);
    unsigned char native_settings[0x40] = {0};
    put_pointer(image, 0x2ad4e30, native_settings);
    unsigned char private_settings[48] = {0};
    put_pointer(manager, 0xde8, private_settings);
    for (int i = 0; i < 4; ++i) put_pointer(test_zones[i], 0x10, maps[i]);
    /* Entering CUP initializes the first native cup and publishes its first map. */
    put_pointer(manager, 0xd88, NULL);
    runtime_online_mode_leave_cup();
    published_tracks = 0;
    memcpy(args, &exit_count, 4);
    project(NULL, pins[0], &opened, args);
    assert(*(void **)(manager + 0xd88) == cup_handle && published_tracks == 1);
    unsigned accept_count = 12;
    memcpy(args, &entry_count, 4);
    project(NULL, pins[0], &opened, args);
    memcpy(args, &accept_count, 4);
    project(NULL, pins[0], &opened, args);
    assert(published_tracks == 2 && *race_mode == 1);
    void (ABI *send_settings)(unsigned char *);
    unsigned char *sender;
    memcpy(&sender, image + (0xebe457 - shift), 8);
    memcpy(&send_settings, sender + 19, 8);
    send_settings(packet);
    assert(*(int *)(packet + 0x3c) == 6);
    memcpy(packet + 0x20, &racing, 4);
    put_pointer(packet, 0x28, maps[0]);
    void (ABI *received)(unsigned char *) = (void *)(image + (0xeb9b60 - shift));
    put_pointer(manager, 0xd88, NULL);
    received(packet);
    assert(received_mode == 1 && *(int *)(packet + 0x3c) == 6);
    assert(runtime_online_mode_private_cup());
    assert(*(void **)(manager + 0xd88) == cup_handle);
    void *forbidden_selector = NULL;
    open_race_ui_project(&forbidden_selector, NULL, "Octane_TeamSelect", NULL, -1, NULL, 0);
    assert(!forbidden_selector); /* Cup's lobby uses mode 1, but cannot inherit Team Select. */
    /* Settings updates use a GUI payload distinct from the network race mode. */
    unsigned char gui_event[64] = {0}, project_object[32] = {0}, project_vtable[0xb8] = {0};
    put_pointer(gui_event, 0x18, make_string(NULL, "CRASH COVE"));
    put_pointer(project_object, 0, project_vtable);
    put_pointer(project_object, 0x10, card);
    put_pointer(project_vtable, 0xb0, dispatch_lobby);
    unsigned char *gui_stub;
    void (ABI *gui_update)(void *, void *);
    memcpy(&gui_stub, image + (0xeb9ffb - shift) + 6, 8);
    memcpy(&gui_update, gui_stub + 8, 8);
    unsigned char composition_root[0x100] = {0}, composition[0x30] = {0}, composition_handle[0x30] = {0};
    put_pointer(composition_root, 0x38, composition_handle);
    put_pointer(composition_handle, 0x28, composition);
    put_pointer(composition, 0, image + 0x2431b18);
    put_pointer(composition, 0x20, card);
    put_pointer(project_object, 0x10, composition_root);
    gui_updates = 0;
    gui_update(project_object, gui_event);
    assert(gui_updates == 1 && *(void **)(gui_event + 0x20) == image_handle);
    assert(!strcmp(*(char **)(card_name + 0x30), "WUMPA CUP"));
    assert(!strcmp(*(char **)(card_mode + 0x30), "CUP RACE - ITEMLESS"));
    assert(received_mode == 1); /* GUI CUP must not turn the lobby into Relic Race. */
    free_string(NULL, *(char **)(gui_event + 0x18) - 16);
    assert(*(int *)(selected_list + 12) == 4);
    for (int i = 0; i < 4; ++i) assert(zone_items[i] == test_handles[i]);
    void (ABI *load_map)(const char *, int) = (void *)(image + 0x9b1fc0);
    void (ABI *timer_done)(void) = (void *)(image + (0xeac310 - shift));
    void (ABI *clear_participants)(void *) = (void *)(image + (0xfff160 - shift));
    int (ABI *check_finished)(void *) = (void *)(image + (0xf34430 - shift));
    void (ABI *receive_results)(unsigned char *);
    memcpy(&receive_results, image + 0x254c430, sizeof receive_results);
    map_loads = cup_advances = lobby_returns = 0;
    guest_schedules = guest_native_returns = 0;
    mode_verified = -1;
    load_map(maps[0], 8);
    assert(map_loads == 0 && lobby_returns == 0);
    void (ABI *pending_update)(void *, void *, float);
    memcpy(&pending_update, image + 0x2777748, 8);
    unsigned char pending_owner[0x100] = {0};
    const int before_updates = lobby_updates;
    pending_update(NULL, pending_owner, 0.25f);
    assert(lobby_updates == before_updates + 1 && map_loads == 0);
    mode_verified = 0;
    pending_update(NULL, pending_owner, 0.25f);
    assert(map_loads == 0 && lobby_returns == 1); /* Unknown mode never opens track assets. */
    /* Team selector keeps ticking when the lobby is hidden. A completed
     * transition must return before dereferencing invalidated selector objects. */
    void (ABI *team_pending_update)(void *, void *, float);
    memcpy(&team_pending_update,image+0x2544140,8);
    mode_verified=-1; load_map(maps[0],8);
    mode_verified=0; team_pending_update(NULL,NULL,.25f);
    assert(map_loads==0 && lobby_returns==2);
    mode_verified = 1;
    lobby_returns = 0;
    unsigned char pool[0x50] = {0}, pool_records[2][0x60] = {{0}};
    unsigned char ai_list[0x28] = {0}, ai_participant[0x48] = {0};
    unsigned char ai_selections[0x28] = {0};
    put_pointer(racers_manager, 0x30, ai_selections);
    void *ai_items[] = {ai_participant};
    put_pointer(ai_list, 0x20, ai_items);
    *(int *)(ai_list + 12) = 1;
    *(int *)(ai_participant + 0x40) = 1;
    put_pointer(racers_manager, 0x38, ai_list);
    for (int slot = 0; slot < 2; ++slot) put_pointer(pool, 0x10 + slot*8, pool_records[slot]);
    put_pointer(image, 0x2ac57c0, pool);
    void (ABI *reset_pool)(unsigned char *) = (void *)(image + 0xe6f830);
    void (ABI *spawn_ai)(void *) = (void *)(image + (0xfff740 - shift));
    void (ABI *place_racers)(void *) = (void *)(image + (0xf31160 - shift));
    grid_calls = 0;
    unsigned char race_ui_owner[0x70] = {0}, panel_event[16] = {0};
    void *panel_event_vtable[] = {(void *)panel_event_type};
    put_pointer(panel_event, 0, panel_event_vtable);
    put_pointer(ranking_handle, 0x28, ranking_project);
    put_pointer(panel_handle, 0x28, panel_project);
    put_pointer(panel_project, 0x10, panel_root);
    *(unsigned *)ranking_handle = *(unsigned *)panel_handle = 100;
    unsigned char cup_title_root[0x120]={0}, cup_title_list[0x28]={0}, cup_title[0x120]={0};
    unsigned char standings_title[0x120]={0};
    void *title_rows[]={standings_title,cup_title};
    put_pointer(ranking_project,0x10,cup_title_root);
    put_pointer(cup_title_root,0x48,cup_title_list);
    put_pointer(cup_title_list,0x20,title_rows); *(int *)(cup_title_list+12)=2;
    put_pointer(cup_title,0x28,"[CupName]");
    put_pointer(standings_title,0x28,"[STANDINGS]");
    put_pointer(standings_title,0x30,"STANDINGS");
    panel_opens = panel_events = ranking_events = context_finishes = 0;
    void (ABI *initialize_race_ui)(void *) = (void *)(image + (0xe9ba30 - shift));
    void (ABI *project_event)(void *, void *) = *(void **)(image + 0x2432338);
    pool_queued = pool_waited = ai_spawns = 0;
    cleared_participants = forced_finishes = 0;
    memset(native_settings, 0, sizeof native_settings);
    put_pointer(image, 0x2ad4e30, native_settings);
    int (ABI *cup_race_order)(unsigned char *, unsigned char *) = (void *)(image + (0xff11a0 - shift));
    unsigned char cup_laps[8][40] = {{0}};
    int cup_times[8][3] = {{0}};
    *(int *)(test_network_state + 0x144) = 16;
    for (cup_index = 0; cup_index < 4; ++cup_index) {
        *race_mode = 1;
        for (int slot = 0; slot < 2; ++slot) {
            put_pointer(pool_records[slot], (cup_index == 2 ? 0x28 : cup_index == 3 ? 0x30 : 0x38), make_string(NULL, "hero-asset"));
            put_pointer(pool_records[slot], 0x48, make_string(NULL, "outfit-asset"));
            put_pointer(pool_records[slot], 0x10, racers[slot]);
            put_pointer(pool_records[slot], 0x20, racers[slot]);
        }
        *(int *)(ai_list + 12) = 1;
        *(int *)(ai_selections + 12) = 1;
        put_pointer(ai_participant, 0x20, racers[0]);
        put_pointer(ai_participant, 0x28, racers[1]);
        for (int k = 0; k < 8; ++k) *(int *)(racers[k] + 0x60) = 8-k;
        if (cup_index==0) {
            mode_verified=-1; load_map(maps[cup_index],8);
            assert(map_loads==0);
            mode_verified=1; team_pending_update(NULL,NULL,.25f);
            assert(map_loads==1); /* No lobby tick needed to start the guest. */
        } else load_map(maps[cup_index], 8);
        assert(*(unsigned *)ranking_handle == 100 && *(unsigned *)panel_handle == 100);
        initialize_race_ui(race_ui_owner);
        put_pointer(panel_event, 8, (void *)11); /* local racer finished */
        project_event(panel_project, panel_event); /* unrelated project */
        assert(panel_opens == cup_index + 1);
        project_event(ranking_project, panel_event);
        project_event(ranking_project, panel_event); /* no duplicate project */
        assert(panel_opens == cup_index + 1 && context_finishes == panel_opens);
        assert(*(unsigned *)ranking_handle == 101 && *(unsigned *)panel_handle == 101);
        int events_before = panel_events;
        put_pointer(panel_event, 8, (void *)99); /* Other project's input/init event. */
        project_event(ranking_project, panel_event);
        assert(panel_events == events_before);
        put_pointer(panel_event, 8, (void *)12); /* full CUP scoreboard */
        /* Guest's missing title and a later native overwrite both get repaired. */
        put_pointer(cup_title,0x30,make_string(NULL,""));
        project_event(ranking_project, panel_event);
        assert(!strcmp(*(char **)(cup_title+0x30),"WUMPA CUP"));
        assert(!strcmp(*(char **)(standings_title+0x30),"STANDINGS"));
        put_pointer(cup_title,0x30,make_string(NULL,"stale cup"));
        project_event(ranking_project, panel_event);
        assert(!strcmp(*(char **)(cup_title+0x30),"WUMPA CUP"));
        assert(panel_root[12] && panel_events == events_before);
        put_pointer(panel_event, 8, (void *)11);
        project_event(ranking_project, panel_event); /* late local finish cannot reopen */
        assert(panel_opens == cup_index + 1 && panel_events == events_before);
        assert(*race_mode == 6 && *(int *)(manager + 0xd68) == cup_index);
        if (!cup_index) {
            unsigned char finisher[0x170]={0}, entity[0x50]={0}, handle[0x30]={0};
            unsigned char laps[0x28]={0}, clock_owner[0x40]={0};
            int times[]={10000,20000,30000};
            put_pointer(handle,0x28,entity); put_pointer(finisher,0x20,handle);
            put_pointer(finisher,0x90,laps); put_pointer(laps,0x20,times);
            *(int *)(laps+12)=3; *(int *)(finisher+0x4c)=4;
            finisher[0xb0]=1; entity[0x3b]=1;
            void (ABI *record_lap)(void *,unsigned char *,int)=(void *)(image+(0xf350c0-shift));
            const int old_required=*(int *)(native_settings+12);
            *(int *)(native_settings+12)=3;
            recorded_laps=completed_ai=0;
            /* Missing times, incomplete laps, a human and a follower cannot finish. */
            times[2]=0; record_lap(clock_owner,finisher,3); assert(!completed_ai);
            times[2]=30000; *(int *)(laps+12)=2;
            record_lap(clock_owner,finisher,3); assert(!completed_ai);
            *(int *)(laps+12)=3; entity[0x3b]=0;
            record_lap(clock_owner,finisher,3); assert(!completed_ai);
            entity[0x3b]=1; put_pointer(finisher,0x10,handles[0]);
            record_lap(clock_owner,finisher,3); assert(!completed_ai);
            put_pointer(finisher,0x10,NULL);
            record_lap(clock_owner,finisher,2); assert(!completed_ai);
            record_lap(clock_owner,finisher,3); assert(completed_ai==1 && finisher[0x8c]);
            record_lap(clock_owner,finisher,3); assert(completed_ai==1 && recorded_laps==7);
            assert(times[0]==10000 && times[1]==20000 && times[2]==30000);
            *(int *)(native_settings+12)=old_required;
        }
        assert(image[0xf34575 - shift] == 0xff);
        assert(!memcmp(image + (0xf344e9 - shift), "\x90\x90", 2));
        assert(hud_gate[3] == 6); /* Same native timer HUD as online Single Race. */
        assert(!memcmp(image + (0xfff9c2 - shift), "\x90\x90\x90\x90\x90\x90", 6));
        clear_participants(racers_manager);
        assert(cleared_participants == cup_index + 1);
        assert(*(int *)(ai_list + 12) == 0);
        assert(!*(void **)(ai_participant + 0x20) && !*(void **)(ai_participant + 0x28));
        assert(*(int *)(ai_selections + 12) == (cup_index ? 1 : 0));
        assert(!memcmp(image + (0xffd9c7 - shift), "\x90\x90\x90\x90\x90\x90", 6));
        reset_pool_mock(pool); /* Earlier teardown already cleared the pool fields. */
        reset_pool(pool);
        assert(!*(void **)(pool_records[0] + 0x38)); /* human uses native reload */
        assert(!*(void **)(pool_records[1] + 0x10)); /* old entity is released */
        assert(!*(void **)(pool_records[1] + 0x20)); /* old resource is released */
        if (cup_index) {
            assert(!strcmp(*(char **)(pool_records[1] + 0x38), "hero-asset"));
            assert(!strcmp(*(char **)(pool_records[1] + 0x48), "outfit-asset"));
        } else assert(!*(void **)(pool_records[1] + 0x38));
        reset_pool_mock(pool);
        spawn_ai(racers_manager); /* A later cleanup must not lose the saved request. */
        assert(ai_spawns == cup_index + 1 && pool_waited == cup_index);
        reset_pool_mock(pool);
        for (int k = 0; k < 8; ++k) {
            racers[k][0x8c] = 0;
            *(int *)(racers[k] + 0xb8) = 0; /* Newly allocated remote/AI racer. */
            *(int *)(racers[k] + 0x60) = 0;
        }
        /* Native list order may change with the new starting grid. */
        for (int k = 0; k < 8; ++k) racer_items[k] = racers[7-k];
        expected_grid = cup_index > 0;
        for (int k = 0; k < 8; ++k) {
            *(int *)(racers[k] + 0x58) = 8-k;
            *(float *)(racers[k] + 0x44) = 0;
        }
        place_racers(racers_manager);
        unsigned char (ABI *ahead)(void *, void *) = (void *)(image + (0xff11a0 - shift));
        for (int a = 0; a < 8; ++a) for (int b = 0; b < 8; ++b)
            assert(ahead(racers[a], racers[b]) == (a > b));
        *(float *)(racers[0] + 0x44) = 10;
        assert(ahead(racers[0], racers[7]) && !ahead(racers[7], racers[0]));
        *(float *)(racers[0] + 0x44) = 0;

        assert(grid_calls == cup_index + 1);
        /* Any of the eight native slots can be the local test client. */
        for (int local = 0; local < 8; ++local) {
            for (int k = 0; k < 8; ++k) {
                racers[k][0xb0] = k == local;
            }
            assert(!check_finished(NULL));
            void (ABI *native_finish)(unsigned char *, int) = (void *)(image + (0xff2320 - shift));
            native_finish(racers[local], 1); /* Isolated mock scoring callback, never live gameplay. */
            for (int k = 0; k < 8; ++k) assert(racers[k][0x8c] == (k <= local));
        }
        assert(check_finished(NULL));
        const int awards[] = {9,6,3,1,0,0,0,0};
        for (int k = 0; k < 8; ++k)
            assert(*(int *)(racers[k] + 0xb8) == (cup_index + 1) * awards[k]);
        unsigned char results[0x40] = {0};
        *(int *)(native_settings + 12) = 3;
        for (int k = 0; k < 8; ++k) {
            put_pointer(racers[k], 0x90, cup_laps[k]);
            put_pointer(cup_laps[k], 0x20, cup_times[k]);
            *(int *)(cup_laps[k] + 12) = 3;
            cup_times[k][2] = 90000 + k * 1000;
        }
        *(int *)(racers[1] + 0x60) = 8;
        for (speed = 0; speed <= 1; ++speed) {
            assert(cup_race_order(racers[1], racers[2]));
            assert(!cup_race_order(racers[2], racers[1]));
        }
        speed = 1;
        /* Delayed host results replace locally awarded positions, including ties
         * resolved by the host; native list order is intentionally reversed. */
        for (int k = 0; k < 8; ++k) *(int *)(results + 0x20 + 4*k) = 7-k;
        if (!cup_index) {
            timer_done(); /* The timer can beat the host result packet. */
            assert(cup_advances == 0);
        }
        unsigned char finish_laps[3][40]={{0}};
        int finish_times[3][3]={{10000,20000,30000},{10000,20000,31000},{10000,20000,0}};
        const int saved_required=*(int *)(native_settings+12);
        *(int *)(native_settings+12)=3;
        for (int k=0;k<3;++k) {
            put_pointer(racers[k],0x90,finish_laps[k]);
            put_pointer(finish_laps[k],0x20,finish_times[k]);
            *(int *)(finish_laps[k]+12)=k==2 ? 2 : 3;
            *(int *)(racers[k]+0x6c)=k==0 ? 29000 : 0;
        }
        receive_results(results);
        assert(*(int *)(racers[0]+0x6c)==30000); /* Local-clock skew. */
        assert(*(int *)(racers[1]+0x6c)==31000); /* Received laps, missing final callback. */
        assert(*(int *)(racers[2]+0x6c)==0); /* Real DNF cannot become a finish. */
        receive_results(results); /* A repeated result must not add points twice. */
        assert(cup_race_order(racers[2], racers[1])); /* Host order wins over guest times. */
        assert(!cup_race_order(racers[1], racers[2]));
        /* A late callback or timestamp packet after results must be repaired
         * before the native panel draws its next event. */
        *(int *)(racers[1]+0x6c)=0;
        finish_times[1][2]=31500;
        put_pointer(panel_event,8,(void *)77);
        project_event(ranking_project,panel_event);
        assert(*(int *)(racers[1]+0x6c)==31500);
        for (int k=0;k<3;++k) put_pointer(racers[k],0x90,NULL);
        *(int *)(native_settings+12)=saved_required;
        if (cup_index == 3) {
            assert(podium_slots[0] == racers[0]); /* CUP leader lost this track. */
            assert(podium_slots[1] == racers[1]);
            assert(podium_slots[2] == racers[7]);
            assert(cup_sorts == 2);
        } else {
            assert(cup_sorts == 0);
            assert(podium_slots[0] == racers[7]);
            assert(podium_slots[1] == racers[6]);
            assert(podium_slots[2] == racers[5]);
        }
        for (int k = 0; k < 8; ++k) {
            assert(*(int *)(racers[k] + 0x64) == 8-k);
            assert(*(int *)(racers[k] + 0xb4) == awards[7-k]);
            assert(*(int *)(racers[k] + 0xb8) == cup_index * awards[k] + awards[7-k]);
        }
        /* Host results can beat a remote finish callback to this client. */
        racers[7][0x8c] = 0;
        *(int *)(racers[7] + 0x58) = 8;
        *(int *)(racers[7] + 0x64) = 2;
        void (ABI *late_finish)(unsigned char *, int) = (void *)(image + (0xff2320 - shift));
        late_finish(racers[7], 1);
        assert(*(int *)(racers[7] + 0x58) == 1);
        assert(*(int *)(racers[7] + 0x60) == 1);
        assert(*(int *)(racers[7] + 0x64) == 1);
        assert(*(int *)(racers[7] + 0xb4) == 9);
        assert(*(int *)(racers[7] + 0xb8) == 9);
        *(int *)(results + 0x24) = 7; /* Invalid duplicate identity: no partial scoring. */
        receive_results(results);
        assert(*(int *)(racers[7] + 0xb8) == cup_index * awards[7] + awards[0]);
        for (int k = 0; k < 8; ++k) *(int *)(results + 0x20 + 4*k) = k;
        receive_results(results);
        for (int k = 0; k < 8; ++k)
            assert(*(int *)(racers[k] + 0xb8) == (cup_index + 1) * awards[k]);
        *(int *)(racers[0] + 0xb8) = 999; /* Late local state is not the next round baseline. */
        private = 0; /* The lobby component is gone while racing. */
        assert(runtime_online_mode_private_cup());
        if (cup_index) timer_done();
        /* A second process receives the native host finish countdown. Its
         * deadline must advance the same track without the lobby/drop flow. */
        void (ABI *guest_done)(void *) = (void *)(image + (0xf39630 - shift));
        const int host_advances = cup_advances, host_returns = lobby_returns;
        guest_done(manager);
        assert(guest_schedules == cup_index + 1 && !guest_native_returns);
        guest_deadline();
        assert(cup_advances == host_advances + (cup_index < 3));
        assert(lobby_returns == host_returns + (cup_index == 3));
        cup_advances = host_advances;
        lobby_returns = host_returns;
        assert(cup_advances == (cup_index < 3 ? cup_index + 1 : 3));
        assert(lobby_returns == (cup_index == 3));
    }
    /* AI spawn slots differ from IDs in the host network result packet. */
    unsigned char network_ai[8][0x60] = {{0}};
    void *network_ai_items[8];
    for (int i = 0; i < 8; ++i) {
        network_ai_items[i] = network_ai[i];
        *(int *)(network_ai[i] + 0x5c) = i;
        *(int *)(network_ai[i] + 0x58) = i + 16;
    }
    put_pointer(ai_selections, 0x20, network_ai_items);
    *(int *)(ai_selections + 12) = 8;
    unsigned char mixed_results[0x40] = {0};
    for (int k = 0; k < 8; ++k) *(int *)(mixed_results + 0x20 + 4*k) = k;
    put_pointer(racers[6], 0x10, NULL);
    put_pointer(racers[7], 0x10, NULL);
    *(int *)(racers[6] + 0x164) = 6;
    *(int *)(racers[7] + 0x164) = 7;
    *(int *)(mixed_results + 0x38) = 22;
    *(int *)(mixed_results + 0x20) = 23;
    *(int *)(mixed_results + 0x3c) = 0;
    receive_results(mixed_results);
    receive_results(mixed_results);
    assert(*(int *)(racers[7] + 0x64) == 1);
    assert(*(int *)(racers[7] + 0xb8) == 9);
    assert(*(int *)(racers[0] + 0xb8) == 27);
    /* Preserve human and AI grid identities across recreated racers. */
    cup_index = 1;
    for (int k = 0; k < 8; ++k) *(int *)(racers[k] + 0x60) = 8-k;
    load_map(maps[cup_index], 8);
    for (int k = 0; k < 8; ++k) *(int *)(racers[k] + 0x60) = 0;
    expected_grid = 2;
    place_racers(racers_manager);
    put_pointer(racers[6], 0x10, handles[6]);
    put_pointer(racers[7], 0x10, handles[7]);
    *(int *)(mixed_results + 0x20) = 7;
    *(int *)(mixed_results + 0x38) = 6;
    unsigned char timer_owner[0x100] = {0}, timer_label[0x100] = {0}, timer_children[40] = {0};
    void *timer_items[] = {timer_label};
    put_pointer(timer_owner, 0x28, "OCTR_PRIVATE_RETURN_TIMER");
    *(float *)(timer_owner + 0x60) = 1601.0f;
    put_pointer(timer_owner, 0x48, timer_children);
    put_pointer(timer_children, 0x20, timer_items);
    *(int *)(timer_children + 12) = 1;
    put_pointer(timer_label, 0x28, "TimerDesc");
    void (ABI *handle_timer)(void *, void *, void *);
    memcpy(&handle_timer, image + 0x2778390, 8);
    cup_index = 0;
    *(int *)(manager + 0xd68) = cup_index;
    handle_timer(NULL, &event, timer_owner);
    assert(!strcmp(*(char **)(timer_label + 0x30), "NEXT RACE 2/4"));
    assert(*(float *)(timer_owner + 0x60) == 1601.0f);
    cup_index = 2;
    *(int *)(manager + 0xd68) = cup_index;
    handle_timer(NULL, &event, timer_owner);
    assert(!strcmp(*(char **)(timer_label + 0x30), "NEXT RACE 4/4"));
    free_string(NULL, *(char **)(timer_label + 0x30) - 16);
    assert(map_loads == 5 && forced_finishes == 36);
    put_pointer(lobby_zone, 0x1b0, lobby_data);
    const char *lobby_map = "octane/octane_onlinelobby/octane_onlinelobby";
    /* A completed final result permits the native podium, even after lobby load. */
    cup_index = 3;
    load_map(maps[3], 8);
    receive_results(mixed_results);
    lobby_data[0x71] = 1;
    load_map(lobby_map, 8);
    assert(lobby_data[0x71] == 1);
    /* Host disconnects on final track: don't show track 3's cached podium. */
    private = 1;
    load_map(maps[3], 8);
    lobby_data[0x71] = 1;
    load_map(lobby_map, 8);
    assert(lobby_data[0x71] == 0);
    /* An ordinary lobby load leaves its native podium flag alone. */
    lobby_data[0x71] = 1;
    load_map(lobby_map, 8);
    assert(lobby_data[0x71] == 1);
    private = 0;
    *race_mode = 1;
    load_map("unrelated-map", 8);
    assert(*race_mode == 1 && !runtime_online_mode_private_cup());
    clear_participants(racers_manager);
    assert(cleared_participants == 5);
    assert(*(int *)(ai_selections + 12) == 0);
    assert(!memcmp(image + (0xffd9c7 - shift), "\x0f\x85\xf2\x0b\0\0", 6));
    assert(image[0xf34575 - shift] == 0xed);
    assert(!memcmp(image + (0xf344e9 - shift), "\x74\x54", 2));
    assert(hud_gate[3] == 1); /* Offline/non-CUP keeps the native gate. */
    assert(!memcmp(image + (0xfff9c2 - shift), "\x0f\x85\xd6\x06\0\0", 6));
    private = 1;
    *(int *)(test_network_state + 0x144) = 8;
    timer_done();
    assert(lobby_returns == 1 && cup_advances == 3);
    void (ABI *guest_single_done)(void *) = (void *)(image + (0xf39630 - shift));
    guest_single_done(manager);
    assert(guest_native_returns == 1);
    *race_mode = 1;
    unsigned before = *(unsigned *)cup_handle;
    received(packet);
    assert(*(unsigned *)cup_handle == before);
    *(int *)(manager + 0xd4c) = 6;
    runtime_online_mode_leave_cup();
    assert(*(int *)(manager + 0xd4c) == 6); /* standings survive host loss */
    load_map("octane/octane_onlinelobby/octane_onlinelobby", 8);
    assert(*race_mode == 1 && *(int *)(manager + 0xd4c) == 1);
    assert(*(int *)(private_settings + 0x20) == 1);
    memcpy(packet + 0x3c, &single_mode, 4);
    *(int *)(private_settings + 0x28) = 1; /* Host selected No Teams. */
    received(packet);
    assert(received_mode == 1 && !runtime_online_mode_private_cup());
    room_active = 0;
    runtime_online_mode_adopt(1);
    card_event(NULL, &event, card);
    assert(!runtime_speed_race_active());
    assert(!strcmp(*(char **)(card_mode + 0x30), "SINGLE RACE - ITEMLESS"));
    runtime_online_mode_adopt(0);
    card_event(NULL, &event, card);
    assert(!strcmp(*(char **)(card_mode + 0x30), "SINGLE RACE - NORMAL"));
    room_active = 1;
    assert(*(int *)(private_settings + 0x28) == 1);
    memset(native_settings, 0, sizeof native_settings);
    *(int *)(native_settings + 0x28) = 2;
    unsigned char team_slot[0x88]={0}, slot_name[0x120]={0}, slot_number[0x120]={0};
    unsigned char slot_tick[0x120]={0}, slot_children[0x28]={0};
    void *slot_child_items[]={slot_tick};
    put_pointer(slot_name,0x48,slot_children);
    *(int *)(slot_children+12)=1;
    put_pointer(slot_children,0x20,slot_child_items);
    *(float *)(slot_tick+0x64)=7.8647f;
    *(float *)(slot_tick+0x94)=100.f;
    *(float *)(slot_tick+0x88)=.65f;
    *(float *)(slot_tick+0x70)=.5f;
    put_pointer(team_slot,0x10,slot_name); put_pointer(team_slot,0x18,slot_number);
    *(float *)(slot_number+0x64)=60.f;
    *(float *)(slot_number+0x94)=40.f; *(float *)(slot_name+0x94)=50.f;
    *(float *)(slot_number+0x70)=.5f; *(float *)(slot_name+0x70)=.5f;
    void (ABI *slot_update)(void *,void *,float)=*(void **)(image+0x2544140);
    private=1;
    for(int update=0;update<2;++update) {
        slot_update(team_slot,NULL,.25f);
        assert(*(unsigned *)(slot_number+12)&0x400);
        assert(*(float *)(slot_name+0x64)==75.f && (*(unsigned *)(slot_name+12)&0x1000));
        assert(*(float *)(slot_tick+0x64)==25.f);
    }
    keep_slot_position=1;
    for (int update=0; update<20; ++update) {
        slot_update(team_slot,NULL,.25f);
        assert(*(float *)(slot_name+0x64)==75.f); /* No cumulative upward drift. */
    }
    *(float *)(slot_number+0x94)=60.f;
    slot_update(team_slot,NULL,.25f);
    assert(*(float *)(slot_name+0x64)==70.f); /* Re-centers after layout size changes. */
    keep_slot_position=0;
    *(float *)(slot_tick+0x64)=7.8647f;
    private=0; slot_update(team_slot,NULL,.25f);
    assert(*(float *)(slot_tick+0x64)==7.8647f);
    assert(!(*(unsigned *)(slot_number+12)&0x400) && *(float *)(slot_name+0x64)==85.f);
    unsigned char gate_manager[0x20] = {0}, gate_list[0x28] = {0};
    put_pointer(gate_manager, 0x10, gate_list);
    put_pointer(image, 0x2ad1160, gate_manager);
    unsigned char gate_lobby[0x50] = {0};
    void (ABI *start_gate)(void *) = (void *)(image + (0xebe570 - shift));
    unsigned char query_pins[0x28] = {0}, query_outputs[0x28] = {0};
    void *query_nodes[] = {(void *)201, (void *)202};
    put_pointer(query_outputs, 0x20, query_nodes);
    void (ABI *start_query)(void *, void *, void *) = (void *)(image + (0x12108f0 - shift));
    for (int room = 0; room <= 1; ++room)
        for (int teams = 1; teams <= 4; teams *= 2)
            for (int ai = 0; ai <= 1; ++ai)
                for (int players = 0; players <= 9; ++players) {
                    private = room;
                    private_settings[0x18] = !ai; /* Stale private settings must not control the gate. */
                    native_settings[0x10] = ai;
                    *(int *)(native_settings + 0x28) = teams;
                    *(int *)(gate_list + 0x0c) = players;
                    gate_notices = gate_native_calls = 0;
                    gate_lobby[0x42] = 0;
                    start_gate(gate_lobby);
                    const int blocked = room && teams > 1 && !ai && (players < teams || players > 8 || players % teams);
                    assert(gate_notices == blocked && gate_native_calls == !blocked);
                    assert(gate_lobby[0x42] == !blocked);
                    for (int marked = 0; marked <= 1; ++marked)
                        for (gate_ready = 0; gate_ready <= 1; ++gate_ready) {
                            *(int *)query_pins = marked;
                            gate_notices = 0;
                            last_graph_node = NULL;
                            start_query(NULL, query_pins, query_outputs);
                            assert(gate_notices == (marked && blocked));
                            assert(last_graph_node == (marked && blocked ? NULL : query_nodes[marked ? 0 : !gate_ready]));
                            if (marked && blocked) {
                                start_query(NULL, query_pins, query_outputs);
                                assert(gate_notices == 2 && last_graph_node == NULL);
                            }
                        }
                    gate_ready = 1;
                }
    private=1;native_settings[0x10]=1;
    *(int *)(native_settings+0x28)=2;
    gate_lobby[0x42]=0;gate_native_calls=0;compatibility_ready=0;
    start_gate(gate_lobby);
    assert(gate_native_calls==0 && gate_lobby[0x42]==0);
    compatibility_ready=-1;start_gate(gate_lobby);
    assert(gate_native_calls==0 && deferred_start==gate_lobby && resume_start);
    compatibility_ready=1;gate_lobby[0x42]=1;
    resume_start(deferred_start);assert(gate_native_calls==0);
    gate_lobby[0x42]=0;resume_start(deferred_start);
    assert(gate_native_calls==1 && gate_lobby[0x42]==1);
    gate_lobby[0x42]=0;mode_ready=0;
    start_gate(gate_lobby);
    assert(gate_native_calls==1 && gate_lobby[0x42]==0);
    mode_ready=1;start_gate(gate_lobby);
    assert(gate_native_calls==2 && gate_lobby[0x42]==1);
    put_pointer(image, 0x2ad1160, NULL);
    void (ABI *setup_teams)(void *) = (void *)(image + (0xe9de80 - shift));
    for (int room = 0; room <= 1; ++room)
        for (int mode = 1; mode <= 13; ++mode)
            for (int teams = 1; teams <= 4; ++teams)
                for (int ai = 0; ai <= 1; ++ai) {
                    private = room;
                    *(int *)(manager + 0xd4c) = mode;
                    *(int *)(native_settings + 0x28) = teams;
                    native_settings[0x10] = ai;
                    ai_team_calls = 0;
                    setup_teams(NULL);
                    assert(ai_team_calls == (room && mode == 1 && teams >= 2 && ai));
                }
    *(int *)(manager + 0xd4c) = 1;
    team_settings_broadcasts = 0;
    private = 1;
    for (int teams = 1; teams <= 4; ++teams) {
        memcpy(native_settings + 0x28, &teams, 4);
        send_settings(packet);
        assert(team_settings_broadcasts == teams && broadcast_team_count == (teams == 3 ? 2 : teams));
        assert(*(int *)(packet + 0x3c) == 1);
    }
    assert(runtime_online_mode_private_teams());
    private = 0;
    assert(!runtime_online_mode_private_teams());
    send_settings(packet);
    assert(team_settings_broadcasts == 4);
    private = 1;
    memcpy(packet + 0x3c, &cup_mode, 4);
    put_pointer(packet, 0x28, "unknown-track");
    received(packet);
    assert(received_mode == 1 && !runtime_online_mode_private_cup());
    runtime_online_mode_leave_cup();
    assert(runtime_online_mode_private_cup()); /* retain own host preference */
    assert(!runtime_online_mode_private_teams()); /* Teams remain disabled in Cup. */
    send_settings(packet);
    assert(*(int *)(packet + 0x3c) == 6);
    assert(team_settings_broadcasts == 5); /* CUP also replicates race settings. */
    free_string(NULL, *(char **)(card_name + 0x30) - 16);
    free_string(NULL, *(char **)(card_mode + 0x30) - 16);
    free_string(NULL, *(char **)(label + 0x30) - 16);
    free_string(NULL, *(char **)(title + 0x30) - 16);
    private = 1;
    memcpy(args, &ordinary_pins, 4);
    project(NULL, pins[0], &opened, args);
    assert(opened == &original_project && *race_mode == 1);
    /* The original choice would overwrite the first eight bytes of the owner. */
    assert(*(uintptr_t *)race_row == 0);
    event.index = -1;
    handle(race, &event, race_row);
    memcpy(&selected, race + 0x190, 4);
    assert(selected == 1);
    handle(self, &event, mode_row);
    unsigned visible;
    memcpy(&visible, race_row + 12, 4);
    assert(!(visible & 0x400));
    assert(*(void **)(self + 0x118) == race_row);
    assert(*(void **)(navigation + 0x110) == race_row);
    selector_refreshes = 0;
    put_pointer(race, 0x198, race_options);
    /* The network type is still Normal while the selector emits Battle. */
    event.index = 2;
    handle(self, &event, mode_row);
    assert(!runtime_online_mode_private_cup());
    memcpy(&visible, race_row + 12, 4);
    assert(visible & 0x400);
    assert(*(void **)(self + 0x118) == button);
    assert(*(void **)(navigation + 0x110) == mode_row);
    void (ABI *update_selector)(void *, void *, float);
    memcpy(&update_selector, image + 0x2773b50, 8);
    *(unsigned *)(race_row + 12) &= ~0x400u;
    update_selector(self, mode_row, 0.25f);
    update_selector(self, mode_row, 0.25f);
    assert(selector_refreshes == 1);
    assert(*(unsigned *)(race_row + 12) & 0x400u);
    event.index = 0;
    memcpy(state + 0x13c, &battle, 4);
    handle(race, &event, race_row); /* Hidden Battle input cannot select CUP. */
    memcpy(state + 0x13c, &normal, 4);
    assert(!runtime_online_mode_private_cup());
    assert(*(int *)(race + 0x190) == 0);
    /* Returning from Battle: lobby state still says Battle when CUP is picked. */
    memcpy(state + 0x13c, &battle, 4);
    event.index = 1;
    handle(self, &event, mode_row);
    handle(race, &event, race_row);
    assert(runtime_online_mode_private_cup());
    assert(*(int *)(race + 0x190) == 1);
    unsigned char team_setting_behavior[0x1d0] = {0}, team_available[40] = {0};
    unsigned char team_choices[] = {1, 1, 1, 1};
    put_pointer(team_setting_behavior, 0x1b0, team_available);
    put_pointer(team_available, 0x20, team_choices);
    *(int *)(team_available + 12) = 4;
    *(int *)(native_settings + 0x28) = 4;
    void (ABI *team_setting_event)(void *, void *, void *) = *(void **)(image + 0x2778150);
    team_setting_event(team_setting_behavior, &event, NULL);
    assert(*(int *)(native_settings + 0x28) == 1);
    assert(team_choices[0] && !team_choices[1] && !team_choices[2] && !team_choices[3]);

    memcpy(state + 0x13c, &normal, 4);
    private = 0;
    event.index = -1;
    handle(self, &event, mode_row);
    memcpy(&visible, race_row + 12, 4);
    assert((visible & 0x400) && !runtime_online_mode_private_cup());
    private = 1;
    event.index = 0;
    handle(race, &event, race_row);
    assert(!runtime_online_mode_private_cup());
    *(int *)(native_settings + 0x28) = 3;
    team_setting_event(team_setting_behavior, &event, NULL);
    assert(*(int *)(native_settings + 0x28) == 2);
    assert(*(int *)(team_setting_behavior + 0x190) == 1);
    assert(team_choices[0] && team_choices[1] && !team_choices[2] && team_choices[3]);
    unsigned char blocked_team_event[0x20] = {0};
    put_pointer(blocked_team_event, 0, vtable);
    *(int *)(blocked_team_event + 0x18) = 2;
    put_pointer(image, 0x2ae4a38, (void *)456);
    const int before_blocked_team = base_calls;
    team_setting_event(team_setting_behavior, blocked_team_event, NULL);
    assert(base_calls == before_blocked_team && *(int *)(native_settings + 0x28) == 2);
    *(int *)(blocked_team_event + 0x18) = 3;
    team_setting_event(team_setting_behavior, blocked_team_event, NULL);
    assert(base_calls == before_blocked_team + 1);
    put_pointer(image, 0x2ae4a38, (void *)999);

    put_pointer(members_handle, 0x28, members_project);
    put_pointer(members_project, 0x10, members_root);
    *(unsigned *)members_handle = 8;
    *(unsigned *)(members_root + 12) = 0;
    void *members = NULL;
    open_race_ui_project(&members, NULL, "Octane_LobbyMembers", NULL, -1, NULL, 0);
    assert(!(*(unsigned *)(members_root + 12) & 0x400));
    expected_ranking = "Octane_TeamSelect";
    *race_mode = 1;
    controls_enabled = 0;
    void *selector_project = NULL;
    open_race_ui_project(&selector_project, NULL, "Octane_TeamSelect", NULL, -1, NULL, 0);
    assert(controls_enabled == 0); /* Streaming/open is too early to enable. */
    assert(*(unsigned *)(members_root + 12) & 0x400);
    open_race_ui_project(&members, NULL, "Octane_LobbyMembers", NULL, -1, NULL, 0);
    assert(*(unsigned *)(members_root + 12) & 0x400); /* Reopen stays hidden. */
    unsigned char selector_behavior[0xa0] = {0}, selector_owner[0x28] = {0};
    unsigned char native_selector_project[0x50] = {0};
    unsigned char native_selector_root[0x20] = {0};
    put_pointer(native_selector_project, 0x10, native_selector_root);
    *(int *)(native_selector_project + 0x48) = 3;
    put_pointer(selector_owner, 0x20, native_selector_project);
    void (ABI *selector_event)(void *, void *, void *) = *(void **)(image + 0x2543d68);
    event.index = -1;
    private = 0; /* The latch survives lobby-component teardown during open. */
    selector_event(selector_behavior, &event, selector_owner);
    assert(controls_enabled == 1);
    assert(*(int *)(native_selector_project + 0x48) == 0);
    assert(native_selector_project[0x34] & 0x20);
    assert(*(unsigned *)(native_selector_project + 0x44) == 0xff);
    /* Cancel never leaves private Single; ready Cancel still reaches native Unready. */
    put_pointer(image, 0x2a21d00, (void *)44);
    direct_stub(image + 0x9c32c0, selector_ready_find_mock);
    unsigned char selector_player[0x100] = {0}, selector_manager[0x18] = {0};
    put_pointer(selector_manager, 0x10, selector_player);
    put_pointer(image, 0x2ad1160, selector_manager);
    local_player_mock = selector_player;
    unsigned char legend_manager[0x100] = {0}, remove_legend_event[0x28] = {0};
    void *legend_vtable[24] = {0}; legend_vtable[0xb0 / 8] = legend_dispatch_mock;
    put_pointer(native_selector_project, 0, legend_vtable);
    put_pointer(legend_manager, 0xf0, remove_legend_event);
    put_pointer(image, 0x2ac0dc0, legend_manager);
    direct_stub(image + 0xa09d30, legend_add_mock);
    legend_adds = 0; legend_buttons[1] = legend_buttons[2] = 0;

    unsigned char input_event[0x28] = {0}; void *input_vtable[] = {panel_event_type};
    put_pointer(input_event, 0, input_vtable); put_pointer(input_event, 8, (void *)44);
    *(int *)(input_event + 0x18) = 9; *(int *)(input_event + 0x1c) = 1;
    selector_ready = 0; selector_ready_found = 1;
    int before_input = selector_native_events;
    selector_event(selector_behavior, input_event, selector_owner);
    assert(selector_native_events == before_input);
    assert(legend_buttons[1] && !legend_buttons[2] && !strcmp(current_legend, "READY"));
    assert(legend_adds == 1);
    selector_ready = 1;
    selector_event(selector_behavior, input_event, selector_owner);
    assert(selector_native_events == ++before_input);
    assert(!legend_buttons[1] && legend_buttons[2] && !strcmp(current_legend, "UNREADY"));
    assert(legend_adds == 2);
    selector_ready_found = 0;
    selector_event(selector_behavior, input_event, selector_owner);
    assert(selector_native_events == before_input);
    selector_ready_found = 1; selector_ready = 0;
    *(int *)(input_event + 0x18) = 8;
    selector_event(selector_behavior, input_event, selector_owner);
    assert(selector_native_events == ++before_input); /* Ready remains native. */
    unsigned char selector_roster[40] = {0}, selector_peers[8][0x100] = {{0}};
    void *selector_players[8];
    for (int i = 0; i < 8; ++i) selector_players[i] = selector_peers[i];
    put_pointer(selector_manager, 0x10, selector_roster);
    put_pointer(selector_roster, 0x20, selector_players);
    *(int *)(selector_roster + 12) = 8;
    const int directions[] = {2, 3, 6, 7};
    for (int teams = 2; teams <= 4; teams += 2) {
        *(int *)(selector_behavior + 0x7c) = teams;
        *(int *)(selector_behavior + 0x80) = 8 / teams;
        for (int i = 0; i < 8; ++i) *(int *)(selector_peers[i] + 0xe4) = i % teams;
        for (int slot = 0; slot < 8; ++slot) {
            local_player_mock = selector_peers[slot];
            for (int d = 0; d < 4; ++d) {
                *(int *)(input_event + 0x18) = directions[d];
                for (int repeat = 0; repeat < 3; ++repeat)
                    selector_event(selector_behavior, input_event, selector_owner);
                assert(selector_native_events == before_input); /* Full lobby never enters native search. */
            }
        }
        local_player_mock = selector_peers[0];
        *(int *)(selector_roster + 12) = 7; /* Last team now has a vacancy. */
        selector_event(selector_behavior, input_event, selector_owner);
        assert(selector_native_events == ++before_input);
        *(int *)(selector_roster + 12) = 8;
        *(int *)(input_event + 0x18) = 8;
        selector_event(selector_behavior, input_event, selector_owner);
        assert(selector_native_events == ++before_input); /* Full lobby can still Ready. */
    }
    local_player_mock = selector_player;
    *race_mode = 9; *(int *)(input_event + 0x18) = 9;
    selector_event(selector_behavior, input_event, selector_owner);
    assert(selector_native_events == ++before_input); /* Battle keeps native Back. */
    *race_mode = 1;
    put_pointer(image, 0x2ad1160, NULL); local_player_mock = NULL;
    put_pointer(image, 0x2ac0dc0, NULL);
    ready_target = native_selector_project; ready_behavior = selector_behavior; ready_owner = selector_owner;
    ready_handler = selector_event; ready_project_events = broadcast_routes = 0;
    unsigned char ready_event[0x28] = {0}; void *ready_vtable[] = {panel_event_type};
    put_pointer(ready_event, 0, ready_vtable); put_pointer(ready_event, 8, (void *)42);
    void (ABI *broadcast)(void *, void *) = (void *)(image + 0x248d10);
    broadcast(NULL, ready_event); assert(ready_project_events == 1); /* Missed native GUI layer. */
    broadcast_routes = 1;
    broadcast(NULL, ready_event); assert(ready_project_events == 2); /* Already delivered: no duplicate. */
    broadcast_routes = 0; put_pointer(ready_event, 8, (void *)43);
    broadcast(NULL, ready_event); assert(ready_project_events == 3); /* Unready uses the same route. */
    put_pointer(ready_event, 8, (void *)99);
    broadcast(NULL, ready_event); assert(ready_project_events == 3);
    unsigned char disable_pins[24] = {0};
    void (ABI *disable_node)(void *, void *) = (void *)(image + (0x110b9c0 - shift));
    void (ABI *legend_node)(void *, void *) = (void *)(image + (0x110bbc0 - shift));
    int legend_before = disable_node_calls;
    *(int *)disable_pins = 5;
    legend_node(NULL, disable_pins);
    assert(disable_node_calls == legend_before); /* Private Back is managed dynamically. */
    *(int *)disable_pins = 6;
    legend_node(NULL, disable_pins);
    assert(disable_node_calls == legend_before); /* Static Ready also suppressed. */
    *(int *)disable_pins = 4;
    legend_node(NULL, disable_pins);
    assert(disable_node_calls == ++legend_before); /* Ready retained. */
    *race_mode = 9;
    *(int *)disable_pins = 5;
    legend_node(NULL, disable_pins);
    assert(disable_node_calls == ++legend_before); /* Battle Back retained. */
    *race_mode = 1;
    const int disable_before = disable_node_calls;
    *(int *)disable_pins = 3;
    disable_node(NULL, disable_pins);
    assert(disable_node_calls == disable_before); /* Open callback cannot relock private selector. */
    *(int *)disable_pins = 2;
    disable_node(NULL, disable_pins);
    assert(disable_node_calls == disable_before + 1); /* Other native disables stay intact. */
    *(int *)disable_pins = 3;
    runtime_online_mode_leave_cup();
    put_pointer(ready_event, 8, (void *)42); broadcast(NULL, ready_event);
    assert(ready_project_events == 3); /* No stale project dispatch after leaving. */
    assert(!(*(unsigned *)(members_root + 12) & 0x400)); /* Restored with lobby. */
    assert(!(native_selector_project[0x34] & 0x20));
    assert(*(unsigned *)(native_selector_root + 12) & 0x400);
    assert(*(int *)(native_settings + 0x28) == 1 && *(int *)(private_settings + 0x28) == 1);
    /* Replay delayed opens after leaving Teams into every mode, with stale team settings. */
    for (int room = 0; room <= 1; ++room)
        for (int mode = 0; mode <= 15; ++mode)
            for (int teams = 1; teams <= 4; ++teams) {
                runtime_online_mode_leave_cup();
                private = room; *race_mode = mode;
                *(int *)(native_settings + 0x28) = teams;
                void *next_selector = NULL;
                open_race_ui_project(&next_selector, NULL, "Octane_TeamSelect", NULL, -1, NULL, 0);
                const int allowed = (room && mode == 1 && teams > 1) ||
                    (mode >= 9 && mode <= 13 && teams > 1);
                assert(!!next_selector == allowed);
                if (!allowed) {
                    int events_before = selector_native_events;
                    selector_event(selector_behavior, input_event, selector_owner);
                    if (!(mode >= 9 && mode <= 13)) assert(selector_native_events == events_before);
                }
            }
    runtime_online_mode_leave_cup();
    private = 1; *race_mode = 1; public_active = 1;
    *(int *)(native_settings + 0x28) = 4;
    void *public_selector = NULL;
    open_race_ui_project(&public_selector, NULL, "Octane_TeamSelect", NULL, -1, NULL, 0);
    assert(!public_selector && *(int *)(native_settings + 0x28) == 1);
    public_active = 0;
    private = 1; public_normal_active = 1;
    *(int *)(native_settings + 0x28) = 4;
    open_race_ui_project(&public_selector, NULL, "Octane_TeamSelect", NULL, -1, NULL, 0);
    assert(!public_selector && *(int *)(native_settings + 0x28) == 1);
    assert(!runtime_online_mode_in_private() && !runtime_online_mode_private_cup());
    public_normal_active = 0;
    runtime_online_mode_leave_cup();
    private = 0; *race_mode = 1;
    ready_target = NULL; ready_handler = NULL;
    disable_node(NULL, disable_pins);
    assert(disable_node_calls == disable_before + 2); /* Outside private selector. */
    *(int *)(native_selector_project + 0x48) = 3;
    private = 0;
    open_race_ui_project(&selector_project, NULL, "Octane_TeamSelect", NULL, -1, NULL, 0);
    selector_event(selector_behavior, &event, selector_owner);
    assert(controls_enabled == 1);
    assert(*(int *)(native_selector_project + 0x48) == 3);
    private = 1;
    *race_mode = 9;
    *(int *)(native_settings + 0x28) = 2; /* Explicit native Battle teams still work. */
    open_race_ui_project(&selector_project, NULL, "Octane_TeamSelect", NULL, -1, NULL, 0);
    assert(controls_enabled == 1);
    disable_node(NULL, disable_pins);
    assert(disable_node_calls == disable_before + 3); /* Battle keeps its native callback. */
    /* Final host order produces team points once, and only in private Single. */
    *(int *)(manager + 0xd4c) = *race_mode = 1;
    *(int *)(native_settings + 0x28) = 2;
    expected_ranking = "Octane_BattleTeam_Ranking";
    unsigned char countdown[0x40] = {0}, config[0x130] = {0};
    timer_config = config;
    put_pointer(state, 0x40, config);
    *(float *)(config + 0x110) = 10.f;
    *(float *)(config + 0x118) = 5.f;
    *(float *)(config + 0x114) = 10.f;
    *(float *)(config + 0x11c) = 30.f;
    void (ABI *init_countdown)(void *) = (void *)(image + (0xeb71d0 - shift));
    init_countdown(countdown);
    assert(timer_ui_initial == 30.f && *(float *)(countdown + 0x28) == 30.f);
    assert(*(float *)(countdown + 0x30) == 30.f); /* Prepare peers on first tick, not 20s later. */
    assert(*(float *)(config + 0x110) == 10.f && *(float *)(config + 0x118) == 5.f);
    void (ABI *tick_countdown)(void *) = (void *)(image + (0xeb7950 - shift));
    *(float *)(countdown + 0x28) = 30.0f;
    *(float *)(countdown + 0x2c) = 10.0f;
    tick_countdown(countdown);
    assert(*(float *)(countdown + 0x2c) == 30.0f && *(float *)(countdown + 0x28) == 30.0f);
    *(float *)(countdown + 0x2c) = -1.0f;
    *(float *)(countdown + 0x28) = 5.0f; /* Includes the native all-ready fast start. */
    tick_countdown(countdown);
    assert(*(float *)(countdown + 0x2c) == -1.0f && *(float *)(countdown + 0x28) == 5.0f);
    unsigned char countdown_owner[0x40] = {0};
    *(void **)(countdown + 0x18) = countdown_owner;
    *(float *)(countdown + 0x28) = 30.f; /* Replicated host value before guest UI creation. */
    init_countdown(countdown);
    assert(timer_ui_initial == 30.f && *(float *)(countdown + 0x30) == 30.f);
    assert(*(float *)(config + 0x114) == 10.f);
    *(float *)(countdown + 0x28) = 22.0f;
    *(float *)(countdown + 0x2c) = 10.0f;
    tick_countdown(countdown);
    assert(*(float *)(countdown + 0x28) == 22.0f && *(float *)(countdown + 0x2c) == 22.0f);
    *(void **)(countdown + 0x18) = NULL;
    *(int *)(native_settings + 0x28) = 1;
    init_countdown(countdown);
    assert(timer_ui_initial == 10.f); /* Private solo retains its native duration. */
    *(int *)(native_settings + 0x28) = 2;
    private = 0;
    init_countdown(countdown);
    assert(timer_ui_initial == 30.f); /* Public retains its native duration. */
    *(float *)(countdown + 0x2c) = 10.0f;
    tick_countdown(countdown);
    assert(*(float *)(countdown + 0x2c) == 10.0f);
    private = 1;
    *(int *)(native_settings + 0x28) = 1;
    load_map("solo-test-track", 8);
    assert(image[0xf34575 - shift] == 0xff); /* Solo includes AI finishers too. */
    individual_sounds = 0;
    void (ABI *finish_confetti)(void *) = (void *)(image + (0xf382f0 - shift));
    void (ABI *finish_sound)(void *, void *) = (void *)(image + (0x1093ca0 - shift));
    unsigned char finish_pins[0x28] = {0};
    *(int *)finish_pins = 5;
    int confetti_before = confetti_calls;
    finish_confetti(racers[0]);
    finish_sound(NULL, finish_pins);
    assert(confetti_calls == confetti_before + 1 && individual_sounds == 1); /* Solo. */
    *(int *)(native_settings + 0x28) = 2;
    load_map("team-test-track", 8);
    confetti_before = confetti_calls;
    finish_confetti(racers[0]);
    finish_sound(NULL, finish_pins);
    assert(confetti_calls == confetti_before && individual_sounds == 1);
    *(int *)finish_pins = 4;
    finish_sound(NULL, finish_pins);
    assert(individual_sounds == 2); /* Other sound nodes remain native. */
    assert(image[0xf34575 - shift] == 0xff); /* An AI finisher starts the native timer too. */
    unsigned char damage_component[0x20] = {0}, victim[0x80] = {0}, attacker[0x80] = {0};
    unsigned char attacker_handle[0x30] = {0};
    put_pointer(damage_component, 0x18, victim);
    put_pointer(attacker_handle, 0x28, attacker);
    void *source_handle = attacker_handle;
    int (ABI *damage)(void *, unsigned, void **, unsigned char) = (void *)(image + 0xe330e0);
    *(int *)(native_settings + 0x28) = 2;
    *(unsigned *)(victim + 0x78) = 1u << 22;
    *(unsigned *)(attacker + 0x78) = 1u << 22;
    unsigned char clock_component[0x30] = {0};
    put_pointer(clock_component, 0x28, attacker_handle);
    void (ABI *clock_apply)(void *, void *, void *) = (void *)(image + (0xf1fb50 - shift));
    clock_calls = 0;
    clock_apply(clock_component, victim, NULL);
    assert(clock_calls == 0); /* No teammate slow effect or hit message. */
    damage_calls = 0;
    for (unsigned kind = 0; kind < 10; ++kind) assert(damage(damage_component, kind, &source_handle, 0) == 1);
    assert(damage_calls == 0);
    *(unsigned *)(attacker + 0x78) = 1u << 23;
    assert(damage(damage_component, 0, &source_handle, 0) == 0 && damage_calls == 1);
    clock_apply(clock_component, victim, NULL);
    assert(clock_calls == 1);
    *(unsigned *)(attacker + 0x78) = 0; /* Unknown faction is not a teammate. */
    assert(damage(damage_component, 0, &source_handle, 0) == 0 && damage_calls == 2);
    assert(damage(damage_component, 0, NULL, 0) == 0 && damage_calls == 3);
    put_pointer(attacker_handle, 0x28, victim); /* Own traps remain native. */
    assert(damage(damage_component, 0, &source_handle, 0) == 0 && damage_calls == 4);
    put_pointer(attacker_handle, 0x28, attacker);
    *(unsigned *)(attacker + 0x78) = 1u << 22;
    *(int *)(native_settings + 0x28) = 1;
    assert(damage(damage_component, 0, &source_handle, 0) == 0 && damage_calls == 5);
    *(int *)(native_settings + 0x28) = 4;
    *(unsigned *)(victim + 0x78) = *(unsigned *)(attacker + 0x78) = 1u << 25;
    assert(damage(damage_component, 0, &source_handle, 0) == 1 && damage_calls == 5);
    *(int *)(native_settings + 0x28) = 2;

    speed = 0;
    private = 0; /* Native lobby component no longer exists on the track. */
    assert(runtime_online_mode_in_private());
    unsigned char score_root[0x120] = {0}, score_position[0x120] = {0}, score_text[0x120] = {0};
    unsigned char lap_lists[8][40] = {{0}};
    int lap_times[8][3] = {{0}};
    *(int *)(native_settings + 12) = 3;
    for (int i = 0; i < 8; ++i) {
        put_pointer(racers[i], 0x90, lap_lists[i]);
        put_pointer(lap_lists[i], 0x20, lap_times[i]);
        *(int *)(lap_lists[i] + 12) = 3;
        lap_times[i][2] = 90000;
    }
    int (ABI *race_order)(unsigned char *, unsigned char *) = (void *)(image + (0xff11a0 - shift));
    /* Before host results: valid times beat stale positions in every requested mode. */
    int old_position[8];
    for (int i = 0; i < 8; ++i) {
        old_position[i] = *(int *)(racers[i] + 0x60);
        *(int *)(racers[i] + 0x60) = i == 1 ? 8 : i + 1;
        racers[i][0x8c] = 1;
        lap_times[i][2] += i * 1000;
    }
    for (int kind = 0; kind < 6; ++kind) {
        speed = kind == 1;
        public_active = kind == 2;
        public_normal_active = kind == 4;
        *(int *)(native_settings + 0x28) = kind == 0 ? 2 : kind == 3 ? 4 : 1;
        assert(race_order(racers[1], racers[2]));
        assert(!race_order(racers[2], racers[1]));
        *(int *)(lap_lists[0] + 12) = 2;
        assert(race_order(racers[1], racers[0]));
        assert(!race_order(racers[0], racers[1]));
        *(int *)(lap_lists[0] + 12) = 3;
        lap_times[0][2] = -1;
        assert(race_order(racers[1], racers[0]));
        lap_times[0][2] = lap_times[1][2];
        int before = race_order_calls;
        assert(race_order(racers[0], racers[1]));
        assert(race_order_calls == before + 1); /* Native tie rule. */
        lap_times[0][2] = 90000;
    }
    speed = public_active = public_normal_active = 0;
    *(int *)(native_settings + 0x28) = 1;
    assert(race_order(racers[1], racers[2])); /* Normal solo also ignores stale finish positions. */
    *(int *)(native_settings + 0x28) = 2;
    for (int i = 0; i < 8; ++i) {
        *(int *)(racers[i] + 0x60) = old_position[i];
        racers[i][0x8c] = 0;
        lap_times[i][2] = 90000;
    }
    unsigned char position_asset[0x60] = {0}, score_asset[0x60] = {0};
    put_pointer(score_text, 0x38, score_asset);
    put_pointer(score_position, 0x38, position_asset);
    unsigned char score_children[40] = {0}, team_results[0x40] = {0}, winner_text[0x120] = {0};
    unsigned char points_text[0x120] = {0};
    put_pointer(points_text, 0x28, "OCTR_TEAM_POINTS");
    unsigned char wins_text[0x120] = {0};
    put_pointer(wins_text, 0x28, "[WinsText]");
    unsigned char leader_position[0x120] = {0};
    *(unsigned *)leaders_handle = 4;
    put_pointer(leaders_handle, 0x28, leaders_project);
    put_pointer(leaders_project, 0x10, leader_position);
    put_pointer(leader_position, 0x28, "[PositionText]");
    put_pointer(leader_position, 0x30, make_string(NULL, "1"));
    unsigned char score_name[0x120] = {0}, score_background[0x120] = {0}, score_base[0x120] = {0};
    const float native_base[] = {.2f, .3f, .4f, 1.f}, native_highlight[] = {1.f, 1.f, 1.f, 1.f};
    memcpy(score_base + 0xc0, native_base, sizeof native_base);
    memcpy(score_background + 0xc0, native_highlight, sizeof native_highlight);
    put_pointer(score_background, 0x40, score_base);
    put_pointer(score_name, 0x28, "[PlayerTagText]");
    put_pointer(score_name, 0x30, make_string(NULL, "Crash"));
    unsigned char list_parent[0x120]={0};
    unsigned char list_placeable[0x120]={0}, list_asset[0xa0]={0}, list_handle[0x30]={0}, list_template[0x120]={0};
    put_pointer(list_placeable,0x28,"OCTR_TEAM_LIST");
    put_pointer(list_placeable,0x40,list_parent);
    *(float *)(list_parent+0x64)=303;
    put_pointer(list_placeable,0x38,list_handle); put_pointer(list_handle,0x28,list_asset);
    put_pointer(list_asset,0x20,list_template);
    *(float *)(list_template+0x94)=86; *(int *)(list_asset+0x90)=4;
    void *score_items[] = {score_position, score_text, winner_text, score_name, wins_text, points_text, list_placeable};
    put_pointer(composition, 0x20, score_root);
    put_pointer(ranking_project, 0x10, composition_root);
    put_pointer(score_root, 0x48, score_children);
    put_pointer(score_children, 0x20, score_items);
    *(int *)(score_children + 12) = 7;
    put_pointer(winner_text, 0x28, "[WinnerNameText]");
    put_pointer(score_position, 0x28, "[PositionText]");
    put_pointer(score_position, 0x30, make_string(NULL, "1"));
    put_pointer(score_text, 0x28, "[BattleScore]");
    for (int i = 0; i < 8; ++i) {
        racer_items[i] = racers[i];
        put_pointer(racers[i], 0x10, handles[i]);
        *(int *)(players[i] + 0xc8) = i;
        *(int *)(players[i] + 0xe4) = i % 2;
        *(int *)(racers[i] + 0x60) = i + 1;
        *(int *)(racers[i] + 0x58) = i + 1;
        racers[i][0x8c] = 0;
        *(int *)(team_results + 0x20 + i * 4) = i;
    }
    unsigned char row_behavior[0x100] = {0}, bound_racer[0x30] = {0};
    const unsigned char native_time_color[] = {1, 74, 132, 255};
    memcpy(row_behavior + 0xda, native_time_color, sizeof native_time_color);
    unsigned char score_avatar[0x120] = {0}, racer_row[0x120] = {0};
    put_pointer(score_avatar,0x40,racer_row);
    put_pointer(score_name,0x40,racer_row);
    put_pointer(score_avatar, 0x28, "OCTR_TEAM_AVATAR");
    *(unsigned *)(score_avatar + 12) = 0x400;
    put_pointer(row_behavior, 0x98, score_avatar);
    unsigned char score_engine[0x120]={0};
    put_pointer(score_engine,0x28,"OCTR_TEAM_ENGINE");
    *(unsigned *)(score_engine+12)=0x400;
    for(int c=0;c<4;++c) *(float *)(score_engine+0xc0+c*4)=1;
    put_pointer(row_behavior,0xa0,score_engine);
    unsigned char row_behaviors[40] = {0};
    void *behaviors[] = {row_behavior};
    put_pointer(row_behavior, 0, image + 0x2543600);
    put_pointer(row_behavior, 0x78, bound_racer);
    put_pointer(row_behavior, 0xa8, score_name);
    put_pointer(row_behavior, 0xc0, score_background);
    put_pointer(row_behavior, 0xb8, score_position);
    put_pointer(row_behavior, 0xd0, score_text);
    put_pointer(score_root, 0xe8, row_behaviors);
    *(int *)(row_behaviors + 12) = 1;
    put_pointer(row_behaviors, 0x20, behaviors);
    void (ABI *row_event)(void *, void *, void *) = *(void **)(image + 0x25436c8);
    put_pointer(image, 0x2ac0dc0, image); /* Non-null native manager fixture. */
    initialize_race_ui(race_ui_owner);
    assert(*(void **)(race_ui_owner + 0x60) == panel_handle && team_score_opens == 1);
    assert(*(void **)(race_ui_owner + 0x68) == ranking_handle); /* Prepared before intro; finish slot unchanged. */
    unsigned char branch_pins[24] = {0}, branch_outputs[40] = {0};
    void *branch_nodes[] = {(void *)101, (void *)102};
    put_pointer(branch_outputs, 0x20, branch_nodes);
    void (ABI *network_branch)(void *, void *, void *) = (void *)(image + (0x115a740 - shift));
    *(int *)branch_pins = 1;
    network_branch(NULL, branch_pins, branch_outputs);
    assert(last_graph_node == branch_nodes[1]); /* Manual private team scoreboard. */
    *(int *)branch_pins = 0;
    network_branch(NULL, branch_pins, branch_outputs);
    assert(last_graph_node == branch_nodes[0]); /* Other online graphs stay online. */
    network_query_value = 0;
    network_branch(NULL, branch_pins, branch_outputs);
    assert(last_graph_node == branch_nodes[1]);
    network_query_value = 1;
    put_pointer(bound_racer, 0x28, racers[0]);
    int finish_events_before = panel_events;
    project_event(panel_project, panel_event);
    assert(panel_events == finish_events_before + 1); /* Native panel receives its own event exactly once. */
    row_event(row_behavior, NULL, NULL);
    assert(*(float *)(score_name + 0xc0) == 1.0f); /* Names stay white before final results. */
    unsigned char local_manager[0x20] = {0};
    put_pointer(local_manager, 0x10, players);
    put_pointer(image, 0x2ad1160, local_manager);
    local_player_mock = players[0];
    unsigned char result_kart[8] = {0}, result_vtable[0x118] = {0}, result_handle[0x30] = {0};
    put_pointer(result_vtable, 0x110, result_animation_mock);
    put_pointer(result_kart, 0, result_vtable);
    put_pointer(result_handle, 0x28, result_kart);
    put_pointer(racers[0], 0x20, result_handle);
    animation_calls = confetti_calls = result_sounds = released_result_sounds = 0;
    /* Native RaceFinished arrives before the final host snapshot. */
    put_pointer(panel_event, 8, (void *)12);
    int score_events_before = ranking_events;
    finish_events_before = panel_events;
    project_event(panel_project, panel_event);
    assert(team_score_opens == 1 && ranking_events == score_events_before + 1);
    assert(panel_events == finish_events_before); /* Old graph must not start Wumpa flow. */
    project_event(panel_project, panel_event);
    assert(panel_events == finish_events_before && ranking_events == score_events_before + 1);

    project_event(ranking_project, panel_event);
    assert(ranking_events == score_events_before + 1); /* No duplicate animation if broadcast also visits it. */
    assert(animation_calls == 0 && confetti_calls == 0); /* Wait for host results. */
    put_pointer(panel_event, 8, (void *)11);
    /* Native local-player lookup can differ from network slot 0. DNF still wins with its team. */
    local_controller_id = 7;
    assert(!racers[0][0x8c]);
    expected_animations_at_result = animation_calls;
    receive_results(team_results);
    expected_animations_at_result = -1; /* Winner effects wait until native status has settled. */
    assert(animation_calls == 1 && animation_selection == 0 && confetti_calls == 1);
    assert(result_sounds == 1 && released_result_sounds == 1 && result_sound_won);
    local_controller_id = 0;
    assert(team_score_opens == 1 && !*(void **)(race_ui_owner + 0x60) &&
           *(void **)(race_ui_owner + 0x68) == ranking_handle);
    assert(!strcmp(*(char **)(winner_text + 0x30), "BLUE TEAM WINS"));
    assert(!strcmp(*(char **)(wins_text + 0x30), "WINS"));
    assert(!strcmp(*(char **)(points_text + 0x30), "22 POINTS"));
    assert(podium_slots[0] == racers[0] && podium_slots[1] == racers[1] && podium_slots[2] == racers[2]);
    local_player_mock = players[1];
    project_event(ranking_project, panel_event);
    assert(!strcmp(*(char **)(wins_text + 0x30), "LOSE"));
    assert(!strcmp(*(char **)(winner_text + 0x30), "BLUE TEAM WINS"));
    assert(!strcmp(*(char **)(points_text + 0x30), "22 POINTS"));
    local_player_mock = players[0];
    handle_timer(NULL, &event, timer_owner);
    assert(*(float *)(timer_owner + 0x60) == 1050.0f);
    assert(*(unsigned *)(timer_owner + 12) & 0x1000);
    assert(*(float *)(wins_text + 0x64) == -19.0f);
    assert(*(float *)(winner_text + 0x64) == -4.0f);
    assert(*(float *)(points_text + 0x64) == 9.0f);
    /* All eight bound identities, in reverse GUI order, including last place.
     * Deliberately wrong display text must never decide the row's identity. */
    const char *awards[] = {"10 POINT", "8 POINT", "6 POINT", "5 POINT", "4 POINT", "3 POINT", "2 POINT", "1 POINT"};
    for (int i = 7; i >= 0; --i) {
        put_pointer(bound_racer, 0x28, racers[i]);
        row_behavior[0xd8] = 1;
        memset(score_text + 0xc0, 0, 16); /* Native DNF dark text. */
        *(int *)(score_asset + 0x40) = 1;
        *(float *)(score_text + 0xe0) = .5f;
        row_event(row_behavior, NULL, NULL);
        for (int c = 0; c < 4; ++c) assert(*(float *)(score_text + 0xc0 + c * 4) == 1.f);
        assert(*(int *)(score_asset + 0x40) == 0);
        assert(*(float *)(score_text + 0xe0) == 1.f);
        assert(*(float *)(racer_row+0xe0)==0.6f && (*(unsigned *)(racer_row+12)&0x1000));
        assert(*(float *)(score_background + 0xe0) == 0.f);
        racers[i][0x8c]=1;
        row_event(row_behavior,NULL,NULL);
        assert(*(float *)(racer_row+0xe0)==0.6f); /* Late finishes cannot upgrade the final snapshot. */
        assert(*(float *)(score_background + 0xe0) == 0.f);
        for (int c = 0; c < 4; ++c) {
            const float expected = 1.f;
            assert(*(float *)(score_text + 0xc0 + c*4) == expected);
            assert(*(float *)(score_name + 0xc0 + c*4) == expected);
            assert(*(float *)(score_engine + 0xc0 + c*4) == expected);
        }
        *(unsigned *)(score_background + 12) |= 0x400;
        row_event(row_behavior, NULL, NULL);
        assert(*(float *)(score_text + 0xc0) == 1.f); /* White on other finished rows. */
        assert(*(unsigned *)(score_background + 12) & 0x400);
        *(unsigned *)(score_background + 12) &= ~0x400u;
        racers[i][0x8c]=0;
        row_event(row_behavior,NULL,NULL);
        assert(*(float *)(racer_row+0xe0)==0.6f);
        assert(!(*(unsigned *)(score_avatar + 12) & 0x400));
        assert(!(*(unsigned *)(score_engine + 12) & 0x400));
        assert(*(float *)(score_name+0x60)==158);
        assert(*(float *)(score_name+0x90)==185);
        for(int c=0;c<4;++c) assert(*(float *)(score_engine+0xc0+c*4)==1);
        assert(*(float *)(list_template+0x94)==86 && *(int *)(list_asset+0x90)==8);
        assert(*(float *)(list_placeable+0x64)==-7);
        assert(*(float *)(list_parent+0x64)==260 && (*(unsigned *)(list_parent+12)&0x1000));
        assert(*(float *)(score_text+0x84)==.75f && *(float *)(score_text+0x88)==.75f);
        assert(!strcmp(*(char **)(score_text + 0x30), awards[i]));
        assert(atoi(*(char **)(score_position + 0x30)) == i + 1);
        assert(!strcmp(*(char **)(score_name + 0x30), "Crash"));
        assert(*(float *)(score_name + 0xc0) == 1.0f);
        const float red = i % 2 ? 1.0f : 0.1f;
        assert(!memcmp(score_background + 0xc0, native_highlight, sizeof native_highlight));
        assert(!memcmp(score_base + 0xc0, native_base, sizeof native_base));
        assert(*(float *)(score_position + 0xc0) == red);
        assert(*(int *)(position_asset + 0x40) == 0);
    }
    receive_results(team_results); /* Duplicate delivery doesn't accumulate. */
    assert(animation_calls == 1 && confetti_calls == 1);
    assert(!strcmp(*(char **)(score_text + 0x30), "10 POINT"));
    *(int *)(team_results + 0x24) = 0;
    receive_results(team_results); /* Malformed order retains last valid snapshot. */
    assert(!strcmp(*(char **)(score_text + 0x30), "10 POINT"));
    *(int *)(team_results + 0x24) = 1;
    /* Scores retain the accepted host order despite stale native positions. */
    int saved_positions[8];
    for (int i = 0; i < 8; ++i) {
        saved_positions[i] = *(int *)(racers[i] + 0x60);
        *(int *)(racers[i] + 0x60) = i == 1 ? 8 : i + 1;
        racers[i][0x8c] = 1;
        lap_times[i][2] = 90000 + i * 1000;
    }
    for (speed = 0; speed <= 1; ++speed) {
        unsigned char *ordered[8];
        for (int i = 0; i < 8; ++i) ordered[i] = racers[7 - i];
        for (int i = 1; i < 8; ++i) {
            unsigned char *racer = ordered[i];
            int j = i;
            while (j && race_order(racer, ordered[j - 1])) {
                ordered[j] = ordered[j - 1];
                --j;
            }
            ordered[j] = racer;
        }
        for (int i = 0; i < 8; ++i) {
            assert(ordered[i] == racers[i]);
            *(int *)(team_results + 0x20 + i * 4) = *(int *)(players[i] + 0xc8);
        }
        put_pointer(bound_racer, 0x28, racers[1]);
        receive_results(team_results);
        receive_results(team_results);
        assert(!strcmp(*(char **)(score_text + 0x30), "8 POINT"));
        assert(!strcmp(*(char **)(score_position + 0x30), "2"));
        assert(!strcmp(*(char **)(points_text + 0x30), "22 POINTS"));
    }
    /* Late human and AI callbacks must preserve host ranks in every race variant. */
    *(int *)(ai_selections + 12) = 8;
    for (int kind = 0; kind < 6; ++kind) {
        speed = kind == 1 || kind == 3;
        public_active = kind == 3;
        public_normal_active = kind == 2;
        *(int *)(native_settings + 0x28) = kind == 4 ? 2 : kind == 5 ? 4 : 1;
        for (int ai = 0; ai < 2; ++ai) {
            void *saved_handle = *(void **)(racers[1] + 0x10);
            *(int *)(racers[1] + 0x164) = *(int *)(players[1] + 0xc8);
            if (ai) {
                put_pointer(racers[1], 0x10, NULL);
                *(int *)(team_results + 0x24) = *(int *)(network_ai[1] + 0x58);
            }
            receive_results(team_results);
            racers[1][0xb0] = 1;
            racers[1][0x8c] = 0;
            *(int *)(racers[1] + 0x58) = 8;
            *(int *)(racers[1] + 0x60) = 8;
            *(int *)(racers[1] + 0x64) = 8;
            void (ABI *late_finish)(unsigned char *, int) = (void *)(image + (0xff2320 - shift));
            late_finish(racers[1], 1);
            assert(*(int *)(racers[1] + 0x58) == 2);
            assert(*(int *)(racers[1] + 0x60) == 2);
            assert(*(int *)(racers[1] + 0x64) == 2);
            put_pointer(racers[1], 0x10, saved_handle);
            *(int *)(team_results + 0x24) = *(int *)(players[1] + 0xc8);
        }
    }
    public_active = public_normal_active = 0;
    *(int *)(ai_selections + 12) = 0;
    *(int *)(native_settings + 0x28) = 2;
    receive_results(team_results);
    speed = 0;
    lap_times[0][2] = -1;
    assert(race_order(racers[0], racers[1])); /* Accepted host order overrides late local data. */
    assert(!race_order(racers[1], racers[0]));
    for (int i = 0; i < 8; ++i) {
        *(int *)(racers[i] + 0x60) = saved_positions[i];
        racers[i][0x8c] = 0;
        lap_times[i][2] = 90000;
    }
    put_pointer(bound_racer, 0x28, racers[0]);
    *(int *)(players[0] + 0xe4) = 4;
    receive_results(team_results);
    assert(!strcmp(*(char **)(winner_text + 0x30), "BLUE TEAM WINS"));
    assert(!strcmp(*(char **)(wins_text + 0x30), "RESULT"));
    assert(!strcmp(*(char **)(points_text + 0x30), "22 POINTS"));
    /* Independent final-result scenarios must begin a fresh race snapshot. */
#define NEW_TEAM_RESULT() do { synced_finished_mask=-1; initialize_race_ui(race_ui_owner); } while (0)
    NEW_TEAM_RESULT();
    *(int *)(native_settings + 0x28) = 3;
    const int tied_teams[] = {1, 0, 2, 2, 0, 1, 2, 0}; /* 13/13/13, RED finishes first. */
    for (int i = 0; i < 8; ++i) *(int *)(players[i] + 0xe4) = tied_teams[i];
    receive_results(team_results);
    assert(!strcmp(*(char **)(wins_text + 0x30), "WINS"));
    assert(!strcmp(*(char **)(winner_text + 0x30), "RED TEAM WINS"));
    assert(!strcmp(*(char **)(points_text + 0x30), "13 POINTS (H2H)"));
    local_player_mock = players[1];
    project_event(ranking_project, panel_event);
    assert(!strcmp(*(char **)(wins_text + 0x30), "LOSE"));
    assert(!strcmp(*(char **)(winner_text + 0x30), "RED TEAM WINS"));
    local_player_mock = players[0];
    NEW_TEAM_RESULT();
    /* The individual winner's team loses on points: only tied leaders enter H2H. */
    const int leader_tie[] = {0, 2, 2, 1, 1, 1, 1, 0}; /* BLUE 11, RED/GREEN 14. */
    for (int i = 0; i < 8; ++i) *(int *)(players[i] + 0xe4) = leader_tie[i];
    receive_results(team_results);
    assert(!strcmp(*(char **)(wins_text + 0x30), "LOSE"));
    assert(!strcmp(*(char **)(winner_text + 0x30), "GREEN TEAM WINS"));
    assert(!strcmp(*(char **)(points_text + 0x30), "14 POINTS (H2H)"));
    NEW_TEAM_RESULT();
    *(int *)(native_settings + 0x28) = 4;
    /* Reported screenshot: GREEN 10+4=14, YELLOW 8+5=13,
     * BLUE 6+1=7, RED 3+2=5. Header points belong to the named winner. */
    const int screenshot_teams[] = {2, 3, 0, 3, 2, 1, 1, 0};
    for (int i = 0; i < 8; ++i) *(int *)(players[i] + 0xe4) = screenshot_teams[i];
    receive_results(team_results);
    for (int i = 0; i < 8; ++i) {
        local_player_mock = players[i];
        project_event(ranking_project, panel_event);
        assert(!strcmp(*(char **)(winner_text + 0x30), "GREEN TEAM WINS"));
        assert(!strcmp(*(char **)(points_text + 0x30), "14 POINTS"));
        assert(!strcmp(*(char **)(wins_text + 0x30), screenshot_teams[i] == 2 ? "WINS" : "LOSE"));
        put_pointer(bound_racer, 0x28, racers[i]);
        row_event(row_behavior, NULL, NULL);
        assert(!strcmp(*(char **)(score_text + 0x30), awards[i]));
    }
    *(int *)(native_settings + 12) = 3;
    for (int i = 0; i < 8; ++i) {
        put_pointer(racers[i], 0x90, lap_lists[i]);
        put_pointer(lap_lists[i], 0x20, lap_times[i]);
        *(int *)(lap_lists[i] + 12) = 2;
        racers[i][0x8c] = 1; /* Native teardown force-finishes DNF racers too. */
    }
    *(int *)(lap_lists[1] + 12) = *(int *)(lap_lists[3] + 12) = 3;
    lap_times[1][2] = 90000; lap_times[3][2] = 95000;
    NEW_TEAM_RESULT();
    /* Itemless DNF awards are zero, including whole teams and repeated packets. */
    speed = 1; room_active = 1;
    racers[1][0x8c] = 0; racers[3][0x8c] = 1;
    complete_result_racer = racers[1]; /* Final guest status becomes visible inside native handling. */
    receive_results(team_results);
    complete_result_racer = NULL;
    room_active = 0; /* Quiet race teardown must not turn Itemless scoring into Normal. */
    receive_results(team_results);
    room_active = 1;
    assert(!strcmp(*(char **)(winner_text + 0x30), "YELLOW TEAM WINS"));
    assert(!strcmp(*(char **)(points_text + 0x30), "13 POINTS"));
    for (int i = 0; i < 8; ++i) {
        put_pointer(bound_racer, 0x28, racers[i]);
        row_event(row_behavior, NULL, NULL);
        assert(!strcmp(*(char **)(score_text + 0x30), i == 1 || i == 3 ? awards[i] : "0 POINT"));
        assert(*(float *)(racer_row + 0xe0) == (i == 1 || i == 3 ? 1.0f : 0.6f));
    }
    void (ABI *team_late_finish)(unsigned char *,int)=(void *)(image+(0xff2320-shift));
    void (ABI *score_tick)(unsigned char *,void *,float)=*(void **)(image+0x25436d8);
    /* Host snapshot: a racer completing laps after results stays DNF/zero. */
    put_pointer(bound_racer,0x28,racers[0]);
    *(int *)(lap_lists[0]+12)=3; lap_times[0][2]=99000;
    racers[0][0x8c]=0; racers[0][0xb0]=1;
    team_late_finish(racers[0],1);
    receive_results(team_results);
    *(float *)(racer_row+0xe0)=1; *(float *)(score_background+0xe0)=1;
    score_tick(row_behavior,NULL,.25f);
    assert(!strcmp(*(char **)(score_text+0x30),"0 POINT"));
    assert(*(float *)(racer_row+0xe0)==.6f && *(float *)(score_background+0xe0)==0);
    assert(!strcmp(*(char **)(points_text+0x30),"13 POINTS"));
    /* Guests use the authority mask, never their local completion timing.
     * Exercise the mask both before and after the native result event. */
    for (int local=0;local<8;++local) for (int early=0;early<2;++early) {
        local_player_mock=players[local];
        NEW_TEAM_RESULT(); following_host=1;
        for (int i=0;i<8;++i) { racers[i][0x8c]=1; *(int *)(lap_lists[i]+12)=3; lap_times[i][2]=99000; }
        racers[1][0x8c]=0; *(int *)(lap_lists[1]+12)=2;
        if (early) synced_finished_mask=0x0a;
        receive_results(team_results);
        if (!early) {
            put_pointer(bound_racer,0x28,racers[0]);
            *(float *)(racer_row+0xe0)=1;
            score_tick(row_behavior,NULL,.25f);
            assert(*(float *)(racer_row+0xe0)==.6f); /* Do not brighten from guest-local flags while waiting. */
        }
        synced_finished_mask=0x0a;
        put_pointer(bound_racer,0x28,racers[1]);
        score_tick(row_behavior,NULL,.25f);
        assert(!strcmp(*(char **)(score_text+0x30),"8 POINT"));
        assert(*(float *)(racer_row+0xe0)==1);
        put_pointer(bound_racer,0x28,racers[0]);
        racers[0][0x8c]=0; racers[0][0xb0]=1;
        team_late_finish(racers[0],1); receive_results(team_results);
        score_tick(row_behavior,NULL,.25f);
        assert(!strcmp(*(char **)(score_text+0x30),"0 POINT"));
        assert(*(float *)(racer_row+0xe0)==.6f);
        assert(!strcmp(*(char **)(points_text+0x30),"13 POINTS"));
        following_host=0;
    }
    local_player_mock=players[0];
    for (int i=0;i<8;++i) { racers[i][0x8c]=1; *(int *)(lap_lists[i]+12)=i==1 || i==3 ? 3 : 2; }
    NEW_TEAM_RESULT();
    /* Guest finishes first, host does not finish: both local viewpoints agree. */
    *(int *)(team_results + 0x20) = 1;
    *(int *)(team_results + 0x24) = 0;
    *(int *)(lap_lists[3] + 12) = 2;
    for (int local = 0; local < 2; ++local) {
        local_player_mock = players[local];
        receive_results(team_results);
        assert(!strcmp(*(char **)(winner_text + 0x30), "YELLOW TEAM WINS"));
        assert(!strcmp(*(char **)(points_text + 0x30), "10 POINTS"));
        assert(!strcmp(*(char **)(wins_text + 0x30), local ? "WINS" : "LOSE"));
        put_pointer(bound_racer, 0x28, racers[local]);
        row_event(row_behavior, NULL, NULL);
        assert(!strcmp(*(char **)(score_text + 0x30), local ? "10 POINT" : "0 POINT"));
        if (local) assert(!strcmp(*(char **)(score_position + 0x30), "1"));
    }
    NEW_TEAM_RESULT();
    *(int *)(team_results + 0x20) = 0;
    *(int *)(team_results + 0x24) = 1;
    *(int *)(lap_lists[3] + 12) = 3;
    *(int *)(lap_lists[1] + 12) = 2;
    lap_times[3][2] = -1; /* Full list with missing final time is also DNF. */
    receive_results(team_results);
    assert(!strcmp(*(char **)(winner_text + 0x30), "NO FINISHERS"));
    assert(!strcmp(*(char **)(points_text + 0x30), "0 POINTS"));
    NEW_TEAM_RESULT();
    speed = 0;
    receive_results(team_results);
    assert(!strcmp(*(char **)(winner_text + 0x30), "GREEN TEAM WINS"));
    assert(!strcmp(*(char **)(points_text + 0x30), "14 POINTS"));
    local_player_mock = players[0];
    for (int i = 0; i < 8; ++i) *(int *)(players[i] + 0xe4) = i % 4;
    NEW_TEAM_RESULT();
    put_pointer(racers[3], 0x10, NULL);
    *(int *)(ai_selections + 12) = 8;
    *(int *)(team_results + 0x2c) = 19;
    *(int *)(racers[3] + 0x164) = 3;
    *(int *)(racers[3] + 0x174) = 0x19;
    put_pointer(bound_racer, 0x28, racers[3]);
    receive_results(team_results);
    assert(!strcmp(*(char **)(score_text + 0x30), "5 POINT"));
    NEW_TEAM_RESULT();
    speed = 1;
    receive_results(team_results); /* AI DNF follows the same Itemless rule. */
    assert(!strcmp(*(char **)(score_text + 0x30), "0 POINT"));
    /* Native late finish awards must not leak into Itemless DNF scoreboard rows. */
    for (int slot = 0; slot <= 3; slot += 3) {
        racers[slot][0xb0] = 1;
        racers[slot][0x8c] = 0;
        *(int *)(lap_lists[slot] + 12) = 2;
        void (ABI *dnf_finish)(unsigned char *, int) = (void *)(image + (0xff2320 - shift));
        dnf_finish(racers[slot], 1);
        put_pointer(bound_racer, 0x28, racers[slot]);
        row_event(row_behavior, NULL, NULL);
        assert(!strcmp(*(char **)(score_text + 0x30), "0 POINT"));
        receive_results(team_results);
        assert(!strcmp(*(char **)(score_text + 0x30), "0 POINT"));
    }
    *(int *)(lap_lists[3] + 12) = 3;
    NEW_TEAM_RESULT();
    racers[3][0x8c] = 1; lap_times[3][2] = 95000;
    receive_results(team_results);
    assert(!strcmp(*(char **)(score_text + 0x30), "5 POINT"));
    NEW_TEAM_RESULT();
    racers[3][0x8c] = 0;
    speed = 0;
    receive_results(team_results);
    /* The finish-time panel has no score field but gets the same readable colors. */
    put_pointer(row_behavior, 0xd0, NULL);
    row_event(row_behavior, NULL, NULL);
    assert(*(float *)(score_name + 0xc0) == 1.0f);
    assert(*(float *)(racer_row + 0xe0) == 0.6f);
    assert(!memcmp(score_base + 0xc0, native_base, sizeof native_base));
    assert(!memcmp(score_background + 0xc0, native_highlight, sizeof native_highlight));
    assert(*(float *)(score_position + 0xc4) == 0.85f);
    unsigned char *finish_racer = *(void **)(bound_racer + 0x28);
    finish_racer[0x8c] = 1;
    row_event(row_behavior, NULL, NULL);
    for (int c = 0; c < 4; ++c) {
        const float expected = 1.f;
        assert(*(float *)(score_name + 0xc0 + c*4) == expected);
        assert(*(float *)(score_engine + 0xc0 + c*4) == expected);
    }
    finish_racer[0x8c] = 0;
    row_event(row_behavior, NULL, NULL);
    put_pointer(row_behavior, 0xd0, score_text);
    unsigned char leader_behavior[0xa0] = {0}, border[0x120] = {0};
    put_pointer(leader_behavior, 0x88, bound_racer);
    put_pointer(leader_behavior, 0x50, border);
    void (ABI *leader_event)(void *, void *, void *) = *(void **)(image + 0x2546008);
    leader_event(leader_behavior, NULL, NULL);
    assert(*(float *)(border + 0xc0) == 1.0f && *(float *)(border + 0xc4) == 0.85f);
    assert(!strcmp(*(char **)(leader_position + 0x30), "1"));
    unsigned char marker[0x90] = {0}, marker_label[0x120] = {0}, arrow[0x120] = {0};
    unsigned char decoration[0x120] = {0}, marker_children[40] = {0};
    void *decorations[] = {decoration};
    put_pointer(arrow, 0x48, marker_children); put_pointer(marker_children, 0x20, decorations);
    *(int *)(marker_children + 12) = 1;
    *(float *)(arrow + 0x94) = 80; *(float *)(arrow + 0x70) = .5f; *(float *)(arrow + 0x88) = 1.8f;
    *(float *)(marker_label + 0x94) = 50; *(float *)(marker_label + 0x70) = .5f;
    put_pointer(marker, 0x70, bound_racer);
    put_pointer(marker, 0x50, marker_label); put_pointer(marker, 0x48, arrow);
    put_pointer(marker_label, 0x30, make_string(NULL, "Old AI name"));
    void (ABI *marker_event)(void *, void *, void *) = *(void **)(image + 0x2545940);
    unsigned char marker_init[0x30] = {0};
    put_pointer(marker_init, 0, panel_event_vtable);
    put_pointer(marker_init, 8, (void *)31);
    put_pointer(image, 0x2abfc40, (void *)31);
    marker_event(marker, marker_init, NULL);
    assert(marker_native_flag == 1 && marker_init[0x28] == 0);
    assert(!strcmp(*(char **)(marker_label + 0x30), ""));
    assert(*(unsigned *)(marker_label + 12) & 0x400);
    assert(*(unsigned *)(decoration + 12) & 0x400);
    assert(*(float *)(arrow + 0xc4) == 0.85f && *(float *)(arrow + 0xe0) == 1);
    put_pointer(bound_racer, 0x28, racers[1]);
    free_string(NULL, *(char **)(marker_label + 0x30) - 16);
    put_pointer(marker_label, 0x30, make_string(NULL, "Human"));
    marker_event(marker, NULL, NULL);
    marker_event(marker, NULL, NULL); /* Repeated native updates must not move the name cumulatively. */
    assert(!strcmp(*(char **)(marker_label + 0x30), "Human"));
    assert(!(*(unsigned *)(marker_label + 12) & 0x400));
    assert(*(float *)(marker_label + 0xc4) == 1);
    assert(*(float *)(marker_label + 0x64) == 107);
    assert(*(float *)(arrow + 0xc4) == 0.15f);
    free_string(NULL, *(char **)(marker_label + 0x30) - 16);
    put_pointer(bound_racer, 0x28, racers[3]);
    /* Native Battle's +68 pipeline: arena result, RaceFinished, cached result
     * context. Only RaceFinished duplicates are suppressed, not companion events. */
    const int prepared_before = team_score_opens;
    initialize_race_ui(race_ui_owner);
    assert(team_score_opens == prepared_before + 1);
    assert(*(void **)(race_ui_owner + 0x60) == panel_handle);
    assert(*(void **)(race_ui_owner + 0x68) == ranking_handle);
    score_events_before=ranking_events; finish_events_before=panel_events;
    put_pointer(panel_event,8,(void *)14);
    project_event(ranking_project,panel_event);
    put_pointer(panel_event,8,(void *)12);
    project_event(ranking_project,panel_event);
    project_event(panel_project,panel_event);
    project_event(ranking_project,panel_event);
    put_pointer(panel_event,8,(void *)13);
    project_event(ranking_project,panel_event);
    assert(ranking_events==score_events_before+3 && panel_events==finish_events_before);
    assert(team_score_opens==prepared_before+1);
    assert(!*(void **)(race_ui_owner+0x60) && *(void **)(race_ui_owner+0x68)==ranking_handle);
    /* A losing local racer gets the native loss selection, without confetti. */
    for (int i=0;i<8;++i) {
        *(int *)(team_results+0x20+i*4)=i;
        put_pointer(racers[i],0x10,handles[i]);
        *(int *)(players[i]+0xe4)=i%2;
    }
    *(int *)(players[0]+0xe4)=1;
    *(int *)(players[1]+0xe4)=0;
    *(int *)(native_settings+0x28)=2;
    racers[0][0x8c] = 1;
    lap_times[0][2] = 90000;
    *(int *)(racers[0] + 0x60) = 1;
    *(int *)(racers[0] + 0x64) = 1;
    local_player_mock = players[0];
    const int animations_before_loss=animation_calls, confetti_before_loss=confetti_calls;
    receive_results(team_results);
    assert(animation_calls==animations_before_loss+1 && animation_selection==1);
    assert(confetti_calls==confetti_before_loss);
    assert(result_sounds == animation_calls && released_result_sounds == result_sounds && !result_sound_won);
    receive_results(team_results);
    assert(animation_calls==animations_before_loss+1 && confetti_calls==confetti_before_loss);

    lobby_data[0x71] = 1;
    private = 0;
    expected_ranking = "Octane_Race_Ranking";
    load_map(lobby_map, 8);
    assert(lobby_data[0x71] == 0); /* No invented team ceremony. CUP stays native. */
    assert(damage(damage_component, 0, &source_handle, 0) == 0 && damage_calls == 6);
    clock_apply(clock_component, victim, NULL);
    assert(clock_calls == 2);
    assert(!runtime_online_mode_in_private());
    initialize_race_ui(race_ui_owner);
    receive_results(team_results);
    assert(!strcmp(*(char **)(score_text + 0x30), "5 POINT"));
    /* Both public modes ignore stale private state and share authoritative results. */
    for (int itemless = 0; itemless < 2; ++itemless) {
        private = 1; public_active = itemless; public_normal_active = !itemless; speed = 0; mode_verified = 0;
        *(int *)(native_settings + 0x28) = 1;
        int public_mode_checks = mode_checks;
        load_map("public-track", 8);
        assert(!runtime_online_mode_in_private());
        assert(mode_checks == public_mode_checks);
        for (int i = 0; i < 8; ++i) {
            racers[i][0x8c] = 1;
            *(int *)(lap_lists[i] + 12) = 3;
            lap_times[i][2] = 90000 + i * 1000;
            *(int *)(racers[i] + 0x60) = 8 - i;
            *(int *)(team_results + 0x20 + i * 4) = 7 - i;
        }
        assert(race_order(racers[0], racers[7]));
        receive_results(team_results);
        assert(race_order(racers[7], racers[0])); /* Accepted host result is authoritative. */
        receive_results(team_results);
        *(int *)(team_results + 0x20) = 6;
        receive_results(team_results); /* Duplicate IDs cannot replace the validated result. */
        assert(race_order(racers[7], racers[0]));
        racers[7][0xb0] = 1;
        racers[7][0x8c] = 0;
        *(int *)(racers[7] + 0x58) = 8;
        *(int *)(racers[7] + 0x64) = 8;
        void (ABI *public_late_finish)(unsigned char *, int) = (void *)(image + (0xff2320 - shift));
        public_late_finish(racers[7], 1);
        assert(*(int *)(racers[7] + 0x58) == 1);
        assert(*(int *)(racers[7] + 0x60) == 1);
        assert(*(int *)(racers[7] + 0x64) == 1);
        mode_verified = 1; public_active = public_normal_active = 0; private = 0;
        load_map(lobby_map, 8);
        *(int *)(racers[0] + 0x60) = 1;
        *(int *)(racers[7] + 0x60) = 8;
        assert(race_order(racers[0], racers[7])); /* No previous-race snapshot leaks to the lobby. */
    }
    /* Reused Battle asset must not bind or reveal private Team Race extras. */
    marker_native_flag = -1;
    marker_event(marker, marker_init, NULL); /* Ordinary Battle keeps its native event unchanged. */
    assert(marker_native_flag == 0 && marker_init[0x28] == 0);
    native_battle_row = 1; row_behavior[0xd8] = 1;
    native_row_icons = 0;
    row_event(row_behavior, NULL, NULL);
    assert(native_row_icons == 0 && row_behavior[0xd8] == 1);
    assert((*(unsigned *)(score_avatar + 12) & 0x400) && (*(unsigned *)(score_engine + 12) & 0x400));
    assert(*(void **)(row_behavior + 0x98) == score_avatar && *(void **)(row_behavior + 0xa0) == score_engine);
    /* Native icons in ordinary finish panels remain bound and visible. */
    put_pointer(score_avatar, 0x28, "NativePortrait");
    put_pointer(score_engine, 0x28, "NativeEngine");
    row_event(row_behavior, NULL, NULL);
    assert(native_row_icons == 2);
    assert(!(*(unsigned *)(score_avatar + 12) & 0x400) && !(*(unsigned *)(score_engine + 12) & 0x400));
    native_battle_row = 0;
    free_string(NULL, *(char **)(score_position + 0x30) - 16);
    free_string(NULL, *(char **)(score_text + 0x30) - 16);
    free_string(NULL, *(char **)(winner_text + 0x30) - 16);
    free_string(NULL, *(char **)(wins_text + 0x30) - 16);
    free_string(NULL, *(char **)(points_text + 0x30) - 16);
    free_string(NULL, *(char **)(score_name + 0x30) - 16);
    free_string(NULL, *(char **)(leader_position + 0x30) - 16);
    put_pointer(ranking_project, 0x10, NULL);
    private = 1;
    memcpy(state + 0x13c, &battle, 4);
    *race_mode = 9;
    *(int *)(native_settings + 0x28) = 3;
    load_map("battle-test-map", 8);
    assert(image[0xf34575 - shift] == 0xed); /* Native Battle timer policy. */
    assert(*(int *)(native_settings + 0x28) == 3); /* Battle keeps native three-team support. */
    *(int *)(native_settings + 0x28) = 4;
    assert(damage(damage_component, 0, &source_handle, 0) == 0 && damage_calls == 7);
    clock_apply(clock_component, victim, NULL);
    assert(clock_calls == 3);
    free_string(NULL, *(char **)(private_settings + 0x10) - 16);
    munmap(image, size);
}

int main(void) {
    test_region(0);
    test_region(0x10);
}
