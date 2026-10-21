// DiscordPresence against a fake Discord on a private pipe (CTR_DISCORD_PIPE): handshake,
// subscriptions, a joinable activity, an invite event that arrives before a reply, a ping,
// and an "Ask to Join" that must be accepted.
#include "discord_presence.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

int g_failures = 0;
#define CHECK(cond, what)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

HANDLE g_server = INVALID_HANDLE_VALUE;

bool ReadFrame(uint32_t& op, std::string& json) {
    uint32_t header[2];
    DWORD got = 0;
    if (!ReadFile(g_server, header, sizeof header, &got, nullptr) || got != sizeof header) return false;
    op = header[0];
    json.assign(header[1], '\0');
    DWORD total = 0;
    while (total < header[1]) {
        if (!ReadFile(g_server, json.data() + total, header[1] - total, &got, nullptr) || !got) return false;
        total += got;
    }
    return true;
}

bool WriteFrame(uint32_t op, const std::string& json) {
    std::string frame(8 + json.size(), '\0');
    const uint32_t len = uint32_t(json.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &len, 4);
    std::memcpy(frame.data() + 8, json.data(), json.size());
    DWORD written = 0;
    return WriteFile(g_server, frame.data(), DWORD(frame.size()), &written, nullptr) && written == frame.size();
}

std::string NonceOf(const std::string& json) {
    const auto at = json.find("\"nonce\":\"");
    if (at == std::string::npos) return {};
    return json.substr(at + 9, json.find('"', at + 9) - at - 9);
}

void Reply(const std::string& request, const std::string& cmd) {
    WriteFrame(1, "{\"cmd\":\"" + cmd + "\",\"data\":{},\"evt\":null,\"nonce\":\"" + NonceOf(request) + "\"}");
}

void FakeDiscord() {
    if (!ConnectNamedPipe(g_server, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        CHECK(false, "client never connected");
        return;
    }
    uint32_t op = 0;
    std::string json;

    CHECK(ReadFrame(op, json) && op == 0 && json.find("\"client_id\":\"123\"") != std::string::npos, "handshake");
    WriteFrame(1, "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1},\"evt\":\"READY\",\"nonce\":null}");

    CHECK(ReadFrame(op, json) && json.find("\"SUBSCRIBE\"") != std::string::npos &&
              json.find("\"ACTIVITY_JOIN\"") != std::string::npos,
          "subscribe ACTIVITY_JOIN");
    Reply(json, "SUBSCRIBE");
    CHECK(ReadFrame(op, json) && json.find("\"ACTIVITY_JOIN_REQUEST\"") != std::string::npos,
          "subscribe ACTIVITY_JOIN_REQUEST");
    Reply(json, "SUBSCRIBE");

    CHECK(ReadFrame(op, json) && json.find("\"SET_ACTIVITY\"") != std::string::npos, "set activity");
    CHECK(json.find("\"party\":{\"id\":\"ctr:host:tdyu\",\"size\":[3,8]}") != std::string::npos, "party in activity");
    CHECK(json.find("\"secrets\":{\"join\":\"ctr1|host|TDYU\"}") != std::string::npos, "join secret in activity");
    // An invite accepted by this player arrives BEFORE the reply it is waiting for.
    WriteFrame(1, "{\"cmd\":\"DISPATCH\",\"data\":{\"secret\":\"ctr1|octr.example:8080|Amigo_1\"},"
                  "\"evt\":\"ACTIVITY_JOIN\",\"nonce\":null}");
    Reply(json, "SET_ACTIVITY");

    WriteFrame(3, "{\"ping\":42}");
    CHECK(ReadFrame(op, json) && op == 4 && json == "{\"ping\":42}", "pong echoes the ping");

    WriteFrame(1, "{\"cmd\":\"DISPATCH\",\"data\":{\"user\":{\"id\":\"4242\",\"username\":\"amigo\"}},"
                  "\"evt\":\"ACTIVITY_JOIN_REQUEST\",\"nonce\":null}");
    CHECK(ReadFrame(op, json) && json.find("\"SEND_ACTIVITY_JOIN_INVITE\"") != std::string::npos &&
              json.find("\"user_id\":\"4242\"") != std::string::npos,
          "ask to join accepted");
    Reply(json, "SEND_ACTIVITY_JOIN_INVITE");
}

}  // namespace

int main() {
    const std::wstring name = L"\\\\.\\pipe\\ctr-discord-test-" + std::to_wstring(GetCurrentProcessId());
    g_server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                65536, 65536, 0, nullptr);
    if (g_server == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "cannot create the fake Discord pipe\n");
        return 1;
    }
    SetEnvironmentVariableW(L"CTR_DISCORD_PIPE", name.c_str());
    std::thread discord(FakeDiscord);

    DiscordPresence::Activity activity;
    activity.details = "Playing online";
    activity.state = "Nickname: TDYU";
    activity.party_id = "ctr:host:tdyu";
    activity.party_size = 3;
    activity.party_max = 8;
    activity.join_secret = "ctr1|host|TDYU";
    DiscordPresence::SetAcceptJoinRequests(true);
    DiscordPresence::Update(true, "123", &activity);

    std::string secret;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!DiscordPresence::TakeJoin(secret) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(secret == "ctr1|octr.example:8080|Amigo_1", "accepted invite handed to the launcher");

    discord.join();
    CHECK(DiscordPresence::GetStatus() == DiscordPresence::Status::Connected, "connected status");
    DiscordPresence::Shutdown();
    CloseHandle(g_server);
    if (g_failures) return 1;
    std::printf("discord presence: all checks passed\n");
    return 0;
}
