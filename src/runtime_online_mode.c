/* Native Normal / Speed / Battle selector; Speed uses the game's race event. */
#define _GNU_SOURCE
#include "runtime.h"
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
static PrivateState private_state;
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
static unsigned char *finish_timer_counter;
static unsigned char *ai_reuse_branch, *ai_asset_reuse_branch;
static const unsigned char native_ai_asset_reuse[] = {0x0f,0x85,0xf2,0x0b,0,0};
static const unsigned char native_ai_reuse[] = {0x0f,0x85,0xd6,0x06,0,0};
typedef void (ABI *ObjectPin)(void *, void *, void **);
static ObjectPin object_pin;
static SetSeason set_race_mode;
static void **race_manager;
static int picker_previous_mode = -1;
static EventHandler original_cup_item;
static EventHandler original_level_button;
static EventHandler original_lobby_event, original_timer_event;
typedef void (ABI *BehaviorUpdate)(unsigned char *, unsigned char *, float);
static BehaviorUpdate original_lobby_update, original_selector_update;
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
typedef void (ABI *ResizeList)(void *, int, int);
static ResizeList resize_list;
static NativeEvent clear_zone_list;
static NativeEvent refresh_selector;
static NativeEvent original_clear_participants;
static NativeEvent original_reset_racer_pool;
static NativeEvent original_spawn_cup_ai;
static NativeEvent original_place_racers;
static NativeEvent original_race_ui;
static NativeEvent finish_project_context;
typedef void (ABI *ProjectEvent)(void *, void *);
typedef void (ABI *OpenProject)(void **, void *, const char *, void *, int, void *, float);
static ProjectEvent original_project_event;
static OpenProject open_project;
static SetSeason hide_placeable;
static void **gui_manager, **race_finished_type;
static void *cup_ranking_project, *cup_finish_project;
static int cup_panel_done;
static SetSeason queue_pool_assets, wait_pool_assets;
static unsigned char **native_racer_pool;
static char *cup_ai_assets[8][2];
static NativeEvent original_race_results;
typedef void (ABI *SortCupRacers)(void **, void **, size_t);
static SortCupRacers sort_cup_racers;
static _Thread_local void *cup_podium_order[8];
static _Thread_local int cup_podium_count;
typedef int (ABI *RaceFinished)(unsigned char *);
static RaceFinished original_race_finished;
static SetSeason original_finish_racer;
struct CupTotal { uint64_t online_id; int id, human, points, finish_position; };
static struct CupTotal cup_totals[8];
static int cup_total_count;
static struct { struct CupTotal key; int position, award; } cup_results[8];
static int cup_result_count;
typedef void (ABI *MakeHandle)(void **, void *);
static MakeHandle make_handle;

typedef void (ABI *LoadMap)(const char *, int);
static LoadMap original_load_map;
typedef void (ABI *NoArgument)(void);
static NoArgument return_to_lobby;
static NativeEvent next_cup_map;
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
static void snapshot_cup_ai_assets(void);
static void restore_cup_total(unsigned char *racer);
static void refresh_lobby_card(unsigned char *owner);
static void clear_cup_finish_panel(void);

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
    memcpy(event + 0x3c, &mode, 4);
    printf("Native private CUP: received %s\n", remote_cup_selected ?
           (const char *)pointer_at(cup, 0x28) : "SINGLE");
}

static void ABI private_settings_sending(unsigned char *event) {
    int mode = 1;
    if (race_manager && *race_manager)
        memcpy(&mode, (unsigned char *)*race_manager + 0xd40, 4);
    if (runtime_online_mode_private_cup() && current_cup()) mode = 6;
    memcpy(event + 0x3c, &mode, 4);
}

/* The map loader runs after settings synchronization, on host and peers. Keep
 * the lobby's racing UI mode until this boundary, then use native CUP scoring. */
static void ABI load_private_map(const char *map, int reason) {
    const int incomplete_cup_return = private_cup_race_active && !cup_final_results_ready &&
        map && !strcasecmp(map, "octane/octane_onlinelobby/octane_onlinelobby");
    clear_cup_finish_panel();
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
    /* Native timer dispatch/replication stays intact; include finished AI in CUP. */
    const unsigned char counter = private_cup_race_active ? 0xff : 0xed;
    if (finish_timer_counter && *finish_timer_counter != counter &&
        !write_code(finish_timer_counter, &counter, 1))
        fputs("Native private CUP: could not enable AI finish countdown\n", stderr);
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
    if (incomplete_cup_return) {
        unsigned char *zone = find_zone(*zone_manager, map);
        unsigned char *data = zone ? pointer_at(zone, 0x1b0) : NULL;
        /* Online podium data can still contain track 1/2/3 after host loss. */
        if (data) {
            data[0x71] = 0;
            puts("Native private CUP: skipped stale podium (no final host results)");
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

static void ABI race_ui(unsigned char *self) {
    clear_cup_finish_panel();
    original_race_ui(self);
    if (private_cup_race_active) {
        assign_handle((unsigned char *)&cup_ranking_project, pointer_at(self, 0x60));
        /* Load with the race HUD so streaming completes before local finish. */
        unsigned char options[0x28] = {0};
        unsigned char context[0x10] = {0};
        const int two = 2, eight = 8, unset = -1;
        memcpy(options + 8, &two, 4);
        memcpy(options + 12, &unset, 4);
        memcpy(options + 24, &eight, 4);
        memcpy(options + 28, &unset, 4);
        open_project(&cup_finish_project, *gui_manager, "Octane_Race_Ranking",
                     options, -1, context, 0.0f);
        finish_project_context(context);
        assign_handle(context + 8, NULL);
        assign_handle(context, NULL);
        puts(cup_finish_project ? "Native private CUP: prepared local finish time panel" :
             "Native private CUP: local finish time panel unavailable");
    }
}

static void ABI cup_project_event(void *self, void *event) {
    if (private_cup_race_active && cup_ranking_project &&
        self == handle_object(&cup_ranking_project)) {
        GetType type = (GetType)pointer_at(pointer_at(event, 0), 0);
        void *kind = type(event);
        if (kind == *race_finished_type) {
            /* The native CUP project owns the full standings and next-race flow. */
            cup_panel_done = 1;
            hide_cup_finish_panel();
        } else if (!cup_panel_done) {
            unsigned char *panel = handle_object(&cup_finish_project);
            if (panel) original_project_event(panel, event);
        }
    }
    original_project_event(self, event);
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
}

static void snapshot_cup_totals(void) {
    cup_total_count = 0;
    unsigned char *manager = racer_manager ? *racer_manager : NULL;
    unsigned char *list = manager ? pointer_at(manager, 0x10) : NULL;
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    for (int i = 0; items && i < count && cup_total_count < 8; ++i)
        cup_totals[cup_total_count++] = racer_identity(items[i]);
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

static void ABI racer_finished(unsigned char *racer, int finished) {
    restore_cup_total(racer);
    original_finish_racer(racer, finished);
    if (!private_cup_race_active || !cup_result_count) return;
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
            if (id != keys[i].id) continue;
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
    for (int i = 0; i < cup_podium_count; ++i)
        if (cup_podium_order[i] == racer) return i + 1;
    return finish_position;
}

static void ABI race_results_received(unsigned char *event) {
    const int applied = apply_cup_results(event);
    if (applied && current_cup_index() == 3) {
        unsigned char *list = pointer_at(*racer_manager, 0x10);
        cup_podium_count = cup_result_count;
        memcpy(cup_podium_order, pointer_at(list, 0x20), cup_podium_count * sizeof(void *));
        /* Use the native CUP sorter, including its human/finish tie breakers. */
        sort_cup_racers(cup_podium_order, cup_podium_order + cup_podium_count, cup_podium_count);
        for (int i = 0; i < cup_podium_count && i < 3; ++i) {
            const struct CupTotal key = racer_identity(cup_podium_order[i]);
            printf("Native private CUP: podium %d player=%d points=%d\n", i + 1, key.id, key.points);
        }
    }
    original_race_results(event);
    if (cup_podium_count) cup_final_results_ready = 1;
    cup_podium_count = 0;
    if (applied) {
        apply_cup_results(event);
        printf("Native private CUP: host results applied for race %d/4 (%d racers)\n",
               current_cup_index() + 1, cup_result_count);
    }
}

static int ABI race_finished(unsigned char *self) {
    unsigned char *manager = racer_manager ? *racer_manager : NULL;
    unsigned char *list = manager ? pointer_at(manager, 0x10) : NULL;
    int count = 0;
    if (list) memcpy(&count, list + 12, 4);
    void **items = list ? pointer_at(list, 0x20) : NULL;
    for (int i = 0; items && i < count; ++i) {
        unsigned char *racer = items[i];
        if (!racer[0x8c]) cup_transition_pending = 0;
        restore_cup_total(racer);
    }
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
    if (name && !strcmp(name, "[ModeName]")) set_text(row, mode);
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

static void ABI lobby_update(unsigned char *self, unsigned char *owner, float dt) {
    original_lobby_update(self, owner, dt);
    refresh_lobby_card(owner);
}

static void refresh_lobby_card(unsigned char *owner) {
    if (!owner || !runtime_online_mode_in_private() || battle_selected()) return;
    const char *tag = pointer_at(owner, 0x28);
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
    snprintf(mode, sizeof mode, "%s RACE - %s", cup_mode ? "CUP" : "SINGLE",
             runtime_speed_race_active() ? "ITEMLESS" : "NORMAL");
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
    if (!owner || !runtime_online_mode_private_cup()) return;
    const char *name = pointer_at(owner, 0x28);
    if (!name || strcmp(name, "OCTR_PRIVATE_RETURN_TIMER")) return;
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
    return private_cup_race_active || (runtime_online_mode_in_private() && !battle_selected() &&
        (remote_cup_selected >= 0 ? remote_cup_selected : private_cup_selected));
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
    unsigned char yes = 0, no = 0;
    private_state(&yes, &no);
    return yes != 0;
}

/* The game's own private-match flag (0 before the selector hook is installed). */
int runtime_online_mode_in_private(void) { return private_state ? is_private() : 0; }

static void ABI select_mode(void *sender, int index) {
    if (customized_selector == 2) {
        if (is_private() && !battle_selected() && (index == 0 || index == 1)) {
            private_cup_selected = index;
            remote_cup_selected = -1;
            if (index) ensure_default_cup();
            printf("Native private Race Type: %s\n", index ? "CUP" : "SINGLE");
        }
        return;
    }
    if (!customized_selector) {
        original_choice(sender, index);
        return;
    }
    selector_mode_index = index;
    const int private = is_private();
    /* PUBLIC_SPEED_STEP_2: integration sketch, NOT implemented protocol APIs.
     * After STEP_1 and authenticated queue separation are implemented:
     *
     *   if (!private && index == 1 && !runtime_public_speed_available()) return;
     *   runtime_public_speed_request(!private && index == 1);
     *   const int speed = index == 1 && (private || runtime_public_speed_available());
     *
     * Submit that request when Go To Lobby starts the actual search, not here.
     * Keep original_choice's Race mapping: this is a local game type, NOT a
     * separate server queue. Reject a failed Speed search instead of falling
     * back to Normal. See docs/SPEED_MODE_MATCHMAKING_PROPOSAL.md.
     */
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
    printf("Native Online mode: %s\n", speed ? "ITEMLESS" : index == 2 ? "BATTLE" : "NORMAL");
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
        /* PUBLIC_SPEED_STEP_1: once the negotiated server capability AND client
         * queue integration exist, this becomes:
         *   items[1] = is_private() || runtime_public_speed_available();
         * That API is proposed, not implemented. Keep separate private/public
         * remembered choices; private_speed_selected must not initialize a public
         * Speed search merely because its option has become available.
         */
        items[1] = (unsigned char)is_private();
        int type;
        memcpy(&type, *network + 0x13c, sizeof type);
        const int index = type == 2 ? 2 : (items[1] && private_speed_selected ? 1 : 0);
        selector_mode_index = index;
        memcpy(self + 0x190, &index, sizeof index);
        runtime_speed_mode_set(index == 1);
        /* The asset already supplies all options. Keep the base selector initialization. */
        base_event(self, event, owner);
        race_type_visibility(self, owner, items[1] && index != 2);
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
    uintptr_t handler;
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
    if (!write_code(trampoline, trampoline, stub_end)) return 0;
    original_clear_participants = (NativeEvent)(trampoline + 544);
    original_reset_racer_pool = (NativeEvent)(trampoline + 688);
    original_spawn_cup_ai = (NativeEvent)(trampoline + 736);
    original_place_racers = (NativeEvent)(trampoline + 784);
    original_race_ui = (NativeEvent)(trampoline + 832);
    original_project_event = (ProjectEvent)(image + 0x253740);
    open_project = (OpenProject)(image + 0x986820);
    finish_project_context = (NativeEvent)(image + 0x27ae10);
    hide_placeable = (SetSeason)(image + 0x252110);
    gui_manager = (void **)(image + 0x2ac0dc0);
    race_finished_type = (void **)(image + 0x2ac02a8);
    queue_pool_assets = (SetSeason)(image + (0xe73820 - shift));
    wait_pool_assets = (SetSeason)(image + (0xe73460 - shift));
    original_race_results = (NativeEvent)(image + 0xa298c0);
    sort_cup_racers = (SortCupRacers)(image + (0xeac6c0 - shift));
    original_race_finished = (RaceFinished)(trampoline + 576);
    original_finish_racer = (SetSeason)(trampoline + 624);
    racer_manager = (void **)(image + 0x2ad4b70);
    native_racer_pool = (unsigned char **)(image + 0x2ac57c0);
    end_timer_hud_mode = image + (0xe6675e);
    finish_timer_counter = image + (0xf34575 - shift);
    ai_reuse_branch = image + (0xfff9c2 - shift);
    ai_asset_reuse_branch = image + (0xffd9c7 - shift);
    original_load_map = (LoadMap)(trampoline + 448);
    next_cup_map = (NativeEvent)(image + (0x1046200 - shift));
    network_state = (NetworkState)(image + 0x94efa0);
    network_race = (NetworkRace)(image + 0x9fa130);
    return_to_lobby = (NoArgument)(image + 0xa9cbe0);
    receive_private_settings = (NativeEvent)(trampoline + 192);
    publish_private_track = (NativeEvent)(image + (0xf4f7c0 - shift));
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
    unsigned char jump[14];
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
    jump_to(jump, (void *)race_finished);
    if (!write_code(image + (0xf34430 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)racer_finished);
    if (!write_code(image + (0xff2320 - shift), jump, sizeof jump)) return 0;
    jump_to(jump, trampoline + 512);
    if (!write_code(image + (0xeb9ffb - shift), jump, sizeof jump)) return 0;
    jump_to(jump, (void *)select_mode);
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
