#include "runtime_net_tunnel.h"

#include <stdatomic.h>
#include <string.h>

/* Per peer address the game sends to:
 *   last_direct    when a packet last arrived from it directly
 *   peer_ok        its latest tunnelled packet said our direct packets reach it
 * A copy goes through the tunnel unless both directions work (our direct packets arrive and
 * its direct packets arrive), so a one-way path still gets the other way tunnelled. Each side
 * keeps sending directly too, which is how the other side notices the direct path. */
typedef struct {
    uint32_t ip;
    uint16_t port;
    int used;
    int peer_ok;
    int answered;          /* it sent something through the tunnel: it has the tunnel too */
    uint64_t first_send;
    uint64_t last_use;
    uint64_t last_direct;
    uint64_t last_tunnel_send;
    uint64_t via_relay;    /* when it last answered through a relay (it has relays too) */
    uint64_t via_server;   /* when it last answered through the server */
} TunPeer;

#define TUN_MAX_PEERS 64

typedef struct {
    uint32_t ip;
    uint16_t port;
    char name[16];
    uint32_t ping;         /* smoothed round trip in ms; 0 until it answers */
    uint64_t last_ack;
    uint64_t last_register;
} TunRelay;

static TunPeer peers[TUN_MAX_PEERS];
static uint64_t keepalive_at;
static TunRelay relays[TUN_MAX_RELAYS];
static int relay_count;
static uint8_t pass[TUN_PASS_LEN];
static int have_pass;
static uint64_t request_at;       /* the pending pass request, for the server's ping */
static int request_pending;
static uint32_t server_ping;
static char forced_relay[16];
static atomic_flag lock = ATOMIC_FLAG_INIT;

static void take(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void give(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

void tun_reset(void) {
    take();
    memset(peers, 0, sizeof(peers));
    keepalive_at = 0;
    memset(relays, 0, sizeof(relays));
    relay_count = 0;
    have_pass = 0;
    request_at = 0;
    request_pending = 0;
    server_ping = 0;
    memset(forced_relay, 0, sizeof(forced_relay));
    give();
}

static uint32_t smooth(uint32_t old, uint64_t sample) {
    if (sample > 10000) sample = 10000;
    if (sample == 0) sample = 1;
    return old ? (uint32_t)((old * 3 + sample) / 4) : (uint32_t)sample;
}

int tun_candidate(uint32_t ip, uint32_t server_ip) {
    const uint8_t *b = (const uint8_t *)&ip;
    if (ip == server_ip || ip == 0 || ip == 0xFFFFFFFFu) return 0;
    if (b[0] == 10 || b[0] == 127 || b[0] == 0 || b[0] >= 224) return 0;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) return 0;
    if (b[0] == 192 && b[1] == 168) return 0;
    if (b[0] == 169 && b[1] == 254) return 0;
    if (b[0] == 100 && (b[1] & 0xC0) == 64) return 0;
    if (b[0] == 0 && b[1] == 255) return 0; /* the game's empty address slot */
    return 1;
}

/* The peer's entry, made (reusing the idlest one) when missing. Holds the lock. */
static TunPeer *peer_for(uint32_t ip, uint16_t port, uint64_t now, int create) {
    TunPeer *free_slot = NULL, *idlest = NULL;
    for (int i = 0; i < TUN_MAX_PEERS; i++) {
        TunPeer *p = &peers[i];
        if (p->used && p->ip == ip && p->port == port) return p;
        if (!p->used || now - p->last_use > TUN_PEER_IDLE_MS) {
            if (!free_slot) free_slot = p;
        } else if (!idlest || p->last_use < idlest->last_use) {
            idlest = p;
        }
    }
    if (!create) return NULL;
    TunPeer *p = free_slot ? free_slot : idlest;
    memset(p, 0, sizeof(*p));
    p->ip = ip;
    p->port = port;
    p->used = 1;
    p->first_send = now;
    p->last_use = now;
    return p;
}

static int direct_fresh(const TunPeer *p, uint64_t now) {
    return p->last_direct && now - p->last_direct < TUN_DIRECT_FRESH_MS;
}

void tun_force_relay(const char *name) {
    take();
    memset(forced_relay, 0, sizeof(forced_relay));
    if (name) strncpy(forced_relay, name, sizeof(forced_relay) - 1);
    give();
}

/* The relay with the best ping that answers, when it beats the server's (or the forced one
 * while it answers). Holds the lock. */
static int best_relay(uint64_t now) {
    int best = -1;
    if (forced_relay[0]) {
        for (int i = 0; i < relay_count; i++) {
            const TunRelay *r = &relays[i];
            if (strcmp(r->name, forced_relay) == 0 && r->last_ack && now - r->last_ack < TUN_RELAY_FRESH_MS) return i;
        }
        return -1;
    }
    for (int i = 0; i < relay_count; i++) {
        const TunRelay *r = &relays[i];
        if (!r->ping || !r->last_ack || now - r->last_ack >= TUN_RELAY_FRESH_MS) continue;
        if (best < 0 || r->ping < relays[best].ping) best = i;
    }
    if (best >= 0 && server_ping && relays[best].ping + TUN_RELAY_MARGIN_MS > server_ping) best = -1;
    return best;
}

int tun_best_relay(uint64_t now_ms) {
    take();
    const int best = best_relay(now_ms);
    give();
    return best;
}

int tun_on_send(uint32_t ip, uint16_t port, uint64_t now_ms, uint8_t *flags, int *relay) {
    take();
    TunPeer *p = peer_for(ip, port, now_ms, 1);
    p->last_use = now_ms;
    const int fresh = direct_fresh(p, now_ms);
    *flags = fresh ? 1 : 0;
    int tunnel = !(fresh && p->peer_ok);
    if (tunnel && !p->answered && now_ms - p->first_send > TUN_GIVE_UP_MS) tunnel = 0;
    int route = 0;
    if (tunnel) {
        p->last_tunnel_send = now_ms;
        const int best = best_relay(now_ms);
        /* A peer that answers only through the server after the trial has no relays. */
        const int relay_ok = best >= 0 &&
            !(p->via_server && !p->via_relay && now_ms - p->first_send > TUN_RELAY_TRIAL_MS);
        if (relay_ok) {
            route |= TUN_VIA_RELAY;
            *relay = best;
        }
        /* Through the server too until the peer has shown it gets the relay's copies. */
        if (!relay_ok || !p->via_relay) route |= TUN_VIA_SERVER;
    }
    give();
    return route;
}

void tun_on_direct(uint32_t ip, uint16_t port, uint64_t now_ms) {
    take();
    TunPeer *p = peer_for(ip, port, now_ms, 0);
    if (p) {
        p->last_direct = now_ms;
        p->last_use = now_ms;
    }
    give();
}

void tun_on_tunnelled(uint32_t ip, uint16_t port, uint8_t flags, int via_relay, uint64_t now_ms) {
    take();
    TunPeer *p = peer_for(ip, port, now_ms, 1);
    p->peer_ok = flags & 1;
    p->answered = 1;
    p->last_use = now_ms;
    if (via_relay) p->via_relay = now_ms;
    else p->via_server = now_ms;
    give();
}

/* --- relays ------------------------------------------------------------------------------- */

size_t tun_pass_request(uint8_t *out, size_t cap, uint64_t now_ms) {
    if (cap < TUN_REQUEST_LEN) return 0;
    memset(out, 0, TUN_REQUEST_LEN);
    memcpy(out, TUN_KEEPALIVE, TUN_KEEPALIVE_LEN);
    out[TUN_KEEPALIVE_LEN] = 2;
    take();
    request_at = now_ms;
    request_pending = 1;
    give();
    return TUN_REQUEST_LEN;
}

int tun_on_offer(const uint8_t *in, size_t len, uint64_t now_ms) {
    if (len < 6 + TUN_PASS_LEN + 1 || memcmp(in, "RYUPS1", 6) != 0) return 0;
    TunRelay fresh[TUN_MAX_RELAYS];
    int count = 0;
    size_t at = 6 + TUN_PASS_LEN + 1;
    const int listed = in[6 + TUN_PASS_LEN];
    for (int i = 0; i < listed && count < TUN_MAX_RELAYS; i++) {
        if (at + 7 > len || at + 7 + in[at + 6] > len) break;
        TunRelay *r = &fresh[count++];
        memset(r, 0, sizeof(*r));
        memcpy(&r->ip, in + at, 4);
        r->port = (uint16_t)(in[at + 4] << 8 | in[at + 5]);
        size_t n = in[at + 6];
        memcpy(r->name, in + at + 7, n < sizeof(r->name) - 1 ? n : sizeof(r->name) - 1);
        at += 7 + n;
    }
    take();
    memcpy(pass, in + 6, TUN_PASS_LEN);
    have_pass = 1;
    if (request_pending) {
        server_ping = smooth(server_ping, now_ms - request_at);
        request_pending = 0;
    }
    /* Relays that stay keep what they measured. */
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < relay_count; j++) {
            if (relays[j].ip == fresh[i].ip && relays[j].port == fresh[i].port) {
                fresh[i].ping = relays[j].ping;
                fresh[i].last_ack = relays[j].last_ack;
                fresh[i].last_register = relays[j].last_register;
            }
        }
    }
    memcpy(relays, fresh, sizeof(TunRelay) * (size_t)count);
    relay_count = count;
    give();
    return 1;
}

int tun_relay_count(void) {
    take();
    const int n = relay_count;
    give();
    return n;
}

int tun_relay_index(uint32_t ip, uint16_t port) {
    int found = -1;
    take();
    for (int i = 0; i < relay_count && found < 0; i++) {
        if (relays[i].ip == ip && relays[i].port == port) found = i;
    }
    give();
    return found;
}

int tun_relay_addr(int i, uint32_t *ip, uint16_t *port) {
    int ok = 0;
    take();
    if (i >= 0 && i < relay_count) {
        *ip = relays[i].ip;
        *port = relays[i].port;
        ok = 1;
    }
    give();
    return ok;
}

size_t tun_relay_register(int i, uint64_t now_ms, uint8_t *out, size_t cap, uint32_t *ip, uint16_t *port) {
    size_t n = 0;
    take();
    if (have_pass && i >= 0 && i < relay_count && cap >= 6 + TUN_PASS_LEN + 4) {
        TunRelay *r = &relays[i];
        if (!r->last_register || now_ms - r->last_register >= TUN_REGISTER_MS) {
            r->last_register = now_ms;
            memcpy(out, "RYURG1", 6);
            memcpy(out + 6, pass, TUN_PASS_LEN);
            const uint32_t stamp = (uint32_t)now_ms;
            out[6 + TUN_PASS_LEN] = (uint8_t)(stamp >> 24);
            out[6 + TUN_PASS_LEN + 1] = (uint8_t)(stamp >> 16);
            out[6 + TUN_PASS_LEN + 2] = (uint8_t)(stamp >> 8);
            out[6 + TUN_PASS_LEN + 3] = (uint8_t)stamp;
            *ip = r->ip;
            *port = r->port;
            n = 6 + TUN_PASS_LEN + 4;
        }
    }
    give();
    return n;
}

int tun_on_register_reply(int i, const uint8_t *in, size_t len, uint64_t now_ms) {
    if (len != 11 || memcmp(in, "RYURG1", 6) != 0) return 0;
    const uint32_t stamp = (uint32_t)in[7] << 24 | (uint32_t)in[8] << 16 | (uint32_t)in[9] << 8 | in[10];
    take();
    if (i >= 0 && i < relay_count && in[6] == 0) {
        relays[i].ping = smooth(relays[i].ping, (uint32_t)now_ms - stamp);
        relays[i].last_ack = now_ms;
    }
    give();
    return 1;
}

const char *tun_relay_name(int i) {
    return i >= 0 && i < TUN_MAX_RELAYS ? relays[i].name : "";
}

uint32_t tun_relay_ping(int i) {
    take();
    const uint32_t ping = i >= 0 && i < relay_count ? relays[i].ping : 0;
    give();
    return ping;
}

int tun_keepalive_due(uint64_t now_ms) {
    take();
    const int due = keepalive_at == 0 || now_ms - keepalive_at >= TUN_KEEPALIVE_MS;
    if (due) keepalive_at = now_ms;
    give();
    return due;
}

int tun_active_peers(uint64_t now_ms) {
    int n = 0;
    take();
    for (int i = 0; i < TUN_MAX_PEERS; i++) {
        if (peers[i].used && peers[i].last_tunnel_send && now_ms - peers[i].last_tunnel_send < TUN_DIRECT_FRESH_MS) n++;
    }
    give();
    return n;
}

size_t tun_wrap(uint8_t *out, size_t cap, uint8_t flags, uint32_t ip, uint16_t port,
                const void *payload, size_t len) {
    if (cap < TUN_HEADER_LEN + len) return 0;
    memcpy(out, "RYUTN1", 6);
    out[6] = flags;
    memcpy(out + 7, &ip, 4);
    out[11] = (uint8_t)(port >> 8);
    out[12] = (uint8_t)port;
    memcpy(out + TUN_HEADER_LEN, payload, len);
    return TUN_HEADER_LEN + len;
}

int tun_unwrap(const uint8_t *in, size_t len, uint8_t *flags, uint32_t *ip, uint16_t *port) {
    if (len < TUN_HEADER_LEN || memcmp(in, "RYUTN1", 6) != 0) return 0;
    *flags = in[6];
    memcpy(ip, in + 7, 4);
    *port = (uint16_t)(in[11] << 8 | in[12]);
    return 1;
}
