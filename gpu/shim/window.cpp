// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#include <fstream>
#include <vector>
#include <miniz.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"

namespace Frontend {

namespace {

// bbport 0.5 (issue 69): the monitor the window (and fullscreen) goes to. BB_DISPLAY: its number
// in SDL's order (1, 2, ...; bb-gpu-capabilities --displays lists them) or a part of its name;
// without it SDL's primary display. The monitors are logged so a report says which one was taken.
SDL_DisplayID ChooseDisplay() {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    const char* wanted = std::getenv("BB_DISPLAY");
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    SDL_DisplayID chosen = 0;
    if (wanted && *wanted) {
        char* end = nullptr;
        const long number = std::strtol(wanted, &end, 10);
        if (end && *end == '\0') {
            if (number >= 1 && number <= count) {
                chosen = ids[number - 1];
            }
        } else {
            for (int i = 0; i < count && !chosen; ++i) {
                const char* name = SDL_GetDisplayName(ids[i]);
                if (name && SDL_strcasestr(name, wanted)) {
                    chosen = ids[i];
                }
            }
        }
    }
    const SDL_DisplayID display = chosen ? chosen : primary;
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display)) {
        BbSettings::Get().display_w = mode->w; // the ultrawide choice (bbport_overlay.cpp)
        BbSettings::Get().display_h = mode->h;
    }
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetDisplayName(ids[i]);
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(ids[i]);
        std::printf("Display %d: %s %dx%d%s%s\n", i + 1, name ? name : "?", mode ? mode->w : 0,
                    mode ? mode->h : 0, ids[i] == primary ? " (primary)" : "",
                    ids[i] == display ? " <- the game's (BB_DISPLAY)" : "");
    }
    if (wanted && *wanted && !chosen) {
        std::printf("Display: BB_DISPLAY=%s matches none, the primary one is used\n", wanted);
    }
    SDL_free(ids);
    return display;
}

} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are updated and sampled by runtime_pad.c's own thread; their events are pumped here
    // with the window's. SacredFire: this pump no longer updates the joysticks, and SDL's device
    // notifications get their own thread: detecting devices after a controller connects or turns
    // off can block for a long time on Windows (DirectInput asking a slow HID device its name), and
    // the window stopped responding meanwhile.
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    const SDL_DisplayID display = ChooseDisplay();
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    // SacredFire: the window starts at the display buffer's size (3840x2160 with a 4K internal
    // resolution). Larger than the screen, the window mode had its borders off screen and looked
    // like fullscreen: the windowed size is kept within 80% of the display's usable area.
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > 0 &&
        (width_ > usable.w * 9 / 10 || height_ > usable.h * 9 / 10)) {
        const double scale = std::min(usable.w * 0.8 / width_, usable.h * 0.8 / height_);
        width_ = std::max(640, int(width_ * scale));
        height_ = std::max(360, int(height_ * scale));
    }
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
#ifdef __APPLE__
    // macOS: a Metal window. MoltenVK draws into the CAMetalLayer of an SDL Metal view below, so
    // SDL needs no Vulkan support of its own (vcpkg's SDL3 is built without it).
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN, true);
#else
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    // On scaled Wayland desktops, window coordinates are logical units. Request a native
    // pixel drawable so the compositor does not enlarge a low-resolution Vulkan surface.
    // The swapchain uses SDL_GetWindowSizeInPixels, including after pixel-size changes.
    // (Not on macOS: a Retina drawable doubles the presented pixels for no gain.)
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
#endif
    const char* fullscreen_env = std::getenv("BB_FULLSCREEN");
    // SacredFire: the setting already folds BB_FULLSCREEN in (Load runs before the window is
    // created); the environment is read again here in case it does not.
    const bool fullscreen = (fullscreen_env && fullscreen_env[0] == '1') ||
                            BbSettings::Get().fullscreen.load(std::memory_order_relaxed);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen);
    const char* borderless_env = std::getenv("BB_BORDERLESS");
    const bool borderless = (borderless_env && borderless_env[0] == '1') ||
                            BbSettings::Get().borderless.load(std::memory_order_relaxed);
    if (borderless) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    }
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window && std::strstr(SDL_GetError(), "Vulkan")) {
        // bbport: SDL found no usable Vulkan (no driver, or a loader without surfaces).
        LOG_CRITICAL(Frontend, "Failed to create window: {}", SDL_GetError());
        Vulkan::StopUnsupportedVulkan("this PC", 0);
    }
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
#ifdef __APPLE__
    // macOS: MoltenVK presents through the CAMetalLayer of an SDL Metal view (vk_platform.cpp
    // creates a VK_EXT_metal_surface from it).
    if (driver && !std::strcmp(driver, "cocoa")) {
        window_info.type = WindowSystemType::Metal;
        window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    int logical_w = 0, logical_h = 0;
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    std::printf("Window: %dx%d logical, %dx%d pixels on %s (pixel density %.3f)\n",
                logical_w, logical_h, w, h, driver ? driver : "?",
                SDL_GetWindowPixelDensity(window));
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

// sacredfire: a small PNG reader for the window icon (8-bit RGB or RGBA, no interlace; the PS4's
// icon0.png is 512x512 RGB). The game's files never enter the repository, so the icon is read from
// the game folder at start. Anything unexpected leaves the default icon.
static bool ReadPngRgba(const char* path, std::vector<u8>& rgba, int& width, int& height) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    static constexpr u8 signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (data.size() < 8 + 25 || std::memcmp(data.data(), signature, 8) != 0) {
        return false;
    }
    const auto be32 = [&](size_t at) {
        return u32(data[at]) << 24 | u32(data[at + 1]) << 16 | u32(data[at + 2]) << 8 | data[at + 3];
    };
    std::vector<u8> idat;
    u32 w = 0, h = 0;
    u8 depth = 0, color = 0, interlace = 0;
    for (size_t pos = 8; pos + 12 <= data.size();) {
        const u32 length = be32(pos);
        if (length > data.size() - pos - 12) {
            return false;
        }
        const char* type = reinterpret_cast<const char*>(&data[pos + 4]);
        const u8* body = &data[pos + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && length == 13) {
            w = be32(pos + 8);
            h = be32(pos + 12);
            depth = body[8];
            color = body[9];
            interlace = body[12];
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + length;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || depth != 8 || interlace != 0 ||
        (color != 2 && color != 6) || idat.empty()) {
        return false;
    }
    const u32 channels = color == 6 ? 4 : 3;
    const size_t stride = size_t(w) * channels;
    std::vector<u8> raw((stride + 1) * h);
    mz_ulong out_len = raw.size();
    if (mz_uncompress(raw.data(), &out_len, idat.data(), idat.size()) != MZ_OK || out_len != raw.size()) {
        return false;
    }
    // Undo the per-row filters (PNG filter types 0..4) in place.
    std::vector<u8> prev(stride, 0), cur(stride);
    rgba.resize(size_t(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        const u8 filter = raw[y * (stride + 1)];
        const u8* src = &raw[y * (stride + 1) + 1];
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= channels ? cur[i - channels] : 0;
            const int b = prev[i];
            const int c = i >= channels ? prev[i - channels] : 0;
            int predictor = 0;
            switch (filter) {
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            cur[i] = u8(src[i] + predictor);
        }
        for (u32 x = 0; x < w; ++x) {
            u8* out = &rgba[(size_t(y) * w + x) * 4];
            out[0] = cur[x * channels];
            out[1] = cur[x * channels + 1];
            out[2] = cur[x * channels + 2];
            out[3] = channels == 4 ? cur[x * channels + 3] : 255;
        }
        std::swap(prev, cur);
    }
    width = int(w);
    height = int(h);
    return true;
}

void WindowSDL::SetIcon(const char* png_path) {
    std::vector<u8> rgba;
    int w = 0, h = 0;
    if (!ReadPngRgba(png_path, rgba, w, h)) {
        LOG_INFO(Frontend, "Window icon: {} not usable, keeping the default", png_path);
        return;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4);
    if (!surface) {
        LOG_WARNING(Frontend, "Window icon: {}", SDL_GetError());
        return;
    }
    if (!SDL_SetWindowIcon(window, surface)) {
        LOG_WARNING(Frontend, "Window icon: {}", SDL_GetError());
    }
    SDL_DestroySurface(surface);
    LOG_INFO(Frontend, "Window icon: {} ({}x{})", png_path, w, h);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextPrompt(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    // SacredFire: the menu's fullscreen switch applies here, on the window thread.
    {
        // A failure is logged once per requested state, not on every poll.
        static int failed_for = -1;
        const bool want = BbSettings::Get().fullscreen.load(std::memory_order_relaxed);
        const bool have = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        if (want == have) {
            failed_for = -1;
        } else if (failed_for != int(want)) {
            if (SDL_SetWindowFullscreen(window, want)) {
                failed_for = -1;
            } else {
                failed_for = int(want);
                LOG_ERROR(Frontend, "Failed to set fullscreen {}: {}", want, SDL_GetError());
            }
        }
        // Borderless switch
        static int cur_borderless = -1;
        const bool want_borderless = BbSettings::Get().borderless.load(std::memory_order_relaxed);
        if (cur_borderless != int(want_borderless)) {
            cur_borderless = int(want_borderless);
            SDL_SetWindowBordered(window, !want_borderless);
        }
    }
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            last_mouse_motion_ms = SDL_GetTicks();
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        // Alt + Enter toggles fullscreen
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            (event.key.mod & SDL_KMOD_ALT) &&
            (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
            bool cur = BbSettings::Get().fullscreen.load(std::memory_order_relaxed);
            BbSettings::Get().fullscreen.store(!cur, std::memory_order_relaxed);
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            // SacredFire: the game ends with code 0 after this; the log says why (a player
            // closing the window or Alt+F4, or the system asking every program to quit).
            std::printf("Window: closed (%s); the game ends\n",
                        event.type == SDL_EVENT_QUIT ? "quit requested: Alt+F4, sign-out or shutdown"
                                                     : "the window's close button");
            std::fflush(stdout);
            is_open = false;
            break;
        default:
            break;
        }
    }
    UpdateCursor();
    return is_open;
}

// Issue #3: the OS cursor over the game. Hidden in fullscreen, and in a window after 3 s without
// moving the mouse; always shown while the settings menu is open.
void WindowSDL::UpdateCursor() {
    const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const bool hide = !BbOverlay::MenuOpen() &&
                      (fullscreen || SDL_GetTicks() - last_mouse_motion_ms > 3000);
    if (hide != cursor_hidden) {
        cursor_hidden = hide;
        hide ? SDL_HideCursor() : SDL_ShowCursor();
    }
}

} // namespace Frontend
