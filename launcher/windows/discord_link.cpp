// Discord account authorization, the browser half (see discord_link.h).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>

#include "discord_link.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace DiscordLink {
namespace {

std::string Hex(const unsigned char* data, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 15];
    }
    return out;
}

/// The OAuth2 state: only the redirect carrying it back is this launcher's own request.
std::string RandomState() {
    unsigned char raw[16];
    if (BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    return Hex(raw, sizeof raw);
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string QueryParam(const std::string& query, const std::string& name) {
    size_t at = 0;
    while (at <= query.size()) {
        const size_t end = query.find('&', at);
        const std::string pair = query.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, name) == 0) return UrlDecode(pair.substr(eq + 1));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return {};
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

void Reply(SOCKET client, const char* status, const std::string& title, const std::string& text) {
    const std::string html =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + title +
        "</title></head><body style=\"font-family:sans-serif;background:#1e1f22;color:#eee;"
        "display:flex;align-items:center;justify-content:center;height:100vh;margin:0\">"
        "<div style=\"text-align:center\"><h2>" + title + "</h2><p>" + text + "</p></div></body></html>";
    const std::string response = std::string("HTTP/1.1 ") + status +
                                 "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                                 "Connection: close\r\nContent-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
    send(client, response.data(), int(response.size()), 0);
}

}  // namespace

std::string FormEncode(const std::string& value) {
    std::string out;
    char buf[4];
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

Result Authorize(const std::string& client_id, int timeout_seconds, bool spanish,
                 const std::function<void(const std::string& url)>& open_url) {
    Result result;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        result.error = "network";
        return result;
    }
    // The first free loopback port: its redirect URI is the one registered for it.
    SOCKET listener = INVALID_SOCKET;
    int port = 0;
    for (const int p : kPorts) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(u_short(p));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(s, 4) == 0) {
            listener = s;
            port = p;
            break;
        }
        closesocket(s);
    }
    const std::string state = RandomState();
    if (listener == INVALID_SOCKET || state.empty()) {
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        result.error = "busy";
        return result;
    }
    result.redirect_uri = "http://127.0.0.1:" + std::to_string(port) + "/discord";
    const std::string url = "https://discord.com/oauth2/authorize?client_id=" + FormEncode(client_id) +
                            "&response_type=code&scope=identify&prompt=none&redirect_uri=" +
                            FormEncode(result.redirect_uri) + "&state=" + state;
    if (open_url) {
        open_url(url);
    } else {
        ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    result.error = "timeout";
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval tv{long(left.count() / 1000), long(left.count() % 1000) * 1000};
        if (select(0, &readable, nullptr, nullptr, &tv) <= 0) break;
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        DWORD recv_timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout), sizeof recv_timeout);
        std::string request;
        char buf[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int got = recv(client, buf, sizeof buf, 0);
            if (got <= 0) break;
            request.append(buf, size_t(got));
        }
        // "GET /discord?code=...&state=... HTTP/1.1"
        std::string target;
        if (request.compare(0, 4, "GET ") == 0) {
            const size_t end = request.find(' ', 4);
            if (end != std::string::npos) target = request.substr(4, end - 4);
        }
        const size_t q = target.find('?');
        const std::string path = target.substr(0, q);
        const std::string query = q == std::string::npos ? std::string() : target.substr(q + 1);
        if (path != "/discord") {
            Reply(client, "404 Not Found", "404", "");
            closesocket(client);
            continue;
        }
        if (QueryParam(query, "state") != state) {
            // Not this launcher's request (an old tab, another program): ignored, keep waiting.
            Reply(client, "400 Bad Request", spanish ? "Solicitud no válida" : "Invalid request",
                  spanish ? "Esta autorización no es la que pidió el launcher." : "This authorization is not the launcher's.");
            closesocket(client);
            continue;
        }
        const std::string error = QueryParam(query, "error");
        const std::string code = QueryParam(query, "code");
        if (!error.empty() || code.empty()) {
            result.error = error == "access_denied" ? "denied" : (error.empty() ? "denied" : error);
            Reply(client, "200 OK", spanish ? "Autorización cancelada" : "Authorization cancelled",
                  spanish ? "Puedes cerrar esta pestaña y volver al launcher." : "You can close this tab and go back to the launcher.");
        } else {
            result.ok = true;
            result.code = code;
            result.error.clear();
            Reply(client, "200 OK", spanish ? "Listo" : "Done",
                  spanish ? "Cuenta de Discord autorizada. Puedes cerrar esta pestaña y volver al launcher."
                          : "Discord account authorized. You can close this tab and go back to the launcher.");
        }
        closesocket(client);
        break;
    }
    closesocket(listener);
    WSACleanup();
    return result;
}

}  // namespace DiscordLink
