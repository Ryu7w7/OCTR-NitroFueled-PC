/* Ranked race results (docs/RANKED_SYSTEM_ARCHITECTURE_SPEC.md): the ladder.
 *
 * The host's game builds the finish order and every client's mod holds the same
 * host-validated permutation (runtime_online_mode.c capture_finish_order). At
 * the results event of a ranked race each client hands its copy to this module
 * (runtime_ranked_race_end), which hands it to its OWN short-lived sender
 * thread: one thread per race, owning a full copy of the report, so nothing
 * shared can stall it. It posts, signed:
 *
 *   POST /ryunet/ranked-result?name=<me>
 *   {"room":42,"mode":1,"track":"octane_t141...","order":["A","B"],"dnf":["B"]}
 *
 * The server scores the race when the room's own host reports (the same
 * authority the game already uses) and answers every reporter with its own
 * outcome: {"status":"resolved","race":7,"season":1,"delta":45,"mmr":1045,
 * "placements":4,"tier":"Gold Mask"}. Guests that reported a moment before the
 * host get "pending" and retry; once the race is resolved a retry carries
 * their delta, which then joins the lobby card:
 * RANKED - NORMAL  +45 (1045 Gold Mask).
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
#ifndef RANKED_RETRY_MS            /* the test runs the sender with 0 */
#define RANKED_RETRY_MS 4000
#endif

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static char result_text[72];         /* " +45 (1045 Gold Mask)" once scored */
static uint64_t result_room;

static const char *valid_chars(const char *s) {
    /* The nickname rule: 3..16 of [A-Za-z0-9_-] (nick_claims.go). */
    for (const char *p = s; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return NULL;
    if (strlen(s) < 3 || strlen(s) > 16) return NULL;
    return s;
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

    /* The sender owns a full copy of the report: the lobby state churns after a
     * race, so nothing of the room is read again once the thread is off. */
    struct ranked_outbox *out = calloc(1, sizeof *out);
    if (!out) return;
    out->room = room;
    out->mode = mode;
    out->count = count;
    memcpy(out->order, order, (size_t)count * sizeof order[0]);
    const char *level = rr_current_level();
    /* The zone's display name has spaces; the server keeps [a-z0-9_-]. */
    for (int i = 0; level && level[i] && i + 1 < (int)sizeof out->track; ++i)
        out->track[i] = (char)(isalnum((unsigned char)level[i]) ? tolower((unsigned char)level[i]) : '_');
    out->track[sizeof out->track - 1] = 0;

    pthread_t thread;
    if (pthread_create(&thread, NULL, ranked_sender, out) == 0) {
        pthread_detach(thread);
        fprintf(stderr, "Ranked: report handed to its sender (room %llu mode %d track %s, up to %d posts)\n",
                (unsigned long long)room, mode, out->track, RANKED_RETRIES);
        return;
    }
    fprintf(stderr, "Ranked: sender thread failed to start (%d)\n", errno);
    free(out);
}

/* json_int/json_string find one top-level field of the server's answer. */
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

/* The sender: owns the outbox, posts it with its retry budget, reads the
 * answers, and leaves the last scored delta for the lobby card. */
void *ranked_sender(void *arg) {
    struct ranked_outbox *out = arg;
    char target[192];
    const char *me = runtime_np_get_online_name();
    if (!me || !valid_chars(me)) {
        fprintf(stderr, "Ranked: no online name to sign the report with\n");
        free(out);
        return NULL;
    }
    snprintf(target, sizeof target, "/ryunet/ranked-result?name=%s", me);

    for (int tries = 1; tries <= RANKED_RETRIES; ++tries) {
        char body[768];
        size_t used = (size_t)snprintf(body, sizeof body,
            "{\"room\":%llu,\"mode\":%d,\"track\":\"%s\",\"order\":[",
            (unsigned long long)out->room, out->mode, out->track);
        for (int i = 0; i < out->count && used < sizeof body; ++i)
            used += (size_t)snprintf(body + used, sizeof body - used,
                                     "%s{\"name\":\"%s\",\"pos\":%d,\"dnf\":%s}",
                                     i ? "," : "", out->order[i].name, out->order[i].pos,
                                     out->order[i].finished ? "false" : "true");
        snprintf(body + used, sizeof body - used, "]}");

        char *reply = NULL;
        size_t reply_size = 0;
        const int status = runtime_ryunet_transfer("POST", target, body, strlen(body), &reply, &reply_size, 512);
        fprintf(stderr, "Ranked: POST -> %d %.160s\n", status, reply ? reply : "(no reply)");
        char state[16] = "", tier[32] = "";
        int delta = 0, mmr = 0;
        int resolved = status == 200 && reply &&
            json_string(reply, "status", state, sizeof state) && !strcmp(state, "resolved") &&
            json_int(reply, "delta", &delta) && json_int(reply, "mmr", &mmr);
        if (resolved) {
            json_string(reply, "tier", tier, sizeof tier);
            pthread_mutex_lock(&lock);
            snprintf(result_text, sizeof result_text, " %+d (%d %s)", delta, mmr, tier);
            result_room = out->room;
            pthread_mutex_unlock(&lock);
        }
        free(reply);
        if (resolved) break;
        if (status != 0 && status != 200) {
            fprintf(stderr, "Ranked: the server answered %d — not retrying\n", status);
            break;                       /* refused or void: an answer, not a reason to retry */
        }
        if (tries < RANKED_RETRIES) {
            /* nanosleep takes a DURATION: the realtime clock's timestamp would
             * sleep for forty-nine years (the original worker did exactly that). */
            struct timespec wait = {
                .tv_sec = RANKED_RETRY_MS / 1000,
                .tv_nsec = (RANKED_RETRY_MS % 1000) * 1000000L,
            };
            nanosleep(&wait, NULL);      /* the host has not reported yet */
        }
    }
    free(out);
    return NULL;
}

void runtime_ranked_start(void) {
    /* Nothing to start: each report carries its own sender thread. Kept as a
     * call site (runtime_net.c) for the day the module needs a shared worker. */
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
