#define _POSIX_C_SOURCE 200809L
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
#ifdef _WIN32
#include <windows.h>
#endif

#define RANKED_RETRIES 4
#define RANKED_TIER_MEMBERS 16
#ifndef RANKED_RETRY_MS            /* the test runs the sender with 0 */
#define RANKED_RETRY_MS 4000
#endif

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static char result_text[72];         /* " +45 (1045 Gold Mask)" once scored */
static uint64_t result_room;
static struct {
    char name[17];
    int tier, mode, mmr;
} tier_members[RANKED_TIER_MEMBERS];
static unsigned tier_member_count;
static uint64_t tier_room, tier_changed_ms, tier_fetched_ms, tier_generation;
static int tier_mode, tier_dirty;

static int json_string(const char *s, const char *key, char *out, size_t size);
static int json_int(const char *s, const char *key, int *out);

static int ranked_tier_reply(const char *reply, const char *name, char *out, size_t size) {
    const char *tiers = reply ? strstr(reply, "\"tiers\":{") : NULL;
    return tiers && json_string(tiers, name, out, size);
}

static int ranked_mmr_reply(const char *reply, const char *name, int *out) {
    const char *mmrs = reply ? strstr(reply, "\"mmr\":{") : NULL;
    return mmrs && json_int(mmrs, name, out);
}

static const char *valid_chars(const char *s) {
    /* The nickname rule: 3..16 of [A-Za-z0-9_-] (nick_claims.go). */
    for (const char *p = s; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return NULL;
    if (strlen(s) < 3 || strlen(s) > 16) return NULL;
    return s;
}

static int same_nick(const char *a, const char *b) {
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) ++a, ++b;
    return !*a && !*b;
}

static const char *const tier_names[2][7] = {
    {"Bronze Wumpa", "Silver Nitro", "Gold Mask", "Platinum Trophy", "Aku Master", "Uka Legend", "Chaos Grand Prix"},
    {"Sapphire Relic", "Gold Relic", "Platinum Relic", "Sacred Fire", "Ultimate Sacred Fire", "Time Crusher", "Oxide Overlord"},
};

static int tier_index(const char *name, int mode) {
    for (int i = 0; i < 7; ++i)
        if (!strcmp(name, tier_names[mode][i])) return i + 1;
    return 0;
}

static void tier_cache_reset(uint64_t room, int mode) {
    memset(tier_members, 0, sizeof tier_members);
    tier_member_count = 0;
    tier_room = room;
    tier_mode = mode;
    tier_changed_ms = tier_fetched_ms = 0;
    tier_dirty = 0;
    ++tier_generation;
}

static void ranked_tier_context(uint64_t *room, int *mode) {
    if (!runtime_ranked_session(room, mode)) {
        *room = UINT64_MAX; /* Online menu, before a Ranked room exists. */
        *mode = runtime_public_itemless_selected() ? 1 : 0;
    }
}

int runtime_ranked_tier(const char *name, int *badge_mode) {
    uint64_t room = 0;
    int mode = 0;
    if (!valid_chars(name)) return 0;
    ranked_tier_context(&room, &mode);
    const uint64_t now = monotonic_ms();
    pthread_mutex_lock(&lock);
    if (tier_room != room || tier_mode != mode) tier_cache_reset(room, mode);
    unsigned i = 0;
    while (i < tier_member_count && !same_nick(name, tier_members[i].name)) ++i;
    if (i == tier_member_count && tier_member_count < RANKED_TIER_MEMBERS) {
        snprintf(tier_members[i].name, sizeof tier_members[i].name, "%s", name);
        ++tier_member_count;
        tier_dirty = 1;
        tier_changed_ms = now;
        ++tier_generation;
    }
    const int tier = i < tier_member_count ? tier_members[i].tier : 0;
    if (badge_mode) *badge_mode = tier ? tier_members[i].mode : 0;
    pthread_mutex_unlock(&lock);
    return tier; /* 0 selects the unranked badge. */
}

int runtime_ranked_mmr(const char *name) {
    if (!valid_chars(name)) return 0;
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < tier_member_count && !same_nick(name, tier_members[i].name)) ++i;
    const int mmr = i < tier_member_count ? tier_members[i].mmr : 0;
    pthread_mutex_unlock(&lock);
    return mmr;
}

const char *runtime_ranked_tier_name(int mode, int tier) {
    return mode >= 0 && mode < 2 && tier >= 1 && tier <= 7 ? tier_names[mode][tier - 1] : "UNRANKED";
}

static void *ranked_tier_worker(void *unused) {
    (void)unused;
    for (;;) {
        struct timespec pause = {1, 0};
        nanosleep(&pause, NULL);

        uint64_t room = 0;
        int mode = 0;
        ranked_tier_context(&room, &mode);

        char names[RANKED_TIER_MEMBERS][17];
        unsigned count = 0;
        uint64_t generation = 0;
        const uint64_t now = monotonic_ms();
        pthread_mutex_lock(&lock);
        if (tier_room != room || tier_mode != mode) tier_cache_reset(room, mode);
        const int due = tier_member_count &&
            ((tier_dirty && now - tier_changed_ms >= 250) || now - tier_fetched_ms >= 30000);
        if (due) {
            count = tier_member_count;
            for (unsigned i = 0; i < count; ++i)
                memcpy(names[i], tier_members[i].name, sizeof names[i]);
            generation = tier_generation;
            tier_dirty = 0;
            tier_fetched_ms = now;
        }
        pthread_mutex_unlock(&lock);
        if (!count) continue;

        int fetched[RANKED_TIER_MEMBERS] = {0};
        int fetched_mmr[RANKED_TIER_MEMBERS] = {0};
        int fetched_mode[RANKED_TIER_MEMBERS] = {0};
        int answered = 0;
        for (int request_mode = mode; request_mode <= mode; ++request_mode) {
            char target[384];
            size_t used = (size_t)snprintf(target, sizeof target, "/ryunet/ranked-tiers?names=");
            for (unsigned i = 0; i < count && used < sizeof target; ++i)
                used += (size_t)snprintf(target + used, sizeof target - used,
                                         "%s%s", i ? "," : "", names[i]);
            if (used >= sizeof target) break;
            snprintf(target + used, sizeof target - used, "&mode=%d", request_mode);
            char *reply = NULL;
            size_t reply_size = 0;
            const int status = runtime_ryunet_transfer("GET", target, NULL, 0,
                                                       &reply, &reply_size, 4096);
            if (status == 200 && reply) {
                answered = 1;
                for (unsigned i = 0; i < count; ++i) {
                    char lower[17], tier_name[40];
                    for (size_t j = 0; names[i][j]; ++j)
                        lower[j] = (char)tolower((unsigned char)names[i][j]);
                    lower[strlen(names[i])] = 0;
                    const int tier = ranked_tier_reply(reply, lower, tier_name, sizeof tier_name)
                                   ? tier_index(tier_name, request_mode) : 0;
                    int mmr = 0;
                    ranked_mmr_reply(reply, lower, &mmr);
                    if (tier > fetched[i]) {
                        fetched[i] = tier;
                        fetched_mode[i] = request_mode;
                        fetched_mmr[i] = mmr;
                    }
                }
            }
            free(reply);
        }
        pthread_mutex_lock(&lock);
        if (answered && tier_room == room && tier_mode == mode) {
            for (unsigned i = 0; i < count; ++i) {
                for (unsigned j = 0; j < tier_member_count; ++j)
                    if (same_nick(names[i], tier_members[j].name)) {
                        tier_members[j].tier = fetched[i];
                        tier_members[j].mode = fetched_mode[i];
                        tier_members[j].mmr = fetched_mmr[i];
                    }
            }
        }
        if (tier_generation != generation) {
            tier_dirty = 1;
            tier_changed_ms = monotonic_ms();
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
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
    static int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_RELAXED)) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, ranked_tier_worker, NULL) == 0)
        pthread_detach(thread);
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
