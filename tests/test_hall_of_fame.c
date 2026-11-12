#include "../src/runtime_hall_of_fame.c"
#include <assert.h>
static const char *response = "{\"current_season\":3,\"seasons\":[{\"id\":3},{\"id\":1},{\"id\":2}],\"players\":[{\"name\":\"Leader\",\"rank\":1,\"tier\":\"Gold Mask\",\"mmr\":3500}],\"player\":{\"name\":\"Local\",\"rank\":101,\"tier\":\"Gold Mask\",\"mmr\":1200}}";
static int requests, http = 200, change_during_fetch, old_server;
static char last_target[192], displayed[64];
static int shown;
static int list_focus_calls;
void runtime_hof_focus_list(void *placeable) { assert(placeable); ++list_focus_calls; }
static int native_ready_calls;
static int input_activation_calls;
int runtime_online_mode_hof_activate(void *project) { assert(project); ++input_activation_calls; return 1; }
static int cycle_calls, last_cycle;
void runtime_online_mode_hof_cycle(void *placeable, int direction) {
    assert(placeable && (direction == -1 || direction == 1));
    ++cycle_calls; last_cycle = direction;
}
void runtime_hof_native_ready(void *project) {
    assert(project == (void *)0x1234 && !pthread_mutex_trylock(&hof_lock));
    pthread_mutex_unlock(&hof_lock);
    ++native_ready_calls;
}
const char *runtime_np_get_online_name(void) { return "Local"; }
const char *runtime_ranked_tier_name(int m, int t) { return m == 0 && t == 3 ? "Gold Mask" : "Other"; }
int runtime_ranked_status(int fresh) { (void)fresh; return 1; }
void runtime_online_mode_set_text(void *p, const char *text) { (void)p; snprintf(displayed, sizeof displayed, "%s", text); }
void runtime_online_mode_show_placeable(void *p, int visible) { (void)p; shown = visible; }
void *runtime_online_mode_content(void *p) { (void)p; return NULL; }
int runtime_ryunet_transfer(const char *method, const char *target, const void *data, size_t size,
                           char **reply, size_t *reply_size, size_t maximum) {
    (void)data; (void)size; (void)maximum;
    assert(!strcmp(method, "GET")); ++requests;
    snprintf(last_target, sizeof last_target, "%s", target);
    const char *body = response;
    if (!old_server && strstr(target, "population=friends")) body = "{\"population\":\"friends\",\"current_season\":3,\"seasons\":[{\"id\":3},{\"id\":1},{\"id\":2}],\"players\":[{\"name\":\"FriendTwo\",\"rank\":2,\"tier\":\"Gold Mask\",\"mmr\":3000},{\"name\":\"FriendOne\",\"rank\":120,\"tier\":\"Gold Mask\",\"mmr\":1111}],\"player\":null}";
    if (change_during_fetch == 1) { change_during_fetch = 0; runtime_hof_close(); }
    if (change_during_fetch == 2) {
        change_during_fetch = 0;
        pthread_mutex_lock(&hof_lock); mode = 1; ++generation; ready = 0; pthread_mutex_unlock(&hof_lock);
    }
    *reply = strdup(body); assert(*reply); *reply_size = strlen(*reply);
    return http;
}
static int input(const char *tag, unsigned signal) {
    unsigned char node[0xf0] = {0}; memcpy(node + 0x28, &tag, sizeof tag);
    return runtime_hof_input(node, signal);
}
static void move(const char *tag, unsigned signal) {
    unsigned char node[0xf0] = {0}; memcpy(node + 0x28, &tag, sizeof tag);
    assert(runtime_hof_input(node, signal));
}
static void friends_self_tests(void) {
    struct HofBoard b = {0};
    strcpy(b.player.name, "Me"); b.player.rank = 66;
    include_self(&b);
    assert(b.count == 1 && !strcmp(b.rows[0].name, "Me") && b.rows[0].rank == 1 && b.player.rank == 1);
    include_self(&b); assert(b.count == 1);
    strcpy(b.rows[0].name, "mE"); include_self(&b); assert(b.count == 1);
    strcpy(b.rows[0].name, "Friend"); b.rows[0].rank = 2;
    include_self(&b); assert(b.count == 2 && b.rows[0].rank == 1 && b.rows[1].rank == 2);
    b.player.rank = 0; include_self(&b); assert(b.count == 2);
}

static void parse_tests(void) {
    struct HofBoard b = {0};
    assert(parse_board(response, &b));
    assert(b.current == 3 && b.count == 1 && b.rows[0].rank == 1);
    assert(b.seasons_count == 3 && b.seasons[0] == 1 && b.seasons[2] == 3);
    assert(b.player.rank == 101); free(b.seasons);
    memset(&b, 0, sizeof b);
    assert(parse_board("{\"current_season\":1,\"seasons\":[{\"id\":1}],\"players\":["
        "{\"name\":\"Lower\",\"rank\":120,\"tier\":\"Gold Mask\",\"mmr\":1000},"
        "{\"name\":\"First\",\"rank\":2,\"tier\":\"Gold Mask\",\"mmr\":3000}]}", &b));
    assert(b.rows[0].rank == 2 && b.rows[1].rank == 120);
    assert(!strcmp(b.rows[0].tier, "Gold Mask")); free(b.seasons);
    const char *bad[] = {"{}", "{\"current_season\":3,\"players\":[],\"seasons\":[]}",
        "{\"current_season\":3,\"players\":[],\"seasons\":[{\"id\":3},{\"id\":3}]}",
        "{\"current_season\":3,\"players\":[{\"name\":\"Bad\\\"Name\",\"rank\":1,\"tier\":\"Gold Mask\",\"mmr\":1}],\"seasons\":[{\"id\":3}]}",
        "{\"current_season\":3,\"players\":[{\"name\":\"Player\",\"rank\":0,\"tier\":\"Gold Mask\",\"mmr\":1}],\"seasons\":[{\"id\":3}]}",
        "{\"current_season\":3,\"players\":[],\"seasons\":[{\"id\":1}]}"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        memset(&b, 0, sizeof b); assert(!parse_board(bad[i], &b)); free(b.seasons);
    }
}
static void player_visibility_tests(void) {
    const int ranks[] = {99, 100, 500, 1};
    const char *flags[] = {"true", "false", "false", "false"};
    for (int population_index = 0; population_index < 2; ++population_index) {
        for (unsigned i = 0; i < sizeof ranks / sizeof ranks[0]; ++i) {
            char json[512];
            snprintf(json, sizeof json,
                "{\"current_season\":1,\"seasons\":[{\"id\":1}],"
                "\"players\":[{\"name\":\"Local\",\"rank\":%d,\"tier\":\"Gold Mask\",\"mmr\":1200}],"
                "\"player\":{\"name\":\"Local\",\"rank\":%d,\"tier\":\"Gold Mask\",\"mmr\":1200,"
                "\"in_leaderboard\":%s}}", ranks[i], ranks[i], flags[i]);
            struct HofBoard parsed = {0};
            assert(parse_board(json, &parsed));
            if (population_index) include_self(&parsed);
            assert(parsed.player.rank == ranks[i] && parsed.rows[0].rank == ranks[i]);
            assert(parsed.count == 1);
            board = parsed; ready = 1; population = population_index; first_row = 0;
            unsigned char node[0x100] = {0};
            const char *tag = "OCTR_HOF_SELF";
            memcpy(node + 0x28, &tag, sizeof tag);
            walk(node, -1, 0, 0);
            assert(shown == (i == 0));
            tag = "OCTR_HOF_RANK";
            memcpy(node + 0x28, &tag, sizeof tag);
            walk(node, 0, 0, 0);
            char numeric[16]; snprintf(numeric, sizeof numeric, "%d", ranks[i]);
            assert(!strcmp(displayed, numeric));
            free(board.seasons); memset(&board, 0, sizeof board);
        }
    }
    ready = population = 0;
}
int main(void) {
    player_visibility_tests();
    friends_self_tests();
    parse_tests();
    assert(!runtime_hof_take_open());
    runtime_hof_open(); assert(runtime_hof_take_open());
    assert(!runtime_hof_take_open());
    runtime_hof_open(); runtime_hof_close(); assert(!runtime_hof_take_open());

    assert(!strcmp(runtime_hof_project("Octane_Lobby_ModeSelect"), "Octane_Lobby_ModeSelect"));
    runtime_hof_open();
    assert(!strcmp(runtime_hof_project("Octane_Lobby_Main"), "Octane_Lobby_Main"));
    assert(!strcmp(runtime_hof_project("Octane_HallOfFame"), "Octane_HallOfFame"));
    assert(!strcmp(runtime_hof_project("Octane_Lobby_ModeSelect"), "Octane_Lobby_ModeSelect"));
    assert(!strcmp(runtime_hof_project("Octane_Highscore_Select"), "Octane_Lobby_Main"));
    assert(!runtime_hof_active());
    assert(!strcmp(runtime_hof_project("Octane_Highscore_Select"), "Octane_Highscore_Select"));

    runtime_hof_open(); fetch_board(NULL);
    assert(ready && season == 3 && strstr(last_target, "count=100&season=0&name=Local"));
    unsigned char selector_text[0x100] = {0};
    const char *heading = "[NameText]", *choice = "[ItemText]";
    const char *headings[] = {"GAME MODE", "POPULATION", "SEASON"};
    const char *choices[] = {"NORMAL", "EVERYONE", "SEASON 3"};
    for (int control = 1; control <= 3; ++control) {
        memcpy(selector_text + 0x28, &heading, sizeof heading);
        walk(selector_text, -1, control, 0); assert(!strcmp(displayed, headings[control - 1]));
        memcpy(selector_text + 0x28, &choice, sizeof choice);
        walk(selector_text, -1, control, 0); assert(!strcmp(displayed, choices[control - 1]));
    }
    unsigned refreshed = generation;
    move("OCTR_HOF_MODE", 8); assert(generation == refreshed + 1 && !ready);
    fetch_board(NULL); assert(ready);
    unsigned old = generation;
    move("OCTR_HOF_SEASON", 3); assert(season == 3 && generation == old);
    assert(cycle_calls == 0);
    move("OCTR_HOF_SEASON", 2); assert(season == 2 && !ready);
    assert(cycle_calls == 1 && last_cycle == -1);
    move("OCTR_HOF_SEASON", 6); assert(season == 1);
    old = generation; move("OCTR_HOF_SEASON", 2); assert(season == 1 && generation == old);
    move("OCTR_HOF_MODE", 3); assert(mode == 1); fetch_board(NULL);
    assert(strstr(last_target, "mode=1&count=100&season=1"));
    move("OCTR_HOF_POPULATION", 3);
    old_server = 1; fetch_board(NULL); assert(failed && !ready);
    old_server = 0; move("OCTR_HOF_POPULATION", 8);
    int before = requests; fetch_board(NULL);
    assert(requests == before + 1 && strstr(last_target, "population=friends") && ready && board.count == 2);
    assert(board.rows[0].rank == 1 && board.rows[1].rank == 2);
    assert(!strcmp(board.rows[1].name, "FriendOne"));
    runtime_hof_season_changed(4); assert(!ready && season == 1);
    season = board.current; runtime_hof_season_changed(4); assert(season == 4);
    runtime_hof_open(); http = 503; fetch_board(NULL); assert(failed && !ready);
    old = generation; move("OCTR_HOF_MODE", 8); assert(generation == old + 1);
    http = 200; change_during_fetch = 1; fetch_board(NULL); assert(!runtime_hof_active() && !ready);
    runtime_hof_open(); fetch_board(NULL); assert(ready && season == 3);
    board.count = 9; move("OCTR_HOF_LIST", 1); assert(first_row == 1);
    move("OCTR_HOF_LIST", 5); assert(first_row == 1);
    move("OCTR_HOF_LIST", 0); assert(first_row == 0);
    assert(!input("OCTR_HOF_LIST", 0) && first_row == 0);
    assert(!input("OCTR_HOF_ROOT", 9)); /* Back remains native. */
    unsigned char tier_node[0x120] = {0};
    const char *tag = "OCTR_HOF_TIER";
    memcpy(tier_node + 0x28, &tag, sizeof tag);
    walk(tier_node, 0, 0, 0); assert(!strcmp(displayed, "Gold Mask"));
    tag = "OCTR_HOF_ICON_0_3"; memcpy(tier_node + 0x28, &tag, sizeof tag);
    walk(tier_node, 0, 0, 0); assert(shown);
    mode = 1; walk(tier_node, 0, 0, 0); assert(!shown);
    runtime_hof_open(); change_during_fetch = 2; fetch_board(NULL);
    assert(!ready && mode == 1 && loaded_generation != generation);
    fetch_board(NULL); assert(ready && strstr(last_target, "mode=1"));
    const char *saved = response;
    response = "{\"current_season\":4,\"seasons\":[{\"id\":4}],\"players\":[],\"player\":null}";
    runtime_hof_open(); fetch_board(NULL);
    assert(ready && board.count == 0 && board.player.rank == 0 && season == 4);
    tag = "[EmptyLeaderboard]"; memcpy(tier_node + 0x28, &tag, sizeof tag);
    walk(tier_node, -1, 0, 0); assert(shown && !strcmp(displayed, "No Data"));
    tag = "OCTR_HOF_SELF"; memcpy(tier_node + 0x28, &tag, sizeof tag);
    walk(tier_node, -1, 0, 0); assert(!shown);
    move("OCTR_HOF_POPULATION", 3); http = 401; fetch_board(NULL);
    assert(failed && !ready);
    tag = "[EmptyLeaderboard]"; memcpy(tier_node + 0x28, &tag, sizeof tag);
    walk(tier_node, -1, 0, 0); assert(shown && !strcmp(displayed, "LEADERBOARD UNAVAILABLE"));
    assert(!input("OCTR_HOF_LIST", 0));
    old = generation; move("OCTR_HOF_ROOT", 8); assert(generation == old + 1 && !failed);
    tag = "OCTR_HOF_LOADING"; memcpy(tier_node + 0x28, &tag, sizeof tag);
    walk(tier_node, -1, 0, 0); assert(shown);
    response = saved; http = 200;
    unsigned char root[0xf0] = {0};
    tag = "OCTR_HOF_ROOT"; memcpy(root + 0x28, &tag, sizeof tag);
    runtime_hof_open(); fetch_board(NULL);
    runtime_hof_update(root, (void *)0x1234); runtime_hof_update(root, (void *)0x1234);
    assert(native_ready_calls == 1 && input_activation_calls == 1);
    move("OCTR_HOF_MODE", 3); fetch_board(NULL); runtime_hof_update(root, (void *)0x1234);
    assert(input_activation_calls == 1);
    assert(native_ready_calls == 1); /* Filters must not restart the entry animation. */
    runtime_hof_close(); runtime_hof_open(); fetch_board(NULL); runtime_hof_update(root, (void *)0x1234);
    assert(native_ready_calls == 2);
    assert(input("OCTR_HOF_SEASON", 1));
    assert(input("OCTR_HOF_SEASON", 10));
    assert(list_focus_calls == 1);
    int saved_count = board.count;
    board.count = 9; first_row = 0; ready = 1;
    assert(!input("OCTR_HOF_ROW_0", 1)); /* Native focus moves to the next row. */
    assert(input("OCTR_HOF_ROW_7", 1) && first_row == 1);
    assert(input("OCTR_HOF_ROW_7", 1) && first_row == 1);
    assert(input("OCTR_HOF_ROW_0", 0) && first_row == 0);
    assert(input("OCTR_HOF_ROW_0", 0)); /* X returns to filters; Up stays in the list. */
    board.count = saved_count;
    runtime_hof_close(); free(board.seasons);
    puts("Hall of Fame API, native friends, filters, season bounds and lifecycle tests passed");
}
