/* The private room's mode (Normal / Speed) through RyuNET.
 *
 * The Game Mode selector (runtime_online_mode.c) is only seen by the player who creates the room:
 * someone who joined by invitation, Discord or the Friends tab raced with item boxes in a Speed
 * room. The host's choice now goes to the server, signed with its nickname's key
 * (POST /ryunet/room-mode?name=<host>&mode=speed|normal) when the room is created and when the
 * host picks again (Normal for any pick outside a private room: the guests taken along to a public
 * search follow it); a player who joins the room cusa13795-session-<host> follows it while inside
 * (GET /ryunet/room-mode?host=<host>) and takes it, Speed or Normal. Both run on their own
 * threads: the game never waits for the server. An old server or no answer leaves the joining
 * player's own choice, as before. */
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

static void *publish(void *arg) {
    const int speed = *(int *)arg;
    free(arg);
    const char *me = runtime_np_get_online_name();
    if (!me || !nickname_ok(me)) return NULL;
    char target[128], body[128];
    snprintf(target, sizeof target, "/ryunet/room-mode?name=%s&mode=%s&format=text", me, speed ? "speed" : "normal");
    const int status = runtime_ryunet_request("POST", target, body, sizeof body);
    printf("Native Online mode: this private room is %s for the players who join (RyuNET %d)\n",
           speed ? "Speed Mode" : "Normal Mode", status);
    fflush(stdout);
    return NULL;
}

static void publish_mode(int speed) {
    int *arg = malloc(sizeof *arg);
    if (!arg) return;
    *arg = speed;
    start(publish, arg);
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

typedef struct {
    char host[24];
    unsigned generation;
} Follow;

static int following(const Follow *f) {
    return __atomic_load_n(&follow_generation, __ATOMIC_ACQUIRE) == f->generation;
}

static void *follow(void *arg) {
    Follow *f = arg;
    int known = -1, warned = 0;
    for (unsigned asked = 0; following(f) && asked < 12u * 3600u / 2u; ++asked) {
        char target[128], body[128];
        snprintf(target, sizeof target, "/ryunet/room-mode?host=%s&format=text", f->host);
        const int status = runtime_ryunet_request("GET", target, body, sizeof body);
        char word[16] = "";
        if (status == 200 && sscanf(body, "mode %15s", word) == 1 &&
            (!strcmp(word, "speed") || !strcmp(word, "normal"))) {
            const int speed = !strcmp(word, "speed");
            if (speed != known && following(f)) runtime_online_mode_adopt(speed);
            known = speed;
        } else if (!warned) {
            warned = 1;
            printf("Native Online mode: %s's room mode is not known (RyuNET %d); your own choice stays\n",
                   f->host, status);
            fflush(stdout);
            if (status == 404) break; /* a server without room modes: nothing to follow */
        }
        const struct timespec pause = {2, 0};
        nanosleep(&pause, NULL);
    }
    free(f);
    return NULL;
}

void runtime_room_mode_joined(const char *session_id) {
    if (!session_id || strncmp(session_id, session_prefix, sizeof session_prefix - 1)) return;
    const char *host = session_id + sizeof session_prefix - 1;
    const char *me = runtime_np_get_online_name();
    if (!nickname_ok(host) || (me && !strcmp(host, me))) return; /* not a player's room, or ours */
    Follow *f = malloc(sizeof *f);
    if (!f) return;
    snprintf(f->host, sizeof f->host, "%s", host);
    f->generation = __atomic_add_fetch(&follow_generation, 1, __ATOMIC_ACQ_REL);
    start(follow, f);
}

/* Leaving the host's room (or picking, or hosting): this player's own choice again. */
void runtime_room_mode_left(void) {
    __atomic_add_fetch(&follow_generation, 1, __ATOMIC_ACQ_REL);
    runtime_speed_mode_set(runtime_online_mode_private_speed());
}
