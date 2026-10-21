// Discord IPC: each frame is u32 opcode | u32 length (little endian) | UTF-8 JSON.
//   op 0 HANDSHAKE {"v":1,"client_id":"..."}  -> op 1 DISPATCH "READY", or op 2 CLOSE on a bad id
//   op 1 FRAME     {"cmd":"SET_ACTIVITY","args":{"pid":..,"activity":{..}|null},"nonce":".."}
//                  {"cmd":"SUBSCRIBE","evt":"ACTIVITY_JOIN","nonce":".."}
//                  events arrive as {"cmd":"DISPATCH","evt":"ACTIVITY_JOIN","data":{"secret":..}}
//   op 2 CLOSE, op 3 PING (answered with op 4 PONG, same payload)
// Closing the pipe makes Discord drop the presence of that process.
#include "discord_presence.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace DiscordPresence {
namespace {

enum : uint32_t { kOpHandshake = 0, kOpFrame = 1, kOpClose = 2, kOpPing = 3, kOpPong = 4 };
constexpr ULONGLONG kRetryMs = 15000;
constexpr auto kPoll = std::chrono::milliseconds(200);  // how often incoming events are read
constexpr DWORD kReplyTimeoutMs = 3000;
constexpr uint32_t kMaxFrame = 64 * 1024;

struct Desired {
    bool enabled = false;
    std::string app_id;
    bool has_activity = false;
    Activity activity;

    bool operator==(const Desired& o) const {
        const Activity& a = activity;
        const Activity& b = o.activity;
        return enabled == o.enabled && app_id == o.app_id && has_activity == o.has_activity &&
               (!has_activity ||
                (a.details == b.details && a.state == b.state && a.start == b.start &&
                 a.large_image == b.large_image && a.large_text == b.large_text && a.party_id == b.party_id &&
                 a.party_size == b.party_size && a.party_max == b.party_max && a.join_secret == b.join_secret));
    }
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_thread;
Desired g_desired;
uint64_t g_version = 0;  // bumped on every change of g_desired
bool g_stop = false;
Status g_status = Status::Off;
std::string g_error;
std::deque<std::string> g_joins;  // accepted invites, for TakeJoin
std::atomic<bool> g_accept_requests{true};

void SetStatus(Status s, std::string error = {}) {
    std::scoped_lock lock{g_mutex};
    g_status = s;
    g_error = std::move(error);
}

std::string Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    return out;
}

/// The first string value of "key" in a JSON text ("" when missing or not a string). Enough
/// for Discord's flat events; \uXXXX escapes are dropped, the values used here are ASCII.
std::string JsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    at += needle.size();
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t')) ++at;
    if (at >= json.size() || json[at] != '"') return {};
    std::string out;
    for (size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') return out;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= json.size()) break;
        switch (json[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'u': i += 4; break;
        default: out += json[i]; break;
        }
    }
    return {};
}

class Pipe {
public:
    ~Pipe() { Close(); }
    bool Open() {
        // Tests point it at a fake Discord (tests/test_discord_presence.cpp).
        wchar_t only[256];
        const DWORD n = GetEnvironmentVariableW(L"CTR_DISCORD_PIPE", only, 256);
        if (n > 0 && n < 256) {
            handle_ = CreateFileW(only, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            return handle_ != INVALID_HANDLE_VALUE;
        }
        for (int i = 0; i < 10; ++i) {
            const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
            HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                handle_ = h;
                return true;
            }
        }
        return false;
    }
    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    /// False once Discord has closed its end.
    bool Alive() const { return IsOpen() && Available() != kGone; }
    /// Bytes waiting to be read, or kGone.
    static constexpr DWORD kGone = ~DWORD{0};
    DWORD Available() const {
        DWORD avail = 0;
        return PeekNamedPipe(handle_, nullptr, 0, nullptr, &avail, nullptr) ? avail : kGone;
    }

    bool Write(uint32_t op, const std::string& json) {
        std::string frame(8 + json.size(), '\0');
        const uint32_t len = uint32_t(json.size());
        std::memcpy(frame.data(), &op, 4);
        std::memcpy(frame.data() + 4, &len, 4);
        std::memcpy(frame.data() + 8, json.data(), json.size());
        DWORD written = 0;
        return WriteFile(handle_, frame.data(), DWORD(frame.size()), &written, nullptr) && written == frame.size();
    }

    /// Reads one frame, giving up after timeout_ms without blocking on an idle pipe.
    bool Read(uint32_t& op, std::string& json, DWORD timeout_ms) {
        uint32_t header[2];
        if (!ReadExact(header, sizeof header, timeout_ms)) return false;
        if (header[1] > kMaxFrame) return false;
        op = header[0];
        json.assign(header[1], '\0');
        return header[1] == 0 || ReadExact(json.data(), header[1], timeout_ms);
    }

private:
    bool ReadExact(void* dst, DWORD size, DWORD timeout_ms) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        DWORD avail;
        while ((avail = Available()) != kGone && avail < size) {
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
        }
        if (avail == kGone) return false;
        DWORD got = 0;
        return ReadFile(handle_, dst, size, &got, nullptr) && got == size;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

/// One connection to Discord: requests, their replies, and the events that arrive between them.
class Session {
public:
    explicit Session(Pipe& pipe) : pipe_(pipe) {}

    /// A new connection: nothing from the previous one is still pending.
    void Reset() {
        nonce_ = 0;
        accept_.clear();
    }

    bool Handshake(const std::string& app_id) {
        if (!pipe_.Write(kOpHandshake, "{\"v\":1,\"client_id\":\"" + Escape(app_id) + "\"}")) return false;
        uint32_t op = 0;
        std::string reply;
        if (!pipe_.Read(op, reply, kReplyTimeoutMs)) {
            SetStatus(Status::Error, "Discord did not answer the handshake");
            return false;
        }
        if (op != kOpFrame || reply.find("\"READY\"") == std::string::npos) {
            // op 2 CLOSE carries the reason, e.g. an unknown client_id.
            SetStatus(Status::Error, reply.substr(0, 200));
            return false;
        }
        // Accepting an invite (or "Join") arrives as ACTIVITY_JOIN; "Ask to Join" from someone
        // else as ACTIVITY_JOIN_REQUEST. Discord delivers a join that started this launcher
        // once it is subscribed.
        return Request("{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"%s\",\"args\":{}}") &&
               Request("{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN_REQUEST\",\"nonce\":\"%s\",\"args\":{}}");
    }

    bool SetActivity(const Desired& d) {
        const std::string activity = d.has_activity ? ActivityJson(d.activity) : "null";
        std::string reply;
        if (!Request("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                         ",\"activity\":" + activity + "},\"nonce\":\"%s\"}",
                     &reply)) {
            return false;
        }
        if (JsonString(reply, "evt") == "ERROR") {
            // Rejected (e.g. a field too short), but the connection is fine.
            SetStatus(Status::Error, reply.substr(0, 200));
        } else {
            SetStatus(Status::Connected);
        }
        return true;
    }

    /// Handles whatever Discord sent on its own (events, pings). False when the pipe is gone.
    bool Pump() {
        DWORD avail;
        while ((avail = pipe_.Available()) != Pipe::kGone && avail >= 8) {
            uint32_t op = 0;
            std::string json;
            if (!pipe_.Read(op, json, kReplyTimeoutMs) || !Handle(op, json)) return false;
        }
        if (avail == Pipe::kGone) return false;
        // Requests are answered here, outside Handle, so a reply is never read inside another.
        std::vector<std::string> accept;
        accept.swap(accept_);
        for (const auto& user : accept) {
            if (!Request("{\"cmd\":\"SEND_ACTIVITY_JOIN_INVITE\",\"args\":{\"user_id\":\"" + Escape(user) +
                         "\"},\"nonce\":\"%s\"}")) {
                return false;
            }
        }
        return true;
    }

private:
    static std::string ActivityJson(const Activity& a) {
        std::string j = "{\"details\":\"" + Escape(a.details) + "\"";
        if (!a.state.empty()) j += ",\"state\":\"" + Escape(a.state) + "\"";
        if (a.start > 0) j += ",\"timestamps\":{\"start\":" + std::to_string(a.start) + "}";
        if (!a.large_image.empty()) {
            j += ",\"assets\":{\"large_image\":\"" + Escape(a.large_image) + "\"";
            if (!a.large_text.empty()) j += ",\"large_text\":\"" + Escape(a.large_text) + "\"";
            j += "}";
        }
        if (!a.party_id.empty() && a.party_size > 0 && a.party_max >= a.party_size) {
            j += ",\"party\":{\"id\":\"" + Escape(a.party_id) + "\",\"size\":[" + std::to_string(a.party_size) + "," +
                 std::to_string(a.party_max) + "]}";
            if (!a.join_secret.empty()) j += ",\"secrets\":{\"join\":\"" + Escape(a.join_secret) + "\"}";
        }
        return j + "}";
    }

    /// Sends a command (its last "%s", the nonce's place, is replaced by a fresh nonce) and
    /// waits for its reply; events that arrive first are handled on the way.
    bool Request(const std::string& format, std::string* reply = nullptr) {
        const std::string nonce = std::to_string(++nonce_);
        std::string json = format;
        if (const auto at = json.rfind("%s"); at != std::string::npos) json.replace(at, 2, nonce);
        if (!pipe_.Write(kOpFrame, json)) return false;
        const ULONGLONG deadline = GetTickCount64() + kReplyTimeoutMs;
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            uint32_t op = 0;
            std::string frame;
            if (!pipe_.Read(op, frame, DWORD(deadline - now))) return false;
            if (op == kOpFrame && JsonString(frame, "nonce") == nonce) {
                if (reply) *reply = std::move(frame);
                return true;
            }
            if (!Handle(op, frame)) return false;
        }
    }

    bool Handle(uint32_t op, const std::string& json) {
        if (op == kOpPing) return pipe_.Write(kOpPong, json);
        if (op == kOpClose) return false;
        if (op != kOpFrame) return true;
        const std::string evt = JsonString(json, "evt");
        if (evt == "ACTIVITY_JOIN") {
            const std::string secret = JsonString(json, "secret");
            if (!secret.empty()) {
                std::scoped_lock lock{g_mutex};
                if (g_joins.size() < 8) g_joins.push_back(secret);
            }
        } else if (evt == "ACTIVITY_JOIN_REQUEST" && g_accept_requests) {
            const std::string user = JsonString(json, "id");  // data.user.id
            if (!user.empty()) accept_.push_back(user);
        }
        return true;
    }

    Pipe& pipe_;
    uint64_t nonce_ = 0;
    std::vector<std::string> accept_;
};

void Run() {
    Pipe pipe;
    Session session{pipe};
    std::string connected_id;
    uint64_t sent = ~uint64_t{0};
    ULONGLONG last_attempt = 0;
    std::unique_lock lock{g_mutex};
    for (;;) {
        const Desired d = g_desired;
        const uint64_t version = g_version;
        lock.unlock();

        if (pipe.IsOpen() && !pipe.Alive()) pipe.Close();  // Discord quit: resend once it is back
        if (!d.enabled || d.app_id.empty()) {
            pipe.Close();
            connected_id.clear();
            SetStatus(d.enabled ? Status::NoAppId : Status::Off);
        } else {
            // Send as soon as something changed, but knock on a missing Discord only every
            // few seconds.
            const bool may_connect =
                pipe.IsOpen() || last_attempt == 0 || GetTickCount64() - last_attempt >= kRetryMs;
            if (may_connect && (sent != version || !pipe.IsOpen())) {
                for (int attempt = 0; attempt < 2; ++attempt) {
                    if (!pipe.IsOpen() || connected_id != d.app_id) {
                        pipe.Close();
                        last_attempt = GetTickCount64();
                        if (!pipe.Open()) {
                            SetStatus(Status::NoDiscord);
                            break;
                        }
                        session.Reset();
                        if (!session.Handshake(d.app_id)) {
                            pipe.Close();
                            break;
                        }
                        connected_id = d.app_id;
                    }
                    if (session.SetActivity(d)) {
                        sent = version;
                        break;
                    }
                    pipe.Close();  // Discord restarted: reconnect once and resend
                }
            }
            if (pipe.IsOpen() && !session.Pump()) pipe.Close();
        }

        lock.lock();
        if (g_stop) break;
        if (g_version == version) g_cv.wait_for(lock, kPoll, [&] { return g_stop || g_version != version; });
        if (g_stop) break;
    }
    lock.unlock();
    pipe.Close();  // clears the presence
    SetStatus(Status::Off);
}

}  // namespace

void Update(bool enabled, const std::string& app_id, const Activity* activity) {
    Desired d;
    d.enabled = enabled;
    d.app_id = app_id;
    if (activity) {
        d.has_activity = true;
        d.activity = *activity;
    }
    std::scoped_lock lock{g_mutex};
    if (g_stop) return;
    if (!g_thread.joinable()) {
        if (!enabled) return;  // never start the thread for a disabled presence
        g_thread = std::thread(Run);
    }
    if (d == g_desired) return;
    g_desired = std::move(d);
    ++g_version;
    g_cv.notify_all();
}

void SetAcceptJoinRequests(bool accept) { g_accept_requests = accept; }

bool TakeJoin(std::string& secret) {
    std::scoped_lock lock{g_mutex};
    if (g_joins.empty()) return false;
    secret = std::move(g_joins.front());
    g_joins.pop_front();
    return true;
}

Status GetStatus() {
    std::scoped_lock lock{g_mutex};
    return g_status;
}

std::string LastError() {
    std::scoped_lock lock{g_mutex};
    return g_error;
}

void Shutdown() {
    {
        std::scoped_lock lock{g_mutex};
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace DiscordPresence
