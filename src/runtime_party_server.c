#define _GNU_SOURCE
/* Server-backed private-room party sessions (opt-in: BB_PARTY_SERVER=1).
 *
 * The default build keeps private-room parties entirely client-side
 * (cusa13795-session-<host> + sessions/members-<sid>.txt in runtime_services.c). When a
 * host leaves, its session is deleted, every guest gets a session-delete, and the game
 * tears down/recreates the native party on each guest — corrupting the TLSF allocator.
 *
 * With BB_PARTY_SERVER=1 the party lives on the RyuNET server (server/party_session.go).
 * The host promotes a successor BEFORE quitting so the session is never deleted: no guest
 * gets a delete, no teardown runs. A guest learns it became host by polling the member
 * view and flips its native role in place.
 *
 * SAFETY: nothing here touches native objects while a race is running
 * (runtime_room_mode_race_frozen()). A promotion seen mid-race is remembered and applied
 * only after runtime_room_mode_end_race(). This is the "no bug during a private race"
 * guarantee requested for stage 2.
 */
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_json.h"
#include "runtime_np_online.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static int enabled;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char session_id[96];      /* cusa13795-session-<host>, from the server */
static char my_name[24];
static char owner_name[24];      /* current server-side owner; tells us if we're still host */
static unsigned long long seen_generation;
static int pending_promote;       /* a promotion arrived while a race was frozen */
static int was_racing;            /* last tick saw a race running */
static char deferred_successor[24]; /* host: successor to promote once the race ends */
static int matchmaking_mode;           /* this session backs a matchmaking queue, not a private room */
static int matchmaking_host_lost_done; /* requeue once when the queue's host disappears */

/* The members poll runs on a thread of its own. runtime_party_server_tick is called on every lobby frame on the
 * game's thread (runtime_public_itemless_update), and a blocking request there, up to the socket's 8 s timeout,
 * froze the room (log of 19/11 19:22: a few frames in 5 s). At most one poll every 2 s; the tick reads the latest
 * reply, and what it does with it stays on the game's thread. */
#define POLL_INTERVAL_NS 2000000000ull
static int poll_busy, poll_ready;
static uint64_t poll_next;
static char poll_target[512], poll_reply[2048];

static void *poll_members(void *unused) {
    (void)unused;
    char target[sizeof poll_target], body[sizeof poll_reply];
    pthread_mutex_lock(&lock);
    snprintf(target, sizeof target, "%s", poll_target);
    pthread_mutex_unlock(&lock);
    body[0] = 0;
    const int status = runtime_ryunet_request("GET", target, body, sizeof body);
    pthread_mutex_lock(&lock);
    if (status == 200) {
        memcpy(poll_reply, body, sizeof poll_reply);
        poll_reply[sizeof poll_reply - 1] = 0;
        poll_ready = 1;
    }
    poll_busy = 0;
    pthread_mutex_unlock(&lock);
    return NULL;
}

int runtime_party_server_enabled(void) { return enabled; }

void runtime_party_server_init(void) {
    /* Server-backed parties are the default now. BB_PARTY_SERVER=0 remains as an escape hatch
     * for debugging the old native-teardown path; no command-line flag needed to use server. */
    const char *v = getenv("BB_PARTY_SERVER");
    enabled = !(v && !strcmp(v, "0"));
    if (!enabled) return;
    fprintf(stderr, "Party server: server-backed private sessions enabled (default)\n");
}

/* Create the party session on the server. Called from the client's POST /v1/sessions
 * interception once the host picks its session id. Returns 1 when the server created it. */
int runtime_party_server_create(const char *sid) {
    if (!enabled || !sid || !*sid) return 0;
    /* The native POST /v1/sessions interception fires again on every takeover recreation.
     * Do not replace an active session: that resets the server generation and desyncs every
     * guest (the "loading on rejoin" bug). Skip when we already track a session. */
    const char *me = runtime_np_get_online_name();
    if (!me || !*me) return 0;
    /* Only our own session counts as active: the one kept from joining someone else's room
     * (cusa13795-session-TachoGHC) made every later room of ours report to it, so the host's team
     * points went to a session it did not own and the guests polled the wrong one (20/11). */
    char own[96];
    snprintf(own, sizeof own, "cusa13795-session-%s", me);
    pthread_mutex_lock(&lock);
    const int have = session_id[0] != 0 && !strcasecmp(session_id, own);
    char stale[96];
    snprintf(stale, sizeof stale, "%s", session_id);
    pthread_mutex_unlock(&lock);
    if (have) {
        fprintf(stderr, "Party server: create skipped, session already active\n");
        return 1;
    }
    if (stale[0]) fprintf(stderr, "Party server: leaving the joined session %s for our own room\n", stale);
    char target[256], body[512];
    snprintf(target, sizeof target, "/v1/sessions?name=%s", me);
    if (runtime_ryunet_request("POST", target, body, sizeof body) != 200) {
        fprintf(stderr, "Party server: create failed for %s\n", me);
        return 0;
    }
    const char *id = runtime_json_field(body, "sessionId");
    pthread_mutex_lock(&lock);
    if (id && *id == '"') {
        size_t n = 0;
        ++id;
        while (id[n] && id[n] != '"' && n + 1 < sizeof session_id) { session_id[n] = id[n]; ++n; }
        session_id[n] = 0;
    } else {
        snprintf(session_id, sizeof session_id, "%s", sid);
    }
    snprintf(my_name, sizeof my_name, "%s", me);
    snprintf(owner_name, sizeof owner_name, "%s", me); /* we just created it */
    seen_generation = 0;
    pending_promote = 0;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "Party server: created %s\n", session_id);
    return 1;
}

/* Guest joining a private room: mirror the join on the server so it knows the members. */
int runtime_party_server_join(const char *sid) {
    if (!enabled || !sid || !*sid) return 0;
    const char *me = runtime_np_get_online_name();
    if (!me || !*me) return 0;
    char target[512], body[256];
    snprintf(target, sizeof target, "/v1/sessions/%s/members?name=%s", sid, me);
    int status = runtime_ryunet_request("POST", target, body, sizeof body);
    fprintf(stderr, "Party server: join %s status=%d\n", me, status);
    /* The join response carries the session id; read it so guests poll the same id. */
    const char *id = runtime_json_field(body, "sessionId");
    pthread_mutex_lock(&lock);
    if (id && *id == '"') {
        size_t n = 0; ++id;
        while (id[n] && id[n] != '"' && n + 1 < sizeof session_id) { session_id[n] = id[n]; ++n; }
        session_id[n] = 0;
    } else {
        snprintf(session_id, sizeof session_id, "%s", sid);
    }
    snprintf(my_name, sizeof my_name, "%s", me);
    owner_name[0] = 0; /* guest: learn the real owner from the first poll */
    pthread_mutex_unlock(&lock);
    return status == 200;
}

/* Matchmaking/Ranked parties get the same server-backed session as private rooms. While the
 * party is only queueing (no ctrRoom yet) there is no server host to detect a native quit, so
 * the shared session is the only place a survivor can learn the host left. Host creates it;
 * guests join the host's deterministic id and poll it like private rooms. */
void runtime_party_server_matchmaking(int i_am_host, const char *host_name) {
    if (!enabled) return;
    pthread_mutex_lock(&lock);
    matchmaking_mode = 1;
    matchmaking_host_lost_done = 0;
    pthread_mutex_unlock(&lock);
    if (i_am_host) {
        const char *me = runtime_np_get_online_name();
        if (!me || !*me) return;
        char sid[96];
        snprintf(sid, sizeof sid, "cusa13795-session-%s", me);
        runtime_party_server_create(sid); /* dedupes: keeps the private-room session if already active */
    } else if (host_name && *host_name) {
        char sid[96];
        snprintf(sid, sizeof sid, "cusa13795-session-%s", host_name);
        runtime_party_server_join(sid);
    }
}

struct Leave {
    char target[512], me[24];
};

static void *delete_member(void *arg) {
    struct Leave *job = arg;
    char body[256];
    const int status = runtime_ryunet_request("DELETE", job->target, body, sizeof body);
    fprintf(stderr, "Party server: queue leave %s status=%d\n", job->me, status);
    free(job);
    return NULL;
}

/* Quitting a queue: drop ourselves from the shared session so the server either promotes the
 * next member or deletes the session. The survivors poll that change and requeue immediately.
 * The request goes to a thread of its own: run on the game's thread (menu_return hook) it
 * stalled the frame for the server's round trip (log 20/11 12:46: 870 ms to 129.151.105.177). */
void runtime_party_server_matchmaking_leave(void) {
    if (!enabled) return;
    pthread_mutex_lock(&lock);
    const int mm = matchmaking_mode;
    char sid[96], me[24];
    snprintf(sid, sizeof sid, "%s", session_id);
    snprintf(me, sizeof me, "%s", my_name);
    matchmaking_mode = 0;
    pthread_mutex_unlock(&lock);
    if (!mm || !sid[0] || !me[0]) return;
    struct Leave *job = calloc(1, sizeof *job);
    pthread_t worker;
    if (job) {
        snprintf(job->target, sizeof job->target, "/v1/sessions/%s/members/me?name=%s", sid, me);
        snprintf(job->me, sizeof job->me, "%s", me);
    }
    if (!job || pthread_create(&worker, NULL, delete_member, job)) {
        free(job);
        return; /* the server drops a silent member when the RyuNET connection closes */
    }
    pthread_detach(worker);
}

/* Host leaving: promote the successor on the server before the native quit.
 * successor is a nickname from the room roster (runtime_room_members). */
int runtime_party_server_promote(const char *successor) {
    if (!enabled || !successor || !*successor) return 0;
    pthread_mutex_lock(&lock);
    const int has = session_id[0] != 0;
    char sid[96], owner[24];
    snprintf(sid, sizeof sid, "%s", session_id);
    snprintf(owner, sizeof owner, "%s", owner_name);
    const char *me = my_name;
    pthread_mutex_unlock(&lock);
    if (!has) return 0;
    /* Only the current owner may promote. After an earlier promotion this client is no
     * longer host (403 from the server); skip instead of spamming a rejected request. */
    /* The server keeps nicknames in lower case (signedOwner): "ryu7w7" is "Ryu7w7". Compared
     * exactly, every host with a capital letter skipped the promotion and its leaving closed the
     * room for everyone (19/11). */
    if (owner[0] && me[0] && strcasecmp(owner, me) != 0) {
        fprintf(stderr, "Party server: promote skipped, not owner (owner=%s me=%s)\n", owner, me);
        return 0;
    }
    char target[512], body[256];
    snprintf(target, sizeof target, "/ryunet/party-promote?name=%s&sid=%s&to=%s",
             me ? me : "", sid, successor);
    int status = runtime_ryunet_request("POST", target, body, sizeof body);
    fprintf(stderr, "Party server: promote to %s status=%d\n", successor, status);
    if (status == 200) {
        pthread_mutex_lock(&lock);
        snprintf(owner_name, sizeof owner_name, "%s", successor); /* we handed it off */
        pthread_mutex_unlock(&lock);
    }
    return status == 200;
}

/* Host left during a race: remember the successor and promote once the race ends.
 * Never sends a request or touches native state while a race is running. */
void runtime_party_server_defer_promote(const char *successor) {
    if (!enabled || !successor || !*successor) return;
    pthread_mutex_lock(&lock);
    snprintf(deferred_successor, sizeof deferred_successor, "%s", successor);
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "Party server: deferred promote to %s until race ends\n", successor);
}

/* Poll the member view for a host change. Called from the lobby frame.
 * While a race runs we never touch native state: we only remember that a promotion was
 * seen. When the race ends (tick no longer frozen), that deferred promotion is applied
 * through the normal host-change path below. This keeps private races bug-free. */
void runtime_party_server_tick(void) {
    if (!enabled) return;
    const int racing = runtime_room_mode_race_frozen();

    /* A promotion that arrived during a race is applied on the first frame after it ends. */
    if (was_racing && !racing) {
        pthread_mutex_lock(&lock);
        int deferred = pending_promote;
        pending_promote = 0;
        char succ[24];
        snprintf(succ, sizeof succ, "%s", deferred_successor);
        deferred_successor[0] = 0;
        pthread_mutex_unlock(&lock);
        if (deferred) fprintf(stderr, "Party server: race ended, applying deferred host promotion\n");
        /* A host that left mid-race must promote now that it is safe. */
        if (succ[0]) {
            fprintf(stderr, "Party server: race ended, promoting deferred successor %s\n", succ);
            /* runtime_party_server_promote uses its own lock-free path; safe to call here. */
            runtime_party_server_promote(succ);
        }
    }
    was_racing = racing;

    pthread_mutex_lock(&lock);
    char sid[96], me[24];
    snprintf(sid, sizeof sid, "%s", session_id);
    snprintf(me, sizeof me, "%s", my_name);
    pthread_mutex_unlock(&lock);
    if (!sid[0] || !me[0]) return;

    /* Poll is read-only HTTP: safe even during a race. Only the role flip below is gated. */
    char target[512], body[sizeof poll_reply];
    snprintf(target, sizeof target, "/v1/sessions/%s/members?name=%s", sid, me);
    const uint64_t now = runtime_wait_clock();
    pthread_mutex_lock(&lock);
    const int start = !poll_busy && now >= poll_next;
    if (start) {
        poll_busy = 1;
        poll_next = now + POLL_INTERVAL_NS;
        snprintf(poll_target, sizeof poll_target, "%s", target);
    }
    const int ready = poll_ready;
    if (ready) {
        memcpy(body, poll_reply, sizeof body);
        poll_ready = 0;
    }
    pthread_mutex_unlock(&lock);
    if (start) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, poll_members, NULL)) {
            pthread_mutex_lock(&lock);
            poll_busy = 0;
            pthread_mutex_unlock(&lock);
        }
        pthread_attr_destroy(&attr);
    }
    if (!ready) return;

    const char *owner = runtime_json_field(body, "owner");
    const char *gen = runtime_json_field(body, "generation");
    unsigned long long g = 0;
    if (gen && *gen) g = strtoull(gen, NULL, 10);
    char ownern[24] = "";
    if (owner && *owner == '"') {
        size_t n = 0; ++owner;
        while (owner[n] && owner[n] != '"' && n + 1 < sizeof ownern) { ownern[n] = owner[n]; ++n; }
        ownern[n] = 0;
    }
    int is_host = ownern[0] && !strcasecmp(ownern, me);   /* the server's owner is lower case */

    pthread_mutex_lock(&lock);
    int changed = g != seen_generation;
    seen_generation = g;
    char prev_owner[24];
    snprintf(prev_owner, sizeof prev_owner, "%s", owner_name);
    const int mm = matchmaking_mode;
    const int mm_done = matchmaking_host_lost_done;
    /* Keep local owner in sync with the server so promote() knows if we are still host. */
    if (ownern[0]) snprintf(owner_name, sizeof owner_name, "%s", ownern);
    pthread_mutex_unlock(&lock);

    if (changed) {
        fprintf(stderr, "Party server: session generation=%llu owner=%s%s%s\n",
                g, ownern, is_host ? " (me)" : "", racing ? " [racing: deferred]" : "");
        if (is_host && racing) {
            /* Remember it; never touch native state while a race is in progress. */
            pthread_mutex_lock(&lock);
            pending_promote = 1;
            pthread_mutex_unlock(&lock);
        }
        /* The native role flip itself is driven by the game's own lobby-update notice
         * (ctrCreateLobby) on the next safe frame; this module only records intent. */
        /* A queue has no ctrRoom to push us: when the session owner changes away from the
         * host we were following, the survivor requeues in the same mode instead of bengong. */
        if (mm && !mm_done && prev_owner[0] && ownern[0] &&
            strcasecmp(prev_owner, ownern)) {   /* "Ryu7w7" (ours) and "ryu7w7" (the server's) are one */
            pthread_mutex_lock(&lock);
            matchmaking_host_lost_done = 1;
            pthread_mutex_unlock(&lock);
            fprintf(stderr, "Party server: queue host %s left (owner now %s), aborting queue\n",
                    prev_owner, ownern);
            runtime_public_itemless_abort_queue();
        }
    }
}
