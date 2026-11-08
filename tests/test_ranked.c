/* Ranked result reporting (src/runtime_ranked.c): the report the worker sends
 * and the lobby-card suffix it reads back. */
#define SACREDFIRE_RELEASE "v9.9.9"
#include "../src/runtime_ranked.c"
#include <assert.h>

static char sent_target[256], sent_body[1024];
static int sent, transfers;
static int transfer_status = 200;
static char transfer_reply[512] = "{\"status\":\"resolved\",\"race\":7,\"delta\":45,\"mmr\":1045,\"placements\":4,\"tier\":\"Gold Mask\"}";
static int session_room = 42;
static int session_mode = 0;

int runtime_ranked_active(void) { return 1; }
int runtime_ranked_session(uint64_t *room, int *mode) {
    if (room) *room = (uint64_t)session_room;
    if (mode) *mode = session_mode;
    return session_room != 0;
}
const char *runtime_np_get_online_name(void) { return "LocalHost"; }
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
    ++transfers;
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

static void reset_report(void) {
    pthread_mutex_lock(&lock);
    memset(&report, 0, sizeof report);
    result_text[0] = 0;
    result_room = 0;
    pthread_mutex_unlock(&lock);
    sent = transfers = 0;
}

static void test_report_and_resolution(void) {
    reset_report();
    runtime_ranked_race_end();
    pthread_mutex_lock(&lock);
    assert(report.count == 3);
    assert(!strcmp(report.track, "electron_avenue")); /* the zone name, spaces folded */
    pthread_mutex_unlock(&lock);
    ranked_tick();
    assert(sent == 1);
    assert(!strcmp(sent_target, "/ryunet/ranked-result?name=LocalHost"));
    assert(strstr(sent_body, "\"room\":42"));
    assert(strstr(sent_body, "\"mode\":0"));
    assert(strstr(sent_body, "\"track\":\"electron_avenue\""));
    assert(strstr(sent_body, "\"order\":[\"HostRacer\",\"SecondRacer\",\"ThirdRacer\"]"));
    assert(strstr(sent_body, "\"dnf\":[\"SecondRacer\"]"));
    assert(!strcmp(runtime_ranked_result_text(), " +45 (1045 Gold Mask)"));
    /* Scored once: the report is gone, no further posts. */
    ranked_tick();
    assert(sent == 1);
}

static void test_guest_pending_then_delta(void) {
    reset_report();
    strcpy(transfer_reply, "{\"status\":\"pending\"}");
    runtime_ranked_race_end();
    ranked_tick();
    assert(sent == 1);
    assert(runtime_ranked_result_text()[0] == 0); /* waits for the host */
    /* The retry waits RANKED_RETRY_MS; the test pulls it due by hand. */
    pthread_mutex_lock(&lock);
    assert(report.tries == 1);
    report.next_try_ms = 0;
    pthread_mutex_unlock(&lock);
    strcpy(transfer_reply, "{\"status\":\"resolved\",\"race\":8,\"delta\":30,\"mmr\":1030,\"placements\":4,\"tier\":\"Gold Mask\"}");
    ranked_tick();
    assert(sent == 2);
    assert(!strcmp(runtime_ranked_result_text(), " +30 (1030 Gold Mask)"));
}

static void test_room_change_clears_the_card(void) {
    reset_report();
    runtime_ranked_race_end();
    ranked_tick();
    assert(runtime_ranked_result_text()[0] != 0);
    session_room = 43; /* the next room: the last room's delta has nowhere to show */
    ranked_tick();
    assert(runtime_ranked_result_text()[0] == 0);
    session_room = 42;
}

static void test_gives_up_after_retries(void) {
    reset_report();
    transfer_status = 0; /* the server cannot be reached */
    strcpy(transfer_reply, "");
    runtime_ranked_race_end();
    for (int i = 0; i < RANKED_RETRIES + 2; ++i) {
        pthread_mutex_lock(&lock);
        report.next_try_ms = 0;
        pthread_mutex_unlock(&lock);
        ranked_tick();
    }
    assert(sent == RANKED_RETRIES);
    assert(runtime_ranked_result_text()[0] == 0);
    pthread_mutex_lock(&lock);
    assert(report.count == 0); /* dropped, no endless retrying */
    pthread_mutex_unlock(&lock);
    transfer_status = 200;
}

static void test_rejected_report_drops_out(void) {
    reset_report();
    strcpy(transfer_reply, "report does not match the room");
    transfer_status = 409;
    runtime_ranked_race_end();
    ranked_tick();
    assert(sent == 1);
    pthread_mutex_lock(&lock);
    assert(report.count == 0); /* a refusal is an answer, not a reason to retry */
    pthread_mutex_unlock(&lock);
    assert(runtime_ranked_result_text()[0] == 0);
    transfer_status = 200;
}

int main(void) {
    test_report_and_resolution();
    test_guest_pending_then_delta();
    test_room_change_clears_the_card();
    test_gives_up_after_retries();
    test_rejected_report_drops_out();
    puts("ranked-test: ok");
    return 0;
}
