/* Exercise private Itemless's peer gate with co-located NAT and game traffic. */
#include "../src/runtime_net.c"
#include <assert.h>

int main(void) {
    setenv("BB_ONLINE", "1", 1);
    setenv("BB_ONLINE_SERVER", "127.0.0.1", 1);
    setenv("BB_ONLINE_NAT_PORT", "3074", 1);
    setenv("BB_ONLINE_TUNNEL", "1", 1);
    VSocket game = {.tunnel = 1}, other = {0};
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(3074)};
    inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
    note_game_peer(&game, &peer);
    assert(!runtime_net_private_lobby_active()); /* NAT traffic is not a player. */
    peer.sin_port = htons(3076);
    note_game_peer(&other, &peer);
    assert(!runtime_net_private_lobby_active());
    note_game_peer(&game, &peer);
    assert(runtime_net_private_lobby_active()); /* Game and NAT traffic share an IP, but use different ports. */
    g_last_game_peer_ms = 0;
    peer.sin_port = htons(3077);
    note_game_peer(&game, &peer);
    assert(runtime_net_private_lobby_active()); /* Guest shares it too. */
    g_last_game_peer_ms = 0;
    peer.sin_port = htons(3074);
    inet_pton(AF_INET, "192.168.1.20", &peer.sin_addr);
    note_game_peer(&game, &peer);
    assert(runtime_net_private_lobby_active()); /* Remote peer on default port. */
    g_last_game_peer_ms = now_ms() - 5001;
    assert(!runtime_net_private_lobby_active());
    puts("private peer gate: ok");
}
