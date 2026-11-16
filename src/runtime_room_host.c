/* Who hosts the room on screen, for the crown on that player's row of the room list
 * (runtime_online_mode.c ranked_member_icons; the crown is scripts/assets/host_crown.png, built
 * into the list by scripts/online_mode.py ranked_lobby_icons as OCTR_RANK_ICON_2_0).
 *
 * A private room's host is known here (runtime_room_mode.c: the host this player follows, or
 * this player). The game never shows who hosts a public or Ranked room, and only the RyuNET
 * server knows: GET /ryunet/room-host?name=<this player> answers {"host":"<nickname>"}, asked in
 * the background at most every 3 seconds while a public room's lobby is on screen.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_json.h"
#include "runtime_np_online.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char public_host[24];
static int asking;
static unsigned long long asked_ms;

static unsigned long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static int same_nick(const char *a, const char *b) {
    if (!*a || !*b) return 0;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) ++a, ++b;
    return !*a && !*b;
}

static int nick_chars(const char *s) {
    const size_t n = strlen(s);
    if (n < 3 || n > 16) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-') return 0;
    return 1;
}

static void *ask_host(void *unused) {
    (void)unused;
    char found[24] = "";
    const char *me = runtime_np_get_online_name();
    if (me && nick_chars(me)) {
        char target[64], body[256];
        snprintf(target, sizeof target, "/ryunet/room-host?name=%s", me);
        if (runtime_ryunet_request("GET", target, body, sizeof body) == 200) {
            const char *value = runtime_json_field(body, "host");
            if (value && *value == '"') {
                size_t n = 0;
                for (++value; value[n] && value[n] != '"' && n + 1 < sizeof found; ++n) found[n] = value[n];
                found[n] = 0;
                if (!nick_chars(found)) found[0] = 0;
            }
        }
    }
    pthread_mutex_lock(&lock);
    memcpy(public_host, found, sizeof public_host);
    asking = 0;
    pthread_mutex_unlock(&lock);
    return NULL;
}

int runtime_room_host_is(const char *nick) {
    if (!nick || !*nick) return 0;
    char host[24] = "";
    if (runtime_online_mode_room_on()) {
        runtime_room_mode_host_name(host);
        return same_nick(host, nick);
    }
    if (!runtime_online_mode_public_on()) {
        pthread_mutex_lock(&lock);
        public_host[0] = 0;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    const unsigned long long now = now_ms();
    pthread_mutex_lock(&lock);
    if (!asking && now - asked_ms >= 3000) {
        asking = 1;
        asked_ms = now;
        pthread_t worker;
        if (pthread_create(&worker, NULL, ask_host, NULL)) asking = 0;
        else pthread_detach(worker);
    }
    memcpy(host, public_host, sizeof host);
    pthread_mutex_unlock(&lock);
    return same_nick(host, nick);
}
