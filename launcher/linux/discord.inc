// Reuse Linux Discord IPC and OAuth services; only transport and lifecycle live here.
} // application namespace
namespace DiscordLink {
std::string FormEncode(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { out += '%'; out += "0123456789ABCDEF"[c >> 4]; out += "0123456789ABCDEF"[c & 15]; }
    }
    return out;
}
Result Authorize(const std::string& client_id, int timeout_seconds, bool, const std::function<void(const std::string&)>&) {
    const auto answer = RunService("{\"op\":\"oauth\",\"client\":\"" + JsonEscape(client_id) +
        "\",\"timeout\":" + std::to_string(timeout_seconds) + "}", (timeout_seconds + 5) * 1000);
    return {JsonBool(answer, "ok"), JsonText(answer, "code"), JsonText(answer, "redirect_uri"), JsonText(answer, "error")};
}
}
namespace DiscordPresence {
namespace {
std::mutex mutex;
std::condition_variable changed;
std::thread worker;
std::string desired, error;
std::deque<std::string> joins;
bool stop = false, accept = true;
Status status = Status::Off;
void Run() {
    int input[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input) != 0) return;
    const auto child = LinuxProcess::Start({Narrow(g_app.python->exe), PathText(g_app.root / "launcher/linux/services.py"), "--presence"},
        PathText(g_app.root), {}, input[1]);
    close(input[1]);
    if (!child.pid) { close(input[0]); return; }
    std::string previous;
    while (true) {
        std::string request;
        {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(500), [&] { return stop || desired != previous; });
            if (stop) break;
            request = desired; previous = desired;
            request += std::string(",\"accept\":") + (accept ? "true" : "false") + "}\n";
        }
        if (send(input[0], request.data(), request.size(), MSG_NOSIGNAL) != ssize_t(request.size())) break;
        std::string answer;
        const auto until = Ticks() + 2000;
        while (answer.find('\n') == std::string::npos && Ticks() < until) {
            pollfd fd{child.output, POLLIN, 0};
            if (poll(&fd, 1, 100) <= 0) continue;
            char bytes[4096]; const auto n = read(child.output, bytes, sizeof bytes);
            if (n <= 0 || answer.size() > 65536) break;
            answer.append(bytes, size_t(n));
        }
        if (answer.find('\n') == std::string::npos) break;
        std::scoped_lock lock(mutex);
        const auto value = JsonText(answer, "status");
        status = value == "connected" ? Status::Connected : value == "off" ? Status::Off :
            value == "no_app_id" ? Status::NoAppId : value == "error" ? Status::Error : Status::NoDiscord;
        error = JsonText(answer, "error");
        const auto join = JsonText(answer, "join");
        if (!join.empty()) joins.push_back(join);
    }
    close(input[0]); close(child.output);
    kill(-child.pid, SIGTERM);
    int child_status; while (waitpid(child.pid, &child_status, 0) < 0 && errno == EINTR) {}
}
}
void Update(bool enabled, const std::string& app_id, const Activity* activity) {
    std::string request = std::string("{\"enabled\":") + (enabled ? "true" : "false") + ",\"app_id\":\"" + JsonEscape(app_id) + "\",\"activity\":";
    if (activity) {
        const auto& a = *activity;
        request += "{\"details\":\"" + JsonEscape(a.details) + "\",\"state\":\"" + JsonEscape(a.state) +
            "\",\"start\":" + std::to_string(a.start) + ",\"large_image\":\"" + JsonEscape(a.large_image) +
            "\",\"large_text\":\"" + JsonEscape(a.large_text) + "\",\"party_id\":\"" + JsonEscape(a.party_id) +
            "\",\"party_size\":" + std::to_string(a.party_size) + ",\"party_max\":" + std::to_string(a.party_max) +
            ",\"join_secret\":\"" + JsonEscape(a.join_secret) + "\"}";
    } else request += "null";
    std::scoped_lock lock(mutex);
    if (stop) return;
    if (enabled && !g_app.python) {
        status = Status::Error; error = "Python 3 is required for Discord presence."; return;
    }
    desired = request;
    if (!worker.joinable() && enabled) worker = std::thread(Run);
    changed.notify_one();
}
void SetAcceptJoinRequests(bool enabled) { std::scoped_lock lock(mutex); accept = enabled; }
bool TakeJoin(std::string& secret) {
    std::scoped_lock lock(mutex);
    if (joins.empty()) return false;
    secret = std::move(joins.front()); joins.pop_front(); return true;
}
Status GetStatus() { std::scoped_lock lock(mutex); return status; }
std::string LastError() { std::scoped_lock lock(mutex); return error; }
void Shutdown() {
    { std::scoped_lock lock(mutex); stop = true; changed.notify_one(); }
    if (worker.joinable()) worker.join();
}
}
namespace {
