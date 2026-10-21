/* The RyuNET tunnel's bookkeeping (src/runtime_net_tunnel.c): when the game's packets to a
 * peer also go through the server, and the wrapped packet format nat_tunnel.go reads. */
#include "runtime_net_tunnel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int relay_slot; /* the relay a copy goes through */
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t ip4(int a, int b, int c, int d) {
    uint8_t bytes[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
    uint32_t ip;
    memcpy(&ip, bytes, 4);
    return ip;
}

static void candidates(void) {
    const uint32_t server = ip4(165, 1, 125, 122);
    CHECK(tun_candidate(ip4(179, 60, 85, 233), server));
    CHECK(tun_candidate(ip4(26, 241, 245, 203), server)); /* Radmin's range is a public one */
    CHECK(!tun_candidate(server, server));
    CHECK(!tun_candidate(ip4(192, 168, 18, 53), server));
    CHECK(!tun_candidate(ip4(10, 0, 0, 23), server));
    CHECK(!tun_candidate(ip4(172, 20, 1, 1), server));
    CHECK(tun_candidate(ip4(172, 32, 1, 1), server));
    CHECK(!tun_candidate(ip4(127, 0, 0, 1), server));
    CHECK(!tun_candidate(ip4(100, 64, 3, 4), server));
    CHECK(!tun_candidate(ip4(0, 255, 0, 255), server));
    CHECK(!tun_candidate(ip4(239, 1, 1, 1), server));
}

static void wrap_format(void) {
    uint8_t out[64], flags;
    const uint8_t payload[] = {0x0C, 0x02, 0x00, 0xAB};
    const uint32_t peer = ip4(201, 189, 26, 165);
    size_t n = tun_wrap(out, sizeof(out), 1, peer, 3074, payload, sizeof(payload));
    CHECK(n == TUN_HEADER_LEN + sizeof(payload));
    CHECK(memcmp(out, "RYUTN1", 6) == 0 && out[6] == 1);
    CHECK(out[7] == 201 && out[8] == 189 && out[9] == 26 && out[10] == 165);
    CHECK(out[11] == 0x0C && out[12] == 0x02); /* 3074, big-endian */
    CHECK(memcmp(out + TUN_HEADER_LEN, payload, sizeof(payload)) == 0);
    uint32_t ip;
    uint16_t port;
    CHECK(tun_unwrap(out, n, &flags, &ip, &port) && flags == 1 && ip == peer && port == 3074);
    CHECK(tun_wrap(out, 10, 0, peer, 1, payload, sizeof(payload)) == 0);
    const uint8_t nat_reply[] = {0x15, 0x02, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    CHECK(!tun_unwrap(nat_reply, sizeof(nat_reply), &flags, &ip, &port));
}

/* Two ports, A and B, as the two ends. */
static void both_directions_work(void) {
    tun_reset();
    const uint32_t b = ip4(187, 190, 184, 34);
    uint8_t flags;
    uint64_t t = 1000;
    /* Nothing proven yet: copies go through the tunnel, saying "your direct packets do not reach me". */
    CHECK(tun_on_send(b, 18270, t, &flags, &relay_slot) == 1 && flags == 0);
    /* B's direct packets arrive: A now tells B so, but keeps tunnelling until B says the same. */
    tun_on_direct(b, 18270, t + 10);
    CHECK(tun_on_send(b, 18270, t + 20, &flags, &relay_slot) == 1 && flags == 1);
    tun_on_tunnelled(b, 18270, 1, 0, t + 30);
    CHECK(tun_on_send(b, 18270, t + 40, &flags, &relay_slot) == 0 && flags == 1);
    /* Direct traffic from B stops: the tunnel takes over again. */
    CHECK(tun_on_send(b, 18270, t + 40 + TUN_DIRECT_FRESH_MS, &flags, &relay_slot) == 1 && flags == 0);
}

static void one_way_path_keeps_the_tunnel(void) {
    tun_reset();
    const uint32_t b = ip4(187, 190, 184, 34);
    uint8_t flags;
    /* A gets B's direct packets, but B does not get A's (it says so through the tunnel). */
    tun_on_send(b, 18270, 1000, &flags, &relay_slot);
    for (uint64_t t = 1100; t < 30000; t += 500) {
        tun_on_direct(b, 18270, t);
        tun_on_tunnelled(b, 18270, 0, 0, t);
        CHECK(tun_on_send(b, 18270, t + 1, &flags, &relay_slot) == 1 && flags == 1);
    }
}

static void peer_without_the_tunnel(void) {
    tun_reset();
    const uint32_t old = ip4(201, 1, 2, 3);
    uint8_t flags;
    CHECK(tun_on_send(old, 3074, 0 + 1, &flags, &relay_slot) == 1);
    CHECK(tun_on_send(old, 3074, TUN_GIVE_UP_MS, &flags, &relay_slot) == 1);
    /* It never answered through the tunnel: copies stop. */
    CHECK(tun_on_send(old, 3074, TUN_GIVE_UP_MS + 2, &flags, &relay_slot) == 0);
}

static void keepalive(void) {
    tun_reset();
    CHECK(tun_keepalive_due(5));
    CHECK(!tun_keepalive_due(5 + TUN_KEEPALIVE_MS - 1));
    CHECK(tun_keepalive_due(5 + TUN_KEEPALIVE_MS));
}

static void many_peers(void) {
    tun_reset();
    uint8_t flags;
    for (int i = 0; i < 200; i++) CHECK(tun_on_send(ip4(80, 1, (uint8_t)(i >> 8), (uint8_t)i), 3074, 1000 + (uint64_t)i, &flags, &relay_slot) == 1);
    CHECK(tun_active_peers(1300) == 64);
}

/* The server's offer: a pass and two relays (relay_pass.go's layout). */
static size_t offer(uint8_t *out) {
    size_t n = 0;
    memcpy(out, "RYUPS1", 6);
    n = 6;
    for (int i = 0; i < TUN_PASS_LEN; i++) out[n++] = (uint8_t)(0xA0 + i);
    out[n++] = 2;
    const uint8_t asia[] = {203, 0, 113, 5, 0x0C, 0x12, 4, 'a', 's', 'i', 'a'};
    const uint8_t eu[] = {198, 51, 100, 7, 0x0C, 0x13, 2, 'e', 'u'};
    memcpy(out + n, asia, sizeof(asia));
    n += sizeof(asia);
    memcpy(out + n, eu, sizeof(eu));
    return n + sizeof(eu);
}

static void relay_reply(uint8_t *out, uint8_t status, uint32_t stamp) {
    memcpy(out, "RYURG1", 6);
    out[6] = status;
    out[7] = (uint8_t)(stamp >> 24);
    out[8] = (uint8_t)(stamp >> 16);
    out[9] = (uint8_t)(stamp >> 8);
    out[10] = (uint8_t)stamp;
}

static void relay_offer_and_registration(void) {
    tun_reset();
    uint8_t buf[512];
    /* The keepalive asking for a pass is padded: the server answers nothing longer. */
    CHECK(tun_pass_request(buf, sizeof(buf), 1000) == TUN_REQUEST_LEN);
    CHECK(memcmp(buf, "RYUKA1", 6) == 0 && buf[6] == 2);
    /* Without a pass there is nothing to register. */
    uint32_t ip;
    uint16_t port;
    CHECK(tun_relay_register(0, 1000, buf, sizeof(buf), &ip, &port) == 0);

    size_t n = offer(buf);
    CHECK(tun_on_offer(buf, n, 1180)); /* answered 180 ms after the request: the server's ping */
    CHECK(tun_relay_count() == 2);
    CHECK(strcmp(tun_relay_name(0), "asia") == 0 && strcmp(tun_relay_name(1), "eu") == 0);
    CHECK(tun_relay_index(ip4(203, 0, 113, 5), 3090) == 0);
    CHECK(tun_relay_index(ip4(198, 51, 100, 7), 3091) == 1);
    CHECK(tun_relay_index(ip4(203, 0, 113, 5), 3091) == -1);
    const uint8_t nat_reply[] = {0x15, 0x02, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    CHECK(!tun_on_offer(nat_reply, sizeof(nat_reply), 1200));

    /* Registration: the pass and a stamp, once per interval. */
    n = tun_relay_register(0, 2000, buf, sizeof(buf), &ip, &port);
    CHECK(n == 6 + TUN_PASS_LEN + 4 && memcmp(buf, "RYURG1", 6) == 0 && buf[6] == 0xA0);
    CHECK(ip == ip4(203, 0, 113, 5) && port == 3090);
    CHECK(buf[n - 1] == (uint8_t)2000 && buf[n - 2] == (uint8_t)(2000 >> 8));
    CHECK(tun_relay_register(0, 2000 + TUN_REGISTER_MS - 1, buf, sizeof(buf), &ip, &port) == 0);
    CHECK(tun_relay_register(0, 2000 + TUN_REGISTER_MS, buf, sizeof(buf), &ip, &port) != 0);

    /* No relay answered yet: copies go through the server. */
    CHECK(tun_best_relay(2100) == -1);
    relay_reply(buf, 0, 2000);
    CHECK(tun_on_register_reply(0, buf, 11, 2030)); /* 30 ms to Asia */
    CHECK(tun_relay_ping(0) == 30);
    CHECK(tun_best_relay(2100) == 0);
    relay_reply(buf, 0, 2000);
    tun_on_register_reply(1, buf, 11, 2175); /* Europe: 175 ms, not better than the server by 10 */
    CHECK(tun_best_relay(2200) == 0);
    /* A refused registration does not count. */
    relay_reply(buf, 1, 2000);
    tun_on_register_reply(1, buf, 11, 2010);
    CHECK(tun_relay_ping(1) == 175);
    /* Asia stops answering: the server again (Europe does not beat it). */
    CHECK(tun_best_relay(2030 + TUN_RELAY_FRESH_MS) == -1);
    /* A new offer keeps what was measured for the relays it lists again. */
    n = offer(buf);
    tun_on_offer(buf, n, 2300);
    CHECK(tun_relay_ping(0) == 30 && tun_best_relay(2300) == 0);
}

static void relay_routes(void) {
    tun_reset();
    uint8_t buf[512], flags;
    uint32_t ip;
    uint16_t port;
    tun_pass_request(buf, sizeof(buf), 0);
    tun_on_offer(buf, offer(buf), 200);
    tun_relay_register(0, 300, buf, sizeof(buf), &ip, &port);
    relay_reply(buf, 0, 300);
    tun_on_register_reply(0, buf, 11, 330);

    /* A peer that has relays too: copies through the relay and the server until it answers
     * through a relay, then the relay only. */
    const uint32_t b = ip4(36, 80, 1, 2);
    relay_slot = -1;
    CHECK(tun_on_send(b, 3074, 1000, &flags, &relay_slot) == (TUN_VIA_RELAY | TUN_VIA_SERVER) && relay_slot == 0);
    tun_on_tunnelled(b, 3074, 0, 1, 1100);
    CHECK(tun_on_send(b, 3074, 1200, &flags, &relay_slot) == TUN_VIA_RELAY);

    /* An older port answers through the server only: after the trial, the server only. */
    const uint32_t old = ip4(49, 1, 2, 3);
    CHECK(tun_on_send(old, 3074, 2000, &flags, &relay_slot) == (TUN_VIA_RELAY | TUN_VIA_SERVER));
    tun_on_tunnelled(old, 3074, 0, 0, 2100);
    CHECK(tun_on_send(old, 3074, 2000 + TUN_RELAY_TRIAL_MS - 1, &flags, &relay_slot) == (TUN_VIA_RELAY | TUN_VIA_SERVER));
    tun_on_tunnelled(old, 3074, 0, 0, 2000 + TUN_RELAY_TRIAL_MS);
    CHECK(tun_on_send(old, 3074, 2000 + TUN_RELAY_TRIAL_MS + 1, &flags, &relay_slot) == TUN_VIA_SERVER);
}

static void forced_relay(void) {
    tun_reset();
    uint8_t buf[512];
    uint32_t ip;
    uint16_t port;
    tun_pass_request(buf, sizeof(buf), 0);
    tun_on_offer(buf, offer(buf), 20); /* the server: 20 ms, better than both relays */
    for (int i = 0; i < 2; i++) {
        tun_relay_register(i, 100, buf, sizeof(buf), &ip, &port);
        relay_reply(buf, 0, 100);
        tun_on_register_reply(i, buf, 11, 180);
    }
    CHECK(tun_best_relay(200) == -1);
    tun_force_relay("eu");
    CHECK(tun_best_relay(200) == 1);
    tun_force_relay("nowhere");
    CHECK(tun_best_relay(200) == -1);
    tun_force_relay(NULL);
    CHECK(tun_best_relay(200) == -1);
}

int main(void) {
    candidates();
    relay_offer_and_registration();
    relay_routes();
    forced_relay();
    wrap_format();
    both_directions_work();
    one_way_path_keeps_the_tunnel();
    peer_without_the_tunnel();
    keepalive();
    many_peers();
    if (failures) {
        fprintf(stderr, "net tunnel: %d failure(s)\n", failures);
        return 1;
    }
    puts("net tunnel: ok");
    return 0;
}
