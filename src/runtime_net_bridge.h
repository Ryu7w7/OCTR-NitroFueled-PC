#ifndef RUNTIME_NET_BRIDGE_H
#define RUNTIME_NET_BRIDGE_H

/* EOS bridge (eosbridge/eosbridge.c). On a network that does not reach the RyuNET server (an ISP
 * that filters its name or its address), the launcher runs ryunet-bridge, which carries the
 * traffic through Epic Online Services to a gateway next to the server, and hands the game:
 *   BB_ONLINE_BRIDGE     "<udp port>,<server port>:<local port>,...", ports on 127.0.0.1
 *   BB_ONLINE_SERVER_IP  the server's address, as the gateway names it (its name may not
 *                        resolve on this network at all)
 * Every connection and datagram to the server then goes to the bridge's port for it instead,
 * and the bridge's datagrams come back as the server's. The rest of the game sees the server.
 * Addresses: IPv4 in network byte order, ports in host byte order. */

#include <stdint.h>

/* 1 while the game goes through the bridge. */
int net_bridge_active(void);
/* The server's address (BB_ONLINE_SERVER): with the bridge, the one it gave; otherwise its
 * name resolved. 0 when there is none. */
int net_server_ipv4(uint32_t *ip);
/* A host's address. The server's name (or address) gives net_server_ipv4's. */
int net_resolve_ipv4(const char *host, uint32_t *ip);
/* A TCP connection (or a connected UDP socket) to the server: 1 when *ip:*port was turned into
 * the bridge's local port for it. */
int net_bridge_route_tcp(uint32_t *ip, uint16_t *port);
/* A datagram to the server's NAT port: 1 when it goes to the bridge instead. */
int net_bridge_route_udp(uint32_t *ip, uint16_t *port);
/* A datagram from the bridge: 1 when it was, with *ip:*port turned into the server's NAT port. */
int net_bridge_from(uint32_t *ip, uint16_t *port);
/* The server's NAT port (BB_ONLINE_NAT_PORT, 3074). */
uint16_t net_server_nat_port(void);

#endif
