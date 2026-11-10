/* The EOS bridge (src/runtime_net_bridge.c) under the game's sockets: everything for the server
 * goes to the bridge's local ports, the bridge's datagrams come back as the server's, and a peer
 * on the internet gets only the tunnelled copy (the server knows this PC by its virtual address).
 * A fake bridge here: a UDP socket and a TCP listener on 127.0.0.1. */
#include "../src/runtime_net.c"
#include <assert.h>
#include <fcntl.h>

static int bind_udp(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (struct sockaddr *)&a, sizeof a));
    socklen_t n = sizeof a;
    assert(!getsockname(fd, (struct sockaddr *)&a, &n));
    *port = ntohs(a.sin_port);
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static int listen_tcp(uint16_t *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (struct sockaddr *)&a, sizeof a) && !listen(fd, 4));
    socklen_t n = sizeof a;
    assert(!getsockname(fd, (struct sockaddr *)&a, &n));
    *port = ntohs(a.sin_port);
    return fd;
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

/* The next datagram at the fake bridge: its length (and the sender's port). */
static int bridge_recv(int fd, uint8_t *buf, size_t cap, uint16_t *from_port) {
    struct sockaddr_in from;
    socklen_t n = sizeof from;
    const int r = (int)recvfrom(fd, buf, cap, 0, (struct sockaddr *)&from, &n);
    if (r > 0 && from_port) *from_port = ntohs(from.sin_port);
    return r;
}

int main(void) {
    uint16_t udp_port, http_port;
    const int bridge_udp = bind_udp(&udp_port);
    const int bridge_http = listen_tcp(&http_port);
    char map[128];
    snprintf(map, sizeof map, "%u,3074:1,8080:%u", udp_port, http_port);
    setenv("BB_ONLINE", "1", 1);
    setenv("BB_ONLINE_SERVER", "server.invalid", 1); /* this network does not resolve it */
    setenv("BB_ONLINE_SERVER_IP", "203.0.113.5", 1);
    setenv("BB_ONLINE_BRIDGE", map, 1);
    setenv("BB_ONLINE_PING", "0", 1);

    /* The server's address comes from the bridge, by name too. */
    uint32_t ip = 0, expected;
    inet_pton(AF_INET, "203.0.113.5", &expected);
    assert(net_bridge_active());
    assert(net_server_ipv4(&ip) && ip == expected);
    ip = 0;
    assert(net_resolve_ipv4("SERVER.invalid", &ip) && ip == expected);
    uint16_t port = 8080;
    assert(net_bridge_route_tcp(&ip, &port) && ip == htonl(INADDR_LOOPBACK) && port == http_port);
    ip = expected;
    port = 9999;
    assert(!net_bridge_route_tcp(&ip, &port) && ip == expected && port == 9999); /* not one of its ports */
    struct sockaddr_in server;
    assert(tunnel_server(&server) && server.sin_addr.s_addr == expected && ntohs(server.sin_port) == 3074);
    assert(!server_is_this_machine());

    /* The game's TCP connection to the server's HTTP port lands on the bridge's. */
    const int tcp = socket(AF_INET, SOCK_STREAM, 0);
    g_sockets[1] = (VSocket){.s = tcp, .in_use = 1, .is_stream = 1};
    OrbisNetSockaddrIn http = orbis("203.0.113.5", 8080);
    assert(runtime_net_connect(2, (const OrbisNetSockaddr *)&http, sizeof http) == 0);
    const int accepted = accept(bridge_http, NULL, NULL);
    assert(accepted >= 0);

    /* The game socket (UDP 3074): its keepalive and NAT probes go to the bridge. */
    uint16_t game_port;
    const int game = bind_udp(&game_port);
    g_sockets[0] = (VSocket){.s = game, .in_use = 1, .tunnel = 1};
    uint8_t buf[4096];
    uint16_t from_port = 0;
    tunnel_keepalive(&g_sockets[0], &server);
    int n = bridge_recv(bridge_udp, buf, sizeof buf, &from_port);
    /* A plain keepalive: no relay pass requested, no relay is reachable through the bridge. */
    assert(n == TUN_KEEPALIVE_LEN && !memcmp(buf, TUN_KEEPALIVE, TUN_KEEPALIVE_LEN) && from_port == game_port);
    OrbisNetSockaddrIn nat = orbis("203.0.113.5", 3074);
    const uint8_t probe[3] = {0x1e, 0x03, 0x00};
    assert(runtime_net_sendto(1, probe, 3, 0, (const OrbisNetSockaddr *)&nat, sizeof nat) == 3);
    n = bridge_recv(bridge_udp, buf, sizeof buf, NULL);
    assert(n == 3 && !memcmp(buf, probe, 3));

    /* A peer on the internet: nothing goes to it directly, its copy goes to the bridge. */
    OrbisNetSockaddrIn peer = orbis("198.51.100.7", 3074);
    assert(runtime_net_sendto(1, "race", 4, 0, (const OrbisNetSockaddr *)&peer, sizeof peer) == 4);
    n = bridge_recv(bridge_udp, buf, sizeof buf, NULL);
    assert(n == TUN_HEADER_LEN + 4 && !memcmp(buf, "RYUTN1", 6) && !memcmp(buf + TUN_HEADER_LEN, "race", 4));
    uint32_t dst_ip;
    memcpy(&dst_ip, buf + 7, 4);
    char text[16];
    inet_ntop(AF_INET, &dst_ip, text, sizeof text);
    assert(!strcmp(text, "198.51.100.7") && buf[11] == 0x0c && buf[12] == 0x02);

    /* The bridge's datagrams are the server's: a NAT reply, then a peer's tunnelled packet. */
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(game_port)};
    const uint8_t reply[9] = {31, 2, 0, 198, 18, 0, 2, 0x02, 0x0c};
    assert(sendto(bridge_udp, reply, sizeof reply, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof reply);
    OrbisNetSockaddrIn from;
    uint32_t from_len = sizeof from;
    n = runtime_net_recvfrom(1, buf, sizeof buf, 0, (OrbisNetSockaddr *)&from, &from_len);
    assert(n == (int)sizeof reply && !memcmp(buf, reply, sizeof reply));
    assert(!memcmp(&from.sin_addr, &expected, 4) && ntohs(from.sin_port) == 3074);
    uint8_t wrapped[64];
    const size_t w = tun_wrap(wrapped, sizeof wrapped, 0, peer.sin_addr.s_addr, 3074, "back", 4);
    assert(sendto(bridge_udp, wrapped, w, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)w);
    n = runtime_net_recvfrom(1, buf, sizeof buf, 0, (OrbisNetSockaddr *)&from, &from_len);
    assert(n == 4 && !memcmp(buf, "back", 4));
    inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
    assert(!strcmp(text, "198.51.100.7") && ntohs(from.sin_port) == 3074);

    /* A LAN peer still gets its packets directly. */
    uint16_t lan_port;
    const int lan = bind_udp(&lan_port);
    OrbisNetSockaddrIn lan_peer = orbis("127.0.0.1", lan_port);
    assert(runtime_net_sendto(1, "lan", 3, 0, (const OrbisNetSockaddr *)&lan_peer, sizeof lan_peer) == 3);
    n = bridge_recv(lan, buf, sizeof buf, NULL);
    assert(n == 3 && !memcmp(buf, "lan", 3));

    close(accepted);
    close(tcp);
    close(game);
    close(lan);
    puts("EOS bridge: the server's TCP and UDP through the bridge, peers through the tunnel, LAN direct: passed");

    /* A wrong value leaves the bridge off (a fresh process: it is read once). */
    return 0;
}
