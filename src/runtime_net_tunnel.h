#ifndef RUNTIME_NET_TUNNEL_H
#define RUNTIME_NET_TUNNEL_H

/* RyuNET tunnel: the game's traffic to a peer it cannot reach directly (strict or carrier
 * NAT on either side) goes through the server's UDP port, wrapped, and comes out at the peer
 * as if sent straight to it. The server side is nat_tunnel.go in ryunet-demonware-ctr, which
 * describes the packets. Pure bookkeeping here; runtime_net.c sends and receives. Addresses
 * are an IPv4 address in network byte order and a port in host byte order. */

#include <stddef.h>
#include <stdint.h>

#define TUN_HEADER_LEN 13
#define TUN_KEEPALIVE "RYUKA1"
#define TUN_KEEPALIVE_LEN 6
#define TUN_KEEPALIVE_MS 15000u
/* A peer counts as reachable directly while its packets keep arriving this often. */
#define TUN_DIRECT_FRESH_MS 3000u
/* A peer that never answers through the tunnel (a port without it) stops getting copies. */
#define TUN_GIVE_UP_MS 20000u
#define TUN_PEER_IDLE_MS 60000u

/* Regional relays (ryunet-relay; relay_pass.go on the server). The keepalive asks the server
 * for a pass (its signed word for the address it sees for this socket) and the relays' list:
 *   to the server   "RYUKA1" 0x02, padded to TUN_REQUEST_LEN (the server answers no longer)
 *   from the server "RYUPS1" pass[TUN_PASS_LEN] count[1] {ip[4] port[2 BE] nameLen[1] name}
 * This socket registers with every relay (its keepalive there, and its ping):
 *   to a relay      "RYURG1" pass stamp[4]      from it  "RYURG1" status[1] stamp[4]
 * and tunnelled copies go through the nearest relay instead of the server, in the same
 * RYUTN1 format. A peer that never answers through a relay (an older port) keeps getting them
 * through the server. */
#define TUN_REQUEST_LEN 300
#define TUN_PASS_LEN 75
#define TUN_MAX_RELAYS 8
#define TUN_REGISTER_MS 5000u
/* A relay counts while it acknowledged a registration this recently. */
#define TUN_RELAY_FRESH_MS 16000u
/* A peer that answers only through the server this long after the first copy: no relay. */
#define TUN_RELAY_TRIAL_MS 10000u
/* A relay is used when its ping beats the server's by at least this much. */
#define TUN_RELAY_MARGIN_MS 10u

#define TUN_VIA_SERVER 1
#define TUN_VIA_RELAY 2

void tun_reset(void);
/* Whether the game's packets to this address may need the tunnel: a public address that is
 * not the server itself. LAN, loopback and carrier-grade (100.64/10) ones go direct only. */
int tun_candidate(uint32_t ip, uint32_t server_ip);
/* The game sends to a peer: the routes a wrapped copy takes (TUN_VIA_SERVER, TUN_VIA_RELAY
 * with *relay its index; 0: none), with *flags set to what this side tells the peer (bit 0:
 * its direct packets reach us). */
int tun_on_send(uint32_t ip, uint16_t port, uint64_t now_ms, uint8_t *flags, int *relay);
void tun_on_direct(uint32_t ip, uint16_t port, uint64_t now_ms);
void tun_on_tunnelled(uint32_t ip, uint16_t port, uint8_t flags, int via_relay, uint64_t now_ms);
/* Keepalive due (keeps this socket's path to the server open): 1 once per interval. */
int tun_keepalive_due(uint64_t now_ms);

/* The keepalive with a pass request. */
size_t tun_pass_request(uint8_t *out, size_t cap, uint64_t now_ms);
/* A packet from the server: 1 for an offer (RYUPS1), taken. */
int tun_on_offer(const uint8_t *in, size_t len, uint64_t now_ms);
int tun_relay_count(void);
/* The relay at this address: its index, or -1. */
int tun_relay_index(uint32_t ip, uint16_t port);
/* Relay i's address: 1 when there is one. */
int tun_relay_addr(int i, uint32_t *ip, uint16_t *port);
/* Relay i's registration when one is due: its length (0: none due), *ip and *port where to. */
size_t tun_relay_register(int i, uint64_t now_ms, uint8_t *out, size_t cap, uint32_t *ip, uint16_t *port);
/* A packet from relay i: 1 for a registration reply (RYURG1), taken. */
int tun_on_register_reply(int i, const uint8_t *in, size_t len, uint64_t now_ms);
/* Tunnelled copies through this relay (by name) whenever it answers, whatever the pings: a
 * player's or a test's choice (BB_ONLINE_RELAYS=<name>). NULL or "" goes back to the pings. */
void tun_force_relay(const char *name);
/* The relay tunnelled copies go through now: its index, or -1 for the server. */
int tun_best_relay(uint64_t now_ms);
/* For the log: relay i's name and ping (0 when it has not answered). */
const char *tun_relay_name(int i);
uint32_t tun_relay_ping(int i);

size_t tun_wrap(uint8_t *out, size_t cap, uint8_t flags, uint32_t ip, uint16_t port,
                const void *payload, size_t len);
/* A packet from the server: 1 for a tunnelled one (its payload starts at TUN_HEADER_LEN),
 * 0 for anything else. */
int tun_unwrap(const uint8_t *in, size_t len, uint8_t *flags, uint32_t *ip, uint16_t *port);

/* For the log: how many peers currently get tunnelled copies. */
int tun_active_peers(uint64_t now_ms);

#endif
