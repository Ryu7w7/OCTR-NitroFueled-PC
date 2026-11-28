/* Clan tags in front of the players' names (27/11, the user: "si entras a un clan, automáticamente en el juego se
 * te añade el prefijo del clan tipo [MC] Ryu7w7"). The clans live on the RyuNET site (the server's clans.go); the
 * game asks /ryunet/clan-tags?names=a,b,c for the names it shows, a few at a time on a thread of its own, and the
 * name texts of the lobby, the karts' markers, the position list and the results read "(MC) Ryu7w7" (in
 * parentheses: the game's fonts draw "[" and "]" as the L1 and R1 buttons; the user chose them, 27/11)
 * (runtime_online_mode.c clan_name). Only shown: the nickname stays the player's identity, so whatever looks a
 * player up by the text on screen (rank badges, colors, the host's crown, voice) takes the tag off first
 * (runtime_clan_identity). A tag is 2 or 3 letters or digits, so "(XX) " never belongs to a nickname (they have
 * no parentheses or spaces). SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime_clan_tags.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define CLAN_CACHE 128
#define CLAN_BATCH 16
#define CLAN_REFRESH_MS (5u * 60u * 1000u)   /* another player's tag: asked again after 5 minutes */
#define CLAN_OWN_REFRESH_MS (60u * 1000u)    /* the player's own: a minute (joined or left on the site) */
#define CLAN_RETRY_MS (30u * 1000u)          /* no answer: tried again after 30 s */

struct clan_entry {
    char nick[17];   /* lower case */
    char tag[4];
    int state;       /* 0 free, 1 waiting for the server, 2 known */
    uint64_t asked, known, used;
};
static struct clan_entry entries[CLAN_CACHE];
static pthread_mutex_t clan_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clan_wake = PTHREAD_COND_INITIALIZER;
static int clan_started, clan_off = -1;

static uint64_t clan_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int nick_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int valid_nick(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    for (; s[n]; ++n)
        if (n >= 16 || !nick_char(s[n])) return 0;
    return n >= 3;
}

/* BB_CLAN_TAGS=0: no tags (to compare). */
static int clan_disabled(void) {
    if (clan_off < 0) {
        const char *v = getenv("BB_CLAN_TAGS");
        clan_off = v && v[0] == '0';
        if (clan_off) puts("Clan tags: off (BB_CLAN_TAGS=0)");
    }
    return clan_off;
}

const char *runtime_clan_identity(const char *text, char account[17]) {
    if (!text || (text[0] != '(' && text[0] != '[')) return text;
    const char close = text[0] == '(' ? ')' : ']';
    size_t n = 1;
    while (n <= 4 && text[n] && text[n] != close) {
        const char c = text[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return text;
        ++n;
    }
    if (n < 3 || n > 4 || text[n] != close || text[n + 1] != ' ' || !text[n + 2]) return text;
    snprintf(account, 17, "%s", text + n + 2);
    return account;
}

static void *clan_worker(void *unused) {
    (void)unused;
    for (;;) {
        char names[CLAN_BATCH][17];
        int count = 0;
        pthread_mutex_lock(&clan_lock);
        for (;;) {
            const uint64_t now = clan_now();
            count = 0;
            for (int i = 0; i < CLAN_CACHE && count < CLAN_BATCH; ++i)
                if (entries[i].state == 1 && now >= entries[i].asked + 250) memcpy(names[count++], entries[i].nick, 17);
            if (count) break;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;
            pthread_cond_timedwait(&clan_wake, &clan_lock, &until);
        }
        const uint64_t sent = clan_now();
        for (int i = 0; i < CLAN_CACHE; ++i)   /* asked now: not again until the answer or the retry */
            for (int j = 0; j < count; ++j)
                if (entries[i].state == 1 && !strcmp(entries[i].nick, names[j])) entries[i].asked = sent + CLAN_RETRY_MS;
        pthread_mutex_unlock(&clan_lock);

        char target[CLAN_BATCH * 18 + 48], body[CLAN_BATCH * 24 + 16] = "";
        int at = snprintf(target, sizeof target, "/ryunet/clan-tags?names=");
        for (int j = 0; j < count; ++j) at += snprintf(target + at, sizeof target - at, "%s%s", j ? "," : "", names[j]);
        const int status = runtime_ryunet_request("GET", target, body, sizeof body);
        if (status != 200 || strncmp(body, "ok\n", 3)) {
            static int told;
            if (!told) { told = 1; printf("Clan tags: the server did not answer (HTTP %d); tried again later\n", status); }
            continue;   /* asked stays in the future: retried after CLAN_RETRY_MS */
        }
        pthread_mutex_lock(&clan_lock);
        const uint64_t now = clan_now();
        for (int j = 0; j < count; ++j) {
            char tag[4] = "";
            /* the answer: "<nick>\t<TAG>\n" for each name in a clan */
            for (const char *line = body + 3; *line;) {
                const char *tab = strchr(line, '\t'), *end = strchr(line, '\n');
                if (!end) end = line + strlen(line);
                if (tab && tab < end && (size_t)(tab - line) == strlen(names[j]) && !strncasecmp(line, names[j], tab - line)) {
                    const size_t n = (size_t)(end - tab - 1);
                    if (n >= 2 && n <= 3) { memcpy(tag, tab + 1, n); tag[n] = 0; }
                }
                line = *end ? end + 1 : end;
            }
            for (int i = 0; i < CLAN_CACHE; ++i)
                if (entries[i].state && !strcmp(entries[i].nick, names[j])) {
                    if (strcmp(entries[i].tag, tag) || entries[i].state != 2)
                        printf("Clan tags: %s %s%s%s\n", names[j], *tag ? "is in [" : "is in no clan", tag, *tag ? "]" : "");
                    snprintf(entries[i].tag, sizeof entries[i].tag, "%s", tag);
                    entries[i].state = 2;
                    entries[i].known = now;
                }
        }
        pthread_mutex_unlock(&clan_lock);
    }
    return NULL;
}

static void clan_start_locked(void) {
    if (clan_started) return;
    clan_started = 1;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, clan_worker, NULL)) clan_started = 0;
    pthread_attr_destroy(&attr);
}

int runtime_clan_lookup(const char *nick, char tag[4]) {
    tag[0] = 0;
    if (clan_disabled() || !valid_nick(nick)) return 0;
    char lower[17];
    size_t n = 0;
    for (; nick[n] && n < 16; ++n) lower[n] = (char)((nick[n] >= 'A' && nick[n] <= 'Z') ? nick[n] + 32 : nick[n]);
    lower[n] = 0;
    const char *me = runtime_np_get_online_name();
    const int own = me && !strcasecmp(me, lower);
    const uint64_t now = clan_now();
    pthread_mutex_lock(&clan_lock);
    struct clan_entry *e = NULL, *spare = NULL;   /* spare: a free slot, else the one used longest ago */
    for (int i = 0; i < CLAN_CACHE && !e; ++i) {
        if (entries[i].state && !strcmp(entries[i].nick, lower)) e = &entries[i];
        else if (!entries[i].state) { if (!spare || spare->state) spare = &entries[i]; }
        else if (!spare || (spare->state && entries[i].used < spare->used)) spare = &entries[i];
    }
    struct clan_entry *oldest = spare;
    int known = 0;
    if (!e) {
        e = oldest;
        memset(e, 0, sizeof *e);
        memcpy(e->nick, lower, sizeof e->nick);
        e->state = 1;
        clan_start_locked();
        pthread_cond_signal(&clan_wake);
    } else {
        if (e->state == 2) {
            snprintf(tag, 4, "%s", e->tag);
            known = 1;
            if (now - e->known > (own ? CLAN_OWN_REFRESH_MS : CLAN_REFRESH_MS)) {   /* shown meanwhile, asked again */
                e->state = 1;
                e->asked = 0;
                pthread_cond_signal(&clan_wake);
            }
        } else if (e->tag[0]) {   /* being asked again: the tag it had */
            snprintf(tag, 4, "%s", e->tag);
        }
    }
    e->used = now;
    pthread_mutex_unlock(&clan_lock);
    return known;
}

void runtime_clan_label(const char *nick, char *label, size_t cap) {
    char tag[4];
    runtime_clan_lookup(nick, tag);
    /* "[MC]" when the fonts have the brackets (scripts/button_prompts.py with_brackets makes them from each font's own
     * letters and sets BB_CLAN_BRACKETS=1); else "(MC)": the game's own fonts have no "[" or "]" (drawn as boxes). */
    static int brackets = -1;
    if (brackets < 0) {
        const char *v = getenv("BB_CLAN_BRACKETS");
        brackets = v && v[0] == '1';
    }
    if (tag[0]) snprintf(label, cap, brackets ? "[%s] %s" : "(%s) %s", tag, nick);
    else snprintf(label, cap, "%s", nick ? nick : "");
}
