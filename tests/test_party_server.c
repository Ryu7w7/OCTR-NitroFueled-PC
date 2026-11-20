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
static char g_points_request[512];
static int g_points_teams, g_points[4], g_adopted_teams, g_adopted[4];

int runtime_room_mode_race_frozen(void) { return g_frozen; }
const char *runtime_np_get_online_name(void) { return g_online_name; }
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t size) {
    ++g_requests;
    g_last_request[0] = 0;
    strncat(g_last_request, method, sizeof g_last_request - strlen(g_last_request) - 1);
    strncat(g_last_request, " ", sizeof g_last_request - strlen(g_last_request) - 1);
    strncat(g_last_request, target, sizeof g_last_request - strlen(g_last_request) - 1);
    if (strstr(target, "party-promote")) snprintf(g_promote_request, sizeof g_promote_request, "%s", g_last_request);
    if (strstr(target, "/points?")) snprintf(g_points_request, sizeof g_points_request, "%s", g_last_request);
    if (body && size) {
        size_t n = strlen(g_reply);
        if (n >= size) n = size - 1;
        memcpy(body, g_reply, n);
        body[n] = 0;
    }
    return 200;
}
void runtime_public_itemless_abort_queue(void) { /* no matchmaking in this test */ }
int runtime_online_team_points_get(int out[4]) { memcpy(out, g_points, sizeof g_points); return g_points_teams; }
void runtime_online_team_points_adopt(int teams, const int points[4]) {
    g_adopted_teams = teams;
    memcpy(g_adopted, points, sizeof g_adopted);
}
#include "../src/runtime_party_server.c"

/* The team points report runs on its own thread too. */
static void wait_points(void) {
    for (int i = 0; i < 2000; ++i) {
        pthread_mutex_lock(&lock);
        const int busy = points_busy;
        pthread_mutex_unlock(&lock);
        if (!busy) return;
        usleep(1000);
    }
    assert(!"points report never finished");
}

static void wait_poll(void);
/* One members poll read by the tick: start it, let it finish, read it (the 2 s interval skipped). */
static void poll_once(void) {
    pthread_mutex_lock(&lock);
    poll_ready = 0;
    poll_next = 0;
    pthread_mutex_unlock(&lock);
    runtime_party_server_tick();   /* starts the poll */
    wait_poll();
    pthread_mutex_lock(&lock);
    poll_next = UINT64_MAX;        /* the next tick only reads it */
    pthread_mutex_unlock(&lock);
    runtime_party_server_tick();
}

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

    /* The room's team points: a guest takes the host's from the members poll... */
    g_frozen = 0;
    strcpy(g_reply, "{\"owner\":\"guestb\",\"generation\":2,\"teams\":4,\"points\":[21,17,19,21]}");
    poll_once();
    assert(g_adopted_teams == 4 && g_adopted[0] == 21 && g_adopted[1] == 17 && g_adopted[2] == 19 && g_adopted[3] == 21);
    /* ...nonsense is not taken... */
    g_adopted_teams = 0;
    strcpy(g_reply, "{\"owner\":\"guestb\",\"generation\":2,\"teams\":4,\"points\":[21,x]}");
    poll_once();
    assert(g_adopted_teams == 0);
    /* ...and the host reports its own, once per change. */
    g_points_teams = 2;
    g_points[0] = 5; g_points[1] = 3;
    g_points_request[0] = 0;
    strcpy(g_reply, "{\"owner\":\"hosta\",\"generation\":3}");
    poll_once();
    wait_points();
    assert(strstr(g_points_request, "POST /v1/sessions/cusa13795-session-HostA/points?name=HostA&teams=2&points=5,3,0,0"));
    g_points_request[0] = 0;
    poll_once();
    wait_points();
    assert(!g_points_request[0]);   /* unchanged: not sent again */
    g_points[0] = 15;
    poll_once();
    wait_points();
    assert(strstr(g_points_request, "teams=2&points=15,3,0,0"));
    assert(g_adopted_teams == 0);   /* the host never takes points from the server */

    /* Hosting a room after joining someone else's: our own session is created, not the joined one kept
     * (the host's points went to a session it did not own, 20/11). */
    g_frozen = 0;
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
