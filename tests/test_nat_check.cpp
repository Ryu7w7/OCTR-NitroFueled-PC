// NAT type test of the launcher (launcher/windows/nat_check.cpp): reply parsing and the
// classification, without the network.
#include "nat_check.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace NatCheck;

static std::uint32_t Ip(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    const std::uint8_t bytes[4] = {a, b, c, d};
    std::uint32_t ip;
    std::memcpy(&ip, bytes, 4);  // network order, as sockaddr_in and the replies hold it
    return ip;
}

int main() {
    // RyuNET: 31, 2, 0, 203.0.113.7, port 3074 little endian.
    {
        const std::uint8_t reply[9] = {31, 2, 0, 203, 0, 113, 7, 0x02, 0x0C};
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        assert(ParseServerReply(reply, sizeof(reply), ip, port));
        assert(ip == Ip(203, 0, 113, 7) && port == 3074);
        assert(!ParseServerReply(reply, 8, ip, port));
        const std::uint8_t other[9] = {21, 2, 0, 1, 2, 3, 4, 5, 6};
        assert(!ParseServerReply(other, sizeof(other), ip, port));
    }
    // RyuNET: the NAT test reply, 21, 2, 0, client 203.0.113.7:3074, server 198.51.100.1:3074.
    {
        const std::uint8_t reply[15] = {21, 2, 0, 203, 0, 113, 7, 0x02, 0x0C, 198, 51, 100, 1, 0x02, 0x0C};
        assert(IsServerTestReply(reply, sizeof(reply)));
        assert(!IsServerTestReply(reply, 9));
        const std::uint8_t address[15] = {31, 2, 0, 203, 0, 113, 7, 0x02, 0x0C};
        assert(!IsServerTestReply(address, sizeof(address)));
    }
    // STUN: a Binding request, and a success response with XOR-MAPPED-ADDRESS 198.51.100.20:40000.
    {
        const std::uint8_t tx[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        std::uint8_t request[20];
        StunRequest(request, tx);
        assert(request[0] == 0 && request[1] == 1 && request[4] == 0x21 && std::memcmp(request + 8, tx, 12) == 0);

        std::uint8_t reply[32] = {0x01, 0x01, 0x00, 12, 0x21, 0x12, 0xA4, 0x42};
        std::memcpy(reply + 8, tx, 12);
        const std::uint16_t xport = 40000 ^ 0x2112;
        const std::uint32_t xaddr = ((198u << 24) | (51u << 16) | (100u << 8) | 20u) ^ 0x2112A442u;
        const std::uint8_t attr[12] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, std::uint8_t(xport >> 8), std::uint8_t(xport),
                                       std::uint8_t(xaddr >> 24), std::uint8_t(xaddr >> 16), std::uint8_t(xaddr >> 8),
                                       std::uint8_t(xaddr)};
        std::memcpy(reply + 20, attr, 12);
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        assert(ParseStunReply(reply, sizeof(reply), tx, ip, port));
        assert(ip == Ip(198, 51, 100, 20) && port == 40000);
        std::uint8_t wrong_tx[12] = {};
        assert(!ParseStunReply(reply, sizeof(reply), wrong_tx, ip, port));
    }
    // Classification.
    {
        const std::uint32_t lan = Ip(192, 168, 1, 10), pub = Ip(203, 0, 113, 7);
        assert(Classify(pub, pub, 3074, false, 0, 0) == Type::Open);
        assert(Classify(lan, pub, 50000, true, pub, 50000) == Type::Moderate);
        assert(Classify(lan, pub, 50000, true, pub, 50001) == Type::Strict);
        assert(Classify(lan, pub, 50000, true, Ip(203, 0, 113, 8), 50000) == Type::Strict);
        assert(Classify(lan, pub, 50000, false, 0, 0) == Type::Unknown);
    }
    std::puts("nat check: ok");
    return 0;
}
