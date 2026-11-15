// Render every shared page with isolated data and no external service account.
#define main LauncherApplicationMain
#include "../launcher/launcher.cpp"
#undef main
#include <cassert>
#include "imgui_internal.h"
int main(int argc, char** argv) {
    static_assert(alignof(Trophy) >= alignof(void*));
    static_assert(alignof(LinuxUpdate) >= alignof(std::mutex));
    assert(argc == 3);
    g_app.root = fs::absolute(argv[1]);
    g_app.data = fs::absolute(argv[2]);
    fs::create_directories(g_app.data);
    setenv("XDG_DATA_HOME", PathText(g_app.data / "xdg-data").c_str(), 1);
    setenv("XDG_CONFIG_HOME", PathText(g_app.data / "xdg-config").c_str(), 1);
    const auto data_override = Env("BB_DATA_DIR");
    unsetenv("BB_DATA_DIR");
    const fs::path previous = g_app.data / "xdg-data/sacredfire";
    fs::remove_all(previous);
    assert(DataRoot(g_app.root) == g_app.root);
    fs::create_directories(previous);
    std::ofstream(previous / "bbport.ini") << "volume=42\n";
    assert(DataRoot(g_app.root) == previous);
    setenv("BB_DATA_DIR", PathText(g_app.data).c_str(), 1);
    assert(DataRoot(g_app.root) == g_app.data);
    if (data_override.empty()) unsetenv("BB_DATA_DIR");
    else setenv("BB_DATA_DIR", data_override.c_str(), 1);
    Ini legacy;
    for (const char* value : {"0", "no", "No", "false", "FALSE", "off"}) {
        legacy.Set("flag", value);
        assert(!legacy.Flag("flag", true));
    }
    for (const char* value : {"1", "yes", "Yes", "true", "TRUE", "on"}) {
        legacy.Set("flag", value);
        assert(legacy.Flag("flag", false));
    }
    assert(legacy.Flag("missing", true) && !legacy.Flag("missing", false));
    const auto sync_path = g_app.data / "sync.ini";
    std::ofstream(sync_path) << "# Keep this comment\nvolume=20\ntemporal=off\nrender_res=1080\nkey.old=Q\n";
    Ini launcher_settings, game_settings;
    assert(launcher_settings.Load(sync_path) && game_settings.Load(sync_path));
    game_settings.Set("volume", "42");
    game_settings.Set("temporal", "fsr1");
    game_settings.Set("render_res", "1440");
    game_settings.Set("effect_ssao", "0");
    assert(game_settings.Save(sync_path, nullptr));
    launcher_settings.Set("key.cross", "W");
    launcher_settings.Erase("key.old");
    assert(launcher_settings.Save(sync_path, nullptr));
    assert(game_settings.Load(sync_path));
    assert(game_settings.Get("volume") == "42" && game_settings.Get("temporal") == "fsr1");
    assert(game_settings.Get("render_res") == "1440" && game_settings.Get("effect_ssao") == "0");
    assert(game_settings.Get("key.cross") == "W" && !game_settings.values.count("key.old"));
    assert(ReadText(sync_path).find("# Keep this comment") != std::string::npos);
    game_settings.Set("volume", "51");
    game_settings.Erase("effect_ssao");
    assert(game_settings.Save(sync_path, nullptr));
    assert(launcher_settings.Refresh(sync_path));
    assert(launcher_settings.Get("volume") == "51" && !launcher_settings.values.count("effect_ssao"));
    {
        // Keep destructive import regression checks inside this test's temporary data directory.
        unsetenv("BB_USER_DIR");
        const auto actual_data = g_app.data;
        g_app.data /= "save-import-test";
        fs::remove_all(g_app.data);
        const auto root = SaveRoot();
        const auto source = g_app.data / "source";
        const auto other = root.parent_path() / "OTHER_TITLE";
        fs::create_directories(source / "sce_sys");
        fs::create_directories(root / "KEEP");
        fs::create_directories(other);
        for (const auto& dir : {root / "KEEP", other}) {
            std::ofstream(dir / "save.dat") << "keep save";
            std::ofstream(dir / "param.bin") << "keep metadata";
        }
        std::ofstream(source / "save.dat") << "imported";
        auto set_slot = [&](const std::string& slot) {
            const char key[] = "SAVEDATA_DIRECTORY";
            std::string sfo(36, '\0');
            auto put32 = [&](size_t at, uint32_t value) {
                for (int i = 0; i < 4; ++i) sfo[at + i] = char(value >> (8 * i));
            };
            sfo.replace(0, 4, std::string("\0PSF", 4));
            put32(4, 0x101); put32(8, 36); put32(12, 36 + sizeof key); put32(16, 1);
            put32(20, 0x02040000); put32(24, slot.size() + 1); put32(28, slot.size() + 1);
            sfo.append(key, sizeof key);
            sfo += slot;
            sfo += '\0';
            assert(WriteTextAtomic(source / "sce_sys/param.sfo", sfo));
        };
        auto assert_preserved = [&] {
            for (const auto& dir : {root / "KEEP", other}) {
                assert(ReadText(dir / "save.dat") == "keep save");
                assert(ReadText(dir / "param.bin") == "keep metadata");
            }
        };
        for (const auto& slot : std::vector<std::string>{"..", ".", "", "../OTHER_TITLE", "..\\OTHER_TITLE",
                 "/tmp", "C:save", ".. ", "...", "SLOT.", std::string("..\0x", 4), std::string(32, 'x')}) {
            set_slot(slot);
            assert(!ImportSaves(source).empty());
            assert_preserved();
            assert(!fs::exists(BackupRoot()));
        }
        set_slot("LINK");
        for (const auto& destination : {root / "KEEP", other}) {
            for (const auto& link : {root / "LINK", root / "LINK.sce_sys"}) {
                fs::create_directory_symlink(destination, link);
                assert(!ImportSaves(source).empty());
                assert_preserved();
                assert(!fs::exists(BackupRoot()));
                fs::remove(link);
            }
        }
        set_slot("SLOT_OK");
        fs::create_directories(root / "SLOT_OK");
        std::ofstream(root / "SLOT_OK/old.dat") << "old save";
        assert(ImportSaves(source).empty());
        assert(ReadText(root / "SLOT_OK/save.dat") == "imported");
        assert(!fs::exists(root / "SLOT_OK/old.dat"));
        assert(fs::file_size(root / "SLOT_OK.sce_sys/param.bin") == sizeof(SaveParam));
        const auto backup = fs::directory_iterator(BackupRoot())->path();
        assert(ReadText(backup / "SLOT_OK/old.dat") == "old save");
        assert_preserved();
        assert(RestoreBackup(backup).empty());
        assert(ReadText(root / "SLOT_OK/old.dat") == "old save");
        g_app.data = actual_data / "save-import-test/fresh";
        assert(ImportSaves(source).empty());
        assert(ReadText(SaveRoot() / "SLOT_OK/save.dat") == "imported");
        g_app.data = actual_data;
    }
    // Exercise the real EOS process/pipe protocol without an SDK or network connection.
    const auto actual_root = g_app.root;
    g_app.root = g_app.data / "eos-fixture";
    fs::create_directories(EosBridgeDir());
    const auto bridge = EosBridgeDir() / "ryunet-bridge";
    std::ofstream(bridge) << "#!/bin/sh\nprintf 'BRIDGE READY ip=203.0.113.1 map=1234,8080:18080\\nBRIDGE PEERS port=2345\\n'\nread line\n";
    fs::permissions(bridge, fs::perms::owner_all);
    std::ofstream(EosBridgeDir() / "libEOSSDK-Linux-Shipping.so") << "fixture";
    EosBridgeStart("on|test.invalid", "on", "test.invalid");
    uint16_t local = 0;
    assert(EosBridgeRoute("test.invalid", 8080, local, 2000) && local == 18080);
    assert(!EosBridgeRoute("another.invalid", 8080, local, 0));
    std::string map, ip, peers;
    for (int i = 0; i < 100; ++i) {
        EosBridgeGameEnv("test.invalid", map, ip, peers, 0);
        if (!peers.empty()) break;
        SleepMs(10);
    }
    assert(map == "1234,8080:18080" && ip == "203.0.113.1" && peers == "2345");
    EosBridgeStop();
    assert(!g_eos.process && g_eos.input == -1 && !g_eos.reader.joinable());
    EosBridgeStart("off|test.invalid", "off", "test.invalid");
    assert(g_eos.state == "off" && !g_eos.process);
    EosBridgeStop();
    g_app.root = actual_root;
    g_app.bin = g_app.root / "out";
    g_app.python.reset();
    DiscordPresence::Update(true, "test", nullptr);
    assert(DiscordPresence::GetStatus() == DiscordPresence::Status::Error);
    g_app.python = FindPython(g_app.root);
    g_app.launcher.Set("discord_rpc", "0");
    g_app.game.Set("online", "0");
    g_app.game.Set("perf_reports", "0");
    g_app.game.Set("online_server", "127.0.0.1:9");
    g_app.game.Erase("key.move_up");
    SaveGameIni();
    Ini external;
    assert(external.Load(GameIni()));
    external.Set("volume", "37");
    external.Set("temporal", "fsr1");
    assert(external.Save(GameIni(), nullptr));
    RefreshGameSettings(true);
    assert(g_app.game.Get("volume") == "37" && g_app.game.Get("temporal") == "fsr1");
    assert(WidePath(TextPath("/tmp/日本語 café")) == L"/tmp/日本語 café");
    const auto quoted = Narrow(Quote(L"/tmp/a'b \"c\" $HOME;`false`"));
    assert(LinuxProcess::Arguments(quoted) == std::vector<std::string>{"/tmp/a'b \"c\" $HOME;`false`"});
    assert(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    unsigned char plain[16]{}, cipher[16]{}, decoded[16]{}, iv[16]{};
    assert(AesCbc(std::string(16, 'k'), iv, plain, sizeof plain, cipher, true));
    assert(AesCbc(std::string(16, 'k'), iv, cipher, sizeof cipher, decoded, false));
    assert(std::memcmp(plain, decoded, sizeof plain) == 0);
    const bool tray_only = Env("BB_TEST_TRAY_ONLY") == "1";
    const bool tray_test = tray_only || Env("BB_TEST_TRAY") == "1";
    if (!tray_test) setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-launcher-test-bus", 1);
    const bool sdl_ready = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!sdl_ready) std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
    assert(sdl_ready);
    g_app.hwnd = SDL_CreateWindow("Launcher UI test", 1120, 840, SDL_WINDOW_HIDDEN);
    assert(g_app.hwnd);
    if (tray_test) {
        EnsureTray();
        for (int i = 0; i < 200 && !g_tray_connected; ++i) { UpdateTray(); SleepMs(10); }
        assert(EnsureTray()); // The desktop watcher accepted the native indicator.
        assert(g_tray && g_tray_open && g_tray_game && g_tray_exit);
        assert(g_tray_labels[0] == "Open launcher");
        assert(!g_action_get_enabled(G_ACTION(g_tray_game)));
        g_app.game_running = true;
        UpdateTray();
        assert(g_action_get_enabled(G_ACTION(g_tray_game)));
        assert(g_tray_labels[1] == "Quit game");
        g_app.game_running = false;
        DestroyTray();
    } else assert(!EnsureTray());
    g_renderer = SDL_CreateRenderer(g_app.hwnd, "vulkan,opengl,opengles2");
    assert(g_renderer);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    SetupStyle(1); SetupFonts();
    assert(ImGui_ImplSDL3_InitForSDLRenderer(g_app.hwnd, g_renderer));
    assert(ImGui_ImplSDLRenderer3_Init(g_renderer));
    LoadArtwork();
    assert(g_hero_art.texture && g_logo_art.texture && g_menu_background.texture);
    if (!tray_only) {
        g_app.saves_dirty = false;
        g_app.saves = {{"SAVE", "A long save title that must wrap inside the description column", "Adventure", "100%", "15/11/2026 12:34", 123456}};
        g_app.backups = {{g_app.data / "backup.zip", "backup-with-a-long-name-2026-11-15.zip", "", 123456}};
        g_cloud.loaded = true;
        g_cloud.list = {{1, 1700000000, 123456, "CUSA13795", "test"}};
        g_trophies_reload = false;
        g_trophies.game = g_app.game_dir;
        g_trophies.language = GameLanguage();
        g_trophies.title_name = "CTR";
        g_trophies.list = {{1, 'G', false, "A trophy title that must wrap in a narrow window",
                           "A long trophy description that must stay inside its column", nullptr, 1700000000}};
        std::set<ImGuiID> rendered_tables;
        auto check_tables = [&] {
            for (int i = 0; i < GImGui->Tables.GetMapSize(); ++i) {
                const auto* table = GImGui->Tables.TryGetMapData(i);
                if (!table || table->LastFrameActive != GImGui->FrameCount) continue;
                rendered_tables.insert(table->ID);
                if (table->OuterRect.Max.x > table->OuterWindow->InnerClipRect.Max.x + 1)
                    std::fprintf(stderr, "Table %08x in %s exceeds its window by %.1f pixels\n", table->ID,
                                 table->OuterWindow->Name, table->OuterRect.Max.x - table->OuterWindow->InnerClipRect.Max.x);
                assert(table->OuterRect.Max.x <= table->OuterWindow->InnerClipRect.Max.x + 1);
                for (const auto& column : table->Columns) {
                    const float content_right = std::max(column.ContentMaxXUnfrozen, column.ContentMaxXHeadersUsed);
                    if (content_right > column.WorkMaxX + 1)
                        std::fprintf(stderr, "Table %08x column overflows by %.1f pixels\n", table->ID,
                                     content_right - column.WorkMaxX);
                    assert(content_right <= column.WorkMaxX + 1);
                }
            }
        };
        for (int page = 0; page < 10; ++page) {
            g_app.active_page = page;
            RenderFrame(); RenderFrame();
            check_tables();
            SDL_Surface* surface = SDL_RenderReadPixels(g_renderer, nullptr);
            assert(surface && surface->w == 1120 && surface->h == 840);
            if (page == 0) {
                SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
                assert(rgba);
                std::set<uint32_t> colors;
                for (int y = 70; y < 130; y += 5) for (int x = 900; x < 1080; x += 5) {
                    uint32_t color;
                    std::memcpy(&color, static_cast<const char*>(rgba->pixels) + y * rgba->pitch + x * 4, 4);
                    colors.insert(color);
                }
                assert(colors.size() > 20); // Both artwork triangles must be drawn.
                SDL_DestroySurface(rgba);
            }
            assert(SDL_SaveBMP(surface, (g_app.data / ("page-" + std::to_string(page) + ".bmp")).c_str()));
            SDL_DestroySurface(surface);
        }
        for (int page : {1, 9}) {
            g_app.active_page = page;
            RenderFrame(); RenderFrame();
            ImGui::GetIO().AddMousePosEvent(900, 700);
            RenderFrame();
            ImGui::GetIO().AddMouseWheelEvent(0, -1000);
            RenderFrame(); RenderFrame();
            SDL_Surface* surface = SDL_RenderReadPixels(g_renderer, nullptr);
            assert(surface && SDL_SaveBMP(surface, (g_app.data / ("bottom-" + std::to_string(page) + ".bmp")).c_str()));
            SDL_DestroySurface(surface);
            ImGui::GetIO().AddMouseWheelEvent(0, 1000);
            RenderFrame(); RenderFrame();
        }
        for (const char* language : {"id", "es", "fr", "de", "it", "pt_BR", "ru", "ja", "zh_CN"}) {
            g_language = language; ApplyLauncherLanguage(); RenderFrame();
        }
        g_language = "en"; ApplyLauncherLanguage();
        for (const auto size : {ImVec2(900, 650), ImVec2(1120, 840), ImVec2(1280, 800)}) {
            SDL_SetWindowSize(g_app.hwnd, int(size.x), int(size.y));
            SDL_SyncWindow(g_app.hwnd);
            SDL_Event resized;
            while (SDL_PollEvent(&resized)) HandleEvent(resized);
            for (const char* language : {"en", "de", "ru", "id"}) {
                g_language = language; ApplyLauncherLanguage();
                for (int page : {0, 3, 4, 6, 8}) {
                    g_app.active_page = page;
                    for (int frame = 0; frame < 4; ++frame) RenderFrame();
                    assert(ImGui::GetIO().DisplaySize.x == size.x && ImGui::GetIO().DisplaySize.y == size.y);
                    check_tables();
                    if (page != 3 && page != 4) continue;
                    ImGuiWindow* content = nullptr;
                    for (ImGuiWindow* window : GImGui->Windows) {
                        if (std::strstr(window->Name, "menu_navigation")) {
                            assert(window->ScrollMax.x < 1);
                            assert(window->ContentSize.y >= 10 * 44.0f * g_ui_scale);
                        }
                        if (std::strstr(window->Name, "/menu_content_") && window->LastFrameActive == GImGui->FrameCount)
                            content = window;
                    }
                    if (!content || content->ScrollMax.x >= 1)
                        std::fprintf(stderr, "Overflow: %s page %d %.0fx%.0f, scroll %.1f\n", language, page, size.x, size.y,
                                     content ? content->ScrollMax.x : -1);
                    assert(content && content->ScrollMax.x < 1);
                    const auto* table = ImGui::TableFindByID(content->GetID(page == 3 ? "pad_table" : "keys"));
                    assert(table);
                    const auto& actions = table->Columns[2];
                    assert(actions.ContentMaxXUnfrozen <= actions.WorkMaxX + 1);
                    assert(actions.MaxX <= content->InnerClipRect.Max.x + 1);
                    if (std::string(language) == "en" && size.x == 900) {
                        SDL_Surface* surface = SDL_RenderReadPixels(g_renderer, nullptr);
                        assert(surface && surface->w == int(size.x) && surface->h == int(size.y));
                        assert(SDL_SaveBMP(surface, (g_app.data / ("compact-" + std::to_string(page) + ".bmp")).c_str()));
                        SDL_DestroySurface(surface);
                    }
                }
            }
        }
        SDL_SetWindowSize(g_app.hwnd, 900, 650);
        SDL_SyncWindow(g_app.hwnd);
        SDL_Event resized;
        while (SDL_PollEvent(&resized)) HandleEvent(resized);
        g_language = "en"; ApplyLauncherLanguage();
        g_app.active_page = 3; g_pad_capture = 0;
        for (int frame = 0; frame < 4; ++frame) RenderFrame();
        for (ImGuiWindow* window : GImGui->Windows) {
            if (window->LastFrameActive != GImGui->FrameCount || !(window->Flags & ImGuiWindowFlags_ChildWindow)) continue;
            if (window->ScrollMax.x >= 1)
                std::fprintf(stderr, "%s overflows horizontally by %.1f pixels\n", window->Name, window->ScrollMax.x);
            assert(window->ScrollMax.x < 1);
        }
        g_pad_capture = -1;
        SDL_SetWindowSize(g_app.hwnd, 1120, 840);
        SDL_SyncWindow(g_app.hwnd);
        while (SDL_PollEvent(&resized)) HandleEvent(resized);
        g_app.active_page = 4;
        SDL_Event key{}; key.type = SDL_EVENT_KEY_DOWN;
        for (auto code : {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_INSERT}) {
            g_key_capture = 0; key.key.scancode = code;
            HandleEvent(key); RenderFrame();
            assert(g_key_capture == -1 && g_app.game.Get("key.move_up").empty());
        }
        g_key_capture = 0; key.key.scancode = SDL_SCANCODE_W;
        HandleEvent(key); RenderFrame();
        assert(g_app.game.Get("key.move_up") == "W");
        const auto source = g_app.root;
        g_app.root = g_app.data / "fake-game";
        fs::create_directories(g_app.root);
        std::ofstream(g_app.root / "run.sh") << "#!/bin/sh\ntrap '' TERM\n"
            "printf '%s' \"${BB_LAUNCHER_TEST_ENV-unset}\" > launch-env\necho ready\nsleep 30 & wait\n";
        unsetenv("BB_LAUNCHER_TEST_ENV");
        SetEnv(L"BB_LAUNCHER_TEST_ENV", "first-launch");
        LaunchGameProcess();
        assert(g_app.game_running);
        SleepMs(100);
        StopGame();
        FinishWorkers();
        assert(g_app.job_finished && g_app.exit_code == 137);
        assert(ReadText(g_app.root / "launch-env") == "first-launch");
        assert(Env("BB_LAUNCHER_TEST_ENV").empty());
        ReleaseGameProcess(); g_app.game_running = false; g_app.job = Job::None;
        BeginGameEnv();
        LaunchGameProcess();
        SleepMs(100);
        signal(SIGTERM, HandleTermination);
        raise(SIGTERM);
        WaitForGame();
        assert(!g_app.game_running && g_app.stopped && g_app.exit_code == 137);
        assert(!g_app.worker.joinable());
        assert(ReadText(g_app.root / "launch-env") == "unset");
        g_terminate = 0;
        fs::create_directories(g_app.root / "launcher/linux");
        std::ofstream(g_app.root / "launcher/linux/services.py") <<
            "from pathlib import Path\nimport json,sys\njson.load(sys.stdin)\n"
            "with open('attempts', 'a') as out: out.write('1')\nprint('{\"error\":\"failed download\"}')\n";
        setenv("APPIMAGE", PathText(g_app.root / "test.AppImage").c_str(), 1);
        fs::remove(g_app.root / "attempts");
        g_linux_update.release = "{}";
        StartUpdateInstall();
        while (g_linux_update.busy) SleepMs(10);
        PollUpdate();
        assert(ReadText(g_app.root / "attempts") == "1");
        assert(!g_linux_update.worker.joinable() && !g_linux_update.busy && !g_linux_update.ready);
        assert(g_linux_update.message == "failed download");
        std::ofstream(g_app.root / "launcher/linux/services.py") <<
            "from pathlib import Path\nimport json,os,sys\nr=json.load(sys.stdin)\n"
            "with open('attempts', 'a') as out: out.write(r['op'] + '\\n')\n"
            "if r['op'] == 'restart':\n"
            " assert r['executable'] == os.environ['APPIMAGE'] and r['args'] == ['--updated']\n"
            "print(json.dumps({'error':'restart failed'} if r['op'] == 'restart' and Path('fail-restart').exists() else {'ok':True}, separators=(',',':')))\n";
        for (bool fail_restart : {true, false}) {
            fs::remove(g_app.root / "attempts");
            if (fail_restart) std::ofstream(g_app.root / "fail-restart") << "1";
            else fs::remove(g_app.root / "fail-restart");
            StartUpdateInstall();
            while (g_linux_update.busy) SleepMs(10);
            g_app.game_running = true;
            PollUpdate();
            assert(g_linux_update.ready && !g_linux_update.restart_sent && !g_quit);
            assert(ReadText(g_app.root / "attempts") == "update_install\n");
            g_app.game_running = false;
            PollUpdate();
            assert(g_linux_update.restart_sent && g_quit == !fail_restart);
            PollUpdate();
            assert(ReadText(g_app.root / "attempts") == "update_install\nrestart\n");
            g_quit = false;
        }
        g_linux_update.ready = false; g_linux_update.release.clear(); g_linux_update.message.clear();
        unsetenv("APPIMAGE");
        g_app.root = source;
        g_language = "en"; ApplyLauncherLanguage();
        g_app.active_page = 5;
        g_mod_configuration.mod.mod_name = "Table test";
        ModOption option;
        option.id = "test"; option.type = "boolean"; option.value = "true";
        option.row = "A long group name that must wrap";
        option.column = "A long option heading that must wrap";
        g_mod_configuration.mod.mod_options = {option};
        for (const auto size : {ImVec2(900, 650), ImVec2(1120, 840), ImVec2(1280, 800)}) {
            SDL_SetWindowSize(g_app.hwnd, int(size.x), int(size.y));
            SDL_SyncWindow(g_app.hwnd);
            while (SDL_PollEvent(&resized)) HandleEvent(resized);
            for (const char* language : {"en", "de", "ru", "id"}) {
                g_language = language; ApplyLauncherLanguage();
                g_mod_configuration.open = true;
                for (int frame = 0; frame < 4; ++frame) RenderFrame();
                check_tables();
            }
        }
        assert(rendered_tables.size() >= 8);
    }
    if (tray_test) {
        EnsureTray();
        for (int i = 0; i < 200 && !g_tray_connected; ++i) { UpdateTray(); SleepMs(10); }
        assert(EnsureTray());
        auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        assert(bus);
        gchar* menu_path = nullptr;
        g_object_get(g_tray_dbusmenu, DBUSMENU_SERVER_PROP_DBUS_OBJECT, &menu_path, nullptr);
        auto tray_call = [&](const char* interface, const char* method, GVariant* parameters) {
            struct Reply { bool done = false; GVariant* value = nullptr; GError* error = nullptr; } reply;
            g_dbus_connection_call(bus, g_dbus_connection_get_unique_name(bus), menu_path,
                interface, method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    auto& reply = *static_cast<Reply*>(data);
                    reply.value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &reply.error);
                    reply.done = true;
                }, &reply);
            while (!reply.done) g_main_context_iteration(nullptr, true);
            if (reply.error) std::fprintf(stderr, "%s: %s\n", method, reply.error->message);
            assert(!reply.error && reply.value);
            return reply.value;
        };
        auto* property = tray_call("org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.kde.StatusNotifierItem", "Menu"));
        gchar* value = g_variant_print(property, false);
        assert(std::strstr(value, menu_path));
        g_free(value); g_variant_unref(property);
        SDL_HideWindow(g_app.hwnd);
        g_variant_unref(tray_call("org.kde.StatusNotifierItem", "Activate", g_variant_new("(ii)", 0, 0)));
        SDL_Event activated;
        while (SDL_PollEvent(&activated)) HandleEvent(activated);
        assert(!(SDL_GetWindowFlags(g_app.hwnd) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)));
        UpdateTray();
        assert(g_tray_labels[0] == "Minimize launcher");
        g_variant_unref(tray_call("org.kde.StatusNotifierItem", "Activate", g_variant_new("(ii)", 0, 0)));
        while (SDL_PollEvent(&activated)) HandleEvent(activated);
        assert(SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_HIDDEN);
        UpdateTray();
        assert(g_tray_labels[0] == "Open launcher");
        // Two activations queued before SDL processes them must toggle twice.
        g_variant_unref(tray_call("org.kde.StatusNotifierItem", "Activate", g_variant_new("(ii)", 0, 0)));
        g_variant_unref(tray_call("org.kde.StatusNotifierItem", "Activate", g_variant_new("(ii)", 0, 0)));
        while (SDL_PollEvent(&activated)) HandleEvent(activated);
        assert(SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_HIDDEN);
        auto* layout = tray_call("com.canonical.dbusmenu", "GetLayout",
            g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0)));
        value = g_variant_print(layout, false);
        for (const auto& label : g_tray_labels) assert(std::strstr(value, label.c_str()));
        g_free(value); g_variant_unref(layout);
        assert(SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_HIDDEN); // Opening the context menu must not restore the launcher.
        SDL_ShowWindow(g_app.hwnd);
        RenderFrame();
        SDL_MinimizeWindow(g_app.hwnd);
        SDL_SyncWindow(g_app.hwnd);
        SDL_HideWindow(g_app.hwnd);
        // Hidden-only tests miss SDL restoring a saved minimize request on Wayland.
        SDL_MinimizeWindow(g_app.hwnd);
        SDL_Event event;
        while (SDL_PollEvent(&event)) HandleEvent(event);
        g_variant_unref(tray_call("com.canonical.dbusmenu", "Event",
            g_variant_new("(isvu)", 1, "clicked", g_variant_new_int32(0), 0u)));
        while (SDL_PollEvent(&event)) HandleEvent(event);
        assert(!(SDL_GetWindowFlags(g_app.hwnd) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)));
        UpdateTray();
        assert(g_tray_labels[0] == "Minimize launcher");
        auto* visible_menu = tray_call("com.canonical.dbusmenu", "GetLayout",
            g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0)));
        value = g_variant_print(visible_menu, false);
        assert(std::strstr(value, "Minimize launcher") && !std::strstr(value, "Open launcher"));
        g_free(value); g_variant_unref(visible_menu);
        g_variant_unref(tray_call("com.canonical.dbusmenu", "Event",
            g_variant_new("(isvu)", 1, "clicked", g_variant_new_int32(0), 0u)));
        while (SDL_PollEvent(&event)) HandleEvent(event);
        assert(SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_HIDDEN);
        g_free(menu_path); g_object_unref(bus);
        g_signal_emit_by_name(g_tray, "connection-changed", false);
        assert(!EnsureTray());
        while (SDL_PollEvent(&event)) HandleEvent(event);
        assert(!(SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_HIDDEN));
        g_quit = false;
        g_app.game_running = true;
        g_action_activate(G_ACTION(g_tray_exit), nullptr);
        assert(g_app.want_close && !g_quit); // Running games retain the exit confirmation.
        g_app.game_running = false;
        g_app.want_close = false;
        g_action_activate(G_ACTION(g_tray_exit), nullptr);
        assert(g_quit);
    }
    DestroyTray();
    ReleaseTrophies(g_trophies); ReleaseArtwork();
    ImGui_ImplSDLRenderer3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
    SDL_DestroyRenderer(g_renderer); SDL_DestroyWindow(g_app.hwnd); SDL_Quit();
}
