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
#include "runtime_json.h"
#include "runtime_hall_of_fame.h"
#include <limits.h>
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

static uint64_t result_generation;
static struct runtime_ranked_result race_result;
static struct {
    char name[17];
    int tier, mode, mmr;
    int best_tier, best_mode;
    int mode_tier[2], mode_mmr[2], placements[2], positions[2], mode_ready[2];
    uint64_t touched;
} tier_members[RANKED_TIER_MEMBERS];
static unsigned tier_member_count;
static uint64_t tier_room, tier_changed_ms, tier_fetched_ms, tier_generation;
static int tier_mode, tier_dirty;

static int json_string(const char *s, const char *key, char *out, size_t size);
static int json_int(const char *s, const char *key, int *out);

/* Status requests never touch guest objects or block the game thread. */
static int status_state = -1, status_pending, status_season;
static uint64_t status_until;

int runtime_ranked_status(int fresh) {
    const uint64_t now = monotonic_ms();
    pthread_mutex_lock(&lock);
    if ((!status_until || now >= status_until || (fresh && status_state == 1)) && !status_pending) {
        status_pending = 1;
        status_state = -1;
    }
    const int state = status_state;
    pthread_mutex_unlock(&lock);
    return state;
}

static int status_integer(const char *reply, const char *key, int *out) {
    const char *p = runtime_json_field(reply, key);
    if (!p || !isdigit((unsigned char)*p)) return 0;
    char *end;
    errno = 0;
    long value = strtol(p, &end, 10);
    while (isspace((unsigned char)*end)) ++end;
    if (errno || value > INT_MAX || (*end != ',' && *end != '}')) return 0;
    *out = (int)value;
    return 1;
}

static int ranked_status_reply(const char *reply, int *season, int *enabled, int *retry) {
    if (!reply || !status_integer(reply, "season", season) || *season < 1 ||
        !status_integer(reply, "retry_after", retry)) return 0;
    const char *p = runtime_json_field(reply, "enabled");
    if (!p) return 0;
    size_t n = !strncmp(p, "true", 4) ? 4 : !strncmp(p, "false", 5) ? 5 : 0;
    if (!n) return 0;
    *enabled = n == 4;
    p += n;
    while (isspace((unsigned char)*p)) ++p;
    return *p == ',' || *p == '}';
}

static void ranked_status_fetch(void) {
    pthread_mutex_lock(&lock);
    const int pending = status_pending;
    pthread_mutex_unlock(&lock);
    if (!pending) return;
    char *reply = NULL;
    size_t size = 0;
    int season = 0, enabled = 0, retry = 5;
    const int valid = runtime_ryunet_transfer("GET", "/ryunet/ranked-status", NULL, 0,
        &reply, &size, 1024) == 200 && ranked_status_reply(reply, &season, &enabled, &retry);
    free(reply);
    pthread_mutex_lock(&lock);
    if (valid && season != status_season) {
        status_season = season;
        runtime_hof_season_changed(season);
        /* Keep registered names; discard ratings from the previous season. */
        for (unsigned i = 0; i < tier_member_count; ++i) {
            tier_members[i].best_tier = -1;
            tier_members[i].tier = tier_members[i].mmr = 0;
            memset(tier_members[i].mode_ready, 0, sizeof tier_members[i].mode_ready);
        }
        tier_dirty = 1;
        tier_fetched_ms = tier_changed_ms = 0;
        ++tier_generation;
    }
    status_state = valid ? enabled : 0;
    status_until = monotonic_ms() + (uint64_t)(valid && !enabled ? (retry ? retry : 1) : 5) * 1000;
    status_pending = 0;
    pthread_mutex_unlock(&lock);
}

static int ranked_tier_reply(const char *reply, const char *name, char *out, size_t size) {
    const char *tiers = reply ? strstr(reply, "\"tiers\":{") : NULL;
    return tiers && json_string(tiers, name, out, size);
}

static int ranked_mmr_reply(const char *reply, const char *name, int *out) {
    const char *mmrs = reply ? strstr(reply, "\"mmr\":{") : NULL;
    return mmrs && json_int(mmrs, name, out);
}

static int ranked_profile_detail(const char *reply, const char *section, int *placements, int *position) {
    char key[24];
    snprintf(key, sizeof key, "\"%s\":{", section);
    const char *start = reply ? strstr(reply, key) : NULL;
    const char *end = start ? strchr(start, '}') : NULL;
    if (!end) return 0;
    const char *field = strstr(start, "\"placements\":");
    *placements = field && field < end ? (int)strtol(field + 13, NULL, 10) : 5;
    field = strstr(start, "\"rank\":");
    *position = field && field < end ? (int)strtol(field + 7, NULL, 10) : 0;
    return 1;
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

static int ranked_best_mode(int normal_tier, int normal_mmr, int itemless_tier, int itemless_mmr) {
    return itemless_tier > normal_tier ||
        (itemless_tier == normal_tier && itemless_mmr > normal_mmr);
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
        *mode = 0; /* Badge uses the best of both modes; selector must not reset its cache. */
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
    if (i == tier_member_count) {
        if (tier_member_count < RANKED_TIER_MEMBERS) ++tier_member_count;
        else {
            i=0;
            for (unsigned j=1;j<tier_member_count;++j)
                if (tier_members[j].touched<tier_members[i].touched) i=j;
        }
        memset(tier_members+i,0,sizeof tier_members[i]);
        snprintf(tier_members[i].name, sizeof tier_members[i].name, "%s", name);
        tier_members[i].best_tier = -1;
        tier_members[i].placements[0] = tier_members[i].placements[1] = -1;
        tier_dirty = 1;
        tier_changed_ms = now;
        ++tier_generation;
        static unsigned queued_logs;
        if (queued_logs++ < 16)
            fprintf(stderr, "Ranked tier queued: %s room=%llu mode=%d\n",
                    name, (unsigned long long)room, mode);
    }
    tier_members[i].touched=now;
    const int tier = tier_members[i].tier;
    if (badge_mode) *badge_mode = tier ? tier_members[i].mode : 0;
    pthread_mutex_unlock(&lock);
    return tier; /* 0 selects the unranked badge. */
}

int runtime_ranked_badge(const char *name, int *badge_mode) {
    if (!name || !valid_chars(name)) return -1;
    runtime_ranked_tier(name, NULL); /* Register the player for the batch fetch. */
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < tier_member_count && !same_nick(name, tier_members[i].name)) ++i;
    const int tier = i < tier_member_count ? tier_members[i].best_tier : 0;
    if (badge_mode) *badge_mode = i < tier_member_count ? tier_members[i].best_mode : 0;
    pthread_mutex_unlock(&lock);
    return tier;
}

int runtime_ranked_profile(const char *name, int mode, int *tier, int *mmr, int *placements, int *position) {
    if (mode < 0 || mode > 1 || !valid_chars(name)) return 0;
    runtime_ranked_tier(name, NULL);
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < tier_member_count && !same_nick(name, tier_members[i].name)) ++i;
    const int ready = i < tier_member_count && tier_members[i].mode_ready[mode];
    if (ready) {
        if (tier) *tier = tier_members[i].mode_tier[mode];
        if (mmr) *mmr = tier_members[i].mode_mmr[mode];
        if (placements) *placements = tier_members[i].placements[mode];
        if (position) *position = tier_members[i].positions[mode];
    }
    pthread_mutex_unlock(&lock);
    return ready;
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
        struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);

        ranked_status_fetch();
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
            ((tier_dirty && now - tier_changed_ms >= 250) || now - tier_fetched_ms >= 1000);
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

        int fetched[2][RANKED_TIER_MEMBERS] = {{0}};
        int fetched_mmr[2][RANKED_TIER_MEMBERS] = {{0}};
        int fetched_placements[2] = {-1, -1};
        int fetched_positions[2] = {0, 0};
        char profile_name[17] = "";
        int answered = 0;
        for (int request_mode = 0; request_mode < 2; ++request_mode) {
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
            static unsigned fetch_logs;
            if (fetch_logs++ < 8)
                fprintf(stderr, "Ranked tiers: mode=%d status=%d reply=%.160s\n",
                        request_mode, status, reply ? reply : "(none)");
            if (status == 200 && reply) {
                ++answered;
                for (unsigned i = 0; i < count; ++i) {
                    char lower[17], tier_name[40];
                    for (size_t j = 0; names[i][j]; ++j)
                        lower[j] = (char)tolower((unsigned char)names[i][j]);
                    lower[strlen(names[i])] = 0;
                    const int tier = ranked_tier_reply(reply, lower, tier_name, sizeof tier_name)
                                   ? tier_index(tier_name, request_mode) : 0;
                    int mmr = 0;
                    ranked_mmr_reply(reply, lower, &mmr);
                    fetched[request_mode][i] = tier;
                    fetched_mmr[request_mode][i] = mmr;
                }
            }
            free(reply);
        }
        const char *local = runtime_np_get_online_name();
        if (local && valid_chars(local))
            for (unsigned i = 0; i < count; ++i)
                if (same_nick(names[i], local)) {
                    memcpy(profile_name, names[i], sizeof profile_name);
                    break;
                }
        if (profile_name[0]) {
            char target[96];
            snprintf(target, sizeof target, "/ryunet/ranked-profile?name=%s&mode=all", profile_name);
            char *reply = NULL;
            size_t reply_size = 0;
            if (runtime_ryunet_transfer("GET", target, NULL, 0, &reply, &reply_size, 4096) == 200 && reply) {
                ranked_profile_detail(reply, "normal", &fetched_placements[0], &fetched_positions[0]);
                ranked_profile_detail(reply, "itemless", &fetched_placements[1], &fetched_positions[1]);
            }
            free(reply);
        }
        pthread_mutex_lock(&lock);
        if (answered == 2 && tier_room == room && tier_mode == mode) {
            for (unsigned i = 0; i < count; ++i) {
                for (unsigned j = 0; j < tier_member_count; ++j)
                    if (same_nick(names[i], tier_members[j].name)) {
                        const int best = ranked_best_mode(fetched[0][i], fetched_mmr[0][i],
                                                          fetched[1][i], fetched_mmr[1][i]);
                        tier_members[j].tier = fetched[mode][i];
                        tier_members[j].mode = mode;
                        tier_members[j].mmr = fetched_mmr[mode][i];
                        tier_members[j].best_tier = fetched[best][i];
                        tier_members[j].best_mode = best;
                        for (int k = 0; k < 2; ++k) {
                            tier_members[j].mode_tier[k] = fetched[k][i];
                            tier_members[j].mode_mmr[k] = fetched_mmr[k][i];
                            tier_members[j].mode_ready[k] = 1;
                            if (same_nick(names[i], profile_name) && fetched_placements[k] >= 0) {
                                tier_members[j].placements[k] = fetched_placements[k];
                                tier_members[j].positions[k] = fetched_positions[k];
                            }
                        }
                    }
            }
        }
        if (tier_generation != generation || answered != 2) {
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
    pthread_mutex_lock(&lock);
    out->generation = result_generation;
    pthread_mutex_unlock(&lock);
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

/* Result fields must belong to the local player, never a nested participant. */
static int result_integer(const char *reply, const char *key, int *value) {
    const char *p = runtime_json_field(reply, key);
    if (!p || (*p != '-' && !isdigit((unsigned char)*p))) return 0;
    char *end;
    errno = 0;
    long n = strtol(p, &end, 10);
    while (isspace((unsigned char)*end)) ++end;
    if (errno || n < INT_MIN || n > INT_MAX || (*end != ',' && *end != '}')) return 0;
    *value = (int)n;
    return 1;
}

static int result_boolean(const char *reply, const char *key, int *value) {
    const char *p = runtime_json_field(reply, key);
    if (!p) return 0;
    size_t length = !strncmp(p, "true", 4) ? 4 : !strncmp(p, "false", 5) ? 5 : 0;
    if (!length) return 0;
    *value = length == 4;
    p += length;
    while (isspace((unsigned char)*p)) ++p;
    return *p == ',' || *p == '}';
}

static int result_string(const char *reply, const char *key, char *value, size_t size) {
    const char *p = runtime_json_field(reply, key);
    if (!p || *p++ != '"') return 0;
    size_t n = 0;
    while (*p && *p != '"' && *p != '\\' && n + 1 < size) value[n++] = *p++;
    value[n] = 0;
    return *p == '"';
}

void runtime_ranked_race_begin(void) {
    pthread_mutex_lock(&lock);
    ++result_generation;
    memset(&race_result, 0, sizeof race_result);
    pthread_mutex_unlock(&lock);
}

int runtime_ranked_result_copy(struct runtime_ranked_result *out) {
    uint64_t room = 0;
    int mode = 0;
    if (!runtime_ranked_session(&room, &mode)) return 0;
    pthread_mutex_lock(&lock);
    *out = race_result;
    if (!out->ready) out->mode = mode;
    const int ready = out->ready && out->room == room && out->mode == mode;
    pthread_mutex_unlock(&lock);
    return ready;
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
        const int status = runtime_ryunet_transfer("POST", target, body, strlen(body), &reply, &reply_size, 16384);
        fprintf(stderr, "Ranked: POST -> %d %.160s\n", status, reply ? reply : "(no reply)");
        char state[16] = "", tier[40] = "", previous[40] = "";
        struct runtime_ranked_result result = {.room = out->room,
            .generation = out->generation, .mode = out->mode};
        int resolved = status == 200 && reply &&
            result_string(reply, "status", state, sizeof state) && !strcmp(state, "resolved") &&
            result_integer(reply, "delta", &result.delta);
        if (resolved) {
            if (runtime_json_field(reply, "ranked"))
                resolved = result_boolean(reply, "ranked", &result.ranked);
            else result.ranked = runtime_json_field(reply, "mmr") != NULL;
            if (runtime_json_field(reply, "ranked_before"))
                resolved = resolved && result_boolean(reply, "ranked_before", &result.ranked_before);
            if (runtime_json_field(reply, "delta_applied"))
                resolved = resolved && result_integer(reply, "delta_applied", &result.delta);
            int response_mode = out->mode;
            if (runtime_json_field(reply, "mode"))
                resolved = resolved && result_integer(reply, "mode", &response_mode) && response_mode == out->mode;
            result_integer(reply, "placements", &result.placements);
            if (result.ranked) {
                resolved = resolved && result_integer(reply, "mmr", &result.mmr) && result.mmr >= 0 &&
                    result_string(reply, "tier", tier, sizeof tier);
                result.tier = tier_index(tier, out->mode);
                resolved = resolved && result.tier > 0;
            }
            if (result.ranked_before) {
                resolved = resolved && result_integer(reply, "mmr_before", &result.before) &&
                    result_string(reply, "tier_before", previous, sizeof previous);
                result.tier_before = tier_index(previous, out->mode);
                resolved = resolved && result.tier_before > 0;
            }
        }
        if (resolved) {
            result.ready = 1;
            pthread_mutex_lock(&lock);
            if (out->generation == result_generation) {
                race_result = result;
            }
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
