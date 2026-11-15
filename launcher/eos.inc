// Included by launcher.cpp inside its application namespace.
// ---- EOS bridge (eosbridge/eosbridge.c: eos\ryunet-bridge.exe) ----
// Some networks never reach the RyuNET server (an ISP that filters its name or its address, ports
// closed on the way). For them ryunet-bridge carries the traffic through Epic Online Services to a
// gateway next to the server: it checks the server directly first and only when that fails logs
// in to EOS, anonymously (no Epic account), and offers local ports for the server's. This
// launcher's requests, run.py's and the game's (BB_ONLINE_BRIDGE) then go through them.
// launcher.ini eos_bridge: auto (the check decides), on (always), off (never). The bridge lives as
// long as its stdin is open: this launcher's end and run.py's (inherited), so a game still
// running when the launcher closes keeps it.
struct EosBridge {
    std::mutex mutex;
    std::condition_variable changed;
    std::string wanted;  // "mode|server" it was started for; "" none
    std::string host;    // the server's, and its HTTP port
    uint16_t http_port = 0;
    // starting, checking, direct, connecting, ready, lost, failed, missing (not installed), off
    std::string state;
    std::string detail, map, server_ip;
    std::string peer_port;  // races through EOS P2P (BRIDGE PEERS): its UDP port, for BB_ONLINE_EOS
    std::deque<std::string> lines;  // its latest log lines
#ifdef _WIN32
    HANDLE process = nullptr, input = nullptr;
#else
    pid_t process = 0;
    int input = -1;
#endif
    std::thread reader;
};
EosBridge g_eos;

fs::path EosBridgeDir() {
    std::error_code error;
    const auto built = g_app.root / "out/eosbridge";
#ifdef _WIN32
    const char* name = "ryunet-bridge.exe";
#else
    const char* name = "ryunet-bridge";
#endif
    return !fs::is_regular_file(g_app.root / "eos" / name, error) &&
           fs::is_regular_file(built / name, error) ? built : g_app.root / "eos";
}

bool EosBridgeInstalled() {
    std::error_code ec;
    const fs::path dir = EosBridgeDir();
    // Its settings are built into ryunet-bridge.exe (an eosbridge.ini next to it overrides them).
#ifdef _WIN32
    return fs::is_regular_file(dir / L"ryunet-bridge.exe", ec) && fs::is_regular_file(dir / L"EOSSDK-Win64-Shipping.dll", ec);
#else
    return fs::is_regular_file(dir / "ryunet-bridge", ec) && fs::is_regular_file(dir / "libEOSSDK-Linux-Shipping.so", ec);
#endif
}

std::string EosBridgeMode() {
    const std::string mode = g_app.launcher.Get("eos_bridge", "auto");
    return mode == "on" || mode == "off" ? mode : "auto";
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void EosBridgeLine(const std::string& line) {
    std::scoped_lock lock{g_eos.mutex};
    if (line.rfind("BRIDGE ", 0) != 0) {
        g_eos.lines.push_back(line);
        while (g_eos.lines.size() > 40) g_eos.lines.pop_front();
        return;
    }
    std::istringstream in(line.substr(7));
    std::string word;
    in >> word;
    if (word == "CHECKING") g_eos.state = "checking";
    else if (word == "DIRECT") g_eos.state = "direct";
    else if (word == "CONNECTING") g_eos.state = "connecting";
    else if (word == "LOST") g_eos.state = "lost";
    else if (word == "PEERS") {
        for (std::string field; in >> field;)
            if (field.rfind("port=", 0) == 0) g_eos.peer_port = field.substr(5);
    }
    else if (word == "FAILED") {
        g_eos.state = "failed";
        std::getline(in, g_eos.detail);
        g_eos.detail = Trim(g_eos.detail);
    } else if (word == "READY") {
        g_eos.state = "ready";
        for (std::string field; in >> field;) {
            if (field.rfind("ip=", 0) == 0) g_eos.server_ip = field.substr(3);
            else if (field.rfind("map=", 0) == 0) g_eos.map = field.substr(4);
        }
    }
    g_eos.changed.notify_all();
}

void EosBridgeStop() {
#ifdef _WIN32
    HANDLE process = nullptr, input = nullptr;
    {
        std::scoped_lock lock{g_eos.mutex};
        process = std::exchange(g_eos.process, nullptr);
        input = std::exchange(g_eos.input, nullptr);
    }
    if (process) {
        TerminateProcess(process, 0);
        CloseHandle(process);
    }
    if (input) CloseHandle(input);
#else
    pid_t process;
    int input;
    {
        std::scoped_lock lock{g_eos.mutex};
        process = std::exchange(g_eos.process, 0);
        input = std::exchange(g_eos.input, -1);
    }
    if (input >= 0) close(input);
    if (process) {
        kill(-process, SIGKILL);
        int status;
        while (waitpid(process, &status, 0) < 0 && errno == EINTR) {}
    }
#endif
    if (g_eos.reader.joinable()) g_eos.reader.join();
    std::scoped_lock lock{g_eos.mutex};
    g_eos.wanted.clear();
    g_eos.host.clear();
    g_eos.state.clear();
    g_eos.detail.clear();
    g_eos.map.clear();
    g_eos.server_ip.clear();
    g_eos.peer_port.clear();
    g_eos.changed.notify_all();
}

void AddLog(const std::string& line);

#ifdef _WIN32
/// This install's processes named name (bin\sacredfire.exe, eos\ryunet-bridge.exe): their ids.
std::vector<DWORD> InstallProcesses(const wchar_t* name, const fs::path& exe) {
    std::vector<DWORD> found;
    const std::wstring wanted = fs::absolute(exe).lexically_normal().wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!process) continue;
        wchar_t path[MAX_PATH * 4];
        DWORD size = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &size) &&
            _wcsicmp(fs::path(path).lexically_normal().c_str(), wanted.c_str()) == 0)
            found.push_back(pe.th32ProcessID);
        CloseHandle(process);
    }
    CloseHandle(snap);
    return found;
}

/// A bridge an earlier launcher left running (v0.8.5 report: one that outlived its launcher held
/// the local ports, and the new one could not start). Ended once, before this launcher's first
/// bridge, unless this install's game is still running: that one may be using it.
void EosBridgeEndLeftovers() {
    static bool done = false;
    if (std::exchange(done, true)) return;
    if (!InstallProcesses(L"sacredfire.exe", g_app.bin / L"sacredfire.exe").empty()) return;
    for (DWORD id : InstallProcesses(L"ryunet-bridge.exe", EosBridgeDir() / L"ryunet-bridge.exe")) {
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, id)) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
            AddLog("EOS bridge: ended one left running by an earlier launcher (process " + std::to_string(id) + ")");
        }
    }
}

#endif

void EosBridgeStart(const std::string& wanted, const std::string& mode, const std::string& server) {
    EosBridgeStop();
#ifdef _WIN32
    EosBridgeEndLeftovers();
#endif
    std::string host;
    uint16_t port = 0;
    SplitServer(server, host, port);
    auto set_state = [&](const char* state, const std::string& detail = {}) {
        std::scoped_lock lock{g_eos.mutex};
        g_eos.wanted = wanted;
        g_eos.host = LowerAscii(host);
        g_eos.http_port = port;
        g_eos.state = state;
        g_eos.detail = detail;
        g_eos.changed.notify_all();
    };
    if (mode == "off") return set_state("off");
    if (!EosBridgeInstalled()) return set_state("missing");
    if (host.empty() || host.find_first_of(" \"") != std::string::npos) return set_state("failed", "server");

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_read = nullptr, out_write = nullptr, in_read = nullptr, in_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) return set_state("failed", "pipe");
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return set_state("failed", "pipe");
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    // The bridge inherits its two pipe ends and nothing else of the launcher's.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[2] = {in_read, out_write};
    const bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit,
                                                    nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = out_write;
    si.lpAttributeList = attrs_ok ? attrs : nullptr;
    const fs::path exe = EosBridgeDir() / L"ryunet-bridge.exe";
    std::wstring cmd = Quote(exe.wstring()) + L" --client --exit-on-eof --server " + Quote(Widen(host)) +
                       L" --http-port " + std::to_wstring(port) + (mode == "on" ? L" --mode on" : L"");
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                   EosBridgeDir().c_str(), &si.StartupInfo, &pi);
    const DWORD start_error = ok ? 0 : GetLastError();
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    CloseHandle(out_write);
    CloseHandle(in_read);
    if (!ok) {
        CloseHandle(out_read);
        CloseHandle(in_write);
        return set_state("failed", "start " + std::to_string(start_error));
    }
    CloseHandle(pi.hThread);
    // From now on run.py inherits the bridge's stdin: a game left running keeps the bridge.
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    set_state("starting");
    {
        std::scoped_lock lock{g_eos.mutex};
        g_eos.process = pi.hProcess;
        g_eos.input = in_write;
        g_eos.lines.clear();
    }
#else
    int input[2];
    if (pipe2(input, O_CLOEXEC) != 0) return set_state("failed", "pipe");
    auto args = std::vector<std::string>{PathText(EosBridgeDir() / "ryunet-bridge"), "--client",
        "--exit-on-eof", "--server", host, "--http-port", std::to_string(port)};
    if (mode == "on") { args.push_back("--mode"); args.push_back("on"); }
    const auto child = LinuxProcess::Start(args, PathText(EosBridgeDir()), {}, input[0]);
    close(input[0]);
    if (!child.pid) { close(input[1]); return set_state("failed", "start"); }
    const int out_read = child.output;
    set_state("starting");
    {
        std::scoped_lock lock{g_eos.mutex};
        g_eos.process = child.pid;
        g_eos.input = input[1];
        g_eos.lines.clear();
    }
#endif
    g_eos.reader = std::thread([out_read] {
        std::string pending;
        char buf[2048];
#ifdef _WIN32
        DWORD n = 0;
        while (ReadFile(out_read, buf, sizeof buf, &n, nullptr) && n) {
#else
        ssize_t n;
        while ((n = read(out_read, buf, sizeof buf)) != 0) {
            if (n < 0) { if (errno == EINTR) continue; break; }
#endif
            pending.append(buf, n);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                EosBridgeLine(Trim(pending.substr(0, nl)));
                pending.erase(0, nl + 1);
            }
        }
#ifdef _WIN32
        CloseHandle(out_read);
#else
        close(out_read);
#endif
        std::scoped_lock lock{g_eos.mutex};
        if (g_eos.state != "direct" && g_eos.state != "failed") {
            g_eos.state = "failed";
            if (g_eos.detail.empty()) g_eos.detail = "exited";
        }
        g_eos.changed.notify_all();
    });
}

/// Starts, restarts or stops the bridge when the online settings change (each frame; not while
/// the game runs, which may be using it).
void EosBridgePoll() {
    App& a = g_app;
    if (a.game_running) return;
    const std::string server = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
    const std::string mode = EosBridgeMode();
    std::string host;
    uint16_t port = 0;
    SplitServer(server, host, port);
    const bool local = host.empty() || host == "127.0.0.1" || LowerAscii(host) == "localhost";
    const std::string want = a.game.Flag("online", true) && !local ? mode + "|" + server : "";
    static std::string pending;
    static uint64_t since = 0;
    static bool first = true;
    std::string current;
    {
        std::scoped_lock lock{g_eos.mutex};
        current = g_eos.wanted;
    }
    if (want == current) {
        pending = want;
        first = false;
        return;
    }
    if (want != pending) {  // settled for a moment first: the server box changes on every key
        pending = want;
        since = Ticks();
    }
    if (!first && Ticks() - since < 1500) return;
    first = false;
    if (want.empty()) EosBridgeStop();
    else EosBridgeStart(want, mode, server);
}

/// Where a connection to this server's port goes: true with local, the bridge's port on
/// 127.0.0.1, when the bridge carries the server. Waits up to wait_ms while the bridge for it is
/// still checking or connecting (background threads only).
bool EosBridgeRoute(const std::string& server, uint16_t server_port, uint16_t& local, int wait_ms) {
    std::string host;
    uint16_t http_port = 0;
    SplitServer(server, host, http_port);
    host = LowerAscii(host);
    std::unique_lock lock{g_eos.mutex};
    const auto deciding = [&] {
        return g_eos.host == host &&
               (g_eos.state == "starting" || g_eos.state == "checking" || g_eos.state == "connecting");
    };
    if (wait_ms > 0) g_eos.changed.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return !deciding(); });
    if (g_eos.host != host || (g_eos.state != "ready" && g_eos.state != "lost")) return false;
    std::istringstream in(g_eos.map);
    std::string part;
    std::getline(in, part, ',');  // the UDP port
    while (std::getline(in, part, ',')) {
        const size_t colon = part.find(':');
        if (colon == std::string::npos || std::atoi(part.substr(0, colon).c_str()) != server_port) continue;
        local = uint16_t(std::atoi(part.substr(colon + 1).c_str()));
        return local != 0;
    }
    return false;
}

/// What the game needs to go through the bridge (BB_ONLINE_BRIDGE, BB_ONLINE_SERVER_IP), or
/// empty strings when it goes to the server directly; peers, its port for races through EOS P2P
/// (BB_ONLINE_EOS), in both cases.
void EosBridgeGameEnv(const std::string& server, std::string& map, std::string& ip, std::string& peers, int wait_ms) {
    map.clear();
    ip.clear();
    peers.clear();
    uint16_t local = 0, port = 0;
    std::string host;
    SplitServer(server, host, port);
    const bool bridged = EosBridgeRoute(server, port, local, wait_ms);
    std::scoped_lock lock{g_eos.mutex};
    if (g_eos.host != LowerAscii(host)) return;
    if (bridged) {
        map = g_eos.map;
        ip = g_eos.server_ip;
    }
    if (g_eos.state == "direct" || bridged) peers = g_eos.peer_port;
}

/// What one server sees of the installation's key: HMAC-SHA256(key, host), as run.py sends it.
/// Every server gets a different value, so none can reuse it on another to take a nickname.
/// SHA-256 of msg (HMAC-SHA256 with key when there is one), in lower-case hex; "" on failure.
