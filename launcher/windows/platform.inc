// Native Windows services; included after the shared application definitions.
#pragma pack(push, 1)
struct LauncherXInputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};
struct LauncherXInputState {
    DWORD dwPacketNumber;
    LauncherXInputGamepad Gamepad;
};
#pragma pack(pop)

typedef DWORD (WINAPI *PFN_LauncherXInputGetState)(DWORD dwUserIndex, void* pState);



void LaunchGameProcess() {
    App& a = g_app;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(a.python->exe) + L" " + a.python->args + L"-X utf8 " + Quote((a.root / L"run.py").wstring());
    const BOOL ok = CreateProcessW(a.python->exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, a.root.c_str(), &si, &pi);
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        AddLog("Could not start Python (error " + std::to_string(GetLastError()) + ")");
        a.exit_code = 1;
        a.show_exit = true;
        return;
    }
    // A job object, so "Stop" ends run.py and the game it started.
    a.job_object = CreateJobObjectW(nullptr, nullptr);
    if (a.job_object) AssignProcessToJobObject(a.job_object, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    a.process = pi.hProcess;
    a.game_running = true;
    a.show_exit = false;
    a.stopped = false;
    a.job = Job::Play;
    a.job_finished = false;
    AddLog("Starting the game...");
    a.worker = std::thread([read] {
        App& app = g_app;
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                AddLog(Trim(pending.substr(0, nl)));
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) AddLog(Trim(pending));
        CloseHandle(read);
        WaitForSingleObject(app.process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(app.process, &code);
        app.exit_code = code;
        app.job_finished = true;
    });
    if (a.hwnd) {
        if (a.launcher.Flag("hide_on_play", false)) ShowWindow(a.hwnd, SW_HIDE);
        else ShowWindow(a.hwnd, SW_MINIMIZE);
    }
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string DetectSystemLocale() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return "en";
    std::wstring locale(name);
    std::transform(locale.begin(), locale.end(), locale.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
    if (locale.rfind(L"pt", 0) == 0) return "pt_BR";
    if (locale.rfind(L"es", 0) == 0) return "es";
    if (locale.rfind(L"fr", 0) == 0) return "fr";
    if (locale.rfind(L"de", 0) == 0) return "de";
    if (locale.rfind(L"it", 0) == 0) return "it";
    if (locale.rfind(L"ja", 0) == 0) return "ja";
    if (locale.rfind(L"zh", 0) == 0) return "zh_CN";
    if (locale.rfind(L"ru", 0) == 0) return "ru";
    if (locale.rfind(L"id", 0) == 0) return "id";
    return "en";
}

fs::path ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

std::optional<Python> FindPython(const fs::path& root) {
    std::error_code ec;
    const fs::path bundled = root / L"python" / L"python.exe";
    if (fs::is_regular_file(bundled, ec)) return Python{bundled.wstring(), L"", "python\\ (bundled)"};
    wchar_t found[MAX_PATH * 4];
    if (SearchPathW(nullptr, L"py.exe", nullptr, DWORD(std::size(found)), found, nullptr)) {
        return Python{found, L"-3 ", Narrow(found)};
    }
    if (SearchPathW(nullptr, L"python.exe", nullptr, DWORD(std::size(found)), found, nullptr)) {
        // Windows' "App execution alias" opens the Microsoft Store instead of running Python.
        if (std::wstring(found).find(L"WindowsApps") == std::wstring::npos) return Python{found, L"", Narrow(found)};
    }
    const fs::path msys = L"C:\\msys64\\clang64\\bin\\python.exe";
    if (fs::is_regular_file(msys, ec)) return Python{msys.wstring(), L"", PathText(msys)};
    return std::nullopt;
}

Captured RunCaptured(const std::wstring& exe, const std::wstring& command_line, const fs::path& cwd,
                     DWORD timeout_ms) {
    Captured result;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return result;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = write;
    si.hStdError = write;
    // It inherits its output pipe and NUL, nothing else (v0.8.5): an inheritable handle of the
    // launcher's (the EOS bridge's stdin, whose end stops the bridge) kept by a helper, or by a
    // process the helper left running, would keep the bridge alive after the launcher closed.
    HANDLE inherit[2] = {write, nul};
    const DWORD inherit_count = nul != INVALID_HANDLE_VALUE ? 2 : 1;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    const bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                    inherit_count * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW six{};
    six.StartupInfo = si;
    six.StartupInfo.cb = sizeof six;
    six.lpAttributeList = attrs_ok ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command_line;
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &six.StartupInfo, &pi);
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    CloseHandle(write);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(read);
        return result;
    }
    result.started = true;
    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) result.output.append(buf, n);
    });
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    reader.join();
    GetExitCodeProcess(pi.hProcess, &result.code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read);
    return result;
}

void OpenInShell(const fs::path& target) {
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::vector<fs::path> PickFiles(LauncherWindow owner, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter) {
    std::vector<fs::path> result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return result;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dialog->SetTitle(title);
    const COMDLG_FILTERSPEC spec[] = {{filter_name, filter}};
    dialog->SetFileTypes(1, spec);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result.emplace_back(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            items->Release();
        }
    }
    dialog->Release();
    return result;
}

std::optional<fs::path> PickFolder(LauncherWindow owner, const wchar_t* title, const fs::path& start) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    std::error_code ec;
    if (!start.empty() && fs::is_directory(start, ec)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

fs::path BackupKeyPath() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    return appdata && *appdata ? fs::path(appdata) / L"CTR-NitroFueled-PC" / L"ryunet_online.key" : fs::path();
}

std::string Sha256Hex(const std::string& msg, const std::string& key) {
    unsigned char out[32];
    bool ok;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    key.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
        BCRYPT_HASH_HANDLE hash = nullptr;
        // No secret at all for a plain hash: a non-null pointer, even with length 0, makes
        // BCryptCreateHash fail (STATUS_INVALID_PARAMETER), and every request went unsigned.
        PUCHAR secret = key.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char*>(key.data()));
        if (BCryptCreateHash(alg, &hash, nullptr, 0, secret, ULONG(key.size()), 0) == 0) {
            ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(msg.data())), ULONG(msg.size()), 0) == 0 &&
                 BCryptFinishHash(hash, out, sizeof out, 0) == 0;
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (!ok) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (const unsigned char b : out) {
        hex += kHex[b >> 4];
        hex += kHex[b & 15];
    }
    return hex;
}

void NoteServerClock(HINTERNET req, const std::string& token) {
    long long server = 0;
    wchar_t stamp[128] = L"";
    DWORD stamp_size = sizeof stamp;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"X-RyuNET-Time", stamp, &stamp_size, WINHTTP_NO_HEADER_INDEX)) {
        g_server_stamps = true;
        long long at = 0;
        wchar_t echoed[70] = L"";
        if (token.empty() || swscanf(stamp, L"%lld %69ls", &at, echoed) != 2 || Widen(token) != echoed) return;
        server = at;
    } else {
        DWORD age_size = 0;
        if (g_server_stamps ||
            WinHttpQueryHeaders(req, WINHTTP_QUERY_AGE, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &age_size,
                                WINHTTP_NO_HEADER_INDEX) ||
            GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND)
            return;
        SYSTEMTIME date{};
        DWORD size = sizeof date;
        FILETIME file{};
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &date, &size, WINHTTP_NO_HEADER_INDEX) ||
            !SystemTimeToFileTime(&date, &file))
            return;
        const long long ticks = (static_cast<long long>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
        server = (ticks - 116444736000000000LL) / 10000000LL;  // 100 ns since 1601 -> Unix
    }
    const long long offset = server - static_cast<long long>(std::time(nullptr));
    // The header has whole seconds: a few seconds either way change nothing.
    g_server_clock_offset = offset > 5 || offset < -5 ? offset : 0;
}

std::string HttpRequest(const std::string& server, const wchar_t* method, const std::wstring& path,
                        const std::string& form, int timeout_ms, bool register_key,
                        const std::string& with_key) {
    std::string host;
    INTERNET_PORT port = 0;
    SplitServer(server, host, port);
    const std::string key_hash = ServerKeyHash(host, with_key);  // by the server's name, whatever the route
    // A network that does not reach the server: through the EOS bridge (EosBridgeRoute).
    std::string connect_host = host;
    INTERNET_PORT connect_port = port;
    if (EosBridgeRoute(server, port, connect_port, 15000)) connect_host = "127.0.0.1";
    else connect_port = port;
    std::string body;
    HINTERNET session = WinHttpOpen(L"CTR-NitroFueled-PC launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET conn = WinHttpConnect(session, Widen(connect_host).c_str(), connect_port, 0);
    for (int attempt = 1; conn && attempt <= 2; ++attempt) {
        const long long offset = g_server_clock_offset.load();
        const std::string proof = ProofHeader(key_hash, Narrow(method).c_str(), Narrow(path));
        HINTERNET req = WinHttpOpenRequest(conn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        const std::string token = ClockToken();
        std::wstring headers = ClockHeaders(token);
        if (!proof.empty()) headers += L"X-RyuNET-Proof: " + Widen(proof) + L"\r\n";
        if (register_key && !key_hash.empty()) headers += L"X-RyuNET-Key-Hash: " + Widen(key_hash) + L"\r\n";
        if (!form.empty()) headers += L"Content-Type: application/x-www-form-urlencoded\r\n";
        body.clear();
        bool answered;
        if (req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                      headers.empty() ? 0 : DWORD(-1),
                                      form.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(form.data()),
                                      DWORD(form.size()), DWORD(form.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            answered = true;
            NoteServerClock(req, token);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 4096) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
                body.append(chunk.data(), got);
            }
        }
        if (req) WinHttpCloseHandle(req);
        const long long moved = g_server_clock_offset.load() - offset;
        if (!answered || proof.empty() || (moved <= 240 && moved >= -240)) break;
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

void SetEnv(const wchar_t* name, const std::string& value) {
    SetEnvironmentVariableW(name, value.empty() ? nullptr : Widen(value).c_str());
}

void StopGame() {
    g_app.stopped = true;
    if (g_app.job_object) TerminateJobObject(g_app.job_object, 1);
    else if (g_app.process) TerminateProcess(g_app.process, 1);
}

void RegisterDiscordLaunch(const std::string& app_id) {
    if (app_id.empty() || app_id.find_first_not_of("0123456789") != std::string::npos) return;
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    const std::wstring path(exe, n);
    const std::wstring base = L"Software\\Classes\\discord-" + Widen(app_id);
    const auto set = [](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
            return;
        RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       DWORD((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    };
    set(base, nullptr, L"URL:Run game " + Widen(app_id) + L" protocol");
    set(base, L"URL Protocol", L"");
    set(base + L"\\DefaultIcon", nullptr, path + L",0");
    set(base + L"\\shell\\open\\command", nullptr, L"\"" + path + L"\"");
}

void BringLauncherToFront() {
    ShowWindow(g_app.hwnd, SW_RESTORE);
    SetForegroundWindow(g_app.hwnd);
}

std::string FileTimeText(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return {};
    FILETIME local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
    FileTimeToSystemTime(&local, &st);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::string Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void SetWindowsGpuPreference(int preference) {
    const std::wstring exe = (g_app.bin / L"sacredfire.exe").wstring();
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (preference) {
        const std::wstring value = L"GpuPreference=" + std::to_wstring(preference) + L";";
        RegSetValueExW(key, exe.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       DWORD((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, exe.c_str());
    }
    RegCloseKey(key);
}


const char* SdlKeyName(unsigned code) {
    static const std::map<unsigned, const char*> names = {
        {0x02, "1"}, {0x03, "2"}, {0x04, "3"}, {0x05, "4"}, {0x06, "5"}, {0x07, "6"}, {0x08, "7"},
        {0x09, "8"}, {0x0A, "9"}, {0x0B, "0"}, {0x0C, "-"}, {0x0D, "="}, {0x0E, "Backspace"},
        {0x0F, "Tab"}, {0x10, "Q"}, {0x11, "W"}, {0x12, "E"}, {0x13, "R"}, {0x14, "T"}, {0x15, "Y"},
        {0x16, "U"}, {0x17, "I"}, {0x18, "O"}, {0x19, "P"}, {0x1A, "["}, {0x1B, "]"},
        {0x1C, "Return"}, {0x1D, "Left Ctrl"}, {0x1E, "A"}, {0x1F, "S"}, {0x20, "D"}, {0x21, "F"},
        {0x22, "G"}, {0x23, "H"}, {0x24, "J"}, {0x25, "K"}, {0x26, "L"}, {0x27, ";"}, {0x28, "'"},
        {0x29, "`"}, {0x2A, "Left Shift"}, {0x2B, "\\"}, {0x2C, "Z"}, {0x2D, "X"}, {0x2E, "C"},
        {0x2F, "V"}, {0x30, "B"}, {0x31, "N"}, {0x32, "M"}, {0x33, ","}, {0x34, "."}, {0x35, "/"},
        {0x36, "Right Shift"}, {0x37, "Keypad *"}, {0x38, "Left Alt"}, {0x39, "Space"},
        {0x3A, "CapsLock"}, {0x3B, "F1"}, {0x3C, "F2"}, {0x3D, "F3"}, {0x3E, "F4"}, {0x3F, "F5"},
        {0x40, "F6"}, {0x41, "F7"}, {0x42, "F8"}, {0x43, "F9"}, {0x44, "F10"}, {0x57, "F11"},
        {0x58, "F12"}, {0x47, "Keypad 7"}, {0x48, "Keypad 8"}, {0x49, "Keypad 9"},
        {0x4A, "Keypad -"}, {0x4B, "Keypad 4"}, {0x4C, "Keypad 5"}, {0x4D, "Keypad 6"},
        {0x4E, "Keypad +"}, {0x4F, "Keypad 1"}, {0x50, "Keypad 2"}, {0x51, "Keypad 3"},
        {0x52, "Keypad 0"}, {0x53, "Keypad ."}, {0x56, "NonUSBackslash"},
        {0x11C, "Keypad Enter"}, {0x11D, "Right Ctrl"}, {0x135, "Keypad /"}, {0x138, "Right Alt"},
        {0x147, "Home"}, {0x148, "Up"}, {0x149, "PageUp"}, {0x14B, "Left"}, {0x14D, "Right"},
        {0x14F, "End"}, {0x150, "Down"}, {0x151, "PageDown"}, {0x153, "Delete"},
    };
    const auto it = names.find(code);
    return it == names.end() ? nullptr : it->second;
}

const char* PollGamepadButton() {
    static HMODULE s_xinput = nullptr;
    static PFN_LauncherXInputGetState s_getState = nullptr;
    static bool s_tried;
    if (!s_tried) {
        s_tried = true;
        s_xinput = LoadLibraryA("xinput1_4.dll");
        if (!s_xinput) s_xinput = LoadLibraryA("xinput1_3.dll");
        if (!s_xinput) s_xinput = LoadLibraryA("xinput9_1_0.dll");
        if (s_xinput) {
            s_getState = (PFN_LauncherXInputGetState)GetProcAddress(s_xinput, "XInputGetState");
        }
    }
    if (!s_getState || GetForegroundWindow() != g_app.hwnd) return nullptr;  // only while in front

    for (DWORD i = 0; i < 4; ++i) {
        LauncherXInputState state{};
        if (s_getState(i, &state) == 0) {
            if (state.Gamepad.bLeftTrigger > 80) return "lefttrigger";
            if (state.Gamepad.bRightTrigger > 80) return "righttrigger";
            if (state.Gamepad.wButtons & 0x1000) return "a";
            if (state.Gamepad.wButtons & 0x2000) return "b";
            if (state.Gamepad.wButtons & 0x4000) return "x";
            if (state.Gamepad.wButtons & 0x8000) return "y";
            if (state.Gamepad.wButtons & 0x0100) return "leftshoulder";
            if (state.Gamepad.wButtons & 0x0200) return "rightshoulder";
            if (state.Gamepad.wButtons & 0x0040) return "leftstick";
            if (state.Gamepad.wButtons & 0x0080) return "rightstick";
            if (state.Gamepad.wButtons & 0x0010) return "start";
            if (state.Gamepad.wButtons & 0x0020) return "back";
            if (state.Gamepad.wButtons & 0x0001) return "dpup";
            if (state.Gamepad.wButtons & 0x0002) return "dpdown";
            if (state.Gamepad.wButtons & 0x0004) return "dpleft";
            if (state.Gamepad.wButtons & 0x0008) return "dpright";
        }
    }
    return nullptr;
}

fs::path FindSteamPath() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"SteamPath", nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS) {
            RegCloseKey(key);
            std::wstring s = buf;
            std::replace(s.begin(), s.end(), L'/', L'\\');
            return fs::path(s);
        }
        RegCloseKey(key);
    }
    for (const auto& p : {L"C:\\Program Files (x86)\\Steam", L"C:\\Program Files\\Steam", L"D:\\Steam"}) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) return fs::path(p);
    }
    return {};
}

fs::path ShortcutTarget(const fs::path& link) {
    fs::path target;
    IShellLinkW* psl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl)))
        return target;
    IPersistFile* ppf = nullptr;
    if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
        wchar_t path[MAX_PATH];
        if (SUCCEEDED(ppf->Load(link.c_str(), STGM_READ)) &&
            SUCCEEDED(psl->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH)))
            target = path;
        ppf->Release();
    }
    psl->Release();
    return target;
}

fs::path LauncherExe() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(std::wstring(path, n));
}

bool CreateDesktopShortcut(std::string& out_error) {
    App& a = g_app;
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        out_error = T("No se pudo obtener la ruta del Escritorio.", "Could not get Desktop path.");
        return false;
    }
    const fs::path target_exe = LauncherExe();
    std::error_code ec;
    // A shortcut of that name may already open something else (another copy of the game, an
    // emulator): never replace it, use the "(PC)" name then.
    const auto ours = [&](const fs::path& link) {
        if (!fs::is_regular_file(link, ec)) return true;
        const fs::path t = ShortcutTarget(link);
        return t.parent_path() == a.root || t == target_exe;
    };
    fs::path link_path = fs::path(desktop) / (std::wstring(kLauncherName) + L".lnk");
    if (!ours(link_path)) link_path = fs::path(desktop) / (std::wstring(kLauncherName) + L" (PC).lnk");
    if (!ours(link_path)) {
        out_error = T("Ya hay accesos directos con ese nombre que abren otra cosa: no se ha cambiado nada.",
                      "Shortcuts with that name already open something else: nothing was changed.");
        return false;
    }

    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);
    if (FAILED(hr)) {
        out_error = T("Error inicializando IShellLink.", "Error initializing IShellLink.");
        return false;
    }
    psl->SetPath(target_exe.c_str());
    psl->SetArguments(L"--launch");
    psl->SetWorkingDirectory(a.root.c_str());
    psl->SetDescription(kLauncherName);
    psl->SetIconLocation(target_exe.c_str(), 0);  // the icon in the executable

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
    if (SUCCEEDED(hr)) {
        hr = ppf->Save(link_path.c_str(), TRUE);
        ppf->Release();
    }
    psl->Release();
    if (FAILED(hr)) {
        out_error = T("No se pudo guardar el acceso directo en el Escritorio.", "Could not save shortcut to Desktop.");
        return false;
    }
    // The shortcut of releases before v0.5.1, when it opened this folder's launcher.
    const fs::path old_link = fs::path(desktop) / kOldShortcut;
    if (fs::is_regular_file(old_link, ec) && ShortcutTarget(old_link).parent_path() == a.root) fs::remove(old_link, ec);
    return true;
}

void MigrateOldLauncher() {
    App& a = g_app;
    std::error_code ec;
    const fs::path self = LauncherExe();
    const fs::path old_exe = a.root / kOldLauncherExe;
    if (self.filename() == kOldLauncherExe) return;
    if (fs::is_regular_file(old_exe, ec) &&
        (fs::file_size(old_exe, ec) != fs::file_size(self, ec) || ReadText(old_exe) != ReadText(self)))
        fs::copy_file(self, old_exe, fs::copy_options::overwrite_existing, ec);
    // The desktop shortcut too when the old executable is already gone (deleted by hand).
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) return;
    const fs::path old_link = fs::path(desktop) / kOldShortcut;
    if (fs::is_regular_file(old_link, ec) && ShortcutTarget(old_link).parent_path() == a.root) {
        std::string error;
        CreateDesktopShortcut(error);
    }
}

bool AddToSteam(std::string& out_msg) {
    App& a = g_app;
    fs::path steam_path = FindSteamPath();
    if (steam_path.empty()) {
        out_msg = T("No se encontró la instalación de Steam en este equipo.", "Steam installation not found on this computer.");
        return false;
    }
    fs::path userdata = steam_path / L"userdata";
    std::error_code ec;
    if (!fs::is_directory(userdata, ec)) {
        out_msg = T("No se encontró la carpeta userdata de Steam.", "Steam userdata folder not found.");
        return false;
    }

    const auto steam_running = [] {
        bool running;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{sizeof pe};
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                        running = true;
                        break;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        return running;
    };
    const bool steam_was_running = steam_running();
    if (steam_was_running) {
        // Steam writes its own list when it exits: the file is changed only once it is gone.
        fs::path steam_exe = steam_path / L"steam.exe";
        if (fs::is_regular_file(steam_exe, ec))
            RunCaptured(steam_exe.wstring(), Quote(steam_exe.wstring()) + L" -shutdown", steam_path, 4000);
        for (int waited = 0; waited < 300 && steam_running(); ++waited) Sleep(100);
        if (steam_running()) {
            out_msg = T("Steam no se cerró a tiempo y no se cambió nada. Ciérralo (menú Steam > Salir) y vuelve a intentarlo.",
                        "Steam did not close in time and nothing was changed. Quit it (Steam menu > Exit) and try again.");
            return false;
        }
        Sleep(500);
    }

    fs::path assets_dir;
    for (const auto& candidate : {a.root / L"art" / L"steamgrid", a.root / L"steamgrid",
                                  a.root / L"packaging" / L"windows" / L"steamgrid"}) {
        if (fs::is_directory(candidate, ec)) {
            assets_dir = candidate;
            break;
        }
    }

    const fs::path target_exe = LauncherExe();

    const std::string app_name = Narrow(kLauncherName);
    const std::string exe_str = "\"" + PathText(target_exe) + "\"";
    const std::string start_dir_str = "\"" + PathText(a.root) + "\"";

    fs::path icon_path;
    if (fs::is_regular_file(a.root / L"launcher/io.github.sacredfire.Launcher.png", ec)) {
        icon_path = a.root / L"launcher/io.github.sacredfire.Launcher.png";
    } else if (fs::is_regular_file(a.root / L"packaging" / L"windows" / L"icons" / L"ctr.ico", ec)) {
        icon_path = a.root / L"packaging" / L"windows" / L"icons" / L"ctr.ico";
    } else {
        icon_path = target_exe;
    }

    const std::string key = exe_str + app_name;
    const uint32_t crc = Crc32(key.data(), key.size());
    const uint32_t appid = crc | 0x80000000;
    const std::string id32 = std::to_string(appid);
    const std::string id64 = std::to_string(((uint64_t)appid << 32) | 0x02000000);

    int profiles_updated = 0, profiles_skipped = 0;
    for (const auto& entry : fs::directory_iterator(userdata, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory()) continue;
        const std::string dirname = entry.path().filename().string();
        if (dirname.empty() || !std::all_of(dirname.begin(), dirname.end(), ::isdigit)) continue;

        fs::path config_dir = entry.path() / L"config";
        fs::path grid_dir = config_dir / L"grid";
        fs::create_directories(grid_dir, ec);
        const auto shared_icon = a.root / L"launcher/io.github.sacredfire.Launcher.png";
        if (fs::is_regular_file(shared_icon, ec)) {
            for (const auto& id : {id32, id64}) {
                fs::copy_file(shared_icon, grid_dir / TextPath(id + "_icon.png"), fs::copy_options::overwrite_existing, ec);
                fs::remove(grid_dir / TextPath(id + "_icon.jpg"), ec);
            }
        }

        if (!assets_dir.empty()) {
            const std::pair<const wchar_t*, const char*> mapping[] = {
                {L"portada.png", "p.png"},
                {L"fondo.png", "_hero.png"},
                {L"logo.png", "_logo.png"},
                {L"portada_ancha.png", ".png"}
            };
            for (const auto& [src_name, suffix] : mapping) {
                fs::path src = assets_dir / src_name;
                if (fs::is_regular_file(src, ec)) {
                    fs::copy_file(src, grid_dir / TextPath(id32 + suffix), fs::copy_options::overwrite_existing, ec);
                    fs::copy_file(src, grid_dir / TextPath(id64 + suffix), fs::copy_options::overwrite_existing, ec);
                }
            }
        }

        fs::path vdf_path = config_dir / L"shortcuts.vdf";
        std::vector<SteamShortcut> shortcuts;
        if (fs::is_regular_file(vdf_path, ec)) {
            std::ifstream f(vdf_path, std::ios::binary);
            if (!f) {
                ++profiles_skipped;
                continue;
            }
            std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            shortcuts = ParseShortcuts(buf);
            if (shortcuts.size() != CountShortcuts(buf)) {
                ++profiles_skipped;  // not read whole: writing it back would lose the player's other games
                continue;
            }
            // The player's other non-Steam games, as they were, in case anything goes wrong.
            fs::copy_file(vdf_path, config_dir / L"shortcuts.vdf.octr-backup", fs::copy_options::overwrite_existing, ec);
        }

        bool found;
        for (auto& sc : shortcuts) {
            if (sc.app_name == app_name || sc.exe.find(PathText(kOldLauncherExe)) != std::string::npos ||
                sc.exe.find(PathText(target_exe.filename())) != std::string::npos) {
                sc.appid = appid;
                sc.app_name = app_name;
                sc.exe = exe_str;
                sc.start_dir = start_dir_str;
                sc.icon = PathText(icon_path);
                sc.launch_options = "--launch";
                found = true;
                break;
            }
        }
        if (!found) {
            SteamShortcut sc;
            sc.appid = appid;
            sc.app_name = app_name;
            sc.exe = exe_str;
            sc.start_dir = start_dir_str;
            sc.icon = PathText(icon_path);
            sc.launch_options = "--launch";
            shortcuts.push_back(sc);
        }

        auto serialized = SerializeShortcuts(shortcuts);
        const fs::path temp = config_dir / L"shortcuts.vdf.octr-new";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (out) out.write(reinterpret_cast<const char*>(serialized.data()), serialized.size());
            if (!out) {
                fs::remove(temp, ec);
                ++profiles_skipped;
                continue;
            }
        }
        if (!MoveFileExW(temp.c_str(), vdf_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fs::remove(temp, ec);
            ++profiles_skipped;
            continue;
        }
        profiles_updated++;
    }

    if (profiles_updated > 0) {
        if (steam_was_running) {
            out_msg = T("¡Añadido a Steam con portadas con éxito!\nSteam se cerró automáticamente para aplicar los cambios; vuelve a abrirlo para ver el juego en tu biblioteca.",
                        "Successfully added to Steam with custom artwork!\nSteam was closed automatically to apply changes; reopen it to see the game in your library.");
        } else {
            out_msg = T("¡Añadido a Steam con portadas con éxito!\nAbre Steam para ver el juego en tu biblioteca.",
                        "Successfully added to Steam with custom artwork!\nOpen Steam to see the game in your library.");
        }
        return true;
    } else if (profiles_skipped > 0) {
        out_msg = T("No se pudo añadir a Steam: su lista de juegos no-Steam tiene un formato que el launcher no "
                    "reconoce, y no se tocó. Añádelo a mano: \"Añadir un juego que no es de Steam\", elige "
                    "este launcher y escribe --launch en Opciones de lanzamiento.",
                    "Could not add to Steam: its list of non-Steam games has a format the launcher does not "
                    "recognize, and it was left untouched. Add it by hand: \"Add a Non-Steam Game\", pick this "
                    "launcher and write --launch in Launch Options.");
        return false;
    } else {
        out_msg = T("No se encontró ningún perfil de usuario en Steam.", "No user profiles found in Steam.");
        return false;
    }
}

LauncherTexture* PngTexture(const std::string& png) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    LauncherTexture* view = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), DWORD(png.size()))) &&
        SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&w, &h)) && w && h && w <= 4096 && h <= 4096) {
        std::vector<uint32_t> pixels(size_t(w) * h);
        if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()))))
            view = MakeTexture(pixels.data(), int(w), int(h));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    wic->Release();
    return view;
}

bool AesCbc(const std::string& key, const unsigned char iv_in[16], const unsigned char* in, size_t size, unsigned char* out,
            bool encrypt) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool ok;
    unsigned char iv[16];
    std::memcpy(iv, iv_in, 16);
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                         sizeof(BCRYPT_CHAIN_MODE_CBC), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, (PUCHAR)key.data(), ULONG(key.size()), 0))) {
        ULONG done = 0;
        ok = BCRYPT_SUCCESS(encrypt ? BCryptEncrypt(handle, (PUCHAR)in, ULONG(size), nullptr, iv, 16, out, ULONG(size), &done, 0)
                                    : BCryptDecrypt(handle, (PUCHAR)in, ULONG(size), nullptr, iv, 16, out, ULONG(size), &done, 0)) &&
             done == size;
    }
    if (handle) BCryptDestroyKey(handle);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string ShadPs4TrophyKey() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (!appdata) return {};
    const std::string json = ReadText(fs::path(appdata) / L"shadPS4" / L"keys.json");
    const size_t at = json.find("\"ReleaseTrophyKey\"");
    if (at == std::string::npos) return {};
    const size_t colon = json.find(':', at);
    if (colon == std::string::npos) return {};
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i < json.size() && json[i] == '"') {  // a hex string
        const size_t end = json.find('"', i + 1);
        return end == std::string::npos ? std::string() : json.substr(i + 1, end - i - 1);
    }
    if (i < json.size() && json[i] == '[') {  // a byte array
        const size_t end = json.find(']', i);
        std::stringstream ss(json.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
        std::string hex, n;
        char buf[3];
        while (std::getline(ss, n, ',')) {
            n = Trim(n);
            if (n.empty()) continue;
            std::snprintf(buf, sizeof buf, "%02x", std::atoi(n.c_str()) & 0xff);
            hex += buf;
        }
        return hex;
    }
    return {};
}

bool RestartLauncher(const std::wstring& args) {
    App& a = g_app;
    wchar_t self[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    fs::path exe = self;
    std::error_code ec;
    const fs::path packaged = a.root / (std::wstring(kLauncherName) + L".exe");
    if (fs::is_regular_file(packaged, ec)) exe = packaged;
    std::wstring cmd = Quote(exe.wstring()) + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, a.root.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostQuitMessage(0);
    return true;
}

bool LoadArtworkFile(const fs::path& path, LauncherArtwork& artwork) {
    if (!g_device || !fs::is_regular_file(path)) return false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    std::vector<BYTE> pixels(size_t(w) * h * 4);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{pixels.data(), w * 4, 0};
        hr = g_device->CreateTexture2D(&td, &sd, &texture);
    }
    if (SUCCEEDED(hr)) hr = g_device->CreateShaderResourceView(texture, nullptr, &artwork.texture);
    if (texture) texture->Release(); if (converter) converter->Release(); if (frame) frame->Release();
    if (decoder) decoder->Release(); if (factory) factory->Release();
    if (FAILED(hr)) { artwork; return false; }
    artwork.width = w; artwork.height = h; return true;
}

std::string HttpsPostJson(const std::string& host, const std::wstring& path, const std::string& json,
                          uint16_t bridge_port) {
    std::string body;
    const std::string key_hash = ServerKeyHash(host);  // by the server's name, whatever the route
    HINTERNET session = WinHttpOpen(L"CTR-NitroFueled-PC launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 15000);
    HINTERNET conn = bridge_port ? WinHttpConnect(session, L"127.0.0.1", bridge_port, 0)
                                 : WinHttpConnect(session, Widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, bridge_port ? 0 : WINHTTP_FLAG_SECURE)
                         : nullptr;
    const std::string token = ClockToken();
    std::wstring headers = L"Content-Type: application/json\r\n" + ClockHeaders(token);
    const std::string proof = ProofHeader(key_hash, "POST", Narrow(path));
    if (!proof.empty()) headers += L"X-RyuNET-Proof: " + Widen(proof) + L"\r\nX-RyuNET-Key-Hash: " + Widen(key_hash) + L"\r\n";
    if (req && WinHttpSendRequest(req, headers.c_str(), DWORD(-1), const_cast<char*>(json.data()), DWORD(json.size()),
                                  DWORD(json.size()), 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        NoteServerClock(req, token);
        unsigned long avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 4096) {
            std::string chunk(avail, '\0');
            unsigned long got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}


bool OpenUrl(const std::string& url) {
    return INT_PTR(ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
constexpr UINT kTrayMessage = WM_APP + 20, kMinimizeToTray = WM_APP + 21, kToggleLauncher = WM_APP + 22;
bool LauncherVisible() { return g_app.hwnd && IsWindowVisible(g_app.hwnd) && !IsIconic(g_app.hwnd); }
void ToggleLauncher() { PostMessageW(g_app.hwnd, kToggleLauncher, 0, 0); }
NOTIFYICONDATAW g_tray{};
bool g_tray_added = false;
bool EnsureTray() {
    if (g_tray_added) return true;
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_app.hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    if (!g_tray.hIcon) g_tray.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    lstrcpynW(g_tray.szTip, kLauncherName, int(std::size(g_tray.szTip)));
    g_tray_added = Shell_NotifyIconW(NIM_ADD, &g_tray) != FALSE;
    return g_tray_added;
}
void DestroyTray() {
    if (g_tray_added) Shell_NotifyIconW(NIM_DELETE, &g_tray);
    if (g_tray.hIcon) DestroyIcon(g_tray.hIcon);
    g_tray = {};
    g_tray_added = false;
}
void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, Widen(TrayLauncherLabel()).c_str());
    AppendMenuW(menu, MF_STRING | ((!g_app.game_running && !TrayCanStart()) ? MF_GRAYED : 0), 2,
        Widen(g_app.game_running ? T("Cerrar el juego", "Quit game") : T("Iniciar juego", "Start game")).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, Widen(T("Salir del launcher", "Exit launcher")).c_str());
    POINT point; GetCursorPos(&point);
    SetForegroundWindow(g_app.hwnd);
    const int command = int(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        point.x, point.y, 0, g_app.hwnd, nullptr));
    DestroyMenu(menu);
    PostMessageW(g_app.hwnd, WM_NULL, 0, 0);
    TrayAction(command);
}
void HideLauncher(bool) { PostMessageW(g_app.hwnd, kMinimizeToTray, 0, 0); }
void ToggleMaximize() { ShowWindow(g_app.hwnd, IsZoomed(g_app.hwnd) ? SW_RESTORE : SW_MAXIMIZE); }
void RequestClose() { SendMessageW(g_app.hwnd, WM_CLOSE, 0, 0); }
void QuitLauncher() { PostQuitMessage(0); }
void ReleaseGameProcess() {
    CloseHandle(g_app.process);
    g_app.process = nullptr;
    if (g_app.job_object) CloseHandle(g_app.job_object);
    g_app.job_object = nullptr;
}
fs::path DataRoot(const fs::path& root) { return root; }
void PrepareData() {}
void SetPlatformGameEnv() {}
void BeginGameEnv() {}
std::vector<fs::path> PlatformFonts() {
    wchar_t fonts[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(fonts, MAX_PATH);
    const fs::path dir = fs::path(std::wstring(fonts, n)) / L"Fonts";
    return {dir / L"segoeui.ttf", dir / L"msyh.ttc", dir / L"YuGothM.ttc", dir / L"msgothic.ttc"};
}

void PlatformSettings() {}

void ApplyPlatformCapabilities() {}
