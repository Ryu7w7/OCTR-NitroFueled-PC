#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

static int speed = -1, private = 1, base_calls;
static int room_active, lobby_updates;
static void ABI base_update(void *self, void *owner, float dt) {
    (void)self;
    assert(owner && dt == 0.25f);
    ++lobby_updates;
}
void runtime_speed_mode_set(int enabled) { speed = enabled; }
int runtime_speed_race_active(void) { return speed == 1 && room_active; }
static void ABI private_state(unsigned char *yes, unsigned char *no) {
    *yes = (unsigned char)private; *no = (unsigned char)!private;
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
static unsigned char test_zones[4][48], test_handles[4][48];
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
    assert(0);
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
static int expected_grid, grid_calls;
static unsigned char ranking_handle[0x30], panel_handle[0x30];
static unsigned char ranking_project[0x20], panel_project[0x20], panel_root[0x20];
static int panel_opens, panel_events, ranking_events, context_finishes;
static void ABI race_ui_mock(unsigned char *self) {
    put_pointer(self, 0x60, ranking_handle);
}
static void *ABI panel_event_type(unsigned char *event) { return *(void **)(event + 8); }
static void ABI project_event_mock(void *self, void *event) {
    assert(event);
    if (self == panel_project) ++panel_events;
    else ++ranking_events;
}
static void ABI open_panel_mock(void **result, void *manager, const char *name,
                               unsigned char *options, int player, void *context, float delay) {
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
    assert(root == panel_root && hidden == 1);
    root[12] = 1;
}
static void ABI place_racers_mock(unsigned char *manager) {
    unsigned char *list = *(void **)(manager + 0x10);
    unsigned char **items = *(void **)(list + 0x20);
    for (int i = 0; i < *(int *)(list + 12); ++i) {
        unsigned char *handle = *(void **)(items[i] + 0x10);
        unsigned char *player = handle ? *(void **)(handle + 0x28) : NULL;
        int id = *(int *)(player ? player + 0xc8 : items[i] + 0x164);
        assert(*(int *)(items[i] + 0x60) == (expected_grid ? 8-id : 0));
    }
    ++grid_calls;
}
static unsigned char **result_racers;
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
    stub(image + (0x1277c70 - shift), (void *)original_handler);
    direct_stub(image + 0x97dfc0, base_update);
    put_pointer(image, 0x2773b50, image + 0x97dfc0);
    void *handler = image + (0x1277c70 - shift);
    memcpy(image + 0x2773b40, &handler, 8);
    memcpy(image + (0xfff160 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x49\x89\xfe", 14);
    memcpy(image + (0xf34430 - shift), "\x48\x8b\x05\x39\x07\xba\x01\x4c\x8b\x40\x10\x4d\x85\xc0", 14);
    memcpy(image + (0xfff16e - shift), "\x5b\x41\x5c\x41\x5e\x41\x5f\x5d", 8);
    direct_stub(image + (0xfff176 - shift), clear_participant_mock);
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
    memcpy(image + (0xf31160 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x58", 17);
    memcpy(image + (0xf31171 - shift), "\x48\x83\xc4\x58\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xf3117f - shift), place_racers_mock);
    memcpy(image + (0xe9ba30 - shift), "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x48", 17);
    memcpy(image + (0xe9ba41 - shift), "\x48\x83\xc4\x48\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d", 14);
    direct_stub(image + (0xe9ba4f - shift), race_ui_mock);
    direct_stub(image + 0x253740, project_event_mock);
    direct_stub(image + 0x986820, open_panel_mock);
    direct_stub(image + 0x27ae10, finish_context_mock);
    direct_stub(image + 0x252110, hide_panel_mock);
    put_pointer(image, 0x2432338, image + 0x253740);
    put_pointer(image, 0x2ac02b0, (void *)11);
    put_pointer(image, 0x2ac02a8, (void *)12);
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
    unsigned char race_ui_owner[0x68] = {0}, panel_event[16] = {0};
    void *panel_event_vtable[] = {(void *)panel_event_type};
    put_pointer(panel_event, 0, panel_event_vtable);
    put_pointer(ranking_handle, 0x28, ranking_project);
    put_pointer(panel_handle, 0x28, panel_project);
    put_pointer(panel_project, 0x10, panel_root);
    *(unsigned *)ranking_handle = *(unsigned *)panel_handle = 100;
    panel_opens = panel_events = ranking_events = context_finishes = 0;
    void (ABI *initialize_race_ui)(void *) = (void *)(image + (0xe9ba30 - shift));
    void (ABI *project_event)(void *, void *) = *(void **)(image + 0x2432338);
    pool_queued = pool_waited = ai_spawns = 0;
    cleared_participants = forced_finishes = 0;
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
        load_map(maps[cup_index], 8);
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
        put_pointer(panel_event, 8, (void *)12); /* full CUP scoreboard */
        project_event(ranking_project, panel_event);
        assert(panel_root[12] && panel_events == events_before);
        put_pointer(panel_event, 8, (void *)11);
        project_event(ranking_project, panel_event); /* late local finish cannot reopen */
        assert(panel_opens == cup_index + 1 && panel_events == events_before);
        assert(*race_mode == 6 && *(int *)(manager + 0xd68) == cup_index);
        assert(image[0xf34575 - shift] == 0xff);
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
        place_racers(racers_manager);
        assert(grid_calls == cup_index + 1);
        /* Any of the eight native slots can be the local test client. */
        for (int local = 0; local < 8; ++local) {
            for (int k = 0; k < 8; ++k) {
                racers[k][0xb0] = k == local;
            }
            check_finished(NULL);
            void (ABI *native_finish)(unsigned char *, int) = (void *)(image + (0xff2320 - shift));
            native_finish(racers[local], 1); /* Isolated mock scoring callback, never live gameplay. */
            for (int k = 0; k < 8; ++k) assert(racers[k][0x8c] == (k <= local));
        }
        const int awards[] = {9,6,3,1,0,0,0,0};
        for (int k = 0; k < 8; ++k)
            assert(*(int *)(racers[k] + 0xb8) == (cup_index + 1) * awards[k]);
        unsigned char results[0x40] = {0};
        /* Delayed host results replace locally awarded positions, including ties
         * resolved by the host; native list order is intentionally reversed. */
        for (int k = 0; k < 8; ++k) *(int *)(results + 0x20 + 4*k) = 7-k;
        receive_results(results);
        receive_results(results); /* A repeated result must not add points twice. */
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
        *(int *)(racers[7] + 0x64) = 2;
        void (ABI *late_finish)(unsigned char *, int) = (void *)(image + (0xff2320 - shift));
        late_finish(racers[7], 1);
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
        private = 0; /* The lobby component is gone while racing. */
        assert(runtime_online_mode_private_cup());
        timer_done();
        assert(cup_advances == (cup_index < 3 ? cup_index + 1 : 3));
        assert(lobby_returns == (cup_index == 3));
    }
    /* AI uses its native player ID, without a human player handle. */
    unsigned char mixed_results[0x40] = {0};
    for (int k = 0; k < 8; ++k) *(int *)(mixed_results + 0x20 + 4*k) = k;
    put_pointer(racers[6], 0x10, NULL);
    put_pointer(racers[7], 0x10, NULL);
    *(int *)(racers[6] + 0x164) = 6;
    *(int *)(racers[7] + 0x164) = 7;
    *(int *)(mixed_results + 0x20) = 7;
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
    expected_grid = 1;
    place_racers(racers_manager);
    put_pointer(racers[6], 0x10, handles[6]);
    put_pointer(racers[7], 0x10, handles[7]);
    unsigned char timer_owner[0x100] = {0}, timer_label[0x100] = {0}, timer_children[40] = {0};
    void *timer_items[] = {timer_label};
    put_pointer(timer_owner, 0x28, "OCTR_PRIVATE_RETURN_TIMER");
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
    assert(hud_gate[3] == 1); /* Offline/non-CUP keeps the native gate. */
    assert(!memcmp(image + (0xfff9c2 - shift), "\x0f\x85\xd6\x06\0\0", 6));
    private = 1;
    *(int *)(test_network_state + 0x144) = 8;
    timer_done();
    assert(lobby_returns == 1 && cup_advances == 3);
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
    received(packet);
    assert(received_mode == 1 && !runtime_online_mode_private_cup());
    memcpy(packet + 0x3c, &cup_mode, 4);
    put_pointer(packet, 0x28, "unknown-track");
    received(packet);
    assert(received_mode == 1 && !runtime_online_mode_private_cup());
    runtime_online_mode_leave_cup();
    assert(runtime_online_mode_private_cup()); /* retain own host preference */
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
    munmap(image, size);
}

int main(void) {
    test_region(0);
    test_region(0x10);
}
