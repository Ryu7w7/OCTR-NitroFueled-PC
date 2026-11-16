/* Host handoff research (staff builds): can the host of a private room change while the room's
 * lobby stays on screen?
 *
 * The game has no way to hand a party over: a host is whoever created the party. 0xa56fb0
 * (CPartyManager, [0x2acf700]) starts a new party session when this player is not its host; when
 * the server confirms it, 0xa57350 makes it the party's session (+0x18), adds the local players to
 * the roster (+0x30, without emptying it), posts CGuiEventLobbyBecomeHost to the menus and
 * registers the host's messages (0x28, 0x29 leaving, 0x2a, 0x2f).
 *
 * Experiment 1 (2026-11-16): a guest calling it in the lobby became host with the lobby kept (START
 * GAME showed), but the old party was dropped while still connected (a corrupt free followed), the
 * roster kept the old members plus a second copy of the local player, and the old host was left
 * alone. Experiment 2, in a lobby:
 *   Ctrl+F9        leave the old party the game's way first (0xa9ffd0: tells its host, message
 *                  0x29, and resets the party), empty the roster, then make the new party;
 *   Ctrl+Shift+F9  only close the old party's connection (0x409700) before the same;
 *   Ctrl+F10       join the other player's room (the old host following the new one): the
 *                  Friends tab's Join (runtime_invite_join).
 * For 30 s after each, every change of the party is written to the log with the roster's names.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include "runtime_np_online.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void runtime_invite_join(const char *host); /* runtime_services.c */

static unsigned char *image;
static void **party_manager;
static uint64_t (ABI *party_create)(void *);            /* 0xa56fb0 */
static void (ABI *party_leave)(void *);                 /* 0xa9ffd0 (traced in runtime_public_itemless.c) */
static void (ABI *connection_close)(void *, int);       /* 0x409700 */
static int request;
static uint64_t watch_until, next_look;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *at(const void *p, size_t offset) { return p ? *(void *const *)((const char *)p + offset) : NULL; }

int runtime_party_experiment_start(unsigned char *base, size_t size) {
    static const unsigned char create[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
    if (size < 0x2acf708 || memcmp(base + 0xa56fb0, create, sizeof create) || base[0x409700] != 0x55)
        return 0;
    image = base;
    party_manager = (void **)(base + 0x2acf700);
    party_create = (uint64_t (ABI *)(void *))(base + 0xa56fb0);
    party_leave = (void (ABI *)(void *))(base + 0xa9ffd0);
    connection_close = (void (ABI *)(void *, int))(base + 0x409700);
    return 1;
}

void runtime_party_experiment_request(int kind) {
    __atomic_store_n(&request, kind, __ATOMIC_RELEASE);
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
    void *pending, *session, *connection;
    int state, role, peers, roster;
    char names[160];
};

static void look(struct Look *l) {
    memset(l, 0, sizeof *l);
    unsigned char *party = party_manager ? *party_manager : NULL;
    if (!party) return;
    l->pending = at(party, 0x10);
    l->session = at(party, 0x18);
    l->connection = at(l->session, 0x10);
    memcpy(&l->state, party + 0x54, sizeof l->state);
    if (l->connection) {
        memcpy(&l->role, (char *)l->connection + 0x118, sizeof l->role);
        memcpy(&l->peers, (char *)l->connection + 0x30, sizeof l->peers);
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
    fprintf(stderr, "Host experiment: %s state=%d role=%d peers=%d roster=%d [%s] pending=%p session=%p connection=%p\n",
            when, l->state, l->role, l->peers, l->roster, l->names, l->pending, l->session, l->connection);
}

/* The other player of a two-instance test: BB_HOST_EXPERIMENT_JOIN, or Ryu7w7 <-> Ryu7w7_test. */
static const char *other_player(void) {
    const char *set = getenv("BB_HOST_EXPERIMENT_JOIN");
    if (set && *set) return set;
    const char *me = runtime_np_get_online_name();
    return me && !strcmp(me, "Ryu7w7") ? "Ryu7w7_test" : "Ryu7w7";
}

static void become_host(unsigned char *party, int kind) {
    if (party_role() == 1) {
        fprintf(stderr, "Host experiment: already a party host; nothing to do\n");
        return;
    }
    if (kind == 1) {
        fprintf(stderr, "Host experiment: leaving the old party (0xa9ffd0)\n");
        party_leave(party);
    } else {
        void *connection = at(at(party, 0x18), 0x10);
        fprintf(stderr, "Host experiment: closing the old party's connection %p (0x409700)\n", connection);
        if (connection) connection_close(connection, 1);
    }
    void *roster = at(party, 0x30);
    if (roster) *(int *)((char *)roster + 12) = 0; /* 0xa57350 appends; its entries are left alive */
    const uint64_t result = party_create(party);
    fprintf(stderr, "Host experiment: 0xa56fb0 returned %" PRIu64 " (1: a party session was already being made)\n", result);
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
        } else {
            become_host(party, asked);
        }
        struct Look after;
        look(&after);
        print_look("right after", &after);
        last = after;
        watch_until = now + 30000;
        next_look = now + 250;
        return;
    }
    if (!watch_until || now < next_look) return;
    next_look = now + 250;
    struct Look l;
    look(&l);
    if (l.pending != last.pending || l.session != last.session || l.connection != last.connection ||
        l.state != last.state || l.role != last.role || l.peers != last.peers || l.roster != last.roster ||
        strcmp(l.names, last.names))
        print_look("changed", &l);
    last = l;
    if (now >= watch_until) {
        print_look("30 s later", &l);
        watch_until = 0;
    }
}
