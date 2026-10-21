// NAT type test for the launcher's Online tab: whether other players can reach this PC directly.
//
// One UDP socket asks two servers which public address they see it from: the RyuNET server's
// NAT port (the request the game itself sends, type 30) and a public STUN server. The same
// public port for both means the router keeps one mapping per local port (players connect
// directly); different ports mean a symmetric NAT (direct connections mostly fail; the v0.5.1
// tunnel carries the game's traffic then). The addresses are compared here and never shown,
// stored or sent anywhere else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace NatCheck {

enum class Type {
    Unknown,   ///< no answer from the RyuNET server, or the comparison could not be made
    Open,      ///< the public address is the PC's own (no NAT in between)
    Moderate,  ///< one public port per local port, whoever is on the other side
    Strict,    ///< a different public port per destination (symmetric NAT)
};

struct Result {
    Type type = Type::Unknown;
    std::string error;  ///< why it is Unknown: "dns", "socket", "server" (no NAT reply), "stun"
    /// The server answered the address request (type 30) but none of its NAT test replies
    /// (type 20) arrived: the game then says "Failed to initialize network" (issue #42).
    bool replies_blocked = false;
};

/// Blocking, a few seconds at most (run it on a worker thread). server: the online_server
/// setting (a host or IP, with an optional :port, which is the HTTP port and is ignored here).
Result Check(const std::string& server, int nat_port = 3074);

// The parts below are the test's.

/// The RyuNET NAT reply to a type 30 request: 31, 2, 0, IPv4 (network order), port (little endian).
bool ParseServerReply(const std::uint8_t* data, std::size_t size, std::uint32_t& ip, std::uint16_t& port);
/// The RyuNET reply to the game's NAT test (type 20): 21, 2, 0, the client's address and port,
/// the server's address and port (15 bytes).
bool IsServerTestReply(const std::uint8_t* data, std::size_t size);
/// A STUN Binding success response for this transaction: XOR-MAPPED-ADDRESS (or MAPPED-ADDRESS).
/// ip in network byte order, port in host order.
bool ParseStunReply(const std::uint8_t* data, std::size_t size, const std::uint8_t transaction[12],
                    std::uint32_t& ip, std::uint16_t& port);
/// The 20-byte STUN Binding request for this transaction.
void StunRequest(std::uint8_t out[20], const std::uint8_t transaction[12]);
/// local_ip: the PC's address towards the RyuNET server; a: what that server saw; b: what the
/// STUN server saw (have_b false when it did not answer). IPs in the same byte order.
Type Classify(std::uint32_t local_ip, std::uint32_t a_ip, std::uint16_t a_port, bool have_b,
              std::uint32_t b_ip, std::uint16_t b_port);

}  // namespace NatCheck
