#ifndef RUNTIME_NET_EOS_H
#define RUNTIME_NET_EOS_H

/* Races through EOS P2P (eosbridge/eosbridge.c). Two players whose direct path does not work
 * (strict NAT, carrier NAT, a network that only reaches the server through the EOS bridge) send
 * each other's game traffic over Epic's P2P service, directly or through Epic's relays, instead
 * of through the RyuNET server's tunnel. The launcher's ryunet-bridge does the EOS side; the game
 * talks to it on 127.0.0.1 (BB_ONLINE_EOS=<its UDP port>). Pure bookkeeping here, runtime_net.c
 * sends and receives. Addresses: IPv4 in network byte order, ports in host byte order.
 *
 * Every message starts with EOSP_MAGIC, then a kind:
 *   game -> bridge   Q                        who am I (the bridge answers I)
 *                    K len id                 this peer may connect (it announced itself)
 *                    S len id payload         a datagram for that peer
 *   bridge -> game   I len id                 this PC's EOS product user id
 *                    R len id payload         a datagram from that peer
 *   peer <-> peer    A len id                 announce: my EOS id (through the tunnel, or
 *                                             directly); only to peers whose tunnelled packets
 *                                             carry TUN_FLAG_EOS, so older ports never see one
 * A peer counts as reached through EOS while its datagrams keep coming that way: its tunnelled
 * copies stop then, and come back by themselves if EOS stops. */

#include <stddef.h>
#include <stdint.h>

#define EOSP_MAGIC "RYUEP1"
#define EOSP_MAGIC_LEN 6
#define EOSP_ID_MAX 40
#define TUN_FLAG_EOS 4           /* bit 2 of a tunnelled packet's flags: the sender has EOS P2P */
#define EOSP_ANNOUNCE_MS 3000u   /* announce again this often until the peer answers through EOS */
#define EOSP_FRESH_MS 2500u      /* a peer counts as reached through EOS this long after its last datagram */
#define EOSP_QUERY_MS 2000u
#define EOSP_MAX_PEERS 32

void eosp_reset(void);
/* This PC's id (from the bridge's I); 1 when known, *id copied (EOSP_ID_MAX). */
void eosp_set_self(const char *id);
int eosp_self(char *id);
/* The query to the bridge is due (until it answered). */
int eosp_query_due(uint64_t now_ms);

/* A peer's tunnelled packet carried TUN_FLAG_EOS. */
void eosp_on_capable(uint32_t ip, uint16_t port, uint64_t now_ms);
/* The announce for this peer when one is due: its length (0: none). */
size_t eosp_announce(uint32_t ip, uint16_t port, uint64_t now_ms, uint8_t *out, size_t cap);
/* A datagram from a peer: 1 when it was an announce (taken). *learned is 1 when its id is new
 * (the bridge then needs a K), with id the peer's. */
int eosp_take_announce(const uint8_t *data, size_t len, uint32_t ip, uint16_t port, uint64_t now_ms,
                       char *id, int *learned);
/* The peer's id: 1 when known. */
int eosp_peer_id(uint32_t ip, uint16_t port, char *id);
/* The address of the peer with this id: 1 when known. */
int eosp_peer_addr(const char *id, uint32_t *ip, uint16_t *port);
/* A datagram from this peer came through EOS. */
void eosp_on_rx(uint32_t ip, uint16_t port, uint64_t now_ms);
/* Its datagrams come through EOS now: its tunnelled copies can stop. */
int eosp_reached(uint32_t ip, uint16_t port, uint64_t now_ms);

/* Messages to the bridge: their length (0 when they do not fit). */
size_t eosp_msg_query(uint8_t *out, size_t cap);
size_t eosp_msg_known(const char *id, uint8_t *out, size_t cap);
size_t eosp_msg_send(const char *id, const void *payload, size_t len, uint8_t *out, size_t cap);
/* A datagram from the bridge: its kind ('I', 'R'; 0 for anything else), with *id and, for R,
 * the payload's offset. */
int eosp_parse_bridge(const uint8_t *data, size_t len, char *id, size_t *payload_at);

#endif
