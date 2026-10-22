#include "runtime_net.h"
#include "runtime_net_overrides.h"
#include "runtime_net_tunnel.h"
#include "runtime_net_ping.h"
#include "runtime_friends.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define ORBIS_OK 0
#define ORBIS_NET_ENETUNREACH ((int32_t)0x80410133)
#define ORBIS_NET_EINVAL      ((int32_t)0x80410116)
#define ORBIS_NET_EWOULDBLOCK ((int32_t)0x80410123)
#define ORBIS_NET_EBADF       ((int32_t)0x80410109)
#define ORBIS_NET_EHOSTUNREACH ((int32_t)0x80410141)

#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR  ((int32_t)0x80412107)

static int g_net_initialized = 0;
static int32_t g_net_errno = 0;

int runtime_net_is_online(void) {
    const char *val = getenv("BB_ONLINE");
    return (val && strcmp(val, "1") == 0);
}

void runtime_net_trace(const char *fmt, ...) {
    static int trace_checked = 0;
    static int trace_enabled = 0;
    if (!trace_checked) {
        trace_checked = 1;
        const char *t = getenv("BB_NET_TRACE");
        if ((t && strcmp(t, "1") == 0) || runtime_net_is_online()) trace_enabled = 1;
    }
    if (!trace_enabled) return;

    va_list ap;
    va_start(ap, fmt);
    printf("[NET] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

int32_t runtime_net_init(void) {
    if (!g_net_initialized) {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[NET] WSAStartup failed\n");
            return -1;
        }
#endif
        net_overrides_init();
        g_net_initialized = 1;
        runtime_net_trace("sceNetInit completed");
    }
    runtime_friends_start();
    return ORBIS_OK;
}

int32_t runtime_net_term(void) {
    if (g_net_initialized) {
#ifdef _WIN32
        WSACleanup();
#endif
        g_net_initialized = 0;
        runtime_net_trace("sceNetTerm completed");
    }
    return ORBIS_OK;
}

int32_t *runtime_net_errno_loc(void) {
    return &g_net_errno;
}

/* Socket table management */
#define MAX_VIRTUAL_SOCKETS 256

typedef struct {
    SOCKET s;
    int in_use;
    int is_stream;
    int non_blocking;
    int tunnel; /* the game's UDP socket (port 3074): runtime_net_tunnel.h */
    char name[32];
} VSocket;

static VSocket g_sockets[MAX_VIRTUAL_SOCKETS];

static int vsocket_alloc(SOCKET s, int is_stream, const char *name) {
    for (int i = 0; i < MAX_VIRTUAL_SOCKETS; i++) {
        if (!g_sockets[i].in_use) {
            g_sockets[i].s = s;
            g_sockets[i].in_use = 1;
            g_sockets[i].is_stream = is_stream;
            g_sockets[i].non_blocking = 0;
            g_sockets[i].tunnel = 0;
            if (name) strncpy(g_sockets[i].name, name, sizeof(g_sockets[i].name) - 1);
            else g_sockets[i].name[0] = '\0';
            return i + 1; /* 1-based handle */
        }
    }
    return -1;
}

static VSocket *vsocket_get(int fd) {
    int idx = fd - 1;
    if (idx < 0 || idx >= MAX_VIRTUAL_SOCKETS || !g_sockets[idx].in_use) return NULL;
    return &g_sockets[idx];
}

static void vsocket_free(int fd) {
    int idx = fd - 1;
    if (idx >= 0 && idx < MAX_VIRTUAL_SOCKETS) {
        g_sockets[idx].in_use = 0;
        g_sockets[idx].s = INVALID_SOCKET;
    }
}

int32_t runtime_net_socket(const char *name, int domain, int type, int protocol) {
    runtime_net_trace("sceNetSocket(name='%s', dom=%d, type=0x%x, proto=%d)",
                      name ? name : "", domain, type, protocol);

    if (!runtime_net_is_online()) {
        g_net_errno = 51;
        runtime_net_trace("sceNetSocket (offline) -> ENETUNREACH");
        return ORBIS_NET_ENETUNREACH;
    }
    runtime_net_init();

    int win_domain = domain;
    if (domain == 28) {
#ifdef AF_INET6
        win_domain = AF_INET6;
#else
        win_domain = 23;
#endif
    }

    int base_type = type & 0xF;
    int is_stream = (base_type == SOCK_STREAM);
    int is_nonblock = ((type & 0x20000000) != 0) || ((type & 0x04000000) != 0);

    SOCKET s = socket(win_domain, base_type, protocol);
    if (s == INVALID_SOCKET) {
        g_net_errno = WSAGetLastError();
        runtime_net_trace("sceNetSocket failed: %d (win_domain=%d, base_type=%d, proto=%d)",
                          g_net_errno, win_domain, base_type, protocol);
        return -1;
    }

    if (is_nonblock) {
        unsigned long on = 1;
        ioctlsocket(s, FIONBIO, &on);
    }

#ifdef _WIN32
    if (!is_stream) {
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        BOOL bNewBehavior = FALSE;
        DWORD dwBytesReturned = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
    }
#endif

    int vfd = vsocket_alloc(s, is_stream, name);
    if (vfd > 0 && is_nonblock) {
        VSocket *vs = vsocket_get(vfd);
        if (vs) vs->non_blocking = 1;
    }

    runtime_net_trace("sceNetSocket -> fd=%d (sock=%llu, nonblock=%d)",
                      vfd, (unsigned long long)s, is_nonblock);
    return vfd;
}

int32_t runtime_net_socket_close(int s) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    runtime_net_trace("sceNetSocketClose(fd=%d)", s);
    closesocket(vs->s);
    vsocket_free(s);
    return ORBIS_OK;
}

int32_t runtime_net_socket_abort(int s, int flags) {
    (void)flags;
    return runtime_net_socket_close(s);
}

int32_t runtime_net_connect(int s, const OrbisNetSockaddr *addr, uint32_t addrlen) {
    (void)addrlen;
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;
    if (!addr) return ORBIS_NET_EINVAL;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    const OrbisNetSockaddrIn *in = (const OrbisNetSockaddrIn *)addr;
    sin.sin_family = in->sin_family;
    sin.sin_port = in->sin_port;
    memcpy(&sin.sin_addr, &in->sin_addr, sizeof(sin.sin_addr));

    char ip_str[32] = {0};
    inet_ntop(AF_INET, &sin.sin_addr, ip_str, sizeof(ip_str));
    runtime_net_trace("sceNetConnect(fd=%d, to=%s:%u)", s, ip_str, ntohs(sin.sin_port));

    int ret = connect(vs->s, (struct sockaddr *)&sin, sizeof(sin));
    if (ret != 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            g_net_errno = 36; /* EINPROGRESS */
            runtime_net_trace("sceNetConnect -> EINPROGRESS");
            return -1;
        }
        g_net_errno = err;
        runtime_net_trace("sceNetConnect failed: %d", err);
        return -1;
    }
    return ORBIS_OK;
}

/* Whether the RyuNET server runs on this machine: no server set, loopback, or one of this
 * PC's own addresses (a Radmin/ZeroTier/LAN IP its friends also use). Only an address that
 * belongs to this machine can be bound, which is how it is told apart. The game then must not
 * take the server's UDP 3074: Windows lets both sockets bind it, and the game's NAT probes
 * came back to the game itself instead of the server ("could not start the network"). */
static int server_is_this_machine(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    const char *srv = getenv("BB_ONLINE_SERVER");
    if (!srv || !*srv || strcmp(srv, "127.0.0.1") == 0 || _stricmp(srv, "localhost") == 0) return cached = 1;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(srv, NULL, &hints, &res) != 0 || !res) return cached = 0;
    struct sockaddr_in addr = *(const struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    if ((ntohl(addr.sin_addr.s_addr) >> 24) == 127) return cached = 1;
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) return cached = 0;
    addr.sin_port = 0;
    cached = bind(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    closesocket(probe);
    runtime_net_trace("RyuNET server %s is %s", srv, cached ? "this machine: the game avoids UDP 3074" : "another machine");
    return cached;
}

/* This PC's address on the network it reaches the RyuNET server through (the LAN, or a
 * Radmin/ZeroTier one). The game puts it in its host address as the local address, which
 * players behind the same router (same public IP) connect through, as two PS4s at home do.
 * 127.0.0.1 there made each of them connect to itself, so they could never join each other.
 * Connecting a UDP socket sends nothing: it only asks the OS which route and source it uses. */
static const char *local_ipv4(void) {
    static char cached[16];
    if (cached[0]) return cached;
    snprintf(cached, sizeof(cached), "127.0.0.1");
    const char *srv = getenv("BB_ONLINE_SERVER");
    if (!srv || !*srv) return cached;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(srv, NULL, &hints, &res) != 0 || !res) return cached;
    struct sockaddr_in to = *(const struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    to.sin_port = htons(3074);
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) return cached;
    struct sockaddr_in me;
    socklen_t me_len = sizeof(me);
    memset(&me, 0, sizeof(me));
    if (connect(probe, (struct sockaddr *)&to, sizeof(to)) == 0 &&
        getsockname(probe, (struct sockaddr *)&me, &me_len) == 0 && me.sin_addr.s_addr != 0) {
        const unsigned char *b = (const unsigned char *)&me.sin_addr.s_addr;
        snprintf(cached, sizeof(cached), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    closesocket(probe);
    runtime_net_trace("Local address for other players on this network: %s", cached);
    return cached;
}

/* --- RyuNET tunnel (runtime_net_tunnel.h): the game socket's path through the server ------ */

static uint64_t now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* The server's NAT port (UDP 3074, BB_ONLINE_NAT_PORT); 0 when the tunnel is off
 * (BB_ONLINE_TUNNEL=0, offline, or the server's address does not resolve). */
static int tunnel_server(struct sockaddr_in *out) {
    static int state = -1;
    static struct sockaddr_in server;
    if (state < 0) {
        state = 0;
        const char *off = getenv("BB_ONLINE_TUNNEL");
        const char *srv = getenv("BB_ONLINE_SERVER");
        if (runtime_net_is_online() && !(off && strcmp(off, "0") == 0) && srv && *srv) {
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(srv, NULL, &hints, &res) == 0 && res) {
                server = *(const struct sockaddr_in *)res->ai_addr;
                freeaddrinfo(res);
                const char *p = getenv("BB_ONLINE_NAT_PORT");
                server.sin_port = htons((uint16_t)(p && atoi(p) > 0 ? atoi(p) : 3074));
                state = 1;
                runtime_net_trace("Tunnel: through %s:%u for peers without a direct path",
                                  inet_ntoa(server.sin_addr), ntohs(server.sin_port));
            }
        }
    }
    if (state == 1 && out) *out = server;
    return state == 1;
}

static int from_server(const struct sockaddr_in *from, const struct sockaddr_in *server) {
    return from->sin_addr.s_addr == server->sin_addr.s_addr && from->sin_port == server->sin_port;
}

static struct sockaddr_in addr_of(uint32_t ip, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    return a;
}

/* Keeps this socket's paths open: to the server (asking for a pass for the regional relays
 * unless BB_ONLINE_RELAYS=0) and, registered with that pass, to every relay. */
static void tunnel_keepalive(VSocket *vs, const struct sockaddr_in *server) {
    static int relays_wanted = -1;
    if (relays_wanted < 0) {
        const char *v = getenv("BB_ONLINE_RELAYS");
        relays_wanted = !(v && strcmp(v, "0") == 0);
        if (relays_wanted && v && *v && strcmp(v, "1") != 0) {
            tun_force_relay(v);
            runtime_net_trace("Tunnel: relay %s chosen (BB_ONLINE_RELAYS)", v);
        }
    }
    const uint64_t now = now_ms();
    if (tun_keepalive_due(now)) {
        uint8_t request[TUN_REQUEST_LEN];
        const size_t n = relays_wanted ? tun_pass_request(request, sizeof(request), now) : 0;
        if (n) sendto(vs->s, (const char *)request, (int)n, 0, (const struct sockaddr *)server, sizeof(*server));
        else sendto(vs->s, TUN_KEEPALIVE, TUN_KEEPALIVE_LEN, 0, (const struct sockaddr *)server, sizeof(*server));
    }
    const int relays = tun_relay_count();
    for (int i = 0; i < relays; i++) {
        uint8_t reg[6 + TUN_PASS_LEN + 4];
        uint32_t ip;
        uint16_t port;
        const size_t n = tun_relay_register(i, now, reg, sizeof(reg), &ip, &port);
        if (!n) continue;
        const struct sockaddr_in to = addr_of(ip, port);
        sendto(vs->s, (const char *)reg, (int)n, 0, (const struct sockaddr *)&to, sizeof(to));
    }
}

/* RyuNET ping (runtime_net_ping.h): the round trip to the players in the room, on the game
 * socket of a port with the tunnel. Off with BB_ONLINE_PING=0. */
static int ping_enabled(void) {
    static int state = -1;
    if (state < 0) {
        const char *v = getenv("BB_ONLINE_PING");
        state = !(v && strcmp(v, "0") == 0) && tunnel_server(NULL);
        if (state) ping_set_name(getenv("BB_ONLINE_NAME"));
    }
    return state;
}

int runtime_net_ping_view(PingPeer *out, int max) {
    return ping_view(now_ms(), out, max);
}

static void tunnel_copy(VSocket *vs, const void *msg, size_t len, const struct sockaddr_in *to);

/* A ping packet the way the game's packets to that peer go: direct, and through the tunnel
 * while the direct path is not proven. */
static void ping_send(VSocket *vs, const uint8_t *packet, size_t n, const struct sockaddr_in *to) {
    sendto(vs->s, (const char *)packet, (int)n, 0, (const struct sockaddr *)to, sizeof(*to));
    tunnel_copy(vs, packet, n, to);
}

/* After the game's send to a peer: the pings that are due. */
static void ping_after_send(VSocket *vs, const struct sockaddr_in *to) {
    struct sockaddr_in server;
    if (!ping_enabled() || !tunnel_server(&server)) return;
    if (!tun_candidate(to->sin_addr.s_addr, server.sin_addr.s_addr) ||
        tun_relay_index(to->sin_addr.s_addr, ntohs(to->sin_port)) >= 0)
        return;
    const uint64_t now = now_ms();
    ping_on_game_send(to->sin_addr.s_addr, ntohs(to->sin_port), now);
    uint8_t packet[PING_PACKET_MAX];
    uint32_t ip;
    uint16_t port;
    for (size_t n; (n = ping_next(now, packet, sizeof(packet), &ip, &port));) {
        const struct sockaddr_in peer = addr_of(ip, port);
        ping_send(vs, packet, n, &peer);
    }
}

/* A datagram from a peer (unwrapped): 1 when it was a ping packet, answered here. */
static int ping_take(VSocket *vs, const uint8_t *data, int len, const struct sockaddr_in *from) {
    if (len < PING_MAGIC_LEN || memcmp(data, PING_MAGIC, PING_MAGIC_LEN) != 0 || !ping_enabled()) return 0;
    uint8_t reply[PING_PACKET_MAX];
    size_t reply_len = 0;
    if (!ping_on_packet(data, (size_t)len, from->sin_addr.s_addr, ntohs(from->sin_port), now_ms(), reply,
                        sizeof(reply), &reply_len))
        return 0;
    if (reply_len) ping_send(vs, reply, reply_len, from);
    return 1;
}

/* After the game's own send to a peer: a wrapped copy through the nearest relay and/or the
 * server while the direct path is not proven both ways. */
static void tunnel_copy(VSocket *vs, const void *msg, size_t len, const struct sockaddr_in *to) {
    struct sockaddr_in server;
    if (!vs->tunnel || !tunnel_server(&server)) return;
    tunnel_keepalive(vs, &server);
    if (!tun_candidate(to->sin_addr.s_addr, server.sin_addr.s_addr) || len > 2048) return;
    if (tun_relay_index(to->sin_addr.s_addr, ntohs(to->sin_port)) >= 0) return;
    uint8_t flags;
    int relay = -1;
    const int route = tun_on_send(to->sin_addr.s_addr, ntohs(to->sin_port), now_ms(), &flags, &relay);
    if (!route) return;
    if (ping_enabled()) flags |= TUN_FLAG_PING;
    uint8_t wrapped[2048 + TUN_HEADER_LEN];
    size_t n = tun_wrap(wrapped, sizeof(wrapped), flags, to->sin_addr.s_addr, ntohs(to->sin_port), msg, len);
    if (!n) return;
    if (route & TUN_VIA_SERVER)
        sendto(vs->s, (const char *)wrapped, (int)n, 0, (const struct sockaddr *)&server, sizeof(server));
    uint32_t ip;
    uint16_t port;
    if ((route & TUN_VIA_RELAY) && tun_relay_addr(relay, &ip, &port)) {
        static int traced = -2;
        if (relay != traced) {
            traced = relay;
            runtime_net_trace("Tunnel: through relay %s (%u ms) for peers without a direct path",
                              tun_relay_name(relay), tun_relay_ping(relay));
        }
        const struct sockaddr_in via = addr_of(ip, port);
        sendto(vs->s, (const char *)wrapped, (int)n, 0, (const struct sockaddr *)&via, sizeof(via));
    }
}

int32_t runtime_net_bind(int s, const OrbisNetSockaddr *addr, uint32_t addrlen) {
    (void)addrlen;
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;
    if (!addr) return ORBIS_NET_EINVAL;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    const OrbisNetSockaddrIn *in = (const OrbisNetSockaddrIn *)addr;
    sin.sin_family = in->sin_family;
    sin.sin_port = in->sin_port;
    const int game_port = ntohs(in->sin_port) == 3074;
    const char *p_env = getenv("BB_ONLINE_PORT");
    if (p_env && atoi(p_env) > 0) {
        sin.sin_port = htons((uint16_t)atoi(p_env));
    } else if (ntohs(sin.sin_port) == 3074 && server_is_this_machine()) {
        sin.sin_port = htons(3075);
    }

    /* Enable SO_REUSEADDR only for TCP streams. On UDP (Windows), SO_REUSEADDR allows port stealing
     * which breaks local multi-instance testing. Keeping it off allows WSAEADDRINUSE to trigger the retry loop. */
    if (vs->is_stream) {
        int opt = 1;
        setsockopt(vs->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    }

    runtime_net_trace("sceNetBind(fd=%d, port=%u)", s, ntohs(sin.sin_port));
    int ret = bind(vs->s, (struct sockaddr *)&sin, sizeof(sin));
    if (ret != 0 && (WSAGetLastError() == WSAEADDRINUSE || WSAGetLastError() == 10048)) {
        for (int p_retry = 1; p_retry <= 10; p_retry++) {
            sin.sin_port = htons(ntohs(sin.sin_port) + 1);
            runtime_net_trace("sceNetBind: port busy, retrying port %u", ntohs(sin.sin_port));
            ret = bind(vs->s, (struct sockaddr *)&sin, sizeof(sin));
            if (ret == 0) break;
        }
    }
    if (ret != 0) {
        g_net_errno = WSAGetLastError();
        runtime_net_trace("sceNetBind(fd=%d, port=%u) failed: win err %d", s, ntohs(sin.sin_port), g_net_errno);
        return -1;
    }
    struct sockaddr_in server;
    if (!vs->is_stream && game_port && tunnel_server(&server)) {
        vs->tunnel = 1;
        tunnel_keepalive(vs, &server);
    }
    return ORBIS_OK;
}

int32_t runtime_net_listen(int s, int backlog) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    runtime_net_trace("sceNetListen(fd=%d, backlog=%d)", s, backlog);
    int ret = listen(vs->s, backlog);
    if (ret != 0) {
        g_net_errno = WSAGetLastError();
        return -1;
    }
    return ORBIS_OK;
}

int32_t runtime_net_accept(int s, OrbisNetSockaddr *addr, uint32_t *addrlen) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    struct sockaddr_in sin;
    NativeSockLen sin_len = sizeof(sin);
    SOCKET client = accept(vs->s, (struct sockaddr *)&sin, &sin_len);
    if (client == INVALID_SOCKET) {
        g_net_errno = WSAGetLastError();
        return -1;
    }

    if (addr) {
        OrbisNetSockaddrIn *out = (OrbisNetSockaddrIn *)addr;
        out->sin_len = sizeof(OrbisNetSockaddrIn);
        out->sin_family = (uint8_t)sin.sin_family;
        out->sin_port = sin.sin_port;
        memcpy(&out->sin_addr, &sin.sin_addr, sizeof(out->sin_addr));
        memset(out->sin_zero, 0, sizeof(out->sin_zero));
    }
    if (addrlen) *addrlen = sizeof(OrbisNetSockaddrIn);

    int vfd = vsocket_alloc(client, 1, "accepted");
    runtime_net_trace("sceNetAccept(fd=%d) -> new fd=%d", s, vfd);
    return vfd;
}

/* The game opens its UDP 3074 on the router through UPnP (AddPortMapping, external port 3074 to
 * this PC's 3074). On a network with several players every PC asked for the same external port,
 * and the router gave it to the last one: the others stopped receiving their lobby's traffic
 * (kicked out of the lobby) or received another PC's (karts under the track, teleports), as the
 * logs of three PCs behind one router showed (2026-10-19). Each PC now asks for its own external
 * port, from its address on the network (3075 + the last byte), still to its local 3074; the
 * server sees that port and gives it to the peers. Same length as "3074": the request's
 * Content-Length stays right. BB_UPNP_PORT=3074 keeps the game's own. */
static int upnp_external_port(void) {
    static int port;
    if (port) return port;
    const char *env = getenv("BB_UPNP_PORT");
    if (env && atoi(env) >= 1024 && atoi(env) <= 9999) return port = atoi(env);
    unsigned a = 0, b = 0, c = 0, d = 0;
    port = sscanf(local_ipv4(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a != 127 ? 3075 + (int)(d & 255) : 3074;
    return port;
}

/* A copy of a UPnP SOAP request with <NewExternalPort>3074< replaced, or NULL when there is none. */
static char *upnp_rewrite(const void *msg, size_t len) {
    static const char tag[] = "<NewExternalPort>3074<";
    const char *data = (const char *)msg;
    if (len < sizeof tag || len > 65536) return NULL;
    int found = 0;
    for (size_t at = 0; !found && at + sizeof tag - 1 <= len; ++at)
        found = data[at] == '<' && memcmp(data + at, tag, sizeof tag - 1) == 0;
    char *copy = found ? malloc(len) : NULL;
    if (!copy) return NULL;
    memcpy(copy, data, len);
    char port[8];
    snprintf(port, sizeof port, "%04d", upnp_external_port());
    int changed = 0;
    for (size_t at = 0; at + sizeof tag - 1 <= len; ++at) {
        if (copy[at] == '<' && memcmp(copy + at, tag, sizeof tag - 1) == 0) {
            memcpy(copy + at + strlen("<NewExternalPort>"), port, 4);
            ++changed;
        }
    }
    static int logged;
    if (changed && !logged) {
        logged = 1;
        printf("[NET] UPnP: external port 3074 -> %s (one per PC on this network; BB_UPNP_PORT sets it)\n", port);
        fflush(stdout);
    }
    return copy;
}

int32_t runtime_net_send(int s, const void *msg, size_t len, int flags) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    int win_flags = 0;
    if (flags & 0x1) win_flags |= MSG_OOB;
    if (flags & 0x4) win_flags |= MSG_DONTROUTE;

    char *upnp = vs->is_stream && upnp_external_port() != 3074 ? upnp_rewrite(msg, len) : NULL;
    int ret = send(vs->s, upnp ? upnp : (const char *)msg, (int)len, win_flags);
    free(upnp);
    if (ret < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            g_net_errno = 35; /* EWOULDBLOCK */
            return -1;
        }
        g_net_errno = err;
        runtime_net_trace("sceNetSend(fd=%d, len=%zu, flags=0x%x) -> err %d (wsa %d)", s, len, flags, g_net_errno, err);
        return -1;
    }
    runtime_net_trace("sceNetSend(fd=%d, len=%zu) -> %d bytes", s, len, ret);
    return ret;
}

int32_t runtime_net_sendto(int s, const void *msg, size_t len, int flags, const OrbisNetSockaddr *to, uint32_t tolen) {
    (void)tolen;
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    int win_flags = 0;
    if (flags & 0x1) win_flags |= MSG_OOB;
    if (flags & 0x4) win_flags |= MSG_DONTROUTE;

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    if (to) {
        const OrbisNetSockaddrIn *in = (const OrbisNetSockaddrIn *)to;
        sin.sin_family = in->sin_family;
        sin.sin_port = in->sin_port;
        memcpy(&sin.sin_addr, &in->sin_addr, sizeof(sin.sin_addr));
        runtime_net_trace("sceNetSendto(fd=%d, len=%zu, to=%s:%u)", s, len, inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
    } else {
        runtime_net_trace("sceNetSendto(fd=%d, len=%zu)", s, len);
    }

    int ret = sendto(vs->s, (const char *)msg, (int)len, win_flags,
                     to ? (struct sockaddr *)&sin : NULL, to ? sizeof(sin) : 0);
    if (ret < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            g_net_errno = 35;
            return -1;
        }
        g_net_errno = err;
        runtime_net_trace("sceNetSendto(fd=%d) failed: win err %d", s, err);
        return -1;
    }
    if (to && vs->tunnel) {
        tunnel_copy(vs, msg, len, &sin);
        ping_after_send(vs, &sin);
    }
    return ret;
}

int32_t runtime_net_recv(int s, void *buf, size_t len, int flags) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    int win_flags = 0;
    if (flags & 0x1) win_flags |= MSG_OOB;
    if (flags & 0x2) win_flags |= MSG_PEEK;
    if (flags & 0x40) win_flags |= MSG_WAITALL;

    int ret = recv(vs->s, (char *)buf, (int)len, win_flags);
    if (ret < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            g_net_errno = 35;
            return -1;
        }
        g_net_errno = err;
        runtime_net_trace("sceNetRecv(fd=%d, len=%zu, flags=0x%x) -> err %d (wsa %d)", s, len, flags, g_net_errno, err);
        return -1;
    }
    if (ret > 0) {
        runtime_net_trace("sceNetRecv(fd=%d, len=%zu) -> %d bytes (first=0x%02X)", s, len, ret, ((unsigned char *)buf)[0]);
    } else {
        runtime_net_trace("sceNetRecv(fd=%d) -> EOF (0 bytes)", s);
    }
    return ret;
}

int32_t runtime_net_recvfrom(int s, void *buf, size_t len, int flags, OrbisNetSockaddr *from, uint32_t *fromlen) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    int win_flags = 0;
    if (flags & 0x1) win_flags |= MSG_OOB;
    if (flags & 0x2) win_flags |= MSG_PEEK;
    if (flags & 0x40) win_flags |= MSG_WAITALL;

    struct sockaddr_in sin;
    NativeSockLen sin_len = sizeof(sin);
    struct sockaddr_in server;
    const int tunnel = vs->tunnel && tunnel_server(&server);
    /* The game socket reads into a buffer with room for a tunnel header, which is stripped.
     * A peek there takes the datagram and keeps it, unwrapped, for the next read. */
    static struct { int fd; int len; struct sockaddr_in from; uint8_t data[2048]; } peeked;
    if (tunnel && peeked.fd == s) {
        int ret = peeked.len > (int)len ? (int)len : peeked.len;
        memcpy(buf, peeked.data, (size_t)ret);
        sin = peeked.from;
        if (!(win_flags & MSG_PEEK)) peeked.fd = 0;
        if (from) {
            OrbisNetSockaddrIn *out = (OrbisNetSockaddrIn *)from;
            out->sin_len = sizeof(OrbisNetSockaddrIn);
            out->sin_family = (uint8_t)sin.sin_family;
            out->sin_port = sin.sin_port;
            memcpy(&out->sin_addr, &sin.sin_addr, sizeof(out->sin_addr));
            memset(out->sin_zero, 0, sizeof(out->sin_zero));
        }
        if (fromlen) *fromlen = sizeof(OrbisNetSockaddrIn);
        return ret;
    }
    uint8_t wrapped[2048 + TUN_HEADER_LEN];
    char *into = tunnel ? (char *)wrapped : (char *)buf;
    const int cap = tunnel ? (int)sizeof(wrapped) : (int)len;
    int ret;
next_datagram: /* after a tunnel control packet, which is not the game's */
    sin_len = sizeof(sin);
    ret = recvfrom(vs->s, into, cap, tunnel ? (win_flags & ~MSG_PEEK) : win_flags,
                   (struct sockaddr *)&sin, &sin_len);
    if (ret < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            g_net_errno = 35;
            return -1;
        }
        g_net_errno = err;
        return -1;
    }
    if (tunnel) {
        tunnel_keepalive(vs, &server);
        const uint8_t *payload = wrapped;
        uint8_t tflags;
        uint32_t ip;
        uint16_t port;
        int relay = -1;
        if (from_server(&sin, &server)) {
            /* The server's pass and relay list. */
            if (tun_on_offer(wrapped, (size_t)ret, now_ms())) goto next_datagram;
            /* The server's own NAT replies (0x15, 0x1F, 0x0B...) go to the game unchanged. */
            if (tun_unwrap(wrapped, (size_t)ret, &tflags, &ip, &port)) {
                tun_on_tunnelled(ip, port, tflags, 0, now_ms());
                if (tflags & TUN_FLAG_PING) ping_on_capable(ip, port, now_ms());
                sin.sin_addr.s_addr = ip;
                sin.sin_port = htons(port);
                payload = wrapped + TUN_HEADER_LEN;
                ret -= TUN_HEADER_LEN;
            }
        } else if ((relay = tun_relay_index(sin.sin_addr.s_addr, ntohs(sin.sin_port))) >= 0) {
            /* A relay: its registration replies, and peers' tunnelled packets. Nothing else
             * from it is the game's. */
            if (!tun_unwrap(wrapped, (size_t)ret, &tflags, &ip, &port)) {
                tun_on_register_reply(relay, wrapped, (size_t)ret, now_ms());
                goto next_datagram;
            }
            tun_on_tunnelled(ip, port, tflags, 1, now_ms());
            if (tflags & TUN_FLAG_PING) ping_on_capable(ip, port, now_ms());
            sin.sin_addr.s_addr = ip;
            sin.sin_port = htons(port);
            payload = wrapped + TUN_HEADER_LEN;
            ret -= TUN_HEADER_LEN;
        } else {
            /* BB_ONLINE_DIRECT=0 (tests): peers' direct packets are dropped, so the game's
             * traffic with them has to come through the tunnel. */
            static int direct_off = -1;
            if (direct_off < 0) {
                const char *v = getenv("BB_ONLINE_DIRECT");
                direct_off = v && strcmp(v, "0") == 0;
                if (direct_off) runtime_net_trace("Tunnel: direct packets from peers ignored (BB_ONLINE_DIRECT=0)");
            }
            if (direct_off && tun_candidate(sin.sin_addr.s_addr, server.sin_addr.s_addr)) goto next_datagram;
            tun_on_direct(sin.sin_addr.s_addr, ntohs(sin.sin_port), now_ms());
        }
        if (ping_take(vs, payload, ret, &sin)) goto next_datagram;
        if ((win_flags & MSG_PEEK) && ret <= (int)sizeof(peeked.data)) {
            peeked.fd = s;
            peeked.len = ret;
            peeked.from = sin;
            memcpy(peeked.data, payload, (size_t)ret);
        }
        if ((size_t)ret > len) ret = (int)len; /* a datagram longer than the buffer is cut, as recvfrom does */
        memcpy(buf, payload, (size_t)ret);
    }

    if (from) {
        OrbisNetSockaddrIn *out = (OrbisNetSockaddrIn *)from;
        out->sin_len = sizeof(OrbisNetSockaddrIn);
        out->sin_family = (uint8_t)sin.sin_family;
        out->sin_port = sin.sin_port;
        memcpy(&out->sin_addr, &sin.sin_addr, sizeof(out->sin_addr));
        memset(out->sin_zero, 0, sizeof(out->sin_zero));
    }
    if (fromlen) *fromlen = sizeof(OrbisNetSockaddrIn);

    runtime_net_trace("sceNetRecvfrom(fd=%d, len=%d, from=%s:%u)", s, ret, inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
    return ret;
}

int32_t runtime_net_setsockopt(int s, int level, int optname, const void *optval, uint32_t optlen) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    if (optname == ORBIS_NET_SO_NBIO) {
        unsigned long mode = optval ? *(const uint32_t *)optval : 0;
        vs->non_blocking = (mode != 0);
        int ret = ioctlsocket(vs->s, FIONBIO, &mode);
        runtime_net_trace("sceNetSetsockopt(fd=%d, SO_NBIO=%lu) -> %d", s, mode, ret);
        return ret == 0 ? ORBIS_OK : -1;
    }

    int win_level = level;
    int win_optname = optname;

    if (level == 0xffff) {
        win_level = SOL_SOCKET;
        switch (optname) {
            case 0x0004: win_optname = SO_REUSEADDR; break;
            case 0x0008: win_optname = SO_KEEPALIVE; break;
            case 0x0080: win_optname = SO_LINGER; break;
            case 0x0100: win_optname = SO_OOBINLINE; break;
            case 0x1001: win_optname = SO_SNDBUF; break;
            case 0x1002: win_optname = SO_RCVBUF; break;
            case 0x1005: win_optname = SO_SNDTIMEO; break;
            case 0x1006: win_optname = SO_RCVTIMEO; break;
            case 0x1007: win_optname = SO_ERROR; break;
            case 0x1008: win_optname = SO_TYPE; break;
            default:
                /* PS4 BSD options: 0x1105 (SO_PROTOCOL/PROTOTYPE), 0x0200 (SO_REUSEPORT),
                 * 0x0800 (SO_NOSIGPIPE). Winsock does not implement them: ignore and succeed. */
                runtime_net_trace("sceNetSetsockopt(fd=%d, level=0x%x, optname=0x%x, len=%u) ignored (PS4 BSD opt)",
                                  s, level, optname, optlen);
                return ORBIS_OK;
        }
    }

    int ret = setsockopt(vs->s, win_level, win_optname, (const char *)optval, (int)optlen);
    if (ret != 0) {
        int err = WSAGetLastError();
        runtime_net_trace("sceNetSetsockopt(fd=%d, level=0x%x, optname=0x%x, len=%u) -> win err %d (treated as OK)",
                          s, level, optname, optlen, err);
        return ORBIS_OK;
    }
    runtime_net_trace("sceNetSetsockopt(fd=%d, level=0x%x, optname=0x%x, len=%u) -> OK",
                      s, level, optname, optlen);
    return ORBIS_OK;
}

int32_t runtime_net_getsockopt(int s, int level, int optname, void *optval, uint32_t *optlen) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    if (optname == ORBIS_NET_SO_NBIO) {
        if (optval && optlen && *optlen >= sizeof(uint32_t)) {
            *(uint32_t *)optval = vs->non_blocking ? 1 : 0;
            *optlen = sizeof(uint32_t);
            return ORBIS_OK;
        }
        return ORBIS_NET_EINVAL;
    }

    int win_level = (level == 0xffff) ? SOL_SOCKET : level;
    int win_optname = optname;
    if (level == 0xffff) {
        switch (optname) {
            case 0x0004: win_optname = SO_REUSEADDR; break;
            case 0x0008: win_optname = SO_KEEPALIVE; break;
            case 0x1001: win_optname = SO_SNDBUF; break;
            case 0x1002: win_optname = SO_RCVBUF; break;
            case 0x1007: win_optname = SO_ERROR; break;
            case 0x1008: win_optname = SO_TYPE; break;
            default:
                if (optval && optlen && *optlen >= 4) {
                    *(uint32_t *)optval = 0;
                    *optlen = 4;
                }
                return ORBIS_OK;
        }
    }

    NativeSockLen len = optlen ? (NativeSockLen)*optlen : 0;
    int ret = getsockopt(vs->s, win_level, win_optname, (char *)optval, &len);
    if (optlen) *optlen = len;
    if (ret != 0) {
        if (optval && optlen && *optlen >= 4) {
            *(uint32_t *)optval = 0;
            *optlen = 4;
        }
        return ORBIS_OK;
    }
    return ORBIS_OK;
}

int32_t runtime_net_getsockname(int s, OrbisNetSockaddr *name, uint32_t *namelen) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;

    struct sockaddr_in sin;
    NativeSockLen sin_len = sizeof(sin);
    int ret = getsockname(vs->s, (struct sockaddr *)&sin, &sin_len);
    if (ret != 0) return -1;

    if (name) {
        OrbisNetSockaddrIn *out = (OrbisNetSockaddrIn *)name;
        out->sin_len = sizeof(OrbisNetSockaddrIn);
        out->sin_family = (uint8_t)sin.sin_family;
        out->sin_port = sin.sin_port;
        memcpy(&out->sin_addr, &sin.sin_addr, sizeof(out->sin_addr));
        memset(out->sin_zero, 0, sizeof(out->sin_zero));
    }
    if (namelen) *namelen = sizeof(OrbisNetSockaddrIn);
    return ORBIS_OK;
}

int32_t runtime_net_shutdown(int s, int how) {
    VSocket *vs = vsocket_get(s);
    if (!vs) return ORBIS_NET_EBADF;
    return shutdown(vs->s, how);
}

/* Epoll emulation */
#define MAX_EPOLLS 16
#define MAX_EPOLL_ENTRIES 64

typedef struct {
    int socket_fd;
    uint32_t events;
    uint64_t ident;
    OrbisNetEpollData data;
} EpollEntry;

typedef struct {
    int in_use;
    EpollEntry entries[MAX_EPOLL_ENTRIES];
    int entry_count;
} EpollContext;

static EpollContext g_epolls[MAX_EPOLLS];

int32_t runtime_net_epoll_create(const char *name, int flags) {
    (void)flags;
    for (int i = 0; i < MAX_EPOLLS; i++) {
        if (!g_epolls[i].in_use) {
            g_epolls[i].in_use = 1;
            g_epolls[i].entry_count = 0;
            runtime_net_trace("sceNetEpollCreate('%s') -> eid=%d", name ? name : "", i + 1);
            return i + 1;
        }
    }
    return -1;
}

int32_t runtime_net_epoll_destroy(int eid) {
    int idx = eid - 1;
    if (idx >= 0 && idx < MAX_EPOLLS) {
        g_epolls[idx].in_use = 0;
        runtime_net_trace("sceNetEpollDestroy(eid=%d)", eid);
    }
    return ORBIS_OK;
}

int32_t runtime_net_epoll_control(int eid, int op, int s, OrbisNetEpollEvent *event) {
    int idx = eid - 1;
    if (idx < 0 || idx >= MAX_EPOLLS || !g_epolls[idx].in_use) return ORBIS_NET_EBADF;
    EpollContext *ep = &g_epolls[idx];

    if (op == ORBIS_NET_EPOLL_CTL_ADD) {
        if (ep->entry_count >= MAX_EPOLL_ENTRIES) return -1;
        ep->entries[ep->entry_count].socket_fd = s;
        ep->entries[ep->entry_count].events = event ? event->events : 0;
        ep->entries[ep->entry_count].ident = event ? event->ident : 0;
        ep->entries[ep->entry_count].data = event ? event->data : (OrbisNetEpollData){0};
        ep->entry_count++;
        runtime_net_trace("sceNetEpollControl(ADD, eid=%d, fd=%d, ev=0x%x)", eid, s, event ? event->events : 0);
        return ORBIS_OK;
    } else if (op == ORBIS_NET_EPOLL_CTL_MOD) {
        for (int i = 0; i < ep->entry_count; i++) {
            if (ep->entries[i].socket_fd == s) {
                ep->entries[i].events = event ? event->events : 0;
                ep->entries[i].ident = event ? event->ident : 0;
                ep->entries[i].data = event ? event->data : (OrbisNetEpollData){0};
                return ORBIS_OK;
            }
        }
        return -1;
    } else if (op == ORBIS_NET_EPOLL_CTL_DEL) {
        for (int i = 0; i < ep->entry_count; i++) {
            if (ep->entries[i].socket_fd == s) {
                ep->entries[i] = ep->entries[ep->entry_count - 1];
                ep->entry_count--;
                return ORBIS_OK;
            }
        }
        return -1;
    }
    return ORBIS_NET_EINVAL;
}

int32_t runtime_net_epoll_wait(int eid, OrbisNetEpollEvent *events, int maxevents, int timeout_usec) {
    int idx = eid - 1;
    if (idx < 0 || idx >= MAX_EPOLLS || !g_epolls[idx].in_use) return ORBIS_NET_EBADF;
    EpollContext *ep = &g_epolls[idx];
    if (ep->entry_count == 0 || maxevents <= 0) return 0;

    fd_set read_fds, write_fds, except_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);

    SOCKET max_sock = 0;
    for (int i = 0; i < ep->entry_count; i++) {
        VSocket *vs = vsocket_get(ep->entries[i].socket_fd);
        if (vs) {
            if (ep->entries[i].events & ORBIS_NET_EPOLLIN) FD_SET(vs->s, &read_fds);
            if (ep->entries[i].events & (ORBIS_NET_EPOLLOUT | 0x00000004)) FD_SET(vs->s, &write_fds);
            FD_SET(vs->s, &except_fds);
            if (vs->s > max_sock) max_sock = vs->s;
        }
    }

    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout_usec >= 0) {
        tv.tv_sec = timeout_usec / 1000000;
        tv.tv_usec = timeout_usec % 1000000;
        ptv = &tv;
    }

    int ret = select((int)(max_sock + 1), &read_fds, &write_fds, &except_fds, ptv);
    if (ret <= 0) return 0;

    int out_count = 0;
    for (int i = 0; i < ep->entry_count && out_count < maxevents; i++) {
        VSocket *vs = vsocket_get(ep->entries[i].socket_fd);
        if (!vs) continue;

        uint32_t revents = 0;
        if (FD_ISSET(vs->s, &read_fds)) revents |= ORBIS_NET_EPOLLIN;
        if (FD_ISSET(vs->s, &write_fds)) {
            revents |= ORBIS_NET_EPOLLOUT;
            if (ep->entries[i].events & 0x00000004) revents |= 0x00000004;
        }
        if (FD_ISSET(vs->s, &except_fds)) revents |= ORBIS_NET_EPOLLERR;

        if (revents) {
            events[out_count].events = revents;
            events[out_count].pad = 0;
            events[out_count].ident = ep->entries[i].ident ? ep->entries[i].ident : (uint64_t)ep->entries[i].socket_fd;
            events[out_count].data = ep->entries[i].data;
            out_count++;
        }
    }

    if (out_count > 0) {
        runtime_net_trace("sceNetEpollWait(eid=%d) -> %d event(s) (ev=0x%x)", eid, out_count, events[0].events);
    }

    return out_count;
}

int32_t runtime_net_epoll_abort(int eid, int flags) {
    (void)eid; (void)flags;
    return ORBIS_OK;
}

/* Resolver */
int32_t runtime_net_resolver_create(const char *name, int pool, int flags) {
    (void)name; (void)pool; (void)flags;
    static int next_rid = 1;
    return next_rid++;
}

int32_t runtime_net_resolver_destroy(int rid) {
    (void)rid;
    return ORBIS_OK;
}

int32_t runtime_net_resolver_start_ntoa(int rid, const char *hostname, struct in_addr *addr, int timeout_usec, int retry, int flags) {
    (void)rid; (void)timeout_usec; (void)retry; (void)flags;
    if (!hostname || !addr) return ORBIS_NET_EINVAL;

    runtime_net_init();

    uint16_t port_override = 0;
    int is_http = 0;
    const char *target = net_overrides_lookup(hostname, &port_override, &is_http);

    if (target) {
        runtime_net_trace("sceNetResolverStartNtoa('%s') -> '%s' (OVERRIDDEN)", hostname, target);
        addr->s_addr = inet_addr(target);
        if (addr->s_addr == INADDR_NONE) {
            struct hostent *he = gethostbyname(target);
            if (he && he->h_addr_list[0]) {
                memcpy(addr, he->h_addr_list[0], sizeof(struct in_addr));
            }
        }
        return ORBIS_OK;
    }

    if (!net_overrides_is_allowed(hostname)) {
        runtime_net_trace("sceNetResolverStartNtoa('%s') -> BLOCKED by whitelist", hostname);
        g_net_errno = 65; /* EHOSTUNREACH */
        return ORBIS_NET_EHOSTUNREACH;
    }

    struct hostent *he = gethostbyname(hostname);
    if (he && he->h_addr_list[0]) {
        memcpy(addr, he->h_addr_list[0], sizeof(struct in_addr));
        char buf[32];
        inet_ntop(AF_INET, addr, buf, sizeof(buf));
        runtime_net_trace("sceNetResolverStartNtoa('%s') -> %s (DNS)", hostname, buf);
        return ORBIS_OK;
    }

    runtime_net_trace("sceNetResolverStartNtoa('%s') -> host not found", hostname);
    return ORBIS_NET_EHOSTUNREACH;
}

int32_t runtime_net_resolver_start_aton(int rid, struct in_addr addr, char *hostname, size_t hostlen, int timeout_usec, int retry, int flags) {
    (void)rid; (void)timeout_usec; (void)retry; (void)flags;
    if (!hostname || hostlen == 0) return ORBIS_NET_EINVAL;
    const char *str = inet_ntop(AF_INET, &addr, hostname, (socklen_t)hostlen);
    return str ? ORBIS_OK : -1;
}

int32_t runtime_net_resolver_abort(int rid, int flags) {
    (void)rid; (void)flags;
    return ORBIS_OK;
}

/* NetCtl implementation */
static int g_netctl_initialized = 0;
typedef ABI void (*NetCtlCallback)(int event, void *arg);

static struct {
    NetCtlCallback cb;
    void *arg;
    int active;
    int notified;
} netctl_cbs[8];

int32_t runtime_netctl_init(void) {
    g_netctl_initialized = 1;
    runtime_net_trace("sceNetCtlInit (online=%d)", runtime_net_is_online());
    return ORBIS_OK;
}

int32_t runtime_netctl_term(void) {
    g_netctl_initialized = 0;
    runtime_net_trace("sceNetCtlTerm");
    return ORBIS_OK;
}

int32_t runtime_netctl_state(int32_t *state) {
    if (!state) return NET_CTL_INVALID_ADDR;
    if (!runtime_net_is_online()) {
        *state = 0; /* DISCONNECTED */
        runtime_net_trace("sceNetCtlGetState -> DISCONNECTED (0)");
        return ORBIS_OK;
    }
    *state = 3; /* IPOBTAINED */
    runtime_net_trace("sceNetCtlGetState -> IPOBTAINED (3)");
    return ORBIS_OK;
}

int32_t runtime_netctl_info(int code, void *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    if (!runtime_net_is_online()) return NET_CTL_NOT_CONNECTED;

    if (code != 4) {
        runtime_net_trace("sceNetCtlGetInfo(code=%d)", code);
    }
    switch (code) {
        case 1:  /* DEVICE: wired */
            *(uint32_t *)info = 0;
            break;
        case 2:  /* ETHER_ADDR */
            memset(info, 0xAA, 6);
            break;
        case 3:  /* MTU */
            *(uint32_t *)info = 1500;
            break;
        case 4:  /* LINK: connected */
            *(uint32_t *)info = 1;
            break;
        case 11: /* IP_CONFIG: DHCP */
            *(uint32_t *)info = 1;
            break;
        case 14: /* IP_ADDRESS */
            strncpy((char *)info, local_ipv4(), 16);
            break;
        case 15: /* NETMASK */
            strncpy((char *)info, "255.255.255.0", 16);
            break;
        case 16: /* DEFAULT_ROUTE */
            strncpy((char *)info, "127.0.0.1", 16);
            break;
        case 17: /* PRIMARY_DNS */
            strncpy((char *)info, "1.1.1.1", 16);
            break;
        case 18: /* SECONDARY_DNS */
            strncpy((char *)info, "8.8.8.8", 16);
            break;
        case 19: /* HTTP_PROXY_CONFIG: none */
            *(uint32_t *)info = 0;
            break;
        default:
            memset(info, 0, 16);
            break;
    }
    return ORBIS_OK;
}

int32_t runtime_netctl_nat(OrbisNetCtlNatInfo *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    if (!runtime_net_is_online()) {
        info->stun_status = 0;
        info->nat_type = 3; /* Strict offline */
        info->mapped_addr = 0;
        runtime_net_trace("sceNetCtlGetNatInfo -> Type 3 (offline)");
        return ORBIS_OK;
    }
    info->stun_status = 0; /* OK */
    info->nat_type = 2;    /* Type 2 - Open / Moderate */
    info->mapped_addr = inet_addr("127.0.0.1");
    runtime_net_trace("sceNetCtlGetNatInfo -> Type 2 (Open, mapped=127.0.0.1)");
    return ORBIS_OK;
}

int32_t runtime_netctl_register(void *cb, void *arg, int32_t *cid) {
    if (!cid) return NET_CTL_INVALID_ADDR;
    for (int i = 0; i < 8; i++) {
        if (!netctl_cbs[i].active) {
            netctl_cbs[i].cb = (NetCtlCallback)cb;
            netctl_cbs[i].arg = arg;
            netctl_cbs[i].active = 1;
            netctl_cbs[i].notified = 0;
            *cid = i + 1;
            runtime_net_trace("sceNetCtlRegisterCallback -> slot %d", i);
            return ORBIS_OK;
        }
    }
    *cid = 1;
    return ORBIS_OK;
}

int32_t runtime_netctl_unregister(int32_t cid) {
    int idx = cid - 1;
    if (idx >= 0 && idx < 8) {
        netctl_cbs[idx].active = 0;
        netctl_cbs[idx].notified = 0;
        runtime_net_trace("sceNetCtlUnregisterCallback -> slot %d", idx);
    }
    return ORBIS_OK;
}

int32_t runtime_netctl_check(void) {
    if (runtime_net_is_online()) {
        for (int i = 0; i < 8; i++) {
            if (netctl_cbs[i].active && !netctl_cbs[i].notified && netctl_cbs[i].cb) {
                netctl_cbs[i].notified = 1;
                runtime_net_trace("sceNetCtlCheckCallback -> notifying netctl cb slot %d: IPOBTAINED (3)", i);
                netctl_cbs[i].cb(3 /* IPOBTAINED */, netctl_cbs[i].arg);
            }
        }
    }
    return ORBIS_OK;
}
