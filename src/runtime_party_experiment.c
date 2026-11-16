/* Host handoff research, experiment 1 (staff builds): can a guest of a private room become the
 * host of a new party of its own while the room's lobby stays on screen?
 *
 * The game has no way to hand a party over: a host is whoever created the party. 0xa56fb0
 * (CPartyManager, [0x2acf700]) starts a new party session when this player is not its host; when
 * the server confirms it, 0xa57350 makes it the party's session (+0x18), rebuilds the roster
 * (+0x30) from the local players, posts CGuiEventLobbyBecomeHost to the menus and registers the
 * host's messages (0x28, 0x29 leaving, 0x2a, 0x2f). 0xa56f70 calls it only while the network is
 * up and nothing else is in progress.
 *
 * In a lobby, Ctrl+F9 calls 0xa56f70 and Ctrl+Shift+F9 calls 0xa56fb0 directly; then for 30 s
 * every change of the party (state, role, roster, sessions, peers) is written to the log, with the
 * roster's names, to see what the game and its lobby do.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned char *image;
static void **party_manager;
static void (ABI *party_create_checked)(void *);           /* 0xa56f70 */
static uint64_t (ABI *party_create)(void *);               /* 0xa56fb0 */
static int request;                                         /* 1 checked, 2 direct */
static uint64_t watch_until, next_look;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *at(const void *p, size_t offset) { return p ? *(void *const *)((const char *)p + offset) : NULL; }

int runtime_party_experiment_start(unsigned char *base, size_t size) {
    static const unsigned char create[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
    static const unsigned char checked[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8b, 0x05};
    if (size < 0x2acf708 || memcmp(base + 0xa56fb0, create, sizeof create) ||
        memcmp(base + 0xa56f70, checked, sizeof checked))
        return 0;
    image = base;
    party_manager = (void **)(base + 0x2acf700);
    party_create = (uint64_t (ABI *)(void *))(base + 0xa56fb0);
    party_create_checked = (void (ABI *)(void *))(base + 0xa56f70);
    return 1;
}

void runtime_party_experiment_request(int direct) {
    __atomic_store_n(&request, direct ? 2 : 1, __ATOMIC_RELEASE);
}

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

/* The game's thread, every frame of an online lobby (runtime_online_mode.c lobby_update). */
void runtime_party_experiment_tick(void) {
    if (!image) return;
    static struct Look last;
    const int asked = __atomic_exchange_n(&request, 0, __ATOMIC_ACQ_REL);
    const uint64_t now = now_ms();
    if (asked) {
        look(&last);
        print_look(asked == 2 ? "before 0xa56fb0 (direct)" : "before 0xa56f70 (checked)", &last);
        unsigned char *party = party_manager ? *party_manager : NULL;
        if (!party) {
            fprintf(stderr, "Host experiment: no party manager\n");
        } else if (asked == 2) {
            const uint64_t result = party_create(party);
            fprintf(stderr, "Host experiment: 0xa56fb0 returned %" PRIu64 " (1: a party session was already being made)\n", result);
        } else {
            party_create_checked(party);
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
