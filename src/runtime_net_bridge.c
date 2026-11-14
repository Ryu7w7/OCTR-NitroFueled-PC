/* runtime_net.h first: on Linux it turns on _GNU_SOURCE (getaddrinfo). */
#include "runtime_net.h"
#include "runtime_net_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRIDGE_MAX_PORTS 16

typedef struct {
    int active;
    uint16_t udp;                        /* the bridge's UDP port on 127.0.0.1 */
    int count;
    uint16_t server_port[BRIDGE_MAX_PORTS];
    uint16_t local_port[BRIDGE_MAX_PORTS];
    uint32_t server_ip;
} Bridge;

static Bridge g_bridge;
static int g_loaded; /* 0 not yet, 1 loading, 2 done */

/* BB_ONLINE_BRIDGE and BB_ONLINE_SERVER_IP, read once. A wrong value leaves the bridge off:
 * the game then goes to the server directly, as without it. */
static const Bridge *bridge(void) {
    int state = __atomic_load_n(&g_loaded, __ATOMIC_ACQUIRE);
    if (state == 2) return &g_bridge;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_loaded, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g_loaded, __ATOMIC_ACQUIRE) != 2) {}
        return &g_bridge;
    }
    Bridge b;
    memset(&b, 0, sizeof b);
    const char *map = getenv("BB_ONLINE_BRIDGE");
    const char *ip = getenv("BB_ONLINE_SERVER_IP");
    struct in_addr addr;
    if (map && *map && ip && inet_pton(AF_INET, ip, &addr) == 1) {
        char copy[512];
        snprintf(copy, sizeof copy, "%s", map);
        char *save = NULL;
        int ok = 1;
        for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            char *colon = strchr(t, ':');
            if (!colon) {
                const int udp = atoi(t);
                if (udp <= 0 || udp > 65535) ok = 0;
                b.udp = (uint16_t)udp;
                continue;
            }
            const int sp = atoi(t), lp = atoi(colon + 1);
            if (sp <= 0 || sp > 65535 || lp <= 0 || lp > 65535 || b.count >= BRIDGE_MAX_PORTS) { ok = 0; continue; }
            b.server_port[b.count] = (uint16_t)sp;
            b.local_port[b.count++] = (uint16_t)lp;
        }
        if (ok && b.udp && b.count) {
            b.active = 1;
            b.server_ip = addr.s_addr;
            printf("[NET] EOS bridge: the server (%s) through Epic Online Services, local ports from %u\n", ip, b.udp);
        } else {
            printf("[NET] EOS bridge: BB_ONLINE_BRIDGE=%s not understood; going to the server directly\n", map);
            memset(&b, 0, sizeof b);
        }
        fflush(stdout);
    }
    g_bridge = b;
    __atomic_store_n(&g_loaded, 2, __ATOMIC_RELEASE);
    return &g_bridge;
}

int net_bridge_active(void) { return bridge()->active; }

uint16_t net_server_nat_port(void) {
    const char *p = getenv("BB_ONLINE_NAT_PORT");
    return (uint16_t)(p && atoi(p) > 0 && atoi(p) < 65536 ? atoi(p) : 3074);
}

static int resolve(const char *host, uint32_t *ip) {
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) {
        *ip = a.s_addr;
        return 1;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    *ip = ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 1;
}

int net_server_ipv4(uint32_t *ip) {
    const Bridge *b = bridge();
    if (b->active) {
        *ip = b->server_ip;
        return 1;
    }
    const char *srv = getenv("BB_ONLINE_SERVER");
    return srv && *srv && resolve(srv, ip);
}

int net_resolve_ipv4(const char *host, uint32_t *ip) {
    if (!host || !*host) return 0;
    const Bridge *b = bridge();
    const char *srv = getenv("BB_ONLINE_SERVER");
    if (b->active && srv && _stricmp(host, srv) == 0) {
        *ip = b->server_ip;
        return 1;
    }
    return resolve(host, ip);
}

int net_bridge_route_tcp(uint32_t *ip, uint16_t *port) {
    const Bridge *b = bridge();
    if (!b->active || *ip != b->server_ip) return 0;
    for (int i = 0; i < b->count; i++) {
        if (b->server_port[i] != *port) continue;
        *ip = htonl(INADDR_LOOPBACK);
        *port = b->local_port[i];
        return 1;
    }
    if (*port == net_server_nat_port()) { /* a connected UDP socket to the NAT port */
        *ip = htonl(INADDR_LOOPBACK);
        *port = b->udp;
        return 1;
    }
    return 0;
}

int net_bridge_route_udp(uint32_t *ip, uint16_t *port) {
    const Bridge *b = bridge();
    if (!b->active || *ip != b->server_ip || *port != net_server_nat_port()) return 0;
    *ip = htonl(INADDR_LOOPBACK);
    *port = b->udp;
    return 1;
}

int net_bridge_from(uint32_t *ip, uint16_t *port) {
    const Bridge *b = bridge();
    if (!b->active || *ip != htonl(INADDR_LOOPBACK) || *port != b->udp) return 0;
    *ip = b->server_ip;
    *port = net_server_nat_port();
    return 1;
}
