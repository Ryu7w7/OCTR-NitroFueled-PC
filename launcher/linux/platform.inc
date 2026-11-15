// SPDX-License-Identifier: GPL-2.0-or-later
// Linux services behind the shared application. No Win32 emulation or shell evaluation.
SDL_Renderer* g_renderer = nullptr;
bool g_quit = false;
std::atomic<uint64_t> g_stop_requested{0};

std::wstring Widen(const std::string& s) { return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(s); }
std::string Narrow(const std::wstring& s) { return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(s); }
std::string DetectSystemLocale() {
    std::string locale = Env("LC_ALL");
    if (locale.empty()) locale = Env("LC_MESSAGES");
    if (locale.empty()) locale = Env("LANG");
    locale = Lower(locale);
    if (locale.rfind("pt", 0) == 0) return "pt_BR";
    if (locale.rfind("zh", 0) == 0) return "zh_CN";
    for (const char* lang : {"es", "fr", "de", "it", "ja", "ru", "id"})
        if (locale.rfind(lang, 0) == 0) return lang;
    return "en";
}
fs::path ExeDir() { return fs::read_symlink("/proc/self/exe").parent_path(); }
std::optional<Python> FindPython(const fs::path&) {
    const auto python = Env("PYTHON");
    const auto executable = python.empty() ? "python3" : python;
    if (LinuxProcess::Run({executable, "--version"}, "", 2000).code != 0) return std::nullopt;
    return Python{Widen(executable), L"", "Python 3"};
}
Captured RunCaptured(const std::wstring& exe, const std::wstring& command_line, const fs::path& cwd, DWORD timeout_ms) {
    auto args = LinuxProcess::Arguments(Narrow(command_line));
    if (args.empty()) return {};
    args[0] = Narrow(exe);
    const auto result = LinuxProcess::Run(args, PathText(cwd), unsigned(timeout_ms));
    return {result.started, static_cast<unsigned long>(result.code), result.output};
}
std::string RunService(const std::string& request, unsigned timeout_ms, const std::atomic<bool>* cancel) {
    const auto& a = g_app;
    const auto result = LinuxProcess::Run({Narrow(a.python ? a.python->exe : L"python3"),
        PathText(a.root / "launcher/linux/services.py")}, PathText(a.root), timeout_ms, request, cancel);
    return result.started && result.code == 0 ? Trim(result.output) : "{\"error\":\"Linux service failed\"}";
}
std::string ServiceRequest(const char* operation) {
    return "{\"op\":\"" + std::string(operation) + "\",\"data\":\"" + JsonEscape(PathText(g_app.data)) + "\"}";
}
bool OpenUrl(const std::string& url) { return SDL_OpenURL(url.c_str()); }
void OpenInShell(const fs::path& target) {
    const std::string value = PathText(target);
    if (value.find("://") != std::string::npos) { OpenUrl(value); return; }
    const auto child = LinuxProcess::Start({"xdg-open", value}, "", {}, -1, false);
    if (!child.pid) return;
    if (child.output >= 0) close(child.output);
    std::thread([pid = child.pid] { int status; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }).detach();
}

struct FileChoice {
    std::mutex mutex;
    bool done = false;
    std::vector<fs::path> files;
};
void SDLCALL FilesChosen(void* userdata, const char* const* files, int) {
    auto& choice = *static_cast<FileChoice*>(userdata);
    std::scoped_lock lock(choice.mutex);
    if (files) for (; *files; ++files) choice.files.emplace_back(*files);
    choice.done = true;
}
std::vector<fs::path> WaitForChoice(FileChoice& choice) {
    for (;;) {
        { std::scoped_lock lock(choice.mutex); if (choice.done) return choice.files; }
        SDL_PumpEvents();
        SDL_Delay(10);
    }
}
std::vector<fs::path> PickFiles(LauncherWindow owner, const wchar_t*, const wchar_t* filter_name, const wchar_t* filter) {
    FileChoice choice;
    std::string name = Narrow(filter_name), pattern = Narrow(filter);
    for (size_t at; (at = pattern.find("*.")) != std::string::npos;) pattern.erase(at, 2);
    if (pattern == "*.*") pattern = "*";
    SDL_DialogFileFilter spec{name.c_str(), pattern.c_str()};
    SDL_ShowOpenFileDialog(FilesChosen, &choice, owner, &spec, 1, nullptr, true);
    return WaitForChoice(choice);
}
std::optional<fs::path> PickFolder(LauncherWindow owner, const wchar_t*, const fs::path& start) {
    FileChoice choice;
    const std::string initial = PathText(start);
    SDL_ShowOpenFolderDialog(FilesChosen, &choice, owner, initial.empty() ? nullptr : initial.c_str(), false);
    const auto files = WaitForChoice(choice);
    return files.empty() ? std::nullopt : std::optional<fs::path>(files.front());
}
fs::path BackupKeyPath() {
    const auto data = Env("XDG_DATA_HOME"), home = Env("HOME");
    if (data.empty() && home.empty()) return {};
    return (data.empty() ? TextPath(home) / ".local/share" : TextPath(data)) / "CTR-NitroFueled-PC/ryunet_online.key";
}
std::string Sha256Hex(const std::string& msg, const std::string& key) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (key.empty()) {
        if (EVP_Digest(msg.data(), msg.size(), digest, &size, EVP_sha256(), nullptr) != 1) return {};
    } else if (!HMAC(EVP_sha256(), key.data(), int(key.size()), reinterpret_cast<const unsigned char*>(msg.data()),
                     msg.size(), digest, &size)) return {};
    std::string hex;
    for (unsigned i = 0; i < size; ++i) { hex += "0123456789abcdef"[digest[i] >> 4]; hex += "0123456789abcdef"[digest[i] & 15]; }
    return hex;
}
std::string HttpRequest(const std::string& server, const wchar_t* method, const std::wstring& path,
                        const std::string& form, int timeout_ms, bool register_key, const std::string& with_key) {
    std::string bridge, ip, peers;
    EosBridgeGameEnv(server, bridge, ip, peers, timeout_ms);
    const auto answer = RunService("{\"op\":\"http\",\"data\":\"" + JsonEscape(PathText(g_app.data)) +
        "\",\"bridge_map\":\"" + JsonEscape(bridge) +
        "\",\"server\":\"" + JsonEscape(server) + "\",\"method\":\"" + Narrow(method) +
        "\",\"path\":\"" + JsonEscape(Narrow(path)) + "\",\"form\":\"" + JsonEscape(form) +
        "\",\"key\":\"" + JsonEscape(with_key) + "\",\"register\":" + (register_key ? "true" : "false") +
        ",\"timeout\":" + std::to_string(timeout_ms) + ",\"clock_offset\":" + std::to_string(g_server_clock_offset.load()) +
        ",\"clock_stamps\":" + (g_server_stamps ? "true" : "false") + "}", timeout_ms * 2 + 3000);
    int offset = 0;
    if (JsonNumber(answer, "clock_offset", offset)) g_server_clock_offset = offset;
    if (JsonBool(answer, "clock_stamps")) g_server_stamps = true;
    return answer;
}
std::string HttpsPostJson(const std::string& host, const std::wstring&, const std::string& json,
                          uint16_t) {
    return RunService("{\"op\":\"password\",\"data\":\"" + JsonEscape(PathText(g_app.data)) +
        "\",\"server\":\"" + JsonEscape(host) + "\",\"body\":\"" + JsonEscape(json) + "\",\"clock_offset\":" + std::to_string(g_server_clock_offset.load()) + "}");
}
std::map<std::string, std::string> g_game_env;
void BeginGameEnv() { g_game_env.clear(); }
void SetEnv(const wchar_t* name, const std::string& value) { g_game_env[Narrow(name)] = value; }
void SetPlatformGameEnv() {
    SetEnv(L"BB_DATA_DIR", PathText(g_app.data));
    SetEnv(L"BB_CONFIG", PathText(GameIni()));
    SetEnv(L"BB_USER_DIR", PathText(UserRoot()));
    for (auto [key, env] : {std::pair{"clear_shader_cache", L"BB_CLEAR_SHADER_CACHE"},
            {"frame_stats", L"BB_FRAME_STATS"}, {"mangohud", L"MANGOHUD"}, {"vk_validation", L"BB_VK_VALIDATION"}})
        SetEnv(env, g_app.launcher.Flag(key, false) ? "1" : "");
    SetEnv(L"BB_GPU_PREFERENCE", g_app.launcher.Get("gpu").empty() ? g_app.launcher.Get("gpu_preference") : "");
    std::istringstream extra(g_app.launcher.Get("extra_env"));
    for (std::string item; extra >> item;) {
        const auto at = item.find('=');
        if (at != std::string::npos && at > 0) SetEnv(Widen(item.substr(0, at)).c_str(), item.substr(at + 1));
    }
}
void LaunchGameProcess() {
    App& a = g_app;
    std::vector<std::string> environment;
    for (char** item = environ; *item; ++item) {
        const std::string value = *item;
        if (!g_game_env.count(value.substr(0, value.find('=')))) environment.push_back(value);
    }
    for (const auto& [key, value] : g_game_env)
        if (!value.empty()) environment.push_back(key + "=" + value);
    std::vector<char*> env;
    for (auto& value : environment) env.push_back(value.data());
    env.push_back(nullptr);
    auto child = LinuxProcess::Start({"bash", PathText(a.root / "run.sh")}, PathText(a.root), {}, -1, true, env.data());
    if (!child.pid) { a.notice = "Could not start run.sh: " + LastPlatformError(); return; }
    g_stop_requested = 0;
    a.process = child.pid;
    a.game_running = true;
    a.show_exit = false;
    a.stopped = false;
    a.job = Job::Play;
    a.job_finished = false;
    AddLog("Starting the game...");
    a.worker = std::thread([child] {
        fcntl(child.output, F_SETFL, O_NONBLOCK);
        std::string pending;
        int status = 0;
        bool exited = false, eof = false;
        while (!exited || !eof) {
            char buffer[4096];
            ssize_t n;
            while ((n = read(child.output, buffer, sizeof buffer)) > 0) {
                pending.append(buffer, size_t(n));
                for (size_t at; (at = pending.find('\n')) != std::string::npos;) {
                    AddLog(Trim(pending.substr(0, at)));
                    pending.erase(0, at + 1);
                }
            }
            eof = n == 0;
            if (!exited) exited = waitpid(child.pid, &status, WNOHANG) == child.pid;
            if (exited && eof) break;
            if (const auto stopped = g_stop_requested.load(); stopped && Ticks() - stopped >= 2000)
                kill(-child.pid, SIGKILL);
            pollfd fd{child.output, POLLIN, 0};
            poll(&fd, 1, 50);
        }
        if (!pending.empty()) AddLog(Trim(pending));
        close(child.output);
        g_app.exit_code = LinuxProcess::ExitCode(status);
        g_app.job_finished = true;
    });
    if (a.hwnd) HideLauncher(a.launcher.Flag("hide_on_play", false));
}
void StopGame() {
    g_app.stopped = true;
    if (g_app.process) { g_stop_requested = Ticks(); kill(-g_app.process, SIGTERM); }
}
void ReleaseGameProcess() { g_app.process = 0; }
void RegisterDiscordLaunch(const std::string&) {} // Discord IPC works without Windows' URL-protocol registration.
void BringLauncherToFront() {
    if (!g_app.hwnd) return;
    SDL_Event event{}; event.type = SDL_EVENT_USER; event.user.code = 3; SDL_PushEvent(&event);
}
bool LauncherVisible() {
    return g_app.hwnd && !(SDL_GetWindowFlags(g_app.hwnd) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
}
void ToggleLauncher() {
    SDL_Event event{}; event.type = SDL_EVENT_USER; event.user.code = 4; SDL_PushEvent(&event);
}
void HideLauncher(bool hidden) {
    // SDL window operations run on the UI thread, including shader-progress notifications.
    SDL_Event event{}; event.type = SDL_EVENT_USER; event.user.code = hidden ? 1 : 2; SDL_PushEvent(&event);
}
void ToggleMaximize() {
    if (SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_app.hwnd);
    else SDL_MaximizeWindow(g_app.hwnd);
}
void RequestClose() { if (g_app.game_running) g_app.want_close = true; else QuitLauncher(); }
void QuitLauncher() { g_quit = true; }
std::string FileTimeText(const fs::path& path) {
    std::error_code ec;
    const auto at = fs::last_write_time(path, ec);
    if (ec) return {};
    const auto time = std::chrono::system_clock::to_time_t(fs::file_time_type::clock::to_sys(at));
    std::tm local{}; LocalTime(&local, &time);
    char text[64]; std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local); return text;
}
std::string Stamp() {
    const auto now = std::time(nullptr); std::tm local{}; LocalTime(&local, &now);
    char text[64]; std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local); return text;
}
void SetWindowsGpuPreference(int) {}
const char* SdlKeyName(unsigned code) { const auto* name = SDL_GetScancodeName(SDL_Scancode(code)); return *name ? name : nullptr; }
const char* PollGamepadButton() {
    if (!(SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_INPUT_FOCUS)) return nullptr;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    const char* pressed = nullptr;
    for (int i = 0; i < count && !pressed; ++i) {
        SDL_Gamepad* pad = SDL_OpenGamepad(ids[i]);
        if (!pad) continue;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 10000) pressed = "lefttrigger";
        else if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 10000) pressed = "righttrigger";
        static constexpr const char* names[] = {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
            "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
        for (int b = 0; b < int(std::size(names)) && !pressed; ++b)
            if (SDL_GetGamepadButton(pad, SDL_GamepadButton(b))) pressed = names[b];
        SDL_CloseGamepad(pad);
    }
    SDL_free(ids);
    return pressed;
}
bool CreateDesktopShortcut(std::string& error) {
    const auto answer = RunService(ServiceRequest("desktop")); error = JsonText(answer, "error"); return JsonBool(answer, "ok");
}
bool AddToSteam(std::string& message) {
    const auto answer = RunService(ServiceRequest("steam"));
    message = JsonText(answer, JsonBool(answer, "ok") ? "message" : "error"); return JsonBool(answer, "ok");
}
void MigrateOldLauncher() {}

SDL_Surface* PngSurface(const std::string& data) {
    png_image image{}; image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data.data(), data.size())) return nullptr;
    image.format = PNG_FORMAT_RGBA;
    if (!image.width || !image.height || image.width > 4096 || image.height > 4096) { png_image_free(&image); return nullptr; }
    SDL_Surface* surface = SDL_CreateSurface(int(image.width), int(image.height), SDL_PIXELFORMAT_RGBA32);
    if (surface && !png_image_finish_read(&image, nullptr, surface->pixels, surface->pitch, nullptr)) {
        SDL_DestroySurface(surface); surface = nullptr;
    }
    png_image_free(&image);
    return surface;
}
LauncherTexture* PngTexture(const std::string& data) {
    SDL_Surface* surface = PngSurface(data);
    if (!surface) return nullptr;
    LauncherTexture* texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
}
bool LoadArtworkFile(const fs::path& path, LauncherArtwork& artwork) {
    artwork.texture = PngTexture(ReadText(path));
    if (!artwork.texture) return false;
    float width, height; SDL_GetTextureSize(artwork.texture, &width, &height);
    artwork.width = unsigned(width); artwork.height = unsigned(height); return true;
}
bool AesCbc(const std::string& key, const unsigned char iv[16], const unsigned char* in, size_t size, unsigned char* out, bool encrypt) {
    if (key.size() != 16 || size % 16 || size > INT_MAX) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int written = 0, last = 0;
    const bool ok = EVP_CipherInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
        reinterpret_cast<const unsigned char*>(key.data()), iv, encrypt) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 && EVP_CipherUpdate(ctx, out, &written, in, int(size)) == 1 &&
        EVP_CipherFinal_ex(ctx, out + written, &last) == 1 && size_t(written + last) == size;
    EVP_CIPHER_CTX_free(ctx); return ok;
}
std::string ShadPs4TrophyKey() {
    return JsonText(RunService(ServiceRequest("trophy_key")), "key");
}
AppIndicator* g_tray = nullptr;
GMenu* g_tray_menu = nullptr;
GSimpleActionGroup* g_tray_actions = nullptr;
GSimpleAction *g_tray_open = nullptr, *g_tray_game = nullptr, *g_tray_exit = nullptr;
std::array<std::string, 3> g_tray_labels;
bool g_tray_connected = false;
DbusmenuServer* g_tray_dbusmenu = nullptr;
std::array<DbusmenuMenuitem*, 3> g_tray_items{};
void UpdateTray() {
    while (g_main_context_iteration(nullptr, false)) {}
    if (!g_tray) return;
    const std::array<std::string, 3> labels{TrayLauncherLabel(),
        g_app.game_running ? T("Cerrar el juego", "Quit game") : T("Iniciar juego", "Start game"),
        T("Salir del launcher", "Exit launcher")};
    if (labels != g_tray_labels) {
        g_menu_remove_all(g_tray_menu);
        const char* actions[] = {"indicator.open", "indicator.game", "indicator.exit"};
        for (size_t i = 0; i < labels.size(); ++i) {
            g_menu_append(g_tray_menu, labels[i].c_str(), actions[i]);
            dbusmenu_menuitem_property_set(g_tray_items[i], DBUSMENU_MENUITEM_PROP_LABEL, labels[i].c_str());
        }
        g_tray_labels = labels;
    }
    g_simple_action_set_enabled(g_tray_game, g_app.game_running || TrayCanStart());
    dbusmenu_menuitem_property_set_bool(g_tray_items[1], DBUSMENU_MENUITEM_PROP_ENABLED,
                                      g_action_get_enabled(G_ACTION(g_tray_game)));
}
void DestroyTray() {
    g_tray_connected = false;
    if (g_tray) app_indicator_set_status(g_tray, APP_INDICATOR_STATUS_PASSIVE);
    g_clear_object(&g_tray);
    g_clear_object(&g_tray_dbusmenu);
    for (auto*& item : g_tray_items) g_clear_object(&item);
    g_clear_object(&g_tray_menu);
    g_clear_object(&g_tray_actions);
    g_clear_object(&g_tray_open);
    g_clear_object(&g_tray_game);
    g_clear_object(&g_tray_exit);
    g_tray_labels = {};
}
bool EnsureTray() {
    if (g_tray) return g_tray_connected;
    // Keep the taskbar entry on desktops without a StatusNotifier host.
    const auto host = LinuxProcess::Run({"gdbus", "call", "--session", "--dest", "org.kde.StatusNotifierWatcher",
        "--object-path", "/StatusNotifierWatcher", "--method", "org.freedesktop.DBus.Properties.Get",
        "org.kde.StatusNotifierWatcher", "IsStatusNotifierHostRegistered"}, "", 1000);
    if (host.code != 0 || host.output.find("true") == std::string::npos) return false;
    const auto icon = g_app.root / "launcher/io.github.sacredfire.Launcher.png";
    std::error_code error;
    if (!fs::is_regular_file(icon, error)) return false;
    const std::string id = "sacredfire_" + std::to_string(getpid());
    g_tray = app_indicator_new(id.c_str(),
                              PathText(icon).c_str(), APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!g_tray) return false;
    g_signal_connect(g_tray, "activate", G_CALLBACK(+[](AppIndicator*, gint, gint, gpointer) {
        TrayAction(1);
    }), nullptr);
    g_signal_connect(g_tray, "connection-changed", G_CALLBACK(+[](AppIndicator* tray, gboolean connected, gpointer) {
        if (tray != g_tray) return;
        if (g_tray_connected && !connected) BringLauncherToFront();
        g_tray_connected = connected;
        if (connected && g_app.hwnd && (SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_MINIMIZED))
            SDL_HideWindow(g_app.hwnd);
    }), nullptr);
    g_tray_menu = g_menu_new();
    g_tray_actions = g_simple_action_group_new();
    g_tray_open = g_simple_action_new("open", nullptr);
    g_tray_game = g_simple_action_new("game", nullptr);
    g_tray_exit = g_simple_action_new("exit", nullptr);
    // KDE and older hosts need DBusMenu on the pinned Ayatana library's menu path.
    g_tray_dbusmenu = dbusmenu_server_new(("/org/ayatana/appindicator/" + id).c_str());
    auto* menu_root = dbusmenu_menuitem_new();
    int command = 0;
    for (auto* action : {g_tray_open, g_tray_game, g_tray_exit}) {
        g_action_map_add_action(G_ACTION_MAP(g_tray_actions), G_ACTION(action));
        g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
            TrayAction(GPOINTER_TO_INT(data));
        }), GINT_TO_POINTER(++command));
        auto* item = g_tray_items[command - 1] = dbusmenu_menuitem_new_with_id(command);
        dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_VISIBLE, true);
        dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_ENABLED, true);
        g_signal_connect(item, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
            G_CALLBACK(+[](DbusmenuMenuitem*, guint, gpointer data) {
                g_action_activate(G_ACTION(data), nullptr);
            }), action);
        dbusmenu_menuitem_child_append(menu_root, item);
    }
    dbusmenu_server_set_root(g_tray_dbusmenu, menu_root);
    g_object_unref(menu_root);
    app_indicator_set_menu(g_tray, g_tray_menu);
    app_indicator_set_actions(g_tray, g_tray_actions);
    app_indicator_set_title(g_tray, Narrow(kLauncherName).c_str());
    app_indicator_set_secondary_activate_target(g_tray, "open");
    UpdateTray();
    app_indicator_set_status(g_tray, APP_INDICATOR_STATUS_ACTIVE);
    return g_tray_connected;
}

bool RestartLauncher(const std::wstring& args) {
    const auto executable = Env("APPIMAGE").empty() ? PathText(fs::read_symlink("/proc/self/exe")) : Env("APPIMAGE");
    std::string arguments;
    for (const auto& arg : LinuxProcess::Arguments(Narrow(args)))
        arguments += (arguments.empty() ? "" : ",") + std::string("\"") + JsonEscape(arg) + "\"";
    const auto answer = RunService("{\"op\":\"restart\",\"executable\":\"" + JsonEscape(executable) +
                                   "\",\"args\":[" + arguments + "]}");
    if (!JsonBool(answer, "ok")) { g_app.notice = JsonText(answer, "error"); return false; }
    QuitLauncher(); return true;
}
fs::path DataRoot(const fs::path& root) {
    const auto override = Env("BB_DATA_DIR");
    if (!override.empty()) return fs::absolute(TextPath(override));
    const auto data = Env("XDG_DATA_HOME"), home = Env("HOME");
    if (!data.empty() || !home.empty()) {
        const fs::path previous = (data.empty() ? TextPath(home) / ".local/share" : TextPath(data)) / "sacredfire";
        if (SettingsImport::IsUsedInstall(previous)) return previous;
    }
    return root;
}
void PrepareData() {
    if (!g_app.python) throw std::runtime_error("Python 3 is required to migrate settings and run the game.");
    const auto result = RunService(ServiceRequest("migrate"));
    const auto error = JsonText(result, "error");
    if (!error.empty()) throw std::runtime_error("Settings migration failed; original files were kept: " + error);
}
std::vector<fs::path> PlatformFonts() {
    std::vector<fs::path> files;
    const auto result = LinuxProcess::Run({"fc-match", "--format=%{file}\\n", "sans:lang=en"}, "", 2000);
    if (result.code == 0) files.push_back(TextPath(Trim(result.output)));
    for (const auto* lang : {"ja", "zh", "ru"}) {
        const auto fallback = LinuxProcess::Run({"fc-match", "--format=%{file}\\n", std::string("sans:lang=") + lang}, "", 2000);
        const auto path = TextPath(Trim(fallback.output));
        if (fallback.code == 0 && std::find(files.begin(), files.end(), path) == files.end()) files.push_back(path);
    }
    return files;
}

#include "discord.inc"

void PlatformSettings() {
    ImGui::SeparatorText("Linux");
    LauncherFlag("mangohud", "MangoHud", false);
    LauncherFlag("vk_validation", "Vulkan validation", false);
    LauncherFlag("frame_stats", "Frame statistics", false);
    LauncherFlag("clear_shader_cache", "Clear shader cache at start", false);
    static char extra[1024]{};
    if (!ImGui::IsAnyItemActive()) std::snprintf(extra, sizeof extra, "%s", g_app.launcher.Get("extra_env").c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##linux_environment", "Extra environment (KEY=value)", extra, sizeof extra)) {
        g_app.launcher.Set("extra_env", extra); SaveLauncher();
    }
    HelpMarker("Extra environment: KEY=value pairs separated by spaces, as in the GTK launcher.");
    ImGui::TextWrapped("Saves: %s", PathText(UserRoot()).c_str());
    if (ImGui::Button("Choose save folder")) {
        if (const auto folder = PickFolder(g_app.hwnd, L"Save folder", UserRoot())) {
            g_app.launcher.Set("user_dir", PathText(*folder)); SaveLauncher(); g_app.saves_dirty = true;
        }
    }
    ImGui::TextDisabled("Direct3D 11 requires Windows. Linux uses Vulkan.");
}

void ApplyPlatformCapabilities() {
    if (g_app.game.Get("renderer") == "d3d11") {
        g_app.game.Set("renderer", "shadps4");
        g_app.notice = "Direct3D 11 is Windows only. Vulkan is selected on Linux.";
    }
}
