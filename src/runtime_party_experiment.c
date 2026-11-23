/* Host handoff: the host of a room goes away and its party lives on.
 *
 * The game has no way to hand a party over: a host is whoever created the party. Native teardown
 * creates a fresh party for every remaining guest. The elected successor keeps that party; every
 * other guest joins it through the game's invitation path after its own teardown completes.
 *
 * Automatic handover applies when a host connection disappears. Private matches also hand over
 * after a manual host exit; public matchmaking and Ranked do not when the host announced return.
 * Never edit the roster, close the connection, or call party creation here. Those operations race
 * callbacks retained by native teardown and corrupt refcounted lobby objects.
 *
 * Keys, only with BB_HOST_EXPERIMENT=1 (staff builds): in a lobby,
 *   Ctrl+F9 / Ctrl+Shift+F9 report the unsafe experiment as disabled;
 *   Ctrl+F10       join the other player's room (the old host following the new one): the
 *                  Friends tab's Join (runtime_invite_join).
 * For 30 s after each, every change of the party is written to the log with the roster's names.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include "runtime_np_online.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void runtime_invite_join(const char *host); /* runtime_services.c */

static unsigned char *image;
static void **party_manager;
static int request;
static int keys; /* BB_HOST_EXPERIMENT=1: the research keys are live */
static int automatic; /* BB_HOST_TAKEOVER=0 turns the automatic handover off */
static int takeover_pending;
static uint64_t takeover_after, takeover_until;
static int takeover_local;
static char takeover_successor[24];
static uint64_t watch_until, next_look;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *at(const void *p, size_t offset) { return p ? *(void *const *)((const char *)p + offset) : NULL; }

int runtime_party_experiment_start(unsigned char *base, size_t size) {
    const char *on = getenv("BB_HOST_EXPERIMENT");
    const char *off = getenv("BB_HOST_TAKEOVER");
    keys = on && !strcmp(on, "1");
    automatic = !(off && !strcmp(off, "0"));
    if (size < 0x2acf708)
        return 0;
    image = base;
    party_manager = (void **)(base + 0x2acf700);
    return 1;
}

void runtime_party_experiment_request(int kind) {
    if (!image || !keys) return;
    fprintf(stderr, "Host experiment: key %d pressed\n", kind);
    __atomic_store_n(&request, kind, __ATOMIC_RELEASE);
}

/* Every guest sees the same roster. The alphabetically first remaining name becomes host; the
 * others join it after its new party has had time to publish. This works for the full eight slots. */
static int elect_successor(char out[24]) {
    unsigned char *party = party_manager ? *party_manager : NULL;
    void *roster = at(party, 0x30);
    int count = roster ? *(int *)((char *)roster + 12) : 0;
    void **items = roster ? at(roster, 0x20) : NULL;
    if (!items || count < 2 || count > 8) return -1;
    char host[24];
    runtime_room_mode_host_name(host);
    const char *best = NULL;
    int local = 0;
    for (int i = 0; i < count; ++i) {
        unsigned char *item = items[i];
        const char *name = item ? at(item, 0x10) : NULL;
        /* Case-insensitive, as the RyuNET server's own pick when a host's game dies
         * (party_session.go): every machine and the server name the same host. */
        if (!name || !*name || !strcasecmp(name, host)) continue;
        if (!best || strcasecmp(name, best) < 0) {
            best = name;
            local = item[0x2c] != 0;
        }
    }
    if (!best) return -1;
    snprintf(out, 24, "%.23s", best);
    return local;
}

/* The work waits for the lobby frame: teardown still runs inside the party update here. */
void runtime_party_host_lost(int role, int private_match, int announced_return) {
    /* Matchmaking/Ranked never uses native takeover: it teardown+recreates the party, wipes the
     * mode flags and sends the survivor back to Casual Normal. Queue/found-room host loss is
     * handled by the shared party session + queue abort back to ModeSelect. */
    if (!private_match) return;
    /* Quit With Party owns this transition. Migrating here fights the host's return message. */
    if (announced_return) {
        __atomic_store_n(&takeover_pending, 0, __ATOMIC_RELEASE);
        return;
    }
    if (!image || !automatic || role != 2) return;
    if (!runtime_room_mode_following() || runtime_room_mode_race_frozen()) return;
    takeover_local = elect_successor(takeover_successor);
    if (takeover_local < 0) return;
    takeover_after = now_ms() + (takeover_local ? 250 : 1250);
    takeover_until = takeover_after + 5000;
    __atomic_store_n(&takeover_pending, 1, __ATOMIC_RELEASE);
}

static int party_role(void) {
    unsigned char *party = party_manager ? *party_manager : NULL;
    unsigned char *connection = at(at(party, 0x18), 0x10);
    int role = 0;
    if (connection) memcpy(&role, connection + 0x118, sizeof role);
    return role;
}

int runtime_party_hosting(void) { return image && party_role() == 1; }

struct Look {
    unsigned char *party;
    void *pending, *session, *connection;
    void *conn_host_peer; /* connection+0x110: host peer retained by native message 0x22 */
    int state, role, peers, roster;
    char names[160];
};

static void look(struct Look *l) {
    memset(l, 0, sizeof *l);
    unsigned char *party = party_manager ? *party_manager : NULL;
    l->party = party;
    if (!party) return;
    l->pending = at(party, 0x10);
    l->session = at(party, 0x18);
    l->connection = at(l->session, 0x10);
    memcpy(&l->state, party + 0x54, sizeof l->state);
    if (l->connection) {
        memcpy(&l->role, (char *)l->connection + 0x118, sizeof l->role);
        memcpy(&l->peers, (char *)l->connection + 0x30, sizeof l->peers);
        l->conn_host_peer = at(l->connection, 0x110);
    }
    void *roster = at(party, 0x30);
    int count = roster ? *(int *)((char *)roster + 12) : 0;
    void **items = roster ? at(roster, 0x20) : NULL;
    if (count < 0 || count > 8 || !items) count = 0;
    l->roster = count;
    size_t used = 0;
    for (int i = 0; i < count && used + 24 < sizeof l->names; ++i) {
        const char *name = items[i] ? at(items[i], 0x10) : NULL;
        const int local = items[i] ? *((unsigned char *)items[i] + 0x2c) : 0;
        used += (size_t)snprintf(l->names + used, sizeof l->names - used, "%s%.16s%s", i ? "," : "",
                                 name ? name : "?", local ? "(local)" : "");
    }
}

static void print_look(const char *when, const struct Look *l) {
    fprintf(stderr, "Party state: %s state=%d role=%d peers=%d roster=%d [%s] pending=%p session=%p connection=%p\n",
            when, l->state, l->role, l->peers, l->roster, l->names, l->pending, l->session, l->connection);
    if (l->connection)
        fprintf(stderr, "Party transport: host_peer=%p\n", l->conn_host_peer);
}

/* The other player of a two-instance test: BB_HOST_EXPERIMENT_JOIN, or Ryu7w7 <-> Ryu7w7_test. */
static const char *other_player(void) {
    const char *set = getenv("BB_HOST_EXPERIMENT_JOIN");
    if (set && *set) return set;
    const char *me = runtime_np_get_online_name();
    return me && !strcmp(me, "Ryu7w7") ? "Ryu7w7_test" : "Ryu7w7";
}

/* The game's thread, every frame of an online lobby (runtime_online_mode.c lobby_update). */
void runtime_party_experiment_tick(void) {
    if (!image) return;
    static struct Look last;
    const int asked = __atomic_exchange_n(&request, 0, __ATOMIC_ACQ_REL);
    const uint64_t now = now_ms();
    if (asked) {
        look(&last);
        print_look(asked == 3 ? "before joining" : "before becoming host", &last);
        unsigned char *party = party_manager ? *party_manager : NULL;
        if (!party) {
            fprintf(stderr, "Host experiment: no party manager\n");
        } else if (asked == 3) {
            const char *host = other_player();
            fprintf(stderr, "Host experiment: joining %s's room (runtime_invite_join)\n", host);
            runtime_invite_join(host);
        } else
            fprintf(stderr, "Host experiment: direct party mutation disabled; native teardown owns migration\n");
        struct Look after;
        look(&after);
        print_look("right after", &after);
        last = after;
        watch_until = now + 30000;
        next_look = now + 250;
        return;
    }
    /* Native teardown already starts a new local party. The elected successor waits for that
     * operation; starting another create while +0x10 is pending corrupts its allocator state. */
    if (__atomic_load_n(&takeover_pending, __ATOMIC_ACQUIRE)) {
        unsigned char *party = party_manager ? *party_manager : NULL;
        void *session = at(party, 0x18);
        if (takeover_local && party && session && party_role() == 1) {
            __atomic_store_n(&takeover_pending, 0, __ATOMIC_RELEASE);
            runtime_room_mode_takeover();
            runtime_public_itemless_takeover_host();
            fprintf(stderr, "Host takeover: %s became host through native party recreation\n",
                            takeover_successor);
        } else if (!runtime_online_mode_lobby_open() || now >= takeover_until) {
            __atomic_store_n(&takeover_pending, 0, __ATOMIC_RELEASE);
        } else if (!takeover_local && now >= takeover_after && party && session && party_role() == 1 && automatic &&
                   !runtime_room_mode_race_frozen()) {
            __atomic_store_n(&takeover_pending, 0, __ATOMIC_RELEASE);
            fprintf(stderr, "Host takeover: joining elected member %s\n", takeover_successor);
            runtime_invite_join(takeover_successor);
        }
    }
    /* Always report every change of the party, roster included: a guest leaving must show here
     * (2026-11-18: the host's roster kept a guest who had left). */
    if (now < next_look) return;
    next_look = now + 250;
    struct Look l;
    look(&l);
    /* A guest leaving shrinks the host's roster. During matchmaking (including a found match)
     * tell the remaining host instead of letting the search/queue continue silently. */
    if (l.role == 1 && last.role == 1 && l.roster < last.roster &&
        runtime_public_itemless_match_active())
        runtime_public_itemless_party_left_notice();
    /* The leader quitting a queue leaves the guest in the found-room UI. Do NOT send it back to
     * the menus: the survivor keeps queueing in the same mode. The server dissolves the room and
     * pushes every player (see ctr_foundroom.go), so the game re-issues start_search on its own.
     * Only the host-side short-roster notice below applies; a guest that was left alone is not
     * told to leave. */
    if (l.pending != last.pending || l.session != last.session || l.connection != last.connection ||
        l.state != last.state || l.role != last.role || l.peers != last.peers || l.roster != last.roster ||
        strcmp(l.names, last.names))
        print_look("changed", &l);
    last = l;
    if (watch_until && now >= watch_until) {
        print_look("30 s later", &l);
        watch_until = 0;
    }
}
