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
 * "placements":4,"tier":"Golden Wumpa"}. Guests that reported a moment before the
 * host get "pending" and retry; once the race is resolved a retry carries
 * their delta, which then joins the lobby card:
 * RANKED - NORMAL  +45 (1045 Golden Wumpa).
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
#include "runtime_net.h"

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

/* The server waits for every racer's report (or RYUNET_RANKED_WINDOW_S, 12 s) before it scores the race:
 * 8 posts 3 s apart outlast that window. */
#define RANKED_RETRIES 8
#define RANKED_TIER_MEMBERS 16
#ifndef RANKED_RETRY_MS            /* the test runs the sender with 0 */
#define RANKED_RETRY_MS 3000
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
/* Ranked needs a nickname linked to Discord or Steam (the server's ranked_account.go): 1 this
 * player may enter, 0 they must link an account first, -1 not known yet. A server that does not
 * say (older, or the request was not signed) lets them in; its admission check still refuses. */
static int status_account = -1;

int runtime_ranked_account(void) {
    pthread_mutex_lock(&lock);
    const int account = status_account;
    pthread_mutex_unlock(&lock);
    return account;
}

/* A ban from Ranked only (the server's ranked_bans.go, made in its admin panel): the reason and
 * when it ends (monotonic ms, 0 permanent), from the signed status. */
static int status_banned;
static char status_ban_reason[256];
static uint64_t status_ban_until;

int runtime_ranked_ban(char *reason, size_t size, long *left_seconds) {
    pthread_mutex_lock(&lock);
    const uint64_t now = monotonic_ms();
    int banned = status_banned && (!status_ban_until || now < status_ban_until);
    if (banned) {
        if (reason && size) snprintf(reason, size, "%s", status_ban_reason);
        if (left_seconds) *left_seconds = status_ban_until ? (long)((status_ban_until - now + 999) / 1000) : 0;
    }
    pthread_mutex_unlock(&lock);
    return banned;
}

int runtime_ranked_status(int fresh) {
    const uint64_t now = monotonic_ms();
    pthread_mutex_lock(&lock);
    if ((!status_until || now >= status_until || (fresh && status_state == 1)) && !status_pending) {
        status_pending = 1;
        /* Keep the last answer while the refresh runs on the worker: the Ranked menu's
         * Go To Lobby stays usable instead of dimming for one round trip. */
        if (!status_until) status_state = -1;
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

/* A JSON true/false field: 1 found (its value in *out), 0 missing or not a boolean */
static int status_bool(const char *reply, const char *key, int *out) {
    const char *p = runtime_json_field(reply, key);
    if (!p) return 0;
    size_t n = !strncmp(p, "true", 4) ? 4 : !strncmp(p, "false", 5) ? 5 : 0;
    if (!n) return 0;
    p += n;
    while (isspace((unsigned char)*p)) ++p;
    if (*p != ',' && *p != '}') return 0;
    *out = n == 4;
    return 1;
}

static int ranked_status_reply(const char *reply, int *season, int *enabled, int *retry) {
    if (!reply || !status_integer(reply, "season", season) || *season < 1 ||
        !status_integer(reply, "retry_after", retry)) return 0;
    return status_bool(reply, "enabled", enabled);
}

/* A JSON string field, unescaped (\uXXXX as UTF-8, surrogate pairs too): 1 found */
static int status_text(const char *reply, const char *key, char *out, size_t size) {
    const char *p = runtime_json_field(reply, key);
    if (!p || *p != '"' || !size) return 0;
    size_t used = 0;
    for (++p; *p && *p != '"'; ++p) {
        unsigned code = (unsigned char)*p;
        if (*p == '\\') {
            ++p;
            if (!*p) break;
            if (*p == 'u') {
                unsigned u = 0;
                for (int i = 1; i <= 4; ++i) {
                    const char c = p[i];
                    if (!isxdigit((unsigned char)c)) return 0;
                    u = u * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
                }
                p += 4;
                if (u >= 0xd800 && u < 0xdc00 && p[1] == '\\' && p[2] == 'u') {
                    unsigned low = 0;
                    int ok = 1;
                    for (int i = 3; i <= 6 && ok; ++i) {
                        const char c = p[i];
                        ok = isxdigit((unsigned char)c);
                        if (ok) low = low * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
                    }
                    if (ok && low >= 0xdc00 && low < 0xe000) {
                        u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
                        p += 6;
                    }
                }
                char utf8[4];
                size_t n = u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;
                if (n == 1) utf8[0] = (char)u;
                else if (n == 2) { utf8[0] = (char)(0xc0 | u >> 6); utf8[1] = (char)(0x80 | (u & 0x3f)); }
                else if (n == 3) { utf8[0] = (char)(0xe0 | u >> 12); utf8[1] = (char)(0x80 | ((u >> 6) & 0x3f)); utf8[2] = (char)(0x80 | (u & 0x3f)); }
                else { utf8[0] = (char)(0xf0 | u >> 18); utf8[1] = (char)(0x80 | ((u >> 12) & 0x3f));
                       utf8[2] = (char)(0x80 | ((u >> 6) & 0x3f)); utf8[3] = (char)(0x80 | (u & 0x3f)); }
                if (used + n >= size) break;
                memcpy(out + used, utf8, n);
                used += n;
                continue;
            }
            code = (unsigned char)(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? ' ' : *p);
        }
        if (used + 1 >= size) break;
        out[used++] = (char)code;
    }
    out[used] = 0;
    return 1;
}

/* 1 may enter Ranked, 0 must link an account (account_required and not account_linked) */
static int ranked_account_reply(const char *reply) {
    int required = 0, linked = 0;
    if (!status_bool(reply, "account_required", &required) || !required) return 1;
    return status_bool(reply, "account_linked", &linked) && linked;
}

static void ranked_status_fetch(void) {
    pthread_mutex_lock(&lock);
    const int pending = status_pending;
    pthread_mutex_unlock(&lock);
    if (!pending) return;
    char *reply = NULL;
    size_t size = 0;
    int season = 0, enabled = 0, retry = 5;
    /* signed with this player's nickname: the server also says whether Ranked lets them in */
    const char *me = runtime_np_get_online_name();
    char target[96] = "/ryunet/ranked-status";
    if (me && *me && strlen(me) <= 16) snprintf(target, sizeof target, "/ryunet/ranked-status?name=%s", me);
    const int valid = runtime_ryunet_transfer("GET", target, NULL, 0,
        &reply, &size, 1024) == 200 && ranked_status_reply(reply, &season, &enabled, &retry);
    const int account = valid ? ranked_account_reply(reply) : -1;
    int banned = 0, ban_left = 0;
    char ban_reason[sizeof status_ban_reason] = "";
    if (valid && status_bool(reply, "ranked_banned", &banned) && banned) {
        if (!status_text(reply, "ranked_ban_reason", ban_reason, sizeof ban_reason)) ban_reason[0] = 0;
        if (!status_integer(reply, "ranked_ban_left", &ban_left)) ban_left = 0;
    }
    free(reply);
    pthread_mutex_lock(&lock);
    if (valid) status_account = account;
    if (valid) {
        status_banned = banned;
        memcpy(status_ban_reason, ban_reason, sizeof status_ban_reason);
        status_ban_until = banned && ban_left > 0 ? monotonic_ms() + (uint64_t)ban_left * 1000u : 0;
    }
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

static int ranked_mode_field(const char *reply, const char *field, int mode, const char *name,
                             char *tier, size_t size, int *mmr) {
    char key[32], player[40];
    snprintf(key, sizeof key, "\"%s%d\":{", field, mode);
    const char *start = reply ? strstr(reply, key) : NULL;
    const char *end = start ? strchr(start, '}') : NULL;
    if (!end) return 0;
    snprintf(player, sizeof player, "\"%s\":", name);
    const char *value = strstr(start, player);
    if (!value || value >= end) return 0;
    value += strlen(player);
    if (tier) {
        if (*value++ != '"') return 0;
        size_t length = 0;
        while (value < end && *value != '"' && length + 1 < size) tier[length++] = *value++;
        tier[length] = 0;
        return value < end && *value == '"';
    }
    *mmr = (int)strtol(value, NULL, 10);
    return 1;
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

/* As RyuNET names them (its ranked_mmr.go rankedTierNames). Normal (FFA) since 2026-11-18, by S_97:
 * Bronze, Silver, Golden Wumpa, Power Crystal, Mythic Gem, Uka's Chosen, Ancient Spirit. Itemless's
 * badges by ESDRONIX; its top tier is Master Driver since 2026-11-19. */
static const char *const tier_names[2][7] = {
    {"Bronze", "Silver", "Golden Wumpa", "Power Crystal", "Mythic Gem", "Uka's Chosen", "Ancient Spirit"},
    {"Sapphire Relic", "Gold Relic", "Platinum Relic", "Sacred Fire", "Ultimate Sacred Fire", "Time Crusher", "Master Driver"},
};
/* the names before that, as an older server still says them */
static const char *const old_normal_tier_names[7] = {
    "Bronze Wumpa", "Silver Nitro", "Gold Mask", "Platinum Trophy", "Aku Master", "Uka Legend", "Chaos Grand Prix"};

static int tier_index(const char *name, int mode) {
    for (int i = 0; i < 7; ++i)
        if (!strcmp(name, tier_names[mode][i]) || (!mode && !strcmp(name, old_normal_tier_names[i]))) return i + 1;
    return mode && !strcmp(name, "Oxide Overlord") ? 7 : 0;
}

static int ranked_best_mode(int normal_tier, int normal_mmr, int itemless_tier, int itemless_mmr) {
    return itemless_tier > normal_tier ||
        (itemless_tier == normal_tier && itemless_mmr > normal_mmr);
}

static void tier_cache_switch(uint64_t room, int mode) {
    tier_room = room;
    tier_mode = mode;
    for (unsigned i = 0; i < tier_member_count; ++i) {
        tier_members[i].tier = tier_members[i].mode_ready[mode] ? tier_members[i].mode_tier[mode] : 0;
        tier_members[i].mmr = tier_members[i].mode_ready[mode] ? tier_members[i].mode_mmr[mode] : 0;
        tier_members[i].mode = mode;
    }
    tier_dirty = 1;
    tier_changed_ms = monotonic_ms();
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
    if (tier_room != room || tier_mode != mode) tier_cache_switch(room, mode);
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
        if (tier_room != room || tier_mode != mode) tier_cache_switch(room, mode);
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
        char target[384];
        size_t used = (size_t)snprintf(target, sizeof target, "/ryunet/ranked-tiers?names=");
        for (unsigned i = 0; i < count && used < sizeof target; ++i)
            used += (size_t)snprintf(target + used, sizeof target - used,
                                     "%s%s", i ? "," : "", names[i]);
        int answered = 0;
        if (used < sizeof target) {
            snprintf(target + used, sizeof target - used, "&mode=all");
            char *reply = NULL;
            size_t reply_size = 0;
            const int status = runtime_ryunet_transfer("GET", target, NULL, 0,
                                                       &reply, &reply_size, 4096);
            static unsigned fetch_logs;
            if (fetch_logs++ < 8)
                fprintf(stderr, "Ranked tiers: mode=all status=%d reply=%.160s\n",
                        status, reply ? reply : "(none)");
            if (status == 200 && reply && strstr(reply, "\"tiers0\":{") && strstr(reply, "\"tiers1\":{") &&
                strstr(reply, "\"mmr0\":{") && strstr(reply, "\"mmr1\":{")) {
                answered = 2;
                for (int request_mode = 0; request_mode < 2; ++request_mode)
                    for (unsigned i = 0; i < count; ++i) {
                        char lower[17], tier_name[40];
                        for (size_t j = 0; names[i][j]; ++j)
                            lower[j] = (char)tolower((unsigned char)names[i][j]);
                        lower[strlen(names[i])] = 0;
                        const int tier = ranked_mode_field(reply, "tiers", request_mode, lower,
                                                           tier_name, sizeof tier_name, NULL)
                                       ? tier_index(tier_name, request_mode) : 0;
                        int mmr = 0;
                        ranked_mode_field(reply, "mmr", request_mode, lower, NULL, 0, &mmr);
                        fetched[request_mode][i] = tier;
                        fetched_mmr[request_mode][i] = mmr;
                    }
            }
            free(reply);
            if (answered != 2) {
                for (int request_mode = 0; request_mode < 2; ++request_mode) {
                    snprintf(target + used, sizeof target - used, "&mode=%d", request_mode);
                    reply = NULL;
                    reply_size = 0;
                    if (runtime_ryunet_transfer("GET", target, NULL, 0, &reply, &reply_size, 4096) == 200 && reply) {
                        ++answered;
                        for (unsigned i = 0; i < count; ++i) {
                            char lower[17], tier_name[40];
                            for (size_t j = 0; names[i][j]; ++j)
                                lower[j] = (char)tolower((unsigned char)names[i][j]);
                            lower[strlen(names[i])] = 0;
                            fetched[request_mode][i] = ranked_tier_reply(reply, lower, tier_name, sizeof tier_name)
                                ? tier_index(tier_name, request_mode) : 0;
                            ranked_mmr_reply(reply, lower, &fetched_mmr[request_mode][i]);
                        }
                    }
                    free(reply);
                }
            }
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
    fprintf(stderr, "[Ranked TX] room %llu mode %d: %d racers in the report\n", (unsigned long long)room, mode, count);
    for (int i = 0; i < count; ++i)
        fprintf(stderr, "[Ranked TX]   pos %d %s%s %s time=%d laps=%d\n", order[i].pos, order[i].name,
                order[i].self ? " (me)" : "", order[i].finished ? "FINISH" : "DNF", order[i].time, order[i].laps);
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
        /* v2 (docs/RANKED_ENDRACE_DESYNC_FIX.md): every racer's finish stamp as this game saw it, and the
         * local player's own result ("self"), which no other game can drop. An older server reads the order. */
        char body[2048];
        size_t used = (size_t)snprintf(body, sizeof body,
            "{\"v\":2,\"room\":%llu,\"mode\":%d,\"track\":\"%s\",\"order\":[",
            (unsigned long long)out->room, out->mode, out->track);
        const struct runtime_ranked_racer *self = NULL;
        for (int i = 0; i < out->count && used < sizeof body; ++i) {
            used += (size_t)snprintf(body + used, sizeof body - used,
                                     "%s{\"name\":\"%s\",\"pos\":%d,\"dnf\":%s,\"time\":%d,\"dc\":%s}",
                                     i ? "," : "", out->order[i].name, out->order[i].pos,
                                     out->order[i].finished ? "false" : "true", out->order[i].time,
                                     out->order[i].dc ? "true" : "false");
            if (out->order[i].self) self = &out->order[i];
        }
        if (used < sizeof body) used += (size_t)snprintf(body + used, sizeof body - used, "]");
        if (self && used < sizeof body) {
            used += (size_t)snprintf(body + used, sizeof body - used,
                                     ",\"self\":{\"finished\":%s,\"time\":%d,\"laps\":[",
                                     self->finished ? "true" : "false", self->time);
            for (int i = 0; i < self->laps && i < 7 && used < sizeof body; ++i)
                used += (size_t)snprintf(body + used, sizeof body - used, "%s%d", i ? "," : "", self->stamps[i]);
            if (used < sizeof body) used += (size_t)snprintf(body + used, sizeof body - used, "]}");
        }
        if (used < sizeof body) snprintf(body + used, sizeof body - used, "}");
        if (tries == 1) fprintf(stderr, "[Ranked TX] %s\n", body);

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

/* ---- the rules notice on the Ranked screen (the server's ranked_rules.go, edited in its admin panel) ----
 * GET /ryunet/ranked-rules?lang=xx: "ok\n", "title\t..\n", "subtitle\t..\n" and "rule\t<no|warn|ok>\t..\n" per
 * rule in the game's language, or "off\n". Asked when the overlay first wants it once online, then every two
 * minutes while it does (bbport_overlay.cpp RankedRulesNotice); until an answer, the built-in notice. */
#define RULES_MAX 6
#define RULES_TEXT 1280
static pthread_mutex_t rules_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int state;                          /* -1 no answer yet, 0 switched off, 1 the server's */
    char title[128], subtitle[128];
    int count, kind[RULES_MAX];         /* 0 no, 1 warn, 2 ok */
    char text[RULES_MAX][RULES_TEXT];
    uint64_t next;
    int running;
} rules = {.state = -1};

/* the game's language as a code for the server (PS4 system language ids, BB_LANGUAGE; as runtime_tips.c) */
static const char *rules_language(void) {
    static const char *const CODES[] = {
        "ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "zh", "zh",
        "fi", "sv", "da", "no", "pl", "pt", "en", "tr", "es", "ar", "fr"};
    const char *v = getenv("BB_LANGUAGE");
    const int id = v ? atoi(v) : 1;
    return id >= 0 && id < (int)(sizeof CODES / sizeof *CODES) ? CODES[id] : "en";
}

static void rules_parse(char *body) {
    int state = -1, count = 0, kind[RULES_MAX] = {0};
    char title[sizeof rules.title] = "", subtitle[sizeof rules.subtitle] = "";
    static char text[RULES_MAX][RULES_TEXT];
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strcmp(line, "off")) state = 0;
        else if (!strcmp(line, "ok")) state = 1;
        else if (!strncmp(line, "title\t", 6)) snprintf(title, sizeof title, "%s", line + 6);
        else if (!strncmp(line, "subtitle\t", 9)) snprintf(subtitle, sizeof subtitle, "%s", line + 9);
        else if (!strncmp(line, "rule\t", 5) && count < RULES_MAX) {
            char *tab = strchr(line + 5, '\t');
            if (!tab || !tab[1]) continue;
            *tab = 0;
            kind[count] = !strcmp(line + 5, "no") ? 0 : !strcmp(line + 5, "ok") ? 2 : 1;
            snprintf(text[count], RULES_TEXT, "%s", tab + 1);
            ++count;
        }
    }
    if (state < 0) return;   /* not an answer: the last one (or the built-in notice) stays */
    pthread_mutex_lock(&rules_lock);
    const int changed = rules.state != state || rules.count != count;
    rules.state = state && count ? 1 : 0;
    rules.count = count;
    memcpy(rules.title, title, sizeof title);
    memcpy(rules.subtitle, subtitle, sizeof subtitle);
    memcpy(rules.kind, kind, sizeof kind);
    memcpy(rules.text, text, sizeof text);
    pthread_mutex_unlock(&rules_lock);
    if (changed) {
        printf("Ranked rules: %s from RyuNET\n", state && count ? "the notice" : "no notice");
        fflush(stdout);
    }
}

static void *rules_worker(void *unused) {
    (void)unused;
    char target[64], *body = malloc(16384);
    snprintf(target, sizeof target, "/ryunet/ranked-rules?lang=%s", rules_language());
    if (body && runtime_ryunet_request("GET", target, body, 16384) == 200) rules_parse(body);
    free(body);
    pthread_mutex_lock(&rules_lock);
    rules.running = 0;
    pthread_mutex_unlock(&rules_lock);
    return NULL;
}

int runtime_ranked_rules(char *title, size_t title_size, char *subtitle, size_t subtitle_size, int *kinds,
                         char (*texts)[RULES_TEXT], int max) {
    const uint64_t now = monotonic_ms();
    pthread_mutex_lock(&rules_lock);
    int ask = !rules.running && now >= rules.next && runtime_net_is_online();
    if (ask) {
        rules.running = 1;
        rules.next = now + 120000;
    }
    const int state = rules.state;
    int count = state == 1 ? rules.count : state;
    if (state == 1) {
        if (count > max) count = max;
        if (title && title_size) snprintf(title, title_size, "%s", rules.title);
        if (subtitle && subtitle_size) snprintf(subtitle, subtitle_size, "%s", rules.subtitle);
        for (int i = 0; i < count; ++i) {
            if (kinds) kinds[i] = rules.kind[i];
            if (texts) memcpy(texts[i], rules.text[i], RULES_TEXT);
        }
    }
    pthread_mutex_unlock(&rules_lock);
    if (ask) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, rules_worker, NULL) == 0) pthread_detach(thread);
        else { pthread_mutex_lock(&rules_lock); rules.running = 0; pthread_mutex_unlock(&rules_lock); }
    }
    return count;
}
