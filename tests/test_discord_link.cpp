// DiscordLink::Authorize with a scripted browser: the authorization URL, the loopback redirect,
// requests that are not this launcher's, cancellation, timeout and busy ports. Never opens a
// real browser.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "discord_link.h"

namespace {

std::string Param(const std::string& url, const std::string& name) {
    const size_t at = url.find(name + "=");
    if (at == std::string::npos) return {};
    const size_t end = url.find('&', at);
    return url.substr(at + name.size() + 1, end == std::string::npos ? std::string::npos : end - at - name.size() - 1);
}

int PortOf(const std::string& url) {
    // redirect_uri=http%3A%2F%2F127.0.0.1%3A53134%2Fdiscord
    const std::string r = Param(url, "redirect_uri");
    const size_t at = r.find("127.0.0.1%3A");
    return at == std::string::npos ? 0 : std::atoi(r.c_str() + at + 12);
}

/// One HTTP GET to the loopback listener; the status line of the answer.
std::string Get(int port, const std::string& target) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u_short(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        closesocket(s);
        return "connect failed";
    }
    const std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(s, request.data(), int(request.size()), 0);
    std::string answer;
    char buf[1024];
    int got;
    while ((got = recv(s, buf, sizeof buf, 0)) > 0) answer.append(buf, size_t(got));
    closesocket(s);
    return answer.substr(0, answer.find("\r\n"));
}

}  // namespace

int main() {
    _set_error_mode(_OUT_TO_STDERR);  // a failed assert prints instead of opening a dialog
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    // 1. The full redirect: stray requests first, then Discord's with the code.
    std::thread browser;
    DiscordLink::Result r = DiscordLink::Authorize("1560408801704943708", 20, true, [&](const std::string& url) {
        assert(url.rfind("https://discord.com/oauth2/authorize?", 0) == 0);
        assert(Param(url, "client_id") == "1560408801704943708");
        assert(Param(url, "scope") == "identify" && Param(url, "response_type") == "code");
        const std::string state = Param(url, "state");
        const int port = PortOf(url);
        assert(state.size() == 32 && port == DiscordLink::kPorts[0]);
        browser = std::thread([state, port] {
            assert(Get(port, "/favicon.ico").find("404") != std::string::npos);
            assert(Get(port, "/discord?code=stolen&state=0123").find("400") != std::string::npos);
            assert(Get(port, "/discord?code=abc%2B123&state=" + state).find("200") != std::string::npos);
        });
    });
    browser.join();
    assert(r.ok && r.code == "abc+123" && r.redirect_uri == "http://127.0.0.1:53134/discord");
    std::puts("ok: code received; stray requests and another state ignored");

    // 2. Cancelled in Discord.
    r = DiscordLink::Authorize("1", 20, false, [&](const std::string& url) {
        const std::string state = Param(url, "state");
        const int port = PortOf(url);
        browser = std::thread([state, port] {
            Get(port, "/discord?error=access_denied&error_description=x&state=" + state);
        });
    });
    browser.join();
    assert(!r.ok && r.error == "denied");
    std::puts("ok: cancellation");

    // 3. Nobody answers.
    r = DiscordLink::Authorize("1", 1, false, [](const std::string&) {});
    assert(!r.ok && r.error == "timeout");
    std::puts("ok: timeout");

    // 4. A port in use: the next one; all in use: busy.
    SOCKET held[3];
    for (int i = 0; i < 3; ++i) {
        held[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(u_short(DiscordLink::kPorts[i]));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(held[i], reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(held[i], 1) == 0);
        if (i == 0) {
            int port = 0;
            r = DiscordLink::Authorize("1", 1, false, [&](const std::string& url) { port = PortOf(url); });
            assert(port == DiscordLink::kPorts[1] && r.redirect_uri == "http://127.0.0.1:53135/discord");
        }
    }
    r = DiscordLink::Authorize("1", 1, false, [](const std::string&) { assert(false); });
    assert(!r.ok && r.error == "busy");
    for (SOCKET s : held) closesocket(s);
    std::puts("ok: next free port; busy when all are taken");

    assert(DiscordLink::FormEncode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
    WSACleanup();
    std::puts("PASS");
    return 0;
}
