/* Tests for the server-backed private-party module (runtime_party_server.c).
 * Proves the stage-2 guarantee: no native state is touched while a private race runs —
 * a host leaving mid-race defers the promote until the race ends.
 * Standalone build: include the module source and stub its 4 runtime dependencies. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Stubs the module links against. Declare before including the module. */
static int g_frozen;
static char g_last_request[512];
static int g_requests;
static char g_reply[2048];
static char g_online_name[24] = "HostA";
static char g_promote_request[512];

int runtime_room_mode_race_frozen(void) { return g_frozen; }
const char *runtime_np_get_online_name(void) { return g_online_name; }
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t size) {
    ++g_requests;
    g_last_request[0] = 0;
    strncat(g_last_request, method, sizeof g_last_request - strlen(g_last_request) - 1);
    strncat(g_last_request, " ", sizeof g_last_request - strlen(g_last_request) - 1);
    strncat(g_last_request, target, sizeof g_last_request - strlen(g_last_request) - 1);
    if (strstr(target, "party-promote")) snprintf(g_promote_request, sizeof g_promote_request, "%s", g_last_request);
    if (body && size) {
        size_t n = strlen(g_reply);
        if (n >= size) n = size - 1;
        memcpy(body, g_reply, n);
        body[n] = 0;
    }
    return 200;
}
void runtime_public_itemless_abort_queue(void) { /* no matchmaking in this test */ }
#include "../src/runtime_party_server.c"

/* The members poll runs on its own thread: wait for it to finish. */
static void wait_poll(void) {
    for (int i = 0; i < 2000; ++i) {
        pthread_mutex_lock(&lock);
        const int busy = poll_busy;
        pthread_mutex_unlock(&lock);
        if (!busy) return;
        usleep(1000);
    }
    assert(!"members poll never finished");
}

static void reset(void) {
    g_frozen = 0; g_requests = 0;
    g_last_request[0] = 0; g_reply[0] = 0;
    setenv("BB_PARTY_SERVER", "1", 1);
    runtime_party_server_init();
    /* seed a session so promote/tick have an id */
    strcpy(g_reply, "{\"sessionId\":\"cusa13795-session-HostA\"}");
    runtime_party_server_create("cusa13795-session-HostA");
}

int main(void) {
    reset();
    assert(runtime_party_server_enabled());

    /* Host leaves mid-race: no promote request is sent, promote is deferred. */
    g_frozen = 1;
    g_requests = 0;
    runtime_party_server_defer_promote("GuestB");
    assert(g_requests == 0); /* nothing touched while racing */

    /* Still racing on the next tick: poll may happen (read-only) but no promote yet. */
    runtime_party_server_tick();
    wait_poll();
    assert(strstr(g_last_request, "GET") == g_last_request); /* only the read-only poll */
    assert(!strstr(g_last_request, "party-promote"));
    /* Lobby frames within the poll interval start no other request (it once ran every frame, blocking). */
    const int polls = g_requests;
    for (int frame = 0; frame < 50; ++frame) runtime_party_server_tick();
    wait_poll();
    assert(g_requests == polls);

    /* Race ends: tick applies the deferred promote. */
    g_frozen = 0;
    g_requests = 0;
    g_promote_request[0] = 0;
    strcpy(g_reply, "{\"owner\":\"GuestB\",\"generation\":2}");
    runtime_party_server_tick();
    assert(strstr(g_promote_request, "party-promote") != NULL); /* promote now sent */
    assert(strstr(g_promote_request, "to=GuestB") != NULL);

    /* Re-creating while racing is deduped: it must NOT reset the server session
     * (that desync caused the "loading on rejoin" bug). No request is sent. */
    g_frozen = 1;
    g_requests = 0;
    strcpy(g_reply, "{\"sessionId\":\"cusa13795-session-HostA\"}");
    assert(runtime_party_server_create("cusa13795-session-HostA") == 1);
    assert(g_requests == 0); /* skipped: session already active */

    /* Hosting a room after joining someone else's: our own session is created, not the joined one kept
     * (the host's points went to a session it did not own, 20/11). */
    g_frozen = 0;
    strcpy(g_reply, "{\"sessionId\":\"cusa13795-session-tachoghc\"}");
    runtime_party_server_join("cusa13795-session-TachoGHC");
    g_requests = 0;
    strcpy(g_reply, "{\"sessionId\":\"cusa13795-session-hosta\"}");
    assert(runtime_party_server_create("cusa13795-session-HostA") == 1);
    assert(g_requests == 1 && strstr(g_last_request, "POST /v1/sessions?name=HostA"));
    assert(!strcmp(session_id, "cusa13795-session-hosta"));
    g_requests = 0;
    assert(runtime_party_server_create("cusa13795-session-HostA") == 1);   /* our own: kept */
    assert(g_requests == 0);

    unsetenv("BB_PARTY_SERVER");
    puts("Party server: race-safety tests passed");
    return 0;
}
