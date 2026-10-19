// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
// SacredFire: the ultrawide question at start (first start, or every start with ultrawide_ask).
std::atomic<bool> ultrawide_prompt{false};
bool ultrawide_prompt_checked = false;
std::atomic<bool> pad_rescan_requested{false}; // SacredFire: the menu's "Rescan controllers"
bool l3_down = false, r3_down = false;
bool dirty = false; // settings changed while open: saved on close
float base_scale = 1.0f;

// The game's text dialog (ImeDialog, the character name), typed on the keyboard: drawn while it
// is open. In fullscreen the window title that showed it is not visible (issues #17, #19).
std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    // The system cursor shows over the menu (window.cpp); ImGui learns where it is now, not at
    // the next motion: mouse motion is not passed on while the menu is closed.
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

// The widget runs before `v` is read: in one call, the order the arguments are evaluated in is
// unspecified (GCC went right to left; clang, the Windows build, left to right stored the old value).
void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, "%.2f");
    Store(value, v, changed);
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Where it was moved last (bbport.ini menu_pos, a fraction of the screen), kept on screen.
    ImVec2 pos(viewport->WorkPos.x + 40.0f * base_scale, viewport->WorkPos.y + 40.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    // SacredFire: the menu title.
    if (!ImGui::Begin(
            BbSettings::MenuText("SacredFire  (Insert / L3+R3)###bbport_settings",
                                 "SacredFire  (Insert / L3+R3)###bbport_settings"),
            &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    // Moved: remembered (saved with the settings when the menu closes).
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }
    // Always readable, even when the rest of the menu is in Russian.
    static const char* languages[] = {"English", "Русский"};
    int language = s.menu_language == BbSettings::MenuLanguage::Russian ? 1 : 0;
    if (ImGui::Combo("Language", &language, languages, 2)) {
        s.menu_language =
            language == 1 ? BbSettings::MenuLanguage::Russian : BbSettings::MenuLanguage::English;
        BbSettings::Save();
    }
    ImGui::Text(BbSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);

    // sacredfire: bbport's temporal upscalers, reactive mask and output scaling read the scene of
    // the game bbport was built for and stay off here (bbport_settings.cpp); their sections show
    // only with BB_UPSCALER_MENU=1, for experiments.
    static const bool upscaler_menu = std::getenv("BB_UPSCALER_MENU") != nullptr;
    if (upscaler_menu) {
    ImGui::SeparatorText(BbSettings::MenuText("Temporal upscaler", "Временной апскейлер"));
    const char* upscalers[] = {
        BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
        BbSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)")};
    static const char* later[] = {"DLSS", "XeSS"};
    int upscaler = s.upscaler;
    if (ImGui::BeginCombo(BbSettings::MenuText("Upscaler", "Апскейлер"), upscalers[upscaler])) {
        for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
            const bool supported = i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
                                   : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                                                                     : true;
            ImGui::BeginDisabled(!supported);
            if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                Store(s.upscaler, i, true);
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", BbSettings::MenuText("— not supported by this GPU",
                                                         "— не поддерживается видеокартой"));
            }
        }
        for (const char* name : later) {
            ImGui::BeginDisabled();
            ImGui::Selectable(name, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", BbSettings::MenuText("— in development", "— в работе"));
        }
        ImGui::EndCombo();
    }
    if (const char* problem = s.fsr4_problem.load()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           BbSettings::MenuText("FSR 4 unavailable: %s", "FSR 4 недоступен: %s"),
                           problem);
        if (!BbSettings::IsFsr4(s.upscaler))
            ImGui::TextUnformatted(BbSettings::MenuText(
                "The mode selected above is active. You can select FSR 4 again.",
                "Активен режим, выбранный выше. FSR 4 можно выбрать снова."));
        ImGui::PopTextWrapPos();
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(BbSettings::MenuText(
                "FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                "(output matches the DLL). One model for Native through Performance and another "
                "for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (requires the DLL "
                "and Proton).",
                "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и "
                "Proton)."));
        } else {
            Hint(BbSettings::MenuText(
                "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                "model (a brief pause). Assets: tools/fetch_fsr4_assets.sh.",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                "пауза). Ассеты: tools/fetch_fsr4_assets.sh."));
        }
        Checkbox(BbSettings::MenuText("FSR 4: auto exposure", "FSR 4: авто-экспозиция"),
                 s.fsr4_auto_exposure);
        Checkbox(BbSettings::MenuText("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter"),
                 s.fsr4_invert_jitter);
        Hint(BbSettings::MenuText(
            "For diagnosing ghosting: the FSR 4 network normalizes color by exposure and uses "
            "it to decide when to discard previous frames. Changes apply immediately, without a "
            "restart.",
            "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
            "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    ImGui::BeginDisabled(taa);
    int preset = taa ? BbSettings::NativeAA : s.preset.load();
    char preset_label[64];
    std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)", BbSettings::PresetName(preset),
                  BbSettings::PresetScale(preset));
    if (ImGui::BeginCombo(BbSettings::MenuText("Preset", "Пресет"), preset_label)) {
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            char label[64];
            const float scale = BbSettings::PresetScale(i);
            const int output = s.output_res;
            std::snprintf(
                label, sizeof(label),
                BbSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)"),
                BbSettings::PresetName(i), scale,
                int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            if (ImGui::Selectable(label, i == preset)) {
                Store(s.preset, i, true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (taa) {
        ImGui::TextWrapped("%s", BbSettings::MenuText(
            "TAA anti-aliases the scene at output resolution, without an FSR model or upscaling. "
            "Your saved FSR preset is restored when you select FSR.",
            "TAA сглаживает сцену в разрешении вывода, без модели FSR и апскейлинга. "
            "Сохранённый пресет FSR восстановится при выборе FSR."));
    }
    ImGui::Text(
        BbSettings::MenuText("Active scene render: %d x %d", "Активный рендер сцены: %d x %d"),
        s.active_render_width.load(), s.active_render_height.load());
    if (BbSettings::FixedRenderSession()) {
        ImGui::Text(BbSettings::MenuText("Startup preset: %s", "Пресет при запуске: %s"),
                    BbSettings::PresetName(s.startup_preset));
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            Hint(BbSettings::MenuText(
                "When output is not 1080p, the entire game renders at the preset resolution "
                "(startup patch): "
                "this is fastest on Steam Deck and weaker GPUs. Preset or output resolution "
                "changes "
                "require a restart. Enable Live resolution changes below to change them without "
                "restarting (post-processing then stays at 1080p, which is slower).",
                "При выводе не 1080p вся игра рисуется в разрешении пресета (патч при запуске): "
                "это быстрее всего на Steam Deck и слабых GPU. Смена пресета или разрешения "
                "вывода — после перезапуска. Пункт «Смена разрешения на лету» ниже включает "
                "смену без перезапуска (постобработка тогда остаётся в 1080p, медленнее)."));
        } else {
            Hint(BbSettings::MenuText(
                "BB_RENDER_RES fixes the scene size at startup. Remove this explicit environment "
                "variable to change resolution and presets without restarting the game.",
                "BB_RENDER_RES фиксирует размер сцены при запуске. Уберите эту явную переменную "
                "для смены разрешения и пресетов без перезапуска игры."));
        }
    } else {
        Hint(BbSettings::MenuText(
            "Native AA: FSR acts as anti-aliasing. Other presets reduce the scene render "
            "resolution "
            "relative to the output. The UI renders at output resolution. "
            "The preset applies from the next frame without restarting the game.",
            "Native AA: FSR работает как сглаживание. Остальные пресеты уменьшают разрешение "
            "отрисовки сцены относительно вывода. Интерфейс рисуется в разрешении вывода. "
            "Пресет применяется со следующего кадра без перезапуска игры."));
    }
    Checkbox(BbSettings::MenuText("Sharpening (RCAS)", "Резкость (RCAS)"), s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    Slider(BbSettings::MenuText("Sharpness", "Сила резкости"), s.sharpness, 0.0f, 2.0f);
    Hint(BbSettings::MenuText(
        "Up to 1: the upscaler's own sharpening (RCAS). Above 1 adds another RCAS pass. "
        "Ctrl+click the slider to enter an exact value.",
        "До 1 — резкость самого апскейлера (RCAS). Выше 1 добавляется ещё один проход RCAS. "
        "Ctrl+клик по ползунку — ввести точное значение."));
    ImGui::EndDisabled();
    Checkbox(BbSettings::MenuText("Subpixel jitter", "Субпиксельный сдвиг (jitter)"), s.jitter);
    Hint(BbSettings::MenuText(
        "Each frame shifts the scene by a fraction of a pixel, letting the upscaler reconstruct "
        "more detail from multiple frames. Without it, only history-based anti-aliasing remains.",
        "Каждый кадр сцена сдвигается на долю пикселя, и апскейлер собирает из нескольких "
        "кадров больше деталей. Без него получается только сглаживание по истории."));

    ImGui::SeparatorText(BbSettings::MenuText("Reactive mask", "Маска реактивности"));
    ImGui::BeginDisabled(taa);
    Checkbox(BbSettings::MenuText("Enable mask", "Включить маску"), s.reactive);
    Hint(BbSettings::IsFsr4(s.upscaler)
             ? BbSettings::MenuText(
                   "Marks transparent effects (haze, light, water, particles). FSR 4 takes no "
                   "mask itself: its output is blended with the current frame there, so the "
                   "effects do not drag previous frames along.",
                   "Помечает прозрачные эффекты (дымку, свет, воду, частицы). FSR 4 сам маску не "
                   "принимает: там его результат смешивается с текущим кадром, и эффекты не "
                   "тянут за собой прошлые кадры.")
             : BbSettings::MenuText(
                   "Marks transparent effects (particles, haze) so the upscaler relies less on "
                   "previous frames. Reduces trails behind effects, but shimmering returns "
                   "underneath them.",
                   "Помечает прозрачные эффекты (частицы, дымку), чтобы апскейлер меньше "
                   "опирался на прошлые кадры. Меньше шлейфов за эффектами, но под ними "
                   "возвращается дрожание."));
    ImGui::BeginDisabled(!s.reactive);
    Slider(BbSettings::MenuText("Scale", "Масштаб"), s.reactive_scale, 0.0f, 4.0f);
    Slider(BbSettings::MenuText("Threshold", "Порог"), s.reactive_threshold, 0.0f, 1.0f);
    Slider(BbSettings::MenuText("Maximum", "Максимум"), s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ImGui::Checkbox(BbSettings::MenuText("Show mask (debug)", "Показать маску (отладка)"),
                        &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    Checkbox(BbSettings::MenuText("Character motion vectors", "Векторы движения персонажей"),
             s.object_motion);
    Hint(BbSettings::MenuText(
        "Accurate vectors for animated objects: clothing and weapons break up less "
        "during movement. The static scene does not get an additional pass. "
        "Changes apply after restarting the game.",
        "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
        "при движении. Статичная сцена не получает дополнительный проход. "
        "Изменение применяется после перезапуска игры."));
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ImGui::Checkbox(BbSettings::MenuText("Show motion vectors (debug)",
                                             "Показать векторы движения (отладка)"),
                        &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    Hint(BbSettings::MenuText(
        "Red/green: horizontal/vertical motion (8 pixels = full brightness). "
        "Blue: the pixel received an accurate object vector, not just camera motion. "
        "The upscaler treats a moving object with no blue or red/green as "
        "stationary, causing trails.",
        "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
        "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
        "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
        "неподвижным, отсюда шлейф."));
    ImGui::EndDisabled(); // upscaler off

    ImGui::SeparatorText(BbSettings::MenuText("Output resolution", "Разрешение вывода"));
    static const char* outputs[] = {"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
    int output = s.output_res;
    if (ImGui::BeginCombo(BbSettings::MenuText("Output resolution", "Разрешение вывода"),
                          outputs[output])) {
        for (int i = 0; i < BbSettings::OutputCount; ++i) {
            if (ImGui::Selectable(outputs[i], i == output)) {
                Store(s.output_res, i, true);
            }
        }
        ImGui::EndCombo();
    }
    if (BbSettings::FixedRenderSession()) {
        Hint(BbSettings::MenuText(
            "Size of the final frame and UI. The preset sets the scene size relative to "
            "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
            "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
            "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."));
    } else {
        Hint(BbSettings::MenuText(
            "The final frame and UI size changes at the next frame boundary. "
            "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
            "Changing the size resets FSR history and may cause a brief pause.",
            "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
            "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
            "Смена размера сбрасывает историю FSR и может вызвать короткую паузу."));
    }
    const char* live_modes[] = {BbSettings::MenuText("Auto (based on GPU)", "Авто (по видеокарте)"),
                                BbSettings::MenuText("Off (faster)", "Выключена (быстрее)"),
                                BbSettings::MenuText("On", "Включена")};
    int live = s.live_resolution + 1;
    if (ImGui::BeginCombo(
            BbSettings::MenuText("Live resolution changes", "Смена разрешения на лету"),
            live_modes[live])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(live_modes[i], i == live)) {
                Store(s.live_resolution, i - 1, true);
            }
        }
        ImGui::EndCombo();
    }
    Hint(BbSettings::MenuText(
        "On: output resolution and preset change without restarting, but game post-processing "
        "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
        "Off: everything renders at the preset resolution, and changes require a restart. Auto "
        "enables this on powerful discrete GPUs. Applies after restarting the game.",
        "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
        "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
        "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
        "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    }
    // Game patches (patches/CrashTeamRacing.xml): bbport's own game-effect patches went with its
    // Bloodborne profile (removed 2026-10-09).
    ImGui::SeparatorText(BbSettings::MenuText("Game patches (restart required)",
                                              "Патчи игры (после перезапуска)"));
    // SacredFire: the scene resolution (render_res) replaces the 1080p Pro patch's checkbox; a
    // DLSS quality mode sets it instead (scripts/patches.py render_plan).
    const bool dlss_sized = s.temporal == BbSettings::TemporalDlss && s.dlss_quality > 0;
    // SacredFire: ultrawide (the monitor's own size instead of 16:9); it replaces render_res.
    const bool ultrawide = s.ultrawide == 1;
    if (BbSettings::UltrawideAvailable() || ultrawide) {
        bool on = ultrawide;
        char label[96];
        std::snprintf(label, sizeof label, "%s (%dx%d)", BbSettings::MenuText("Ultrawide", "Ультраширокий"),
                      s.display_w.load(), s.display_h.load());
        if (ImGui::Checkbox(label, &on)) {
            BbSettings::SetUltrawide(on);
            dirty = true;
            std::printf("Menu: ultrawide = %d (applies after the restart)\n", int(on));
        }
        Checkbox(BbSettings::MenuText("Ask about ultrawide at every start", "Спрашивать при каждом запуске"),
                 s.ultrawide_ask);
        Hint(BbSettings::MenuText(
            "Draws the game at the monitor's size instead of 16:9 with black bars. Replaces the "
            "internal resolution. Applies after restarting the game.",
            "Игра рисуется в размере монитора вместо 16:9. Применяется после перезапуска игры."));
    }
    {
        static const char* labels[] = {"854 x 480", "1280 x 720", "1920 x 1080",
                                       "2560 x 1440 (PS4 Pro)", "2880 x 1620", "3840 x 2160 (4K)"};
        const int current = s.render_res;
        const char* preview =
            dlss_sized    ? BbSettings::MenuText("(DLSS quality decides)", "(решает качество DLSS)")
            : current >= 0 ? labels[current]
                           : BbSettings::MenuText("(game patches decide)", "(решают патчи)");
        ImGui::BeginDisabled(dlss_sized || ultrawide);
        if (ImGui::BeginCombo(BbSettings::MenuText("Internal resolution", "Внутреннее разрешение"),
                              preview)) {
            for (int r = 0; r < BbSettings::RenderResCount; ++r) {
                if (ImGui::Selectable(labels[r], r == current)) {
                    Store(s.render_res, r, r != current);
                    std::printf("Menu: render_res = %d (applies after the restart)\n",
                                BbSettings::RenderResHeights[r]);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Hint(BbSettings::MenuText(
            "The size the game draws its 3D scene at. Above 1080p it uses the game's PS4 Pro path "
            "and the picture is output at 4K; on a 1080p screen that smooths jagged edges. 4K needs "
            "a strong GPU.",
            "Размер, в котором игра рисует 3D-сцену. Выше 1080p используется режим PS4 Pro, кадр "
            "выводится в 4K; на экране 1080p это сглаживает края. Для 4K нужна мощная видеокарта."));
    }
    // SacredFire: one upscaler at a time (bbport.ini temporal): FSR 1 (spatial) or the temporal
    // ones, FSR 3 and DLSS (vk_ctr_fsr3). Live; DLSS's quality mode and output need a restart.
    {
        static constexpr const char* labels[BbSettings::TemporalCount] = {
            "Off", "AMD FSR 1", "AMD FSR 3 (experimental)", "NVIDIA DLSS (experimental)"};
        const int current = std::clamp(s.temporal.load(), 0, BbSettings::TemporalCount - 1);
        if (ImGui::BeginCombo(BbSettings::MenuText("Upscaling", "Апскейл"), labels[current])) {
            for (int t = 0; t < BbSettings::TemporalCount; ++t) {
                if (ImGui::Selectable(labels[t], t == current)) {
                    Store(s.temporal, t, t != current);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. "
            "FSR 3 and DLSS rebuild detail over several frames from the game's depth and motion, "
            "and smooth edges even at the same resolution. DLSS needs an NVIDIA RTX card (FSR 3 "
            "runs otherwise). Applies at once.",
            "FSR 1 увеличивает сцену чётче простого растяжения. FSR 3 и DLSS восстанавливают "
            "детали за несколько кадров. DLSS нужна видеокарта RTX. Применяется сразу."));
    }
    if (s.temporal == BbSettings::TemporalDlss) {
        static constexpr const char* qualities[BbSettings::DlssQualityCount] = {
            "Auto (internal resolution)", "DLAA (native)", "Quality", "Balanced", "Performance",
            "Ultra Performance"};
        const int quality = std::clamp(s.dlss_quality.load(), 0, BbSettings::DlssQualityCount - 1);
        if (ImGui::BeginCombo(BbSettings::MenuText("DLSS quality", "Качество DLSS"), qualities[quality])) {
            for (int q = 0; q < BbSettings::DlssQualityCount; ++q) {
                if (ImGui::Selectable(qualities[q], q == quality)) {
                    Store(s.dlss_quality, q, q != quality);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Auto keeps the internal resolution above. The others render at NVIDIA's share of the "
            "DLSS output: Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%, DLAA "
            "100%. Applies after the restart.",
            "Авто: внутреннее разрешение выше. Остальные: доля от вывода DLSS. После перезапуска."));
        if (quality > 0) {
            static constexpr const char* outputs[] = {"1920 x 1080", "3840 x 2160 (4K)"};
            const int output = s.dlss_output ? 1 : 0;
            if (ImGui::BeginCombo(BbSettings::MenuText("DLSS output", "Вывод DLSS"), outputs[output])) {
                for (int o = 0; o < 2; ++o) {
                    if (ImGui::Selectable(outputs[o], o == output)) {
                        Store(s.dlss_output, o, o != output);
                    }
                }
                ImGui::EndCombo();
            }
        }
        static constexpr const char* models[] = {"Transformer (recommended)", "CNN"};
        const int model = s.dlss_model ? 1 : 0;
        if (ImGui::BeginCombo(BbSettings::MenuText("DLSS model", "Модель DLSS"), models[model])) {
            for (int m = 0; m < 2; ++m) {
                if (ImGui::Selectable(models[m], m == model)) {
                    Store(s.dlss_model, m, m != model);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Transformer: NVIDIA's current model (DLSS 4), sharper and steadier, a little heavier. "
            "CNN: the older model (preset E), lighter; NVIDIA marks it deprecated. Applies at once.",
            "Transformer: текущая модель NVIDIA. CNN: старая, легче. Применяется сразу."));
    }
    if (s.temporal == BbSettings::TemporalFsr3) {
        Slider(BbSettings::MenuText("FSR 3 sharpness", "Резкость FSR 3"), s.fsr3_sharpness, 0.0f, 1.0f);
    }

    for (int g = 0; g < BbSettings::GamePatchCount; ++g) {
        const auto& patch = BbSettings::GamePatches[g];
        if (std::strcmp(patch.key, "patch_pro_1080p") == 0) continue; // see Internal resolution
        const bool before = s.game_patches[g];
        Checkbox(BbSettings::MenuText(patch.label, patch.label_ru), s.game_patches[g]);
        if (before != s.game_patches[g]) {
            std::printf("Menu: %s = %d (applies after the restart)\n", patch.key, int(s.game_patches[g]));
        }
    }
    Hint(BbSettings::MenuText("From patches/CrashTeamRacing.xml (illusionyy/PS-Game-Patch), applied to the executable at start.",
                              "Из patches/CrashTeamRacing.xml (illusionyy/PS-Game-Patch), применяются к исполняемому файлу при запуске."));

    bool restart =
        s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    for (int g = 0; g < BbSettings::GamePatchCount; ++g) {
        restart |= s.game_patches[g] != s.startup_game_patches[g];
    }
    restart |= BbSettings::RenderSizeNeedsRestart();
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           BbSettings::MenuText("Changes apply after restarting the game",
                                                "Изменения применятся после перезапуска игры"));
        if (ImGui::Button(
                BbSettings::MenuText("Apply and restart game", "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    // SacredFire: controller ports for split screen. Each port takes one connected gamepad; the
    // runtime logs the matching user in and reads that pad for it. Port 1 is the player.
    ImGui::SeparatorText(BbSettings::MenuText("Controllers (split screen)", "Контроллеры (разделённый экран)"));
    {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        std::vector<std::pair<std::string, std::string>> pads; // guid, label
        for (int i = 0; ids && i < count; ++i) {
            char guid[33];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), guid, sizeof guid);
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            pads.emplace_back(guid, std::string(name ? name : "gamepad") + "  [" + std::string(guid).substr(0, 8) + "]");
        }
        SDL_free(ids);
        bool changed = false;
        for (int p = 0; p < BbSettings::ControllerPorts; ++p) {
            std::string current;
            {
                std::lock_guard lk{BbSettings::PortsMutex()};
                current = s.ports[size_t(p)].data();
            }
            // Ports 2-4 can take the keyboard: that player plays on it and port 1 keeps its gamepad.
            const char* keyboard = BbSettings::MenuText("Keyboard", "Клавиатура");
            std::string label = current.empty() ? (p == 0 ? BbSettings::MenuText("(first / last used)", "(первый / последний использованный)") : BbSettings::MenuText("(none)", "(нет)")) : current;
            if (p > 0 && current == "keyboard") label = keyboard;
            for (const auto& [guid, text] : pads) {
                if (guid == current) label = text;
            }
            char title[32];
            std::snprintf(title, sizeof title, "%s %d", BbSettings::MenuText("Port", "Порт"), p + 1);
            if (ImGui::BeginCombo(title, label.c_str())) {
                if (ImGui::Selectable(p == 0 ? BbSettings::MenuText("(first / last used)", "(первый / последний использованный)") : BbSettings::MenuText("(none)", "(нет)"), current.empty())) {
                    std::lock_guard lk{BbSettings::PortsMutex()};
                    s.ports[size_t(p)][0] = 0;
                    changed = true;
                }
                if (p > 0 && ImGui::Selectable(keyboard, current == "keyboard")) {
                    std::lock_guard lk{BbSettings::PortsMutex()};
                    for (auto& other : s.ports) {  // one player on the keyboard
                        if (std::strcmp(other.data(), "keyboard") == 0) other[0] = 0;
                    }
                    std::snprintf(s.ports[size_t(p)].data(), s.ports[size_t(p)].size(), "keyboard");
                    changed = true;
                }
                for (const auto& [guid, text] : pads) {
                    if (ImGui::Selectable(text.c_str(), guid == current)) {
                        std::lock_guard lk{BbSettings::PortsMutex()};
                        std::snprintf(s.ports[size_t(p)].data(), s.ports[size_t(p)].size(), "%s", guid.c_str());
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (changed) {
            BbSettings::Save();
        }
        ImGui::TextDisabled("%s", BbSettings::MenuText("Ports 2-4 log in as Player 2-4 while their gamepad is connected "
                                                       "(the keyboard: always; port 1 then has the gamepad only)",
                                                 "Порты 2-4 входят как Player 2-4, пока подключён их геймпад "
                                                 "(клавиатура: всегда; у порта 1 тогда только геймпад)"));
    }

    // SacredFire: the game's volume (runtime_audio.c reads it for every buffer).
    ImGui::SeparatorText(BbSettings::MenuText("Sound", "Звук"));
    {
        int volume = s.volume;
        const bool changed = ImGui::SliderInt(BbSettings::MenuText("Volume", "Громкость"), &volume, 0, 200, "%d%%");
        Store(s.volume, std::clamp(volume, 0, 200), changed);
        Hint(BbSettings::MenuText("100% is the level the game was mixed at; above it the loudest peaks are "
                                  "rounded off. Applies at once.",
                                  "100% — уровень, на котором сведена игра; выше самые громкие пики "
                                  "сглаживаются. Сразу."));
    }

    // SacredFire: performance (both apply at once).
    ImGui::SeparatorText(BbSettings::MenuText("Performance", "Производительность"));
    {
        static constexpr int limits[] = {0, 30, 60, 72, 90, 120, 144, 240};
        const char* names[] = {BbSettings::MenuText("No limit", "Без ограничения"), "30", "60",
                               BbSettings::MenuText("72 (half of 144 Hz)", "72 (половина 144 Гц)"),
                               "90", "120", "144", "240"};
        const int current = s.fps_limit;
        int index = -1;
        for (int i = 0; i < int(std::size(limits)); ++i) {
            if (limits[i] == current) {
                index = i;
            }
        }
        char other[32];
        std::snprintf(other, sizeof(other), "%d", current);
        if (ImGui::BeginCombo(BbSettings::MenuText("FPS limit", "Ограничение FPS"),
                              index >= 0 ? names[index] : other)) {
            for (int i = 0; i < int(std::size(limits)); ++i) {
                if (ImGui::Selectable(names[i], i == index)) {
                    Store(s.fps_limit, limits[i], true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "With the 60 FPS patch the game runs as fast as your PC allows. 60 is the pace it was "
            "made for; on a 144 Hz screen without G-Sync/FreeSync, 72 divides the refresh evenly "
            "(240 does the same on a 240 Hz screen). Applies at once.",
            "С патчем 60 FPS игра идёт так быстро, как позволяет ПК. 60 — темп, для которого она "
            "сделана; на экране 144 Гц без G-Sync/FreeSync 72 делит частоту ровно (240 — то же "
            "на экране 240 Гц). Сразу."));
        Checkbox(BbSettings::MenuText("Compile shaders in the background (experimental)",
                                      "Компилировать шейдеры в фоне (экспериментально)"),
                 s.async_pipelines);
        Hint(BbSettings::MenuText(
            "A shader seen for the first time no longer pauses the game: an effect may appear a "
            "frame or two late instead. Shaders seen before load at start either way. Experimental.",
            "Новый шейдер больше не останавливает игру: эффект может появиться на кадр-два позже. "
            "Уже виденные шейдеры загружаются при запуске в любом случае. Экспериментально."));
    }

    ImGui::SeparatorText(BbSettings::MenuText("Display", "Экран"));
    {
        Checkbox(BbSettings::MenuText("Fullscreen", "Полный экран"), s.fullscreen);
        Hint(BbSettings::MenuText("Fullscreen on the game's display (BB_DISPLAY). "
                                  "Applies at once; BB_FULLSCREEN=1 at start.",
                                  "Полный экран на дисплее игры (BB_DISPLAY). "
                                  "Сразу; BB_FULLSCREEN=1 при запуске."));
        Checkbox(BbSettings::MenuText("Borderless window", "Без рамок"), s.borderless);
        Hint(BbSettings::MenuText("Remove window borders in windowed mode.",
                                  "Убрать рамку окна в оконном режиме."));
    }

    ImGui::SeparatorText(BbSettings::MenuText("Other", "Прочее"));
    Checkbox(BbSettings::MenuText("FPS counter in corner", "Счётчик FPS в углу"), s.show_fps);
    // SacredFire: the Steam Deck sometimes shows a controller connected after launch only to a
    // fresh enumeration; the runtime restarts SDL's gamepad subsystem on the next input sample.
    if (ImGui::Button(BbSettings::MenuText("Rescan controllers", "Переопознать контроллеры"))) {
        pad_rescan_requested.store(true, std::memory_order_relaxed);
    }
    Hint(BbSettings::MenuText("For a controller connected after the game started and not seen by it.",
                              "Для контроллера, подключённого после запуска игры и не замеченного ею."));

    ImGui::Spacing();
    if (ImGui::Button(BbSettings::MenuText("Close", "Закрыть"))) {
        keep_open = false;
    }
    // bbport 0.51 (issues 74, 116): leaving the game from the pad (this menu opens with L3+R3). A
    // second press confirms; the window thread then closes like the window's own close button.
    static bool quit_armed;
    ImGui::SameLine();
    if (ImGui::Button(quit_armed ? BbSettings::MenuText("Press again to quit", "Нажмите ещё раз для выхода")
                                 : BbSettings::MenuText("Quit game", "Выйти из игры"))) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemFocused() && !ImGui::IsItemHovered()) {
        quit_armed = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BbSettings::MenuText("Settings are saved to bbport.ini",
                                             "Настройки сохраняются в bbport.ini"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    ImGui::Text(BbSettings::MenuText("%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f мс  %s"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3     ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa    ? "TAA"
                                                           : "");
    ImGui::End();
}

// RyuNET server announcements ("maintenance in 5 minutes"): the launcher writes
// ryunet_announce.txt next to bbport.ini (id, level, expires, text) and each new one is drawn
// over the picture for a few seconds. No input is taken: the game goes on underneath.
struct Announcement {
    std::string text;
    int level = 0; // 0 info, 1 warn, 2 urgent
    std::chrono::steady_clock::time_point until{};
};
std::mutex announce_mutex;
Announcement announce;
std::string announce_last_id;
std::chrono::steady_clock::time_point announce_checked{};
std::filesystem::file_time_type announce_stamp{};
constexpr float kAnnounceSeconds = 12.0f;

std::filesystem::path AnnouncePath() {
    const char* config = std::getenv("BB_CONFIG");
    const std::filesystem::path dir =
        config && *config ? std::filesystem::path(config).parent_path() : std::filesystem::path();
    return dir / "ryunet_announce.txt";
}

/// At most once a second: a new announcement in the file starts showing.
void PollAnnouncement() {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{announce_mutex};
    if (now - announce_checked < std::chrono::seconds(1)) {
        return;
    }
    announce_checked = now;
    std::error_code ec;
    const auto path = AnnouncePath();
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec || stamp == announce_stamp) {
        return;
    }
    announce_stamp = stamp;
    std::ifstream in(path, std::ios::binary);
    std::string line, id, text, level;
    long long expires = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("id=", 0) == 0) {
            id = line.substr(3);
        } else if (line.rfind("level=", 0) == 0) {
            level = line.substr(6);
        } else if (line.rfind("expires=", 0) == 0) {
            expires = std::atoll(line.c_str() + 8);
        } else if (line.rfind("text=", 0) == 0) {
            text = line.substr(5);
        }
    }
    if (id.empty() || text.empty() || id == announce_last_id || expires <= static_cast<long long>(std::time(nullptr))) {
        return;
    }
    announce_last_id = id;
    announce.text = text;
    announce.level = level == "urgent" ? 2 : level == "warn" ? 1 : 0;
    announce.until = now + std::chrono::milliseconds(static_cast<int>(kAnnounceSeconds * 1000));
    std::printf("Overlay: server announcement (%s): %s\n", level.c_str(), text.c_str());
}

bool AnnouncementActive() {
    PollAnnouncement();
    std::scoped_lock lock{announce_mutex};
    return std::chrono::steady_clock::now() < announce.until;
}

void AnnouncementBanner() {
    Announcement a;
    {
        std::scoped_lock lock{announce_mutex};
        a = announce;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= a.until) {
        return;
    }
    const float left = std::chrono::duration<float>(a.until - now).count();
    const float alpha = std::clamp(std::min((kAnnounceSeconds - left) / 0.3f, left / 1.0f), 0.0f, 1.0f);
    static const ImVec4 colors[3] = {ImVec4(0.12f, 0.33f, 0.62f, 1.0f), ImVec4(0.72f, 0.47f, 0.05f, 1.0f),
                                     ImVec4(0.68f, 0.12f, 0.12f, 1.0f)};
    const ImVec4 bg = colors[std::clamp(a.level, 0, 2)];
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x * 0.7f, 900.0f * base_scale);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 24.0f * base_scale),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * base_scale, 10.0f * base_scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bg.x, bg.y, bg.z, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
    ImGui::Begin("##announcement", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextDisabled("RyuNET");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(a.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel");
    ImGui::End();
}

// SacredFire: "Ultrawide?" with Yes / No and "Ask me every time". A choice that differs from the
// one the game started with restarts it (the size is patched into the executable at start).
void UltrawidePrompt() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##ultrawide", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Ultrawide %dx%d?", s.display_w.load(), s.display_h.load());
    ImGui::Separator();
    ImGui::TextUnformatted("Yes: the game fills the whole monitor (gameplay is drawn wider).");
    ImGui::TextUnformatted("No: 16:9 with black bars, as on the PS4.");
    ImGui::Spacing();
    int choice = -1;
    if (ImGui::Button("Yes, ultrawide")) choice = 1;
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    if (ImGui::Button("No, 16:9")) choice = 0;
    ImGui::Spacing();
    bool ask = s.ultrawide_ask;
    if (ImGui::Checkbox("Ask me every time the game starts", &ask)) {
        s.ultrawide_ask = ask;
    }
    ImGui::TextDisabled("Cross / Enter: choose   Insert or L3+R3: settings menu later");
    ImGui::End();
    if (choice >= 0) {
        BbSettings::SetUltrawide(choice == 1);
        BbSettings::Save();
        ultrawide_prompt = false;
        std::printf("Ultrawide: %s (ask every time: %d)\n", choice ? "on" : "off", int(s.ultrawide_ask));
        if (BbSettings::RenderSizeNeedsRestart()) {
            runtime_restart();
        }
    }
}

} // namespace

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert or L3+R3)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open || ultrawide_prompt;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (menu_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    if (initialized && !ultrawide_prompt_checked) {
        // Once, at the first presented frame: the first start, or every start with ultrawide_ask.
        ultrawide_prompt_checked = true;
        const auto& s = BbSettings::Get();
        if (BbSettings::UltrawideAvailable() && (s.ultrawide < 0 || s.ultrawide_ask)) {
            ultrawide_prompt = true;
        }
    }
    return initialized && (menu_open || prompt_active || ultrawide_prompt || BbSettings::Get().show_fps ||
                           AnnouncementActive());
}

bool MenuOpen() {
    return menu_open;
}

bool TakePadRescan() {
    return pad_rescan_requested.exchange(false, std::memory_order_relaxed);
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    return menu_open || prompt_active || ultrawide_prompt;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    AnnouncementBanner();
    if (ultrawide_prompt && !menu_open) {
        UltrawidePrompt();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
