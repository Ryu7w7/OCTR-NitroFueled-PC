// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "../../src/settings_lock.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

const char* PreferencesPath() {
    const char* env = std::getenv("BB_LAUNCHER_CONFIG");
    return env && env[0] ? env : Path();
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key.size() == 6 && key.rfind("port.", 0) == 0 && key[5] >= '1' && key[5] <= '4') {
        std::lock_guard lk{PortsMutex()};
        auto& slot = v.ports[size_t(key[5] - '1')];
        std::snprintf(slot.data(), slot.size(), "%s", value.c_str());
    } else if (key == "menu_language") {
        bool known = false;
        const MenuLanguage language = MenuLanguageOf(value.c_str(), &known);
        if (known) v.menu_language = language;
        else std::printf("Settings: unknown menu_language '%s' (expected en, ru, es, fr, pt, de or it)\n", value.c_str());
    } else if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "show_ping") {
        v.show_ping = i != 0;
    } else if (key == "voice_enabled") {
        v.voice_enabled = i != 0;
    } else if (key == "voice_open_mic") {
        v.voice_open_mic = i != 0;
    } else if (key == "voice_key") {
        v.voice_key = std::clamp(i, 4, 511);
    } else if (key == "voice_volume") {
        v.voice_volume = Clamp(f, 0.0f, 2.0f);
    } else if (key == "voice_mic_gain") {
        v.voice_mic_gain = Clamp(f, 0.0f, 4.0f);
    } else if (key == "voice_threshold") {
        v.voice_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "voice_proximity") {
        v.voice_proximity = i != 0;
    } else if (key == "voice_swap_sides") {
        v.voice_swap_sides = i != 0;
    } else if (key == "voice_input") {
        std::lock_guard lk{PortsMutex()};
        std::snprintf(v.voice_input.data(), v.voice_input.size(), "%s", value.c_str());
    } else if (key == "split_auto") {
        v.split_auto = i != 0;
    } else if (key == "friends_hide_activity") {
        v.friends_hide_activity = i != 0;
    } else if (key == "friends_hide_me") {
        v.friends_hide_me = i != 0;
    } else if (key == "friends_no_notices") {
        v.friends_no_notices = i != 0;
    } else if (key == "spectate_share") {
        v.spectate_share = i != 0;
    } else if (key == "chat_enabled") {
        v.chat_enabled = i != 0;
    } else if (key == "chat_public") {
        v.chat_public = i != 0;
    } else if (key == "chat_in_race") {
        v.chat_in_race = i != 0;
    } else if (key == "notice_position") {
        v.notice_position = std::clamp(i, 0, int(NoticeCount) - 1);
    } else if (key == "fps_limit") {
        v.fps_limit = std::clamp(i, 0, 1000);
    } else if (key == "async_pipelines") {
        v.async_pipelines = i != 0;
    } else if (key == "msaa") {
        v.msaa = i != 0;
    } else if (key == "starting_camera") {
        if (value == "-1" || value == "0" || value == "1") v.starting_camera = i;
    } else if (key == "graphics_tab") {
        v.graphics_tab = i != 0;
    } else if (key == "fullscreen") {
        v.fullscreen = i != 0;
    } else if (key == "borderless") {
        v.borderless = i != 0;
    } else if (key == "volume") {
        v.volume = std::clamp(i, 0, 200);
    } else if (key == "menu_pos") {
        if (value.empty()) v.menu_x = v.menu_y = -1.0f;
        float x = -1.0f, y = -1.0f;
        if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 1.0f && y >= 0.0f &&
            y <= 1.0f) {
            v.menu_x = x;
            v.menu_y = y;
        }
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "fsr1") {
        v.legacy_fsr1 = i != 0 ? 1 : 0; // v0.3 test builds: before fsr1 joined `temporal`
    } else if (key == "temporal") {
        for (int t = 0; t < TemporalCount; ++t) {
            if (value == TemporalNames[t]) {
                v.temporal = t;
                v.temporal_seen = true;
            }
        }
    } else if (key == "fsr3") {
        v.legacy_fsr3 = i != 0; // test builds before `temporal`
    } else if (key == "dlss_quality") {
        for (int q = 0; q < DlssQualityCount; ++q) {
            if (value == DlssQualityNames[q]) v.dlss_quality = q;
        }
    } else if (key == "dlss_output") {
        v.dlss_output = i == 2160 ? 1 : 0;
    } else if (key == "dlss_model") {
        v.dlss_model = 0;
        for (int m = 0; m < DlssModelCount; ++m) {
            if (value == DlssModelNames[m]) v.dlss_model = m;
        }
    } else if (key == "fsr3_sharpness") {
        v.fsr3_sharpness = Clamp(f, 0.0f, 1.0f);
    } else if (key == "render_res") {
        if (value.empty()) v.render_res = -1;
        for (int r = 0; r < RenderResCount; ++r) {
            if (i == RenderResHeights[r]) v.render_res = r;
        }
    } else if (key == "ultrawide") {
        v.ultrawide = value.empty() ? -1 : i != 0 ? 1 : 0;
    } else if (key == "aspect_ratio") {
        v.aspect_ratio = Aspect16x9;
        for (int a = 0; a < AspectRatioCount; ++a) {
            if (value == AspectRatioKeys[a]) v.aspect_ratio = a;
        }
        v.aspect_ratio_seen = true;
    } else if (key == "widescreen_layout" && !v.aspect_ratio_seen) {
        v.aspect_ratio = value == "21x9" ? Aspect21x9 : value == "32x9" ? Aspect32x9 : Aspect16x9;
    } else if (key == "aspect_output" || (key == "widescreen_output" && !v.aspect_output_seen)) {
        if (key == "aspect_output") {
            v.aspect_w = v.aspect_h = 0;
            v.aspect_output_seen = true;
        }
        int w = 0, h = 0;
        if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            v.aspect_w = w;
            v.aspect_h = h;
        }
    } else if (key == "ultrawide_ask") {
        v.ultrawide_ask = i != 0;
    } else if (key == "ultrawide_size") {
        if (value.empty()) v.ultrawide_w = v.ultrawide_h = 0;
        int w = 0, h = 0;
        if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 16384 &&
            h <= 8640) {
            v.ultrawide_w = w;
            v.ultrawide_h = h;
        }
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
        for (int g = 0; g < GamePatchCount; ++g) {
            if (key == GamePatches[g].key) {
                v.game_patches[g] = i != 0;
            }
        }
    }
}

using SettingsMap = std::map<std::string, std::string>;
std::mutex sync_mutex;
SettingsMap file_values, baseline;
std::filesystem::file_time_type file_time{};
std::string file_path;
std::chrono::steady_clock::time_point checked;

bool ReadSettings(SettingsMap& values) {
    std::ifstream file(PreferencesPath());
    if (!file) return false;
    std::string line;
    while (std::getline(file, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        const auto eq = line.find('=');
        if (start == std::string::npos || line[start] == '#' || line[start] == ';' || eq == std::string::npos) continue;
        std::string key = line.substr(start, eq - start), value = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t\r") + 1);
        value.erase(0, value.find_first_not_of(" \t\r"));
        value.erase(value.find_last_not_of(" \t\r") + 1);
        values[key] = value;
    }
    return !file.bad();
}

} // namespace

std::mutex& PortsMutex() { static std::mutex m; return m; }

Values& Get() {
    static Values values;
    return values;
}

const char* MenuText(const char* english, const char* russian) {
    switch (Get().menu_language.load()) {
    case MenuLanguage::Russian: return russian;
    case MenuLanguage::Spanish: return Spanish(english);
    case MenuLanguage::French: return MoreLanguages(english, 0);
    case MenuLanguage::Portuguese: return MoreLanguages(english, 1);
    case MenuLanguage::German: return MoreLanguages(english, 2);
    case MenuLanguage::Italian: return MoreLanguages(english, 3);
    default: return english;
    }
}

namespace {
constexpr struct { MenuLanguage language; const char* code; } kMenuLanguageCodes[] = {
    {MenuLanguage::English, "en"}, {MenuLanguage::Russian, "ru"}, {MenuLanguage::Spanish, "es"},
    {MenuLanguage::French, "fr"}, {MenuLanguage::Portuguese, "pt"}, {MenuLanguage::German, "de"},
    {MenuLanguage::Italian, "it"},
};
} // namespace

const char* MenuLanguageCode(MenuLanguage language) {
    for (const auto& entry : kMenuLanguageCodes) {
        if (entry.language == language) return entry.code;
    }
    return "en";
}

MenuLanguage MenuLanguageOf(const char* code, bool* ok) {
    for (const auto& entry : kMenuLanguageCodes) {
        if (code && !std::strcmp(code, entry.code)) {
            if (ok) *ok = true;
            return entry.language;
        }
    }
    if (ok) *ok = false;
    return MenuLanguage::English;
}

static std::vector<std::pair<std::string, std::string>> Snapshot(const Values& v);
static SettingsMap CurrentSettings() {
    const auto values = Snapshot(Get());
    return {values.begin(), values.end()};
}
static void RememberSettings() {
    file_values.clear();
    ReadSettings(file_values);
    baseline = CurrentSettings();
    std::error_code error;
    file_time = std::filesystem::last_write_time(PreferencesPath(), error);
    file_path = PreferencesPath();
}

void Load() {
    std::scoped_lock lock{sync_mutex};
    auto& v = Get();
    v.aspect_ratio = Aspect16x9;
    v.aspect_w = v.aspect_h = 0;
    v.aspect_ratio_seen = v.aspect_output_seen = false;
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    // bbport: the menu's language until one is chosen in it (menu_language in bbport.ini): the
    // launcher's (BB_MENU_LANGUAGE), else the system's, as the launcher picks its own.
    {
        const char* lang = std::getenv("BB_MENU_LANGUAGE");
        if (!lang || !*lang) {
            for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
                if (const char* value = std::getenv(key); value && *value) {
                    lang = value;
                    break;
                }
            }
        }
        auto is = [&](char a, char b) {
            return lang && (lang[0] == a || lang[0] == a - 32) && (lang[1] == b || lang[1] == b - 32);
        };
        v.menu_language = is('r', 'u')   ? MenuLanguage::Russian
                          : is('e', 's') ? MenuLanguage::Spanish
                          : is('f', 'r') ? MenuLanguage::French
                          : is('p', 't') ? MenuLanguage::Portuguese
                          : is('d', 'e') ? MenuLanguage::German
                          : is('i', 't') ? MenuLanguage::Italian
                                         : MenuLanguage::English;
        // The launcher's own languages are English and Spanish: with English there and the game in
        // French, Portuguese, German or Italian (BB_LANGUAGE, PS4 language ids), the menu follows the
        // game until a language is chosen in the menu (menu_language in bbport.ini).
        if (v.menu_language == MenuLanguage::English) {
            if (const char* game = std::getenv("BB_LANGUAGE"); game && *game) {
                const int id = std::atoi(game);
                v.menu_language = (id == 2 || id == 22) ? MenuLanguage::French
                                  : (id == 17 || id == 7) ? MenuLanguage::Portuguese
                                  : id == 4             ? MenuLanguage::German
                                  : id == 5             ? MenuLanguage::Italian
                                                        : MenuLanguage::English;
            }
        }
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // SacredFire: bbport's temporal upscalers and object motion vectors read the scene of the game
    // bbport was built for (its render targets, constants and vertex streams), not this one; they
    // stay off. BB_UPSCALER / BB_OBJECT_MOTION below still turn them on for experiments.
    v.upscaler = UpscalerOff;
    v.object_motion = false;
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    // SacredFire: BB_FULLSCREEN=1 starts fullscreen; the menu toggles it later (window.cpp
    // applies the setting on the window thread, so the checkbox takes effect at once).
    if (!v.temporal_seen) {
        v.temporal = v.legacy_fsr3 ? TemporalFsr3 : v.legacy_fsr1 == 0 ? TemporalOff : TemporalFsr1;
    }
    if (const char* fsr3 = std::getenv("BB_FSR3")) {
        v.temporal = fsr3[0] == '1' ? TemporalFsr3 : TemporalOff;
    }
    if (const char* temporal = std::getenv("BB_TEMPORAL")) {
        for (int t = 0; t < TemporalCount; ++t) {
            if (std::strcmp(temporal, TemporalNames[t]) == 0) v.temporal = t;
        }
    }
    if (const char* full = std::getenv("BB_FULLSCREEN")) {
        v.fullscreen = full[0] == '1';
    }
    if (const char* bdr = std::getenv("BB_BORDERLESS")) {
        v.borderless = bdr[0] == '1';
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    for (int g = 0; g < GamePatchCount; ++g) {
        v.startup_game_patches[g] = v.game_patches[g];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_render_res = v.render_res;
    v.startup_ultrawide = v.ultrawide == 1 && v.ultrawide_w > 0;
    v.startup_ultrawide_w = v.ultrawide_w;
    v.startup_ultrawide_h = v.ultrawide_h;
    v.startup_aspect_ratio = v.aspect_ratio;
    v.startup_temporal = v.temporal;
    v.startup_dlss_quality = v.dlss_quality;
    v.startup_dlss_output = v.dlss_output;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    RememberSettings();
}

bool RenderSizeNeedsRestart() {
    const auto& v = Get();
    // As scripts/patches.py render_plan: a DLSS quality mode decides, else render_res.
    const auto key = [](int temporal, int quality, int output, int render_res) {
        return temporal == TemporalDlss && quality > 0 ? 100 + quality * 2 + output : render_res;
    };
    const bool ultrawide = v.ultrawide == 1 && v.ultrawide_w > 0;
    if (ultrawide != v.startup_ultrawide ||
        (ultrawide && (v.ultrawide_w != v.startup_ultrawide_w || v.ultrawide_h != v.startup_ultrawide_h))) {
        return true;
    }
    if (v.LiveGraphics())
        return v.dlss_output != v.startup_dlss_output;
    if (ultrawide) {
        return false; // the monitor's size decides, not render_res
    }
    return key(v.temporal, v.dlss_quality, v.dlss_output, v.render_res) !=
           key(v.startup_temporal, v.startup_dlss_quality, v.startup_dlss_output,
               v.startup_render_res);
}

bool UltrawideAvailable() {
    const auto& v = Get();
    // Wider than 16:9 by more than rounding (2560x1080, 3440x1440, 5120x1440, ...).
    return v.display_w > 0 && v.display_h > 0 && v.display_w * 9 > v.display_h * 16 + v.display_h / 2;
}

void SetUltrawide(bool on) {
    auto& v = Get();
    v.ultrawide = on ? 1 : 0;
    if (on && v.display_w > 0 && v.display_h > 0) {
        v.ultrawide_w = v.display_w.load();
        v.ultrawide_h = v.display_h.load();
    }
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

bool Fsr4Available() {
    // As vk_fsr4.cpp's AssetDir; the notice comes with every bundle.
    static const bool assets = [] {
        const char* dir = std::getenv("BB_FSR4_DIR");
        std::error_code ec;
        return std::filesystem::is_regular_file(
            std::filesystem::path(dir && dir[0] ? dir : "fsr4_shaders") / "LICENSE-FSR4-v07.txt", ec);
    }();
    return assets && Get().fsr4_supported;
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

static std::vector<std::pair<std::string, std::string>> Snapshot(const Values& v) {
    // The keys the menu writes, in this order; the rest of the file stays as it is: the
    // launcher's controls (key.* / pad.*) and its other keys, comments.
    std::vector<std::pair<std::string, std::string>> keys;
    const auto put = [&](const char* key, std::string value) { keys.emplace_back(key, std::move(value)); };
    const auto fixed = [](float value, int digits) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", digits, value);
        return std::string(text);
    };
    const auto flag = [](bool value) { return std::string(value ? "1" : "0"); };
    {
        std::lock_guard lk{PortsMutex()};
        for (int p = 0; p < ControllerPorts; ++p) {
            put(("port." + std::to_string(p + 1)).c_str(), std::string(v.ports[size_t(p)].data()));
        }
    }
    put("split_auto", flag(v.split_auto));
    put("menu_language", MenuLanguageCode(v.menu_language));
    put("upscaler", UpscalerName(v.upscaler));
    put("preset", std::to_string(v.preset.load()));
    put("sharpen", flag(v.sharpen));
    put("sharpness", fixed(v.sharpness, 2));
    put("jitter", flag(v.jitter));
    put("reactive", flag(v.reactive));
    put("object_motion", flag(v.object_motion));
    put("reactive_scale", fixed(v.reactive_scale, 2));
    put("reactive_threshold", fixed(v.reactive_threshold, 2));
    put("reactive_max", fixed(v.reactive_max, 2));
    put("debug_view", std::to_string(v.debug_view.load()));
    put("show_fps", flag(v.show_fps));
    put("show_ping", flag(v.show_ping));
    put("voice_enabled", flag(v.voice_enabled));
    put("voice_open_mic", flag(v.voice_open_mic));
    put("voice_key", std::to_string(v.voice_key.load()));
    put("voice_volume", fixed(v.voice_volume, 2));
    put("voice_mic_gain", fixed(v.voice_mic_gain, 2));
    put("voice_threshold", fixed(v.voice_threshold, 3));
    put("voice_proximity", flag(v.voice_proximity));
    put("voice_swap_sides", flag(v.voice_swap_sides));
    {
        std::lock_guard lk{PortsMutex()};
        put("voice_input", std::string(v.voice_input.data()));
    }
    put("friends_hide_activity", flag(v.friends_hide_activity));
    put("friends_hide_me", flag(v.friends_hide_me));
    put("friends_no_notices", flag(v.friends_no_notices));
    put("spectate_share", flag(v.spectate_share));
    put("chat_enabled", flag(v.chat_enabled));
    put("chat_public", flag(v.chat_public));
    put("chat_in_race", flag(v.chat_in_race));
    put("notice_position", std::to_string(v.notice_position.load()));
    put("fps_limit", std::to_string(v.fps_limit.load()));
    put("async_pipelines", flag(v.async_pipelines));
    put("msaa", flag(v.msaa));
    put("starting_camera", std::to_string(v.starting_camera.load()));
    put("graphics_tab", flag(v.graphics_tab));
    put("fullscreen", flag(v.fullscreen));
    put("borderless", flag(v.borderless));
    put("volume", std::to_string(v.volume.load()));
    put("fsr4_auto_exposure", flag(v.fsr4_auto_exposure));
    put("fsr4_invert_jitter", flag(v.fsr4_invert_jitter));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        put(Effects[e].key, flag(v.effects[e]));
    }
    for (int g = 0; g < GamePatchCount; ++g) {
        put(GamePatches[g].key, flag(v.game_patches[g]));
    }
    put("model_lod", std::to_string(v.model_lod.load()));
    if (v.render_res >= 0) put("render_res", std::to_string(RenderResHeights[v.render_res]));
    if (v.ultrawide >= 0) put("ultrawide", flag(v.ultrawide == 1));
    put("ultrawide_ask", flag(v.ultrawide_ask));
    {
        const int ratio = std::clamp(v.aspect_ratio.load(), 0, AspectRatioCount - 1);
        put("aspect_ratio", AspectRatioKeys[ratio]);
        const bool wide = ratio == Aspect21x9 || ratio == Aspect32x9;
        put("widescreen_layout", wide ? AspectRatioKeys[ratio] : "off");
        if (v.aspect_w > 0 && v.aspect_h > 0) {
            const std::string output = std::to_string(v.aspect_w.load()) + "x" + std::to_string(v.aspect_h.load());
            put("aspect_output", output);
            if (wide) put("widescreen_output", output);
        }
    }
    if (v.ultrawide_w > 0) {
        put("ultrawide_size", std::to_string(v.ultrawide_w.load()) + "x" + std::to_string(v.ultrawide_h.load()));
    }
    put("temporal", TemporalNames[std::clamp(v.temporal.load(), 0, TemporalCount - 1)]);
    put("dlss_quality", DlssQualityNames[std::clamp(v.dlss_quality.load(), 0, DlssQualityCount - 1)]);
    put("dlss_output", std::to_string(DlssOutputHeights[v.dlss_output ? 1 : 0]));
    put("dlss_model", DlssModelNames[std::clamp(v.dlss_model.load(), 0, DlssModelCount - 1)]);
    put("fsr3_sharpness", fixed(v.fsr3_sharpness, 2));
    put("output_res", std::to_string(OutputWidths[v.output_res]) + "x" +
                          std::to_string(OutputHeights[v.output_res]));
    // Read by run.sh at start.
    put("live_resolution", v.live_resolution < 0 ? "auto" : flag(v.live_resolution != 0));
    if (v.menu_x >= 0.0f && v.menu_y >= 0.0f) {
        put("menu_pos", fixed(v.menu_x, 4) + "," + fixed(v.menu_y, 4));
    }

    return keys;
}

static bool RefreshLocked(bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - checked < std::chrono::milliseconds(250)) return false;
    checked = now;
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(PreferencesPath(), error);
    if (error || (!force && file_path == PreferencesPath() && stamp == file_time)) return false;
    SettingsMap disk;
    if (!ReadSettings(disk)) return false;
    const auto before = CurrentSettings();
    auto& values = Get();
    const int old_resolution = values.render_res;
    auto changes = disk;
    Values defaults;
    for (int e = 0; e < EffectCount; ++e) defaults.effects[e] = Effects[e].default_on;
    const auto default_keys = Snapshot(defaults);
    const SettingsMap default_values(default_keys.begin(), default_keys.end());
    for (const auto& [key, value] : file_values) {
        if (!disk.contains(key)) {
            const auto found = default_values.find(key);
            changes[key] = found == default_values.end() ? "" : found->second;
        }
    }
    for (const auto& [key, value] : changes) {
        const auto old = file_values.find(key);
        const auto local = before.find(key);
        const auto saved = baseline.find(key);
        if ((old == file_values.end() || old->second != value || !disk.contains(key)) &&
            (local == before.end() || (saved != baseline.end() && local->second == saved->second)))
            Set(values, key, value);
    }
    // The legacy upscaler reads another game's buffers; keep the same guard as Load().
    if (!std::getenv("BB_UPSCALER")) values.upscaler = UpscalerOff;
    if (!std::getenv("BB_OBJECT_MOTION")) values.object_motion = false;
    const auto after = CurrentSettings();
    if (values.LiveGraphics() && values.render_res != old_resolution)
        values.native_resolution_requested = true;
    for (const auto& [key, value] : after) {
        const auto local = before.find(key);
        const auto saved = baseline.find(key);
        if (local == before.end() || (saved != baseline.end() && local->second == saved->second)) baseline[key] = value;
    }
    file_values = std::move(disk);
    file_time = stamp;
    file_path = PreferencesPath();
    return before != after;
}

static bool WriteSettings(const char* path,
                          const std::vector<std::pair<std::string, std::string>>& keys) {
    std::string out;
    bool had_lines = false;
    std::vector<bool> written(keys.size());
    if (FILE* file = std::fopen(path, "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), file)) {
            had_lines = true;
            std::string text{line};
            text.erase(text.find_last_not_of("\r\n") + 1);
            const auto eq = text.find('=');
            if (!text.empty() && text[0] != '#' && eq != std::string::npos) {
                std::string key = text.substr(0, eq);
                key.erase(key.find_last_not_of(" \t") + 1);
                key.erase(0, key.find_first_not_of(" \t"));
                const auto it = std::find_if(keys.begin(), keys.end(),
                                             [&](const auto& k) { return k.first == key; });
                if (it != keys.end()) {
                    out += it->first + "=" + it->second + "\n";
                    written[size_t(it - keys.begin())] = true;
                    continue;
                }
            }
            out += text + "\n";
        }
        std::fclose(file);
    }
    if (!had_lines) {
        out = "# bbport settings (in-game menu: Insert / L3+R3)\n";
    }
    for (size_t k = 0; k < keys.size(); ++k) {
        if (!written[k]) {
            out += keys[k].first + "=" + keys[k].second + "\n";
        }
    }
    // Replaced whole: a crash while writing leaves the old file.
    const std::string temporary = std::string(path) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    bool ok = file && std::fwrite(out.data(), 1, out.size(), file) == out.size();
    ok = file && std::fclose(file) == 0 && ok;
    // std::filesystem::rename replaces an existing file on Windows too (std::rename does not).
    std::error_code error;
    if (ok) {
        std::filesystem::rename(temporary, path, error);
    }
    if (!ok || error) {
        std::printf("Settings: cannot write %s (%s)\n", path, error ? error.message().c_str() : "write failed");
        return false;
    }
    return true;
}

bool Refresh(bool force) {
    std::scoped_lock lock{sync_mutex};
    const bool changed = RefreshLocked(force);
    if (changed && std::strcmp(Path(), PreferencesPath()) != 0) {
        // Runtime readers and Windows restarts still use the mod session's effective settings.
        SettingsLock session_lock(Path());
        if (session_lock) WriteSettings(Path(), Snapshot(Get()));
        else std::printf("Settings: cannot lock %s\n", Path());
    }
    return changed;
}

void Save() {
    std::scoped_lock lock{sync_mutex};
    const char* preferences = PreferencesPath();
    SettingsLock file_lock(preferences);
    if (!file_lock) { std::printf("Settings: cannot lock %s\n", preferences); return; }
    RefreshLocked(true);
    const auto& v = Get();
    const auto snapshot = Snapshot(v);
    auto keys = snapshot;
    const bool session = std::strcmp(Path(), preferences) != 0;
    if (session) {
        // Persist user edits, not the mod's initial overrides or generated defaults.
        std::erase_if(keys, [](const auto& entry) {
            const auto saved = baseline.find(entry.first);
            return saved != baseline.end() && saved->second == entry.second;
        });
    }
    if (!keys.empty() && !WriteSettings(preferences, keys)) return;
    if (session) {
        SettingsLock session_lock(Path());
        if (!session_lock) { std::printf("Settings: cannot lock %s\n", Path()); return; }
        if (!WriteSettings(Path(), snapshot)) return;
    }
    RememberSettings();
    baseline = SettingsMap(snapshot.begin(), snapshot.end());
    std::printf("Settings: saved %s (", preferences);
    for (int g = 0; g < GamePatchCount; ++g) {
        std::printf("%s%s=%d", g ? ", " : "", GamePatches[g].key, int(v.game_patches[g]));
    }
    std::printf(")\n");
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

} // namespace BbSettings

namespace AmdGpu {
bool BbNoMsaa() {
    static const bool value = [] {
        if (const char* env = std::getenv("BB_NO_MSAA")) {
            return env[0] == '1';
        }
        return !BbSettings::Get().msaa.load();
    }();
    return value;
}
} // namespace AmdGpu
