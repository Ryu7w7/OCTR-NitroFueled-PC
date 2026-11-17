// SPDX-License-Identifier: GPL-2.0-or-later
// SDL3 renders the same ImGui draw data as the Windows backend.
double g_active_until = 0;
bool g_vsync = false;
volatile sig_atomic_t g_terminate = 0;
void HandleTermination(int signal) { g_terminate = signal; }
double NowSeconds() { return double(Ticks()) / 1000.0; }
void KeepAnimating(double seconds) { g_active_until = std::max(g_active_until, NowSeconds() + seconds); }

SDL_HitTestResult SDLCALL HitTest(SDL_Window* window, const SDL_Point* point, void*) {
    int width, height; SDL_GetWindowSize(window, &width, &height);
    const int edge = 8;
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)) {
        const bool l = point->x < edge, r = point->x >= width - edge, t = point->y < edge, b = point->y >= height - edge;
        if (t && l) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (t && r) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (b && l) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (b && r) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (l) return SDL_HITTEST_RESIZE_LEFT;
        if (r) return SDL_HITTEST_RESIZE_RIGHT;
        if (t) return SDL_HITTEST_RESIZE_TOP;
        if (b) return SDL_HITTEST_RESIZE_BOTTOM;
    }
    return point->x < g_titlebar_drag.right && point->y < g_titlebar_drag.bottom
        ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}
void HandleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) RequestClose();
    else if (event.type == SDL_EVENT_USER) {
        const int command = event.user.code == 4 ? (LauncherVisible() ? 1 : 3) : event.user.code;
        if (command == 1 || command == 2) {
            if (EnsureTray()) SDL_HideWindow(g_app.hwnd);
            else SDL_MinimizeWindow(g_app.hwnd);
        }
        else if (command == 3) {
            // Clear SDL's saved minimized state before showing, or Wayland hides it again.
            SDL_RestoreWindow(g_app.hwnd);
            SDL_ShowWindow(g_app.hwnd);
            SDL_RaiseWindow(g_app.hwnd);
        }
    }
    else if (event.type == SDL_EVENT_WINDOW_MINIMIZED && EnsureTray()) SDL_HideWindow(g_app.hwnd);
    if (event.type == SDL_EVENT_KEY_DOWN && g_pad_capture >= 0 && event.key.scancode == SDL_SCANCODE_ESCAPE) g_pad_capture = -1;
    if (g_key_capture >= 0) {
        if (event.type == SDL_EVENT_KEY_DOWN) { g_key_captured = unsigned(event.key.scancode) + 1; return; }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { g_key_captured = kMouseCode + event.button.button + 1; return; }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && event.wheel.y) { g_key_captured = kMouseCode + (event.wheel.y > 0 ? 6 : 7) + 1; return; }
    }
    if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        const float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(g_app.hwnd));
        ImGui::GetStyle().ScaleAllSizes(scale / g_ui_scale);
        ImGui::GetStyle().FontScaleDpi = scale;
        g_ui_scale = scale;
    }
    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) ImportMods({TextPath(event.drop.data)});
    ImGui_ImplSDL3_ProcessEvent(&event);
    if ((event.type >= SDL_EVENT_KEY_DOWN && event.type <= SDL_EVENT_MOUSE_WHEEL) || event.type == SDL_EVENT_WINDOW_EXPOSED || event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        KeepAnimating(1.0);
}
void RenderFrame() {
    auto& io = ImGui::GetIO();
    if (SDL_GetWindowFlags(g_app.hwnd) & SDL_WINDOW_INPUT_FOCUS) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key)
        if (ImGui::IsKeyDown(ImGuiKey(key))) { KeepAnimating(1.0); break; }
    Frame();
    ImGui::Render();
    SDL_SetRenderDrawColor(g_renderer, 20, 20, 26, 255);
    SDL_RenderClear(g_renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_RenderPresent(g_renderer);
}
void FinishWorkers() {
    if (g_app.worker.joinable()) g_app.worker.join();
    if (g_app.probe.joinable()) g_app.probe.join();
    g_linux_update.cancel = true;
    if (g_linux_update.worker.joinable()) g_linux_update.worker.join();
    DiscordPresence::Shutdown();
    EosBridgeStop();
}
void WaitForGame() {
    while (g_app.game_running) {
        if (g_terminate && !g_app.stopped) StopGame();
        PollJobs();
        SleepMs(50);
    }
}
} // namespace

int main(int argc, char** argv) {
    signal(SIGINT, HandleTermination);
    signal(SIGTERM, HandleTermination);
    bool direct = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") { std::puts("SacredFire launcher: --play starts the saved game without a window."); return 0; }
        if (arg == "--play" || arg == "--launch" || arg == "--direct" || arg == "-play" || arg == "-launch") direct = true;
    }
    try { LoadState(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        if (!direct) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SacredFire", error.what(), nullptr);
        return 1;
    }
    if (g_app.launcher.Flag("discord_rpc", true)) RegisterDiscordLaunch(DiscordAppId());
    if (direct) {
        if (g_app.worker.joinable()) g_app.worker.join();
        if (g_app.probe.joinable()) g_app.probe.join();
        PollJobs();
        if (g_terminate) { FinishWorkers(); return 128 + g_terminate; }
        if (!GameReady()) { std::fprintf(stderr, "Choose and verify the game folder in the launcher first.\n"); FinishWorkers(); return 1; }
        StartGame();
        WaitForGame();
        FinishWorkers(); return int(g_app.exit_code);
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { std::fprintf(stderr, "%s\n", SDL_GetError()); FinishWorkers(); return 1; }
    SDL_Rect bounds{0, 0, 1120, 840};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds);
    g_app.hwnd = SDL_CreateWindow(Narrow(kLauncherName).c_str(), std::min(1120, bounds.w), std::min(840, bounds.h),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS);
    const std::string render_driver = Env("SDL_RENDER_DRIVER");
    if (g_app.hwnd) g_renderer = SDL_CreateRenderer(g_app.hwnd,
        render_driver.empty() ? "vulkan,opengl,opengles2" : render_driver.c_str());
    if (!g_renderer) { std::fprintf(stderr, "%s\n", SDL_GetError()); FinishWorkers(); SDL_Quit(); return 1; }
    if (auto* icon = PngSurface(ReadText(g_app.root / "launcher/io.github.sacredfire.Launcher.png"))) {
        SDL_SetWindowIcon(g_app.hwnd, icon); SDL_DestroySurface(icon);
    }
    SDL_SetWindowMinimumSize(g_app.hwnd, std::min(900, bounds.w), std::min(650, bounds.h));
    SDL_SetWindowHitTest(g_app.hwnd, HitTest, nullptr);
    g_vsync = SDL_SetRenderVSync(g_renderer, 1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    g_ui_scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(g_app.hwnd));
    SetupStyle(g_ui_scale); SetupFonts();
    ImGui_ImplSDL3_InitForSDLRenderer(g_app.hwnd, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);
    LoadArtwork();
    EnsureTray();
    if (g_app.launcher.Flag("update_check", true)) StartUpdateCheck(g_app.launcher.Flag("update_auto", false));
    KeepAnimating(1.0);
    while (!g_quit && !g_terminate) {
        const auto flags = SDL_GetWindowFlags(g_app.hwnd);
        const bool hidden = flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN);
        const bool active = !hidden && (NowSeconds() < g_active_until || io.WantTextInput || ImGui::IsAnyItemActive());
        const int wait = hidden ? 250 : active ? (g_vsync ? 0 : 16) : 250;
        SDL_Event event;
        if (wait && SDL_WaitEventTimeout(&event, wait)) HandleEvent(event);
        while (SDL_PollEvent(&event)) HandleEvent(event);
        UpdateTray();
        if (g_quit) break;
        if (hidden) { PollJobs(); PollUpdate(); continue; }
        RenderFrame();
    }
    if (g_app.game_running) StopGame();
    FinishWorkers();
    DestroyTray();
    ReleaseTrophies(g_trophies); ReleaseArtwork();
    ImGui_ImplSDLRenderer3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
    SDL_DestroyRenderer(g_renderer); SDL_DestroyWindow(g_app.hwnd); SDL_Quit();
    return 0;
}
