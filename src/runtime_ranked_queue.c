/* Ranked search in the background (27/11, the user: "busqueda en segundo plano" in Ranked; meanwhile the player
 * plays anything but public matchmaking: private rooms, time trial, races with bots). The Online menu's button
 * (OCTR_RANKED_QUEUE_BUTTON, scripts/online_mode.py) and the Ranked mode chosen after it put the player in the
 * server's queue (ryunet-server-eos ranked_queue.go, /ryunet/ranked-queue): a thread of its own asks every few
 * seconds, the overlay shows "BUSCANDO PARTIDA RANKED · 5/6" on top (bbport_overlay.cpp RankedQueueBanner), and
 * when the mode's players (in rooms, searching or waiting here) reach its minimum the server says "go" with the
 * fullest room. Then the Online menu opens on Ranked and the search goes to that room (as the Discord Join:
 * runtime_public_itemless_ranked_go); the move out of what the player is doing is the next step.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void runtime_public_itemless_ranked_go(int mode, uint64_t room);   /* runtime_public_itemless.c */
int runtime_public_match_active(void);
int runtime_ranked_selected(void);

enum { QUEUE_OFF, QUEUE_WAITING, QUEUE_GO };
static int queue_state, queue_mode, queue_count, queue_needed, queue_thread_running, queue_joined;
static uint64_t queue_room, queue_go_ms, queue_started_ms;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int queue_generation;   /* a leave or a new join: an older thread's answers are dropped */

static uint64_t queue_clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int field(const char *body, const char *key) {
    const char *p = strstr(body, key);
    return p ? atoi(p + strlen(key)) : -1;
}

static void *queue_worker(void *arg) {
    const int generation = (int)(intptr_t)arg;
    int first = 1, failures = 0;
    for (;;) {
        pthread_mutex_lock(&queue_lock);
        const int stop = generation != queue_generation || queue_state != QUEUE_WAITING;
        const int mode = queue_mode;
        pthread_mutex_unlock(&queue_lock);
        if (stop) break;
        char target[160], body[256] = "";
        const char *me = runtime_np_get_online_name();
        /* ?name=: the server's signedOwner checks the signature against it (without it: "bad_request", 27/11) */
        snprintf(target, sizeof target, "/ryunet/ranked-queue?action=%s&mode=%d&name=%s", first ? "join" : "poll", mode,
                 me ? me : "");
        const int status = runtime_ryunet_request("GET", target, body, sizeof body);
        pthread_mutex_lock(&queue_lock);
        if (generation != queue_generation) { pthread_mutex_unlock(&queue_lock); break; }
        if (status == 200 && !strncmp(body, "ok\n", 3)) {
            failures = 0;
            first = !!strstr(body, "state=none");   /* the server forgot us (a restart): join again */
            queue_joined = 1;
            const int count = field(body, "count="), needed = field(body, "needed=");
            if (count >= 0) queue_count = count;
            if (needed > 0) queue_needed = needed;
            if (strstr(body, "state=go")) {
                const char *room = strstr(body, "room=");
                queue_room = room ? strtoull(room + 5, NULL, 10) : 0;
                queue_state = QUEUE_GO;
                queue_go_ms = queue_clock_ms();
                printf("Ranked queue: the server says go (%d/%d, room %llu)\n", queue_count, queue_needed,
                       (unsigned long long)queue_room);
            }
        } else if (status == 200 && !strncmp(body, "no\n", 3)) {
            printf("Ranked queue: refused (%s)\n", strstr(body, "banned") ? "banned from Ranked" :
                   strstr(body, "no_account") ? "no Discord or Steam account linked" : "bad request");
            queue_state = QUEUE_OFF;
        } else if (++failures == 5) {
            printf("Ranked queue: the server does not answer (HTTP %d); still trying\n", status);
        }
        pthread_mutex_unlock(&queue_lock);
        for (int i = 0; i < 40; ++i) {   /* 4 s, a leave stops it sooner */
            struct timespec pause = {0, 100 * 1000 * 1000};
            nanosleep(&pause, NULL);
            pthread_mutex_lock(&queue_lock);
            const int gone = generation != queue_generation || queue_state != QUEUE_WAITING;
            pthread_mutex_unlock(&queue_lock);
            if (gone) break;
        }
    }
    pthread_mutex_lock(&queue_lock);
    if (generation == queue_generation) queue_thread_running = 0;
    pthread_mutex_unlock(&queue_lock);
    return NULL;
}

static void *queue_leave_worker(void *arg) {
    char target[160], body[64] = "";
    const char *me = runtime_np_get_online_name();
    snprintf(target, sizeof target, "/ryunet/ranked-queue?action=leave&mode=%d&name=%s", (int)(intptr_t)arg, me ? me : "");
    runtime_ryunet_request("GET", target, body, sizeof body);
    return NULL;
}

static void detached(void *(*fn)(void *), void *arg) {
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, fn, arg);
    pthread_attr_destroy(&attr);
}

/* The Ranked mode chosen after the Online menu's background button (0 Normal, 1 Itemless). */
void runtime_ranked_queue_join(int mode) {
    pthread_mutex_lock(&queue_lock);
    ++queue_generation;
    queue_state = QUEUE_WAITING;
    queue_mode = mode & 1;
    queue_count = 1;
    queue_needed = 0;
    queue_room = 0;
    queue_joined = 0;
    queue_started_ms = queue_clock_ms();
    queue_thread_running = 1;
    const int generation = queue_generation;
    pthread_mutex_unlock(&queue_lock);
    printf("Ranked queue: searching Ranked %s in the background\n", mode ? "Itemless" : "Normal");
    detached(queue_worker, (void *)(intptr_t)generation);
}

void runtime_ranked_queue_leave(const char *why) {
    pthread_mutex_lock(&queue_lock);
    const int was = queue_state, mode = queue_mode;
    ++queue_generation;
    queue_state = QUEUE_OFF;
    pthread_mutex_unlock(&queue_lock);
    if (was == QUEUE_OFF) return;
    printf("Ranked queue: left (%s)\n", why ? why : "?");
    detached(queue_leave_worker, (void *)(intptr_t)mode);
}

/* 0 off, 1 waiting, 2 the match is ready. */
int runtime_ranked_queue_state(void) { return __atomic_load_n(&queue_state, __ATOMIC_SEQ_CST); }

/* Every frame on the game's thread (runtime_public_itemless_poll_ranked_request). */
void runtime_ranked_queue_tick(void) {
    static int steered;
    pthread_mutex_lock(&queue_lock);
    const int state = queue_state, mode = queue_mode;
    const uint64_t room = queue_room, go = queue_go_ms;
    pthread_mutex_unlock(&queue_lock);
    /* A Ranked search of the player's own started (Online > Ranked > Go to lobby, or the room it was sent to): out
     * of the queue, waiting or ready, so it is not counted twice. */
    if (state != QUEUE_OFF && runtime_public_match_active() && runtime_ranked_selected()) {
        runtime_ranked_queue_leave("the Ranked search started");
        steered = 0;
        return;
    }
    if (state != QUEUE_GO) { steered = 0; return; }
    if (!steered) {   /* the Online menu opens on Ranked, the next Ranked search of that mode goes to the room */
        steered = 1;
        runtime_public_itemless_ranked_go(mode, room);
    }
    if (queue_clock_ms() - go > 120000) runtime_ranked_queue_leave("the match was not joined within 2 minutes");
}

/* The overlay's banner texts, in the game's language (runtime_text, the lang folder JSON), as its notices are: 0 when
 * there is nothing to show. The overlay adds the pulsing dots. */
const char *runtime_text(const char *english);
int runtime_ranked_queue_banner(int *state, int *mode, int *count, int *needed, int *seconds);
int runtime_ranked_queue_banner_text(char *title, size_t title_size, char *detail, size_t detail_size) {
    int state = 0, mode = 0, count = 0, needed = 0, seconds = 0;
    if (!runtime_ranked_queue_banner(&state, &mode, &count, &needed, &seconds)) return 0;
    const char *kind = runtime_text(mode ? "Itemless" : "Normal");
    if (state == QUEUE_GO) {
        snprintf(title, title_size, "%s", runtime_text("RANKED MATCH READY!"));
        snprintf(detail, detail_size, "%s", runtime_text("Open Online > Ranked"));
    } else {
        snprintf(title, title_size, "%s", runtime_text("SEARCHING FOR A RANKED MATCH"));
        if (needed > 0)
            snprintf(detail, detail_size, "%s %d/%d  \xC2\xB7  %s", runtime_text("Waiting for players"), count, needed, kind);
        else
            snprintf(detail, detail_size, "%s", kind);
    }
    return state;
}

/* For the overlay: 0 nothing to show; else state (1 waiting, 2 ready), mode, count, needed and the seconds
 * since the search began (waiting) or since it was ready. */
int runtime_ranked_queue_banner(int *state, int *mode, int *count, int *needed, int *seconds) {
    pthread_mutex_lock(&queue_lock);
    const int s = queue_state;
    if (state) *state = s;
    if (mode) *mode = queue_mode;
    if (count) *count = queue_count;
    if (needed) *needed = queue_needed;
    if (seconds) *seconds = (int)((queue_clock_ms() - (s == QUEUE_GO ? queue_go_ms : queue_started_ms)) / 1000);
    pthread_mutex_unlock(&queue_lock);
    return s != QUEUE_OFF;
}
