#ifndef RUNTIME_NET_PING_H
#define RUNTIME_NET_PING_H

/* RyuNET ping: the round trip to each player the game talks to, for the in-game menu's list of
 * the players in the room. A port with it marks its tunnelled packets (TUN_FLAG_PING, bit 1 of
 * the RYUTN1 flags, which older ports, the server and the relays pass along or ignore), and
 * only a peer that marked its packets gets pings: a port without it never sees one. They go on
 * the game socket the way the game's own packets go (direct and, while the direct path is not
 * proven, through the tunnel):
 *   "RYUPN1" kind[1] (1 ping, 2 answer) stamp[4 BE] nameLen[1] name
 * An answer returns the ping's stamp; both carry the sender's nickname. Pure bookkeeping here;
 * runtime_net.c sends and receives. Addresses as in runtime_net_tunnel.h. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PING_MAGIC "RYUPN1"
#define PING_MAGIC_LEN 6
#define PING_NAME 17 /* a nickname (3 to 16 characters) and its NUL */
#define PING_PACKET_MAX (PING_MAGIC_LEN + 1 + 4 + 1 + PING_NAME - 1)
#define PING_MAX_PEERS 16
#define PING_INTERVAL_MS 1000u
/* A peer the game has not sent to this long is not in the room any more. */
#define PING_ACTIVE_MS 5000u
/* A round trip shown is at most this old. */
#define PING_FRESH_MS 4000u
#define TUN_FLAG_PING 2

enum { PING_NO_ANSWER = -1, PING_OLDER_PORT = -2 };
typedef struct {
    char name[PING_NAME]; /* "" for a port without the ping */
    int ms;               /* the smoothed round trip, PING_NO_ANSWER or PING_OLDER_PORT */
} PingPeer;

void ping_reset(void);
/* This player's nickname, sent in pings and answers. */
void ping_set_name(const char *name);
/* A peer's tunnelled packet carried TUN_FLAG_PING. */
void ping_on_capable(uint32_t ip, uint16_t port, uint64_t now_ms);
/* The game sent to this peer. */
void ping_on_game_send(uint32_t ip, uint16_t port, uint64_t now_ms);
/* A ping that is due: its length (0: none), *ip and *port where to. Call until it returns 0. */
size_t ping_next(uint64_t now_ms, uint8_t *out, size_t cap, uint32_t *ip, uint16_t *port);
/* A datagram from a peer: 1 when it is a RyuNET ping packet, which the game must not see; then
 * *reply_len is the length of the answer to send back (0: none). */
int ping_on_packet(const uint8_t *in, size_t len, uint32_t ip, uint16_t port, uint64_t now_ms,
                   uint8_t *reply, size_t cap, size_t *reply_len);
/* The players in the room (the peers the game sends to), by nickname: their count. */
int ping_view(uint64_t now_ms, PingPeer *out, int max);

/* runtime_net.c: ping_view now, for the menu. */
int runtime_net_ping_view(PingPeer *out, int max);

#ifdef __cplusplus
}
#endif

#endif
