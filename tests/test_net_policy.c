/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real socket policy without a game, PSN backend or remote service.
 * Include the implementation to inspect the tunnel/UPnP decisions as well as the
 * public socket behavior. Each invocation is a fresh process for cached settings. */
#include "../src/runtime_net.c"
#include <assert.h>

static void environment(const char *key, const char *value) {
#ifdef _WIN32
    assert(_putenv_s(key, value ? value : "") == 0);
#else
    assert((value ? setenv(key, value, 1) : unsetenv(key)) == 0);
#endif
}

static OrbisNetSockaddrIn guest_address(const char *ip, uint16_t port) {
    OrbisNetSockaddrIn address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(ip);
    address.sin_port = htons(port);
    return address;
}

static OrbisNetSockaddrIn bound_address(int fd) {
    OrbisNetSockaddrIn address = {0};
    uint32_t length = sizeof(address);
    assert(runtime_net_getsockname(fd, (OrbisNetSockaddr *)&address, &length) == 0);
    assert(length == sizeof(address));
    return address;
}

static int guest_socket(void) {
    int fd = runtime_net_socket("policy-test", AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(fd > 0);
    return fd;
}

int main(int argc, char **argv) {
    assert(argc == 4);
    const int preserve = strcmp(argv[2], "1") == 0;
    const int tunnel = strcmp(argv[3], "1") == 0;
    environment("BB_PSN_BACKEND", strcmp(argv[1], "unset") == 0 ? NULL :
                strcmp(argv[1], "empty") == 0 ? "" : "/unused/social-backend.so");
    environment("BB_NET_PRESERVE_ENDPOINTS", strcmp(argv[2], "unset") == 0 ? NULL : argv[2]);
    environment("BB_ONLINE_TUNNEL", argv[3]);
    environment("BB_ONLINE_SERVER", "127.0.0.1");
    environment("BB_ONLINE_NAT_PORT", "43210");
    environment("BB_ONLINE_PORT", NULL);
    environment("BB_UPNP_PORT", "4321");
    environment("BB_NET_TRACE", "0");

    /* Identity selection cannot override the independent offline/online switch. */
    environment("BB_ONLINE", "0");
    assert(runtime_net_is_online() == 0);
    int32_t state = -1;
    assert(runtime_netctl_state(&state) == 0 && state == 0);
    assert(runtime_net_socket("offline", AF_INET, SOCK_DGRAM, IPPROTO_UDP) < 0);
    environment("BB_ONLINE", "1");
    assert(runtime_net_is_online() == 1);
    assert(runtime_netctl_state(&state) == 0 && state == 3);

    int first = guest_socket();
    OrbisNetSockaddrIn requested = guest_address("127.0.0.2", 0);
    assert(runtime_net_bind(first, (OrbisNetSockaddr *)&requested, sizeof(requested)) == 0);
    OrbisNetSockaddrIn actual = bound_address(first);
    assert(actual.sin_addr.s_addr == (preserve ? inet_addr("127.0.0.2") : INADDR_ANY));
    assert(actual.sin_port != 0);
    assert(runtime_net_socket_close(first) == 0);

    /* Reserve an OS-assigned loopback port, avoiding a fixed test/service port. */
    SOCKET blocker = INVALID_SOCKET;
    struct sockaddr_in reserved = {0};
    for (int attempt = 0; attempt < 20; ++attempt) {
        blocker = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(blocker != INVALID_SOCKET);
#ifdef _WIN32
        const int exclusive = 1;
        assert(setsockopt(blocker, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                          (const char *)&exclusive, sizeof(exclusive)) == 0);
#endif
        reserved.sin_family = AF_INET;
        reserved.sin_addr.s_addr = INADDR_ANY;
        reserved.sin_port = 0;
        assert(bind(blocker, (struct sockaddr *)&reserved, sizeof(reserved)) == 0);
        socklen_t length = sizeof(reserved);
        assert(getsockname(blocker, (struct sockaddr *)&reserved, &length) == 0);
        if (ntohs(reserved.sin_port) < 65525) break;
        closesocket(blocker);
        blocker = INVALID_SOCKET;
    }
    assert(blocker != INVALID_SOCKET && ntohs(reserved.sin_port) < 65525);
    char port[8];
    snprintf(port, sizeof(port), "%u", ntohs(reserved.sin_port));
    environment("BB_ONLINE_PORT", port);
    int overridden = guest_socket();
    requested = guest_address("127.0.0.1", 0);
    assert(runtime_net_bind(overridden, (OrbisNetSockaddr *)&requested, sizeof(requested)) == 0);
    actual = bound_address(overridden);
    if (preserve) {
        assert(actual.sin_port != reserved.sin_port);
        assert(actual.sin_addr.s_addr == inet_addr("127.0.0.1"));
    } else {
        assert(ntohs(actual.sin_port) > ntohs(reserved.sin_port));
        assert(ntohs(actual.sin_port) <= ntohs(reserved.sin_port) + 10);
    }
    assert(runtime_net_socket_close(overridden) == 0);

    environment("BB_ONLINE_PORT", NULL);
    int occupied = guest_socket();
    requested = guest_address("127.0.0.1", ntohs(reserved.sin_port));
    int32_t result = runtime_net_bind(occupied, (OrbisNetSockaddr *)&requested, sizeof(requested));
    if (preserve) {
        assert(result != 0); /* Never silently move the requested endpoint. */
    } else {
        assert(result == 0); /* Existing automatic retry remains the default. */
        assert(bound_address(occupied).sin_port != reserved.sin_port);
    }
    assert(runtime_net_socket_close(occupied) == 0);
    closesocket(blocker);

    /* Endpoint preservation is separate from both the tunnel and PSN selection. */
    struct sockaddr_in server;
    assert(tunnel_server(&server) == tunnel);
    if (tunnel) {
        assert(server.sin_addr.s_addr == inet_addr("127.0.0.1"));
        assert(ntohs(server.sin_port) == 43210);
    }
    assert(upnp_external_port() == (preserve ? 3074 : 4321));
    const char soap[] = "<NewExternalPort>3074</NewExternalPort>";
    char *rewritten = upnp_rewrite(soap, sizeof(soap));
    assert(rewritten != NULL);
    assert(strcmp(rewritten, preserve ? soap :
                  "<NewExternalPort>4321</NewExternalPort>") == 0);
    free(rewritten);
    assert(runtime_net_term() == 0);
    printf("Network policy: backend=%s preserve=%s tunnel=%s passed\n", argv[1], argv[2], argv[3]);
    return 0;
}
