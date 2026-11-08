/* Ranked result reporting (src/runtime_ranked.c): the sender's posts and the
 * lobby-card suffix it leaves. The sender runs inline here (deterministic),
 * with the retry sleep flattened to zero so the pending scenario is instant. */
#define RANKED_RETRY_MS 0
#define SACREDFIRE_RELEASE "v9.9.9"
#include "../src/runtime_ranked.c"
#include <assert.h>

static char sent_target[256], sent_body[1024];
static int sent;
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

int main(void) {
    test_resolution();
    test_guest_pending_then_delta();
    test_room_change_clears_the_card();
    test_gives_up_after_retries();
    test_rejected_report_drops_out();
    puts("ranked-test: ok");
    return 0;
}
