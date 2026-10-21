// NAT type test (nat_check.h).
#include "nat_check.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace NatCheck {

namespace {

constexpr std::uint32_t kStunMagic = 0x2112A442;
// Cloudflare's public STUN server (RFC 5389, UDP 3478).
constexpr const char* kStunHost = "stun.cloudflare.com";
constexpr const char* kStunPort = "3478";

std::uint16_t Be16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }
std::uint32_t Be32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

bool Resolve(const std::string& host, const char* port, sockaddr_in& out) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), port, &hints, &list) != 0 || !list) return false;
    std::memcpy(&out, list->ai_addr, sizeof(out));
    freeaddrinfo(list);
    return true;
}

/// Sends `request` up to three times and waits for a reply `accept` takes.
template <typename Accept>
bool Ask(SOCKET s, const sockaddr_in& to, const std::uint8_t* request, int size, Accept&& accept) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        sendto(s, reinterpret_cast<const char*>(request), size, 0, reinterpret_cast<const sockaddr*>(&to),
               sizeof(to));
        const ULONGLONG until = GetTickCount64() + 700;
        for (ULONGLONG now = GetTickCount64(); now < until; now = GetTickCount64()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(s, &set);
            timeval tv{0, int((until - now) * 1000)};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0) break;
            std::uint8_t buf[512];
            sockaddr_in from{};
            int from_size = sizeof(from);
            const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_size);
            if (n > 0 && from.sin_addr.s_addr == to.sin_addr.s_addr && from.sin_port == to.sin_port &&
                accept(buf, std::size_t(n)))
                return true;
        }
    }
    return false;
}

}  // namespace

bool ParseServerReply(const std::uint8_t* data, std::size_t size, std::uint32_t& ip, std::uint16_t& port) {
    if (size < 9 || data[0] != 31) return false;
    std::memcpy(&ip, data + 3, 4);
    port = std::uint16_t(data[7] | data[8] << 8);
    return port != 0;
}

bool IsServerTestReply(const std::uint8_t* data, std::size_t size) { return size >= 15 && data[0] == 21; }

void StunRequest(std::uint8_t out[20], const std::uint8_t transaction[12]) {
    out[0] = 0x00, out[1] = 0x01;  // Binding request
    out[2] = 0x00, out[3] = 0x00;  // no attributes
    out[4] = 0x21, out[5] = 0x12, out[6] = 0xA4, out[7] = 0x42;
    std::memcpy(out + 8, transaction, 12);
}

bool ParseStunReply(const std::uint8_t* data, std::size_t size, const std::uint8_t transaction[12],
                    std::uint32_t& ip, std::uint16_t& port) {
    if (size < 20 || Be16(data) != 0x0101 || Be32(data + 4) != kStunMagic ||
        std::memcmp(data + 8, transaction, 12) != 0)
        return false;
    const std::size_t end = std::min<std::size_t>(size, 20u + Be16(data + 2));
    bool found = false;
    for (std::size_t at = 20; at + 4 <= end;) {
        const std::uint16_t type = Be16(data + at), length = Be16(data + at + 2);
        const std::uint8_t* value = data + at + 4;
        if (at + 4 + length > end) break;
        if ((type == 0x0020 || type == 0x0001) && length >= 8 && value[1] == 0x01) {  // IPv4
            std::uint16_t p = Be16(value + 2);
            std::uint32_t a = Be32(value + 4);
            if (type == 0x0020) {
                p ^= std::uint16_t(kStunMagic >> 16);
                a ^= kStunMagic;
            }
            port = p;
            const std::uint32_t network = htonl(a);
            std::memcpy(&ip, &network, 4);
            found = true;
            if (type == 0x0020) break;  // XOR-MAPPED-ADDRESS is the one to trust
        }
        at += 4 + ((length + 3u) & ~3u);
    }
    return found;
}

Type Classify(std::uint32_t local_ip, std::uint32_t a_ip, std::uint16_t a_port, bool have_b,
              std::uint32_t b_ip, std::uint16_t b_port) {
    if (a_ip == local_ip) return Type::Open;
    if (!have_b) return Type::Unknown;
    return a_ip == b_ip && a_port == b_port ? Type::Moderate : Type::Strict;
}

Result Check(const std::string& server_setting, int nat_port) {
    Result result;
    std::string host = server_setting;
    if (const auto colon = host.find(':'); colon != std::string::npos) host.resize(colon);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        result.error = "socket";
        return result;
    }
    sockaddr_in server{}, stun{};
    const bool have_stun = Resolve(kStunHost, kStunPort, stun);
    if (!Resolve(host, std::to_string(nat_port).c_str(), server)) {
        result.error = "dns";
        WSACleanup();
        return result;
    }
    // The PC's address on the way to the server (a connected socket picks the route).
    std::uint32_t local_ip = 0;
    if (SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); probe != INVALID_SOCKET) {
        sockaddr_in name{};
        int name_size = sizeof(name);
        if (connect(probe, reinterpret_cast<sockaddr*>(&server), sizeof(server)) == 0 &&
            getsockname(probe, reinterpret_cast<sockaddr*>(&name), &name_size) == 0)
            local_ip = name.sin_addr.s_addr;
        closesocket(probe);
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        if (s != INVALID_SOCKET) closesocket(s);
        result.error = "socket";
        WSACleanup();
        return result;
    }
    std::uint32_t a_ip = 0, b_ip = 0;
    std::uint16_t a_port = 0, b_port = 0;
    const std::uint8_t request[3] = {30, 2, 0};
    const bool have_a = Ask(s, server, request, 3, [&](const std::uint8_t* d, std::size_t n) {
        return ParseServerReply(d, n, a_ip, a_port);
    });
    // The game's second request, whose reply is longer: some networks let the first reply
    // through and drop these, and the game cannot start online then.
    const std::uint8_t test_request[4] = {20, 2, 0, 0};
    if (have_a && !Ask(s, server, test_request, 4, IsServerTestReply)) result.replies_blocked = true;
    bool have_b = false;
    if (have_a && have_stun && a_ip != local_ip) {
        std::uint8_t transaction[12];
        BCryptGenRandom(nullptr, transaction, sizeof(transaction), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        std::uint8_t stun_request[20];
        StunRequest(stun_request, transaction);
        have_b = Ask(s, stun, stun_request, 20, [&](const std::uint8_t* d, std::size_t n) {
            return ParseStunReply(d, n, transaction, b_ip, b_port);
        });
    }
    closesocket(s);
    WSACleanup();
    if (!have_a) {
        result.error = "server";
        return result;
    }
    result.type = Classify(local_ip, a_ip, a_port, have_b, b_ip, b_port);
    if (result.type == Type::Unknown) result.error = "stun";
    return result;
}

}  // namespace NatCheck
