// Win32 / Direct3D 11 event loop.
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapchain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_occluded = false;

ID3D11ShaderResourceView* MakeTexture(const uint32_t* bgra, int width, int height) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(width);
    td.Height = UINT(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{bgra, UINT(width * 4), 0};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (!g_device || FAILED(g_device->CreateTexture2D(&td, &data, &texture))) return nullptr;
    g_device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    return view;
}

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    // The flip model (v0.8.8): DWM shows our buffer itself instead of copying it, which presents
    // with less latency and without the stutter of the old blit model when the window moves or
    // resizes. Windows 10 has FLIP_DISCARD, 8.1 FLIP_SEQUENTIAL; the blit model stays the last
    // resort.
    HRESULT hr = E_FAIL;
    for (const DXGI_SWAP_EFFECT effect : {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                                          DXGI_SWAP_EFFECT_DISCARD}) {
        sd.SwapEffect = effect;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &level, &g_context);
        if (hr == DXGI_ERROR_UNSUPPORTED)
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &level, &g_context);
        if (hr == S_OK) break;
    }
    if (hr != S_OK) return false;
    // One frame queued at most: what is drawn is what the mouse did a frame ago, not three.
    IDXGIDevice1* dxgi = nullptr;
    if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        dxgi->SetMaximumFrameLatency(1);
        dxgi->Release();
    }
    CreateRenderTarget();
    return true;
}

void CleanupDevice() {
    CleanupRenderTarget();
    if (g_swapchain) g_swapchain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapchain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

// Frame pacing (v0.8.8). While the launcher is in use (input in the last second, an item held,
// text being typed, an animation that asked for it) frames come at the display's rate; otherwise
// it draws a few frames a second, enough for background work's progress, and nothing while it
// is minimized or hidden. It used to draw 60 frames a second all the time, the game's included.
double g_active_until = 0.0;
bool g_sizing = false;   // inside the move/resize loop: WM_SIZE draws the frame itself
bool g_in_frame = false; // RenderFrame is running (a WM_SIZE it causes must not start another)

double NowSeconds() {
    static const double period = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * period;
}

/// Full frame rate for the next `seconds` (an animation or a transition in progress).
void KeepAnimating(double seconds) { g_active_until = std::max(g_active_until, NowSeconds() + seconds); }

/// Milliseconds to wait for input before the next frame; 0 draws it at once (vsync paces it).
DWORD FrameWait(HWND hwnd) {
    if (IsIconic(hwnd) || !IsWindowVisible(hwnd)) return 250;
    if (NowSeconds() < g_active_until) return 0;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive()) return 0;
    // In front: every refresh (vsync paces it). With NVIDIA G-SYNC for windowed mode the monitor
    // runs at the front window's frame rate, so the 20 frames a second this used to draw slowed the
    // whole desktop of that monitor to 20 Hz (players' report, 2026-11-17). Behind: a few a second.
    return GetForegroundWindow() == hwnd ? 0 : 250;
}

void RenderFrame() {
    if (g_in_frame || !g_swapchain) return;
    g_in_frame = true;
    ImGuiIO& io = ImGui::GetIO();
    if (g_resize_w && g_resize_h) {
        CleanupRenderTarget();
        g_swapchain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
        g_resize_w = g_resize_h = 0;
        CreateRenderTarget();
    }
    // The controller drives the launcher only while it is the window in front: XInput is
    // read whatever window has the focus, so playing moved the launcher's menus too.
    if (GetForegroundWindow() == g_app.hwnd) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // The controller sends no window messages: a button or stick read this frame is input too.
    if (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) {
        for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key) {
            if (ImGui::IsKeyDown(ImGuiKey(key))) {
                KeepAnimating(1.0);
                break;
            }
        }
    }
    Frame();
    ImGui::Render();
    const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_occluded = g_swapchain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    g_in_frame = false;
}

} // namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbar_created) {
        const bool had_tray = g_tray_added;
        g_tray_added = false;
        if (had_tray && !EnsureTray()) BringLauncherToFront();
        return 0;
    }
    if (msg == kMinimizeToTray || msg == kToggleLauncher) {
        if (msg == kToggleLauncher && !LauncherVisible()) BringLauncherToFront();
        else ShowWindow(hwnd, EnsureTray() ? SW_HIDE : SW_MINIMIZE);
        return 0;
    }
    if (msg == kShowLauncher) {
        BringLauncherToFront();
        return 0;
    }
    if (msg == kTrayMessage) {
        if (lparam == WM_LBUTTONUP) TrayAction(1);
        else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) ShowTrayMenu();
        return 0;
    }
    // Input, focus and size changes: full frame rate for a second (FrameWait).
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
        msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
        msg == WM_ACTIVATE || msg == WM_SIZE || msg == WM_DPICHANGED || msg == WM_DROPFILES)
        KeepAnimating(1.0);
    if (msg == WM_NCHITTEST) {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        RECT client{}; GetClientRect(hwnd, &client);
        if (!IsZoomed(hwnd)) {
            const LONG edge = LONG(8.0f * g_ui_scale);
            const bool l=pt.x<edge, r=pt.x>=client.right-edge, t=pt.y<edge, b=pt.y>=client.bottom-edge;
            if (t&&l) return HTTOPLEFT; if (t&&r) return HTTOPRIGHT;
            if (b&&l) return HTBOTTOMLEFT; if (b&&r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT; if (r) return HTRIGHT; if (t) return HTTOP; if (b) return HTBOTTOM;
        }
        return PtInRect(&g_titlebar_drag, pt) ? HTCAPTION : HTCLIENT;
    }
    // Gamepad tab: Esc cancels capture
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_pad_capture >= 0) {
        if (wparam == VK_ESCAPE) {
            g_pad_capture = -1;
            return 0;
        }
    }
    // Keyboard tab: the next key's scan code, before ImGui sees it.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && g_key_capture >= 0) {
        const unsigned code = unsigned((lparam >> 16) & 0xff) | ((lparam & (1 << 24)) ? 0x100u : 0u);
        g_key_captured = code + 1;
        return 0;
    }
    // ...or the next mouse button or wheel step (kMouseCode + SDL's button number). "Change" acts
    // on the button's release, so this is the click after it.
    if (g_key_capture >= 0) {
        unsigned button = 0;
        if (msg == WM_LBUTTONDOWN) button = 1;
        else if (msg == WM_MBUTTONDOWN) button = 2;
        else if (msg == WM_RBUTTONDOWN) button = 3;
        else if (msg == WM_XBUTTONDOWN) button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 4 : 5;
        else if (msg == WM_MOUSEWHEEL && GET_WHEEL_DELTA_WPARAM(wparam) != 0)
            button = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 6 : 7;
        if (button) {
            g_key_captured = kMouseCode + button + 1;
            return msg == WM_XBUTTONDOWN ? TRUE : 0;
        }
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wparam) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
            if (IsZoomed(hwnd)) {
                MONITORINFO monitor{sizeof monitor};
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                    params->rgrc[0] = monitor.rcWork;
            }
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = LONG(900.0f * g_ui_scale);
        info->ptMinTrackSize.y = LONG(640.0f * g_ui_scale);
        MONITORINFO monitor{sizeof monitor};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
        }
        return 0;
    }
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            if (EnsureTray()) ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        g_resize_w = LOWORD(lparam);
        g_resize_h = HIWORD(lparam);
        // Dragging a border runs Windows' own loop, not ours: the frame is drawn here, so the
        // content follows the border instead of freezing or stretching until it is let go.
        if (g_sizing) RenderFrame();
        return 0;
    case WM_ENTERSIZEMOVE:
        g_sizing = true;
        return 0;
    case WM_EXITSIZEMOVE:
        g_sizing = false;
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_CLOSE:
        if (g_app.game_running) {
            g_app.want_close = true;
            ShowWindow(hwnd, SW_RESTORE);
            return 0;
        }
        break;
    case WM_DESTROY:
        DestroyTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
#ifdef LAUNCHER_SELFTEST
    // Test build only (CMake never defines it): file operations without the window.
    //   --import <folder> <port>, --restore <backup> <port>, --patches <port>; result in <port>\selftest.txt
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc >= 3) {
            const std::wstring op = argv[1];
            g_app.root = argv[argc - 1];
            std::string result;
            if (op == L"--import" && argc == 4) result = ImportSaves(argv[2]);
            else if (op == L"--restore" && argc == 4) result = RestoreBackup(argv[2]);
            else if (op == L"--dlc") {
                for (const auto& d : ScanDlc())
                    result += d.file + " | " + d.title + " | " + d.label + " | " + (d.ok ? "ok" : d.problem) + "\n";
            } else if (op == L"--aes") {
                // FIPS-197 C.1: AES-128, key 00..0f, plaintext 00112233..ff.
                std::string key;
                unsigned char plain[16], out[16], zero[16]{};
                for (int i = 0; i < 16; ++i) {
                    key += char(i);
                    plain[i] = (unsigned char)(i * 0x11);
                }
                AesCbc(key, zero, plain, 16, out, true);
                char hex[40]{};
                for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", out[i]);
                result = std::string("aes ") + hex + (std::string(hex) == "69c4e0d86a7b0430d8cdb78070b4c55a" ? " ok" : " BAD");
            } else if (op == L"--esfm" && argc == 6) {
                // --esfm <key hex> <NP comm ID> <entry file> <port>
                const std::string xml = DecryptEsfm(TrophyKeyBytes(PathText(fs::path(argv[2]))), PathText(fs::path(argv[3])),
                                                    ReadText(argv[4]));
                result = xml.empty() ? "esfm: no xml" : "esfm: " + xml.substr(0, 200);
            } else if (op == L"--trophies" && argc == 4) {
                // --trophies <game folder> <port>
                g_app.game_dir = argv[2];
                LoadTrophies();
                result = "title " + g_trophies.title_id + " np " + g_trophies.np_comm_id + " trophies " +
                         std::to_string(g_trophies.list.size()) + (g_trophies.error.empty() ? "" : " error " + g_trophies.error) + "\n";
                for (const auto& tr : g_trophies.list)
                    result += std::to_string(tr.id) + " " + (tr.unlocked ? "unlocked" : "locked") + "\n";
            } else if (op == L"--patches") {
                for (const auto& p : LoadPatches())
                    result += p.key + " | " + p.author + " | " + p.version + " | " + (p.ini_key ? p.ini_key : "-") +
                              (p.default_on ? " | on" : "") + "\n";
                std::vector<std::string> on, off;
                ReadPatchConfig(on, off);
                for (const auto& k : on) result += "json enabled: " + k + "\n";
                for (const auto& k : off) result += "json disabled: " + k + "\n";
            }
            std::ofstream(g_app.root / L"selftest.txt", std::ios::binary) << (result.empty() ? "OK" : result) << "\n";
            return 0;
        }
    }
#endif
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int cmd_argc = 0;
    LPWSTR* cmd_argv = CommandLineToArgvW(GetCommandLineW(), &cmd_argc);
    bool direct_launch = false, updated = false, prepare = false;
    fs::path prepare_base, prepare_update, prepare_out;
    std::string ranked_link;  // sacredfire://ranked?mode=N: a Discord announcement's Join (RankedLinkRequest)
    if (cmd_argv) {
        for (int i = 1; i < cmd_argc; ++i) {
            if (_wcsicmp(cmd_argv[i], L"--prepare") == 0) prepare = true;
            // "--name value" or "--name=value" (before --play, which ends the list)
            const std::pair<std::wstring, fs::path*> options[] = {
                {L"--base", &prepare_base}, {L"--update", &prepare_update}, {L"--out", &prepare_out}, {L"--game", &g_game_override}};
            const std::wstring arg = cmd_argv[i];
            for (const auto& [flag, target] : options) {
                if (arg == flag && i + 1 < cmd_argc) *target = cmd_argv[++i];
                else if (arg.rfind(flag + L"=", 0) == 0) *target = arg.substr(flag.size() + 1);
            }
            if (_wcsnicmp(cmd_argv[i], L"sacredfire:", 11) == 0) ranked_link = Narrow(cmd_argv[i]);
            if (_wcsicmp(cmd_argv[i], L"--updated") == 0) updated = true;  // started by an update (PollUpdate)
            if (_wcsicmp(cmd_argv[i], L"--launch") == 0 || _wcsicmp(cmd_argv[i], L"--play") == 0 ||
                _wcsicmp(cmd_argv[i], L"-launch") == 0 || _wcsicmp(cmd_argv[i], L"-play") == 0 ||
                _wcsicmp(cmd_argv[i], L"--direct") == 0) {
                direct_launch = true;
                break;
            }
        }
        LocalFree(cmd_argv);
    }
    if (prepare) {
        const int code = PrepareMerge(prepare_base, prepare_update, prepare_out);
        CoUninitialize();
        return code;
    }

    LoadState();
    MigrateOldLauncher();
    RegisterRankedLink();
    if (!ranked_link.empty() && RankedLinkRequest(ranked_link)) {
        // A running game took the request: this launcher was only started for the link.
        if (g_app.worker.joinable()) g_app.worker.join();
        if (g_app.probe.joinable()) g_app.probe.join();
        CoUninitialize();
        return 0;
    }
    // One launcher per install: starting it again brings the running one back (it may be hidden in the
    // tray while the game runs, 20/11: a second one was opened without knowing) instead of a second
    // copy. Per install folder, so test builds side by side each keep their own launcher.
    std::wstring instance_tag;
    {
        std::string root = PathText(fs::weakly_canonical(g_app.root));
        for (char& c : root) c = char(std::tolower(static_cast<unsigned char>(c)));
        uint64_t hash = 1469598103934665603ull;   // FNV-1a
        for (const unsigned char c : root) hash = (hash ^ c) * 1099511628211ull;
        wchar_t text[32];
        swprintf(text, 32, L"%016llx", static_cast<unsigned long long>(hash));
        instance_tag = std::wstring(L"SacredFireLauncher-") + text;
    }
    HANDLE single_instance = CreateMutexW(nullptr, FALSE, (L"Local\\" + instance_tag).c_str());
    if (single_instance && GetLastError() == ERROR_ALREADY_EXISTS && ranked_link.empty()) {
        struct Find { const std::wstring* tag; HWND hwnd; } find{&instance_tag, nullptr};
        EnumWindows([](HWND w, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            if (!GetPropW(w, f->tag->c_str())) return TRUE;
            f->hwnd = w;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&find));
        if (find.hwnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(find.hwnd, &pid);
            AllowSetForegroundWindow(pid);   // this start came from the user: hand its focus over
            PostMessageW(find.hwnd, kShowLauncher, 0, 0);
        }
        CloseHandle(single_instance);
        if (g_app.worker.joinable()) g_app.worker.join();
        if (g_app.probe.joinable()) g_app.probe.join();
        CoUninitialize();
        return 0;
    }
    if (!direct_launch) {
        CleanupUpdate();
        if (updated) {
            g_app.notice = Format(T("Actualizado a %s. Tus ajustes, amigos, parches y partidas se mantienen.",
                                    "Updated to %s. Your settings, friends, patches and saves are kept."),
                                  g_app.version);
        } else if (g_app.launcher.Flag("update_check", true) && Env("RECOMPLAUNCHER") != "1") {  // it updates the game itself
            StartUpdateCheck(g_app.launcher.Flag("update_auto", false));
        }
    }

    if (direct_launch) {
        // The EOS bridge too (#226): without the window, nothing started it, and a game launched from
        // Steam or a shortcut went to the server directly on networks that only reach it through Epic.
        EosBridgePoll();
        if (g_app.worker.joinable()) g_app.worker.join();
        PollJobs();
        if (GameReady()) {
            if (g_app.launcher.Flag("discord_rpc", true))
                RegisterDiscordLaunch(DiscordAppId());
            {   // its check and EOS login take a while on a blocked network: wait for its decision
                const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
                std::string host;
                INTERNET_PORT port = 0, local = 0;
                SplitServer(server, host, port);
                if (g_app.game.Flag("online", true)) EosBridgeRoute(server, port, local, 30000);
            }
            // A chosen graphics card that shares its name with another is told apart by the GPU list.
            if (!g_app.launcher.Get("gpu").empty() && g_app.probe.joinable()) g_app.probe.join();
            StartGame();
            while (g_app.game_running) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                PollJobs();
                UpdateDiscord();
                Sleep(50);
            }
            DiscordPresence::Shutdown();
            EosBridgeStop();  // the game is over: the bridge ends with it
            if (g_app.worker.joinable()) g_app.worker.join();
            if (g_app.probe.joinable()) g_app.probe.join();
            CoUninitialize();
            return g_app.exit_code;
        }
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"CtrNitroFueledPcLauncher", nullptr};
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                              GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    const int width = int(1120 * scale), height = int(840 * scale);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + std::max(0, int(work.right - work.left - width) / 2);
    const int y = work.top + std::max(0, int(work.bottom - work.top - height) / 2);
    g_ui_scale = scale;
    HWND hwnd = CreateWindowW(wc.lpszClassName, kLauncherName,
                              WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
                              x, y, width, height, nullptr, nullptr, instance, nullptr);
    g_app.hwnd = hwnd;
    SetPropW(hwnd, instance_tag.c_str(), reinterpret_cast<HANDLE>(1));   // found by a second start
    const BOOL dark_titlebar = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark_titlebar, sizeof(dark_titlebar));
    if (!CreateDevice(hwnd)) {
        CleanupDevice();
        MessageBoxW(nullptr, L"Direct3D 11 is not available.", kLauncherName, MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    SetupStyle(scale);
    SetupFonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    LoadArtwork();
    EnsureTray();
    if (g_app.launcher.Flag("discord_rpc", true))
        RegisterDiscordLaunch(DiscordAppId());

    bool done = false;
    while (!done) {
        MSG msg;
        // Idle when nothing moves: wait for input, or a few frames a second for background work.
        if (const DWORD wait = FrameWait(hwnd))
            MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd) || !IsWindowVisible(hwnd)) {
            PollJobs();
            PollUpdate();
            continue;
        }
        if (g_occluded && g_swapchain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            PollJobs();
            continue;
        }
        g_occluded = false;
        RenderFrame();
    }

    // The bridge first (v0.8.5): the waits below for background work (a network check, an update)
    // could hold the closed launcher's process, and with it a bridge nobody uses any more.
    if (g_app.game_running) {  // the game may go through the bridge: it ends with run.py
        std::scoped_lock lock{g_eos.mutex};
        if (g_eos.input) CloseHandle(std::exchange(g_eos.input, nullptr));
        if (g_eos.process) CloseHandle(std::exchange(g_eos.process, nullptr));
    }
    if (g_eos.reader.joinable()) {
        if (g_app.game_running) g_eos.reader.detach();
        else EosBridgeStop();
    }
    DiscordPresence::Shutdown();
    if (g_update.worker.joinable()) {  // a download stops; an install in progress finishes
        g_update.progress.cancel = true;
        g_update.worker.join();
    }
    // Background work finishes on its own; a running game keeps running.
    if (g_app.worker.joinable()) {
        if (g_app.job == Job::Play) g_app.worker.detach();
        else g_app.worker.join();
    }
    if (g_app.probe.joinable()) g_app.probe.join();
    ReleaseTrophies(g_trophies);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseArtwork();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
