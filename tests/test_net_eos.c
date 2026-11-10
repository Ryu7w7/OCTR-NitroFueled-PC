/* Races through EOS P2P (src/runtime_net_eos.h) under the game's socket: a fake server (its
 * tunnel) and a fake ryunet-bridge on 127.0.0.1. A peer on the internet whose tunnelled packets
 * say it has EOS gets this PC's id; once its own id is known, the game's datagrams for it go to
 * the bridge too, and once its datagrams come back through EOS the tunnelled copies stop. */
#include "../src/runtime_net.c"
#include <assert.h>
#include <fcntl.h>

static int bind_udp(const char *ip, uint16_t *port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(*port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    assert(!bind(fd, (struct sockaddr *)&a, sizeof a));
    socklen_t n = sizeof a;
    assert(!getsockname(fd, (struct sockaddr *)&a, &n));
    *port = ntohs(a.sin_port);
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static int recv_from(int fd, uint8_t *buf, size_t cap) { return (int)recv(fd, buf, cap, 0); }

/* Nothing more waiting (a short wait). */
static int quiet(int fd) {
    struct timeval tv = {0, 150000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t b[64];
    const int n = (int)recv(fd, b, sizeof b, 0);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return n < 0;
}

static OrbisNetSockaddrIn orbis(const char *ip, uint16_t port) {
    OrbisNetSockaddrIn a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static size_t with_id(uint8_t *out, char kind, const char *id, const char *payload) {
    size_t n = 0;
    memcpy(out, EOSP_MAGIC, EOSP_MAGIC_LEN);
    n = EOSP_MAGIC_LEN;
    out[n++] = (uint8_t)kind;
    out[n++] = (uint8_t)strlen(id);
    memcpy(out + n, id, strlen(id));
    n += strlen(id);
    if (payload) {
        memcpy(out + n, payload, strlen(payload));
        n += strlen(payload);
    }
    return n;
}

/* The game reads one datagram (its socket is non-blocking): its length, from where. */
static int game_read(uint8_t *buf, size_t cap, char *from_ip, uint16_t *from_port) {
    for (int tries = 0; tries < 100; tries++) {
        OrbisNetSockaddrIn from;
        uint32_t from_len = sizeof from;
        const int n = runtime_net_recvfrom(1, buf, cap, 0, (OrbisNetSockaddr *)&from, &from_len);
        if (n >= 0) {
            inet_ntop(AF_INET, &from.sin_addr, from_ip, 16);
            *from_port = ntohs(from.sin_port);
            return n;
        }
        struct timespec d = {0, 10000000};
        nanosleep(&d, NULL);
    }
    return -1;
}

int main(void) {
    uint16_t server_port = 0, bridge_port = 0, game_port = 0;
    const int server = bind_udp("127.0.0.2", &server_port);
    const int bridge = bind_udp("127.0.0.1", &bridge_port);
    char text[16];
    setenv("BB_ONLINE", "1", 1);
    setenv("BB_ONLINE_SERVER", "127.0.0.2", 1);
    snprintf(text, sizeof text, "%u", server_port);
    setenv("BB_ONLINE_NAT_PORT", text, 1);
    snprintf(text, sizeof text, "%u", bridge_port);
    setenv("BB_ONLINE_EOS", text, 1);
    setenv("BB_ONLINE_PING", "0", 1);
    setenv("BB_ONLINE_RELAYS", "0", 1);

    const int game = bind_udp("0.0.0.0", &game_port);
    fcntl(game, F_SETFL, fcntl(game, F_GETFL, 0) | O_NONBLOCK);
    g_sockets[0] = (VSocket){.s = game, .in_use = 1, .tunnel = 1};
    struct sockaddr_in srv;
    assert(tunnel_server(&srv));
    const struct sockaddr_in to_game = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(game_port)};
    uint8_t buf[4096], m[512];
    char ip[16];
    uint16_t port;

    /* The keepalive asks the bridge who this PC is; its answer is not the game's. */
    tunnel_keepalive(&g_sockets[0], &srv);
    int n = recv_from(bridge, buf, sizeof buf);
    assert(n == EOSP_MAGIC_LEN + 1 && !memcmp(buf, EOSP_MAGIC, EOSP_MAGIC_LEN) && buf[EOSP_MAGIC_LEN] == 'Q');
    n = recv_from(server, buf, sizeof buf); /* the keepalive itself */
    assert(n == TUN_KEEPALIVE_LEN);
    size_t len = with_id(m, 'I', "selfid0001", NULL);
    sendto(bridge, m, len, 0, (const struct sockaddr *)&to_game, sizeof to_game);
    assert(game_read(buf, sizeof buf, ip, &port) < 0 || 0); /* taken, nothing for the game */
    char self_id[EOSP_ID_MAX];
    assert(eosp_self(self_id) && !strcmp(self_id, "selfid0001"));

    /* The game sends to a peer on the internet: direct and tunnelled, flagged as EOS-capable. */
    OrbisNetSockaddrIn peer = orbis("198.51.100.7", 3074);
    assert(runtime_net_sendto(1, "lap1", 4, 0, (const OrbisNetSockaddr *)&peer, sizeof peer) == 4);
    n = recv_from(server, buf, sizeof buf);
    assert(n == TUN_HEADER_LEN + 4 && !memcmp(buf, "RYUTN1", 6) && (buf[6] & TUN_FLAG_EOS) && !memcmp(buf + 13, "lap1", 4));
    assert(quiet(bridge)); /* its id is not known: nothing through EOS yet */

    /* Its tunnelled answer says it has EOS: the next send carries this PC's announce. */
    const struct sockaddr_in from_peer_wrap = to_game;
    uint8_t wrapped[256];
    size_t w = tun_wrap(wrapped, sizeof wrapped, TUN_FLAG_EOS, peer.sin_addr.s_addr, 3074, "hi", 2);
    sendto(server, wrapped, w, 0, (const struct sockaddr *)&from_peer_wrap, sizeof from_peer_wrap);
    n = game_read(buf, sizeof buf, ip, &port);
    assert(n == 2 && !memcmp(buf, "hi", 2) && !strcmp(ip, "198.51.100.7") && port == 3074);
    assert(runtime_net_sendto(1, "lap2", 4, 0, (const OrbisNetSockaddr *)&peer, sizeof peer) == 4);
    int saw_announce = 0, saw_lap2 = 0;
    for (int i = 0; i < 2; i++) {
        n = recv_from(server, buf, sizeof buf);
        assert(n > TUN_HEADER_LEN);
        if (!memcmp(buf + 13, EOSP_MAGIC, EOSP_MAGIC_LEN) && buf[13 + EOSP_MAGIC_LEN] == 'A' &&
            !memcmp(buf + 13 + EOSP_MAGIC_LEN + 2, "selfid0001", 10))
            saw_announce = 1;
        if (n == TUN_HEADER_LEN + 4 && !memcmp(buf + 13, "lap2", 4)) saw_lap2 = 1;
    }
    assert(saw_announce && saw_lap2);

    /* The peer's announce (through the tunnel): the bridge is told it may connect; the game sees nothing. */
    len = with_id(m, 'A', "peerid0002", NULL);
    w = tun_wrap(wrapped, sizeof wrapped, TUN_FLAG_EOS, peer.sin_addr.s_addr, 3074, m, len);
    sendto(server, wrapped, w, 0, (const struct sockaddr *)&from_peer_wrap, sizeof from_peer_wrap);
    assert(game_read(buf, sizeof buf, ip, &port) < 0);
    n = recv_from(bridge, buf, sizeof buf);
    assert(n == (int)with_id(m, 'K', "peerid0002", NULL) && !memcmp(buf, m, (size_t)n));

    /* Its id known: the game's datagrams go to the bridge too, and through the tunnel until EOS answers. */
    assert(runtime_net_sendto(1, "lap3", 4, 0, (const OrbisNetSockaddr *)&peer, sizeof peer) == 4);
    n = recv_from(bridge, buf, sizeof buf);
    len = with_id(m, 'S', "peerid0002", "lap3");
    assert(n == (int)len && !memcmp(buf, m, len));
    n = recv_from(server, buf, sizeof buf);
    assert(n == TUN_HEADER_LEN + 4 && !memcmp(buf + 13, "lap3", 4));

    /* Its datagram through EOS: the game gets it as the peer's... */
    len = with_id(m, 'R', "peerid0002", "back");
    sendto(bridge, m, len, 0, (const struct sockaddr *)&to_game, sizeof to_game);
    n = game_read(buf, sizeof buf, ip, &port);
    assert(n == 4 && !memcmp(buf, "back", 4) && !strcmp(ip, "198.51.100.7") && port == 3074);
    /* ...and from then on its copies go through EOS only, not the server's tunnel. */
    assert(runtime_net_sendto(1, "lap4", 4, 0, (const OrbisNetSockaddr *)&peer, sizeof peer) == 4);
    n = recv_from(bridge, buf, sizeof buf);
    len = with_id(m, 'S', "peerid0002", "lap4");
    assert(n == (int)len && !memcmp(buf, m, len));
    assert(quiet(server));

    /* EOS goes quiet: the tunnel comes back by itself. */
    struct timespec wait = {EOSP_FRESH_MS / 1000 + 1, 0};
    nanosleep(&wait, NULL);
    assert(runtime_net_sendto(1, "lap5", 4, 0, (const OrbisNetSockaddr *)&peer, sizeof peer) == 4);
    n = recv_from(bridge, buf, sizeof buf);
    assert(n > 0); /* still offered to EOS */
    int back_in_tunnel = 0;
    for (int i = 0; i < 3 && !back_in_tunnel; i++) {
        n = recv_from(server, buf, sizeof buf);
        if (n == TUN_HEADER_LEN + 4 && !memcmp(buf + 13, "lap5", 4)) back_in_tunnel = 1;
    }
    assert(back_in_tunnel);

    /* A datagram through EOS from an id the game never learned is dropped. */
    len = with_id(m, 'R', "strangerid9", "evil");
    sendto(bridge, m, len, 0, (const struct sockaddr *)&to_game, sizeof to_game);
    assert(game_read(buf, sizeof buf, ip, &port) < 0);

    puts("EOS P2P: capability, ids, datagrams through EOS, the tunnel as fallback, strangers dropped: passed");
    return 0;
}
