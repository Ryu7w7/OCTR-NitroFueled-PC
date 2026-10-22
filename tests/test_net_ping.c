/* RyuNET ping's bookkeeping (src/runtime_net_ping.c): who gets pings, the answers, and the list
 * of the players in the room the in-game menu shows. */
#include "runtime_net_ping.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static size_t make(uint8_t *out, int kind, uint32_t stamp, const char *name) {
    const size_t n = strlen(name);
    memcpy(out, PING_MAGIC, PING_MAGIC_LEN);
    out[6] = (uint8_t)kind;
    out[7] = (uint8_t)(stamp >> 24);
    out[8] = (uint8_t)(stamp >> 16);
    out[9] = (uint8_t)(stamp >> 8);
    out[10] = (uint8_t)stamp;
    out[11] = (uint8_t)n;
    memcpy(out + 12, name, n);
    return 12 + n;
}

static const PingPeer *find(const PingPeer *list, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].name, name) == 0) return &list[i];
    return NULL;
}

int main(void) {
    const uint32_t bob = 0x0A000001, old = 0x0A000002, carol = 0x0A000003;
    uint8_t packet[PING_PACKET_MAX], reply[PING_PACKET_MAX];
    uint32_t ip;
    uint16_t port;
    size_t reply_len;
    PingPeer list[PING_MAX_PEERS];
    ping_reset();
    ping_set_name("Alice<> !");

    /* Nobody gets a ping before it says it has them (an older port never sees one). */
    ping_on_game_send(bob, 3074, 1000);
    ping_on_game_send(old, 3074, 1000);
    CHECK(ping_next(1000, packet, sizeof(packet), &ip, &port) == 0);
    int n = ping_view(1000, list, PING_MAX_PEERS);
    CHECK(n == 2 && list[0].ms == PING_OLDER_PORT && list[1].ms == PING_OLDER_PORT);

    /* Bob's tunnelled packets carry the flag: a ping, then one a second. */
    ping_on_capable(bob, 3074, 1000);
    size_t len = ping_next(1000, packet, sizeof(packet), &ip, &port);
    CHECK(len == 12 + 5 && ip == bob && port == 3074 && packet[6] == 1);
    CHECK(memcmp(packet + 12, "Alice", 5) == 0); /* a nickname's characters only */
    CHECK(ping_next(1000, packet, sizeof(packet), &ip, &port) == 0);
    CHECK(ping_next(1500, packet, sizeof(packet), &ip, &port) == 0);

    /* Bob answers in 40 ms, direct and through the tunnel: counted once. */
    uint8_t answer[PING_PACKET_MAX];
    size_t answer_len = make(answer, 2, 1000, "Bob");
    CHECK(ping_on_packet(answer, answer_len, bob, 3074, 1040, reply, sizeof(reply), &reply_len) == 1 && reply_len == 0);
    CHECK(ping_on_packet(answer, answer_len, bob, 3074, 1090, reply, sizeof(reply), &reply_len) == 1);
    n = ping_view(1100, list, PING_MAX_PEERS);
    const PingPeer *b = find(list, n, "Bob");
    CHECK(n == 2 && b && b->ms == 40);
    CHECK(find(list, n, "") && find(list, n, "")->ms == PING_OLDER_PORT);

    /* The next answer moves the smoothed time a quarter of the way. */
    ping_on_game_send(bob, 3074, 2000);
    CHECK(ping_next(2000, packet, sizeof(packet), &ip, &port) > 0);
    answer_len = make(answer, 2, 2000, "Bob");
    ping_on_packet(answer, answer_len, bob, 3074, 2080, reply, sizeof(reply), &reply_len);
    n = ping_view(2100, list, PING_MAX_PEERS);
    CHECK(find(list, n, "Bob")->ms == 50);

    /* Carol pings us: the answer returns her stamp with our name. She is in the room only once
     * the game sends to her. */
    answer_len = make(answer, 1, 77777, "Carol");
    CHECK(ping_on_packet(answer, answer_len, carol, 3075, 2200, reply, sizeof(reply), &reply_len) == 1);
    CHECK(reply_len == 12 + 5 && reply[6] == 2 && reply[10] == (uint8_t)77777 && memcmp(reply + 12, "Alice", 5) == 0);
    CHECK(!find(list, ping_view(2200, list, PING_MAX_PEERS), "Carol"));
    ping_on_game_send(carol, 3075, 2300);
    n = ping_view(2300, list, PING_MAX_PEERS);
    CHECK(find(list, n, "Carol") && find(list, n, "Carol")->ms == PING_NO_ANSWER);

    /* A broken ping packet is still not the game's, and gets no answer. */
    memcpy(packet, PING_MAGIC, PING_MAGIC_LEN);
    packet[6] = 1;
    packet[11] = 40;
    CHECK(ping_on_packet(packet, 14, carol, 3075, 2400, reply, sizeof(reply), &reply_len) == 1 && reply_len == 0);
    CHECK(ping_on_packet((const uint8_t *)"\x0c\x02\x00\xab", 4, carol, 3075, 2400, reply, sizeof(reply), &reply_len) == 0);

    /* No answer for a while: no time shown. Players the game stopped sending to leave the list. */
    n = ping_view(2100 + PING_FRESH_MS + 100, list, PING_MAX_PEERS);
    CHECK(find(list, n, "Bob") && find(list, n, "Bob")->ms == PING_NO_ANSWER);
    CHECK(ping_view(2300 + PING_ACTIVE_MS + 1, list, PING_MAX_PEERS) == 0);

    if (failures) {
        fprintf(stderr, "FAIL: %d checks\n", failures);
        return 1;
    }
    puts("PASS: pings only to ports with them, answers counted once, the room's list");
    return 0;
}
