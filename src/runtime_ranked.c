/* Ranked race results (docs/RANKED_SYSTEM_ARCHITECTURE_SPEC.md): the ladder.
 *
 * The host's game builds the finish order and every client's mod holds the same
 * host-validated permutation (runtime_online_mode.c capture_finish_order). At
 * the results event of a ranked race each client hands its copy to this module
 * (runtime_ranked_race_end), and a worker thread posts it signed to the server:
 *
 *   POST /ryunet/ranked-result?name=<me>
 *   {"room":42,"mode":0,"track":"electron_avenue","order":["A","B"],"dnf":["B"]}
 *
 * The server scores the race when the room's own host reports (the same
 * authority the game already uses) and answers every reporter with its own
 * outcome: {"status":"resolved","delta":45,"mmr":1045,"placements":4,
 * "tier":"Gold Mask"}. Guests that reported a moment before the host get
 * "pending" and retry; once the race is resolved a retry carries their delta,
 * which then joins the lobby card: RANKED - NORMAL  +45 (1045 Gold Mask).
 * The Python launcher stays out of it: the mod talks to RyuNET itself, exactly
 * like chat, spectate and name colors do.
 */
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include "runtime_ranked.h"
#include "runtime_replay.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RANKED_RETRIES 4
#define RANKED_RETRY_MS 4000
#define RANKED_TICK_MS 250

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct ranked_report {
    int count;                       /* racers waiting to be sent, 0 none */
    uint64_t room;                   /* captured at race end, with the queue */
    int mode;                        /* 0 Normal, 1 Itemless (the search's) */
    char track[48];
    struct runtime_ranked_racer order[8];
    int tries;                       /* posts made for this report */
    long next_try_ms;
    int diag;                        /* one diagnostic line per armed report */
};
static struct ranked_report report;

static char result_text[72];         /* " +45 (1045 Gold Mask)" once scored */
static uint64_t result_room;

static const char *valid_chars(const char *s) {
    /* The nickname rule: 3..16 of [A-Za-z0-9_-] (nick_claims.go). */
    for (const char *p = s; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return NULL;
    if (strlen(s) < 3 || strlen(s) > 16) return NULL;
    return s;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void runtime_ranked_race_end(void) {
    if (!runtime_ranked_active()) return;
    uint64_t room = 0;
    int mode = 0;
    if (!runtime_ranked_session(&room, &mode)) return;
    struct runtime_ranked_racer order[8];
    const int count = runtime_online_mode_ranked_order(order, 8);
    fprintf(stderr, "Ranked: race end, order entries %d\n", count);
    for (int i = 0; i < count; ++i)
        fprintf(stderr, "Ranked:   %d. %s%s\n", i + 1, order[i].name, order[i].finished ? "" : " (DNF)");
    if (count < 2) return;
    const char *level = rr_current_level();
    pthread_mutex_lock(&lock);
    report.count = count;
    report.room = room;              /* captured now: the lobby state churns after a race */
    report.mode = mode;
    memcpy(report.order, order, (size_t)count * sizeof order[0]);
    report.tries = 0;
    report.diag = 0;
    report.next_try_ms = now_ms();
    report.track[0] = 0;
    /* The zone's display name has spaces; the server keeps [a-z0-9_-]. */
    for (int i = 0; level && level[i] && i + 1 < (int)sizeof report.track; ++i)
        report.track[i] = (char)(isalnum((unsigned char)level[i]) ? tolower((unsigned char)level[i]) : '_');
    report.track[sizeof report.track - 1] = 0;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "Ranked: report armed (room %llu mode %d track %s, retrying up to %d)\n",
            (unsigned long long)room, mode, report.track, RANKED_RETRIES);
}

/* json_int/json_value find one top-level field of the server's answer. */
static int json_int(const char *s, const char *key, int *out) {
    char pattern[32];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char *at = strstr(s, pattern);
    if (!at) return 0;
    *out = (int)strtol(at + strlen(pattern), NULL, 10);
    return 1;
}

static int json_string(const char *s, const char *key, char *out, size_t size) {
    char pattern[32];
    snprintf(pattern, sizeof pattern, "\"%s\":\"", key);
    const char *at = strstr(s, pattern);
    if (!at) return 0;
    at += strlen(pattern);
    size_t used = 0;
    while (*at && *at != '"' && used + 1 < size) out[used++] = *at++;
    out[used] = 0;
    return 1;
}

/* One poll of the worker: send the report when due, read the answer. */
static void ranked_tick(void) {
    uint64_t room = 0;
    int mode = 0;
    const int active = runtime_ranked_session(&room, &mode);
    pthread_mutex_lock(&lock);
    if (report.count && (!active || report.tries >= RANKED_RETRIES) && !report.diag) {
        /* Once per armed report: why the first post did not go out (or why it stopped). */
        fprintf(stderr, "Ranked: tick diag — active=%d room=%llu tries=%d armed=%d\n",
                active, (unsigned long long)room, report.tries, report.count);
        report.diag = 1;
    }
    if (!active || room != result_room) {
        /* A new room: the last room's delta has nowhere to show. */
        if (result_room != room) result_text[0] = 0;
        result_room = room;
    }
    if (!report.count) {             /* nothing armed; the room state only affects the card */
        pthread_mutex_unlock(&lock);
        return;
    }
    room = report.room;              /* captured at race end: the lobby state churns after it */
    mode = report.mode;
    if (report.tries >= RANKED_RETRIES || now_ms() < report.next_try_ms) {
        pthread_mutex_unlock(&lock);
        return;
    }
    char body[768];
    size_t used = (size_t)snprintf(body, sizeof body,
        "{\"room\":%llu,\"mode\":%d,\"track\":\"%s\",\"order\":[",
        (unsigned long long)room, mode, report.track);
    for (int i = 0; i < report.count && used < sizeof body; ++i)
        used += (size_t)snprintf(body + used, sizeof body - used, "%s\"%s\"",
                                 i ? "," : "", report.order[i].name);
    used += (size_t)snprintf(body + used, sizeof body - used, "],\"dnf\":[");
    int first = 1;
    for (int i = 0; i < report.count && used < sizeof body; ++i)
        if (!report.order[i].finished) {
            used += (size_t)snprintf(body + used, sizeof body - used, "%s\"%s\"",
                                     first ? "" : ",", report.order[i].name);
            first = 0;
        }
    snprintf(body + used, sizeof body - used, "]}");

    char target[192];
    const char *me = runtime_np_get_online_name();
    if (!me || !valid_chars(me)) {
        report.count = 0;
        pthread_mutex_unlock(&lock);
        return;
    }
    snprintf(target, sizeof target, "/ryunet/ranked-result?name=%s", me);
    ++report.tries;
    report.next_try_ms = now_ms() + RANKED_RETRY_MS;
    pthread_mutex_unlock(&lock);

    char *reply = NULL;
    size_t reply_size = 0;
    const int status = runtime_ryunet_transfer("POST", target, body, strlen(body), &reply, &reply_size, 512);
    fprintf(stderr, "Ranked: POST -> %d %.160s\n", status, reply ? reply : "(no reply)");
    int drop = report.tries >= RANKED_RETRIES; /* the budget ran out either way */
    if (status == 200 && reply) {
        char state[16] = "", tier[32] = "";
        int delta = 0, mmr = 0;
        if (json_string(reply, "status", state, sizeof state) && !strcmp(state, "resolved") &&
            json_int(reply, "delta", &delta) && json_int(reply, "mmr", &mmr)) {
            json_string(reply, "tier", tier, sizeof tier);
            pthread_mutex_lock(&lock);
            snprintf(result_text, sizeof result_text, " %+d (%d %s)", delta, mmr, tier);
            result_room = room;
            pthread_mutex_unlock(&lock);
            drop = 1;
        } else if (state[0] && strcmp(state, "pending") != 0) {
            drop = 1;                /* rejected or void: an answer, not a reason to retry */
        }
    } else if (status != 0 && status != 200) {
        drop = 1;                    /* the server refused the report: retrying adds nothing */
    }
    if (drop) {
        pthread_mutex_lock(&lock);
        report.count = 0;
        pthread_mutex_unlock(&lock);
    }
    free(reply);
}

static void *worker(void *unused) {
    (void)unused;
    fprintf(stderr, "Ranked: worker started\n");
    for (;;) {
        ranked_tick();
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += RANKED_TICK_MS * 1000000L;
        if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
        nanosleep(&until, NULL);
    }
    return NULL;
}

void runtime_ranked_start(void) {
    static int started;
    if (started) return;
    started = 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) == 0) pthread_detach(thread);
    else fprintf(stderr, "Ranked: worker thread failed to start (%d)\n", errno);
}

const char *runtime_ranked_result_text(void) {
    uint64_t room = 0;
    int mode = 0;
    if (!runtime_ranked_session(&room, &mode)) return "";
    pthread_mutex_lock(&lock);
    const char *text = result_room == room && result_text[0] ? result_text : "";
    pthread_mutex_unlock(&lock);
    return text;
}
