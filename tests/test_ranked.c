/* Ranked result reporting (src/runtime_ranked.c): the sender's posts and the
 * lobby-card suffix it leaves. The sender runs inline here (deterministic),
 * with the retry sleep flattened to zero so the pending scenario is instant. */
#define RANKED_RETRY_MS 0
#define SACREDFIRE_RELEASE "v9.9.9"
#include "../src/runtime_ranked.c"
#include <assert.h>

void runtime_hof_season_changed(int season) { (void)season; }

static char sent_target[256], sent_body[1024];
static int sent;
static int transfer_status = 200;
static char transfer_reply[512] = "{\"status\":\"resolved\",\"race\":7,\"delta\":45,\"mmr\":1045,\"placements\":4,\"tier\":\"Gold Mask\"}";
static int tier_fetch_enabled, tier_revision, tier_gets;
static const char *status_reply = "{\"season\":2,\"enabled\":false,\"retry_after\":895}";
static int status_http = 200, status_gets;
static int session_room = 42;
static int session_mode = 0;
static const char *local_name = "LocalHost";

int runtime_ranked_active(void) { return 1; }
int runtime_ranked_session(uint64_t *room, int *mode) {
    if (room) *room = (uint64_t)session_room;
    if (mode) *mode = session_mode;
    return session_room != 0;
}
int runtime_public_itemless_selected(void) { return session_mode; }
const char *runtime_np_get_online_name(void) { return local_name; }
const char *rr_current_level(void) { return "Electron Avenue"; }

int runtime_online_mode_ranked_order(struct runtime_ranked_racer *out, int max) {
    (void)max;
    snprintf(out[0].name, sizeof out[0].name, "HostRacer");
    out[0].finished = 1;
    snprintf(out[1].name, sizeof out[1].name, "SecondRacer");
    out[1].finished = 0; /* DNF: the report's dnf list */
    snprintf(out[2].name, sizeof out[2].name, "ThirdRacer");
    out[2].finished = 1;
    return 3;
}

int runtime_ryunet_transfer(const char *method, const char *target, const void *data, size_t size,
                            char **reply, size_t *reply_size, size_t max_reply) {
    (void)size; (void)max_reply;
    if (!strcmp(method, "GET")) {
        if (!strcmp(target, "/ryunet/ranked-status")) {
            ++status_gets;
            *reply = strdup(status_reply);
            *reply_size = strlen(*reply);
            return status_http;
        }
        assert(tier_fetch_enabled);
        if (strstr(target, "/ryunet/ranked-profile?name=HostRacer&mode=all")) {
            const char *body = "{\"normal\":{\"ranked\":true,\"placements\":3,\"rank\":42},"
                               "\"itemless\":{\"ranked\":true,\"placements\":0,\"rank\":7}}";
            *reply = strdup(body);
            assert(*reply);
            *reply_size = strlen(*reply);
            return 200;
        }
        assert(strstr(target, "/ryunet/ranked-tiers?names=HostRacer&mode="));
        const int itemless = strstr(target, "&mode=1") != NULL;
        const int updated = __atomic_load_n(&tier_revision, __ATOMIC_RELAXED);
        const char *body = updated
            ? (itemless ? "{\"tiers\":{\"hostracer\":\"Sacred Fire\"},\"mmr\":{\"hostracer\":1600}}"
                        : "{\"tiers\":{\"hostracer\":\"Chaos Grand Prix\"},\"mmr\":{\"hostracer\":3100}}")
            : (itemless ? "{\"tiers\":{\"hostracer\":\"Oxide Overlord\"},\"mmr\":{\"hostracer\":3000}}"
                        : "{\"tiers\":{\"hostracer\":\"Gold Mask\"},\"mmr\":{\"hostracer\":1045}}");
        *reply = strdup(body);
        assert(*reply);
        *reply_size = strlen(*reply);
        __atomic_add_fetch(&tier_gets, 1, __ATOMIC_RELAXED);
        return 200;
    }
    assert(!strcmp(method, "POST"));
    snprintf(sent_target, sizeof sent_target, "%s", target);
    snprintf(sent_body, sizeof sent_body, "%s", (const char *)data);
    ++sent;
    *reply = malloc(strlen(transfer_reply) + 1);
    assert(*reply);
    strcpy(*reply, transfer_reply);
    *reply_size = strlen(*reply);
    return transfer_status;
}

static void test_status_contract(void) {
    int season, enabled, retry;
    assert(ranked_status_reply("{\"season\":3,\"enabled\":false,\"retry_after\":895}", &season, &enabled, &retry));
    assert(season == 3 && !enabled && retry == 895);
    assert(ranked_status_reply("{ \"enabled\": true, \"retry_after\": 0, \"season\": 4 }", &season, &enabled, &retry));
    assert(season == 4 && enabled && !retry);
    const char *bad[] = {"{}", "{\"season\":1,\"enabled\":1,\"retry_after\":0}",
        "{\"season\":1,\"enabled\":truefalse,\"retry_after\":0}",
        "{\"season\":1,\"enabled\":true,\"retry_after\":-1}",
        "{\"season\":1,\"enabled\":true,\"retry_after\":0,\"enabled\":false}",
        "{\"season\":99999999999999999999,\"enabled\":true,\"retry_after\":0}"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        assert(!ranked_status_reply(bad[i], &season, &enabled, &retry));
    status_state = 0;
    status_until = monotonic_ms() + 895000;
    assert(runtime_ranked_status(1) == 0 && !status_pending);
    status_until = 0;
    assert(runtime_ranked_status(0) == -1 && status_pending);
    status_pending = 0;
    status_state = 1;
    status_until = monotonic_ms() + 5000;
    assert(runtime_ranked_status(0) == 1);
    assert(runtime_ranked_status(1) == -1 && status_pending);
    status_pending = 0;
    status_until = 0;
}

static void test_status_refresh(void) {
    tier_member_count = 1;
    strcpy(tier_members[0].name, "HostRacer");
    tier_members[0].best_tier = 7;
    tier_members[0].mode_ready[0] = tier_members[0].mode_ready[1] = 1;
    status_season = 1;
    status_pending = 1;
    uint64_t before = monotonic_ms();
    ranked_status_fetch();
    assert(status_season == 2 && status_state == 0 && !status_pending);
    assert(status_until >= before + 895000 && tier_dirty);
    assert(tier_members[0].best_tier == -1 && !tier_members[0].mode_ready[0]);
    assert(!tier_members[0].mode_ready[1]);
    int calls = status_gets;
    assert(runtime_ranked_status(1) == 0);
    ranked_status_fetch();
    assert(status_gets == calls);
    status_until = 0;
    status_reply = "{\"season\":2,\"enabled\":true,\"retry_after\":0}";
    assert(runtime_ranked_status(0) == -1);
    ranked_status_fetch();
    assert(runtime_ranked_status(0) == 1);
    status_http = 503;
    assert(runtime_ranked_status(1) == -1);
    ranked_status_fetch();
    assert(runtime_ranked_status(0) == 0);
    status_http = 200;
    status_pending = 0;
    status_until = 0;
    tier_cache_reset(0, 0);
}

static struct ranked_outbox *make_outbox(void) {
    struct ranked_outbox *out = calloc(1, sizeof *out);
    out->room = 42;
    out->mode = 0;
    out->count = 3;
    snprintf(out->track, sizeof out->track, "electron_avenue");
    runtime_online_mode_ranked_order(out->order, 8);
    /* Honest on-screen positions: two AIs finished between first and second. */
    out->order[0].pos = 1;
    out->order[1].pos = 4;
    out->order[2].pos = 3;
    return out;
}

static void reset(void) {
    pthread_mutex_lock(&lock);
    result_text[0] = 0;
    result_room = 0;
    pthread_mutex_unlock(&lock);
    sent = 0;
}

static void test_resolution(void) {
    reset();
    ranked_sender(make_outbox());
    assert(sent == 1);
    assert(!strcmp(sent_target, "/ryunet/ranked-result?name=LocalHost"));
    assert(strstr(sent_body, "\"room\":42"));
    assert(strstr(sent_body, "\"mode\":0"));
    assert(strstr(sent_body, "\"track\":\"electron_avenue\""));
    if (!strstr(sent_body, "\"order\":[{\"name\":\"HostRacer\",\"pos\":1,\"dnf\":false},"))
        fprintf(stderr, "ACTUAL BODY: %s\n", sent_body);
    assert(strstr(sent_body, "\"order\":[{\"name\":\"HostRacer\",\"pos\":1,\"dnf\":false},"
                 "{\"name\":\"SecondRacer\",\"pos\":4,\"dnf\":true},"
                 "{\"name\":\"ThirdRacer\",\"pos\":3,\"dnf\":false}]"));
    assert(!strcmp(runtime_ranked_result_text(), " +45 (1045 Gold Mask)"));
}

static void test_guest_pending_then_delta(void) {
    reset();
    strcpy(transfer_reply, "{\"status\":\"pending\"}");
    ranked_sender(make_outbox());
    assert(sent == RANKED_RETRIES);   /* the whole budget goes on waiting for the host */
    assert(runtime_ranked_result_text()[0] == 0); /* waits for the host */
    strcpy(transfer_reply, "{\"status\":\"resolved\",\"race\":8,\"delta\":30,\"mmr\":1030,\"placements\":4,\"tier\":\"Gold Mask\"}");
    ranked_sender(make_outbox());
    assert(sent == RANKED_RETRIES + 1);
    assert(!strcmp(runtime_ranked_result_text(), " +30 (1030 Gold Mask)"));
}

static void test_room_change_clears_the_card(void) {
    reset();
    ranked_sender(make_outbox());
    assert(runtime_ranked_result_text()[0] != 0);
    session_room = 43; /* the next room: the last room's delta has nowhere to show */
    assert(runtime_ranked_result_text()[0] == 0);
    session_room = 42;
}

static void test_gives_up_after_retries(void) {
    reset();
    transfer_status = 0; /* the server cannot be reached */
    strcpy(transfer_reply, "");
    ranked_sender(make_outbox());
    assert(sent == RANKED_RETRIES);
    assert(runtime_ranked_result_text()[0] == 0);
    transfer_status = 200;
}

static void test_rejected_report_drops_out(void) {
    reset();
    strcpy(transfer_reply, "report does not match the room");
    transfer_status = 409;
    ranked_sender(make_outbox());
    assert(sent == 1); /* a refusal is an answer, not a reason to retry */
    assert(runtime_ranked_result_text()[0] == 0);
    transfer_status = 200;
}

static void test_tier_batch_response(void) {
    char tier[40];
    const char *reply = "{\"mode\":0,\"tiers\":{\"hostracer\":\"Gold Mask\"},\"mmr\":{\"hostracer\":1045}}";
    assert(ranked_tier_reply(reply, "hostracer", tier, sizeof tier));
    assert(!strcmp(tier, "Gold Mask"));
    assert(!ranked_tier_reply(reply, "missing", tier, sizeof tier));
    assert(!ranked_tier_reply("{\"hostracer\":\"Gold Mask\"}", "hostracer", tier, sizeof tier));
    int mmr = 0;
    assert(ranked_mmr_reply(reply, "hostracer", &mmr) && mmr == 1045);
    assert(!ranked_mmr_reply(reply, "missing", &mmr));
}

static void test_tier_before_first_race(void) {
    session_room = 0;
    int mode = -1;
    assert(runtime_ranked_tier("HostRacer", &mode) == 0 && mode == 0);
    assert(runtime_ranked_badge("HostRacer", &mode) == -1 && mode == 0);
    session_room = 42;
    pthread_mutex_lock(&lock);
    tier_cache_reset(0, 0);
    pthread_mutex_unlock(&lock);
    assert(runtime_ranked_tier("HostRacer", &mode) == 0 && mode == 0);
    pthread_mutex_lock(&lock);
    tier_members[0].tier = 5;
    pthread_mutex_unlock(&lock);
    tier_members[0].mode = 0;
    tier_members[0].mmr = 2468;
    tier_members[0].best_tier = 6;
    tier_members[0].best_mode = 1;
    assert(runtime_ranked_tier("HostRacer", &mode) == 5 && mode == 0);
    assert(runtime_ranked_mmr("HostRacer") == 2468);
    assert(runtime_ranked_badge("HostRacer", &mode) == 6 && mode == 1);
    assert(!strcmp(runtime_ranked_tier_name(1, 5), "Ultimate Sacred Fire"));
}

static void test_best_badge(void) {
    int mode = -1;
    assert(runtime_ranked_badge("Waiting on Player...", &mode) == -1);
    assert(runtime_ranked_badge(NULL, &mode) == -1);
    assert(!ranked_best_mode(5, 2100, 4, 2300));
    assert(ranked_best_mode(4, 2300, 5, 2100));
    assert(!ranked_best_mode(5, 2300, 5, 2100));
    assert(ranked_best_mode(5, 2100, 5, 2300));
    assert(!ranked_best_mode(0, 0, 0, 0));
}

static void test_tier_worker_refresh(void) {
    local_name = "HostRacer";
    pthread_mutex_lock(&lock);
    tier_cache_reset(0, 0);
    pthread_mutex_unlock(&lock);
    int mode = -1;
    assert(runtime_ranked_badge("HostRacer", &mode) == -1);
    tier_fetch_enabled = 1;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, ranked_tier_worker, NULL));
    for (int i = 0; i < 300; ++i) {
        if (runtime_ranked_badge("HostRacer", &mode) == 7 && mode == 1) break;
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    }
    assert(runtime_ranked_badge("HostRacer", &mode) == 7 && mode == 1);
    int tier = -1, mmr = -1, placements = -1, position = -1;
    assert(runtime_ranked_profile("HostRacer", 0, &tier, &mmr, &placements, &position));
    assert(tier == 3 && mmr == 1045 && placements == 3 && position == 42);
    assert(runtime_ranked_profile("HostRacer", 1, &tier, &mmr, &placements, &position));
    assert(tier == 7 && mmr == 3000 && placements == 0 && position == 7);
    assert(runtime_ranked_tier("HostRacer", NULL) == 3);
    assert(runtime_ranked_mmr("HostRacer") == 1045);
    __atomic_store_n(&tier_revision, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < 300; ++i) {
        if (runtime_ranked_badge("HostRacer", &mode) == 7 && mode == 0) break;
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    }
    assert(runtime_ranked_badge("HostRacer", &mode) == 7 && mode == 0);
    session_room = 0;
    for (int i = 0; i < 300; ++i) {
        if (runtime_ranked_badge("HostRacer", &mode) == 7) break;
        nanosleep(&(struct timespec){0, 10000000}, NULL);
    }
    assert(runtime_ranked_badge("HostRacer", &mode) == 7);
    session_mode = 1;
    assert(runtime_ranked_badge("HostRacer", &mode) == 7);
    session_mode = 0;
    assert(runtime_ranked_badge("HostRacer", &mode) == 7);
    assert(__atomic_load_n(&tier_gets, __ATOMIC_RELAXED) >= 4);
    pthread_cancel(thread);
    pthread_join(thread, NULL);
}

int main(void) {
    test_status_contract();
    test_status_refresh();
    test_resolution();
    test_guest_pending_then_delta();
    test_room_change_clears_the_card();
    test_gives_up_after_retries();
    test_rejected_report_drops_out();
    test_tier_batch_response();
    test_tier_before_first_race();
    test_best_badge();
    test_tier_worker_refresh();
    puts("ranked-test: ok");
    return 0;
}
