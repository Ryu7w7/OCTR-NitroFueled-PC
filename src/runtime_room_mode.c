/* The private room's mode (Normal / Speed) through RyuNET.
 *
 * The Game Mode selector (runtime_online_mode.c) is only seen by the player who creates the room:
 * someone who joined by invitation, Discord or the Friends tab raced with item boxes in a Speed
 * room. The host's choice now goes to the server, signed with its nickname's key
 * (POST /ryunet/room-mode?name=<host>&mode=speed|normal) when the room is created and when the
 * host picks again (Normal for any pick outside a private room: the guests taken along to a public
 * search follow it); a player who joins the room cusa13795-session-<host> follows it while inside
 * (GET /ryunet/room-mode?host=<host>) and takes it, Speed or Normal. Lobby updates run on
 * worker threads. Before loading a private track, a final read verifies and freezes the mode;
 * an unavailable server cancels that transition instead of using a local guess. */
#define _POSIX_C_SOURCE 200809L /* nanosleep with -std=c11 */
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_net.h"
#include "runtime_np_online.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void (*runtime_online_mode_chosen)(int speed, int private_room); /* runtime_online_mode.c */
extern void runtime_speed_mode_set(int enabled);                          /* runtime_file.c */

static const char session_prefix[] = "cusa13795-session-";

static int nickname_ok(const char *name) {
    const size_t n = strlen(name);
    if (n < 3 || n > 16) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-'))
            return 0;
    }
    return 1;
}

static void start(void *(*run)(void *), void *arg) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, run, arg) == 0) pthread_detach(thread);
    else free(arg);
}

static pthread_mutex_t publish_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned publish_generation;
static int publish_running, publish_speed;
static int publish_ok;

static void *publish(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&publish_lock);
        const unsigned generation = publish_generation;
        const int speed = publish_speed;
        pthread_mutex_unlock(&publish_lock);
        int accepted = 0;
        const char *me = runtime_np_get_online_name();
        if (me && nickname_ok(me)) {
            runtime_public_itemless_announce();
            char target[128], body[128];
            snprintf(target, sizeof target, "/ryunet/room-mode?name=%s&mode=%s&format=text", me, speed ? "speed" : "normal");
            const int status = runtime_ryunet_request("POST", target, body, sizeof body);
            accepted = status == 200 && !strcmp(body, "status ok\n");
            printf("Native Online mode: this private room is %s for the players who join (RyuNET %d)\n",
                   speed ? "ITEMLESS" : "NORMAL", status);
            fflush(stdout);
        }
        pthread_mutex_lock(&publish_lock);
        if (generation == publish_generation) {
            publish_ok = accepted;
            publish_running = 0;
            pthread_mutex_unlock(&publish_lock);
            return NULL;
        }
        pthread_mutex_unlock(&publish_lock);
    }
}

static void publish_mode(int speed) {
    pthread_mutex_lock(&publish_lock);
    publish_speed = speed;
    publish_ok = 0;
    ++publish_generation;
    /* One writer preserves selection order even when HTTP requests finish late. */
    if (!publish_running) {
        pthread_t thread;
        publish_running = 1;
        if (pthread_create(&thread, NULL, publish, NULL) == 0) pthread_detach(thread);
        else publish_running = 0;
    }
    pthread_mutex_unlock(&publish_lock);
}

/* Installed before main: the selector tells this file when the player picks a mode. */
static void chosen(int speed, int private_room) {
    runtime_room_mode_left(); /* our own choice now, not another host's room */
    /* Outside a private room it is Normal: the players of our party (the private room's guests,
     * taken along to a public search) follow it too. */
    publish_mode(private_room && speed);
}
__attribute__((constructor)) static void install(void) { runtime_online_mode_chosen = chosen; }

/* The game makes its session when the player enters the online menus (before choosing private
 * or public): Speed only for a private room already chosen as Speed, Normal otherwise, which also
 * replaces an old choice left on the server. */
void runtime_room_mode_hosting(void) {
    runtime_room_mode_left();
    publish_mode(runtime_online_mode_private_speed() && runtime_online_mode_in_private());
}

/* The joining player follows the host's room while in it: the host can pick Normal or Speed again
 * with everyone inside, and the track is read with the mode in force when it loads. One question
 * every 2 seconds (a few bytes); a new join or leaving the room ends it (the generation changes). */
static unsigned follow_generation;
static pthread_mutex_t follow_lock = PTHREAD_MUTEX_INITIALIZER;
static char followed_host[24];
static int race_frozen;

int runtime_room_mode_following(void) {
    pthread_mutex_lock(&follow_lock);
    const int active = followed_host[0] != 0;
    pthread_mutex_unlock(&follow_lock);
    return active;
}

int runtime_room_mode_ready(void) {
    if (runtime_room_mode_following()) return 1;
    pthread_mutex_lock(&publish_lock);
    const int ready = publish_ok && !publish_running;
    const int retry = !publish_running && !publish_ok;
    const int speed = publish_speed;
    pthread_mutex_unlock(&publish_lock);
    if (retry) publish_mode(speed);
    return ready;
}

static int read_mode(const char *host) {
    char target[128], body[128] = "", word[16], extra;
    snprintf(target, sizeof target, "/ryunet/room-mode?host=%s&format=text", host);
    const int status = runtime_ryunet_request("GET", target, body, sizeof body);
    if (status != 200 || sscanf(body, "mode %15s %c", word, &extra) != 1) return -1;
    return !strcmp(word, "speed") ? 1 : !strcmp(word, "normal") ? 0 : -1;
}

typedef struct { char host[24]; unsigned generation, publication, epoch; int guest; } VerifyMode;
static int verify_running, verify_done, verify_speed;
static unsigned verify_epoch;
static VerifyMode verification;
static void *verify_mode(void *arg) {
    VerifyMode *job=arg;
    int speed=read_mode(job->host);
    pthread_mutex_lock(&follow_lock);
    if (job->epoch == verify_epoch) {
        verification=*job; verify_speed=speed; verify_done=1;
    }
    verify_running=0;
    pthread_mutex_unlock(&follow_lock);
    free(job); return NULL;
}

/* -1 keeps the pending transition in the lobby while the worker verifies it. */
int runtime_room_mode_begin_race(void) {
    pthread_mutex_lock(&follow_lock);
    if (race_frozen) { pthread_mutex_unlock(&follow_lock); return 1; }
    if (verify_running) { pthread_mutex_unlock(&follow_lock); return -1; }
    if (verify_done) {
        verify_done=0;
        pthread_mutex_lock(&publish_lock);
        int valid=verification.generation==follow_generation && verify_speed>=0 &&
            (verification.guest || (verification.publication==publish_generation && publish_ok &&
                                    verify_speed==runtime_speed_mode_enabled()));
        pthread_mutex_unlock(&publish_lock);
        if (valid) {
            race_frozen=1;
            if (verification.guest) runtime_online_mode_adopt(verify_speed);
        }
        pthread_mutex_unlock(&follow_lock);
        return valid;
    }
    VerifyMode *job=malloc(sizeof *job);
    if (!job) { pthread_mutex_unlock(&follow_lock); return 0; }
    job->guest=followed_host[0]!=0;
    const char *name=job->guest ? followed_host : runtime_np_get_online_name();
    snprintf(job->host,sizeof job->host,"%s",name ? name : "");
    job->generation=follow_generation; job->epoch=verify_epoch;
    pthread_mutex_lock(&publish_lock); job->publication=publish_generation; pthread_mutex_unlock(&publish_lock);
    if (!nickname_ok(job->host)) { free(job); pthread_mutex_unlock(&follow_lock); return 0; }
    pthread_t worker; verify_running=1;
    if (pthread_create(&worker,NULL,verify_mode,job)) { free(job); verify_running=0; }
    int result=verify_running ? -1 : 0;
    pthread_mutex_unlock(&follow_lock);
    return result;
}

void runtime_room_mode_end_race(void) {
    pthread_mutex_lock(&follow_lock);
    race_frozen = 0; verify_done=0; ++verify_epoch;
    pthread_mutex_unlock(&follow_lock);
}

typedef struct {
    char host[24];
    unsigned generation;
} Follow;

static int following(const Follow *f) {
    return __atomic_load_n(&follow_generation, __ATOMIC_ACQUIRE) == f->generation;
}

static void follow_update(const Follow *f, int speed) {
    pthread_mutex_lock(&follow_lock);
    if (speed >= 0 && following(f) && !race_frozen && speed != runtime_speed_mode_enabled())
        runtime_online_mode_adopt(speed);
    pthread_mutex_unlock(&follow_lock);
}

static void *follow(void *arg) {
    Follow *f = arg;
    runtime_public_itemless_announce();
    for (unsigned asked = 0; following(f) && asked < 12u * 3600u / 2u; ++asked) {
        const int speed = read_mode(f->host);
        follow_update(f, speed);
        const struct timespec pause = {2, 0};
        nanosleep(&pause, NULL);
    }
    free(f);
    return NULL;
}

void runtime_room_mode_joined(const char *session_id) {
    runtime_public_itemless_party_ready();
    if (!session_id || strncmp(session_id, session_prefix, sizeof session_prefix - 1)) return;
    const char *host = session_id + sizeof session_prefix - 1;
    const char *me = runtime_np_get_online_name();
    if (!nickname_ok(host) || (me && !strcmp(host, me))) return; /* not a player's room, or ours */
    Follow *f = malloc(sizeof *f);
    if (!f) return;
    snprintf(f->host, sizeof f->host, "%s", host);
    pthread_mutex_lock(&follow_lock);
    if (!strcmp(followed_host, f->host)) {
        pthread_mutex_unlock(&follow_lock);
        free(f);
        return;
    }
    f->generation = __atomic_add_fetch(&follow_generation, 1, __ATOMIC_ACQ_REL);
    memcpy(followed_host, f->host, sizeof followed_host);
    race_frozen = 0;
    pthread_mutex_unlock(&follow_lock);
    start(follow, f);
}

/* Leaving the host's room (or picking, or hosting): this player's own choice again. */
void runtime_room_mode_left(void) {
    pthread_mutex_lock(&follow_lock);
    followed_host[0] = 0;
    race_frozen = 0;
    __atomic_add_fetch(&follow_generation, 1, __ATOMIC_ACQ_REL);
    runtime_speed_mode_set(runtime_online_mode_private_speed());
    pthread_mutex_unlock(&follow_lock);
    runtime_race_options_leave_room();
    runtime_online_mode_leave_cup();
}

void runtime_room_mode_host_name(char out[24]) {
    pthread_mutex_lock(&follow_lock);
    const char *host=followed_host[0] ? followed_host : runtime_np_get_online_name();
    snprintf(out,24,"%s",host ? host : "");
    pthread_mutex_unlock(&follow_lock);
}
