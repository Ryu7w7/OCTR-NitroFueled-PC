// SPDX-License-Identifier: GPL-2.0-or-later
// Shared CTR-NitroFueled-PC launcher. Sets up the game folder (the 1.21 update merged over
// the base game with hard links), edits the settings the game reads (bbport.ini, the same keys
// as the in-game menu) and starts run.py with the bundled Python, showing its output.
// Dear ImGui UI; Win32/D3D11 and Linux/SDL3 services live in their platform directories.
//
// Files: launcher.ini (this launcher: game folders, language, display...) and bbport.ini (the
// game's settings) in the port folder; run.py writes last_run.log there.

#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <array>
#include <cmath>
#include <cwctype>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "ui/table.h"
#include "../src/settings_lock.h"

#include "windows/discord_link.h"
#include "windows/nat_check.h"
#include "windows/port_update.h"
#include "windows/settings_import.h"
#include "windows/discord_presence.h"
#include "windows/launcher_locales.h"

namespace fs = std::filesystem;

namespace {

// The Americas build (CUSA13795) and the European one (CUSA14876, SacredFire EU): the port runs both.
constexpr const char* kTitleIds[] = {"CUSA13795", "CUSA14876"};
bool IsCtrTitle(const std::string& title) {
    for (const char* id : kTitleIds)
        if (title == id) return true;
    return false;
}
constexpr const char* kUpdateVersion = "01.21";
constexpr const char* kRepoUrl = "https://github.com/GetLostIn/CTR-NitroFueled-PC";
constexpr const char* kSiteUrl = "https://octr.ryu7w7.xyz/";
constexpr const char* kPortRepoUrl = "https://github.com/Ryu7w7/OCTR-NitroFueled-PC";
// The 1.21 update alone is about 8.5 GB; base game + update is much larger.
constexpr std::uintmax_t kCompleteGameMinBytes = 10ull << 30;

struct LauncherArtwork {
    LauncherTexture* texture = nullptr;
    unsigned width = 0;
    unsigned height = 0;
};
LauncherArtwork g_hero_art;
LauncherArtwork g_logo_art;
LauncherArtwork g_menu_background;
LauncherArtwork g_pad_xbox_art;  // art/pad-xbox.png, art/pad-playstation.png: the Gamepad page's picture
LauncherArtwork g_pad_ps_art;
ImFont* g_ctr_font = nullptr;
DragRect g_titlebar_drag{};
float g_ui_scale = 1.0f;
#ifdef _WIN32
extern ID3D11Device* g_device;
#endif

// ---------------------------------------------------------------------------------------------
// Text helpers

std::wstring Widen(const std::string& s);

std::string Narrow(const std::wstring& w);

std::wstring WidePath(const fs::path& p);
std::string PathText(const fs::path& p) { const auto text = p.u8string(); return std::string(text.begin(), text.end()); }
fs::path TextPath(const std::string& s) { return fs::u8path(s); }
std::wstring WidePath(const fs::path& p) { return Widen(PathText(p)); }

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteTextAtomic(const fs::path& p, const std::string& text) {
    fs::path temp = p;
    temp += L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    return ReplaceFile(temp, p);
}

std::string FormatBytes(std::uintmax_t bytes) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
    return buf;
}

const ImVec4 kGreen{0.45f, 0.85f, 0.45f, 1.0f};
const ImVec4 kRed{1.0f, 0.45f, 0.40f, 1.0f};
// The game found no Vulkan 1.3 (GPU, driver or loader): UnsupportedVulkanExitCode in vk_platform.h.
constexpr unsigned long kUnsupportedGpuCode = 24;
// Windows could not give the game its memory (error 1455): OUT_OF_MEMORY_CODE in run.py.
constexpr unsigned long kOutOfMemoryCode = 26;
const ImVec4 kYellow{1.0f, 0.80f, 0.35f, 1.0f};
const ImVec4 kGrey{0.65f, 0.65f, 0.68f, 1.0f};
// The Achievements page (TrophiesTab, from GetLostIn's launcher v0.4, GPL-2.0) draws with its
// cards, colours and heading font; its icon-font glyphs are left out (this launcher has none).
const ImVec4 kText{0.92f, 0.93f, 0.96f, 1.0f};
const ImVec4 kBlue{0.30f, 0.64f, 1.0f, 1.0f};
const ImVec4 kCard{0.118f, 0.133f, 0.188f, 1.0f};
const ImVec4 kFrame{0.149f, 0.169f, 0.231f, 1.0f};
const ImVec4 kBorder{0.173f, 0.192f, 0.259f, 1.0f};
#define ICON_INFO ""
#define ICON_CHECK ""
#define ICON_FOLDER ""
#define ICON_UNDO ""
#define ICON_STAR ""
ImFont* g_font_heading = nullptr;
ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
/// A rounded panel that grows with its content.
void BeginCard(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * g_ui_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_ui_scale, 12.0f * g_ui_scale));
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------------------------
// Language: Spanish or English, chosen in the launcher (default: Windows' language).

bool g_spanish = false;
std::string g_language = "auto";
std::string g_active_locale = "en";

std::string DetectSystemLocale();

void ApplyLauncherLanguage() {
    g_active_locale = g_language == "auto" ? DetectSystemLocale() : g_language;
    g_spanish = g_active_locale == "es";
}

const char* T(const char* es, const char* en) {
    if (g_active_locale == "es") return es;
    if (g_active_locale == "en") return en;
    const auto& all = GeneratedLocales();
    const auto language = all.find(g_active_locale);
    if (language != all.end()) {
        const auto value = language->second.find(en);
        if (value != language->second.end()) return value->second.c_str();
    }
    return en;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------------
// key=value files, edited in place: comments, order and unknown keys are kept.

struct Ini {
    std::vector<std::string> lines;
    std::map<std::string, std::string> values;
    std::map<std::string, std::string> saved;

    bool Load(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (file.bad()) return false;
        lines.clear();
        values.clear();
        erased.clear();
        std::stringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            const auto t = Trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            values[Trim(t.substr(0, eq))] = Trim(t.substr(eq + 1));
        }
        saved = values;
        return true;
    }
    bool Refresh(const fs::path& path) {
        Ini disk;
        if (!disk.Load(path)) return false;
        for (const auto& [key, value] : values) {
            const auto previous = saved.find(key);
            if (previous == saved.end() || previous->second != value) disk.values[key] = value;
        }
        for (const auto& key : erased) disk.values.erase(key);
        disk.erased = erased;
        *this = std::move(disk);
        return true;
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    bool Flag(const std::string& key, bool fallback) const {
        const auto v = Lower(Get(key));
        return v.empty() ? fallback : v != "0" && v != "no" && v != "false" && v != "off";
    }
    int Int(const std::string& key, int fallback) const {
        const auto v = Get(key);
        if (v.empty()) return fallback;
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        return end && *end == '\0' ? int(n) : fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        values[key] = value;
        erased.erase(key);
    }
    // Removes the key: its line is dropped on Save.
    void Erase(const std::string& key) {
        values.erase(key);
        erased.insert(key);
    }
    std::set<std::string> erased;
    bool Save(const fs::path& path, const char* header) {
        std::error_code error;
        if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), error);
        if (error) return false;
        SettingsLock lock(path);
        if (!lock) return false;
        if (fs::exists(path, error) && !Refresh(path)) return false;
        if (error) return false;
        std::vector<std::string> out;
        std::map<std::string, bool> written;
        for (const auto& line : lines) {
            const auto t = Trim(line);
            const auto eq = t.find('=');
            if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos) {
                const auto key = Trim(t.substr(0, eq));
                if (erased.count(key)) continue;
                const auto it = values.find(key);
                if (it != values.end()) {
                    if (!written[key]) out.push_back(key + "=" + it->second);
                    written[key] = true;
                    continue;
                }
            }
            out.push_back(line);
        }
        if (lines.empty() && header) out.push_back(header);
        for (const auto& [key, value] : values) {
            if (!written[key]) out.push_back(key + "=" + value);
        }
        std::string text;
        for (const auto& line : out) text += line + "\n";
        if (!WriteTextAtomic(path, text)) return false;
        lines = out;
        saved = values;
        erased.clear();
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// param.sfo: the game's title, version and category (gd = game, gp = patch).

std::map<std::string, std::string> ParseSfo(const std::string& data) {
    std::map<std::string, std::string> out;
    auto u32 = [&](size_t at) -> uint32_t {
        if (at + 4 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8 |
               uint32_t(uint8_t(data[at + 2])) << 16 | uint32_t(uint8_t(data[at + 3])) << 24;
    };
    auto u16 = [&](size_t at) -> uint32_t {
        if (at + 2 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8;
    };
    if (data.size() < 20 || data.compare(0, 4, std::string("\0PSF", 4)) != 0) return out;
    const uint32_t keys = u32(8), values = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= data.size(); ++i) {
        const size_t entry = 20 + size_t(i) * 16;
        const uint32_t key_off = u16(entry), fmt = u16(entry + 2), len = u32(entry + 4), off = u32(entry + 12);
        const size_t key_at = size_t(keys) + key_off;
        if (key_at >= data.size()) continue;
        const size_t key_end = data.find('\0', key_at);
        if (key_end == std::string::npos) continue;
        const std::string key = data.substr(key_at, key_end - key_at);
        const size_t value_at = size_t(values) + off;
        if (value_at + len > data.size()) continue;
        if (fmt == 0x0404) {
            out[key] = std::to_string(u32(value_at));
        } else {
            std::string v = data.substr(value_at, len);
            while (!v.empty() && v.back() == '\0') v.pop_back();
            out[key] = v;
        }
    }
    return out;
}

std::map<std::string, std::string> ReadSfo(const fs::path& file) { return ParseSfo(ReadText(file)); }

enum class Kind { Empty, Missing, NotGame, OtherTitle, OtherVersion, Base, Update, Complete };

struct FolderInfo {
    Kind kind = Kind::Empty;
    std::string title, version, category;
    std::uintmax_t bytes = 0;
    bool split = false;  // Image0 + Sc0 (PS4 PKG Tool): needs preparing before it runs
};

std::uintmax_t FolderBytes(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += it->file_size(e2);
    }
    return total;
}

// Where a dump keeps its files. Most tools give the PS4 layout (eboot.bin and sce_sys\param.sfo
// side by side); PS4 PKG Tool splits it into Image0\ (eboot.bin, sce_module, the data) and Sc0\
// (what goes in sce_sys). Such a folder is "split": preparing the game flattens it.
struct Layout {
    fs::path files, sys;  // the folder that becomes the game root, the one that becomes sce_sys
    bool split = false;
    explicit operator bool() const { return !files.empty(); }
};

Layout FindLayout(const fs::path& dir) {
    std::error_code ec;
    const auto has = [&](const fs::path& p) { return fs::is_regular_file(p, ec); };
    const bool flat = has(dir / L"eboot.bin") && has(dir / L"sce_sys" / L"param.sfo");
    const fs::path image = dir / L"Image0";
    // A folder fixed by hand keeps Image0 next to a copied eboot.bin: it is flat only once
    // sce_module is in the root too.
    if (flat && (!has(image / L"eboot.bin") || fs::is_directory(dir / L"sce_module", ec))) return {dir, dir / L"sce_sys", false};
    if (!has(image / L"eboot.bin")) return {};
    for (const fs::path& sys : {image / L"sce_sys", dir / L"Sc0", dir / L"sce_sys"}) {
        if (has(sys / L"param.sfo")) return {image, sys, true};
    }
    return {};
}

// A folder picked one level too high (the folder that holds CUSA13795) stands for its only game.
fs::path GameRoot(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty() || FindLayout(dir)) return dir;
    // Image0 itself, picked from a PS4 PKG Tool dump, stands for the dump.
    if (PathText(dir.filename()) == "Image0" && FindLayout(dir.parent_path())) return dir.parent_path();
    fs::path found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && FindLayout(it->path())) {
            if (!found.empty()) return dir;  // more than one: the player chooses
            found = it->path();
        }
    }
    return found.empty() ? dir : found;
}

/// The files of a dump as (source, path in the PS4 layout).
std::vector<std::pair<fs::path, fs::path>> LayoutFiles(const fs::path& dir, std::error_code& ec) {
    std::vector<std::pair<fs::path, fs::path>> files;
    const Layout layout = FindLayout(dir);
    if (!layout) return files;
    auto add = [&](const fs::path& from, const fs::path& prefix) {
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) files.emplace_back(it->path(), prefix / fs::relative(it->path(), from, e2));
        }
    };
    add(layout.files, {});
    if (!ec && layout.split && layout.sys != layout.files / L"sce_sys") add(layout.sys, L"sce_sys");
    return files;
}

FolderInfo Analyze(const fs::path& dir) {
    FolderInfo info;
    if (dir.empty()) return info;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        info.kind = Kind::Missing;
        return info;
    }
    const Layout layout = FindLayout(dir);
    if (!layout) {
        info.kind = Kind::NotGame;
        return info;
    }
    info.split = layout.split;
    const auto sfo = ReadSfo(layout.sys / L"param.sfo");
    auto get = [&](const char* k) {
        const auto it = sfo.find(k);
        return it == sfo.end() ? std::string() : it->second;
    };
    info.title = get("TITLE_ID");
    info.version = get("APP_VER");
    info.category = get("CATEGORY");
    if (!IsCtrTitle(info.title)) {
        info.kind = Kind::OtherTitle;
    } else if (info.category == "gd") {
        info.kind = Kind::Base;
    } else if (info.version != kUpdateVersion) {
        info.kind = Kind::OtherVersion;
    } else {
        info.bytes = FolderBytes(layout.split ? layout.files : dir);
        info.kind = info.bytes >= kCompleteGameMinBytes ? Kind::Complete : Kind::Update;
    }
    return info;
}

// ---------------------------------------------------------------------------------------------
// Processes

fs::path ExeDir();

/// The port folder: the launcher's own (portable package), or the source tree above out\.
fs::path FindRoot() {
    fs::path dir = ExeDir();
    for (int i = 0; i < 3 && !dir.empty(); ++i) {
        std::error_code ec;
        if (fs::is_regular_file(dir / L"run.py", ec)) return dir;
        dir = dir.parent_path();
    }
    return ExeDir();
}

std::wstring Quote(const std::wstring& s) {
#ifdef _WIN32
    return L"\"" + s + L"\"";
#else
    std::wstring out = L"'";
    for (wchar_t c : s) out += c == L'\'' ? L"'\\''" : std::wstring(1, c);
    return out + L"'";
#endif
}

struct Python {
    std::wstring exe;
    std::wstring args;  // before the script, e.g. L"-3 "
    std::string description;
};

std::optional<Python> FindPython(const fs::path& root);

struct Captured {
    bool started = false;
    unsigned long code = 1;
    std::string output;
};

/// Runs a console program without a window and returns its output (stdout + stderr).
Captured RunCaptured(const std::wstring& exe, const std::wstring& command_line, const fs::path& cwd,
                     DWORD timeout_ms);

void OpenInShell(const fs::path& target);
fs::path PortFolder();

std::vector<fs::path> PickFiles(LauncherWindow owner, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter);

std::optional<fs::path> PickFolder(LauncherWindow owner, const wchar_t* title, const fs::path& start);

// ---------------------------------------------------------------------------------------------
// The launcher

struct Choice {
    const char* es;
    const char* en;
    const char* value;
};

// PS4 system language codes (BB_LANGUAGE) of the languages CUSA13795 (the Americas build) has:
// English (1, US), Latin American Spanish (20), French (2) and Brazilian Portuguese (17). Other
// codes (Spain's Spanish 3, German, Italian...) are not in this build and fall back to English.
constexpr Choice kGameLanguages[] = {
    {"Inglés", "English", "1"},
    {"Español latinoamericano", "Spanish (Latin America)", "20"},
    {"Francés", "French", "2"},
    {"Portugués (Brasil)", "Portuguese (Brazil)", "17"},
};
// The European build (CUSA14876) has English, Spain's Spanish (3), French, German (4), Italian (5)
// and Arabic (21): the .lng texts in its archives.
constexpr Choice kGameLanguagesEU[] = {
    {"Inglés", "English", "1"},
    {"Español (España)", "Spanish (Spain)", "3"},
    {"Francés", "French", "2"},
    {"Alemán", "German", "4"},
    {"Italiano", "Italian", "5"},
    {"Árabe", "Arabic", "21"},
};
constexpr Choice kPresentModes[] = {
    {"Mailbox (recomendado)", "Mailbox (recommended)", "Mailbox"},
    {"FIFO (VSync)", "FIFO (VSync)", "Fifo"},
    {"FIFO relajado", "FIFO Relaxed", "FifoRelaxed"},
    {"Inmediato (sin VSync)", "Immediate (no VSync)", "Immediate"},
};
constexpr int kFpsLimits[] = {0, 30, 60, 72, 90, 120, 144, 240};

struct SaveParam {  // OrbisSaveDataParam
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
};
static_assert(sizeof(SaveParam) == 1328, "OrbisSaveDataParam layout");

struct SaveSlot {
    std::string dir, title, subtitle, detail, modified;
    std::uintmax_t bytes = 0;
};

struct Backup {
    fs::path path;
    std::string name, modified;
    std::uintmax_t bytes = 0;
};

struct ModOption {
    bool advanced=false;
    std::string id, type, label, description, value, default_value, row, column;
    std::vector<std::pair<std::string,std::string>> choices;
};

struct PatchInfo {
    std::vector<ModOption> mod_options;
    std::string mod_name, mod_hash, mod_source, mod_description, mod_versions;
    std::string key, name, author, note, version;
    std::string title;  // the edition its file is for (CUSA13795, CUSA14876), "" for any game
    bool default_on = false;
    const char* ini_key = nullptr;
};

struct DlcInfo {
    fs::path path;
    std::string file, title, label, problem;
    bool ok = false;
};

enum class Job { None, Merge, Check, Play };

struct App {
    LauncherWindow hwnd = nullptr;
    int active_page = 0;
    fs::path root, data, bin;
    std::string version;
    std::optional<Python> python;

    Ini launcher;  // launcher.ini
    Ini game;      // bbport.ini
    char online_server_buf[128] = "octr.ryu7w7.xyz";
    char online_name_buf[32] = "";

    // Game folders.
    fs::path game_dir, base_dir, update_dir;
    FolderInfo game_info, base_info, update_info;
    std::atomic<bool> game_checked{false};
    bool game_ok = false;
    std::string check_message;

    // Background work (one at a time).
    std::atomic<Job> job{Job::None};
    std::thread worker;
    std::mutex mutex;  // guards the strings below and log
    std::string job_error;
    std::atomic<size_t> merge_done{0}, merge_total{0};
    std::atomic<bool> job_finished{false};

    // The running game.
#ifdef _WIN32
    HANDLE process = nullptr, job_object = nullptr;
#else
    int process = 0;
#endif
    std::deque<std::string> log;
    std::string last_status;
    std::atomic<bool> game_running{false};
    // The native renderer's shaders being built before the game starts (run.py's "@progress"
    // lines): percent, -1 when no build is on; its stage (extract, export, translate, compile).
    std::atomic<int> shader_percent{-1};
    std::string shader_stage;
    unsigned long exit_code = 0;
    bool show_exit = false, stopped = false;

    // Monitors and controllers (bb-gpu-capabilities).
    std::vector<std::pair<std::string, std::string>> displays;  // value (number), label
    std::vector<std::pair<std::string, std::string>> gamepads;  // GUID, name
    struct Gpu { std::string kind, vulkan, name; };              // bb-gpu-capabilities --gpus
    std::vector<Gpu> gpus;
    std::thread probe;
    std::atomic<bool> probe_done{false};

    bool want_close = false;
    bool confirm_clear_cache = false;
    bool ask_nick = false;  // PLAY pressed online without a usable nickname
    // A Discord invite for another server, waiting for the player to confirm the switch.
    bool ask_join_server = false;
    std::string join_server, join_nick;
    bool pending_play = false;  // an accepted invite starts the game once it is ready
    std::string notice;

    // Patches tab.
    std::vector<PatchInfo> patches;
    std::vector<std::string> patch_enabled, patch_disabled;
    // Saves tab.
    std::vector<SaveSlot> saves;
    std::vector<Backup> backups;
    bool saves_dirty = true;
    fs::path pending_path;
    int pending_action = 0, confirm_action = 0;  // 1 restore, 2 delete a backup, 3 remove a DLC
    // DLC tab.
    std::vector<DlcInfo> dlc;
    bool dlc_dirty = true;
};

App g_app;

/// Bringing the settings from another installation (settings_import.h, ImportPopup).
enum class ImportState { Idle, Copying, Done, Failed };
struct {
    std::vector<fs::path> from;  // the installations found, the last played first
    int choice = 0;
    bool ask = false;            // open the question on the next frame
    std::atomic<ImportState> state{ImportState::Idle};
    std::string error;           // written before state turns Failed
} g_import;

bool RestartLauncher(const std::wstring& args);

/// The title ID of the chosen game (the merged folder, else the update or the base game): the
/// saves, the shader cache and the merged folder's name follow it. CUSA13795 when none is known.
std::wstring GameTitle() {
    for (const FolderInfo* info : {&g_app.game_info, &g_app.update_info, &g_app.base_info})
        if (IsCtrTitle(info->title)) return Widen(info->title);
    return L"CUSA13795";
}

std::string EffectiveRenderRes();

fs::path LauncherIni() { return g_app.data / L"launcher.ini"; }
fs::path GameIni() { const auto override = Env("BB_CONFIG"); return override.empty() ? g_app.data / L"bbport.ini" : TextPath(override); }

void SaveLauncher() {
    App& a = g_app;
    a.launcher.Set("game_dir", PathText(a.game_dir));
    a.launcher.Set("base_dir", PathText(a.base_dir));
    a.launcher.Set("update_dir", PathText(a.update_dir));
    a.launcher.Set("ui_language", g_language);
    if (!a.launcher.Save(LauncherIni(), "# CTR-NitroFueled-PC launcher")) {
        a.notice = T("No se pudo guardar launcher.ini.", "Could not save launcher.ini.");
    }
}

void SaveGameIni() {
    if (!g_app.game.Save(GameIni(), "# bbport settings (in-game menu: Insert / L3+R3)")) {
        g_app.notice = T("No se pudo guardar bbport.ini.", "Could not save bbport.ini.");
    }
}

void SyncOnlineBuffers() {
    App& a = g_app;
    std::string srv = a.game.Get("online_server", "octr.ryu7w7.xyz");
    if (srv.empty()) {
        srv = "octr.ryu7w7.xyz";
        a.game.Set("online_server", srv);
    }
    std::string nick = a.game.Get("online_name", a.game.Get("user_name"));
    std::strncpy(a.online_server_buf, srv.c_str(), sizeof(a.online_server_buf) - 1);
    a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
    std::strncpy(a.online_name_buf, nick.c_str(), sizeof(a.online_name_buf) - 1);
    a.online_name_buf[sizeof(a.online_name_buf) - 1] = '\0';
}

void RefreshGameSettings(bool force = false) {
    static uint64_t checked = 0;
    static fs::file_time_type modified{};
    static fs::path previous_path;
    static bool sync_buffers = false;
    if (!force && Ticks() - checked < 500) return;
    checked = Ticks();
    std::error_code error;
    const auto path = GameIni();
    const auto time = fs::last_write_time(path, error);
    if (!error && (force || path != previous_path || time != modified) && g_app.game.Refresh(path)) {
        modified = time;
        previous_path = path;
        sync_buffers = true;
    }
    if (sync_buffers && (!ImGui::GetCurrentContext() || !ImGui::IsAnyItemActive())) {
        SyncOnlineBuffers();
        sync_buffers = false;
    }
}

std::string CurrentNick() { return Trim(g_app.game.Get("online_name", g_app.game.Get("user_name"))); }

/// The server derives the player's ID from the nickname, so everyone left on the old default
/// (RyuPC) was ONE player online: one wallet, and matchmaking evicting each other. These names
/// are refused; run.py refuses them too.
bool ReservedNick(const std::string& nick) {
    static const char* const kReserved[] = {"ryupc", "ryunet", "player", "jugador", "guest"};
    std::string lower = nick;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* r : kReserved)
        if (lower == r) return true;
    return false;
}

bool NickChar(unsigned c) { return c < 128 && (std::isalnum(int(c)) || c == '_' || c == '-'); }

/// Why this nickname cannot be used online, or nullptr when it can.
const char* NickProblem(const std::string& nick) {
    if (nick.empty()) return T("Falta tu apodo.", "Your nickname is missing.");
    if (nick.size() < 3 || nick.size() > 16)
        return T("El apodo debe tener de 3 a 16 caracteres.", "The nickname must be 3 to 16 characters.");
    for (const char c : nick)
        if (!NickChar(static_cast<unsigned char>(c)))
            return T("Solo letras, números, _ y -.", "Only letters, digits, _ and -.");
    if (ReservedNick(nick))
        return T("Ese es un apodo por defecto que usa mucha gente: elige uno propio.",
                 "That is a default nickname many people share: choose your own.");
    return nullptr;
}

int NickFilter(ImGuiInputTextCallbackData* data) { return NickChar(data->EventChar) ? 0 : 1; }

/// Nickname field (letters, digits, _ and - only, 16 at most); true when it was edited.
bool NickInput(const char* label) {
    App& a = g_app;
    constexpr size_t kMaxNick = 16;
    static_assert(sizeof(a.online_name_buf) > kMaxNick);
    if (!ImGui::InputText(label, a.online_name_buf, kMaxNick + 1, ImGuiInputTextFlags_CallbackCharFilter, NickFilter))
        return false;
    a.game.Set("online_name", a.online_name_buf);
    a.game.Set("user_name", a.online_name_buf);
    return true;
}

/// This installation's secret, shared with run.py (ryunet_online.key next to it): the server
/// lets the first key to use a nickname keep it. Created here when missing, so the nickname
/// check below already speaks for this installation.
/// A second copy of the key outside the game folder (%APPDATA%\CTR-NitroFueled-PC), so that
/// deleting, moving or re-downloading the folder on the same PC keeps the nicknames.
fs::path BackupKeyPath();

std::string ReadKeyFile(const fs::path& path) {
    std::string key;
    if (path.empty()) return key;
    std::ifstream(path, std::ios::binary) >> key;
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos) key.clear();
    return key;
}

bool WriteKeyFile(const fs::path& path, const std::string& key) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << key << "\n";
    return bool(out);
}

/// Every key used on this PC (run.py known_keys), one per line next to the backup: a nickname
/// the current key does not own may belong to another folder's (two installations, or the
/// backup from an older one), and the nickname check switches to it (AskNickStatus).
fs::path KeyringPath() {
    const fs::path backup = BackupKeyPath();
    return backup.empty() ? fs::path() : backup.parent_path() / L"ryunet_online_keys.txt";
}

std::vector<std::string> KnownKeys() {
    std::vector<std::string> keys;
    const fs::path path = KeyringPath();
    if (path.empty()) return keys;
    std::ifstream in(path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        line = Lower(Trim(line));
        if (line.size() == 64 && line.find_first_not_of("0123456789abcdef") == std::string::npos &&
            std::find(keys.begin(), keys.end(), line) == keys.end())
            keys.push_back(line);
    }
    return keys;
}

void RememberKey(const std::string& key) {
    const fs::path path = KeyringPath();
    if (path.empty() || key.size() != 64) return;
    const std::vector<std::string> keys = KnownKeys();
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::app) << key << "\n";
}

std::mutex g_online_key_mutex;  // the server checks ask from background threads
std::string g_online_key;

std::string OnlineKey() {
    std::scoped_lock lock{g_online_key_mutex};
    if (!g_online_key.empty()) return g_online_key;
    const fs::path path = g_app.data / L"ryunet_online.key";
    const fs::path backup = BackupKeyPath();
    std::string key = ReadKeyFile(path);
    if (key.empty()) key = ReadKeyFile(backup);  // a new folder on a PC that played before
    if (key.empty()) {
        unsigned char raw[32];
        if (!RandomBytes(raw, sizeof raw)) return {};
        static const char kHex[] = "0123456789abcdef";
        for (const unsigned char b : raw) {
            key += kHex[b >> 4];
            key += kHex[b & 15];
        }
    }
    if (ReadKeyFile(path) != key && !WriteKeyFile(path, key)) return {};
    if (ReadKeyFile(backup).empty()) WriteKeyFile(backup, key);  // never overwrites another folder's
    RememberKey(ReadKeyFile(backup));
    RememberKey(key);
    g_online_key = key;
    return g_online_key;
}

/// This folder plays with another of this PC's keys from now on (the game reads the file too).
bool UseOnlineKey(const std::string& key) {
    std::scoped_lock lock{g_online_key_mutex};
    if (!WriteKeyFile(g_app.data / L"ryunet_online.key", key)) return false;
    g_online_key = key;
    return true;
}

/// Server availability of a nickname: "free", "yours", "taken", "reserved", "invalid", or
/// "" while unknown or when the server could not be asked (it checks again at login).
struct NickCheck {
    struct Result {
        std::string status;
        uint64_t at = 0;  // GetTickCount64: an unreachable server is asked again later
        bool linked = false;          // the nickname is linked to a Discord account
        bool link_available = false;  // the server can link Discord accounts
        std::string discord;          // the linked account's name (only for our own nickname)
        std::string banned;           // the ban's reason when this player or address is banned
        std::string ban_kind;         // "player" or "ip"
        long long ban_until = 0;      // unix time; 0: permanent
        bool key_switched = false;    // it was another installation's on this PC: its key is used now
        bool account_required = false;  // the server plays only accounts (RYUNET_REQUIRE_ACCOUNT)
    };
    std::mutex mutex;
    std::map<std::string, Result> results;  // "server|nick" -> answer
    std::string in_flight;
};
NickCheck g_nick_check;

/// The server setting's host and HTTP port, split exactly as run.py does ("host" or "host:port").
void SplitServer(const std::string& server, std::string& host, uint16_t& port) {
    host = server;
    port = 8080;  // run.py's online_http_port default
    const auto colon = server.find(':');
    if (colon == std::string::npos || server.front() == '[') return;
    const std::string digits = server.substr(colon + 1);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return;
    host = server.substr(0, colon);
    port = uint16_t(std::atoi(digits.c_str()));
}

#include "eos.inc"

std::string Sha256Hex(const std::string& msg, const std::string& key = {});

std::string ServerKey(const std::string& host, const std::string& key) {
    if (key.size() != 64) return {};
    std::string raw(32, '\0');
    for (int i = 0; i < 32; ++i) raw[size_t(i)] = static_cast<char>(std::stoi(key.substr(i * 2, 2), nullptr, 16));
    return Sha256Hex(Lower(Trim(host)), raw);
}

/// What the server stores of the server key (its keyHash): the key requests are signed with.
/// key: one of this PC's keys (KnownKeys); the installation's own when empty.
std::string ServerKeyHash(const std::string& host, const std::string& key = {}) {
    const std::string server_key = ServerKey(host, key.empty() ? OnlineKey() : key);
    return server_key.empty() ? std::string() : Sha256Hex(server_key);
}

/// Seconds to add to this PC's clock to get the RyuNET server's (run.py SERVER_CLOCK_OFFSET), from
/// the Date of its answers. Signatures carry the time and the server takes them only within 5
/// minutes of its own clock: with the PC's clock off, every signed request was refused and the
/// player's own nickname read as taken.
std::atomic<long long> g_server_clock_offset{0};

/// Something on the way (an ISP's cache, an antivirus web shield) kept the server's answers and gave
/// them back hours later: their Date made a right clock look hours off ("This PC's clock is 650 min
/// off" with Windows' automatic time on), and every signed request then went with the wrong time.
/// Each request sends a fresh token (X-RyuNET-Clock) and the server answers with its time and that
/// token (X-RyuNET-Time, its clock_stamp.go): a kept answer has an old one. True once the server
/// sent X-RyuNET-Time: an answer without it is not its own.
std::atomic<bool> g_server_stamps{false};

/// A fresh X-RyuNET-Clock token: 16 hex digits.
std::string RunService(const std::string& request, unsigned timeout_ms = 30000, const std::atomic<bool>* cancel = nullptr);
std::string JsonEscape(const std::string& s);
std::string ClockToken() {
    unsigned char raw[8];
    if (!RandomBytes(raw, sizeof raw)) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string token;
    for (const unsigned char b : raw) {
        token += kHex[b >> 4];
        token += kHex[b & 15];
    }
    return token;
}

/// The request headers for the server's time: the token, and no answer kept on the way.
std::wstring ClockHeaders(const std::string& token) {
    std::wstring headers = L"Cache-Control: no-cache\r\n";
    if (!token.empty()) headers += L"X-RyuNET-Clock: " + Widen(token) + L"\r\n";
    return headers;
}

/// An answer of the RyuNET server sets g_server_clock_offset: its X-RyuNET-Time when it carries
/// the token sent with the request, else (a server older than that) its Date, unless a cache says
/// it kept the answer (Age).
#ifdef _WIN32
void NoteServerClock(HINTERNET req, const std::string& token);
#endif

/// X-RyuNET-Proof for one request (the server's nick_proof.go, run.py proof_header): the key
/// never crosses the network, and the signature is good once, for a few minutes of the
/// server's time.
std::string ProofHeader(const std::string& key_hash, const char* method, const std::string& target) {
    unsigned char raw[16];
    if (key_hash.empty() || !RandomBytes(raw, sizeof raw)) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string nonce;
    for (const unsigned char b : raw) {
        nonce += kHex[b >> 4];
        nonce += kHex[b & 15];
    }
    const std::string ts = std::to_string(static_cast<long long>(std::time(nullptr)) + g_server_clock_offset.load());
    const std::string mac = Sha256Hex(ts + "\n" + nonce + "\n" + method + "\n" + target, key_hash);
    return mac.empty() ? std::string() : "1." + ts + "." + nonce + "." + mac;
}

/// A request to the RyuNET server's HTTP port, signed with this installation's key for that
/// server; register also sends the key's hash (a Discord link takes it for a new installation).
/// The body, or "" when the server could not be reached. A request signed with a clock found off
/// by minutes goes again once: the server refuses it before doing anything. Called from
/// background threads. with_key signs with another of this PC's keys (KnownKeys).
std::string HttpRequest(const std::string& server, const wchar_t* method, const std::wstring& path,
                        const std::string& form = {}, int timeout_ms = 3000, bool register_key = false,
                        const std::string& with_key = {}, size_t max_body = 4096);

std::string HttpGet(const std::string& server, const std::wstring& path) { return HttpRequest(server, L"GET", path); }

/// A string field of a small flat JSON answer ("" when missing).
std::string JsonField(const std::string& body, const char* key) {
    const std::string field = std::string("\"") + key + "\":\"";
    const auto at = body.find(field);
    if (at == std::string::npos) return {};
    const auto end = body.find('"', at + field.size());
    return end == std::string::npos ? std::string() : body.substr(at + field.size(), end - at - field.size());
}

/// A number field of a small flat JSON answer.
bool JsonNumber(const std::string& body, const char* key, int& value) {
    const std::string field = std::string("\"") + key + "\":";
    const auto at = body.find(field);
    if (at == std::string::npos) return false;
    char* end = nullptr;
    const long n = std::strtol(body.c_str() + at + field.size(), &end, 10);
    if (end == body.c_str() + at + field.size()) return false;
    value = int(n);
    return true;
}

// Defined with the Discord join secrets below.
std::string JsonText(const std::string& body, const char* key);

/// A true/false field of a small flat JSON answer.
bool JsonBool(const std::string& body, const char* key) {
    return body.find(std::string("\"") + key + "\":true") != std::string::npos;
}

NickCheck::Result AskNickStatus(const std::string& server, const std::string& nick) {
    // {"status":"...","linked":..,"discord_link":..,"discord":".."}; a server without this
    // endpoint answers something else.
    const std::wstring path = L"/ryunet/nick?name=" + Widen(nick);  // [A-Za-z0-9_-]
    std::string body = HttpGet(server, path);
    bool key_switched = false;
    if (JsonField(body, "status") == "taken") {
        // Another installation on this PC may own it (KnownKeys): this folder takes its key.
        const std::string own = OnlineKey();
        for (const std::string& key : KnownKeys()) {
            if (key == own) continue;
            const std::string other = HttpRequest(server, L"GET", path, {}, 3000, false, key);
            if (JsonField(other, "status") == "yours" && UseOnlineKey(key)) {
                body = other;
                key_switched = true;
                break;
            }
        }
    }
    NickCheck::Result r;
    r.key_switched = key_switched;
    r.status = JsonField(body, "status");
    r.linked = JsonBool(body, "linked");
    r.link_available = JsonBool(body, "discord_link");
    r.account_required = JsonBool(body, "account_required");
    r.discord = JsonField(body, "discord");
    r.banned = JsonText(body, "banned");
    r.ban_kind = JsonField(body, "ban_kind");
    if (const auto at = body.find("\"ban_until\":"); at != std::string::npos)
        r.ban_until = std::strtoll(body.c_str() + at + 12, nullptr, 10);
    return r;
}

/// Cached answer for the current nickname, asking the server in the background once per
/// nickname. "?" while the question is out.
std::string NickAvailability() {
    const std::string nick = CurrentNick();
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    if (NickProblem(nick) || server.empty()) return "";
    const std::string id = server + "|" + nick;
    std::scoped_lock lock{g_nick_check.mutex};
    if (const auto it = g_nick_check.results.find(id); it != g_nick_check.results.end()) {
        if (!it->second.status.empty() || Ticks() - it->second.at < 30000) return it->second.status;
    }
    if (!g_nick_check.in_flight.empty()) return "?";
    g_nick_check.in_flight = id;
    std::thread([server, nick, id] {
        NickCheck::Result result = AskNickStatus(server, nick);
        result.at = Ticks();
        std::scoped_lock done{g_nick_check.mutex};
        if (result.key_switched) g_nick_check.results.clear();  // the other answers were for the old key
        g_nick_check.results[id] = result;
        g_nick_check.in_flight.clear();
    }).detach();
    return "?";
}

/// Whether the server can link Discord accounts, asked once in the background without a
/// nickname (a reinstalled launcher has none yet).
bool DiscordLinkAvailable() {
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    if (server.empty()) return false;
    const std::string id = server + "|*";
    std::scoped_lock lock{g_nick_check.mutex};
    if (const auto it = g_nick_check.results.find(id); it != g_nick_check.results.end()) {
        if (it->second.link_available || Ticks() - it->second.at < 30000) return it->second.link_available;
    }
    if (!g_nick_check.in_flight.empty()) return false;
    g_nick_check.in_flight = id;
    std::thread([server, id] {
        NickCheck::Result result = AskNickStatus(server, "");
        result.at = Ticks();
        std::scoped_lock done{g_nick_check.mutex};
        g_nick_check.results[id] = result;
        g_nick_check.in_flight.clear();
    }).detach();
    return false;
}

/// The last answer about the current nickname (status "" when unknown), without asking.
NickCheck::Result NickInfo() {
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    std::scoped_lock lock{g_nick_check.mutex};
    const auto it = g_nick_check.results.find(server + "|" + CurrentNick());
    return it == g_nick_check.results.end() ? NickCheck::Result{} : it->second;
}

constexpr const wchar_t* kCommunityDiscord = L"https://discord.gg/PAT5SJzdXw";

/// The community's Discord (bugs, questions, ban appeals): a line and a button.
void CommunityDiscordLine(const char* why) {
    ImGui::TextColored(kGrey, "%s", why);
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Abrir Discord", "Open Discord"))) OpenInShell(kCommunityDiscord);
}

/// This player's ban, as the server's nickname check tells it (nothing when not banned).
bool BanNotice() {
    const NickCheck::Result info = NickInfo();
    if (info.banned.empty()) return false;
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextWrapped("%s", info.ban_kind == "ip" ? T("Tu conexión (IP) está baneada del online.", "Your connection (IP) is banned from online play.") : T("Tu cuenta está baneada del online.", "Your account is banned from online play."));
    ImGui::TextWrapped(T("Motivo: %s", "Reason: %s"), info.banned.c_str());
    if (info.ban_until > 0) {
        char when[32];
        const std::time_t t = std::time_t(info.ban_until);
        std::tm local{};
        LocalTime(&local, &t);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &local);
        ImGui::TextWrapped(T("Hasta: %s", "Until: %s"), when);
    } else {
        ImGui::TextWrapped("%s", T("Duración: permanente", "Duration: permanent"));
    }
    ImGui::PopStyleColor();
    CommunityDiscordLine(T("Si crees que es un error, apela en nuestro Discord.", "If you think it is a mistake, appeal on our Discord."));
    return true;
}

/// One line under a nickname field: what the server says about it.
void NickAvailabilityLine() {
    const std::string s = NickAvailability();
    if (BanNotice()) return;
    if (s == "?") {
        ImGui::TextColored(kGrey, "%s", T("Comprobando el apodo en el servidor...", "Checking the nickname with the server..."));
    } else if (s == "free") {
        ImGui::TextColored(kGreen, "%s", T("Disponible: será tuyo la primera vez que juegues online.",
                                           "Available: it becomes yours the first time you play online."));
    } else if (s == "yours" && NickInfo().key_switched) {
        ImGui::TextColored(kGreen, "%s", T("Este apodo es tuyo (recuperado de otra instalación de este PC).",
                                           "This nickname is yours (recovered from another installation on this PC)."));
    } else if (s == "yours") {
        ImGui::TextColored(kGreen, "%s", T("Este apodo es tuyo.", "This nickname is yours."));
    } else if (s == "taken" && NickInfo().linked) {
        ImGui::TextColored(kRed, "%s", T("Ese apodo está vinculado a una cuenta de Discord. Si es tuyo, recupéralo en la "
                                         "pestaña Online (Recuperar con Discord); si no, elige otro.",
                                         "That nickname is linked to a Discord account. If it is yours, recover it in the "
                                         "Online tab (Recover with Discord); otherwise choose another one."));
    } else if (s == "taken") {
        ImGui::TextColored(kRed, "%s", T("Ese apodo ya lo usa otra persona: elige otro.",
                                         "Someone else already uses that nickname: choose another one."));
    } else if (s == "reserved" || s == "invalid") {
        ImGui::TextColored(kRed, "%s", T("El servidor no acepta ese apodo: elige otro.", "The server does not accept that nickname: choose another one."));
    }
    const long long clock_off = std::llabs(g_server_clock_offset.load());
    if (clock_off > 240) {
        ImGui::PushStyleColor(ImGuiCol_Text, kYellow);
#ifdef _WIN32
        ImGui::TextWrapped(T("La hora de este PC va %lld min desfasada: el online la corrige, pero activa la fecha y hora "
                             "automáticas en la configuración de Windows.",
                             "This PC's clock is %lld min off: online corrects it, but set the date and time to "
                             "automatic in the Windows settings."),
                           (clock_off + 30) / 60);
#else
        ImGui::TextWrapped(T("La hora de este PC va %lld min desfasada. Activa la sincronización automática de hora.",
                             "This PC's clock is %lld min off. Enable automatic time synchronization."), (clock_off + 30) / 60);
#endif
        ImGui::PopStyleColor();
    }
}

bool NickTaken() {
    const std::string s = NickAvailability();
    return s == "taken" || s == "reserved" || s == "invalid";
}

/// The lobby this player hosts (their party or private room), from the server's
/// /ryunet/party, refreshed in the background every 5 s while it is asked for: Discord's
/// "1 of 8" follows players joining and leaving. Only changes reach Discord (DiscordPresence
/// sends nothing for an unchanged activity), within its 5 updates per 20 s.
struct PartyPoll {
    std::mutex mutex;
    std::string id;  // "server|nick" of the last answer
    bool known = false;  // the server answered
    int players = 0, max = 0;
    uint64_t at = 0;
    bool in_flight = false;
};
PartyPoll g_party;

/// False while unknown (no answer yet, or a server without /ryunet/party).
bool PartyInfo(const std::string& server, const std::string& nick, int& players, int& max) {
    const std::string id = server + "|" + nick;
    std::scoped_lock lock{g_party.mutex};
    if ((g_party.id != id || Ticks() - g_party.at >= 5000) && !g_party.in_flight) {
        g_party.in_flight = true;
        std::thread([server, nick, id] {
            const std::string body = HttpGet(server, L"/ryunet/party?name=" + Widen(nick));
            int p = 0, m = 0;
            const bool known = JsonNumber(body, "players", p) && JsonNumber(body, "max", m);
            std::scoped_lock done{g_party.mutex};
            g_party.id = id;
            g_party.known = known;
            g_party.players = p;
            g_party.max = m;
            g_party.at = Ticks();
            g_party.in_flight = false;
        }).detach();
    }
    if (g_party.id != id || !g_party.known) return false;
    players = g_party.players;
    max = g_party.max;
    return true;
}

/// The NAT type test (nat_check.h), on a worker thread: once per server while the launcher is
/// open (the Play page's status line starts it), again with the Online tab's button.
struct NatState {
    std::mutex mutex;
    bool running = false, done = false;
    std::string server;  // what the last test asked
    NatCheck::Result result;
};
NatState g_nat;

void StartNatCheck(const std::string& server) {
    {
        std::scoped_lock lock{g_nat.mutex};
        if (g_nat.running) return;
        g_nat.running = true;
        g_nat.server = server;
    }
    std::thread([server] {
        const NatCheck::Result r = NatCheck::Check(server);
        std::scoped_lock lock{g_nat.mutex};
        g_nat.result = r;
        g_nat.running = false;
        g_nat.done = true;
    }).detach();
}

/// Short label and colour for the Play page ("NAT: type 2 (moderate)"); starts the first test
/// for this server.
std::string NatLabel(const std::string& server, ImVec4& color) {
    bool running, done;
    std::string checked;
    NatCheck::Result r;
    {
        std::scoped_lock lock{g_nat.mutex};
        running = g_nat.running;
        done = g_nat.done;
        checked = g_nat.server;
        r = g_nat.result;
    }
    if (!running && (!done || checked != server)) {
        StartNatCheck(server);
        running = true;
    }
    color = kGrey;
    if (running) return std::string("NAT: ") + T("comprobando...", "checking...");
    if (r.replies_blocked || r.error == "server") {
        color = kRed;
        return r.replies_blocked ? T("NAT: online bloqueado (¿firewall?)", "NAT: online blocked (firewall?)")
                                 : T("NAT: el servidor no responde", "NAT: no answer from the server");
    }
    switch (r.type) {
    case NatCheck::Type::Open: color = kGreen; return T("NAT: tipo 1 (abierto)", "NAT: type 1 (open)");
    case NatCheck::Type::Moderate: color = kGreen; return T("NAT: tipo 2 (moderado)", "NAT: type 2 (moderate)");
    case NatCheck::Type::Strict: color = kYellow; return T("NAT: tipo 3 (estricto)", "NAT: type 3 (strict)");
    case NatCheck::Type::Unknown: break;
    }
    return T("NAT: desconocido", "NAT: unknown");
}

/// A server setting that can travel inside a Discord join secret (and be trusted coming out
/// of one): a host name or IP with an optional port.
bool ValidServer(const std::string& s) {
    return !s.empty() && s.size() <= 64 &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") == std::string::npos;
}

bool ListHas(const std::string& csv, const std::string& name) {
    std::stringstream in(csv);
    for (std::string item; std::getline(in, item, ',');)
        if (Lower(Trim(item)) == Lower(name)) return true;
    return false;
}

/// run.py's "@progress native-shaders ..." lines: the launcher comes back to show the shader build
/// (ShaderProgressPopup) and goes away again when the game starts. Not shown in the log.
bool ShaderProgressLine(const std::string& line) {
    static const std::string prefix = "@progress native-shaders ";
    if (line.compare(0, prefix.size(), prefix) != 0) return line.rfind("@progress ", 0) == 0;
    App& a = g_app;
    const std::string rest = line.substr(prefix.size());
    if (rest.rfind("done", 0) == 0) {
        a.shader_percent = -1;
        if (a.hwnd) HideLauncher(a.launcher.Flag("hide_on_play", false));
        return true;
    }
    const size_t space = rest.find(' ');
    const int percent = std::clamp(std::atoi(rest.substr(0, space).c_str()), 0, 100);
    {
        std::scoped_lock lock{a.mutex};
        a.shader_stage = space == std::string::npos ? "" : Trim(rest.substr(space + 1));
    }
    if (a.shader_percent.exchange(percent) < 0 && a.hwnd) {
        BringLauncherToFront();
    }
    return true;
}

void AddLog(const std::string& line) {
    if (ShaderProgressLine(line)) return;
    std::scoped_lock lock{g_app.mutex};
    g_app.log.push_back(line);
    while (g_app.log.size() > 2000) g_app.log.pop_front();
    if (!line.empty()) g_app.last_status = line;
}

void JoinWorker() {
    if (g_app.worker.joinable()) g_app.worker.join();
}

void StartCheck() {
    App& a = g_app;
    a.game_info = Analyze(a.game_dir);
    a.game_checked = false;
    a.game_ok = false;
    a.check_message.clear();
    if (a.game_info.kind != Kind::Complete || !a.python) return;
    if (a.game_info.split) {  // Image0 + Sc0: PlayTab offers to prepare it
        a.game_checked = true;
        return;
    }
    JoinWorker();
    a.job = Job::Check;
    a.job_finished = false;
    const fs::path dir = a.game_dir;
    a.worker = std::thread([dir] {
        App& app = g_app;
        const std::wstring cmd = Quote(app.python->exe) + L" " + app.python->args + L"-X utf8 " +
                                 Quote(WidePath((app.root / L"scripts" / L"game_check.py"))) + L" " + Quote(WidePath(dir));
        const Captured r = RunCaptured(app.python->exe, cmd, app.root, 120000);
        {
            std::scoped_lock lock{app.mutex};
            app.game_ok = r.started && r.code == 0;
            app.check_message = r.started ? Trim(r.output) : "Python could not be started.";
        }
        app.game_checked = true;
        app.job_finished = true;
    });
}

fs::path MergeTarget(const fs::path& near_dir) {
    App& a = g_app;
    const fs::path parent = near_dir.parent_path();
    fs::path target = parent / (GameTitle() + L"-1.21");
    if (target == near_dir || target == a.base_dir || target == a.update_dir || target == a.game_dir) {
        target = parent / (GameTitle() + L"-1.21-merged");
    }
    return target;
}

/// Base game + 1.21 update (or one already merged dump in the Image0 + Sc0 layout) -> a new
/// folder of hard links in the PS4 layout (copies across drives).
void StartMerge(std::vector<fs::path> sources) {
    App& a = g_app;
    if (sources.empty()) return;
    const fs::path target = MergeTarget(sources.front());
    JoinWorker();
    a.job = Job::Merge;
    a.job_finished = false;
    a.merge_done = 0;
    a.merge_total = 0;
    {
        std::scoped_lock lock{a.mutex};
        a.job_error.clear();
    }
    a.worker = std::thread([sources, target] {
        App& app = g_app;
        std::vector<std::pair<fs::path, fs::path>> files;  // source, relative
        std::error_code ec;
        for (const fs::path& from : sources) {
            auto more = LayoutFiles(from, ec);
            if (ec) break;
            files.insert(files.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        }
        std::string error;
        if (ec) error = "Cannot read the game folders: " + ec.message();
        app.merge_total = files.size();
        size_t copied = 0;
        for (size_t i = 0; error.empty() && i < files.size(); ++i) {
            const fs::path dest = target / files[i].second;
            std::error_code e2;
            fs::create_directories(dest.parent_path(), e2);
            if (fs::exists(dest, e2)) fs::remove(dest, e2);
            fs::create_hard_link(files[i].first, dest, e2);
            if (e2) {
                if (!CopyFile(files[i].first, dest)) {
                    error = "Cannot create " + PathText(dest) + " (error " + LastPlatformError() + ")";
                    break;
                }
                ++copied;
            }
            app.merge_done = i + 1;
        }
        {
            std::scoped_lock lock{app.mutex};
            app.job_error = error;
            if (error.empty()) {
                app.game_dir = target;
                app.log.push_back("Merged " + std::to_string(files.size()) + " files into " + PathText(target) +
                                  (copied ? " (" + std::to_string(copied) + " copied: different drive)" : ""));
            }
        }
        app.job_finished = true;
    });
}

/// The European edition is chosen (its languages differ from the Americas build's).
bool IsEuGame() { return GameTitle() == L"CUSA14876"; }

/// The game language of the launcher's own language, where the chosen edition has it.
const char* DefaultGameLanguage() {
    const bool eu = IsEuGame();
    if (g_active_locale == "es") return eu ? "3" : "20";
    if (g_active_locale == "fr") return "2";
    if (eu && g_active_locale == "de") return "4";
    if (eu && g_active_locale == "it") return "5";
    if (!eu && g_active_locale == "pt_BR") return "17";
    return "1";
}

/// The game language to start with: the saved one when the chosen edition has it, else its
/// counterpart there (Spanish: Latin American in the Americas build, Spain's in the European one),
/// else the default.
std::string GameLanguage() {
    const std::string saved = g_app.launcher.Get("language");
    const bool eu = IsEuGame();
    auto has = [&](const std::string& code) {
        if (eu) {
            for (const auto& c : kGameLanguagesEU)
                if (code == c.value) return true;
        } else {
            for (const auto& c : kGameLanguages)
                if (code == c.value) return true;
        }
        return false;
    };
    if (has(saved)) return saved;
    if (saved == "3" && !eu) return "20";
    if (saved == "20" && eu) return "3";
    return DefaultGameLanguage();
}

/// Where the game writes what the player does (BB_PRESENCE_FILE) and run.py the tracks' names.
fs::path PresenceFolder() { return g_app.data / L"out" / L"presence"; }

void SetEnv(const wchar_t* name, const std::string& value);

/// The place in gpus (bb-gpu-capabilities --gpus, Vulkan's order: BB_GPU_ID) of the nth GPU
/// called name when several share it, the first one's if there are no longer that many; -1 when
/// the name is unique or not listed (BB_GPU_NAME is enough).
int GpuIndex(const std::vector<App::Gpu>& gpus, const std::string& name, int nth) {
    std::vector<int> same;
    for (size_t i = 0; i < gpus.size(); ++i)
        if (gpus[i].name == name) same.push_back(int(i));
    if (same.size() < 2) return -1;
    return nth >= 0 && nth < int(same.size()) ? same[nth] : same[0];
}

bool UpdateBusy();

void LaunchGameProcess();
void BeginGameEnv();
void SetPlatformGameEnv();
void ApplyPlatformCapabilities();
void StartGame() {
    App& a = g_app;
    if (!a.python || a.game_running || UpdateBusy()) return;
    RefreshGameSettings(true);
    BeginGameEnv();
    ApplyPlatformCapabilities();
    SaveGameIni();
    SaveLauncher();
    JoinWorker();
    {
        std::scoped_lock lock{a.mutex};
        a.log.clear();
        a.last_status.clear();
    }
    {
        // The EOS bridge, when this network reaches the server only through it.
        std::string map, ip, peers;
        if (a.game.Flag("online", true))
            EosBridgeGameEnv(Trim(a.game.Get("online_server", "octr.ryu7w7.xyz")), map, ip, peers, 8000);
        SetEnv(L"BB_ONLINE_BRIDGE", map);
        SetEnv(L"BB_ONLINE_SERVER_IP", ip);
        SetEnv(L"BB_ONLINE_EOS", peers);  // races through EOS P2P with peers that have no direct path
        if (!map.empty()) AddLog("Online through Epic Online Services: this network does not reach the server directly");
    }
    SetEnv(L"BB_GAME_DIR", PathText(a.game_dir));
    SetEnv(L"BB_NEO", EffectiveRenderRes() != "1080" ? "1" : "0");  // the resolution picks the path
    SetEnv(L"BB_LANGUAGE", GameLanguage());
    SetEnv(L"BB_DISPLAY", a.launcher.Get("display"));
    {
        // The graphics card, by name (empty: automatic); one of several GPUs that share a name by
        // its place in Vulkan's list instead (BB_GPU_NAME would take the first of them). Otherwise
        // a BB_GPU_ID set outside the launcher stays: the one it started with, not an earlier game's.
        static const std::string outside_id = Env("BB_GPU_ID");
        const std::string gpu = a.launcher.Get("gpu");
        int index;
        {
            std::scoped_lock lock{a.mutex};
            index = GpuIndex(a.gpus, gpu, a.launcher.Int("gpu_nth", 0));
        }
        SetEnv(L"BB_GPU_NAME", index < 0 ? gpu : "");
        if (index >= 0) SetEnv(L"BB_GPU_ID", std::to_string(index));
        else SetEnv(L"BB_GPU_ID", outside_id);
    }
    SetEnv(L"BB_PRESENT_MODE", a.launcher.Get("present_mode", "Mailbox"));
    SetEnv(L"BB_GAMEPAD", a.launcher.Get("gamepad"));
    SetEnv(L"BB_MENU_LANGUAGE", g_spanish ? "es" : "en");  // the in-game menu follows the launcher
    SetEnv(L"BB_SAVE_LOG", a.launcher.Flag("save_log", false) ? "1" : "");
    // Discord presence: the game writes what the player does there (src/runtime_presence.c), and
    // run.py the tracks' own names beside it (scripts/track_names.py).
    {
        std::error_code ec;
        fs::create_directories(PresenceFolder(), ec);
        fs::remove(PresenceFolder() / L"presence.txt", ec);
        SetEnv(L"BB_PRESENCE_FILE", PathText(PresenceFolder() / L"presence.txt"));
    }
    if (a.launcher.Flag("hdr", false)) {  // off: BB_HDR as set outside
        SetEnv(L"BB_HDR", "1");
        const std::string peak = a.launcher.Get("hdr_peak", "0");
        if (peak != "0") SetEnv(L"BB_HDR_PEAK", peak);
        const std::string gain = a.launcher.Get("hdr_gain", "1");
        if (gain != "1") SetEnv(L"BB_HDR_GAIN", gain);
    }
    SetEnv(L"BB_DLC", a.launcher.Flag("dlc", true) ? "1" : "0");
    SetEnv(L"BB_DLC_DISABLED", a.launcher.Get("dlc_disabled"));
    SetEnv(L"BB_FULLSCREEN", "");  // bbport.ini's fullscreen= (the menu switch) decides
    SetEnv(L"PYTHONUTF8", "1");
    SetEnv(L"PYTHONIOENCODING", "utf-8");

    SetPlatformGameEnv();
    LaunchGameProcess();
}

void StopGame();
void ReleaseGameProcess();

/// Discord application whose name and art the presence shows (and whose OAuth2 links
/// nicknames). launcher.ini discord_app_id can name another one; empty or missing is this one.
constexpr const char* kDiscordAppId = "1560408801704943708";

std::string DiscordAppId() {
    const std::string id = g_app.launcher.Get("discord_app_id");
    return id.empty() ? kDiscordAppId : id;
}

/// What the game says the player does (src/runtime_presence.c, presence.txt), read every two seconds.
struct GameFacts {
    bool known = false;
    int game = 0, race = 0;  // EGameMode (Story 1, Arcade 8, OnlinePlay 16), EOctaneRaceModes
    std::string level, room;
    bool lobby = false, itemless = false, speed = false;
};

GameFacts ReadGameFacts() {
    static GameFacts facts;
    static uint64_t read_at = 0;
    const uint64_t now = Ticks();
    if (read_at && now - read_at < 2000) return facts;
    read_at = now;
    GameFacts f;
    std::istringstream in(ReadText(PresenceFolder() / L"presence.txt"));
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = Trim(line.substr(eq + 1));
        f.known = true;
        if (key == "game") f.game = std::atoi(value.c_str());
        else if (key == "race") f.race = std::atoi(value.c_str());
        else if (key == "level") f.level = value;
        else if (key == "room") f.room = value;
        else if (key == "lobby") f.lobby = value == "1";
        else if (key == "itemless") f.itemless = value == "1";
        else if (key == "speed") f.speed = value == "1";
    }
    facts = f;
    return facts;
}

/// A level's own name in the launcher's language (track_names.txt from scripts/track_names.py: the
/// game's zone infos), "" for menus and levels without one.
std::string ZoneName(const std::string& level) {
    static std::map<std::string, std::map<std::string, std::string>> names;
    static uint64_t loaded_at = 0;
    const uint64_t now = Ticks();
    if (!loaded_at || (names.empty() && now - loaded_at > 10000)) {  // run.py writes it at the start
        loaded_at = now;
        names.clear();
        std::istringstream in(ReadText(PresenceFolder() / L"track_names.txt"));
        for (std::string line; std::getline(in, line);) {
            const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos) continue;
            names[line.substr(0, a)][line.substr(a + 1, b - a - 1)] = Trim(line.substr(b + 1));
        }
    }
    std::string zone = Lower(level);
    const size_t slash = zone.find_last_of("/\\");
    if (slash != std::string::npos) zone = zone.substr(slash + 1);
    const std::string locale = g_active_locale == "es" || g_active_locale == "fr" || g_active_locale == "pt_BR"
                                   ? g_active_locale : "en";
    // the zone, else its leading parts (h100_hub_chunk_... is the Adventure hub, h100_hub)
    for (std::string key = zone; !key.empty();) {
        const auto it = names.find(key);
        if (it != names.end()) {
            const auto name = it->second.find(locale);
            if (name != it->second.end()) return name->second;
            const auto en = it->second.find("en");
            return en != it->second.end() ? en->second : std::string();
        }
        const size_t cut = key.find_last_of('_');
        if (cut == std::string::npos) break;
        key = key.substr(0, cut);
    }
    return "";
}

/// The race mode's name (EOctaneRaceModes), "" for none or the Adventure hub.
std::string RaceModeName(int race) {
    switch (race) {
    case 1: return T("Carrera", "Race");
    case 2: return T("Contrarreloj", "Time Trial");
    case 3: return T("Carrera por la reliquia", "Relic Race");
    case 4: return T("Carrera contra el jefe", "Boss Race");
    case 5: return T("Desafío CTR", "CTR Challenge");
    case 6: return T("Carrera de copa", "Cup Race");
    case 8: return T("Desafío de cristales", "Crystal Challenge");
    case 9: return T("Batalla", "Battle");
    case 10: return T("Batalla: captura la bandera", "Battle: Capture the Flag");
    case 11: return T("Batalla: último kart en pie", "Battle: Last Kart Driving");
    case 12: return T("Batalla: roba el tocino", "Battle: Steal the Bacon");
    case 13: return T("Batalla: atrapa cristales", "Battle: Crystal Grab");
    case 15: return T("Carrera de anillos", "Ring Race");
    default: return "";
    }
}

/// From what the game says: what the player does and where (the presence's first line), and the
/// kind of online match ("" offline); false when the game said nothing yet.
bool DescribePlay(std::string& doing, std::string& match) {
    const GameFacts f = ReadGameFacts();
    if (!f.known) return false;
    const std::string place = ZoneName(f.level);
    std::string mode = RaceModeName(f.race);
    if (f.itemless) mode += T(" sin objetos", " (itemless)");
    if (f.speed) mode += " (Speed)";
    const bool online = f.game == 16 || !f.room.empty();
    match = f.room == "private" ? T("Sala privada", "Private room")
            : online           ? T("Partida pública", "Public match")
                               : "";
    if (place.empty()) {  // menus, an online lobby, a level without a name
        if (f.lobby && f.room == "private") doing = T("En una sala privada online", "In a private online room");
        else if (f.lobby || f.room == "public") doing = T("Buscando partida online", "Looking for an online match");
        else doing = online ? T("En los menús online", "In the online menus") : T("En los menús", "In the menus");
        if (f.lobby || f.room == "public") match.clear();  // the first line says it
        return true;
    }
    if (f.game == 1) {  // Adventure: the hub, or a race of it
        doing = std::string(T("Modo aventura", "Adventure Mode")) +
                (f.race == 7 || mode.empty() ? ": " + place : " · " + mode + ": " + place);
        return true;
    }
    doing = mode.empty() ? place : mode + ": " + place;
    return true;
}

/// Shows the game in the player's Discord status while it runs (launcher.ini discord_rpc,
/// on by default). Called every frame; DiscordPresence only talks to Discord on a change.
void UpdateDiscord() {
    App& a = g_app;
    static int64_t started = 0;
    if (!a.game_running) {
        started = 0;
    } else if (started == 0) {
        started = int64_t(std::time(nullptr));
    }
    const bool enabled = a.launcher.Flag("discord_rpc", true);
    const std::string app_id = DiscordAppId();
    if (!a.game_running) {
        DiscordPresence::Update(enabled, app_id, nullptr);
        return;
    }
    DiscordPresence::Activity activity;
    // what the player does (the mode, the track) when the game told it (src/runtime_presence.c)
    // first line: what and where; second: the nickname (and the online match's kind)
    std::string doing, match;
    const bool described = DescribePlay(doing, match);
    activity.large_text = "Crash Team Racing Nitro-Fueled";
    if (a.game.Flag("online", true)) {
        activity.details = described ? doing : T("Jugando online", "Playing online");
        const std::string nick = CurrentNick();
        if (!nick.empty()) activity.state = std::string(T("Apodo: ", "Nickname: ")) + nick;
        if (described && !match.empty())
            activity.state += activity.state.empty() ? match : std::string(" · ") + match;
        // Joinable: Discord's "Join" and the chat's "+ > Invite" hand our server and nickname to
        // whoever accepts; their launcher adds us as a friend and starts their game.
        const std::string server = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
        if (!NickProblem(nick) && ValidServer(server)) {
            int players = 0, max = 0;
            if (!PartyInfo(server, nick, players, max)) {
                players = 1;  // a server without /ryunet/party: offer it anyway
                max = 8;
            }
            if (players > 0 && max >= players) {
                activity.party_id = "ctr:" + Lower(server) + ":" + Lower(nick);
                activity.party_size = players;
                activity.party_max = max;
                if (players < max) activity.join_secret = "ctr1|" + server + "|" + nick;
            }
        }
    } else {
        activity.details = described ? doing : T("Jugando sin conexión", "Playing offline");
        if (described) activity.state = T("Sin conexión", "Offline");
    }
    activity.start = started;
    activity.large_image = "logo";
    DiscordPresence::Update(enabled, app_id, &activity);
}

/// Lets Discord start this launcher when the player accepts an invite with it closed: the
/// discord-<app id> URL protocol, registered for the current user as Discord's own SDKs do.
/// Written on every start, so a portable folder that moved points at its new place.
void RegisterDiscordLaunch(const std::string& app_id);

/// What the PLAY button does: online without a usable nickname asks for one first (NickPopup).
void PressPlay() {
    RefreshGameSettings(true);
    App& a = g_app;
    if (a.game.Flag("online", true) && (NickProblem(CurrentNick()) || NickTaken())) a.ask_nick = true;
    else StartGame();
}

bool GameReady() {
    const App& a = g_app;
    return a.game_info.kind == Kind::Complete && a.game_checked && a.game_ok && a.job == Job::None && a.python;
}

void BringLauncherToFront();
void RequestClose();
bool TrayCanStart() { return GameReady() && !UpdateBusy() && !g_app.game_running; }
const char* TrayLauncherLabel() {
    return LauncherVisible() ? T("Minimizar launcher", "Minimize launcher") : T("Abrir launcher", "Open launcher");
}
void TrayAction(int command) {
    if (command == 1) ToggleLauncher();
    else if (command == 2) {
        if (g_app.game_running) StopGame();
        else if (TrayCanStart()) { BringLauncherToFront(); PressPlay(); }
    } else if (command == 3) { BringLauncherToFront(); RequestClose(); }
}

std::string Format(const char* format, const std::string& a, const std::string& b = {}) {
    char buf[512];
    std::snprintf(buf, sizeof buf, format, a.c_str(), b.c_str());
    return buf;
}

/// An invite accepted in Discord, for a server we use (or agreed to switch to): the host
/// becomes a friend (the game lists friends' lobbies as joinable) and the game starts.
void AcceptDiscordJoin(const std::string& server, const std::string& nick) {
    App& a = g_app;
    a.game.Set("online", "1");
    a.game.Set("online_server", server);
    std::string friends = Trim(a.game.Get("online_friends"));
    if (!ListHas(friends, nick)) friends = friends.empty() ? nick : friends + "," + nick;
    a.game.Set("online_friends", friends);
    SaveGameIni();  // a running game reads the new friend from bbport.ini
    SyncOnlineBuffers();
    {
        std::scoped_lock lock{g_nick_check.mutex};  // the server may have changed
        g_nick_check.results.clear();
    }
    // The game takes this as a PS4 invitation accepted in the system UI and joins the host's
    // session by itself (src/runtime_services.c, SESSION_INVITATION). Sessions are named after
    // their host. The file is stamped so a game that starts much later ignores it.
    {
        std::ofstream join(GameIni().parent_path() / L"ryunet_join.txt", std::ios::binary | std::ios::trunc);
        join << "session=cusa13795-session-" << nick << "\nhost=" << nick << "\ntime=" << int64_t(std::time(nullptr))
             << "\n";
    }
    if (a.game_running) {
        a.notice = Format(T("Invitación de %s aceptada: el juego se une a su partida en unos segundos. Si no lo "
                            "hace, abre Amigos en el juego y únete a %s.",
                            "%s's invite accepted: the game joins their match in a few seconds. If it does "
                            "not, open Friends in the game and join %s."),
                          nick, nick);
        return;
    }
    a.notice = Format(T("Invitación de %s aceptada: el juego se abre y se une a su partida (si no, abre Amigos "
                        "y únete a %s).",
                        "%s's invite accepted: the game starts and joins their match (if not, open Friends "
                        "and join %s)."),
                      nick, nick);
    BringLauncherToFront();
    a.pending_play = true;  // PollJobs presses PLAY as soon as the game folder is checked
}

/// A join secret from Discord: "ctr1|<server>|<nickname>", written by UpdateDiscord.
void OnDiscordJoin(const std::string& secret) {
    App& a = g_app;
    const auto p1 = secret.find('|');
    const auto p2 = p1 == std::string::npos ? p1 : secret.find('|', p1 + 1);
    const std::string server = p2 == std::string::npos ? "" : secret.substr(p1 + 1, p2 - p1 - 1);
    const std::string nick = p2 == std::string::npos ? "" : secret.substr(p2 + 1);
    if (secret.compare(0, p1, "ctr1") != 0 || !ValidServer(server) || NickProblem(nick)) {
        a.notice = T("Esa invitación de Discord no es de este launcher.", "That Discord invite is not from this launcher.");
        BringLauncherToFront();
        return;
    }
    if (Lower(nick) == Lower(CurrentNick())) return;  // our own invite
    if (Lower(server) != Lower(Trim(a.game.Get("online_server", "octr.ryu7w7.xyz")))) {
        // Never switch servers silently: a crafted invite could send us to any server.
        a.join_server = server;
        a.join_nick = nick;
        a.ask_join_server = true;  // JoinServerPopup
        BringLauncherToFront();
        return;
    }
    AcceptDiscordJoin(server, nick);
}

/// A JSON string field with its escapes decoded (\" \\ \/ \n \t \uXXXX, surrogate pairs), as
/// UTF-8; "" when missing.
std::string JsonText(const std::string& body, const char* key) {
    const std::string field = std::string("\"") + key + "\":\"";
    size_t i = body.find(field);
    if (i == std::string::npos) return {};
    i += field.size();
    std::string out;
    const auto put = [&out](unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    };
    const auto hex4 = [&body](size_t at, unsigned& v) {
        if (at + 4 > body.size()) return false;
        v = 0;
        for (size_t k = at; k < at + 4; ++k) {
            const char c = body[k];
            v = v * 16 + unsigned(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99);
            if (v > 0xFFFF) return false;
        }
        return true;
    };
    for (; i < body.size() && body[i] != '"'; ++i) {
        if (body[i] != '\\' || i + 1 >= body.size()) {
            out += body[i];
            continue;
        }
        const char e = body[++i];
        if (e == 'n') out += ' ';
        else if (e == 't') out += ' ';
        else if (e == 'u') {
            unsigned cp = 0, low = 0;
            if (!hex4(i + 1, cp)) break;
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < body.size() && body[i + 1] == '\\' && body[i + 2] == 'u' &&
                hex4(i + 3, low) && low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            }
            put(cp);
        } else {
            out += e;  // \" \ \/
        }
    }
    return out;
}

/// The digits of a JSON number field ("" when missing); IDs past 32 bits stay exact.
std::string JsonDigits(const std::string& body, const char* key) {
    const std::string field = std::string("\"") + key + "\":";
    const auto at = body.find(field);
    if (at == std::string::npos) return {};
    size_t end = at + field.size();
    while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) ++end;
    return body.substr(at + field.size(), end - at - field.size());
}

/// Server announcements ("maintenance in 5 minutes"): /ryunet/announce, asked every 30 s while
/// online play is on. A new one goes to the game in ryunet_announce.txt next to bbport.ini
/// (the port draws it over the picture, gpu/shim/bbport_overlay.cpp) and to the launcher's
/// notice line.
struct AnnouncePoll {
    std::mutex mutex;
    uint64_t asked = 0;
    bool in_flight = false;
    std::string seen_id;   // the last announcement handed on
    std::string pending;   // its text, for the launcher's notice (UI thread)
};
AnnouncePoll g_announce;

void PollAnnouncement() {
    App& a = g_app;
    if (!a.game.Flag("online", true)) return;
    const std::string server = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
    std::string text;
    {
        std::scoped_lock lock{g_announce.mutex};
        text.swap(g_announce.pending);
        if (!g_announce.in_flight && !server.empty() && Ticks() - g_announce.asked >= 30000) {
            g_announce.in_flight = true;
            g_announce.asked = Ticks();
            const fs::path file = GameIni().parent_path() / L"ryunet_announce.txt";
            std::thread([server, file] {
                const std::string body = HttpGet(server, L"/ryunet/announce");
                const std::string id = JsonDigits(body, "id"), msg = JsonText(body, "text");
                int expires_in = 0;
                JsonNumber(body, "expires_in", expires_in);
                std::scoped_lock done{g_announce.mutex};
                g_announce.in_flight = false;
                if (id.empty() || id == "0" || msg.empty() || expires_in <= 0 || id == g_announce.seen_id) return;
                g_announce.seen_id = id;
                g_announce.pending = msg;
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << "id=" << id << "\nlevel=" << JsonText(body, "level") << "\nexpires="
                    << int64_t(std::time(nullptr)) + expires_in << "\ntext=" << msg << "\n";
            }).detach();
        }
    }
    if (!text.empty()) a.notice = std::string(T("Aviso del servidor: ", "Server notice: ")) + text;
}

/// Called every frame: picks up finished background work.
void ApplyRecoveredNick();
void ApplyAccountNick();

void PollJobs() {
    App& a = g_app;
    RefreshGameSettings();
    UpdateDiscord();
    ApplyRecoveredNick();
    ApplyAccountNick();
    PollAnnouncement();
    DiscordPresence::SetAcceptJoinRequests(a.launcher.Flag("discord_accept_requests", true));
    for (std::string secret; DiscordPresence::TakeJoin(secret);) OnDiscordJoin(secret);
    if (a.pending_play && !a.game_running && GameReady()) {
        a.pending_play = false;
        PressPlay();
    }
    if (a.probe_done && a.probe.joinable()) a.probe.join();
    if (!a.job_finished) return;
    const Job finished = a.job.exchange(Job::None);
    a.job_finished = false;
    JoinWorker();
    if (finished == Job::Merge) {
        std::string error;
        {
            std::scoped_lock lock{a.mutex};
            error = a.job_error;
        }
        if (error.empty()) {
            SaveLauncher();
            StartCheck();
        } else {
            a.notice = error;
        }
    } else if (finished == Job::Play) {
        ReleaseGameProcess();
        a.game_running = false;
        a.show_exit = true;
        RefreshGameSettings(true);
        SyncOnlineBuffers();
        {
            std::scoped_lock lock{g_nick_check.mutex};  // a free nickname is now ours
            g_nick_check.results.clear();
        }
        a.saves_dirty = true;
        if (a.hwnd) {
            if (a.launcher.Flag("close_on_play", false)) {
                QuitLauncher();
            } else {
                BringLauncherToFront();
            }
        }
    }
}

void StartProbe() {
    App& a = g_app;
    if (a.probe.joinable()) a.probe.join();
    a.probe_done = false;
    a.probe = std::thread([] {
        App& app = g_app;
        const fs::path tool = app.bin / kGpuTool;
        std::vector<std::pair<std::string, std::string>> displays, pads;
        const Captured d = RunCaptured(WidePath(tool), Quote(WidePath(tool)) + L" --displays", app.bin, 10000);
        std::stringstream ds(d.output);
        std::string line;
        int number = 0;
        while (std::getline(ds, line)) {
            line = Trim(line);
            const auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const auto t2 = line.find('\t', t1 + 1);
            ++number;
            std::string label = std::to_string(number) + ": " + line.substr(0, t1) + " (" +
                                line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1) + ")";
            if (t2 != std::string::npos && line.substr(t2 + 1) == "1") label += g_spanish ? " - principal" : " - primary";
            displays.emplace_back(std::to_string(number), label);
        }
        const Captured p = RunCaptured(WidePath(tool), Quote(WidePath(tool)) + L" --gamepads", app.bin, 10000);
        std::stringstream ps(p.output);
        while (std::getline(ps, line)) {
            line = Trim(line);
            const auto t = line.find('\t');
            if (t != std::string::npos) pads.emplace_back(line.substr(0, t), line.substr(t + 1));
        }
        std::vector<App::Gpu> gpus;
        const Captured g = RunCaptured(WidePath(tool), Quote(WidePath(tool)) + L" --gpus", app.bin, 10000);
        std::stringstream gs(g.output);
        while (std::getline(gs, line)) {
            line = Trim(line);
            const auto t1 = line.find('\t');
            const auto t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            gpus.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
        }
        std::scoped_lock lock{app.mutex};
        app.displays = std::move(displays);
        app.gamepads = std::move(pads);
        app.gpus = std::move(gpus);
        app.probe_done = true;
    });
}

void ChooseFolder(fs::path& slot, FolderInfo& info, const wchar_t* title) {
    App& a = g_app;
    fs::path start = slot.empty() ? (a.base_dir.empty() ? a.root : a.base_dir.parent_path()) : slot.parent_path();
    auto picked = PickFolder(a.hwnd, title, start);
    if (!picked) return;
    *picked = GameRoot(*picked);
    const FolderInfo found = Analyze(*picked);
    // Whatever was picked goes where it belongs.
    switch (found.kind) {
    case Kind::Complete:
        a.game_dir = *picked;
        a.game_info = found;
        SaveLauncher();
        if (found.split) {
            StartMerge({*picked});  // Image0 + Sc0 -> the PS4 layout, next to it
        } else {
            StartCheck();
        }
        return;
    case Kind::Base:
        a.base_dir = *picked;
        a.base_info = found;
        break;
    case Kind::Update:
        a.update_dir = *picked;
        a.update_info = found;
        break;
    default:
        slot = *picked;
        info = found;
        break;
    }
    SaveLauncher();
}

// ---------------------------------------------------------------------------------------------
// Saves: <port>\user\savedata\<user>\CUSA13795\<dir> holds the game's files and <dir>.sce_sys its
// param.bin (title, subtitle, detail) and icon0.png, as src/runtime_savedata.c keeps them.






std::string FileTimeText(const fs::path& p);

std::string Stamp();

std::string Fixed(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

std::string FormatSize(std::uintmax_t bytes) {
    char buf[64];
    if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / double(1ull << 20));
    else std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
    return buf;
}

fs::path UserRoot() {
    static const auto override = Env("BB_USER_DIR");
    const auto user = override.empty() ? g_app.launcher.Get("user_dir") : override;
    return user.empty() ? g_app.data / L"user" : TextPath(user);
}
fs::path SaveRoot() {
    const fs::path base = UserRoot() / L"savedata";
    std::error_code ec;
    // Player 1's; guest<N> holds split-screen players 2-4's, emptied each run (src/runtime_savedata.c).
    if (fs::is_directory(base / L"1" / GameTitle(), ec)) return base / L"1" / GameTitle();
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (WidePath(it->path().filename()).rfind(L"guest", 0) == 0) continue;
        if (fs::is_directory(it->path() / GameTitle(), e2)) return it->path() / GameTitle();
    }
    return base / L"1" / GameTitle();
}

fs::path BackupRoot() { return g_app.data / L"backups"; }

bool IsMetaName(const std::wstring& name) {
    auto ends = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
    };
    return ends(L".sce_sys") || ends(L".memory");
}

std::vector<SaveSlot> ListSaves() {
    std::vector<SaveSlot> slots;
    const fs::path root = SaveRoot();
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || IsMetaName(WidePath(it->path().filename()))) continue;
        SaveSlot s;
        s.dir = PathText(it->path().filename());
        s.bytes = FolderBytes(it->path());
        fs::path meta = it->path();
        meta += L".sce_sys";
        const std::string raw = ReadText(meta / L"param.bin");
        if (raw.size() >= sizeof(SaveParam)) {
            SaveParam p;
            std::memcpy(&p, raw.data(), sizeof p);
            s.title = Fixed(p.title, sizeof p.title);
            s.subtitle = Fixed(p.subtitle, sizeof p.subtitle);
            s.detail = Fixed(p.detail, sizeof p.detail);
        }
        s.modified = FileTimeText(it->path());
        for (fs::recursive_directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
            std::error_code e3;
            if (f->is_regular_file(e3)) {
                const std::string t = FileTimeText(f->path());
                if (t > s.modified) s.modified = t;
            }
        }
        slots.push_back(std::move(s));
    }
    std::sort(slots.begin(), slots.end(), [](const SaveSlot& a, const SaveSlot& b) { return a.dir < b.dir; });
    return slots;
}

std::vector<Backup> ListBackups() {
    std::vector<Backup> list;
    std::error_code ec;
    for (fs::directory_iterator it(BackupRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = PathText(it->path().filename());
        if (!it->is_directory(e2) || name.rfind("saves-", 0) != 0) continue;
        list.push_back({it->path(), name, FileTimeText(it->path()), FolderBytes(it->path())});
    }
    std::sort(list.begin(), list.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
    return list;
}

bool HasSaves() {
    std::error_code ec;
    return fs::is_directory(SaveRoot(), ec) && !fs::is_empty(SaveRoot(), ec);
}

std::string CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return ec ? PathText(from) + " -> " + PathText(to) + ": " + ec.message() : std::string();
}

/// A copy of all saves in backups\saves-<date>[-label]; empty error when there is nothing to copy.
std::string BackupSaves(const char* label, fs::path* made = nullptr) {
    if (!HasSaves()) return {};
    fs::path target = BackupRoot() / Widen("saves-" + Stamp() + (label ? std::string("-") + label : ""));
    const std::string error = CopyTree(SaveRoot(), target);
    if (error.empty() && made) *made = target;
    return error;
}

std::string RestoreBackup(const fs::path& backup) {
    std::string error = BackupSaves("auto");
    if (!error.empty()) return error;
    std::error_code ec;
    fs::remove_all(SaveRoot(), ec);
    if (ec) return PathText(SaveRoot()) + ": " + ec.message();
    return CopyTree(backup, SaveRoot());
}

/// A save folder from elsewhere: this launcher's backup or another PC's CUSA13795 folder (copied
/// as it is), or a decrypted PS4 save (Apollo Save Tool, Save Wizard: its files plus
/// sce_sys\param.sfo), converted to the runtime's layout.
std::string ImportSaves(const fs::path& from) {
    std::error_code ec;
    const fs::path sfo_path = from / L"sce_sys" / L"param.sfo";
    if (fs::is_regular_file(sfo_path, ec)) {
        const auto sfo = ReadSfo(sfo_path);
        auto get = [&](const char* k) {
            const auto it = sfo.find(k);
            return it == sfo.end() ? std::string() : it->second;
        };
        std::string dir = get("SAVEDATA_DIRECTORY");
        if (dir.empty() || dir.size() >= 32 || dir.find_first_of("\\/:") != std::string::npos ||
            dir.find('\0') != std::string::npos || dir.back() == '.' || dir.back() == ' ') {
            return T("param.sfo no dice en qué ranura va la partida (SAVEDATA_DIRECTORY).",
                     "param.sfo does not name the save slot (SAVEDATA_DIRECTORY).");
        }
        const fs::path root = fs::weakly_canonical(SaveRoot(), ec);
        if (ec) return PathText(SaveRoot()) + ": " + ec.message();
        const fs::path target = root / Widen(dir);
        fs::path meta = target;
        meta += L".sce_sys";
        // Check both destinations before backup or deletion; metadata links can overwrite other saves too.
        for (const auto& path : {target, meta}) {
            if (fs::is_symlink(path, ec) || fs::weakly_canonical(path, ec) != path || ec) {
                return T("La ruta de la ranura no es válida o apunta a otra ubicación.",
                         "The save slot path is invalid or points to another location.");
            }
        }
        std::string error = BackupSaves("antes-de-importar");
        if (!error.empty()) return error;
        fs::remove_all(target, ec);
        if (ec) return PathText(target) + ": " + ec.message();
        fs::create_directories(target, ec);
        if (ec) return PathText(target) + ": " + ec.message();
        fs::create_directories(meta, ec);
        if (ec) return PathText(meta) + ": " + ec.message();
        for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() == L"sce_sys") continue;
            std::error_code e2;
            fs::copy(it->path(), target / it->path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, e2);
            if (e2) return PathText(it->path()) + ": " + e2.message();
        }
        SaveParam p{};
        std::snprintf(p.title, sizeof p.title, "%s", get("MAINTITLE").c_str());
        std::snprintf(p.subtitle, sizeof p.subtitle, "%s", get("SUBTITLE").c_str());
        std::snprintf(p.detail, sizeof p.detail, "%s", get("DETAIL").c_str());
        p.mtime = int64_t(time(nullptr));
        if (!WriteTextAtomic(meta / L"param.bin", std::string(reinterpret_cast<const char*>(&p), sizeof p))) {
            return T("No se pudo escribir param.bin.", "Could not write param.bin.");
        }
        fs::copy_file(from / L"sce_sys" / L"icon0.png", meta / L"icon0.png", fs::copy_options::overwrite_existing, ec);
        return {};
    }
    // The runtime's layout: <dir> next to <dir>.sce_sys.
    bool ours = false;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path meta = it->path();
        meta += L".sce_sys";
        std::error_code e2;
        if (!IsMetaName(WidePath(it->path().filename())) && fs::is_directory(meta, e2)) ours = true;
    }
    if (!ours) {
        return T("No reconozco esa carpeta: elige una copia de seguridad, la carpeta CUSA13795 de las partidas de "
                 "otro PC o una partida de PS4 descifrada (con sce_sys\\param.sfo).",
                 "This folder is not recognized: choose a backup, the CUSA13795 saves folder from another PC or a "
                 "decrypted PS4 save (with sce_sys\\param.sfo).");
    }
    std::string error = BackupSaves("antes-de-importar");
    if (!error.empty()) return error;
    return CopyTree(from, SaveRoot());
}

// ---------------------------------------------------------------------------------------------
// Patches: every eboot patch for CUSA13795 01.21 in patches\*.xml (shadPS4/GoldHEN XML format).
// The three community patches keep their bbport.ini keys (shared with the in-game menu); others
// are switched in patches.json ({"enabled": [...], "disabled": [...]}), as scripts/patches.py reads.



constexpr std::pair<const char*, const char*> kIniPatches[] = {
    {"CrashTeamRacing.xml/60 FPS Unlock", "patch_60fps"},
    {"CrashTeamRacing.xml/Disable Motion Blur", "patch_no_motion_blur"},
    {"CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)", "patch_pro_1080p"},
    {"CrashTeamRacingUnlockAll.xml/Unlock Everything", "patch_unlock_all"},
    // The European edition's files: the same switches (scripts/patches.py BUNDLED_FILES).
    {"CrashTeamRacingUnlockAllEU.xml/Unlock Everything", "patch_unlock_all"},
    {"CrashTeamRacingEU.xml/60 FPS Unlock", "patch_60fps"},
    {"CrashTeamRacingEU.xml/Disable Motion Blur", "patch_no_motion_blur"},
    {"CrashTeamRacingEU.xml/Resolution Patch (1080p for PS4 Pro)", "patch_pro_1080p"},
};

/// "USA" or "EU" for a CTR title ID, "" otherwise.
const char* RegionOf(const std::string& title) {
    if (title == "CUSA13795") return "USA";
    if (title == "CUSA14876") return "EU";
    return "";
}

std::string XmlUnescape(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#10;", "\n"}, {"&#13;", "\r"}, {"&#9;", "\t"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities) {
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += std::strlen(to))
            s.replace(at, std::strlen(from), to);
    }
    return s;
}

std::map<std::string, std::string> XmlAttributes(const std::string& tag) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < tag.size()) {
        const size_t eq = tag.find('=', i);
        if (eq == std::string::npos) break;
        size_t name_end = eq;
        while (name_end > i && (tag[name_end - 1] == ' ' || tag[name_end - 1] == '\n' || tag[name_end - 1] == '\r' || tag[name_end - 1] == '\t')) --name_end;
        size_t name_start = name_end;
        while (name_start > i && !std::isspace(static_cast<unsigned char>(tag[name_start - 1])) && tag[name_start - 1] != '<') --name_start;
        size_t q = eq + 1;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) break;
        const char quote = tag[q];
        const size_t close = tag.find(quote, q + 1);
        if (close == std::string::npos) break;
        out[tag.substr(name_start, name_end - name_start)] = XmlUnescape(tag.substr(q + 1, close - q - 1));
        i = close + 1;
    }
    return out;
}

void ScanModPackages() {
    App& a = g_app;
    if (!a.python || a.game_running) return;
    const fs::path script = a.root / L"scripts" / L"mod_packages.py";
    if (!fs::is_regular_file(script)) return;
    const std::wstring cmd = Quote(a.python->exe) + L" " + a.python->args + L"-X utf8 " +
        Quote(WidePath(script)) + L" --scan " + Quote(WidePath(a.data));
    const Captured r = RunCaptured(a.python->exe, cmd, a.root, 120000);
    if (!r.output.empty()) AddLog(r.output);
    if (!r.started || r.code != 0) AddLog("Mod scan failed; check the log before enabling packages.");
}

std::vector<PatchInfo> LoadPatches(bool scan = true) {
    if(scan) ScanModPackages();
    std::vector<PatchInfo> list;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(g_app.data / L"patches", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == L".xml") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const std::string xml = ReadText(file);
        // Files naming other games are skipped; a file without <ID> applies to any.
        bool any_id = false, ours = false;
        std::string file_title;
        for (size_t at = 0; (at = xml.find("<ID>", at)) != std::string::npos; at += 4) {
            const size_t close = xml.find("</ID>", at);
            if (close == std::string::npos) break;
            any_id = true;
            const std::string id = Trim(xml.substr(at + 4, close - at - 4));
            if (IsCtrTitle(id)) {
                ours = true;
                if (file_title.empty()) file_title = id;
            }
        }
        if (any_id && !ours) continue;
        for (size_t at = 0; (at = xml.find("<Metadata", at)) != std::string::npos; ++at) {
            const size_t close = xml.find('>', at);
            if (close == std::string::npos) break;
            const auto attr = XmlAttributes(xml.substr(at + 9, close - at - 9));
            auto get = [&](const char* k) {
                const auto it = attr.find(k);
                return it == attr.end() ? std::string() : it->second;
            };
            const std::string elf = get("AppElf");
            if (get("AppVer") != kUpdateVersion || !(elf.empty() || elf == "eboot.bin")) continue;
            PatchInfo p;
            p.name = get("Name");
            p.key = PathText(file.filename()) + "/" + p.name;
            p.author = get("Author");
            p.note = get("Note");
            p.version = get("PatchVer");
            p.title = file_title;
            std::string on = get("isEnabled");
            std::transform(on.begin(), on.end(), on.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            p.default_on = on == "true";
            p.mod_name = get("ModName");
            p.mod_hash = get("ModHash");
            p.mod_source = get("ModSource");
            p.mod_description = get("ModDescription");
            p.mod_versions = get("ModVersions");
            // Only our generated package catalog declares configurable controls.
            if (file.filename() == L"OCTR-Mod-Library.xml" && !p.mod_hash.empty()) {
                const size_t meta_end=xml.find("</Metadata>",close);
                for (size_t pos=close; meta_end!=std::string::npos && p.mod_options.size()<16 &&
                     (pos=xml.find("<ModOption ",pos))!=std::string::npos && pos<meta_end; ++pos) {
                    const size_t end=xml.find('>',pos), tail=xml.find("</ModOption>",end);
                    if (end==std::string::npos || end>meta_end) break;
                    const auto fields=XmlAttributes(xml.substr(pos+11,end-pos-11));
                    auto field=[&](const char* key) { const auto i=fields.find(key); return i==fields.end()?std::string():i->second; };
                    ModOption option; option.id=field("Id");option.type=field("Type");option.label=field("Label");
                    option.description=field("Description");option.value=field("Value");option.default_value=field("Default");
                    option.advanced=field("Advanced")=="true";
                    option.row=field("Row");option.column=field("Column");
                    if (option.type=="choice" && tail!=std::string::npos && tail<meta_end) {
                        for (size_t c=end;option.choices.size()<32 && (c=xml.find("<Choice ",c))!=std::string::npos && c<tail;++c) {
                            const size_t ce=xml.find('>',c);if(ce==std::string::npos || ce>tail)break;
                            const auto choice=XmlAttributes(xml.substr(c+8,ce-c-8));
                            if(choice.count("Value") && choice.count("Label"))option.choices.emplace_back(choice.at("Value"),choice.at("Label"));
                        }
                    }
                    if (!option.id.empty() && !option.label.empty() &&
                        (option.type=="boolean" || (option.type=="choice" && !option.choices.empty())))
                        p.mod_options.push_back(std::move(option));
                }
            }
            for (const auto& [key, ini] : kIniPatches)
                if (p.key == key) p.ini_key = ini;
            list.push_back(std::move(p));
        }
    }
    return list;
}

/// patches.json's "enabled" and "disabled" string lists (the file only holds those).
void ReadPatchConfig(std::vector<std::string>& enabled, std::vector<std::string>& disabled) {
    enabled.clear();
    disabled.clear();
    const std::string json = ReadText(g_app.data / L"patches.json");
    auto read_list = [&](const char* name, std::vector<std::string>& out) {
        size_t at = json.find(std::string("\"") + name + "\"");
        if (at == std::string::npos) return;
        at = json.find('[', at);
        const size_t end = at == std::string::npos ? at : json.find(']', at);
        if (at == std::string::npos || end == std::string::npos) return;
        for (size_t i = at + 1; i < end; ++i) {
            if (json[i] != '"') continue;
            std::string value;
            for (++i; i < end && json[i] != '"'; ++i) {
                if (json[i] == '\\' && i + 1 < end) ++i;
                value += json[i];
            }
            out.push_back(value);
        }
    };
    read_list("enabled", enabled);
    read_list("disabled", disabled);
}

void WritePatchConfig(const std::vector<std::string>& enabled, const std::vector<std::string>& disabled) {
    auto list = [](const std::vector<std::string>& items) {
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            out += i ? ", \"" : "\"";
            for (const char c : items[i]) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
        }
        return out + "]";
    };
    WriteTextAtomic(g_app.data / L"patches.json",
                    "{\n  \"enabled\": " + list(enabled) + ",\n  \"disabled\": " + list(disabled) + "\n}\n");
}

// ---------------------------------------------------------------------------------------------
// UI

std::string Describe(const FolderInfo& f) {
    switch (f.kind) {
    case Kind::Empty: return T("sin elegir", "not chosen");
    case Kind::Missing: return T("la carpeta no existe", "the folder does not exist");
    case Kind::NotGame:
        return T("no tiene eboot.bin y sce_sys\\param.sfo (ni Image0 y Sc0)", "no eboot.bin and sce_sys\\param.sfo (nor Image0 and Sc0)");
    case Kind::OtherTitle: return std::string(T("es otro juego o región: ", "another game or region: ")) + f.title;
    case Kind::OtherVersion: return std::string(T("actualización ", "update ")) + f.version + T(" (se necesita la 01.21)", " (01.21 is needed)");
    case Kind::Base: return std::string(T("juego base ", "base game ")) + f.title + " v" + f.version;
    case Kind::Update: return std::string(T("actualización ", "update ")) + f.version + " (" + FormatBytes(f.bytes) + ")";
    case Kind::Complete: return std::string(f.title) + " v" + f.version + " (" + FormatBytes(f.bytes) + ")";
    }
    return {};
}

// The settings pages are laid out as a form: each name on the left and every control lined up in
// one column at g_form_column, all of the same width. 0 (the other pages): ImGui's own layout,
// label after the control. FormBegin/FormEnd switch it on and off.
float g_form_column = 0.0f;

void FormBegin() { g_form_column = ImGui::GetCursorPosX() + ImGui::GetFontSize() * 14.0f; }
void FormEnd() { g_form_column = 0.0f; }
float FormControlWidth() { return ImGui::GetFontSize() * 16.0f; }

/// In a form, draws the name on the left, moves to the control column and sizes the next
/// control; returns the hidden ID to give that control. Outside a form, returns the label as is.
const char* FormLabel(const char* label) {
    if (g_form_column <= 0.0f) return label;
    static std::string id;
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(g_form_column - ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    ImGui::SameLine(g_form_column);
    ImGui::SetNextItemWidth(FormControlWidth());
    id = std::string("##") + label;
    return id.c_str();
}

/// A grey note under a control: in a form it starts at the control column.
void FormNote(const char* text) {
    if (g_form_column > 0.0f) ImGui::SetCursorPosX(g_form_column);
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/// A section title: the heading font in the launcher's cyan over a line that fades out, with air above.
void FormSection(const char* title) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.45f));
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.1f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.88f, 1.0f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (g_font_heading) ImGui::PopFont();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(p.x, p.y - 2.0f), ImVec2(p.x + w, p.y),
                                                        IM_COL32(97, 224, 255, 200), IM_COL32(97, 224, 255, 0),
                                                        IM_COL32(97, 224, 255, 0), IM_COL32(97, 224, 255, 200));
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.25f));
}

/// An on/off switch the height of a checkbox (forms use it instead of checkboxes).
bool FormToggle(const char* id, bool& on) {
    const float h = ImGui::GetFrameHeight() * 0.86f, w = h * 1.9f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    const bool changed = ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    if (changed) on = !on;
    ImDrawList* d = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 track = on ? (hovered ? IM_COL32(70, 214, 255, 255) : IM_COL32(48, 196, 247, 255))
                           : (hovered ? IM_COL32(72, 94, 130, 255) : IM_COL32(58, 79, 117, 255));
    const ImU32 knob = on ? IM_COL32(10, 34, 70, 255) : IM_COL32(190, 210, 240, 255);
    d->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), track, h * 0.5f);
    const float r = h * 0.5f - 3.0f;
    d->AddCircleFilled(ImVec2(on ? p.x + w - h * 0.5f : p.x + h * 0.5f, y + h * 0.5f), r, knob);
    if (ImGui::IsItemFocused())
        d->AddRect(ImVec2(p.x - 2, y - 2), ImVec2(p.x + w + 2, y + h + 2), ImGui::GetColorU32(ImGuiCol_Text), h * 0.5f + 2.0f);
    return changed;
}

/// A checkbox, or in a form a switch in the control column.
bool FormCheckbox(const char* label, bool& on) {
    if (g_form_column <= 0.0f) return ImGui::Checkbox(label, &on);
    return FormToggle(FormLabel(label), on);
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

const char* PerfReportsText();  // the performance diagnostic's explanation (PerfReportsPopup)

void GameFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.game.Flag(key, fallback);
    if (FormCheckbox(label, v)) {
        g_app.game.Set(key, v ? "1" : "0");
        SaveGameIni();
    }
    if (help) HelpMarker(help);
}

void LauncherFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.launcher.Flag(key, fallback);
    if (FormCheckbox(label, v)) {
        g_app.launcher.Set(key, v ? "1" : "0");
        SaveLauncher();
    }
    if (help) HelpMarker(help);
}

template <size_t N>
void ChoiceCombo(const char* label, const char* key, const Choice (&choices)[N], const char* fallback) {
    const std::string current = g_app.launcher.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    if (ImGui::BeginCombo(FormLabel(label), T(selected->es, selected->en))) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(T(c.es, c.en), &c == selected)) {
                g_app.launcher.Set(key, c.value);
                SaveLauncher();
            }
        }
        ImGui::EndCombo();
    }
}

bool CreateDesktopShortcut(std::string& out_error);
bool AddToSteam(std::string& out_msg);
void DrawMenuIcon(ImDrawList* d, int id, ImVec2 c, ImU32 ink, float s, ImU32 cut = IM_COL32(6, 39, 89, 255));

/// "Add to Steam" and its confirmation (Steam is closed first), on the Play tab and in Settings.
void SteamButton() {
    auto& a = g_app;
    if (ImGui::Button(T("Añadir a Steam", "Add to Steam"))) ImGui::OpenPopup("steam_add");
    if (ImGui::BeginPopupModal("steam_add", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("Si Steam está abierto, se cerrará para añadir el juego (vuelve a abrirlo después).",
                                 "If Steam is open, it will be closed to add the game (open it again afterwards)."));
        if (ImGui::Button(T("Añadir", "Add"))) {
            std::string message;
            AddToSteam(message);
            a.notice = message;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

bool PrimaryPlayButton(const char* label, ImVec2 size, bool disabled) {
    const bool pressed = ImGui::InvisibleButton("##primary_play", size);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 actual_size = ImGui::GetItemRectSize();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const float press_offset = active ? 2.0f : 0.0f;
    const float y0 = a.y + 2.0f + press_offset;
    const float y1 = a.y + actual_size.y - 2.0f + press_offset;
    const float cy = (y0 + y1) * 0.5f;
    const float inset = 11.0f;
    ImVec2 shape[] = {
        {a.x + inset, y0}, {a.x + actual_size.x - 22.0f, y0}, {a.x + actual_size.x - 5.0f, cy},
        {a.x + actual_size.x - 22.0f, y1}, {a.x + inset, y1}, {a.x + inset + 13.0f, cy},
    };
    ImDrawList* d = ImGui::GetWindowDrawList();
    if (!active) {
        ImVec2 shadow[6];
        for (int i = 0; i < 6; ++i) shadow[i] = ImVec2(shape[i].x, shape[i].y + 4.0f);
        d->AddConcavePolyFilled(shadow, 6, IM_COL32(0, 29, 76, disabled ? 90 : 185));
    }
    const ImU32 fill = disabled ? IM_COL32(37, 78, 119, 255)
                                : hovered ? IM_COL32(250, 255, 255, 255) : IM_COL32(224, 247, 255, 255);
    d->AddConcavePolyFilled(shape, 6, fill);
    d->AddPolyline(shape, 6, disabled ? IM_COL32(72, 118, 156, 255) : IM_COL32(255, 255, 255, 255),
                   ImDrawFlags_Closed, hovered ? 2.4f : 1.8f);
    d->AddLine({a.x + 24.0f, y0 + 5.0f}, {a.x + actual_size.x - 28.0f, y0 + 5.0f},
               disabled ? IM_COL32(112, 158, 190, 90) : IM_COL32(255, 255, 255, 170), 1.0f);
    DrawMenuIcon(d, 0, {a.x + 39.0f, cy}, disabled ? IM_COL32(164, 191, 211, 255) : IM_COL32(5, 63, 109, 255), g_ui_scale, fill);
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);
    const ImVec2 text_pos{a.x + (actual_size.x - text_size.x) * 0.5f, cy - text_size.y * 0.5f};
    d->AddText(font, font_size, text_pos,
               disabled ? IM_COL32(164, 191, 211, 255) : IM_COL32(4, 49, 86, 255), label);
    return pressed;
}

void PlayTab() {
    App& a = g_app;
    const bool busy = a.job != Job::None;
    FormSection(T("Juego", "Game"));

    const bool ready = a.game_info.kind == Kind::Complete && a.game_checked && a.game_ok;
    if (!a.python) {
        ImGui::TextColored(kRed, "%s", T("Falta Python 3: no se encontró python\\python.exe ni Python instalado.",
                                         "Python 3 is missing: no python\\python.exe and no installed Python."));
    }
    if (a.game_info.kind == Kind::Complete) {
        if (a.job == Job::Merge) {
            const size_t total = a.merge_total, done = a.merge_done;
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%zu / %zu", done, total);
            ImGui::TextColored(kYellow, "%s", T("Preparando el juego...", "Preparing the game..."));
            ImGui::ProgressBar(total ? float(done) / float(total) : 0.0f, ImVec2(-FLT_MIN, 0), overlay);
        } else if (a.game_info.split) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kYellow, "%s",
                               T("Esta carpeta tiene el formato de PS4 PKG Tool (Image0 y Sc0). Hay que prepararla: "
                                 "se crea una carpeta nueva con enlaces duros, sin tocar la tuya.",
                                 "This folder has the PS4 PKG Tool layout (Image0 and Sc0). It needs preparing: a "
                                 "new folder of hard links is created, yours is not changed."));
            ImGui::PopTextWrapPos();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(T("Preparar el juego", "Prepare the game"))) StartMerge({a.game_dir});
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s %s", T("se crea", "creates"), PathText(MergeTarget(a.game_dir)).c_str());
        } else if (!a.game_checked) {
            ImGui::TextColored(kYellow, "%s", T("Comprobando el juego...", "Checking the game..."));
        } else if (a.game_ok) {
            ImGui::TextColored(kGreen, "%s %s", T("Listo:", "Ready:"), Describe(a.game_info).c_str());
            if (a.game.Flag("online", true)) {
                std::string srv = a.game.Get("online_server", "octr.ryu7w7.xyz");
                if (srv.empty()) srv = "octr.ryu7w7.xyz";
                const std::string nick = CurrentNick().empty() ? std::string("-") : CurrentNick();
                ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "[Online RyuNET] %s: %s  |  %s: %s",
                                   T("Servidor", "Server"), srv.c_str(), T("Apodo", "Nick"), nick.c_str());
                {
                    ImVec4 nat_color;
                    const std::string nat = NatLabel(Trim(srv), nat_color);
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "  |  ");
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(nat_color, "%s", nat.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", T("Si otros jugadores pueden conectarse directamente contigo. Más detalles en la "
                                                  "pestaña Online. Tu IP no se muestra.",
                                                  "Whether other players can connect to you directly. Details in the Online "
                                                  "tab. Your IP is not shown."));
                }
                if (!NickInfo().banned.empty()) ImGui::TextColored(kRed, "%s", T("Baneado del online: mira la pestaña Online.", "Banned from online play: see the Online tab."));
                if (const char* problem = NickProblem(CurrentNick()))
                    ImGui::TextColored(kYellow, "%s %s", problem, T("Se te pedirá al pulsar Jugar.", "You will be asked when you press Play."));
            } else {
                ImGui::TextColored(kGrey, "[Online] %s", T("Modo offline (actívalo en la pestaña Online)", "Offline mode (enable it in the Online tab)"));
            }
        } else {
            ImGui::TextColored(kRed, "%s", T("Estos archivos no sirven:", "These files do not work:"));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(a.check_message.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextColored(kGrey, "%s", PathText(a.game_dir).c_str());
        if (ImGui::Button(T("Cambiar carpeta del juego...", "Change game folder...")) && !busy && !a.game_running) {
            ChooseFolder(a.game_dir, a.game_info, L"CUSA13795 + 1.21");
        }
    } else {
        if (a.game_info.kind == Kind::Update) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(),
                               T("solo tiene la actualización 1.21, falta el juego base",
                                 "only has the 1.21 update, the base game is missing"));
        } else if (a.game_info.kind == Kind::Base) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(),
                               T("es el juego base sin la actualización 1.21", "is the base game without the 1.21 update"));
        } else if (a.game_info.kind != Kind::Empty) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(), Describe(a.game_info).c_str());
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(T("Necesitas tu propio dump de PS4: el juego base (CUSA14876 en Europa, CUSA13795 en "
                                 "América) y la actualización 1.21. Si ya los tienes juntos en una carpeta, elige esa.",
                                 "You need your own PS4 dump: the base game (CUSA14876 in Europe, CUSA13795 in the "
                                 "Americas) and the 1.21 update. If they are already merged in one folder, choose that one."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (LauncherUI::Table table{"setup", 3}) {
            table.StretchColumn("what", 0.8f);
            table.StretchColumn("state", 1.4f);
            table.FixedColumn("button", LauncherUI::Table::ButtonWidth(T("Elegir...", "Choose...")));
            auto row = [&](const char* what, fs::path& slot, FolderInfo& info, Kind good, const wchar_t* title, const char* id) {
                table.Row();
                table.Cell();
                ImGui::TextUnformatted(what);
                table.Cell();
                const bool ok = info.kind == good;
                ImGui::TextColored(ok ? kGreen : (info.kind == Kind::Empty ? kGrey : kRed), "%s", Describe(info).c_str());
                if (!slot.empty()) ImGui::TextColored(kGrey, "%s", PathText(slot).c_str());
                table.Cell();
                ImGui::PushID(id);
                if (table.Action(T("Elegir...", "Choose...")) && !busy) ChooseFolder(slot, info, title);
                ImGui::PopID();
            };
            row(T("Juego base", "Base game"), a.base_dir, a.base_info, Kind::Base, L"CUSA13795", "base");
            row(T("Actualización 1.21", "Update 1.21"), a.update_dir, a.update_info, Kind::Update, L"CUSA13795-patch", "update");
        }
        const bool can_merge = a.base_info.kind == Kind::Base && a.update_info.kind == Kind::Update;
        if (a.job == Job::Merge) {
            const size_t total = a.merge_total, done = a.merge_done;
            const float fraction = total ? float(done) / float(total) : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%zu / %zu", done, total);
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
        } else {
            ImGui::BeginDisabled(!can_merge || busy);
            if (ImGui::Button(T("Preparar el juego (base + 1.21)", "Prepare the game (base + 1.21)"))) {
                StartMerge({a.base_dir, a.update_dir});
            }
            ImGui::EndDisabled();
            if (can_merge) {
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "%s %s", T("se crea", "creates"),
                                   PathText(MergeTarget(a.base_dir)).c_str());
            }
            HelpMarker(T("Une la actualización con el juego base en una carpeta nueva usando enlaces duros: no ocupa "
                         "espacio extra ni cambia tus carpetas originales (si están en discos distintos, se copia).",
                         "Merges the update over the base game into a new folder with hard links: no extra disk "
                         "space and your original folders are not changed (copied if they are on different drives)."));
        }
        ImGui::Spacing();
        if (ImGui::Button(T("Ya tengo el juego preparado...", "I already have the merged game...")) && !busy) {
            ChooseFolder(a.game_dir, a.game_info, L"CUSA13795-1.21");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const float play_height = ImGui::GetFontSize() * 3.0f;
    if (a.game_running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.14f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.20f, 0.28f, 1.0f));
        if (ImGui::Button(T("Cerrar el juego", "Stop the game"), ImVec2(-FLT_MIN, play_height))) StopGame();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::BeginDisabled(!ready || busy || !a.python);
        ImGui::PushFont(g_ctr_font, ImGui::GetStyle().FontSizeBase * 1.5f);
        if (PrimaryPlayButton(T("JUGAR", "PLAY"), ImVec2(-FLT_MIN, play_height),
                              !ready || busy || !a.python)) PressPlay();
        ImGui::PopFont();
        ImGui::EndDisabled();
    }

    if (ready && !a.game_running) {
        ImGui::Spacing();
        if (ImGui::Button(T("Crear acceso directo en el Escritorio", "Create Desktop Shortcut"))) {
            std::string err;
            if (CreateDesktopShortcut(err)) {
                a.notice = T("Acceso directo creado en el Escritorio con éxito (inicia directo sin el launcher).",
                             "Desktop shortcut created successfully (launches directly without launcher).");
            } else {
                a.notice = err;
            }
        }
        ImGui::SameLine();
        SteamButton();
    }

    std::string status;
    {
        std::scoped_lock lock{a.mutex};
        status = a.last_status;
    }
    if (a.game_running) {
        ImGui::TextColored(kYellow, "%s", T("El juego está abierto. El primer arranque compila shaders y tarda un poco.",
                                            "The game is running. The first start compiles shaders and takes a while."));
    } else if (a.show_exit) {
        if (a.exit_code == 0 || a.stopped) ImGui::TextColored(kGrey, "%s", T("El juego se cerró.", "The game was closed."));
        else if (a.exit_code == kUnsupportedGpuCode) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
#ifdef _WIN32
            ImGui::TextWrapped("%s", T("Tu tarjeta gráfica o su driver no soporta Vulkan 1.3 / sparse binding, que el juego necesita. Actualiza el "
                                       "driver de la GPU; si ya es el último, la tarjeta es demasiado antigua para Windows (en "
                                       "Linux, el driver Mesa da Vulkan 1.3 a muchas GPU antiguas).",
                                       "Your graphics card or its driver doesn't support Vulkan 1.3 / sparse binding, which the game needs. Update "
                                       "the GPU driver; if it is already the latest, the card is too old for Windows (on Linux, "
                                       "the Mesa driver gives Vulkan 1.3 to many older GPUs)."));
#else
            ImGui::TextWrapped("%s", T("La GPU o su driver no admite las funciones Vulkan necesarias. Actualiza el driver "
                                       "de tu distribución o selecciona otra GPU.",
                                       "The GPU or its driver lacks required Vulkan features. Update your distribution's "
                                       "GPU driver or select another GPU."));
#endif
            ImGui::PopStyleColor();
        }
        else if (a.exit_code == kOutOfMemoryCode) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
#ifdef _WIN32
            ImGui::TextWrapped("%s", T("El juego se quedó sin memoria: Windows no pudo darle la que necesita (error 1455, la RAM más el "
                                       "archivo de paginación no alcanzan). Agranda el archivo de paginación: busca \"Ajustar la "
                                       "apariencia y rendimiento de Windows\" > Opciones avanzadas > Memoria virtual > Cambiar, marca "
                                       "\"Administrar automáticamente\" o pon al menos 8192 MB en C:, y reinicia el PC. Antes de jugar, "
                                       "cierra los programas y overlays que no uses.",
                                       "The game ran out of memory: Windows could not give it what it needs (error 1455, RAM plus the "
                                       "page file are not enough). Enlarge the page file: search \"Adjust the appearance and "
                                       "performance of Windows\" > Advanced > Virtual memory > Change, tick \"Automatically manage\" "
                                       "or set at least 8192 MB on C:, and restart the PC. Before playing, close the programs and "
                                       "overlays you don't use."));
#else
            ImGui::TextWrapped("%s", T("El juego se quedó sin memoria. Cierra otras aplicaciones y comprueba la RAM y el swap disponibles.",
                                       "The game ran out of memory. Close other applications and check available RAM and swap."));
#endif
            ImGui::PopStyleColor();
        }
        else ImGui::TextColored(kRed, T("El juego se cerró con un error (código %lu). Mira el registro abajo o en last_run.log.",
                                        "The game ended with an error (code %lu). See the log below or last_run.log."),
                                (unsigned long)a.exit_code);
    }
    if (!status.empty()) ImGui::TextColored(kGrey, "%s", status.c_str());

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("Registro", "Log"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::scoped_lock lock{a.mutex};
            ImGuiListClipper clipper;
            clipper.Begin(int(a.log.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) ImGui::TextUnformatted(a.log[size_t(i)].c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

// bbport.ini render_res: the scene size (scripts/patches.py, run.py; docs/sacredfire/13-resolution.md).
struct RenderRes {
    const char* value;
    const char* es;
    const char* en;
};
constexpr RenderRes kRenderRes[] = {
    {"480", "480p (854 x 480, PCs muy modestos)", "480p (854 x 480, very low-end PCs)"},
    {"720", "720p (1280 x 720, PCs modestos)", "720p (1280 x 720, low-end PCs)"},
    {"1080", "1080p (1920 x 1080)", "1080p (1920 x 1080)"},
    {"1440", "1440p (2560 x 1440, PS4 Pro)", "1440p (2560 x 1440, PS4 Pro)"},
    {"1620", "1620p (2880 x 1620)", "1620p (2880 x 1620)"},
    {"2160", "4K (3840 x 2160)", "4K (3840 x 2160)"},
};

// The render_res in effect: the setting, or for older settings the Pro mode and 1080p patch.
std::string EffectiveRenderRes() {
    const std::string v = g_app.game.Get("render_res");
    for (const auto& r : kRenderRes)
        if (v == r.value) return v;
    return "1080";
}

/// bbport.ini temporal (the upscaler), with the keys of v0.3 test builds before it: fsr3=1, fsr1=0.
std::string GameTemporal() {
    const Ini& game = g_app.game;
    const std::string value = game.Get("temporal");
    for (const char* known : {"off", "fsr1", "fsr3", "dlss", "fsr4"})
        if (value == known) return value;
    if (game.Flag("fsr3", false)) return "fsr3";
    return game.Flag("fsr1", true) ? "fsr1" : "off";
}

/// The separately distributed NGX bridge and NVIDIA runtime must both be installed.
bool DlssFilesPresent() {
    std::error_code ec;
#ifdef _WIN32
    return fs::is_regular_file(g_app.root / "bin/sacredfire_dlss.dll", ec) &&
           fs::is_regular_file(g_app.root / "bin/nvngx_dlss.dll", ec);
#else
    for (const auto& dir : {g_app.root / "bin", g_app.root / "out/dlss", TextPath(Env("BB_DLSS_DIR"))}) {
        if (!dir.empty() && fs::is_regular_file(dir / "libsacredfire_dlss.so", ec) &&
            fs::is_regular_file(dir / "libnvidia-ngx-dlss.so", ec)) return true;
    }
    return false;
#endif
}

/// v0.7.4: FSR 4's model files (the fsr4_shaders folder next to the launcher, where sacredfire.exe
/// looks for them; not included). Without them FSR 4 is still listed, and the game uses FSR 3.
bool Fsr4FilesPresent() {
    static const bool present = [] {
        std::error_code ec;
        return fs::is_regular_file(g_app.root / L"fsr4_shaders" / L"LICENSE-FSR4-v07.txt", ec);
    }();
    return present;
}

/// A combo over a bbport.ini key; true when the player picked another value (already set).
template <size_t N>
bool GameChoiceCombo(const char* label, const char* key, const Choice (&choices)[N], const char* fallback,
                     float width_em) {
    App& a = g_app;
    const std::string current = a.game.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width_em);
    if (ImGui::BeginCombo(FormLabel(label), T(selected->es, selected->en))) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(T(c.es, c.en), &c == selected) && &c != selected) {
                a.game.Set(key, c.value);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void RenderResCombo() {
    App& a = g_app;
    const std::string current = EffectiveRenderRes();
    const RenderRes* selected = &kRenderRes[0];
    for (const auto& r : kRenderRes)
        if (current == r.value) selected = &r;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo(FormLabel(T("Resolución interna", "Internal resolution")), T(selected->es, selected->en))) {
        for (const auto& r : kRenderRes) {
            if (ImGui::Selectable(T(r.es, r.en), &r == selected)) {
                a.game.Set("render_res", r.value);
                SaveGameIni();
            }
        }
        ImGui::EndCombo();
    }
    HelpMarker(T("El tamaño al que el juego dibuja la escena 3D. Por encima de 1080p usa el modo PS4 Pro del juego "
                 "y la imagen sale en 4K: en una pantalla 1080p eso quita los dientes de sierra. "
                 "4K necesita una GPU potente (una RTX 3070 va a unos 100 FPS); 720p y 480p alivian PCs modestos "
                 "(la imagen sale a 1080p).",
                 "The size the game draws its 3D scene at. Above 1080p it uses the game's PS4 Pro path (turned "
                 "on automatically) and the picture is output at 4K: on a 1080p screen that removes jagged "
                 "edges. 4K needs a strong GPU (an RTX 3070 runs it at about 100 FPS); 720p and 480p help low-end "
                 "PCs (the picture is output at 1080p)."));
}

// Aspect ratio is independent of the corrected widescreen layouts: 16:10 prepares the
// display/camera in scripts/aspect_ratio.py; 21:9/32:9 keep tcosi's menus and HUD.
/// Windows' own graphics preference for the game (Settings > Display > Graphics), so a laptop's
/// switchable graphics runs it on the chosen GPU: 2 high performance, 1 power saving, 0 removes
/// the launcher's entry (Windows decides again).
void SetWindowsGpuPreference(int preference);

/// The place of gpus[i] among the GPUs with its name (0: the first), and how many have it.
int GpuNth(const std::vector<App::Gpu>& gpus, size_t i, int& same) {
    int nth = 0;
    same = 0;
    for (size_t j = 0; j < gpus.size(); ++j) {
        if (gpus[j].name != gpus[i].name) continue;
        if (j < i) ++nth;
        ++same;
    }
    return nth;
}

/// GPUs that share a name are told apart: #1, #2 in Vulkan's order.
std::string GpuLabel(const std::vector<App::Gpu>& gpus, size_t i) {
    const App::Gpu& gpu = gpus[i];
    std::string label = gpu.name;
    int same = 0;
    const int nth = GpuNth(gpus, i, same);
    if (same > 1) label += " #" + std::to_string(nth + 1);
    if (gpu.kind == "discrete") label += T(" (dedicada)", " (dedicated)");
    else if (gpu.kind == "integrated") label += T(" (integrada)", " (integrated)");
    else if (gpu.kind == "cpu") label += T(" (por software, muy lenta)", " (software, very slow)");
    const auto dot = gpu.vulkan.find('.');
    const int major = std::atoi(gpu.vulkan.c_str()), minor = dot == std::string::npos ? 0 : std::atoi(gpu.vulkan.c_str() + dot + 1);
    if (major < 1 || (major == 1 && minor < 3))
        label += std::string(T(" - Vulkan ", " - Vulkan ")) + gpu.vulkan + T(", no sirve para el juego", ", not enough for the game");
    return label;
}

/// The graphics card: automatic (the engine prefers a dedicated GPU with Vulkan 1.3) or one of
/// the GPUs Vulkan sees, by name (BB_GPU_NAME; one that is gone falls back to automatic) and,
/// when several share it, which of them (gpu_nth, BB_GPU_ID).
void GpuCombo() {
    App& a = g_app;
    std::scoped_lock lock{a.mutex};
    const std::string current = a.launcher.Get("gpu");
    const int index = GpuIndex(a.gpus, current, a.launcher.Int("gpu_nth", 0));
    const auto chosen = [&](size_t i) { return a.gpus[i].name == current && (index < 0 || index == int(i)); };
    std::string preview = T("Automática", "Automatic");
    bool listed = false;
    for (size_t i = 0; i < a.gpus.size(); ++i)
        if (chosen(i)) { preview = GpuLabel(a.gpus, i); listed = true; }
    if (!current.empty() && !listed) preview = current + T(" (no detectada)", " (not detected)");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    if (ImGui::BeginCombo(FormLabel(T("Tarjeta gráfica", "Graphics card")), preview.c_str())) {
        if (ImGui::Selectable(T("Automática", "Automatic"), current.empty())) {
            a.launcher.Set("gpu", "");
            a.launcher.Erase("gpu_nth");
            SaveLauncher();
            SetWindowsGpuPreference(0);
        }
        for (size_t i = 0; i < a.gpus.size(); ++i) {
            const auto& gpu = a.gpus[i];
            ImGui::PushID(int(i));  // GPUs that share a name share their label too
            if (ImGui::Selectable(GpuLabel(a.gpus, i).c_str(), chosen(i))) {
                int same = 0;
                const int nth = GpuNth(a.gpus, i, same);
                a.launcher.Set("gpu", gpu.name);
                if (nth) a.launcher.Set("gpu_nth", std::to_string(nth));
                else a.launcher.Erase("gpu_nth");
                SaveLauncher();
                SetWindowsGpuPreference(gpu.kind == "discrete" ? 2 : gpu.kind == "integrated" ? 1 : 0);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    HelpMarker(T("En notebooks con dos GPU, elige la dedicada (NVIDIA o AMD) si el juego arranca con la integrada. "
                 "Automática usa la dedicada cuando tiene Vulkan 1.3. Se aplica al iniciar el juego. Si no ves tu "
                 "GPU dedicada en la lista, actualiza su driver.",
                 "On laptops with two GPUs, choose the dedicated one (NVIDIA or AMD) if the game starts on the "
                 "integrated one. Automatic uses the dedicated GPU when it has Vulkan 1.3. Applied when the game "
                 "starts. If your dedicated GPU is not listed, update its driver."));
}

void AspectRatioSettings() {
    App& a = g_app;
    std::string mode = a.game.Get("aspect_ratio", a.game.Get("widescreen_layout", "16x9"));
    static constexpr Choice ratios[] = {{"16:9", "16:9", "16x9"},
                                        {"16:10", "16:10", "16x10"},
                                        {"21:9", "21:9", "21x9"}, {"32:9", "32:9", "32x9"}};
    if (mode != "16x10" && mode != "21x9" && mode != "32x9") mode = "16x9";
    const char* preview = "16:9";
    for (const auto& c : ratios) if (mode == c.value) preview = T(c.es, c.en);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo(FormLabel(T("Relación de aspecto", "Aspect ratio")), preview)) {
        for (const auto& c : ratios) {
            if (ImGui::Selectable(T(c.es, c.en), mode == c.value)) {
                mode = c.value;
                a.game.Set("aspect_ratio", mode);
                a.game.Set("widescreen_layout", mode == "21x9" || mode == "32x9" ? mode : "off");
                if (mode != "16x9") {
                    a.game.Set("ultrawide", "0");
                    std::string output = a.game.Get("aspect_output", a.game.Get("widescreen_output"));
                    const bool tall = output == "1920x1200" || output == "1280x800" || output == "2560x1600";
                    const bool wide = output == "3440x1440" || output == "2560x1080";
                    const bool super = output == "5120x1440" || output == "3840x1080" ||
                                       output == "2560x720" || output == "7680x2160";
                    if (!((mode == "16x10" && tall) || (mode == "21x9" && wide) || (mode == "32x9" && super)))
                        output = mode == "16x10" ? "1920x1200" : mode == "21x9" ? "3440x1440" : "5120x1440";
                    a.game.Set("aspect_output", output);
                    if (mode == "21x9" || mode == "32x9") a.game.Set("widescreen_output", output);
                }
                SaveGameIni();
            }
        }
        ImGui::EndCombo();
    }
    if (mode == "16x9") return;
    static constexpr Choice tall[] = {{"1920 x 1200", "1920 x 1200", "1920x1200"},
                                      {"1280 x 800", "1280 x 800", "1280x800"},
                                      {"2560 x 1600", "2560 x 1600", "2560x1600"}};
    static constexpr Choice wide[] = {{"3440 x 1440", "3440 x 1440", "3440x1440"},
                                      {"2560 x 1080", "2560 x 1080", "2560x1080"}};
    static constexpr Choice super[] = {{"5120 x 1440", "5120 x 1440", "5120x1440"},
                                       {"3840 x 1080", "3840 x 1080", "3840x1080"},
                                       {"2560 x 720", "2560 x 720", "2560x720"},
                                       {"7680 x 2160", "7680 x 2160", "7680x2160"}};
    const std::string output = a.game.Get("aspect_output", a.game.Get("widescreen_output"));
    const char* label = T("Tamaño de pantalla", "Screen size");
    bool changed;
    if (mode == "16x10") {
        changed = GameChoiceCombo(label, "aspect_output", tall, output.empty() ? "1920x1200" : output.c_str(), 16.0f);
        FormNote(T("16:10: pantalla y cámara adaptadas; efectos del HUD alineados. "
            "Actualización 1.21, USA o Europa. Sin FSR ni DLSS. Requiere reiniciar.",
            "16:10 display and camera; aligned HUD effects. USA or Europe 1.21. "
            "No FSR or DLSS. Requires a restart."));
    } else if (mode == "21x9") {
        changed = GameChoiceCombo(label, "aspect_output", wide, output.empty() ? "3440x1440" : output.c_str(), 16.0f);
    } else {
        changed = GameChoiceCombo(label, "aspect_output", super, output.empty() ? "5120x1440" : output.c_str(), 16.0f);
    }
    if (changed) {
        a.game.Set("aspect_ratio", mode);
        a.game.Set("widescreen_layout", mode == "21x9" || mode == "32x9" ? mode : "off");
        if (mode == "21x9" || mode == "32x9") a.game.Set("widescreen_output", a.game.Get("aspect_output"));
        SaveGameIni();
    }
    if (mode != "16x10") FormNote(T("Menús, fondos y HUD centrado corregidos. En línea o sin conexión con la actualización 1.21, USA o Europa "
        "(si no, el juego arranca en 16:9). Sin FSR ni DLSS; la resolución interna elige la variante.",
        "Corrected menus, backgrounds and a centered HUD. Online or offline, with the 1.21 update, USA or Europe "
        "(otherwise the game starts at 16:9). No FSR or DLSS; the internal resolution picks the variant."));
}

/// Player 1's controller, the buttons the game shows, and split screen (Gamepad page).
void ControllerSettings() {
    App& a = g_app;
    {
        std::scoped_lock lock{a.mutex};
        const std::string current = a.launcher.Get("gamepad");
        std::string preview = T("El primero conectado", "First connected");
        bool listed = current.empty();
        for (const auto& [guid, name] : a.gamepads)
            if (guid == current) {
                preview = name;
                listed = true;
            }
        if (!listed) preview = a.launcher.Get("gamepad_name", current) + T(" (no conectado)", " (not connected)");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo(FormLabel(T("Mando", "Controller")), preview.c_str())) {
            if (ImGui::Selectable(T("El primero conectado", "First connected"), current.empty())) {
                a.launcher.Set("gamepad", "");
                a.launcher.Set("gamepad_name", "");
                SaveLauncher();
            }
            for (const auto& [guid, name] : a.gamepads) {
                ImGui::PushID(guid.c_str());
                if (ImGui::Selectable(name.c_str(), guid == current)) {
                    a.launcher.Set("gamepad", guid);
                    a.launcher.Set("gamepad_name", name);
                    SaveLauncher();
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!a.probe_done && a.probe.joinable());
    if (ImGui::Button(T("Buscar mandos", "Refresh"))) {
        if (a.probe.joinable()) a.probe.join();
        StartProbe();
    }
    ImGui::EndDisabled();
    // bbport.ini button_prompts (scripts/button_prompts.py): the buttons the game shows.
    {
        static constexpr Choice kButtonPrompts[] = {
            {"PlayStation", "PlayStation", "playstation"},
            {"Xbox", "Xbox", "xbox"},
        };
        if (GameChoiceCombo(T("Iconos de botones", "Button prompts"), "button_prompts", kButtonPrompts, "playstation", 12.0f))
            SaveGameIni();
        HelpMarker(T("Los botones que muestra el juego en menús y avisos. Xbox: A, B, X, Y, LB, RB, LT, RT y View. "
                     "Se aplica al pulsar Jugar.",
                     "The buttons the game shows in menus and prompts. Xbox: A, B, X, Y, LB, RB, LT, RT and View. "
                     "Applies when you press Play."));
    }

    FormSection(T("Pantalla dividida", "Split screen"));
    GameFlag("split_auto", T("Asignar los mandos automáticamente", "Assign controllers automatically"), true,
             T("Para jugar de 2 a 4 con varios mandos: cada mando que conectes es el siguiente jugador (el primero, el "
               "jugador 1). Desactivado, una persona puede cambiar de mando cuando quiera, y los mandos de los jugadores "
               "2-4 se eligen en el menú del juego (Insert o L3+R3), Controllers.",
               "For 2 to 4 players with several controllers: each controller you connect is the next player (the first "
               "one, player 1). Off, one person can switch controllers at will, and players 2-4's controllers are chosen "
               "in the in-game menu (Insert or L3+R3), Controllers."));
    FormNote(T("Más mandos para la pantalla dividida: menú del juego (Insert o L3+R3), Controllers. "
               "Quién juega con el teclado se elige en la página Teclado.",
               "More controllers for split screen: in-game menu (Insert or L3+R3), Controllers. "
               "Who plays on the keyboard is chosen on the Keyboard page."));
}

/// bbport.ini port.N=keyboard (N 2..4): that split-screen player plays on the keyboard and player 1
/// on the controller alone (src/runtime_pad.c). None: the keyboard drives player 1 too (Keyboard page).
void KeyboardPlayerSetting() {
    App& a = g_app;
    const auto port_key = [](int n) { return "port." + std::to_string(n); };
    int current = 1;
    for (int n = 2; n <= 4; ++n)
        if (Lower(Trim(a.game.Get(port_key(n)))) == "keyboard") current = n;
    const auto name = [](int n) {
        return n == 1 ? std::string(T("Jugador 1 (junto al mando)", "Player 1 (with the controller)"))
                      : std::string(T("Jugador ", "Player ")) + std::to_string(n);
    };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo(FormLabel(T("El teclado juega como", "The keyboard plays as")), name(current).c_str())) {
        for (int n = 1; n <= 4; ++n) {
            if (!ImGui::Selectable(name(n).c_str(), n == current) || n == current) continue;
            for (int k = 2; k <= 4; ++k)
                if (Lower(Trim(a.game.Get(port_key(k)))) == "keyboard") a.game.Set(port_key(k), "");
            if (n > 1) a.game.Set(port_key(n), "keyboard");
            SaveGameIni();
        }
        ImGui::EndCombo();
    }
    HelpMarker(T("Para jugar dos en pantalla dividida, uno con teclado y otro con mando, elige Jugador 2: el mando "
                 "queda para el jugador 1 y el teclado para el 2 (Espacio es X). Si juegas solo con teclado, déjalo "
                 "en Jugador 1. También se cambia en el menú del juego (Insert o L3+R3).",
                 "For two players in split screen, one on the keyboard and one on a controller, choose Player 2: "
                 "the controller stays player 1 and the keyboard plays player 2 (Space is Cross). If you play on "
                 "the keyboard alone, leave it on Player 1. The in-game menu (Insert or L3+R3) changes it too."));
}

/// Settings > Graphics: the engine, the frame rate and the image quality.
void GraphicsSettings() {
    App& a = g_app;
    FormSection(T("Rendimiento", "Performance"));
    {
        // renderer=shadps4|native (bbport.ini; run.py sets the native renderer up and builds its
        // shaders from the player's game the first time)
        static constexpr Choice kRenderer[] = {{"Vulkan (shadPS4)", "Vulkan (shadPS4)", "shadps4"},
                                               {"Vulkan nativo", "Native Vulkan", "native"},
                                               #ifdef _WIN32
                                               {"DirectX 11 (experimental)", "DirectX 11 (experimental)", "d3d11"},
#endif
        };
        if (GameChoiceCombo(T("Motor gráfico", "Graphics engine"), "renderer", kRenderer, "shadps4", 16.0f)) SaveGameIni();
        HelpMarker(T("Vulkan (shadPS4) es el motor de siempre. El nativo dibuja el juego directamente en Vulkan, sin la "
                     "emulación de la GPU de PS4: más FPS y menos tirones. La primera vez prepara "
                     "sus shaders a partir de tu copia del juego (unos minutos). Con el nativo no hay FSR 3 ni DLSS "
                     "(FSR 1 sí). Si algo va mal, vuelve a Vulkan (shadPS4).",
                     "Vulkan (shadPS4) is the usual engine. The native one draws the game straight with Vulkan, without "
                     "emulating the PS4's GPU: more FPS and fewer hitches. The first time it "
                     "builds its shaders from your copy of the game (a few minutes). The native engine has no FSR 3 or "
                     "DLSS (FSR 1 works). If something goes wrong, go back to Vulkan (shadPS4)."));
    }
    GpuCombo();
    {
        int limit = a.game.Int("fps_limit", 60);
        auto name = [](int v) -> std::string {
            if (v == 0) return T("Sin límite", "No limit");
            return std::to_string(v) + " FPS";
        };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo(FormLabel(T("Límite de FPS", "FPS limit")), name(limit).c_str())) {
            for (int v : kFpsLimits) {
                if (ImGui::Selectable(name(v).c_str(), v == limit)) {
                    a.game.Set("fps_limit", std::to_string(v));
                    SaveGameIni();
                }
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("Con el parche de 60 FPS el juego va tan rápido como permita tu PC. En una pantalla de 144 Hz sin "
                     "G-Sync/FreeSync, 72 reparte los fotogramas de forma pareja.",
                     "With the 60 FPS patch the game runs as fast as your PC allows. On a 144 Hz screen without "
                     "G-Sync/FreeSync, 72 divides the refresh evenly."));
    }
    GameFlag("async_pipelines", T("Shaders en segundo plano (experimental)", "Background shaders (experimental)"), false,
             T("Compila los shaders en segundo plano: menos tirones la primera vez que aparece un efecto; a veces un "
               "efecto tarda un fotograma en verse.",
               "Compiles shaders in the background: fewer hitches when an effect is first seen; an effect may show up "
               "a frame late."));

    FormSection(T("Calidad de imagen", "Image quality"));
    const std::string temporal = GameTemporal();
    const bool dlss_sized = temporal == "dlss" && a.game.Get("dlss_quality", "auto") != "auto";
    ImGui::BeginDisabled(dlss_sized);
    RenderResCombo();
    ImGui::EndDisabled();
    if (dlss_sized) FormNote(T("La resolución interna la decide la calidad de DLSS.", "The internal resolution is set by the DLSS quality."));
    {
        // temporal=off|fsr1|fsr3|dlss|fsr4 (bbport.ini): one upscaler at a time. FSR 4 runs with its
        // model files (Fsr4FilesPresent); without them the line below says what is missing.
        static constexpr Choice kTemporalFsr4[] = {{"Desactivado", "Off", "off"},
                                                   {"AMD FSR 1", "AMD FSR 1", "fsr1"},
                                                   {"AMD FSR 3 (experimental)", "AMD FSR 3 (experimental)", "fsr3"},
                                                   {"NVIDIA DLSS (experimental)", "NVIDIA DLSS (experimental)", "dlss"},
                                                   {"AMD FSR 4 (experimental)", "AMD FSR 4 (experimental)", "fsr4"}};
        // the native engine has DLSS of its own (gpu/shim/bbport_native_vk_dlss.cpp); FSR 3 and
        // FSR 4 live in shadPS4's rasterizer only
        static constexpr Choice kTemporalNative[] = {{"Desactivado", "Off", "off"},
                                                     {"AMD FSR 1", "AMD FSR 1", "fsr1"},
                                                     {"NVIDIA DLSS (experimental)", "NVIDIA DLSS (experimental)", "dlss"},
        };
        const bool native = a.game.Get("renderer") == "native";
        const bool fsr4 = Fsr4FilesPresent();
        const bool picked = native ? GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporalNative,
                                                     temporal == "off" || temporal == "dlss" ? temporal.c_str() : "fsr1",
                                                     16.0f)
                                   : GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporalFsr4,
                                                     temporal.c_str(), 16.0f);
        if (picked) {
            a.game.Erase("fsr1");
            a.game.Erase("fsr3");
            SaveGameIni();
        }
        HelpMarker(T("FSR 1 agranda una escena más pequeña con bordes más nítidos que el estirado simple del juego. "
                     "FSR 3 y DLSS reconstruyen el detalle a lo largo de varios cuadros con la profundidad y el movimiento "
                     "del juego, y a la misma resolución también suavizan los bordes. DLSS necesita una tarjeta NVIDIA "
                     "RTX y FSR 4 una gráfica con soporte INT8 y sus archivos (si no, se usa FSR 3). Experimentales: "
                     "si ves estelas o parpadeos, cambia a FSR 1.",
                     "FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. FSR 3 and DLSS "
                     "rebuild detail over several frames from the game's depth and motion, and smooth edges even at the "
                     "same resolution. DLSS needs an NVIDIA RTX card and FSR 4 a GPU with INT8 support and its files "
                     "(FSR 3 runs otherwise). Experimental: switch to FSR 1 if you see trails or shimmering."));
        if (native && (temporal == "fsr3" || temporal == "fsr4")) {
            FormNote(T("FSR 3 y FSR 4 no funcionan con el motor nativo: se usa FSR 1.",
                       "FSR 3 and FSR 4 do not work with the native engine: FSR 1 is used."));
        } else if (temporal == "dlss" && !DlssFilesPresent()) {
            FormNote(T("DLSS necesita sacredfire_dlss.dll y nvngx_dlss.dll en la carpeta bin, "
                       "que no están en esta copia. Mientras falten, el juego no usa DLSS.",
                       "DLSS needs sacredfire_dlss.dll and nvngx_dlss.dll in the bin folder, "
                       "which this copy does not have. While they are missing, the game does "
                       "not use DLSS."));
        } else if (temporal == "fsr4" && !fsr4) {
            FormNote(T("FSR 4 necesita sus archivos, que no vienen con el juego: pon la carpeta "
                       "fsr4_shaders junto al launcher. Mientras falten, se usa FSR 3. También "
                       "necesita una gráfica con soporte INT8.",
                       "FSR 4 needs its files, which do not come with the game: put the "
                       "fsr4_shaders folder next to the launcher. While they are missing, FSR 3 "
                       "is used. It also needs a GPU with INT8 support."));
        }
    }
    if (temporal == "dlss") {
        static constexpr Choice kQuality[] = {
            {"Automática (resolución interna)", "Auto (internal resolution)", "auto"},
            {"DLAA (nativa)", "DLAA (native)", "dlaa"},
            {"Calidad", "Quality", "quality"},
            {"Equilibrado", "Balanced", "balanced"},
            {"Rendimiento", "Performance", "performance"},
            {"Ultra rendimiento", "Ultra Performance", "ultra_performance"}};
        if (GameChoiceCombo(T("Calidad de DLSS", "DLSS quality"), "dlss_quality", kQuality, "auto", 16.0f)) SaveGameIni();
        HelpMarker(T("Automática usa la resolución interna de arriba. Las demás dibujan la escena a la proporción de NVIDIA "
                     "respecto a la salida de DLSS: Calidad 67 %, Equilibrado 58 %, Rendimiento 50 %, Ultra rendimiento "
                     "33 %, DLAA 100 %.",
                     "Auto uses the internal resolution above. The others draw the scene at NVIDIA's share of the DLSS "
                     "output: Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%, DLAA 100%."));
        if (a.game.Get("dlss_quality", "auto") != "auto") {
            static constexpr Choice kOutput[] = {{"1920 x 1080", "1920 x 1080", "1080"},
                                                 {"3840 x 2160 (4K)", "3840 x 2160 (4K)", "2160"}};
            if (GameChoiceCombo(T("Salida de DLSS", "DLSS output"), "dlss_output", kOutput, "1080", 16.0f)) SaveGameIni();
        }
        static constexpr Choice kModel[] = {{"Transformer (recomendado)", "Transformer (recommended)", "transformer"},
                                            {"CNN", "CNN", "cnn"}};
        if (GameChoiceCombo(T("Modelo de DLSS", "DLSS model"), "dlss_model", kModel, "transformer", 16.0f)) SaveGameIni();
        HelpMarker(T("Transformer: el modelo actual de NVIDIA (DLSS 4), más nítido y estable, algo más pesado. CNN: el "
                     "modelo anterior (preset E), más ligero; NVIDIA lo marca como obsoleto.",
                     "Transformer: NVIDIA's current model (DLSS 4), sharper and steadier, a little heavier. CNN: the "
                     "older model (preset E), lighter; NVIDIA marks it deprecated."));
    }
    if (temporal == "fsr3" || temporal == "fsr4") {
        float sharp = float(std::atof(a.game.Get("fsr3_sharpness", "0.30").c_str()));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderFloat(FormLabel(T("Nitidez de FSR", "FSR sharpness")), &sharp, 0.0f, 1.0f, "%.2f")) {
            char text[16];
            std::snprintf(text, sizeof text, "%.2f", sharp);
            a.game.Set("fsr3_sharpness", text);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
    }
    GameFlag("msaa", T("MSAA del juego (2x)", "Game MSAA (2x)"), false,
             T("El antialiasing 2x de la PS4: bordes más limpios en la vegetación. En NVIDIA ya no dibuja "
               "líneas punteadas en el cielo; en otras gráficas puede que sí. Cuesta algo de "
               "rendimiento en gráficas modestas. Requiere reiniciar.",
               "The PS4's 2x anti-aliasing: cleaner foliage edges. On NVIDIA it no longer draws dotted "
               "lines in the sky; on other GPUs it may. Costs some frames on modest GPUs. "
               "Needs a restart."));
}

/// Settings > Display: the window, the screen shape, HDR and the counters drawn over the game.
void DisplaySettings() {
    App& a = g_app;
    FormSection(T("Ventana", "Window"));
    {
        std::scoped_lock lock{a.mutex};
        const std::string current = a.launcher.Get("display");
        std::string preview = T("Principal", "Primary");
        for (const auto& [value, label] : a.displays)
            if (value == current) preview = label;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo(FormLabel(T("Monitor", "Monitor")), preview.c_str())) {
            if (ImGui::Selectable(T("Principal", "Primary"), current.empty())) {
                a.launcher.Set("display", "");
                SaveLauncher();
            }
            for (const auto& [value, label] : a.displays) {
                if (ImGui::Selectable(label.c_str(), value == current)) {
                    a.launcher.Set("display", value);
                    SaveLauncher();
                }
            }
            ImGui::EndCombo();
        }
    }
    GameFlag("fullscreen", T("Pantalla completa", "Fullscreen"), false);
    GameFlag("borderless", T("Ventana sin bordes", "Borderless window"), false,
             T("Quita los bordes de la ventana en modo ventana.", "Removes window borders in windowed mode."));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ChoiceCombo(T("Presentación", "Presentation"), "present_mode", kPresentModes, "Mailbox");

    FormSection(T("Formato de pantalla", "Screen shape"));
    AspectRatioSettings();

    FormSection("HDR");
    LauncherFlag("hdr", T("HDR del juego", "The game's HDR"), false,
                 T("La salida HDR propia del juego (HDR10). Activa también el HDR de Windows y apaga el "
                   "RTX HDR para este juego. Experimental.",
                   "The game's own HDR output (HDR10). Also turn on Windows HDR and turn off RTX HDR for "
                   "this game. Experimental."));
    if (a.launcher.Flag("hdr", false)) {
        static constexpr Choice kHdrPeak[] = {{"Como el juego", "As the game", "0"},
                                              {"400 nits", "400 nits", "400"},
                                              {"450 nits", "450 nits", "450"},
                                              {"600 nits", "600 nits", "600"},
                                              {"800 nits", "800 nits", "800"},
                                              {"1000 nits", "1000 nits", "1000"}};
        ChoiceCombo(T("Brillo máximo HDR", "HDR peak brightness"), "hdr_peak", kHdrPeak, "0");
        HelpMarker(T("El brillo máximo de tu pantalla (el de la calibración HDR de Windows): las partes "
                     "más brillantes del juego se ajustan a él en vez de quemarse.",
                     "Your display's peak brightness (from the Windows HDR calibration): the game's "
                     "brightest parts are fitted to it instead of clipping."));
        static constexpr Choice kHdrGain[] = {{"60%", "60%", "0.6"},   {"70%", "70%", "0.7"},
                                              {"80%", "80%", "0.8"},   {"90%", "90%", "0.9"},
                                              {"100% (el juego)", "100% (the game)", "1"},
                                              {"115%", "115%", "1.15"}, {"130%", "130%", "1.3"}};
        ChoiceCombo(T("Blanco de referencia HDR", "HDR paper white"), "hdr_gain", kHdrGain, "1");
        HelpMarker(T("Cuánto brillan los blancos normales (menús, textos, cielo) frente a lo que el juego "
                     "pone. Bájalo si los menús se ven quemados; los brillos fuertes siguen limitados al "
                     "brillo máximo.",
                     "How bright ordinary whites (menus, text, sky) are against what the game puts out. "
                     "Lower it if menus look blown out; strong highlights stay within the peak brightness."));
    }

    FormSection(T("En pantalla", "On screen"));
    GameFlag("show_fps", T("Contador de FPS", "FPS counter"), false,
             T("Los FPS en la esquina de la pantalla.", "The FPS in the corner of the screen."));
    GameFlag("show_ping", T("Contador de ping", "Ping counter"), false,
             T("En una sala online, el ping de cada jugador en ms, debajo del contador de FPS. También en el menú "
               "del juego (Insert o L3+R3), pestaña Sala.",
               "In an online room, each player's ping in ms, under the FPS counter. Also in the in-game menu "
               "(Insert or L3+R3), Room tab."));
}

/// Settings > Game: sound, language and the start of the game.
void GameSettings() {
    App& a = g_app;
    FormSection(T("Sonido", "Sound"));
    {
        int volume = std::clamp(a.game.Int("volume", 100), 0, 200);
        if (ImGui::SliderInt(FormLabel(T("Volumen del juego", "Game volume")), &volume, 0, 200, "%d%%")) {
            a.game.Set("volume", std::to_string(volume));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
        HelpMarker(T("100% es el nivel al que se mezcló el juego. Por encima, los picos más fuertes se suavizan en vez "
                     "de distorsionar. Mientras juegas también se cambia en el menú del juego (Insert o L3+R3).",
                     "100% is the level the game was mixed at. Above it, the loudest peaks are rounded off instead of "
                     "distorting. While you play it can also be changed in the in-game menu (Insert or L3+R3)."));
    }

    FormSection(T("Idioma", "Language"));
    if (IsEuGame()) {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguagesEU, DefaultGameLanguage());
        HelpMarker(T("Los idiomas que trae la versión europea (CUSA14876): inglés, español de España, francés, alemán, "
                     "italiano y árabe. La lista cambia sola según la versión del juego que elijas. Se aplica al "
                     "pulsar Jugar.",
                     "The languages of the European edition (CUSA14876): English, Spain's Spanish, French, German, "
                     "Italian and Arabic. The list follows the edition of the game you choose. Applied when you "
                     "press Play."));
    } else {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguages, DefaultGameLanguage());
        HelpMarker(T("Los idiomas que trae la versión de América (CUSA13795): inglés, español latinoamericano, francés "
                     "y portugués de Brasil. La lista cambia sola según la versión del juego que elijas (la europea "
                     "trae español de España, alemán, italiano y árabe). Se aplica al pulsar Jugar.",
                     "The languages of the Americas edition (CUSA13795): English, Latin American Spanish, French and "
                     "Brazilian Portuguese. The list follows the edition of the game you choose (the European one has "
                     "Spain's Spanish, German, Italian and Arabic). Applied when you press Play."));
    }

    FormSection(T("Inicio", "Start"));
    GameFlag("skip_intro", T("Saltar los vídeos de inicio", "Skip intro videos"), false,
             T("Sin los logos de Activision y Beenox al arrancar el juego.",
               "Skips the Activision and Beenox logo videos when the game starts."));
}

/// Settings > Launcher: what the launcher does while the game runs, and the diagnostic.
void LauncherSettings() {
    FormSection(T("Al jugar", "While playing"));
    LauncherFlag("hide_on_play", T("Ocultar el launcher mientras juegas", "Hide the launcher while playing"), false,
                 T("Oculta completamente la ventana del launcher mientras juegas y la restaura automáticamente al salir.",
                   "Hides the launcher window completely while playing and restores it automatically when exiting."));
    LauncherFlag("close_on_play", T("Cerrar el launcher al salir del juego", "Close the launcher after playing"), false,
                 T("Cierra el launcher por completo una vez que sales del juego.",
                   "Closes the launcher completely once you exit the game."));

    FormSection(T("Privacidad", "Privacy"));
    GameFlag("perf_reports", T("Enviar datos anónimos de rendimiento", "Send anonymous performance data"), false,
             PerfReportsText());
}

/// One page of Settings: its own scrolling area under the page tabs, laid out as a form.
void SettingsPage(const char* title, void (*draw)()) {
    if (!ImGui::BeginTabItem(title)) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("settings_page", ImVec2(0, 0), ImGuiChildFlags_None);
    FormBegin();
    draw();
    FormEnd();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndTabItem();
}

void SettingsTab() {
    App& a = g_app;
    if (a.launcher.Get("language") != GameLanguage()) a.launcher.Set("language", GameLanguage());
    FormNote(T("Se aplican al pulsar Jugar. El menú del juego (Insert o L3+R3) cambia los mismos ajustes.",
               "Applied when you press Play. The in-game menu (Insert or L3+R3) changes the same settings."));
    ImGui::Spacing();
    if (!ImGui::BeginTabBar("settings_pages")) return;
    SettingsPage(T("Gráficos", "Graphics"), GraphicsSettings);
    SettingsPage(T("Pantalla", "Display"), DisplaySettings);
    SettingsPage(T("Juego", "Game"), GameSettings);
    SettingsPage("Launcher", LauncherSettings);
    ImGui::EndTabBar();
}

/// Linking the nickname to the player's Discord account, so that a reinstall or another PC
/// recovers it. The browser half is discord_link.cpp; the server trades the code for the
/// account (ryunet-demonware-ctr/server/discord_link.go).
struct DiscordLinkState {
    std::mutex mutex;
    bool busy = false;
    bool error = false;
    std::string message;     // the last outcome, under the buttons
    std::string other_nick;  // "other_nick": the nickname this Discord account already has
    std::string recovered;   // a recovered nickname for the UI thread to put in the field
};
DiscordLinkState g_discord_link;

/// Link the current nickname, unlink it, or recover the Discord account's own nickname (for a
/// reinstalled launcher that does not know it any more).
enum class DiscordLinkMode { Link, Unlink, Recover };

void StartDiscordLink(DiscordLinkMode mode) {
    const bool unlink = mode == DiscordLinkMode::Unlink;
    App& a = g_app;
    const std::string server = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
    const std::string nick = mode == DiscordLinkMode::Recover ? std::string() : CurrentNick();
    const std::string app_id = DiscordAppId();
    {
        std::scoped_lock lock{g_discord_link.mutex};
        if (g_discord_link.busy) return;
        g_discord_link.busy = true;
        g_discord_link.error = false;
        g_discord_link.message.clear();
        g_discord_link.other_nick.clear();
    }
    std::thread([server, nick, app_id, unlink] {
        std::string message, other, recovered;
        bool error = true;
        const auto form = [](const char* key, const std::string& value) {
            return std::string(key) + "=" + DiscordLink::FormEncode(value);
        };
        if (unlink) {
            const std::string body = HttpRequest(server, L"POST", L"/ryunet/discord/unlink", form("nick", nick), 10000);
            if (JsonBool(body, "unlinked")) {
                error = false;
                message = T("Apodo desvinculado de Discord.", "Nickname unlinked from Discord.");
            } else {
                message = T("No se pudo desvincular.", "Could not unlink.");
            }
        } else {
            const DiscordLink::Result auth = DiscordLink::Authorize(app_id, 180, g_spanish);
            if (!auth.ok) {
                message = auth.error == "denied"  ? T("Cancelaste la autorización en Discord.", "You cancelled the authorization in Discord.")
                          : auth.error == "busy"  ? T("Los puertos 53134 a 53136 están ocupados: cierra otros launchers abiertos.",
                                                      "Ports 53134 to 53136 are in use: close other open launchers.")
                          : auth.error == "timeout" ? T("La autorización de Discord no llegó a tiempo (3 minutos).",
                                                        "Discord's authorization did not arrive in time (3 minutes).")
                                                    : T("No se pudo autorizar con Discord.", "Could not authorize with Discord.");
            } else {
                const std::string body =
                    HttpRequest(server, L"POST", L"/ryunet/discord/link",
                                form("nick", nick) + "&" + form("code", auth.code) + "&" + form("redirect_uri", auth.redirect_uri), 25000,
                                true);
                const std::string status = JsonField(body, "status"), account = JsonField(body, "discord");
                if (status == "linked") {
                    error = false;
                    message = Format(T("Listo: %s queda vinculado a tu cuenta de Discord %s. Si reinstalas o cambias de PC, "
                                       "pulsa Recuperar con Discord.",
                                       "Done: %s is linked to your Discord account %s. If you reinstall or change PC, "
                                       "press Recover with Discord."),
                                     nick, account);
                } else if (status == "recovered") {
                    error = false;
                    recovered = nick.empty() ? JsonField(body, "nick") : nick;
                    message = Format(T("Recuperado: %s vuelve a ser tuyo en esta instalación (Discord: %s).",
                                       "Recovered: %s is yours again on this installation (Discord: %s)."),
                                     recovered, account);
                } else if (status == "own_nick") {
                    error = false;
                    recovered = JsonField(body, "nick");
                    message = Format(T("Esta instalación ya tenía el apodo %s, sin vincular a Discord. Ya está puesto: pulsa "
                                       "Vincular con Discord para poder recuperarlo en cualquier PC.",
                                       "This installation already had the nickname %s, not linked to Discord. It is filled "
                                       "in: press Link with Discord to recover it on any PC."),
                                     recovered);
                } else if (status == "no_nick") {
                    message = Format(T("Tu cuenta de Discord %s no tiene ningún apodo vinculado en este servidor: escribe "
                                       "uno y pulsa Vincular con Discord.",
                                       "Your Discord account %s has no nickname linked on this server: type one and "
                                       "press Link with Discord."),
                                     account);
                } else if (status == "other_nick") {
                    other = JsonField(body, "nick");
                    message = Format(T("Tu cuenta de Discord ya tiene el apodo %s: cada cuenta tiene uno.",
                                       "Your Discord account already has the nickname %s: one per account."),
                                     other);
                } else if (status == "taken") {
                    message = T("Ese apodo es de otra persona o de otra cuenta de Discord.",
                                "That nickname belongs to someone else or to another Discord account.");
                } else if (status == "invalid" || status == "reserved") {
                    message = T("El servidor no aceptó la solicitud.", "The server did not accept the request.");
                } else if (body.empty()) {
                    message = T("No se pudo contactar con el servidor.", "Could not reach the server.");
                } else {
                    message = T("El servidor no pudo confirmar la cuenta con Discord. Prueba otra vez.",
                                "The server could not confirm the account with Discord. Try again.");
                }
            }
        }
        {
            std::scoped_lock lock{g_discord_link.mutex};
            g_discord_link.busy = false;
            g_discord_link.error = error;
            g_discord_link.message = message;
            g_discord_link.other_nick = other;
            g_discord_link.recovered = recovered;
        }
        std::scoped_lock lock{g_nick_check.mutex};  // ask the server for the new state
        g_nick_check.results.clear();
    }).detach();
}

/// The running authorization or the last outcome, with "Use <nickname>" when Discord named one.
void DiscordLinkStatusLines() {
    App& a = g_app;
    bool busy, error;
    std::string message, other;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        busy = g_discord_link.busy;
        error = g_discord_link.error;
        message = g_discord_link.message;
        other = g_discord_link.other_nick;
    }
    if (busy) {
        ImGui::TextColored(kGrey, "%s", T("Autoriza tu cuenta en la pestaña de Discord que se abrió en el navegador...",
                                          "Authorize your account in the Discord tab that opened in the browser..."));
        return;
    }
    if (message.empty()) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(error ? kYellow : kGreen, "%s", message.c_str());
    ImGui::PopTextWrapPos();
    if (!other.empty() && ImGui::SmallButton(Format(T("Usar %s", "Use %s"), other).c_str())) {
        std::snprintf(a.online_name_buf, sizeof a.online_name_buf, "%s", other.c_str());
        a.game.Set("online_name", a.online_name_buf);
        a.game.Set("user_name", a.online_name_buf);
        SaveGameIni();
        std::scoped_lock lock{g_discord_link.mutex};
        g_discord_link.message.clear();
        g_discord_link.other_nick.clear();
    }
}

/// With no usable nickname yet (a reinstall, another PC): recover the one linked to Discord.
void DiscordRecoverOffer() {
    if (!DiscordLinkAvailable()) return;
    bool busy;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        busy = g_discord_link.busy;
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(T("Recuperar mi apodo con Discord", "Recover my nickname with Discord")))
        StartDiscordLink(DiscordLinkMode::Recover);
    ImGui::EndDisabled();
    HelpMarker(T("¿Reinstalaste el juego o cambiaste de PC y tu apodo estaba vinculado a Discord? Autoriza la misma "
                 "cuenta de Discord y el launcher pondrá tu apodo, aunque no lo recuerdes.",
                 "Did you reinstall the game or change PC, and your nickname was linked to Discord? Authorize the "
                 "same Discord account and the launcher fills in your nickname, even if you do not remember it."));
    DiscordLinkStatusLines();
}

/// The recovered nickname, put in the field by the UI thread (PollJobs).
void ApplyRecoveredNick() {
    App& a = g_app;
    std::string nick;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        nick.swap(g_discord_link.recovered);
    }
    if (nick.empty() || NickProblem(nick)) return;
    std::snprintf(a.online_name_buf, sizeof a.online_name_buf, "%s", nick.c_str());
    a.game.Set("online_name", a.online_name_buf);
    a.game.Set("user_name", a.online_name_buf);
    SaveGameIni();
}

// Tacho: tu foto de Discord junto a tu apodo. El servidor la manda en PNG (GET /ryunet/avatar,
// ryunet_avatar.go; la de Discord por defecto si no tienes); se pide una vez por servidor y apodo.
LauncherTexture* PngTexture(const std::string& png);
struct OwnAvatarState {
    std::mutex mutex;
    std::string id;   // server|nick it is for
    std::string png;  // the answer, while the texture is not made
    bool ready = false;
    LauncherTexture* texture = nullptr;
};
OwnAvatarState g_own_avatar;

LauncherTexture* OwnAvatar() {
    const std::string nick = CurrentNick();
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    if (NickProblem(nick) || server.empty()) return nullptr;
    const std::string id = server + "|" + nick;
    std::scoped_lock lock{g_own_avatar.mutex};
    if (g_own_avatar.id != id) {
        g_own_avatar.id = id;
        g_own_avatar.png.clear();
        g_own_avatar.ready = false;
        ReleaseTexture(g_own_avatar.texture);
        g_own_avatar.texture = nullptr;
        std::thread([server, nick, id] {
            const std::wstring path = L"/ryunet/avatar?name=" + Widen(nick) + L"&who=" + Widen(nick);  // [A-Za-z0-9_-]
            std::string png = HttpRequest(server, L"GET", path, {}, 8000, false, {}, 256 << 10);
            if (png.size() < 8 || png.compare(0, 4, "\x89PNG") != 0) png.clear();
            std::scoped_lock lock{g_own_avatar.mutex};
            if (g_own_avatar.id != id) return;
            g_own_avatar.png = std::move(png);
            g_own_avatar.ready = true;
        }).detach();
    }
    if (g_own_avatar.ready && !g_own_avatar.texture && !g_own_avatar.png.empty()) {
        g_own_avatar.texture = PngTexture(g_own_avatar.png);
        g_own_avatar.png.clear();
    }
    return g_own_avatar.texture;
}

/// The picture in a circle, `size` pixels, at the cursor; then SameLine. Nothing when there is none.
void OwnAvatarImage(float size) {
    LauncherTexture* texture = OwnAvatar();
    if (!texture) return;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImGui::GetWindowDrawList()->AddImageRounded(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(texture))), p,
                                                ImVec2(p.x + size, p.y + size), ImVec2(0, 0), ImVec2(1, 1),
                                                IM_COL32_WHITE, size * 0.5f);
    ImGui::SameLine();
}

/// Under the nickname: its Discord and Steam accounts, and Link / Recover / Unlink.
void DiscordAccountSection() {
    if (NickProblem(CurrentNick())) {
        DiscordRecoverOffer();
        return;
    }
    const NickCheck::Result info = NickInfo();
    bool busy;
    std::string message;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        busy = g_discord_link.busy;
        message = g_discord_link.message;
    }
    if (!info.link_available && message.empty()) return;  // unknown yet, or this server cannot link
    ImGui::BeginDisabled(busy);
    if (info.status == "yours" && info.linked) {
        ImGui::TextColored(kGreen, "%s", Format(T("Cuenta de Discord: %s", "Discord account: %s"), info.discord).c_str());
        ImGui::SameLine();
        // Unlinking frees the nickname field and stops Discord recovery: a second press confirms.
        static uint64_t unlink_armed = 0;
        const bool armed = unlink_armed && Ticks() - unlink_armed < 5000;
        if (ImGui::SmallButton(armed ? T("Pulsa otra vez para desvincular", "Press again to unlink") : T("Desvincular", "Unlink"))) {
            if (armed) {
                unlink_armed = 0;
                StartDiscordLink(DiscordLinkMode::Unlink);
            } else {
                unlink_armed = Ticks();
            }
        }
        if (!armed && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Deja de estar vinculado: podrás cambiar el apodo, pero ya no lo recuperarás con Discord "
                                      "si reinstalas.",
                                      "Unlinks it: you can change the nickname, but can no longer recover it with Discord "
                                      "if you reinstall."));
    } else if (info.status == "yours" || info.status == "free") {
        if (ImGui::Button(T("Vincular con Discord", "Link with Discord"))) StartDiscordLink(DiscordLinkMode::Link);
        if (info.status == "free") {
            // A reinstall shows a free nickname the player just typed; theirs may be linked.
            ImGui::SameLine();
            if (ImGui::SmallButton(T("¿Ya tenías un apodo vinculado? Recupéralo", "Had a linked nickname already? Recover it")))
                StartDiscordLink(DiscordLinkMode::Recover);
        }
        HelpMarker(T("Tu apodo queda ligado a tu cuenta de Discord: si reinstalas el juego o juegas en otro PC, lo "
                     "recuperas autorizando Discord otra vez. Se abre Discord en el navegador para que lo autorices; "
                     "el launcher solo recibe tu identificador de Discord, nunca tu contraseña.",
                     "Your nickname is tied to your Discord account: if you reinstall the game or play on another PC, "
                     "you recover it by authorizing Discord again. Discord opens in the browser for you to authorize; "
                     "the launcher only receives your Discord ID, never your password."));
    } else if (info.status == "taken" && info.linked) {
        if (ImGui::Button(T("Recuperar con Discord", "Recover with Discord"))) StartDiscordLink(DiscordLinkMode::Link);
        HelpMarker(T("Si este apodo es tuyo, autoriza la misma cuenta de Discord con la que lo vinculaste y "
                     "podrás usarlo en esta instalación.",
                     "If this nickname is yours, authorize the same Discord account you linked it with and you can "
                     "use it on this installation."));
    } else if (info.status == "taken") {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kGrey, "%s",
                           T("Si es tuyo y lo perdiste al reinstalar, pide al administrador del servidor que lo libere, "
                             "o espera: un apodo sin vincular y sin uso se libera a los 60 días.",
                             "If it is yours and you lost it when reinstalling, ask the server's administrator to "
                             "release it, or wait: an unlinked nickname nobody uses is released after 60 days."));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndDisabled();
    DiscordLinkStatusLines();
}

/// The EOS bridge's setting (settings) or what it is doing (!settings) (EosBridge).
void EosBridgeSection(bool settings) {
    App& a = g_app;
    std::string state, detail;
    bool peers;
    {
        std::scoped_lock lock{g_eos.mutex};
        state = g_eos.state;
        detail = g_eos.detail;
        peers = !g_eos.peer_port.empty();
    }
    if (settings) {
        const char* labels[] = {T("Automática", "Automatic"), T("Siempre", "Always"), T("Nunca", "Never")};
        const char* values[] = {"auto", "on", "off"};
        const std::string mode = EosBridgeMode();
        int current = mode == "on" ? 1 : mode == "off" ? 2 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::Combo(FormLabel(T("Conexión por Epic Online Services", "Connection through Epic Online Services")), &current, labels, 3)) {
            a.launcher.Set("eos_bridge", values[current]);
            SaveLauncher();
        }
        HelpMarker(T("Si tu red no llega al servidor (algunos proveedores de internet lo bloquean), el juego se conecta a "
                     "través de los servidores de Epic Games, sin cuenta de Epic y sin abrir puertos. Con NAT estricta, "
                     "las carreras también van por Epic. Automática: solo cuando hace falta. Siempre: también cuando la "
                     "conexión directa funciona (para probar).",
                     "If your network does not reach the server (some internet providers block it), the game connects "
                     "through Epic Games' servers, with no Epic account and no ports to open. With strict NAT, races go "
                     "through Epic too. Automatic: only when needed. Always: even when the direct connection works (for "
                     "testing)."));
        return;
    }
    if (state == "starting" || state == "checking")
        ImGui::TextColored(kGrey, "%s", T("Comprobando la conexión con el servidor...", "Checking the connection to the server..."));
    else if (state == "direct")
        ImGui::TextColored(kGreen, "%s", peers ? T("Conexión directa con el servidor. Las carreras con quien no tenga camino directo "
                                                   "(NAT estricta) van por Epic Online Services.",
                                                   "Direct connection to the server. Races with players there is no direct path to "
                                                   "(strict NAT) go through Epic Online Services.")
                                               : T("Conexión directa con el servidor.", "Direct connection to the server."));
    else if (state == "connecting")
        ImGui::TextColored(kGrey, "%s", T("Tu red no llega al servidor: conectando por Epic Online Services...",
                                          "Your network does not reach the server: connecting through Epic Online Services..."));
    else if (state == "ready")
        ImGui::TextColored(kGreen, "%s", T("Conectado por Epic Online Services: tu red no llega al servidor directamente.",
                                           "Connected through Epic Online Services: your network does not reach the server directly."));
    else if (state == "lost")
        ImGui::TextColored(kYellow, "%s", T("Se perdió la conexión con Epic Online Services; reconectando...",
                                            "The connection through Epic Online Services was lost; reconnecting..."));
    else if (state == "failed")
        ImGui::TextColored(kRed, T("No se pudo conectar por Epic Online Services (%s).", "Could not connect through Epic Online Services (%s)."),
                           detail.empty() ? "?" : detail.c_str());
    else if (state == "missing")
        ImGui::TextColored(kGrey, "%s", T("La conexión por Epic no está en esta instalación (falta la carpeta eos).",
                                          "The connection through Epic is not in this installation (the eos folder is missing)."));
}


void NatTypeSection(const std::string& server) {
    bool running, done;
    NatCheck::Result result;
    {
        std::scoped_lock lock{g_nat.mutex};
        running = g_nat.running;
        done = g_nat.done;
        result = g_nat.result;
    }
    ImGui::BeginDisabled(running);
    if (ImGui::Button(T("Comprobar tipo de NAT", "Check NAT type"))) StartNatCheck(server);
    ImGui::EndDisabled();
    HelpMarker(T("Comprueba si otros jugadores pueden conectarse directamente a tu PC. Pregunta a tu servidor RyuNET "
                 "y a un servidor STUN público (Cloudflare) con qué dirección te ven y las compara aquí: tu IP no se "
                 "muestra ni se guarda.",
                 "Checks whether other players can connect to your PC directly. It asks your RyuNET server and a "
                 "public STUN server (Cloudflare) which address they see you from and compares them here: your IP "
                 "is neither shown nor saved."));
    ImGui::SameLine();
    if (running) {
        ImGui::TextColored(kGrey, "%s", T("Comprobando...", "Checking..."));
        return;
    }
    if (!done) return;
    switch (result.type) {
    case NatCheck::Type::Open:
        ImGui::TextColored(kGreen, "%s", T("NAT tipo 1 (abierto): sin router de por medio.",
                                           "NAT type 1 (open): no router in between."));
        break;
    case NatCheck::Type::Moderate:
        ImGui::TextColored(kGreen, "%s", T("NAT tipo 2 (moderado): los demás jugadores pueden conectarse contigo.",
                                           "NAT type 2 (moderate): other players can connect to you."));
        break;
    case NatCheck::Type::Strict:
        ImGui::TextColored(kYellow, "%s", T("NAT tipo 3 (estricto): tu router cambia de puerto según con quién hables.",
                                            "NAT type 3 (strict): your router changes ports for each destination."));
        ImGui::TextWrapped("%s", T("Con otros jugadores de NAT estricto la conexión directa suele fallar y el juego pasa "
                                   "por el túnel del servidor. Ayuda activar UPnP en el router o abrir el puerto UDP 3074 "
                                   "hacia este PC.",
                                   "Direct connections with other strict-NAT players often fail and the game goes "
                                   "through the server's tunnel. Turning on UPnP in the router or forwarding UDP port "
                                   "3074 to this PC helps."));
        break;
    case NatCheck::Type::Unknown:
        ImGui::TextColored(result.error == "server" ? kRed : kGrey, "%s",
                           result.error == "server" ? T("El servidor no respondió (¿está bien escrito? ¿UDP 3074 bloqueado?).",
                                                        "The server did not answer (is it spelled right? UDP 3074 blocked?).")
                           : result.error == "dns" ? T("No se encontró el servidor.", "The server was not found.")
                           : result.error == "stun" ? T("No se pudo comparar (el servidor STUN no respondió).",
                                                        "Could not compare (the STUN server did not answer).")
                                                    : T("No se pudo comprobar.", "Could not check."));
        break;
    }
    if (result.replies_blocked || result.error == "server") {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
#ifdef _WIN32
        ImGui::TextWrapped("%s", T("Tu red bloquea las respuestas del servidor (UDP 3074): el juego dirá \"Failed to initialize "
                                   "network\" al entrar en Online. Permite el launcher y sacredfire.exe en el Firewall de "
                                   "Windows (redes privadas y públicas), revisa la protección de red del antivirus y las VPN, "
                                   "reinicia el router o prueba otra red (los datos del móvil). No hace falta abrir puertos "
                                   "en el router.",
                                   "Your network blocks the server's replies (UDP 3074): the game will say \"Failed to "
                                   "initialize network\" when you enter Online. Allow the launcher and sacredfire.exe in "
                                   "Windows Firewall (private and public networks), check your antivirus' network protection "
                                   "and any VPN, restart your router or try another network (a phone hotspot). You don't need "
                                   "to forward ports on your router."));
#else
        ImGui::TextWrapped("%s", T("Tu red bloquea las respuestas UDP del servidor (puerto 3074). Revisa el firewall y las VPN, "
                                   "reinicia el router o prueba otra red. No hace falta redirigir puertos en el router.",
                                   "Your network blocks the server's UDP replies (port 3074). Check your firewall and VPN, "
                                   "restart the router or try another network. Router port forwarding is not required."));
#endif
        ImGui::PopStyleColor();
    }
}

// ---- The account (the server's account_device.go; the site's /cuenta) ----
// "Log in / Create account" opens the site with a code: the player logs in there with Discord (or
// creates the account: nickname and password) and authorizes this PC; the launcher asks the server
// every few seconds until the answer names the nickname. The installation's key never leaves the
// launcher: the code names a request only this installation can collect. "Log in with nickname and
// password" sends them to the server by HTTPS only, and the server adds this installation's key
// to the account the same way.
struct AccountLoginState {
    std::mutex mutex;
    bool busy = false, error = false, cancel = false;
    bool copy_url = false;  // no browser opened: the UI thread copies url to the clipboard
    std::string code, message, url;
    std::string nick;  // the account's nickname, for the UI thread to put in place (ApplyAccountNick)
};
AccountLoginState g_account;

void AccountResult(bool error, const std::string& message, const std::string& nick = {}) {
    std::scoped_lock lock{g_account.mutex};
    g_account.busy = false;
    g_account.error = error;
    g_account.message = message;
    g_account.code.clear();
    if (!nick.empty()) g_account.nick = nick;
}

void StartWebLogin() {
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    {
        std::scoped_lock lock{g_account.mutex};
        if (g_account.busy) return;
        g_account.busy = true;
        g_account.cancel = false;
        g_account.error = false;
        g_account.message.clear();
        g_account.code.clear();
    }
    std::thread([server] {
        const std::string started = HttpRequest(server, L"POST", L"/ryunet/device/start", {}, 10000, true);
        const std::string code = JsonField(started, "code"), url = JsonField(started, "url");
        if (code.empty() || url.rfind("http", 0) != 0) {
            const std::string why = JsonField(started, "error");
            AccountResult(true, started.empty() ? T("No se pudo contactar con el servidor.", "Could not reach the server.")
                                : why.rfind("banned", 0) == 0 ? T("Este PC no puede iniciar sesión en este servidor.",
                                                                 "This PC cannot log in on this server.")
                                                              : T("Este servidor todavía no tiene cuentas: usa el apodo a mano (abajo).",
                                                                  "This server has no accounts yet: type the nickname by hand (below)."));
            return;
        }
        // ShellExecute answers more than 32 when something opened the address.
        const bool opened = OpenUrl(url);
        {
            std::scoped_lock lock{g_account.mutex};
            g_account.code = code;
            g_account.url = url;
            g_account.copy_url = !opened;
        }
        const std::wstring poll = L"/ryunet/device/poll?code=" + Widen(code);
        for (int i = 0; i < 200; ++i) {  // 10 minutes, as long as the code lives
            for (int t = 0; t < 30; ++t) {
                SleepMs(100);
                std::scoped_lock lock{g_account.mutex};
                if (g_account.cancel) {
                    g_account.busy = false;
                    g_account.code.clear();
                    return;
                }
            }
            const std::string answer = HttpRequest(server, L"POST", poll, {}, 10000, true);
            const std::string status = JsonField(answer, "status");
            if (status == "approved") {
                const std::string nick = JsonField(answer, "nick");
                AccountResult(false, Format(T("Listo: este PC juega como %s.", "Done: this PC plays as %s."), nick), nick);
                std::scoped_lock lock{g_nick_check.mutex};
                g_nick_check.results.clear();
                return;
            }
            if (status == "expired") break;
        }
        AccountResult(true, T("El código venció: pulsa Iniciar sesión otra vez.", "The code expired: press Log in again."));
    }).detach();
}

/// POST of a JSON body to the server's HTTPS (443, its nginx), signed with this installation's key.
/// bridge_port: the EOS bridge's local port for the server's HTTP port instead (EosBridgeRoute),
/// plain HTTP to 127.0.0.1: EOS P2P is encrypted up to the gateway on the server's machine.
std::string HttpsPostJson(const std::string& host, const std::wstring& path, const std::string& json,
                          uint16_t bridge_port = 0);

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out;
}

void StartPasswordLogin(const std::string& nick, const std::string& password) {
    std::string host;
    uint16_t port = 0;
    const std::string server = Trim(g_app.game.Get("online_server", "octr.ryu7w7.xyz"));
    SplitServer(server, host, port);
    {
        std::scoped_lock lock{g_account.mutex};
        if (g_account.busy) return;
        g_account.busy = true;
        g_account.error = false;
        g_account.message.clear();
    }
    std::thread([server, host, port, nick, password] {
        // A network that does not reach the server: the EOS bridge carries the login too.
        uint16_t bridge_port = 0;
        if (!EosBridgeRoute(server, port, bridge_port, 15000)) bridge_port = 0;
        if (!bridge_port && host.find_first_not_of("0123456789.") == std::string::npos) {
            AccountResult(true, T("La contraseña solo viaja por HTTPS, que necesita el nombre del servidor (no su IP): "
                                  "usa Iniciar sesión / Crear cuenta.",
                                  "The password only travels by HTTPS, which needs the server's name (not its IP): use "
                                  "Log in / Create account."));
            return;
        }
        const std::string body = HttpsPostJson(host, L"/ryunet/account/login",
                                               "{\"nick\":\"" + JsonEscape(nick) + "\",\"password\":\"" + JsonEscape(password) + "\"}",
                                               bridge_port);
        const std::string status = JsonField(body, "status"), error = JsonField(body, "error");
        if (status == "ok") {
            const std::string account = JsonField(body, "nick");
            AccountResult(false, Format(T("Listo: este PC juega como %s.", "Done: this PC plays as %s."), account), account);
            std::scoped_lock lock{g_nick_check.mutex};
            g_nick_check.results.clear();
            return;
        }
        AccountResult(true, error == "wrong"      ? T("Apodo o contraseña incorrectos.", "Wrong nickname or password.")
                            : error == "too_many" ? T("Demasiados intentos: espera 15 minutos.", "Too many attempts: wait 15 minutes.")
                            : error.rfind("banned", 0) == 0 || error == "banned"
                                ? T("Este PC no puede iniciar sesión en este servidor.", "This PC cannot log in on this server.")
                            : body.empty() ? T("No se pudo contactar con el servidor por HTTPS.", "Could not reach the server by HTTPS.")
                                           : T("El servidor no aceptó el inicio de sesión.", "The server did not accept the login."));
    }).detach();
}

/// The account's nickname, put in place by the UI thread (PollJobs).
void ApplyAccountNick() {
    App& a = g_app;
    std::string nick;
    {
        std::scoped_lock lock{g_account.mutex};
        nick.swap(g_account.nick);
    }
    if (nick.empty() || NickProblem(nick)) return;
    std::snprintf(a.online_name_buf, sizeof a.online_name_buf, "%s", nick.c_str());
    a.game.Set("online_name", a.online_name_buf);
    a.game.Set("user_name", a.online_name_buf);
    SaveGameIni();
}

/// The Player section's top: who this PC plays as, or Log in / Create account.
/// Logged in: this PC plays a nickname of its own that is an account (linked to Discord, as the
/// site's accounts are). A nickname only typed here is not one.
bool SignedIn() {
    const NickCheck::Result info = NickInfo();
    return !NickProblem(CurrentNick()) && info.status == "yours" && info.linked;
}

void AccountSection() {
    App& a = g_app;
    const NickCheck::Result info = NickInfo();
    bool busy, error;
    std::string message, code;
    {
        std::scoped_lock lock{g_account.mutex};
        busy = g_account.busy;
        error = g_account.error;
        message = g_account.message;
        code = g_account.code;
    }
    bool discord_busy;
    {
        std::scoped_lock lock{g_discord_link.mutex};
        discord_busy = g_discord_link.busy;
    }
    const std::string nick = CurrentNick();
    if (SignedIn()) {
        ImGui::TextColored(kGreen, "%s", Format(T("Sesión iniciada como %s", "Logged in as %s"), nick).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Mi cuenta en la web", "My account on the web"))) {
            std::string host;
            uint16_t port = 0;
            SplitServer(Trim(a.game.Get("online_server", "octr.ryu7w7.xyz")), host, port);
            // The test servers (an address) serve the site on :8094 over HTTP; a named one by HTTPS.
            const bool address = host.find_first_not_of("0123456789.") == std::string::npos;
            OpenInShell(fs::path(Widen(address ? "http://" + host + ":8094/cuenta/" : "https://" + host + "/cuenta/")));
        }
        ImGui::SameLine();
        static uint64_t out_armed = 0;
        const bool armed = out_armed && Ticks() - out_armed < 5000;
        if (ImGui::SmallButton(armed ? T("Pulsa otra vez para cerrar sesión", "Press again to log out") : T("Cerrar sesión", "Log out"))) {
            if (armed) {
                out_armed = 0;
                a.online_name_buf[0] = '\0';
                a.game.Set("online_name", "");
                SaveGameIni();
            } else {
                out_armed = Ticks();
            }
        }
        if (!armed && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Este PC deja de jugar con tu cuenta hasta que vuelvas a iniciar sesión. Tu cuenta, "
                                      "monedas y logros siguen en el servidor.",
                                      "This PC stops playing with your account until you log in again. Your account, "
                                      "coins and trophies stay on the server."));
    } else {
        // Where the player stands, in one line.
        ImGui::PushTextWrapPos(0.0f);
        if (info.account_required)
            ImGui::TextColored(kYellow, "%s", T("Para jugar online en este servidor tienes que iniciar sesión.",
                                                "You need to log in to play online on this server."));
        else if (!NickProblem(nick) && info.status == "yours")
            ImGui::TextColored(kGrey, "%s", Format(T("Juegas como %s sin cuenta: inicia sesión para no perder tu apodo.",
                                                     "You play as %s without an account: log in to keep your nickname."),
                                                   nick).c_str());
        else
            ImGui::TextColored(kGrey, "%s", T("Inicia sesión para jugar online con tu cuenta.", "Log in to play online with your account."));
        ImGui::PopTextWrapPos();

        // The ways in, side by side.
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(T("Iniciar sesión / Crear cuenta", "Log in / Create account"))) StartWebLogin();
        ImGui::EndDisabled();
        HelpMarker(T("Abre la web del servidor: entras con Discord (o creas tu cuenta) y autorizas este PC. Tu apodo, "
                     "monedas, amigos, logros y partidas guardadas te siguen a cualquier PC.",
                     "Opens the server's site: you log in with Discord (or create your account) and authorize this PC. "
                     "Your nickname, coins, friends, trophies and saves follow you to any PC."));
        if (DiscordLinkAvailable()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(discord_busy);
            if (ImGui::Button(T("Entrar con Discord", "Log in with Discord"))) StartDiscordLink(DiscordLinkMode::Recover);
            ImGui::EndDisabled();
            HelpMarker(T("Si tu cuenta ya está vinculada a Discord: lo autorizas en el navegador y este PC juega con ella, "
                         "sin escribir nada (también tras reinstalar o en otro PC).",
                         "If your account is already linked to Discord: authorize it in the browser and this PC plays "
                         "with it, nothing to type (also after reinstalling or on another PC)."));
        }
        DiscordLinkStatusLines();

        if (busy && !code.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Cancelar", "Cancel"))) {
                std::scoped_lock lock{g_account.mutex};
                g_account.cancel = true;
            }
            std::string url;
            bool copy = false;
            {
                std::scoped_lock lock{g_account.mutex};
                url = g_account.url;
                copy = g_account.copy_url;
                g_account.copy_url = false;
            }
            static bool copied = false;
            if (copy && !url.empty()) {
                ImGui::SetClipboardText(url.c_str());  // no browser: the link is ready to paste
                copied = true;
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kGrey, "%s", Format(copied ? T("No se pudo abrir el navegador: el enlace se copió. Pégalo en tu navegador, "
                                                              "entra y autoriza este PC (código %s)...",
                                                              "The browser did not open: the link was copied. Paste it in your browser, "
                                                              "log in and authorize this PC (code %s)...")
                                                          : T("Se abrió la web: entra y autoriza este PC (código %s)...",
                                                              "The web opened: log in and authorize this PC (code %s)..."),
                                                   code).c_str());
            ImGui::PopTextWrapPos();
            if (!url.empty()) {
                ImGui::TextColored(kGrey, "%s", url.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(T("Copiar enlace", "Copy link"))) ImGui::SetClipboardText(url.c_str());
            }
            bool through_epic = false;
            {
                std::scoped_lock lock{g_eos.mutex};
                through_epic = g_eos.state == "ready" || g_eos.state == "lost";
            }
            if (through_epic) {  // the site is on the server this network does not reach
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(kYellow, "%s", T("Tu red no llega al servidor, así que la web puede no abrir: abre el enlace en el "
                                                    "móvil (con datos móviles), o usa Entrar con Discord o el apodo y contraseña, "
                                                    "que pasan por Epic Online Services.",
                                                    "Your network does not reach the server, so the site may not open: open the link on "
                                                    "your phone (on mobile data), or use Log in with Discord or the nickname and "
                                                    "password, which go through Epic Online Services."));
                ImGui::PopTextWrapPos();
            }
        }

        static char login_nick[24] = "", login_password[132] = "";
        if (ImGui::TreeNode(T("Entrar con apodo y contraseña", "Log in with nickname and password"))) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            ImGui::InputText(T("Apodo##login", "Nickname##login"), login_nick, sizeof login_nick);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            ImGui::InputText(T("Contraseña##login", "Password##login"), login_password, sizeof login_password,
                             ImGuiInputTextFlags_Password);
            ImGui::BeginDisabled(busy || !login_nick[0] || !login_password[0]);
            if (ImGui::Button(T("Entrar", "Log in"))) {
                StartPasswordLogin(Trim(login_nick), login_password);
                std::memset(login_password, 0, sizeof login_password);
            }
            ImGui::EndDisabled();
            HelpMarker(T("La contraseña de tu cuenta (la pones en la web, en Mi cuenta).",
                         "Your account's password (you set it on the site, in My account)."));
            ImGui::TreePop();
        }
    }
    if (!message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(error ? kYellow : kGreen, "%s", message.c_str());
        ImGui::PopTextWrapPos();
    }
}

/// The player's picture in a circle, `size` pixels: the Discord one (OwnAvatar), or the initial of
/// the nickname on a coloured disc while there is none.
void ProfileAvatar(float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 c{p.x + size * 0.5f, p.y + size * 0.5f};
    if (LauncherTexture* texture = OwnAvatar()) {
        d->AddImageRounded(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(texture))), p,
                           ImVec2(p.x + size, p.y + size), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, size * 0.5f);
    } else {
        d->AddCircleFilled(c, size * 0.5f, IM_COL32(18, 104, 168, 255));
        const std::string nick = CurrentNick();
        char initial[2] = {nick.empty() ? '?' : char(std::toupper(static_cast<unsigned char>(nick[0]))), 0};
        ImFont* font = ImGui::GetFont();
        const float font_size = size * 0.48f;
        const ImVec2 t = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, initial);
        d->AddText(font, font_size, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), IM_COL32(235, 248, 255, 255), initial);
    }
    d->AddCircle(c, size * 0.5f, IM_COL32(97, 232, 255, 200), 0, 2.0f);
}

/// The Online page's head: picture, nickname, account and server, always in sight.
void OnlineProfileCard() {
    App& a = g_app;
    const bool online = a.game.Flag("online", true);
    const std::string nick = CurrentNick();
    const bool has_nick = !NickProblem(nick);
    BeginCard("online_profile");
    ProfileAvatar(ImGui::GetFrameHeight() * 2.6f);
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.9f);
    ImGui::BeginGroup();
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextUnformatted(has_nick ? nick.c_str() : T("Sin apodo", "No nickname"));
    if (g_font_heading) ImGui::PopFont();
    if (!online) {
        ImGui::TextColored(kGrey, "%s", T("Modo online desactivado (pestaña Servidor).", "Online mode is off (Server tab)."));
    } else if (SignedIn()) {
        const NickCheck::Result info = NickInfo();
        ImGui::TextColored(kGreen, "%s", info.discord.empty()
                                             ? T("Sesión iniciada", "Logged in")
                                             : Format(T("Sesión iniciada · Discord: %s", "Logged in · Discord: %s"), info.discord).c_str());
    } else if (has_nick) {
        ImGui::TextColored(kYellow, "%s", T("Sin cuenta: inicia sesión para no perder tu apodo.",
                                            "No account: log in to keep your nickname."));
    } else {
        ImGui::TextColored(kYellow, "%s", T("Inicia sesión o elige un apodo en Cuenta.", "Log in or pick a nickname in Account."));
    }
    ImGui::TextColored(kGrey, "%s", Format(T("Servidor: %s", "Server: %s"), Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"))).c_str());
    if (online) EosBridgeSection(false);  // the connection, one line
    ImGui::EndGroup();
    EndCard();
}

/// Online > Account: logging in, the nickname and its Discord account.
void OnlineAccountPage() {
    App& a = g_app;
    const bool online = a.game.Flag("online", true);
    if (!online) FormNote(T("El modo online está desactivado: actívalo en la pestaña Servidor.",
                            "Online mode is off: turn it on in the Server tab."));
    ImGui::BeginDisabled(!online);
    FormSection(T("Cuenta", "Account"));
    AccountSection();
    // Without an account: a nickname typed here, which Link with Discord or Steam turns into one
    // (the way in when the site does not open on this network).
    const NickCheck::Result policy = NickInfo();
    const bool required = policy.account_required;
    if (SignedIn()) {
        FormSection(T("Cuentas vinculadas", "Linked accounts"));
        if (online) DiscordAccountSection();
    } else {
        FormSection(required ? T("Crear la cuenta desde aquí", "Create the account from here")
                             : T("Jugar sin cuenta", "Play without an account"));
        FormNote(required ? T("Elige un apodo y vincúlalo con Discord o Steam.", "Pick a nickname and link it with Discord or Steam.")
                          : T("Escribe tu apodo a mano.", "Type your nickname by hand."));
        // A nickname linked to this player's Discord or Steam account is their identity: it is
        // changed by unlinking it first (DiscordAccountSection), not by typing over it.
        const NickCheck::Result nick_info = NickInfo();
        const bool nick_locked = nick_info.status == "yours" && nick_info.linked;
        ImGui::BeginDisabled(nick_locked);
        NickInput(FormLabel(T("Apodo", "Nickname")));
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
        ImGui::EndDisabled();
        HelpMarker(nick_locked
                       ? T("Este apodo está vinculado a tu cuenta de Discord o Steam y queda fijo. Para cambiarlo, pulsa "
                           "Desvincular abajo.",
                           "This nickname is linked to your Discord or Steam account and stays fixed. To change it, press "
                           "Unlink below.")
                       : T("Tu nombre en el online, obligatorio: de 3 a 16 letras, números, _ o -. Cada apodo es una cuenta "
                           "distinta en el servidor (monedas, objetos del Pit Stop), así que no uses el de otra persona.",
                           "Your online name, required: 3 to 16 letters, digits, _ or -. Each nickname is a separate account "
                           "on the server (coins, Pit Stop items), so do not use someone else's."));
        if (const char* problem = NickProblem(CurrentNick())) {
            ImGui::TextColored(kYellow, "%s", problem);
        } else if (online) {
            NickAvailabilityLine();
            DiscordAccountSection();
        }
    }
    FormSection(T("Amigos", "Friends"));
    // Friends: the in-game menu's Friends tab (Insert / L3+R3) finds, adds and invites them;
    // the names of the old Friends field (online_friends) get friend requests once.
    FormNote(T("Tus amigos se buscan, agregan e invitan dentro del juego: menú (Insert / L3+R3) > Amigos.",
               "Friends are found, added and invited in the game: menu (Insert / L3+R3) > Friends."));
    ImGui::EndDisabled();
}

/// Online > Server: the server and the connection to it.
void OnlineServerPage() {
    App& a = g_app;
    FormSection(T("Servidor", "Server"));
    GameFlag("online", T("Modo online", "Online mode"), true,
             T("Conecta a un servidor Demonware RyuNET para jugar online, emparejamiento, pit stop y marcadores.",
               "Connects to a RyuNET Demonware server for online play, matchmaking, pit stop, and leaderboards."));
    const bool online_on = a.game.Flag("online", true);
    ImGui::BeginDisabled(!online_on);
    if (ImGui::InputText(FormLabel(T("Servidor RyuNET", "RyuNET server")), a.online_server_buf, sizeof(a.online_server_buf))) {
        a.game.Set("online_server", a.online_server_buf);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (Trim(a.online_server_buf).empty()) {
            std::strncpy(a.online_server_buf, "octr.ryu7w7.xyz", sizeof(a.online_server_buf) - 1);
            a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
            a.game.Set("online_server", a.online_server_buf);
        }
        SaveGameIni();
    }
    HelpMarker(T("IP o dominio del servidor RyuNET (predeterminado: octr.ryu7w7.xyz, o 127.0.0.1 para servidor local). Puerto HTTP opcional :8080.",
                 "IP or domain of the RyuNET server (default: octr.ryu7w7.xyz, or 127.0.0.1 for local server). Optional HTTP port :8080."));
    ImGui::SetCursorPosX(g_form_column > 0.0f ? g_form_column : ImGui::GetCursorPosX());
    if (ImGui::SmallButton(T("Por defecto", "Default"))) {
        std::strncpy(a.online_server_buf, "octr.ryu7w7.xyz", sizeof(a.online_server_buf) - 1);
        a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
        a.game.Set("online_server", a.online_server_buf);
        SaveGameIni();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Local (127.0.0.1)", "Local (127.0.0.1)"))) {
        std::strncpy(a.online_server_buf, "127.0.0.1", sizeof(a.online_server_buf) - 1);
        a.online_server_buf[sizeof(a.online_server_buf) - 1] = '\0';
        a.game.Set("online_server", a.online_server_buf);
        SaveGameIni();
    }

    FormSection(T("Conexión", "Connection"));
    EosBridgeSection(false);  // its state, one line
    ImGui::Spacing();
    NatTypeSection(Trim(a.game.Get("online_server", "octr.ryu7w7.xyz")));
    ImGui::Spacing();
    EosBridgeSection(true);
    ImGui::EndDisabled();
}

/// Online > Progress: which Pit Stop items count as yours online.
void OnlineProgressPage() {
    App& a = g_app;
    ImGui::BeginDisabled(!a.game.Flag("online", true));
    // The server's, bought with coins and tied to the nickname, or the save's (run.py sends it at login).
    FormSection(T("Personajes, skins y karts en online", "Characters, skins and karts online"));
    const bool from_save = Lower(Trim(a.game.Get("online_inventory", "server"))) == "save";
    if (ImGui::RadioButton(T("Progresión en el servidor", "Progress on the server"), !from_save)) {
        a.game.Set("online_inventory", "server");
        SaveGameIni();
    }
    FormNote(T("Ganas monedas en las carreras y compras en el Pit Stop; queda en el servidor, ligado a tu apodo.",
               "Races give coins and you buy in the Pit Stop; kept on the server, tied to your nickname."));
    ImGui::Spacing();
    if (ImGui::RadioButton(T("Progresión de guardado local", "Local save progress"), from_save)) {
        a.game.Set("online_inventory", "save");
        SaveGameIni();
    }
    FormNote(T("Online tienes lo mismo que en tu partida guardada (pestaña Guardados).",
               "Online you have what your save has (Saves tab)."));
    ImGui::EndDisabled();

    FormSection("Crash Team Rank");
    // Record validation now; ranked play with ranks and MMR, tied to the server, later.
    FormNote(T("Valida tus récords (próximamente, competitivo con rangos y MMR).",
               "Validate your records (ranked play with MMR coming)."));
    if (g_form_column > 0.0f) ImGui::SetCursorPosX(g_form_column);
    if (ImGui::SmallButton(T("Abrir página", "Open page"))) OpenInShell(L"https://crashteamrank.vercel.app");
}

/// Online > Discord: the status shown on Discord and joining through it.
void OnlineDiscordPage() {
    App& a = g_app;
    FormSection(T("Estado en Discord", "Discord status"));
    LauncherFlag("discord_rpc", T("Mostrar actividad en Discord", "Show activity on Discord"), true,
                 T("Mientras juegas, tu estado de Discord muestra CTR Nitro-Fueled, si juegas online o sin conexión, "
                   "tu apodo y el tiempo de juego. Necesita la aplicación de Discord abierta en este PC.",
                   "While you play, your Discord status shows CTR Nitro-Fueled, whether you play online or offline, "
                   "your nickname and the time played. Needs the Discord app running on this PC."));
    if (!a.launcher.Flag("discord_rpc", true)) return;
    const char* status = "";
    ImVec4 color = kGrey;
    switch (DiscordPresence::GetStatus()) {
    case DiscordPresence::Status::Connected:
        status = T("Conectado a Discord.", "Connected to Discord.");
        color = ImVec4(0.45f, 0.8f, 0.45f, 1.0f);
        break;
    case DiscordPresence::Status::NoDiscord:
        status = T("Discord no está abierto; se conectará solo cuando lo abras.",
                   "Discord is not running; it connects on its own once you open it.");
        break;
    case DiscordPresence::Status::NoAppId:
        status = T("Falta el ID de la aplicación de Discord (discord_app_id en launcher.ini).",
                   "The Discord application ID is missing (discord_app_id in launcher.ini).");
        color = kYellow;
        break;
    case DiscordPresence::Status::Error:
        status = T("Discord rechazó la conexión:", "Discord refused the connection:");
        color = kYellow;
        break;
    case DiscordPresence::Status::Off:
        status = T("Conectando...", "Connecting...");
        break;
    }
    if (g_form_column > 0.0f) ImGui::SetCursorPosX(g_form_column);
    ImGui::TextColored(color, "%s", status);
    if (DiscordPresence::GetStatus() == DiscordPresence::Status::Error) {
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "%s", DiscordPresence::LastError().c_str());
    }

    FormSection(T("Invitaciones", "Invites"));
    LauncherFlag("discord_accept_requests", T("Aceptar a quien pida unirse", "Accept people who ask to join"), true,
                 T("Cuando alguien pulsa \"Pedir unirse\" en tu perfil, se le acepta solo: recibe lo mismo que con "
                   "una invitación tuya (tu apodo y tu servidor).",
                   "When someone presses \"Ask to Join\" on your profile, they are accepted automatically: they "
                   "get the same as from an invite of yours (your nickname and your server)."));
    FormNote(T("Invitar a una privada o a tu grupo: mientras juegas online, en un chat de Discord pulsa "
               "+ y elige \"Invitar a jugar\". Quien acepte te tendrá en sus amigos y se le abrirá el "
               "juego; dentro, abre Amigos y únete a ti. Hace falta este launcher y Discord en los dos PC.",
               "Invite to a private match or your party: while you play online, press + in a Discord "
               "chat and choose \"Invite to play\". Whoever accepts gets you as a friend and their game "
               "starts; in it, they open Friends and join you. Both PCs need this launcher and Discord."));
}

/// Online > Rules: the rules of the online, and where to report and appeal.
void OnlineRulesPage() {
    FormSection(T("Reglas del online", "Online rules"));
    for (const char* rule : {T("Nada de trampas: hacks, trainers, modificaciones de velocidad u objetos, ni tiempos falsos en los récords. Arruinan la partida de todos.", "No cheating: hacks, trainers, speed or item modifications, or fake leaderboard times. They ruin the game for everyone."), T("Respeta a los demás: sin apodos ofensivos, insultos ni acoso.", "Respect others: no offensive nicknames, insults or harassment."), T("No sabotees partidas ni abandones a propósito para molestar.", "Do not sabotage races or quit on purpose to annoy others."), T("Usa tu propio apodo: no te hagas pasar por otra persona.", "Use your own nickname: do not impersonate anyone."), T("¿Ves a alguien haciendo trampas? Repórtalo en el juego: menú (Insert / L3+R3) > Amigos > Reportar jugador.", "See someone cheating? Report them in the game: menu (Insert / L3+R3) > Friends > Report a player.")}) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", rule);
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kYellow, "%s", T("Romper las reglas puede costarte tus récords y un ban del online, temporal o permanente, también por IP.", "Breaking the rules can cost you your records and a ban from online play, temporary or permanent, also by IP."));
    ImGui::PopTextWrapPos();
    FormSection(T("Ayuda", "Help"));
    CommunityDiscordLine(T("Errores, dudas o apelar un baneo: nuestro Discord", "Bugs, questions or a ban appeal: our Discord"));
}

/// One page of Online, like the Settings ones but without the form's column (the account's
/// buttons and messages run the whole width).
void OnlinePage(const char* title, void (*draw)(), bool form) {
    if (!ImGui::BeginTabItem(title)) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("online_page", ImVec2(0, 0), ImGuiChildFlags_None);
    if (form) FormBegin();
    draw();
    FormEnd();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndTabItem();
}

void OnlineTab() {
    OnlineProfileCard();
    ImGui::Spacing();
    if (!ImGui::BeginTabBar("online_pages")) return;
    OnlinePage(T("Cuenta", "Account"), OnlineAccountPage, true);
    OnlinePage(T("Servidor", "Server"), OnlineServerPage, true);
    OnlinePage(T("Progreso", "Progress"), OnlineProgressPage, false);
    OnlinePage("Discord", OnlineDiscordPage, true);
    OnlinePage(T("Reglas", "Rules"), OnlineRulesPage, false);
    ImGui::EndTabBar();
}

std::string PackagePatchId(const PatchInfo& patch) {
    const std::string prefix = "OCTR-Mod-Library.xml/Mod: ";
    return patch.key.rfind(prefix, 0) == 0 ? patch.key.substr(prefix.size()) : std::string();
}

bool ModEnableToggle(bool& on) {
    const ImVec2 size{46.0f, 24.0f};
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    const bool changed = ImGui::Button("##mod_enabled", size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    if (changed) on = !on;
    const ImVec2 p = ImGui::GetItemRectMin();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 track = ImGui::GetColorU32(on ? ImVec4(0.19f, 0.77f, 0.97f, 1.0f)
                                           : ImVec4(0.23f, 0.31f, 0.46f, 1.0f));
    draw->AddRectFilled(p, {p.x + size.x, p.y + size.y}, track, size.y * 0.5f);
    draw->AddCircleFilled({p.x + (on ? size.x - 12.0f : 12.0f), p.y + 12.0f}, 9.0f,
                          ImGui::GetColorU32(on ? ImVec4(0.04f, 0.13f, 0.27f, 1) : ImVec4(0.75f, 0.82f, 0.94f, 1)));
    if (ImGui::IsItemFocused()) draw->AddRect({p.x - 2, p.y - 2}, {p.x + size.x + 2, p.y + size.y + 2},
                                             ImGui::GetColorU32(ImGuiCol_Text), 14.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", on ? T("Desactivar mod", "Disable mod")
                                                                       : T("Activar mod", "Enable mod"));
    return changed;
}

void ImportMods(const std::vector<fs::path>& inputs) {
    App& a = g_app;
    size_t added = 0;
    for (const auto& source : inputs) {
        std::error_code ec;
        std::wstring extension = WidePath(source.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (!fs::is_directory(source) && extension != L".pak" && extension != L".zip" && extension != L".xml") {
            a.notice = T("Añade archivos .pak, .zip, .xml o una carpeta.", "Add .pak, .zip, .xml files or a folder.");
            continue;
        }
        const fs::path destination = a.data / L"mods" / source.filename();
        fs::create_directories(a.data / L"mods", ec);
        if (fs::is_directory(source)) {
            // The mods folder itself, or one holding it (the port's), would be copied into
            // itself without end (issue #70); one already inside it is in the library.
            const auto inside = [](const fs::path& path, const fs::path& folder) {
                const fs::path relative = path.lexically_relative(folder);
                return !relative.empty() && *relative.begin() != L"..";
            };
            std::error_code e1, e2;
            const fs::path real = fs::weakly_canonical(source, e1), mods = fs::weakly_canonical(a.data / L"mods", e2);
            if (!e1 && !e2 && inside(mods, real)) {
                a.notice = T("Esa es la carpeta de mods (o la contiene): añade la carpeta del propio mod.",
                             "That is the mods folder (or holds it): add the mod's own folder.");
                continue;
            }
            if (!e1 && !e2 && inside(real, mods)) {
                a.notice = T("Esa carpeta ya está dentro de la carpeta de mods.", "That folder is already in the mods folder.");
                continue;
            }
        }
        if (fs::exists(destination)) {
            a.notice = T("Ya existe un mod con ese nombre. No se ha sobrescrito.",
                         "A mod with that filename already exists. It was not overwritten.");
            continue;
        }
        if (fs::is_directory(source)) fs::copy(source, destination, fs::copy_options::recursive, ec);
        else fs::copy_file(source, destination, fs::copy_options::none, ec);
        if (ec) {
            a.notice = T("No se pudo importar el mod. Revisa el registro.", "Could not import the mod. Check the log.");
            AddLog("Mod import: " + ec.message());
        } else ++added;
    }
    if (added) {
        a.patches = LoadPatches();
        ReadPatchConfig(a.patch_enabled, a.patch_disabled);
        a.notice = T("Mods añadidos. Revisa la lista y actívalos para jugar.",
                     "Mods added. Review the list and enable them to play.");
    }
}


bool ModIsOn(const PatchInfo& p) {
    const App& a = g_app;
    if (p.ini_key) return a.game.Flag(p.ini_key, false);
    const bool enabled = std::find(a.patch_enabled.begin(), a.patch_enabled.end(), p.key) != a.patch_enabled.end();
    const bool disabled = std::find(a.patch_disabled.begin(), a.patch_disabled.end(), p.key) != a.patch_disabled.end();
    return enabled || (p.default_on && !disabled);
}

bool VisibleMod(const PatchInfo& p) {
    if (!p.title.empty() && p.title != Narrow(GameTitle())) return false;
    if (p.key.find("Online: custom Demonware key") != std::string::npos) return false;
    return !p.ini_key || std::strcmp(p.ini_key, "patch_pro_1080p") != 0;
}

void SetModOn(const PatchInfo& p, bool on) {
    App& a = g_app;
    if (p.ini_key) { a.game.Set(p.ini_key, on ? "1" : "0"); SaveGameIni(); return; }
    auto drop = [&](std::vector<std::string>& v) { v.erase(std::remove(v.begin(), v.end(), p.key), v.end()); };
    drop(a.patch_enabled); drop(a.patch_disabled);
    if (on) a.patch_enabled.push_back(p.key);
    else a.patch_disabled.push_back(p.key);
    WritePatchConfig(a.patch_enabled, a.patch_disabled);
}

void ModHeading(const char* title) {
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.12f);
    ImGui::TextUnformatted(title);
    if (g_font_heading) ImGui::PopFont();
}

void ModSection(const char* title, bool builtin) {
    int total = 0, on = 0;
    for (const auto& p : g_app.patches) if (VisibleMod(p) && (p.ini_key != nullptr) == builtin) {
        ++total; if (ModIsOn(p)) ++on;
    }
    ModHeading(title); ImGui::SameLine(0, 12);
    ImGui::TextColored(ImVec4(.65f,.72f,.85f,1), T("%d de %d activos", "%d of %d on"),on,total);
}

struct ModConfigurationDraft {
    PatchInfo mod;
    std::string error;
    bool open=false,advanced=false,ready=false;
    std::future<Captured> pending;
};
ModConfigurationDraft g_mod_configuration;

void ModConfigurationDialog() {
    auto& draft=g_mod_configuration;
    const char* title=T("Configurar mod","Mod configuration");
    if(draft.open) {
        draft.error.clear();draft.ready=false;
        const std::string ident=PackagePatchId(draft.mod);
        // Opening a dialog only reads the existing catalog. Full integrity and
        // source-hash checks remain in Save; add/reload refreshes the library.
        g_app.patches=LoadPatches(false);
        const auto latest=std::find_if(g_app.patches.begin(),g_app.patches.end(),[&](const auto& p){return PackagePatchId(p)==ident;});
        if(latest!=g_app.patches.end()) {draft.mod=*latest;draft.ready=true;}
        else draft.error=T("El mod ya no está disponible.","This mod is no longer available.");
        ImGui::OpenPopup(title);draft.open=false;
    }
    const auto* viewport=ImGui::GetMainViewport();
    const ImVec2 dialog_size{std::min(760.f,viewport->WorkSize.x-48.f),std::min(520.f,viewport->WorkSize.y-48.f)};
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+viewport->WorkSize.x*.5f,
                                 viewport->WorkPos.y+viewport->WorkSize.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(dialog_size,ImGuiCond_Always);
    if(!ImGui::BeginPopupModal(title,nullptr,ImGuiWindowFlags_NoResize))return;
    if(draft.pending.valid()) {
        if(draft.pending.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
            ImGui::TextUnformatted(T("Guardando...","Saving..."));
            ImGui::EndPopup();return;
        }
        const Captured r=draft.pending.get();
        if(!r.output.empty())AddLog(r.output);
        const bool success=r.started && r.code==0;
        if(!success) {
            draft.error=T("No se pudieron guardar las opciones. Revisa el registro.","Could not update mod options. Check the log.");
            const auto reason=r.output.rfind("ValueError: ");
            if(reason!=std::string::npos) {
                draft.error=r.output.substr(reason+12);
                const auto end=draft.error.find_first_of("\r\n");
                if(end!=std::string::npos)draft.error.resize(end);
            }
        } else {
            g_app.patches=LoadPatches(false);
            g_app.notice=T("Opciones guardadas. Se aplican al iniciar el juego.","Mod options saved. Applied when you start the game.");
            ImGui::CloseCurrentPopup();ImGui::EndPopup();return;
        }
    }
    ImGui::TextWrapped("%s",draft.mod.mod_name.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(g_app.game_running || !g_app.python || !draft.ready);
    const auto advanced_count=std::count_if(draft.mod.mod_options.begin(),draft.mod.mod_options.end(),[](const auto& o){return o.advanced;});
    const auto grid_count=std::count_if(draft.mod.mod_options.begin(),draft.mod.mod_options.end(),[](const auto& o){return !o.row.empty();});
    const float footer=(draft.error.empty()?62.f:110.f)+(advanced_count?45.f:0.f)+
                       (draft.advanced?40.f*static_cast<float>(advanced_count):0.f);
    const float options_height=std::max(80.f,ImGui::GetContentRegionAvail().y-footer);
    ImGui::BeginChild("options",ImVec2(0,options_height));
    auto render_option=[&](ModOption& option) {
        ImGui::PushID(option.id.c_str());
        if(option.type=="boolean") {
            bool on=option.value=="true";
            if(ImGui::Checkbox("##enabled",&on))option.value=on?"true":"false";
            ImGui::SameLine();ImGui::TextUnformatted(option.label.c_str());
        } else {
            ImGui::TextUnformatted(option.label.c_str());
            std::string preview=option.value;
            for(const auto& [value,label]:option.choices)if(value==option.value)preview=label;
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##choice",preview.c_str())) {
                for(const auto& [value,label]:option.choices) {
                    bool selected=value==option.value;
                    if(ImGui::Selectable(label.c_str(),selected))option.value=value;
                    if(selected)ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        if(!option.description.empty())ImGui::TextWrapped("%s",option.description.c_str());
        ImGui::Spacing();ImGui::PopID();
    };
    for(auto& option:draft.mod.mod_options)if(!option.advanced&&option.row.empty())render_option(option);
    std::vector<std::string> rows,columns;
    for(const auto& o:draft.mod.mod_options)if(!o.row.empty()) {
        if(std::find(rows.begin(),rows.end(),o.row)==rows.end())rows.push_back(o.row);
        if(std::find(columns.begin(),columns.end(),o.column)==columns.end())columns.push_back(o.column);
    }
    if (LauncherUI::Table table{"mod-pieces", static_cast<int>(columns.size() + 1), !rows.empty()}) {
        table.StretchColumn(T("Grupo","Group"), 1.3f);
        for(const auto& column:columns)table.StretchColumn(column.c_str());
        table.Headers();
        for(const auto& row:rows) {
            table.Row();table.Cell();ImGui::TextUnformatted(row.c_str());
            for(const auto& column:columns) {
                table.Cell();
                for(auto& o:draft.mod.mod_options)if(o.row==row&&o.column==column) {
                    ImGui::PushID(o.id.c_str());bool on=o.value=="true";
                    if(ImGui::Checkbox("##piece",&on))o.value=on?"true":"false";
                    if(!o.description.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",o.description.c_str());
                    ImGui::PopID();
                }
            }
        }
    }
    if(advanced_count) {
        draft.advanced=ImGui::CollapsingHeader(T("Opciones avanzadas","Advanced settings"));
        if(draft.advanced)for(auto& option:draft.mod.mod_options)if(option.advanced)render_option(option);
    }
    ImGui::EndChild();
    if(!draft.error.empty())ImGui::TextWrapped("%s",draft.error.c_str());
    if(ImGui::Button(T("Restablecer valores","Reset to defaults")))
        for(auto& option:draft.mod.mod_options)option.value=option.default_value;
    ImGui::SameLine();
    if(ImGui::Button(T("Guardar","Save"))) {
        draft.error.clear();
        const std::string ident=PackagePatchId(draft.mod);
        const bool valid=!ident.empty() && std::all_of(ident.begin(),ident.end(),[](unsigned char c){return std::isalnum(c)||c=='-';});
        if(valid && g_app.python) {
            const auto& a=g_app;
            std::wstring cmd=Quote(a.python->exe)+L" "+a.python->args+L"-X utf8 "+
                Quote(WidePath((a.root/L"scripts"/L"mod_packages.py")))+L" --scan "+Quote(WidePath(a.data))+
                L" --configure "+Quote(Widen(ident))+L" --expected-sha "+Quote(Widen(draft.mod.mod_hash));
            for(const auto& o:draft.mod.mod_options)cmd+=L" --option "+Quote(Widen(o.id+"="+o.value));
            const auto exe=a.python->exe;
            const auto root=a.root;
            draft.pending=std::async(std::launch::async,[exe,cmd,root]{return RunCaptured(exe,cmd,root,120000);});
        }
    }
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::Button(T("Cancelar","Cancel")))ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

bool ModConfigureIcon() {
    bool clicked=ImGui::Button("##configure",ImVec2(32,25));
    const ImVec2 q=ImGui::GetItemRectMin();auto* d=ImGui::GetWindowDrawList();
    const ImU32 ink=ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 c{q.x+16,q.y+12};
    // Thin outline and the same button frame/ink as the adjacent trash icon.
    const ImVec2 outline[]={{c.x-6,c.y+7},{c.x-8,c.y+5},{c.x-1,c.y-2},
        {c.x-2,c.y-5},{c.x+1,c.y-8},{c.x+1,c.y-4},{c.x+4,c.y-2},
        {c.x+7,c.y-4},{c.x+6,c.y+1},{c.x+3,c.y+3},{c.x,c.y+2}};
    d->AddPolyline(outline,IM_ARRAYSIZE(outline),ink,1.0f,ImDrawFlags_Closed);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",T("Configurar mod","Configure mod"));
    return clicked;
}

bool ModCard(const PatchInfo& p) {
    const bool package = !PackagePatchId(p).empty();
    bool on = ModIsOn(p), remove = false;
    ImGui::PushID(p.key.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, on ? ImVec4(.075f,.19f,.38f,1) : ImVec4(.065f,.14f,.29f,1));
    ImGui::PushStyleColor(ImGuiCol_Border, on ? ImVec4(.12f,.36f,.57f,1) : ImVec4(.12f,.21f,.36f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 11);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(17,10));
    ImGui::BeginChild("card", ImVec2(0,0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginDisabled(g_app.game_running || (package && !g_app.python));
    if (ModEnableToggle(on)) SetModOn(p,on);
    ImGui::EndDisabled();
    ImGui::SameLine(0,18);
    const float text_x = ImGui::GetCursorPosX();
    const float top = ImGui::GetCursorPosY();
    const float right = ImGui::GetWindowContentRegionMax().x;
    static std::set<std::string> expanded;
    const bool details = expanded.count(p.key) != 0;
    const float action_width = package ? (p.mod_options.empty()?76.0f:112.0f) : 32.0f;
    // A separate text child wraps long names/metadata without colliding with actions.
    ImGui::BeginChild("summary", ImVec2(std::max(80.0f,right-text_x-action_width-12),0), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (g_font_heading) ImGui::PushFont(g_font_heading, 0);
    ImGui::TextWrapped("%s", package ? p.mod_name.c_str() : p.name.c_str());
    if (g_font_heading) ImGui::PopFont();
    std::string meta;
    const char* region = RegionOf(p.title);
    if (*region && !package) meta = std::string(region) + "   ";
    meta += p.author;
    if (!p.version.empty()) meta += "  · v" + p.version;
    if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(meta.c_str()).x + 12) ImGui::SameLine(0,10);
    ImGui::TextColored(ImVec4(.65f,.73f,.87f,1), "%s",meta.c_str());
    const char* description = nullptr;
    if (p.ini_key && std::strcmp(p.ini_key,"patch_60fps")==0)
        description = T("Ejecuta el juego a 60 FPS. La versión 2.0 también elimina el vsync de doble búfer.",
                        "Runs the game at 60 FPS. Version 2.0 also removes double-buffer vsync.");
    if (p.ini_key && std::strcmp(p.ini_key,"patch_no_motion_blur")==0)
        description = T("Desactiva el desenfoque de movimiento: el de la cámara y el de los objetos que se mueven (público, plantas).",
                        "Turns off the motion blur: the camera's and that of moving objects (crowd, plants).");
    const bool unlock = p.ini_key && std::strcmp(p.ini_key,"patch_unlock_all")==0;
    if (unlock) description = T("Desbloquea el contenido que el juego concede al cumplir sus condiciones, con el interruptor de las demos E3/PAX.",
                               "Unlocks what the game normally grants for meeting its unlock conditions, using the same switch as the E3/PAX demos.");
    if (details && description) ImGui::TextWrapped("%s",description);
    if (unlock && details) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(.20f,.23f,.30f,1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10,7));
        ImGui::BeginChild("save_warning",ImVec2(0,0),ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextColored(kYellow,"%s",T("Copia antes tu partida.","Back up your save first.")); ImGui::SameLine();
        if (ImGui::TextLink(T("Abrir Guardados", "Open Saves tab"))) g_app.active_page=6;
        ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor();
    }
    if (details && package) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", p.mod_description.empty()
            ? T("El creador no ha añadido una descripción.","The creator has not provided a description.")
            : p.mod_description.c_str());
        ImGui::TextColored(ImVec4(.65f,.73f,.87f,1),"mods/%s",p.mod_source.c_str());
        if (!p.mod_versions.empty()) ImGui::TextWrapped("%s",p.mod_versions.c_str());
    }
    if (details && !package) {
        ImGui::Spacing();
        if (!p.note.empty()) ImGui::TextWrapped("%s",p.note.c_str());
        ImGui::TextColored(ImVec4(.65f,.73f,.87f,1),"%s",p.key.c_str());
    }
    ImGui::EndChild();
    const float end_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(right-action_width,top));
    if (package) {
        ImGui::BeginDisabled(g_app.game_running || !g_app.python);
        if (!p.mod_options.empty()) {
            if(ModConfigureIcon()){g_mod_configuration.mod=p;g_mod_configuration.open=true;g_mod_configuration.advanced=false;}
            ImGui::SameLine(0,4);
        }
        if (ImGui::Button("##remove",ImVec2(32,25))) remove=true;
        const ImVec2 q=ImGui::GetItemRectMin();
        const ImVec2 c{q.x+16,q.y+12};
        auto* d=ImGui::GetWindowDrawList(); const ImU32 ink=ImGui::GetColorU32(ImGuiCol_Text);
        d->AddRect({c.x-4,c.y-3},{c.x+4,c.y+6},ink,1);
        d->AddLine({c.x-6,c.y-5},{c.x+6,c.y-5},ink);
        d->AddLine({c.x-2,c.y-7},{c.x+2,c.y-7},ink);
        d->AddLine({c.x-1,c.y-1},{c.x-1,c.y+4},ink);
        d->AddLine({c.x+2,c.y-1},{c.x+2,c.y+4},ink);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",T("Quitar: se conserva una copia y las partidas.","Remove: keeps a recoverable copy and saves."));
        ImGui::EndDisabled();
    }
    // Reserve the same top-right slot regardless of expansion or card height.
    ImGui::SetCursorPos(ImVec2(right-32,top));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0,0,0,0));
    if (ImGui::Button("##details",ImVec2(32,25))) {
        if (details) expanded.erase(p.key); else expanded.insert(p.key);
    }
    const bool hovered=ImGui::IsItemHovered();
    if (hovered) ImGui::SetTooltip("%s",details ? T("Ocultar detalles","Hide details") : T("Mostrar detalles","Show details"));
    const ImVec2 origin=ImGui::GetItemRectMin();
    const ImVec2 center{origin.x+16,origin.y+12.5f};
    ImGuiStorage* storage=ImGui::GetStateStorage();
    const ImGuiID rotation_id=ImGui::GetID("chevron_rotation");
    const float target=expanded.count(p.key) ? 3.14159265f : 0.0f;
    float angle=storage->GetFloat(rotation_id,details ? 3.14159265f : 0.0f);
    angle+=(target-angle)*std::min(1.0f,ImGui::GetIO().DeltaTime*20.0f);
    storage->SetFloat(rotation_id,angle);
    const auto rotated=[&](float x,float y) {
        return ImVec2(center.x+x*std::cos(angle)-y*std::sin(angle),center.y+x*std::sin(angle)+y*std::cos(angle));
    };
    auto* draw=ImGui::GetWindowDrawList();
    const ImU32 ink=ImGui::GetColorU32(hovered ? ImVec4(.85f,.95f,1,1) : ImVec4(.65f,.77f,.91f,1));
    draw->AddLine(rotated(-5,-2.5f),rotated(0,2.5f),ink,1.8f);
    draw->AddLine(rotated(0,2.5f),rotated(5,-2.5f),ink,1.8f);
    ImGui::PopStyleColor(3); ImGui::PopStyleVar();
    ImGui::SetCursorPosY(std::max(end_y,top+26));
    ImGui::Dummy(ImVec2(0,0));
    ImGui::EndChild();
    ImGui::PopStyleVar(2); ImGui::PopStyleColor(2); ImGui::PopID();
    return remove;
}

bool ModActionIcon(const char* id, const char* tooltip, bool folder, float size) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,6);
    const bool clicked=ImGui::Button(id,ImVec2(size,size));
    ImGui::PopStyleVar(); ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",tooltip);
    const ImVec2 a=ImGui::GetItemRectMin();
    const ImVec2 c{a.x+size*.5f,a.y+size*.5f};
    const ImU32 ink=ImGui::GetColorU32(ImVec4(.70f,.80f,.94f,1));
    ImDrawList* draw=ImGui::GetWindowDrawList();
    if (folder) {
        draw->PathLineTo({c.x-8,c.y-5}); draw->PathLineTo({c.x-2,c.y-5});
        draw->PathLineTo({c.x,c.y-2}); draw->PathLineTo({c.x+8,c.y-2});
        draw->PathLineTo({c.x+8,c.y+6}); draw->PathLineTo({c.x-8,c.y+6});
        draw->PathStroke(ink,ImDrawFlags_Closed,1.5f);
    } else {
        draw->PathArcTo(c,7,.65f,6.0f,24); draw->PathStroke(ink,0,1.5f);
        draw->AddLine({c.x+6.8f,c.y-2},{c.x+6.8f,c.y-8},ink,1.5f);
        draw->AddLine({c.x+6.8f,c.y-2},{c.x+1,c.y-2},ink,1.5f);
    }
    return clicked;
}

void ModsTab() {
    App& a = g_app;
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.83f,.88f,.97f,1));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,ImVec4(.63f,.71f,.85f,1));
    ModSection(T("Mods incluidos","Built-in mods"),true);
    ImGui::Spacing();
    for (const auto& p : a.patches) if (VisibleMod(p) && p.ini_key) ModCard(p);
    ImGui::Dummy(ImVec2(0,14));
    const float left=ImGui::GetCursorPosX();
    const float right=ImGui::GetWindowContentRegionMax().x;
    const float gap=ImGui::GetStyle().ItemSpacing.x;
    const float icon_size=36.0f;
    const float folder_width=ImGui::CalcTextSize(T("Añadir carpeta","Add folder")).x+24;
    const float files_width=ImGui::CalcTextSize(T("+ Añadir archivos","+ Add files")).x+28;
    const float toolbar_width=icon_size*2+folder_width+files_width+gap*3;
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ModSection(T("Tus mods y parches","Your mods & patches"),false);
    ImGui::EndGroup();
    const float heading_end=ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x;
    if (heading_end+24+toolbar_width<=right) ImGui::SameLine(right-toolbar_width);
    else {
        ImGui::Spacing();
        ImGui::SetCursorPosX(std::max(left,right-toolbar_width));
    }
    ImGui::BeginGroup();
    ImGui::BeginDisabled(a.game_running || !a.python);
    if (ModActionIcon("##reload_mods",T("Recargar mods","Reload mods"),false,icon_size)) {
        a.patches=LoadPatches(); ReadPatchConfig(a.patch_enabled,a.patch_disabled);
    }
    ImGui::SameLine();
    if (ModActionIcon("##open_mods",T("Abrir carpeta de mods","Open mods folder"),true,icon_size)) {
        std::error_code ec; fs::create_directories(a.data / L"mods",ec); OpenInShell(a.data / L"mods");
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Añadir carpeta","Add folder"),ImVec2(folder_width,icon_size)))
        if (const auto folder=PickFolder(a.hwnd,L"Add mod folder",fs::path())) ImportMods({*folder});
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.17f,.72f,.94f,1));
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(.02f,.10f,.20f,1));
    if (ImGui::Button(T("+ Añadir archivos","+ Add files"),ImVec2(files_width,icon_size)))
        ImportMods(PickFiles(a.hwnd,L"Add mods or patches",L"Mods (*.pak;*.zip;*.xml)",L"*.pak;*.zip;*.xml"));
    ImGui::PopStyleColor(2); ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::Spacing();
    std::string remove_id, remove_hash;
    for (const auto& p : a.patches) if (VisibleMod(p) && !p.ini_key) {
        if (ModCard(p)) { remove_id=PackagePatchId(p); remove_hash=p.mod_hash; }
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(0.015f,0.13f,0.32f,0.82f));
    ImGui::PushStyleColor(ImGuiCol_Border,ImVec4(.18f,.29f,.44f,1));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,11);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(18,17));
    ImGui::BeginChild("mod_drop_area",ImVec2(0,0),ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding);
    const auto centered = [](const char* text) {
        const float available=ImGui::GetContentRegionAvail().x;
        const float width=ImGui::CalcTextSize(text).x;
        if (width<available) ImGui::SetCursorPosX(ImGui::GetCursorPosX()+(available-width)*.5f);
        ImGui::TextWrapped("%s",text);
    };
    centered(T("Añade .pak, .zip, .xml o una carpeta con los botones de arriba.",
               "Use the buttons above to add .pak, .zip or .xml files, or a folder."));
    centered(T("Se copian a la carpeta mods junto al launcher.","They're copied into the mods folder next to the launcher."));
    ImGui::Spacing();
    const char* guide=T("Lee la guía de empaquetado","Read the packaging guide");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,(ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize(guide).x)*.5f));
    // v0.8.5 packages carry no docs\ (a folder that has one, from an older version, still opens it)
    if (ImGui::TextLink(guide)) {
        std::error_code ec;
        const fs::path local=a.root/L"docs"/L"mod-packaging.html";
        OpenInShell(fs::is_regular_file(local,ec) ? local
                    : fs::path(L"https://github.com/Ryu7w7/OCTR-NitroFueled-PC/tree/main/docs"));
    }
    // The game's page on GameBanana, where the community shares its mods.
    const char* gamebanana=T("Buscar mods en GameBanana","Find mods on GameBanana");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,(ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize(gamebanana).x)*.5f));
    if (ImGui::TextLink(gamebanana)) OpenInShell(L"https://gamebanana.com/games/26138");
    // The modding community's Discord server: help making and packaging mods.
    const char* modding=T("Discord de modding (ayuda para crear mods)","Modding Discord (help making mods)");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.0f,(ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize(modding).x)*.5f));
    if (ImGui::TextLink(modding)) OpenInShell(L"https://discord.gg/434Rx9hq6T");
    ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopStyleColor(2);
    ModConfigurationDialog();
    if (a.game_running) ImGui::TextColored(kYellow,"%s",T("Cierra el juego para cambiar mods.","Close the game to change mods."));
    ImGui::PopStyleColor(2);
    // Reload only after rendering: the loop's references must remain valid.
    if (!remove_id.empty() && a.python) {
        const bool valid_id = std::all_of(remove_id.begin(), remove_id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
        const bool valid_hash = remove_hash.size() == 64 && std::all_of(remove_hash.begin(), remove_hash.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
        });
        if (!valid_id || !valid_hash) {
            a.notice = T("Los datos del mod no son válidos. Pulsa Recargar.", "Mod details are invalid. Press Reload.");
            return;
        }
        const std::wstring cmd = Quote(a.python->exe) + L" " + a.python->args + L"-X utf8 " +
            Quote(WidePath((a.root / L"scripts" / L"mod_packages.py"))) + L" --scan " + Quote(WidePath(a.data)) +
            L" --remove " + Quote(Widen(remove_id)) + L" --expected-sha " + Quote(Widen(remove_hash));
        const Captured r = RunCaptured(a.python->exe, cmd, a.root, 120000);
        if (!r.output.empty()) AddLog(r.output);
        a.notice = r.started && r.code == 0 ? T("Mod quitado. El mod y las partidas se han conservado.",
                                                                  "Mod removed. The mod and saves have been kept.")
                                           : T("No se pudo quitar el mod. Revisa el registro y pulsa Recargar.",
                                               "Could not remove the mod. Check the log and press Reload.");
        a.patches = LoadPatches();
        ReadPatchConfig(a.patch_enabled, a.patch_disabled);
    }
}

// ---- Cloud saves (scripts/cloud_cli.py; the server's cloud_saves.go) ----
// The account's last saves on the server, listed on request; restoring one keeps this PC's save
// in user/cloud/backup-<title>.zip first. The game uploads after every session it changed.
struct CloudVersion {
    int id = 0, created = 0, size = 0;
    std::string title, source;
};
struct CloudSavesState {
    std::mutex mutex;
    bool busy = false, error = false, loaded = false;
    std::string message;
    std::vector<CloudVersion> list;
};
CloudSavesState g_cloud;

void CloudCli(const std::vector<std::wstring>& args, std::function<void(const std::string&)> done) {
    App& a = g_app;
    if (!a.python) {
        std::scoped_lock lock{g_cloud.mutex};
        g_cloud.error = true;
        g_cloud.message = T("Falta Python 3.", "Python 3 is missing.");
        return;
    }
    {
        std::scoped_lock lock{g_cloud.mutex};
        if (g_cloud.busy) return;
        g_cloud.busy = true;
        g_cloud.message.clear();
    }
    std::wstring cmd = Quote(a.python->exe) + L" " + a.python->args + L"-I -X utf8 " + Quote(WidePath((a.root / L"scripts" / L"cloud_cli.py")));
    for (const auto& arg : args) cmd += L" " + arg;
    cmd += L" --data " + Quote(WidePath(a.data)) + L" --config " + Quote(WidePath(GameIni())) +
           L" --user " + Quote(WidePath(UserRoot()));
    const std::wstring exe = a.python->exe;
    const fs::path root = a.root;
    std::thread([cmd, exe, root, done] {
        const Captured r = RunCaptured(exe, cmd, root, 180000);
        // The script prints one JSON object, last.
        std::string out = r.output;
        if (const auto line = out.rfind("\n{"); line != std::string::npos) out = out.substr(line + 1);
        else if (const auto brace = out.find('{'); brace != std::string::npos) out = out.substr(brace);
        done(r.started ? out : std::string("{\"error\":\"python\"}"));
        std::scoped_lock lock{g_cloud.mutex};
        g_cloud.busy = false;
    }).detach();
}

void CloudList() {
    CloudCli({L"list"}, [](const std::string& out) {
        std::vector<CloudVersion> list;
        for (size_t at = out.find("{\"id\""); at != std::string::npos; at = out.find("{\"id\"", at + 1)) {
            const std::string one = out.substr(at, out.find('}', at) - at + 1);
            CloudVersion v;
            JsonNumber(one, "id", v.id);
            JsonNumber(one, "created", v.created);
            JsonNumber(one, "size", v.size);
            v.title = JsonField(one, "title");
            v.source = JsonField(one, "source");
            list.push_back(v);
        }
        const std::string error = JsonField(out, "error");
        std::scoped_lock lock{g_cloud.mutex};
        g_cloud.loaded = error.empty();
        g_cloud.list = std::move(list);
        g_cloud.error = !error.empty();
        g_cloud.message = error == "no_account" ? T("Inicia sesión con tu cuenta (pestaña Online) para ver tus partidas en la nube.",
                                                    "Log in with your account (Online tab) to see your cloud saves.")
                          : !error.empty()      ? T("No se pudo consultar el servidor.", "Could not ask the server.")
                          : g_cloud.list.empty() ? T("Todavía no hay partidas en la nube: se suben solas al terminar de jugar online.",
                                                     "No cloud saves yet: they upload by themselves after you play online.")
                                                 : std::string();
    });
}

void CloudRestore(int id) {
    CloudCli({L"restore", L"--id", std::to_wstring(id)}, [](const std::string& out) {
        const std::string error = JsonField(out, "error");
        std::scoped_lock lock{g_cloud.mutex};
        g_cloud.error = !error.empty();
        g_cloud.message = error.empty() ? Format(T("Partida restaurada. La que tenías quedó en %s.", "Save restored. The one you had is in %s."),
                                                 JsonText(out, "backup"))
                                        : Format(T("No se pudo restaurar: %s", "Could not restore: %s"), error);
        g_app.saves_dirty = true;
    });
}

void CloudSavesSection() {
    App& a = g_app;
    FormSection(T("Partidas en la nube", "Cloud saves"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Al terminar de jugar online, tu partida se sube a tu cuenta (las 5 últimas versiones). Desde aquí "
                                      "puedes traer una a este PC: la que tienes ahora se guarda antes como copia.",
                                      "After you play online, your save goes up to your account (the last 5 versions). From here you "
                                      "can bring one to this PC: the one you have now is kept first as a backup."));
    ImGui::PopTextWrapPos();
    bool busy, error, loaded;
    std::string message;
    std::vector<CloudVersion> list;
    {
        std::scoped_lock lock{g_cloud.mutex};
        busy = g_cloud.busy;
        error = g_cloud.error;
        loaded = g_cloud.loaded;
        message = g_cloud.message;
        list = g_cloud.list;
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(loaded ? T("Actualizar lista", "Refresh list") : T("Ver mis partidas en la nube", "See my cloud saves"))) CloudList();
    ImGui::EndDisabled();
    if (busy) {
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "%s", T("Consultando…", "Working…"));
    }
    if (LauncherUI::Table table{"cloud", 4, loaded && !list.empty()}) {
        table.StretchColumn(T("Fecha", "Date"), 1.4f);
        table.StretchColumn(T("Juego", "Game"), 0.5f);
        table.StretchColumn(T("Tamaño", "Size"), 0.6f);
        table.FixedColumn("", std::max(LauncherUI::Table::ButtonWidth(T("Pulsa otra vez", "Press again")),
                                           LauncherUI::Table::ButtonWidth(T("Traer a este PC", "Bring to this PC"))));
        table.Headers();
        static int armed = 0;
        static uint64_t armed_at = 0;
        for (const auto& v : list) {
            ImGui::PushID(v.id);
            table.Row();
            table.Cell();
            char when[64] = "-";
            const time_t t = time_t(v.created);
            struct tm local {};
            if (LocalTime(&local, &t) == 0) std::strftime(when, sizeof when, "%d/%m/%Y %H:%M", &local);
            ImGui::TextUnformatted(when);
            table.Cell();
            ImGui::TextUnformatted(v.title == "CUSA14876" ? "EU" : "USA");
            table.Cell();
            ImGui::TextUnformatted(FormatSize(uint64_t(v.size)).c_str());
            table.Cell();
            const bool is_armed = armed == v.id && Ticks() - armed_at < 5000;
            ImGui::BeginDisabled(busy || a.game_running);
            if (table.Action(is_armed ? T("Pulsa otra vez", "Press again") : T("Traer a este PC", "Bring to this PC"))) {
                if (is_armed) {
                    armed = 0;
                    CloudRestore(v.id);
                } else {
                    armed = v.id;
                    armed_at = Ticks();
                }
            }
            ImGui::EndDisabled();
            if (!is_armed && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Reemplaza la partida de este PC por esta versión (la actual se guarda como copia).",
                                          "Replaces this PC's save with this version (the current one is kept as a backup)."));
            ImGui::PopID();
        }
    }
    if (!message.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(error ? kYellow : kGreen, "%s", message.c_str());
        ImGui::PopTextWrapPos();
    }
}

/// A table column as wide as `sample` in the current font, plus the cell padding.
float ColumnWidthFor(const char* sample) {
    return ImGui::CalcTextSize(sample).x + ImGui::GetStyle().CellPadding.x * 2.0f + 6.0f;
}

/// A button in the launcher's cyan, for the main action of a section.
bool AccentButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.56f, 0.82f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.68f, 0.92f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.22f, 0.78f, 0.98f, 1.0f));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    return pressed;
}

void SavesListPage();
void SavesBackupsPage();

void SavesTab() {
    App& a = g_app;
    if (a.saves_dirty) {
        a.saves = ListSaves();
        a.backups = ListBackups();
        a.saves_dirty = false;
    }
    if (a.game_running) {
        ImGui::TextColored(kYellow, "%s", T("Cierra el juego para copiar, importar o restaurar partidas.",
                                            "Close the game to back up, import or restore saves."));
    }
    if (!ImGui::BeginTabBar("saves_pages")) return;
    SettingsPage(T("Partidas", "Saves"), SavesListPage);
    // "###": the tab keeps its identity while the count in its name changes
    const std::string backups_tab = (a.backups.empty() ? std::string(T("Copias de seguridad", "Backups"))
                                                       : Format(T("Copias de seguridad (%s)", "Backups (%s)"),
                                                                std::to_string(a.backups.size()))) +
                                    "###saves_backups";
    SettingsPage(backups_tab.c_str(), SavesBackupsPage);
    SettingsPage(T("En la nube", "Cloud"), CloudSavesSection);
    ImGui::EndTabBar();
}

/// Saves > Saves: what is on this PC, and backing it up, exporting and importing.
void SavesListPage() {
    App& a = g_app;
    const bool locked = a.game_running;
    FormSection(T("Partidas en este PC", "Saves on this PC"));
    ImGui::TextColored(kGrey, "%s", PathText(SaveRoot()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Abrir carpeta", "Open folder"))) {
        std::error_code ec;
        fs::create_directories(SaveRoot(), ec);
        OpenInShell(SaveRoot());
    }
    ImGui::Spacing();
    if (a.saves.empty()) {
        ImGui::TextUnformatted(T("Todavía no hay partidas guardadas.", "No saves yet."));
    } else if (LauncherUI::Table table{"saves", 4}) {
        ImGui::TableSetupColumn(T("Ranura", "Slot"), ImGuiTableColumnFlags_WidthFixed, ColumnWidthFor("RaceReplay0"));
        ImGui::TableSetupColumn(T("Descripción", "Description"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed, ColumnWidthFor("000.0 MB"));
        ImGui::TableSetupColumn(T("Modificada", "Modified"), ImGuiTableColumnFlags_WidthFixed, ColumnWidthFor("2026-11-15 23:59"));
        ImGui::TableHeadersRow();
        for (const auto& s : a.saves) {
            table.Row();
            table.Cell();
            ImGui::TextUnformatted(s.dir.c_str());
            table.Cell();
            std::string text = s.title;
            if (!s.subtitle.empty()) text += (text.empty() ? "" : " - ") + s.subtitle;
            if (!s.detail.empty()) text += (text.empty() ? "" : " - ") + s.detail;
            ImGui::PushTextWrapPos(0.0f);  // a long description wraps in its column
            ImGui::TextUnformatted(text.empty() ? "-" : text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(s.bytes).c_str());
            table.Cell();
            ImGui::TextUnformatted(s.modified.c_str());
        }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(locked);
    ImGui::BeginDisabled(a.saves.empty());
    if (AccentButton(T("Hacer copia de seguridad", "Back up now"))) {
        fs::path made;
        const std::string error = BackupSaves(nullptr, &made);
        a.notice = error.empty() ? std::string(T("Copia creada: ", "Backup created: ")) + PathText(made.filename()) : error;
        a.saves_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Exportar...", "Export..."))) {
        if (const auto dest = PickFolder(a.hwnd, L"Exportar partidas / Export saves", fs::path())) {
            const fs::path target = *dest / Widen("CTR-saves-" + Stamp());
            const std::string error = CopyTree(SaveRoot(), target);
            a.notice = error.empty() ? std::string(T("Partidas exportadas a ", "Saves exported to ")) + PathText(target) : error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Importar...", "Import..."))) {
        if (const auto from = PickFolder(a.hwnd, L"Importar partida / Import save", fs::path())) {
            const std::string error = ImportSaves(*from);
            a.notice = error.empty() ? std::string(T("Partida importada. La anterior quedó en una copia de seguridad.",
                                                     "Save imported. The previous one was backed up."))
                                     : error;
            a.saves_dirty = true;
        }
    }
    HelpMarker(T("Acepta una copia de seguridad de este launcher, la carpeta CUSA13795 de las partidas de otro PC, o una "
                 "partida de PS4 descifrada (Apollo Save Tool, Save Wizard: una carpeta con sce_sys\\param.sfo). Antes de "
                 "importar se hace una copia de seguridad de tus partidas.",
                 "Takes a backup made by this launcher, the CUSA13795 saves folder from another PC, or a decrypted PS4 "
                 "save (Apollo Save Tool, Save Wizard: a folder with sce_sys\\param.sfo). Your saves are backed up "
                 "before importing."));
    ImGui::EndDisabled();
    FormNote(T("Copia de seguridad: guarda una copia aquí mismo (pestaña Copias de seguridad). Exportar: una copia en la "
               "carpeta que elijas. Importar: trae una partida de otro PC o de PS4.",
               "Back up: keeps a copy here (Backups tab). Export: a copy in the folder you pick. Import: brings a save "
               "from another PC or a PS4."));
}

/// Saves > Backups: the copies made here, to restore or delete.
void SavesBackupsPage() {
    App& a = g_app;
    const bool locked = a.game_running;
    FormSection(T("Copias de seguridad", "Backups"));
    FormNote(T("Se hacen al pulsar Hacer copia de seguridad y antes de importar o restaurar, para que nunca pierdas una partida.",
               "Made when you press Back up now and before importing or restoring, so a save is never lost."));
    ImGui::Spacing();
    if (a.backups.empty()) {
        ImGui::TextColored(kGrey, "%s", T("Todavía no hay copias.", "No backups yet."));
    } else if (LauncherUI::Table table{"backups", 4}) {
        ImGui::TableSetupColumn(T("Copia", "Backup"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed, ColumnWidthFor("000.0 MB"));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ColumnWidthFor(T("Restaurar", "Restore")) + 12.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ColumnWidthFor(T("Borrar", "Delete")) + 12.0f);
        ImGui::TableHeadersRow();
        for (const auto& b : a.backups) {
            ImGui::PushID(b.name.c_str());
            table.Row();
            table.Cell();
            ImGui::TextUnformatted(b.name.c_str());
            table.Cell();
            ImGui::TextUnformatted(FormatSize(b.bytes).c_str());
            table.Cell();
            ImGui::BeginDisabled(locked);
            if (table.Action(T("Restaurar", "Restore"))) {
                a.pending_path = b.path;
                a.pending_action = 1;
            }
            ImGui::EndDisabled();
            table.Cell();
            if (table.Action(T("Borrar", "Delete"))) {
                a.pending_path = b.path;
                a.pending_action = 2;
            }
            ImGui::PopID();
        }
    }
    if (ImGui::Button(T("Abrir carpeta de copias", "Open backups folder"))) {
        std::error_code ec;
        fs::create_directories(BackupRoot(), ec);
        OpenInShell(BackupRoot());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar", "Refresh"))) a.saves_dirty = true;
    CloudSavesSection();
}

// ---------------------------------------------------------------------------------------------
// DLC (optional): the player's add-on packages in <port>\dlc, as .pkg files or extracted folders.
// CTR Nitro-Fueled's DLC hold only the license; scripts/content_profile.py reads the same folder
// and the runtime reports each one installed (BB_DLC, BB_DLC_DISABLED from launcher.ini).

fs::path DlcRoot() { return g_app.data / L"dlc"; }

uint32_t Be32(const char* p) {
    return uint32_t(uint8_t(p[0])) << 24 | uint32_t(uint8_t(p[1])) << 16 | uint32_t(uint8_t(p[2])) << 8 | uint32_t(uint8_t(p[3]));
}

/// param.sfo of a PS4 package (entry 0x1000), when it is stored in the clear.
std::map<std::string, std::string> ReadPkgSfo(const fs::path& pkg, std::string& error) {
    std::ifstream f(pkg, std::ios::binary);
    char header[0x20];
    if (!f.read(header, sizeof header) || std::memcmp(header, "\x7f" "CNT", 4) != 0) {
        error = T("no es un paquete de PS4", "not a PS4 package");
        return {};
    }
    const uint32_t count = Be32(header + 0x10), table = Be32(header + 0x18);
    if (count == 0 || count > 4096) {
        error = T("paquete dañado", "damaged package");
        return {};
    }
    std::string entries(size_t(count) * 32, '\0');
    f.seekg(table);
    if (!f.read(entries.data(), std::streamsize(entries.size()))) {
        error = T("paquete dañado", "damaged package");
        return {};
    }
    for (uint32_t i = 0; i < count; ++i) {
        const char* e = entries.data() + size_t(i) * 32;
        if (Be32(e) != 0x1000) continue;
        const uint32_t flags = Be32(e + 8), offset = Be32(e + 16), size = Be32(e + 20);
        if (flags & 0x80000000u) {
            error = T("param.sfo está cifrado", "param.sfo is encrypted");
            return {};
        }
        if (size > (1u << 20)) break;
        std::string data(size, '\0');
        f.seekg(offset);
        if (!f.read(data.data(), size)) break;
        return ParseSfo(data);
    }
    error = T("no tiene param.sfo", "no param.sfo");
    return {};
}

DlcInfo DescribeDlc(const fs::path& path) {
    DlcInfo d;
    d.path = path;
    d.file = PathText(path.filename());
    std::error_code ec;
    std::map<std::string, std::string> sfo;
    if (fs::is_directory(path, ec)) {
        if (!fs::is_regular_file(path / L"sce_sys" / L"param.sfo", ec)) {
            d.problem = T("la carpeta no tiene sce_sys\\param.sfo", "the folder has no sce_sys\\param.sfo");
            return d;
        }
        sfo = ReadSfo(path / L"sce_sys" / L"param.sfo");
    } else {
        sfo = ReadPkgSfo(path, d.problem);
        if (!d.problem.empty()) return d;
    }
    auto get = [&](const char* k) {
        const auto it = sfo.find(k);
        return it == sfo.end() ? std::string() : it->second;
    };
    const std::string content = get("CONTENT_ID");
    const size_t dash = content.rfind('-');
    d.label = dash == std::string::npos ? std::string() : content.substr(dash + 1);
    d.title = get("TITLE");
    if (!IsCtrTitle(get("TITLE_ID")) || get("CATEGORY") != "ac" || d.label.size() != 16) {
        d.problem = T("no es un DLC de CTR Nitro-Fueled (CUSA13795 / CUSA14876)",
                      "not a CTR Nitro-Fueled (CUSA13795 / CUSA14876) DLC");
        return d;
    }
    if (d.title.empty()) d.title = d.label;
    d.ok = true;
    return d;
}

std::vector<DlcInfo> ScanDlc() {
    std::vector<DlcInfo> list;
    std::error_code ec;
    for (fs::directory_iterator it(DlcRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool pkg = it->is_regular_file(e2) && (it->path().extension() == L".pkg" || it->path().extension() == L".PKG");
        if (pkg || it->is_directory(e2)) list.push_back(DescribeDlc(it->path()));
    }
    std::sort(list.begin(), list.end(), [](const DlcInfo& a, const DlcInfo& b) { return a.title < b.title; });
    return list;
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!Trim(item).empty()) out.push_back(Trim(item));
    return out;
}

std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
    return out;
}

void AddDlcPackages() {
    App& a = g_app;
    const auto files = PickFiles(a.hwnd, L"DLC (.pkg)", L"PS4 DLC (*.pkg)", L"*.pkg");
    if (files.empty()) return;
    std::error_code ec;
    fs::create_directories(DlcRoot(), ec);
    int added = 0;
    std::string problems;
    for (const auto& file : files) {
        const DlcInfo d = DescribeDlc(file);
        if (!d.ok) {
            problems += PathText(file.filename()) + ": " + d.problem + ". ";
            continue;
        }
        const fs::path target = DlcRoot() / file.filename();
        if (!CopyFile(file, target)) {
            problems += PathText(file.filename()) + ": error " + LastPlatformError() + ". ";
            continue;
        }
        ++added;
    }
    a.notice = std::string(T("DLC añadidos: ", "DLC added: ")) + std::to_string(added) + (problems.empty() ? "" : ". " + problems);
    a.dlc_dirty = true;
}

void DlcTab() {
    App& a = g_app;
    if (a.dlc_dirty) {
        a.dlc = ScanDlc();
        a.dlc_dirty = false;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Opcional: el juego funciona igual sin DLC. Los DLC de CTR Nitro-Fueled solo traen la licencia "
                             "(el contenido ya está en el juego); el port los marca como instalados al iniciar.",
                             "Optional: the game runs the same without DLC. CTR Nitro-Fueled's DLC only carry the license "
                             "(the content is already in the game); the port reports them installed at start."));
    ImGui::TextColored(kGrey, "%s", T("Usa tus propios paquetes DLC de PS4 (.pkg) de CUSA13795. Se copian a la carpeta dlc del "
                                      "port; tus archivos originales no cambian.",
                                      "Use your own PS4 DLC packages (.pkg) for CUSA13795. They are copied into the port's "
                                      "dlc folder; your original files are not changed."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    LauncherFlag("dlc", T("Activar los DLC", "Enable DLC"), true);
    const bool enabled = a.launcher.Flag("dlc", true);
    std::vector<std::string> disabled = SplitList(a.launcher.Get("dlc_disabled"));

    FormSection(T("DLC encontrados", "DLC found"));
    if (a.dlc.empty()) ImGui::TextColored(kGrey, "%s", T("No hay DLC en la carpeta dlc.", "No DLC in the dlc folder."));
    ImGui::BeginDisabled(!enabled);
    for (const auto& d : a.dlc) {
        ImGui::PushID(d.file.c_str());
        if (d.ok) {
            bool on = std::find(disabled.begin(), disabled.end(), d.label) == disabled.end();
            if (ImGui::Checkbox(d.title.c_str(), &on)) {
                disabled.erase(std::remove(disabled.begin(), disabled.end(), d.label), disabled.end());
                if (!on) disabled.push_back(d.label);
                a.launcher.Set("dlc_disabled", JoinList(disabled));
                SaveLauncher();
            }
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s", d.label.c_str());
        } else {
            ImGui::TextColored(kRed, "%s: %s", d.file.c_str(), d.problem.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(a.game_running);
        if (ImGui::SmallButton(T("Quitar", "Remove"))) {
            a.pending_path = d.path;
            a.pending_action = 3;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Button(T("Añadir DLC (.pkg)...", "Add DLC (.pkg)..."))) AddDlcPackages();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir la carpeta dlc", "Open the dlc folder"))) {
        std::error_code ec;
        fs::create_directories(DlcRoot(), ec);
        OpenInShell(DlcRoot());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar##dlc", "Refresh##dlc"))) a.dlc_dirty = true;
    ImGui::TextColored(kGrey, "%s", T("Los cambios se aplican la próxima vez que pulses Jugar.", "Changes apply the next time you press Play."));
}

/// Asked when PLAY is pressed online without a usable nickname: the game only starts once one
/// is chosen (or online is turned off for this launch).
void NickPopup() {
    App& a = g_app;
    if (a.ask_nick) {
        ImGui::OpenPopup("nick_required");
        a.ask_nick = false;
    }
    if (!ImGui::BeginPopupModal("nick_required", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ImGui::TextUnformatted(T("Elige tu apodo para jugar online", "Choose your nickname to play online"));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextColored(kGrey, "%s", T("Es tu nombre en el online y tu cuenta en el servidor: tus monedas y objetos del Pit Stop "
                                      "van ligados a él. De 3 a 16 letras, números, _ o -.",
                                      "It is your online name and your account on the server: your coins and Pit Stop items "
                                      "belong to it. 3 to 16 letters, digits, _ or -."));
    ImGui::PopTextWrapPos();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    NickInput(T("Apodo", "Nickname"));
    const char* problem = NickProblem(CurrentNick());
    if (problem) ImGui::TextColored(kYellow, "%s", problem);
    else NickAvailabilityLine();
    if (problem || NickTaken()) DiscordRecoverOffer();  // a reinstall: the nickname linked to Discord
    ImGui::BeginDisabled(problem != nullptr || NickTaken());
    if (ImGui::Button(T("Guardar y jugar", "Save and play"))) {
        SaveGameIni();
        ImGui::CloseCurrentPopup();
        StartGame();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Cancelar", "Cancel"))) {
        a.game.Load(GameIni());  // drop what was typed: every other setting is already saved
        SyncOnlineBuffers();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// The anonymous performance diagnostic (bbport.ini perf_reports, scripts/perf_report.py): asked
/// once, while it is neither 1 nor 0. A plain question, no alarm: what is sent and what is not.
const char* PerfReportsText() {
    return T("Al cerrar el juego, OCTR puede enviar un resumen anónimo de cómo le fue a tu PC: tarjeta "
             "gráfica, procesador y memoria, los ajustes gráficos y los FPS de cada pista. Nada personal: ni "
             "tu apodo, ni tus partidas, ni tus amigos, y tu IP no se guarda. Sirve para saber en qué PCs "
             "hay que mejorar el juego. Siempre queda una copia en la carpeta perf.",
             "When the game closes, OCTR can send an anonymous summary of how your PC did: graphics card, "
             "processor and memory, the graphics settings and the FPS of each track. Nothing personal: not "
             "your nickname, your races or your friends, and your IP is not stored. It shows which PCs the "
             "game needs to improve on. A copy always stays in the perf folder.");
}

void PerfReportsPopup() {
    App& a = g_app;
    // Shown once per launcher start, after any other question: on the first start of a new folder
    // the settings import opened in the same frame and replaced it, so it never came back.
    static bool shown = false;
    const std::string v = a.game.Get("perf_reports");
    if (!shown && v != "1" && v != "0" && !a.game_running && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        ImGui::OpenPopup("perf_reports");
    if (!ImGui::BeginPopupModal("perf_reports", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    shown = true;
    ImGui::TextUnformatted(T("¿Nos ayudas a mejorar OCTR?", "Help us improve OCTR?"));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextColored(kGrey, "%s", PerfReportsText());
    ImGui::TextColored(kGrey, "%s", T("Puedes cambiarlo cuando quieras en Ajustes.", "You can change it any time in Settings."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(T("Sí, enviar datos anónimos", "Yes, send anonymous data"))) {
        a.game.Set("perf_reports", "1");
        SaveGameIni();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("No, gracias", "No, thanks"))) {
        a.game.Set("perf_reports", "0");
        SaveGameIni();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// While run.py builds the native renderer's shaders (the first start with it, ~4 minutes).
void ShaderProgressPopup() {
    App& a = g_app;
    const int percent = a.shader_percent.load();
    if (percent >= 0 && !ImGui::IsPopupOpen("native_shaders")) ImGui::OpenPopup("native_shaders");
    if (!ImGui::BeginPopupModal("native_shaders", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    if (percent < 0 || !a.game_running) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    std::string stage;
    {
        std::scoped_lock lock{a.mutex};
        stage = a.shader_stage;
    }
    const char* step = stage == "extract"     ? T("Leyendo los shaders del juego...", "Reading the game's shaders...")
                       : stage == "export"    ? T("Preparando los programas...", "Preparing the programs...")
                       : stage == "translate" ? T("Traduciendo los shaders...", "Translating the shaders...")
                       : stage == "compile"   ? T("Compilando los shaders...", "Compiling the shaders...")
                                              : T("Terminando...", "Finishing...");
    ImGui::TextUnformatted(T("Compilando shaders del motor nativo", "Compiling the native engine's shaders"));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextColored(kGrey, "%s", T("Solo la primera vez: se preparan a partir de tu copia del juego y tardan unos "
                                      "minutos. El juego se abre solo al terminar.",
                                      "Only the first time: they are built from your copy of the game and take a few "
                                      "minutes. The game opens by itself when they are done."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    char overlay[16];
    std::snprintf(overlay, sizeof overlay, "%d %%", percent);
    ImGui::ProgressBar(percent / 100.0f, ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), overlay);
    ImGui::TextColored(kGrey, "%s", step);
    ImGui::Spacing();
    if (ImGui::Button(T("Cancelar", "Cancel"))) {
        StopGame();
        a.shader_percent = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// A Discord invite for a server other than ours: switching is the player's call.
void JoinServerPopup() {
    App& a = g_app;
    if (a.ask_join_server) {
        ImGui::OpenPopup("join_server");
        a.ask_join_server = false;
    }
    if (!ImGui::BeginPopupModal("join_server", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ImGui::Text(T("Invitación de %s", "Invite from %s"), a.join_nick.c_str());
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    const std::string mine = Trim(a.game.Get("online_server", "octr.ryu7w7.xyz"));
    ImGui::TextColored(kYellow, T("Es para el servidor %s y tú usas %s.", "It is for the server %s and you use %s."),
                       a.join_server.c_str(), mine.c_str());
    ImGui::TextColored(kGrey, "%s", T("Cámbialo solo si confías en ese servidor: verá tu apodo y tu partida online. "
                                      "Puedes volver al tuyo en la pestaña Online.",
                                      "Only switch if you trust that server: it will see your nickname and your online "
                                      "play. You can switch back in the Online tab."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(T("Cambiar de servidor y unirme", "Switch server and join"))) {
        AcceptDiscordJoin(a.join_server, a.join_nick);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void SaveConfirmations() {
    App& a = g_app;
    if (a.pending_action) {
        ImGui::OpenPopup("save_confirm");
        a.confirm_action = a.pending_action;
        a.pending_action = 0;
    }
    if (ImGui::BeginPopupModal("save_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        const std::string name = PathText(a.pending_path.filename());
        if (a.confirm_action == 1) {
            ImGui::Text(T("¿Restaurar %s?", "Restore %s?"), name.c_str());
            ImGui::TextColored(kGrey, "%s", T("Tus partidas actuales se guardan antes en una copia automática.",
                                              "Your current saves are backed up automatically first."));
        } else if (a.confirm_action == 3) {
            ImGui::Text(T("¿Quitar %s de la carpeta dlc?", "Remove %s from the dlc folder?"), name.c_str());
            ImGui::TextColored(kGrey, "%s", T("Solo se borra la copia del port.", "Only the port's copy is deleted."));
        } else {
            ImGui::Text(T("¿Borrar la copia %s? No se puede deshacer.", "Delete the backup %s? This cannot be undone."), name.c_str());
        }
        if (ImGui::Button(a.confirm_action == 1 ? T("Restaurar", "Restore") : a.confirm_action == 3 ? T("Quitar", "Remove") : T("Borrar", "Delete"))) {
            std::string error;
            if (a.confirm_action == 1) {
                error = RestoreBackup(a.pending_path);
                if (error.empty()) a.notice = std::string(T("Restaurada: ", "Restored: ")) + name;
            } else {
                std::error_code ec;
                fs::remove_all(a.pending_path, ec);
                if (ec) error = name + ": " + ec.message();
            }
            if (!error.empty()) a.notice = error;
            a.saves_dirty = true;
            a.dlc_dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------
// Keyboard tab: bbport.ini key.<input>=<SDL key names> (src/runtime_pad.c reads them at start;
// a missing line keeps the runtime's default). Keys are captured by their scan code (the key's
// place on the keyboard, as SDL's scancodes), so the binding holds whatever the layout.

struct KeyInput {
    const char* id;      // runtime_pad.c input name
    const char* es;
    const char* en;
    const char* fallback;  // runtime_pad.c bind_defaults()
};
constexpr KeyInput kKeyInputs[] = {
    {"move_up", "Stick izquierdo arriba", "Left stick up", "W"},
    {"move_down", "Stick izquierdo abajo", "Left stick down", "S"},
    {"move_left", "Stick izquierdo izquierda (girar)", "Left stick left (steer)", "A"},
    {"move_right", "Stick izquierdo derecha (girar)", "Left stick right (steer)", "D"},
    {"cross", "X (acelerar)", "Cross (accelerate)", "Space"},
    {"square", "Cuadrado (frenar)", "Square (brake)", "E"},
    {"circle", "Círculo", "Circle", "Left Shift"},
    {"triangle", "Triángulo", "Triangle", "Q"},
    {"r1", "R1", "R1", "3"},
    {"l1", "L1", "L1", "1"},
    {"r2", "R2", "R2", "F"},
    {"l2", "L2", "L2", "R"},
    {"l3", "L3", "L3", "Z"},
    {"r3", "R3", "R3", "C"},
    {"options", "Options (pausa)", "Options (pause)", "Return"},
    {"touchpad", "Panel táctil", "Touchpad", "Tab"},
    {"touchpad_right", "Panel táctil (derecha)", "Touchpad (right)", "Backspace"},
    {"up", "Cruceta arriba", "D-pad Up", "I"},
    {"down", "Cruceta abajo", "D-pad Down", "K"},
    {"left", "Cruceta izquierda", "D-pad Left", "J"},
    {"right", "Cruceta derecha", "D-pad Right", "L"},
    {"look_up", "Stick derecho arriba", "Right stick up", "Up"},
    {"look_down", "Stick derecho abajo", "Right stick down", "Down"},
    {"look_left", "Stick derecho izquierda", "Right stick left", "Left"},
    {"look_right", "Stick derecho derecha", "Right stick right", "Right"},
};

// Scan code set 1 (WM_KEYDOWN lParam bits 16-23, + 0x100 for E0-extended) -> SDL scancode name.
const char* SdlKeyName(unsigned code);

// Mouse buttons and wheel steps as captured keys: kMouseCode + SDL's button number (wheel 6 and 7),
// with the names src/runtime_pad.c and tools/gpu_capabilities.c --read-input use.
constexpr unsigned kMouseCode = 0x1000;
const char* MouseKeyName(unsigned code) {
    static const char* const names[] = {"Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1",
                                        "Mouse X2",   "Wheel Up",     "Wheel Down"};
    return code > kMouseCode && code <= kMouseCode + 7 ? names[code - kMouseCode - 1] : nullptr;
}

// How a key name reads in the launcher.
std::string KeyLabel(const std::string& name) {
    static const std::map<std::string, std::pair<const char*, const char*>> labels = {
        {"Space", {"Espacio", "Space"}}, {"Return", {"Enter", "Enter"}},
        {"Backspace", {"Retroceso", "Backspace"}}, {"Left Shift", {"Mayús izq.", "Left Shift"}},
        {"Right Shift", {"Mayús der.", "Right Shift"}}, {"Left Ctrl", {"Ctrl izq.", "Left Ctrl"}},
        {"Right Ctrl", {"Ctrl der.", "Right Ctrl"}}, {"Left Alt", {"Alt", "Left Alt"}},
        {"Right Alt", {"Alt Gr", "Right Alt"}}, {"Up", {"Flecha arriba", "Up arrow"}},
        {"Down", {"Flecha abajo", "Down arrow"}}, {"Left", {"Flecha izquierda", "Left arrow"}},
        {"Right", {"Flecha derecha", "Right arrow"}}, {"CapsLock", {"Bloq Mayús", "Caps Lock"}},
        {"Delete", {"Supr", "Delete"}}, {"Home", {"Inicio", "Home"}}, {"End", {"Fin", "End"}},
        {"PageUp", {"Re Pág", "Page Up"}}, {"PageDown", {"Av Pág", "Page Down"}},
        {"`", {"Tecla a la izquierda del 1", "` key"}},
        {"NonUSBackslash", {"Tecla < >", "< > key"}}, {"Keypad Enter", {"Enter (teclado num.)", "Keypad Enter"}},
        {"Mouse Left", {"Clic izquierdo", "Left click"}}, {"Mouse Right", {"Clic derecho", "Right click"}},
        {"Mouse Middle", {"Clic central (rueda)", "Middle click (wheel)"}},
        {"Mouse X1", {"Botón lateral 1 del ratón", "Mouse side button 1"}},
        {"Mouse X2", {"Botón lateral 2 del ratón", "Mouse side button 2"}},
        {"Wheel Up", {"Rueda arriba", "Wheel up"}}, {"Wheel Down", {"Rueda abajo", "Wheel down"}},
    };
    const auto it = labels.find(name);
    if (it != labels.end()) return T(it->second.first, it->second.second);
    if (name.rfind("Keypad ", 0) == 0) return std::string(T("Num ", "Num ")) + name.substr(7);
    return name;
}

std::atomic<int> g_key_capture{-1};      // the input waiting for a key, -1 none
std::atomic<unsigned> g_key_captured{0};  // scan code + 1 (0: nothing yet)
bool g_key_replace = false;                 // the captured key replaces the default binding

std::vector<std::string> SplitKeys(const std::string& value) {
    std::vector<std::string> keys;
    std::stringstream ss(value);
    std::string key;
    while (std::getline(ss, key, ',')) {
        key = Trim(key);
        if (!key.empty()) keys.push_back(key);
    }
    return keys;
}


// ---- The keyboard picture (Keyboard page) ----
// A full-size keyboard's main block and arrows, each bound key coloured and labelled with what it
// does (bbport.ini key.*); the rest of the bindings (keypad, mouse, F keys) are listed under it.

int g_key_hover_input = -1;  // the table row under the mouse (last frame): its keys light up

/// A keyboard input's job, as short as fits on a key cap.
std::string KeyRoleShort(const std::string& id) {
    static const std::map<std::string, std::pair<const char*, const char*>> roles = {
        {"move_up", {"Stick ↑", "Stick ↑"}},       {"move_down", {"Stick ↓", "Stick ↓"}},
        {"move_left", {"Girar ←", "Steer ←"}},     {"move_right", {"Girar →", "Steer →"}},
        {"cross", {"Acelerar", "Gas"}},            {"square", {"Frenar", "Brake"}},
        {"circle", {"Círculo", "Circle"}},         {"triangle", {"Triáng.", "Triangle"}},
        {"r1", {"Salto R1", "Hop R1"}},            {"l1", {"Salto L1", "Hop L1"}},
        {"r2", {"Mirar atrás", "Look back"}},      {"l2", {"Objeto", "Item"}},
        {"l3", {"L3", "L3"}},                      {"r3", {"R3", "R3"}},
        {"options", {"Pausa", "Pause"}},           {"touchpad", {"Panel", "Pad"}},
        {"touchpad_right", {"Panel der.", "Pad R"}},
        {"up", {"Cruceta ↑", "D-pad ↑"}},          {"down", {"Cruceta ↓", "D-pad ↓"}},
        {"left", {"Cruceta ←", "D-pad ←"}},        {"right", {"Cruceta →", "D-pad →"}},
        {"look_up", {"Stick D ↑", "R stick ↑"}},   {"look_down", {"Stick D ↓", "R stick ↓"}},
        {"look_left", {"Stick D ←", "R stick ←"}}, {"look_right", {"Stick D →", "R stick →"}},
    };
    const auto it = roles.find(id);
    return it == roles.end() ? id : std::string(T(it->second.first, it->second.second));
}

struct KeyCap {
    const char* sdl;  // nullptr: a gap
    const char* es;   // the cap's text (nullptr: the SDL name)
    const char* en;
    float w;          // in key units
};

void KeyboardDiagram() {
    App& a = g_app;
    std::map<std::string, std::vector<int>> by_key;
    for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
        const std::string key = std::string("key.") + kKeyInputs[i].id;
        const bool custom = a.game.values.count(key) > 0;
        for (const auto& k : SplitKeys(custom ? a.game.Get(key) : kKeyInputs[i].fallback)) by_key[k].push_back(i);
    }
    static const std::vector<std::vector<KeyCap>> rows = {
        {{"`", nullptr, nullptr, 1}, {"1", nullptr, nullptr, 1}, {"2", nullptr, nullptr, 1}, {"3", nullptr, nullptr, 1},
         {"4", nullptr, nullptr, 1}, {"5", nullptr, nullptr, 1}, {"6", nullptr, nullptr, 1}, {"7", nullptr, nullptr, 1},
         {"8", nullptr, nullptr, 1}, {"9", nullptr, nullptr, 1}, {"0", nullptr, nullptr, 1}, {"-", nullptr, nullptr, 1},
         {"=", nullptr, nullptr, 1}, {"Backspace", "Retroceso", "Backspace", 2},
         {nullptr, nullptr, nullptr, 0.5f}, {"Insert", "Ins (menú)", "Ins (menu)", 1}, {"Home", "Inicio", "Home", 1},
         {"PageUp", "RePág", "PgUp", 1}},
        {{"Tab", nullptr, nullptr, 1.5f}, {"Q", nullptr, nullptr, 1}, {"W", nullptr, nullptr, 1}, {"E", nullptr, nullptr, 1},
         {"R", nullptr, nullptr, 1}, {"T", nullptr, nullptr, 1}, {"Y", nullptr, nullptr, 1}, {"U", nullptr, nullptr, 1},
         {"I", nullptr, nullptr, 1}, {"O", nullptr, nullptr, 1}, {"P", nullptr, nullptr, 1}, {"[", nullptr, nullptr, 1},
         {"]", nullptr, nullptr, 1}, {"\\", nullptr, nullptr, 1.5f},
         {nullptr, nullptr, nullptr, 0.5f}, {"Delete", "Supr", "Del", 1}, {"End", "Fin", "End", 1},
         {"PageDown", "AvPág", "PgDn", 1}},
        {{"CapsLock", "Bloq Mayús", "Caps", 1.75f}, {"A", nullptr, nullptr, 1}, {"S", nullptr, nullptr, 1},
         {"D", nullptr, nullptr, 1}, {"F", nullptr, nullptr, 1}, {"G", nullptr, nullptr, 1}, {"H", nullptr, nullptr, 1},
         {"J", nullptr, nullptr, 1}, {"K", nullptr, nullptr, 1}, {"L", nullptr, nullptr, 1}, {";", nullptr, nullptr, 1},
         {"'", nullptr, nullptr, 1}, {"Return", "Enter", "Enter", 2.25f}},
        {{"Left Shift", "Mayús", "Shift", 2.25f}, {"Z", nullptr, nullptr, 1}, {"X", nullptr, nullptr, 1},
         {"C", nullptr, nullptr, 1}, {"V", nullptr, nullptr, 1}, {"B", nullptr, nullptr, 1}, {"N", nullptr, nullptr, 1},
         {"M", nullptr, nullptr, 1}, {",", nullptr, nullptr, 1}, {".", nullptr, nullptr, 1}, {"/", nullptr, nullptr, 1},
         {"Right Shift", "Mayús", "Shift", 2.75f},
         {nullptr, nullptr, nullptr, 1.5f}, {"Up", "↑", "↑", 1}},
        {{"Left Ctrl", "Ctrl", "Ctrl", 1.5f}, {nullptr, nullptr, nullptr, 1.25f}, {"Left Alt", "Alt", "Alt", 1.5f},
         {"Space", "Espacio", "Space", 6.5f}, {"Right Alt", "Alt Gr", "Alt Gr", 1.5f}, {nullptr, nullptr, nullptr, 1.25f},
         {"Right Ctrl", "Ctrl", "Ctrl", 1.5f},
         {nullptr, nullptr, nullptr, 0.5f}, {"Left", "←", "←", 1}, {"Down", "↓", "↓", 1}, {"Right", "→", "→", 1}},
    };
    const float units = 18.5f;
    const float width = std::max(ImGui::GetContentRegionAvail().x, 560.0f);
    const float u = std::min(width / units, 58.0f * g_ui_scale);
    const float gap = std::max(2.0f, u * 0.07f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float x_start = o.x + (width - u * units) * 0.5f;
    ImGui::InvisibleButton("key_picture", ImVec2(width, u * 5.0f));
    const bool picture_hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    std::vector<std::string> lit;
    if (g_key_hover_input >= 0 && g_key_hover_input < int(std::size(kKeyInputs))) {
        const KeyInput& in = kKeyInputs[g_key_hover_input];
        const std::string key = std::string("key.") + in.id;
        lit = SplitKeys(a.game.values.count(key) ? a.game.Get(key) : in.fallback);
    }
    ImFont* font = ImGui::GetFont();
    const float cap_size = std::min(ImGui::GetFontSize() * 0.78f, u * 0.32f);
    const float role_size = std::min(ImGui::GetFontSize() * 0.66f, u * 0.27f);
    std::string hovered;
    for (size_t r = 0; r < rows.size(); ++r) {
        float x = x_start;
        const float y = o.y + float(r) * u;
        for (const KeyCap& k : rows[r]) {
            const float w = k.w * u;
            if (!k.sdl) {
                x += w;
                continue;
            }
            const ImVec2 p0{x + gap * 0.5f, y + gap * 0.5f}, p1{x + w - gap * 0.5f, y + u - gap * 0.5f};
            x += w;
            const std::string sdl = k.sdl;
            const auto it = by_key.find(sdl);
            const bool bound = it != by_key.end();
            const bool reserved = sdl == "Insert";  // the in-game menu
            const bool hover = picture_hovered && mouse.x >= p0.x && mouse.x < p1.x && mouse.y >= p0.y && mouse.y < p1.y;
            if (hover) hovered = sdl;
            const bool hot = hover || std::find(lit.begin(), lit.end(), sdl) != lit.end();
            const ImU32 fill = bound ? IM_COL32(22, 118, 178, 255) : reserved ? IM_COL32(70, 52, 30, 255) : IM_COL32(10, 42, 80, 255);
            d->AddRectFilled(ImVec2(p0.x, p0.y + 2), ImVec2(p1.x, p1.y + 2), IM_COL32(2, 14, 32, 200), u * 0.12f);  // shadow
            d->AddRectFilled(p0, p1, fill, u * 0.12f);
            d->AddRect(p0, p1, hot ? IM_COL32(255, 205, 90, 255) : bound ? IM_COL32(110, 225, 255, 230) : IM_COL32(60, 110, 160, 160),
                       u * 0.12f, 0, hot ? 2.5f : 1.0f);
            const char* cap = k.es ? T(k.es, k.en) : k.sdl;
            d->PushClipRect(p0, p1, true);
            d->AddText(font, cap_size, ImVec2(p0.x + u * 0.1f, p0.y + u * 0.06f),
                       bound ? IM_COL32(240, 250, 255, 255) : IM_COL32(160, 190, 220, 220), cap);
            if (bound) {
                std::string role;
                for (const int i : it->second) role += (role.empty() ? "" : " / ") + KeyRoleShort(kKeyInputs[i].id);
                const ImVec2 t = font->CalcTextSizeA(role_size, FLT_MAX, 0.0f, role.c_str());
                d->AddText(font, role_size, ImVec2(std::max(p0.x + 3.0f, (p0.x + p1.x - t.x) * 0.5f), p1.y - role_size - u * 0.08f),
                           IM_COL32(255, 232, 150, 255), role.c_str());
            }
            d->PopClipRect();
        }
    }
    if (!hovered.empty()) {
        std::string what;
        if (const auto it = by_key.find(hovered); it != by_key.end())
            for (const int i : it->second) what += (what.empty() ? "" : "\n") + std::string(T(kKeyInputs[i].es, kKeyInputs[i].en));
        if (hovered == "Insert") what = T("Abre el menú del juego (no se puede asignar)", "Opens the in-game menu (cannot be bound)");
        ImGui::SetTooltip("%s\n%s", KeyLabel(hovered).c_str(), what.empty() ? T("Sin uso en el juego", "Not used in the game") : what.c_str());
    }

    // Bindings the picture has no key for: keypad, mouse, F keys and the like.
    std::set<std::string> drawn;
    for (const auto& row : rows)
        for (const KeyCap& k : row)
            if (k.sdl) drawn.insert(k.sdl);
    std::string others;
    for (const auto& [key, inputs] : by_key) {
        if (drawn.count(key)) continue;
        for (const int i : inputs)
            others += (others.empty() ? "" : "   ·   ") + KeyLabel(key) + ": " + KeyRoleShort(kKeyInputs[i].id);
    }
    if (!others.empty()) FormNote((std::string(T("Fuera del dibujo:  ", "Not on the picture:  ")) + others).c_str());
    FormNote(T("Azul: tecla con uso en el juego. Pasa el ratón por una tecla o una fila para verla.",
               "Blue: a key the game uses. Hover a key or a row to see it."));
}

/// The width of a table row's Change / Clear / Default buttons in the current font (the CTR one is
/// wider than a fixed size allowed: the last button was cut off).
float RowButtonsWidth() {
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::CalcTextSize(T("Cambiar", "Change")).x + ImGui::CalcTextSize(T("Quitar", "Clear")).x +
           ImGui::CalcTextSize(T("Original", "Default")).x + style.FramePadding.x * 6.0f + style.ItemSpacing.x * 2.0f + 4.0f;
}

void KeyboardTab() {    App& a = g_app;
    FormBegin();
    FormSection(T("Pantalla dividida", "Split screen"));
    KeyboardPlayerSetting();
    FormEnd();
    FormSection(T("Controles", "Controls"));
    FormNote(T("El teclado funciona a la vez que el mando. Pulsa \"Cambiar\" y luego la tecla; cada acción admite "
               "hasta 4 teclas. Vale desde el próximo arranque. Insert abre el menú del juego y Esc cancela, por eso "
               "no se pueden asignar.",
               "The keyboard works together with the controller. Press \"Change\", then the key; each action takes up "
               "to 4 keys. Used from the next start. Insert opens the in-game menu and Esc cancels, so they cannot be "
               "bound."));
    KeyboardDiagram();
    ImGui::Spacing();
    const int capturing = g_key_capture;
    if (capturing >= 0) {
        const unsigned got = g_key_captured.exchange(0);
        if (got) {
            const unsigned code = got - 1;
            if (code == kCancelKey) {
                g_key_capture = -1;  // Esc: cancel
            } else if (const char* name = code > kMouseCode ? MouseKeyName(code) : SdlKeyName(code);
                       name && code != kMenuKey) {
                const KeyInput& in = kKeyInputs[capturing];
                const std::string key = std::string("key.") + in.id;
                auto keys = g_key_replace ? std::vector<std::string>{} : SplitKeys(a.game.Get(key));
                if (std::find(keys.begin(), keys.end(), name) == keys.end()) {
                    if (keys.size() >= 4) keys.erase(keys.begin());
                    keys.push_back(name);
                }
                std::string value;
                for (const auto& k : keys) value += (value.empty() ? "" : ",") + k;
                a.game.Set(key, value);
                SaveGameIni();
                g_key_capture = -1;
            } else {
                a.notice = T("Esa tecla no se puede asignar.", "That key cannot be bound.");
                g_key_capture = -1;
            }
        }
    }

    int key_row = -1;
    if (LauncherUI::Table table{"keys", 3}) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Teclas", "Keys"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, RowButtonsWidth());
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
            const KeyInput& in = kKeyInputs[i];
            const std::string key = std::string("key.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto keys = SplitKeys(custom ? a.game.Get(key) : in.fallback);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float row_top = ImGui::GetCursorScreenPos().y;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));
            table.Cell();
            if (capturing == i) {
                ImGui::TextColored(kYellow, "%s", T("Pulsa una tecla o un botón del ratón... (Esc cancela)",
                                                    "Press a key or a mouse button... (Esc cancels)"));
            } else if (keys.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin tecla)", "(none)"));
            } else {
                std::string text;
                for (const auto& k : keys) text += (text.empty() ? "" : "  /  ") + KeyLabel(k);
                if (custom) ImGui::TextUnformatted(text.c_str());
                else ImGui::TextColored(kGrey, "%s", text.c_str());
            }
            table.Cell();
            ImGui::BeginDisabled(capturing >= 0);
            if (table.Action(T("Cambiar", "Change"))) {
                // A key replaces the default binding; on a custom one it is added (up to 4).
                g_key_replace = !custom;
                g_key_captured = 0;
                g_key_capture = i;
            }
            if (table.Action(T("Quitar", "Clear"))) {
                a.game.Set(key, "");
                SaveGameIni();
            }
            ImGui::BeginDisabled(!custom);
            if (table.Action(T("Original", "Default"))) {
                a.game.Erase(key);
                SaveGameIni();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (ImGui::IsWindowHovered() && ImGui::GetIO().MousePos.y >= row_top &&
                ImGui::GetIO().MousePos.y < ImGui::GetItemRectMax().y + ImGui::GetStyle().CellPadding.y)
                key_row = i;
            ImGui::PopID();
        }
    }
    g_key_hover_input = key_row;
    ImGui::Spacing();
    ImGui::BeginDisabled(capturing >= 0);
    if (ImGui::Button(T("Restaurar todas las teclas", "Restore all keys"))) {
        for (const auto& in : kKeyInputs) a.game.Erase(std::string("key.") + in.id);
        SaveGameIni();
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kGrey, "%s", T("Gris: tecla original. Las teclas van por su posición en el teclado.",
                                      "Grey: default key. Keys go by their place on the keyboard."));
}

// ---------------------------------------------------------------------------------------------
// Gamepad tab: bbport.ini pad.<input>=<SDL gamepad button names> (src/runtime_pad.c reads them).

struct PadInput {
    const char* id;
    const char* es;
    const char* en;
    const char* fallback;
};

constexpr PadInput kPadInputs[] = {
    {"cross", "X (Acelerar / Seleccionar)", "Cross (Accelerate / Select)", "a"},
    {"square", "Cuadrado (Frenar / Marcha atrás)", "Square (Brake / Reverse)", "x"},
    {"circle", "Círculo", "Circle", "b"},
    {"triangle", "Triángulo", "Triangle", "y"},
    {"l1", "L1 / LB (Salto / Derrape)", "L1 / LB (Hop / Drift)", "leftshoulder"},
    {"r1", "R1 / RB (Salto / Derrape)", "R1 / RB (Hop / Drift)", "rightshoulder"},
    {"l2", "L2 / LT (Usar objeto / Ítem)", "L2 / LT (Use item)", "lefttrigger"},
    {"r2", "R2 / RT (Mirar atrás)", "R2 / RT (Look back)", "righttrigger"},
    {"l3", "L3 (Click stick izq.)", "L3 Click (Left Stick)", "leftstick"},
    {"r3", "R3 (Click stick der.)", "R3 Click (Right Stick)", "rightstick"},
    {"options", "Options / Start (Pausa)", "Options / Start (Pause)", "start"},
    {"touchpad", "Touchpad / Back / Select", "Touchpad / Back / Select", "back,touchpad"},
    {"up", "Cruceta arriba", "D-pad Up", "dpup"},
    {"down", "Cruceta abajo", "D-pad Down", "dpdown"},
    {"left", "Cruceta izquierda", "D-pad Left", "dpleft"},
    {"right", "Cruceta derecha", "D-pad Right", "dpright"},
};

std::string PadButtonLabel(const std::string& name) {
    static const std::map<std::string, std::pair<const char*, const char*>> labels = {
        {"a", {"Cruz / A (Sur)", "Cross / A (South)"}},
        {"b", {"Círculo / B (Este)", "Circle / B (East)"}},
        {"x", {"Cuadrado / X (Oeste)", "Square / X (West)"}},
        {"y", {"Triángulo / Y (Norte)", "Triangle / Y (North)"}},
        {"leftshoulder", {"L1 / LB", "L1 / LB"}},
        {"rightshoulder", {"R1 / RB", "R1 / RB"}},
        {"lefttrigger", {"Gatillo L2 / LT", "Trigger L2 / LT"}},
        {"righttrigger", {"Gatillo R2 / RT", "Trigger R2 / RT"}},
        {"leftstick", {"Click L3 (Stick Izq.)", "L3 Click (Left Stick)"}},
        {"rightstick", {"Click R3 (Stick Der.)", "R3 Click (Right Stick)"}},
        {"start", {"Options / Start", "Options / Start"}},
        {"back", {"Share / Back / Select", "Share / Back / Select"}},
        {"touchpad", {"Touchpad", "Touchpad"}},
        {"dpup", {"Cruceta Arriba", "D-pad Up"}},
        {"dpdown", {"Cruceta Abajo", "D-pad Down"}},
        {"dpleft", {"Cruceta Izquierda", "D-pad Left"}},
        {"dpright", {"Cruceta Derecha", "D-pad Right"}},
        {"guide", {"Botón Guía / PS", "Guide / PS"}},
        {"misc1", {"Captura / Misc", "Capture / Misc"}},
        {"paddle1", {"Palanca 1", "Paddle 1"}},
        {"paddle2", {"Palanca 2", "Paddle 2"}},
        {"paddle3", {"Palanca 3", "Paddle 3"}},
        {"paddle4", {"Palanca 4", "Paddle 4"}},
    };
    const auto it = labels.find(name);
    return it != labels.end() ? T(it->second.first, it->second.second) : name;
}

const char* PollGamepadButton();

// ---- The controller, read through SDL while the Gamepad page is open ----
// bb-gpu-capabilities --watch-pad prints "pad <button>" for every press on any controller SDL
// knows: PlayStation pads too, which XInput (PollGamepadButton) does not see. The page uses it to
// assign buttons and to move through the list with the controller.
#ifdef _WIN32
struct PadWatchState {
    std::mutex mutex;
    std::deque<std::string> presses;
    HANDLE process = nullptr;
    std::thread reader;
    std::atomic<bool> running{false};
};
PadWatchState g_pad_watch;

void PadWatchStop() {
    PadWatchState& w = g_pad_watch;
    if (w.process) {
        TerminateProcess(w.process, 0);
        CloseHandle(w.process);
        w.process = nullptr;
    }
    if (w.reader.joinable()) w.reader.join();
    w.running = false;
    std::scoped_lock lock{w.mutex};
    w.presses.clear();
}

void PadWatchStart() {
    PadWatchState& w = g_pad_watch;
    if (w.running) return;
    PadWatchStop();  // a watcher that ended (its 10 minutes): start another
    const fs::path tool = g_app.bin / L"bb-gpu-capabilities.exe";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    // Only its output pipe is inherited (as RunCaptured: nothing of the launcher's stays open in it).
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[1] = {write};
    const bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit,
                                                    nullptr, nullptr);
    STARTUPINFOEXW six{};
    six.StartupInfo.cb = sizeof six;
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdOutput = write;
    six.StartupInfo.hStdError = write;
    six.lpAttributeList = attrs_ok ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = Quote(tool.wstring()) + L" --watch-pad";
    const BOOL ok = CreateProcessW(tool.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                   g_app.bin.c_str(), &six.StartupInfo, &pi);
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    CloseHandle(write);
    if (!ok) {
        CloseHandle(read);
        return;
    }
    CloseHandle(pi.hThread);
    w.process = pi.hProcess;
    w.running = true;
    w.reader = std::thread([read] {
        PadWatchState& w = g_pad_watch;
        std::string pending;
        char buf[512];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) {
            pending.append(buf, n);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                const std::string line = Trim(pending.substr(0, nl));
                pending.erase(0, nl + 1);
                // presses count only while the launcher is in front (not while playing)
                if (line.rfind("pad ", 0) == 0 && GetForegroundWindow() == g_app.hwnd) {
                    std::scoped_lock lock{w.mutex};
                    if (w.presses.size() < 16) w.presses.push_back(line.substr(4));
                }
            }
        }
        CloseHandle(read);
        w.running = false;
    });
}

bool PadWatchTake(std::string& button) {
    std::scoped_lock lock{g_pad_watch.mutex};
    if (g_pad_watch.presses.empty()) return false;
    button = std::move(g_pad_watch.presses.front());
    g_pad_watch.presses.pop_front();
    return true;
}
bool PadWatchRunning() { return g_pad_watch.running; }
#else
void PadWatchStop() {}
void PadWatchStart() {}
bool PadWatchRunning() { return true; }
bool PadWatchTake(std::string& button) {
    const char* pressed = PollGamepadButton();
    if (!pressed) return false;
    button = pressed;
    return true;
}
#endif

int g_pad_cursor = -1;           // the row the controller is on (-1: none yet)
bool g_pad_cursor_moved = false;  // scroll it into view this frame

std::atomic<int> g_pad_capture{-1};
bool g_pad_waiting_release = false;

// ---- The controller picture (Gamepad page) ----
// An Xbox or PlayStation pad drawn with ImDrawList, each physical button labelled with what it
// does in the game under the current bbport.ini pad.* mapping. While a row waits for a button
// ("Change"), clicking a button on the picture assigns it.

int g_pad_hover_input = -1;  // the table row under the mouse (last frame): its buttons light up
std::string g_pad_menu_button;  // the button whose menu is open (a click on the picture)

/// What a game input does, short; nullptr when its button name says it all.
const char* InputRole(const std::string& id) {
    static const std::map<std::string, std::pair<const char*, const char*>> roles = {
        {"cross", {"Acelerar", "Accelerate"}},       {"square", {"Frenar", "Brake"}},
        {"l1", {"Salto / Derrape", "Hop / Drift"}},  {"r1", {"Salto / Derrape", "Hop / Drift"}},
        {"l2", {"Usar objeto", "Use item"}},         {"r2", {"Mirar atrás", "Look back"}},
        {"options", {"Pausa", "Pause"}},
    };
    const auto it = roles.find(id);
    return it == roles.end() ? nullptr : T(it->second.first, it->second.second);
}

/// The game's name for an input's button, in the pad's style.
std::string GameButtonName(const std::string& id, bool xbox) {
    static const std::map<std::string, std::array<const char*, 3>> names = {  // xbox, es, en
        {"cross", {"A", "Cruz", "Cross"}},          {"circle", {"B", "Círculo", "Circle"}},
        {"square", {"X", "Cuadrado", "Square"}},    {"triangle", {"Y", "Triángulo", "Triangle"}},
        {"l1", {"LB", "L1", "L1"}},                 {"r1", {"RB", "R1", "R1"}},
        {"l2", {"LT", "L2", "L2"}},                 {"r2", {"RT", "R2", "R2"}},
        {"l3", {"LS", "L3", "L3"}},                 {"r3", {"RS", "R3", "R3"}},
        {"options", {"Menu", "Options", "Options"}}, {"touchpad", {"View", "Touchpad", "Touchpad"}},
        {"up", {nullptr, "Cruceta ↑", "D-pad ↑"}},  {"down", {nullptr, "Cruceta ↓", "D-pad ↓"}},
        {"left", {nullptr, "Cruceta ←", "D-pad ←"}}, {"right", {nullptr, "Cruceta →", "D-pad →"}},
    };
    const auto it = names.find(id);
    if (it == names.end()) return id;
    if (xbox && it->second[0]) return it->second[0];
    return T(it->second[1], it->second[2]);
}

/// A physical button's name on this pad.
std::string PhysicalButtonName(const std::string& sdl, bool xbox) {
    static const std::map<std::string, std::array<const char*, 3>> names = {  // xbox, es, en (PlayStation)
        {"a", {"A", "Cruz", "Cross"}},            {"b", {"B", "Círculo", "Circle"}},
        {"x", {"X", "Cuadrado", "Square"}},       {"y", {"Y", "Triángulo", "Triangle"}},
        {"leftshoulder", {"LB", "L1", "L1"}},     {"rightshoulder", {"RB", "R1", "R1"}},
        {"lefttrigger", {"LT", "L2", "L2"}},      {"righttrigger", {"RT", "R2", "R2"}},
        {"leftstick", {"LS", "L3", "L3"}},        {"rightstick", {"RS", "R3", "R3"}},
        {"start", {"Menu", "Options", "Options"}}, {"back", {"View", "Share", "Share"}},
        {"touchpad", {"Touchpad", "Touchpad", "Touchpad"}},
        {"dpup", {nullptr, "↑", "↑"}},  {"dpdown", {nullptr, "↓", "↓"}},
        {"dpleft", {nullptr, "←", "←"}}, {"dpright", {nullptr, "→", "→"}},
    };
    const auto it = names.find(sdl);
    if (it == names.end()) return sdl;
    if (xbox && it->second[0]) return it->second[0];
    return T(it->second[1], it->second[2]);
}

/// Xbox or PlayStation: by the chosen (or first) controller's name, else by the button prompts.
bool PadLooksXbox() {
    App& a = g_app;
    std::string name;
    {
        std::scoped_lock lock{a.mutex};
        const std::string chosen = a.launcher.Get("gamepad");
        for (const auto& [guid, n] : a.gamepads)
            if (chosen.empty() ? name.empty() : guid == chosen) name = n;
    }
    name = Lower(name);
    for (const char* ps : {"dualsense", "dualshock", "playstation", "ps4", "ps5", "wireless controller"})
        if (name.find(ps) != std::string::npos) return false;
    for (const char* xb : {"xbox", "xinput", "x-box"})
        if (name.find(xb) != std::string::npos) return true;
    return Lower(Trim(g_app.game.Get("button_prompts", "playstation"))) == "xbox";
}

struct PadSpot {
    const char* sdl;
    float u, v;    // centre, in the pad's square
    float hw, hh;  // half size (both: the radius of a round one)
    bool round;
};

void DrawPadGlyph(ImDrawList* d, const std::string& sdl, ImVec2 c, float r, bool xbox) {
    if (xbox) {
        const ImU32 color = sdl == "a" ? IM_COL32(96, 190, 80, 255) : sdl == "b" ? IM_COL32(225, 64, 64, 255)
                          : sdl == "x" ? IM_COL32(60, 130, 230, 255) : IM_COL32(240, 192, 40, 255);
        const char text[2] = {char(std::toupper(static_cast<unsigned char>(sdl[0]))), 0};
        ImFont* font = ImGui::GetFont();
        const ImVec2 t = font->CalcTextSizeA(r * 1.2f, FLT_MAX, 0.0f, text);
        d->AddText(font, r * 1.2f, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), color, text);
        return;
    }
    const float w = std::max(1.5f, r * 0.16f);
    if (sdl == "y") {
        d->AddTriangle(ImVec2(c.x, c.y - r * 0.5f), ImVec2(c.x - r * 0.5f, c.y + r * 0.34f),
                       ImVec2(c.x + r * 0.5f, c.y + r * 0.34f), IM_COL32(64, 214, 176, 255), w);
    } else if (sdl == "b") {
        d->AddCircle(c, r * 0.45f, IM_COL32(244, 92, 100, 255), 0, w);
    } else if (sdl == "a") {
        const ImU32 blue = IM_COL32(120, 160, 245, 255);
        d->AddLine(ImVec2(c.x - r * 0.42f, c.y - r * 0.42f), ImVec2(c.x + r * 0.42f, c.y + r * 0.42f), blue, w);
        d->AddLine(ImVec2(c.x + r * 0.42f, c.y - r * 0.42f), ImVec2(c.x - r * 0.42f, c.y + r * 0.42f), blue, w);
    } else {
        d->AddRect(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r * 0.4f),
                   IM_COL32(236, 128, 206, 255), 0.0f, 0, w);
    }
}

void PadDiagram() {
    App& a = g_app;
    const bool xbox = PadLooksXbox();
    const int capturing = g_pad_capture;

    // Which inputs each physical button drives now.
    std::map<std::string, std::vector<int>> by_button;
    for (int i = 0; i < int(std::size(kPadInputs)); ++i) {
        const std::string key = std::string("pad.") + kPadInputs[i].id;
        const bool custom = a.game.values.count(key) > 0;
        for (const auto& b : SplitKeys(custom ? a.game.Get(key) : kPadInputs[i].fallback)) by_button[b].push_back(i);
    }
    const auto identity = [](int i, const std::string& sdl) {  // the input's own button, nothing to say
        if (InputRole(kPadInputs[i].id)) return false;
        for (const auto& b : SplitKeys(kPadInputs[i].fallback))
            if (b == sdl) return true;
        return false;
    };
    const auto actions = [&](const std::string& sdl, bool all) {
        std::string text;
        const auto it = by_button.find(sdl);
        if (it == by_button.end()) return text;
        for (const int i : it->second) {
            if (!all && identity(i, sdl)) continue;
            const char* role = InputRole(kPadInputs[i].id);
            const std::string what = role ? role : GameButtonName(kPadInputs[i].id, xbox);
            if (text.find(what) == std::string::npos) text += (text.empty() ? "" : ", ") + what;
        }
        return text;
    };

    // The picture's frame: labels on both sides, the pad in the middle third.
    const float width = std::max(ImGui::GetContentRegionAvail().x, 560.0f);
    const float height = std::min(width * 0.46f, 420.0f * g_ui_scale);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("pad_picture", ImVec2(width, height));
    const bool picture_hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const LauncherArtwork& art = xbox ? g_pad_xbox_art : g_pad_ps_art;
    const bool picture = art.texture && art.width > 0 && art.height > 0;
    float qw = std::min(width * 0.44f, height), qh = qw;
    if (picture) {
        const float aspect = float(art.width) / float(art.height);
        qw = std::min(width * 0.46f, height * aspect);
        qh = qw / aspect;
    }
    const float side = qw;  // sizes are in the picture's width
    const ImVec2 p0{o.x + (width - qw) * 0.5f, o.y + (height - qh) * 0.5f};
    const auto at = [&](float u, float v) { return ImVec2(p0.x + u * qw, p0.y + v * qh); };

    // Body, from the real pads' outline: a dark Xbox one, or a white DualSense with its black middle.
    // Each outline is cubic curves in the pad's square (start, then control, control, end).
    const auto shape = [&](ImVec2 start, std::initializer_list<std::array<ImVec2, 3>> curves) {
        std::vector<ImVec2> points{at(start.x, start.y)};
        ImVec2 p = start;
        for (const auto& c : curves) {
            for (int k = 1; k <= 16; ++k) {
                const float t = k / 16.0f, s = 1.0f - t;
                const float x = s * s * s * p.x + 3 * s * s * t * c[0].x + 3 * s * t * t * c[1].x + t * t * t * c[2].x;
                const float y = s * s * s * p.y + 3 * s * s * t * c[0].y + 3 * s * t * t * c[1].y + t * t * t * c[2].y;
                points.push_back(at(x, y));
            }
            p = c[2];
        }
        points.pop_back();  // back at the start
        return points;
    };
    const ImU32 ink = IM_COL32(18, 20, 26, 255);
    const ImU32 body = xbox ? IM_COL32(56, 60, 68, 255) : IM_COL32(242, 244, 248, 255);
    const ImU32 trigger = xbox ? IM_COL32(36, 38, 44, 255) : IM_COL32(24, 26, 32, 255);
    const ImU32 key_fill = xbox ? IM_COL32(30, 32, 37, 255) : IM_COL32(248, 249, 251, 255);
    const ImU32 key_edge = xbox ? IM_COL32(84, 90, 102, 255) : ink;
    const ImU32 part_text = IM_COL32(232, 238, 246, 255);
    const float stroke = std::max(1.5f, side * 0.009f);

    const float dp = 0.026f, arm = 0.045f;  // the d-pad's half width and arm offset
    std::vector<PadSpot> spots;
    const auto dpad = [&](float u, float v) {
        spots.push_back({"dpup", u, v - arm, dp, dp, false});
        spots.push_back({"dpdown", u, v + arm, dp, dp, false});
        spots.push_back({"dpleft", u - arm, v, dp, dp, false});
        spots.push_back({"dpright", u + arm, v, dp, dp, false});
    };
    const auto faces = [&](float u, float v, float gap, float r) {
        spots.push_back({"y", u, v - gap, r, r, true});
        spots.push_back({"x", u - gap, v, r, r, true});
        spots.push_back({"b", u + gap, v, r, r, true});
        spots.push_back({"a", u, v + gap, r, r, true});
    };
    std::vector<ImVec2> outline, middle;
    if (xbox) {
        spots.push_back({"lefttrigger", 0.24f, 0.085f, 0.06f, 0.035f, false});
        spots.push_back({"righttrigger", 0.76f, 0.085f, 0.06f, 0.035f, false});
        spots.push_back({"leftshoulder", 0.22f, 0.16f, 0.09f, 0.022f, false});
        spots.push_back({"rightshoulder", 0.78f, 0.16f, 0.09f, 0.022f, false});
        spots.push_back({"leftstick", 0.28f, 0.35f, 0.06f, 0.06f, true});
        dpad(0.43f, 0.50f);
        spots.push_back({"rightstick", 0.60f, 0.50f, 0.06f, 0.06f, true});
        faces(0.75f, 0.35f, 0.062f, 0.03f);
        spots.push_back({"back", 0.42f, 0.28f, 0.018f, 0.018f, true});
        spots.push_back({"start", 0.58f, 0.28f, 0.018f, 0.018f, true});
        outline = shape({0.25f, 0.17f}, {{{{0.38f, 0.12f}, {0.62f, 0.12f}, {0.75f, 0.17f}}},
                                         {{{0.86f, 0.20f}, {0.92f, 0.30f}, {0.95f, 0.45f}}},
                                         {{{0.98f, 0.58f}, {0.97f, 0.68f}, {0.90f, 0.70f}}},
                                         {{{0.84f, 0.72f}, {0.80f, 0.64f}, {0.74f, 0.58f}}},
                                         {{{0.66f, 0.53f}, {0.34f, 0.53f}, {0.26f, 0.58f}}},
                                         {{{0.20f, 0.64f}, {0.16f, 0.72f}, {0.10f, 0.70f}}},
                                         {{{0.03f, 0.68f}, {0.02f, 0.58f}, {0.05f, 0.45f}}},
                                         {{{0.08f, 0.30f}, {0.14f, 0.20f}, {0.25f, 0.17f}}}});
    } else {
        spots.push_back({"lefttrigger", 0.22f, 0.095f, 0.06f, 0.03f, false});
        spots.push_back({"righttrigger", 0.78f, 0.095f, 0.06f, 0.03f, false});
        spots.push_back({"leftshoulder", 0.21f, 0.15f, 0.075f, 0.018f, false});
        spots.push_back({"rightshoulder", 0.79f, 0.15f, 0.075f, 0.018f, false});
        dpad(0.22f, 0.31f);
        faces(0.78f, 0.31f, 0.066f, 0.03f);
        spots.push_back({"touchpad", 0.5f, 0.255f, 0.165f, 0.095f, false});
        spots.push_back({"back", 0.296f, 0.20f, 0.010f, 0.022f, false});
        spots.push_back({"start", 0.704f, 0.20f, 0.010f, 0.022f, false});
        spots.push_back({"leftstick", 0.356f, 0.445f, 0.05f, 0.05f, true});
        spots.push_back({"rightstick", 0.644f, 0.445f, 0.05f, 0.05f, true});
        outline = shape({0.30f, 0.145f}, {{{{0.40f, 0.14f}, {0.60f, 0.14f}, {0.70f, 0.145f}}},
                                          {{{0.80f, 0.135f}, {0.88f, 0.15f}, {0.91f, 0.22f}}},
                                          {{{0.95f, 0.32f}, {0.97f, 0.52f}, {0.95f, 0.66f}}},
                                          {{{0.94f, 0.73f}, {0.87f, 0.76f}, {0.83f, 0.72f}}},
                                          {{{0.78f, 0.66f}, {0.74f, 0.58f}, {0.68f, 0.545f}}},
                                          {{{0.62f, 0.525f}, {0.38f, 0.525f}, {0.32f, 0.545f}}},
                                          {{{0.26f, 0.58f}, {0.22f, 0.66f}, {0.17f, 0.72f}}},
                                          {{{0.13f, 0.76f}, {0.06f, 0.73f}, {0.05f, 0.66f}}},
                                          {{{0.03f, 0.52f}, {0.05f, 0.32f}, {0.09f, 0.22f}}},
                                          {{{0.12f, 0.15f}, {0.20f, 0.135f}, {0.30f, 0.145f}}}});
        middle = shape({0.33f, 0.35f}, {{{{0.45f, 0.35f}, {0.55f, 0.35f}, {0.67f, 0.35f}}},
                                        {{{0.72f, 0.36f}, {0.76f, 0.42f}, {0.78f, 0.50f}}},
                                        {{{0.80f, 0.58f}, {0.81f, 0.64f}, {0.83f, 0.70f}}},
                                        {{{0.79f, 0.68f}, {0.75f, 0.60f}, {0.69f, 0.555f}}},
                                        {{{0.63f, 0.53f}, {0.37f, 0.53f}, {0.31f, 0.555f}}},
                                        {{{0.25f, 0.60f}, {0.21f, 0.68f}, {0.17f, 0.70f}}},
                                        {{{0.19f, 0.64f}, {0.20f, 0.58f}, {0.22f, 0.50f}}},
                                        {{{0.24f, 0.42f}, {0.28f, 0.36f}, {0.33f, 0.35f}}}});
    }
    if (picture) {
        // Measured on the pictures without their black outline (639 x 444 and 454 x 312); the
        // outline adds the same margin on every side.
        const float base_w = xbox ? 639.0f : 454.0f;
        const float iw = float(art.width), ih = float(art.height), margin = std::max(0.0f, (iw - base_w) * 0.5f);
        const auto px = [&](const char* sdl, float x, float y, float rx, float ry, bool round) {
            return PadSpot{sdl, (x + margin) / iw, (y + margin) / ih, rx / iw, ry / iw, round};
        };
        if (xbox) {
            spots = {px("lefttrigger", 150, 10, 46, 12, false),  px("righttrigger", 490, 10, 46, 12, false),
                     px("leftshoulder", 150, 34, 60, 10, false), px("rightshoulder", 490, 34, 60, 10, false),
                     px("leftstick", 155, 120, 40, 40, true),    px("rightstick", 405, 215, 40, 40, true),
                     px("back", 270, 120, 13, 13, true),         px("start", 365, 120, 13, 13, true),
                     px("y", 487, 75, 22, 22, true),             px("x", 440, 120, 22, 22, true),
                     px("b", 532, 120, 22, 22, true),            px("a", 487, 163, 22, 22, true),
                     px("dpup", 232, 193, 16, 16, false),        px("dpdown", 232, 258, 16, 16, false),
                     px("dpleft", 198, 225, 16, 16, false),      px("dpright", 266, 225, 16, 16, false)};
        } else {
            spots = {px("lefttrigger", 88, 6, 30, 6, false),     px("righttrigger", 366, 6, 30, 6, false),
                     px("leftshoulder", 88, 20, 34, 7, false),   px("rightshoulder", 366, 20, 34, 7, false),
                     px("back", 120, 42, 6, 11, false),          px("start", 331, 42, 6, 11, false),
                     px("touchpad", 227, 58, 86, 42, false),
                     px("dpup", 87, 66, 11, 11, false),          px("dpdown", 87, 110, 11, 11, false),
                     px("dpleft", 65, 88, 11, 11, false),        px("dpright", 109, 88, 11, 11, false),
                     px("y", 369, 52, 16, 16, true),             px("x", 336, 88, 16, 16, true),
                     px("b", 401, 88, 16, 16, true),             px("a", 369, 124, 16, 16, true),
                     px("leftstick", 155, 150, 23, 23, true),    px("rightstick", 296, 150, 23, 23, true)};
        }
        d->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(art.texture))), p0, ImVec2(p0.x + qw, p0.y + qh));
    }
    // Triggers and bumpers first: the body covers their lower edge.
    for (const auto& s : spots) {
        if (picture) break;
        const std::string sdl = s.sdl;
        if (sdl.find("trigger") == std::string::npos && sdl.find("shoulder") == std::string::npos) continue;
        const ImVec2 c = at(s.u, s.v);
        const float hw = s.hw * side, hh = s.hh * side;
        d->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), trigger, std::min(hw, hh) * 0.9f);
    }
    if (!picture) d->AddConcavePolyFilled(outline.data(), int(outline.size()), body);
    if (!picture && !middle.empty()) d->AddConcavePolyFilled(middle.data(), int(middle.size()), ink);
    if (!picture) d->AddPolyline(outline.data(), int(outline.size()), xbox ? IM_COL32(96, 104, 118, 255) : ink, ImDrawFlags_Closed, stroke * 1.4f);
    if (picture) {
    } else if (xbox) {  // the guide button
        d->AddCircleFilled(at(0.5f, 0.19f), side * 0.04f, key_fill);
        d->AddCircle(at(0.5f, 0.19f), side * 0.04f, IM_COL32(200, 210, 222, 255), 0, stroke);
    } else {  // the microphone's dots and the PS button
        for (int k = -2; k <= 2; ++k) d->AddCircleFilled(at(0.5f + k * 0.012f, 0.385f), side * 0.004f, IM_COL32(220, 224, 232, 255));
        d->AddRectFilled(at(0.485f, 0.47f), at(0.515f, 0.478f), IM_COL32(220, 224, 232, 255), side * 0.004f);
    }

    // The button under the mouse, and the ones the hovered table row uses.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    std::string hovered;
    for (const auto& s : spots) {
        const ImVec2 c = at(s.u, s.v);
        const float hw = s.hw * side, hh = s.hh * side;
        const bool in = s.round ? (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) <= hw * hw
                                : std::fabs(mouse.x - c.x) <= hw && std::fabs(mouse.y - c.y) <= hh;
        if (picture_hovered && in) hovered = s.sdl;
    }
    std::vector<std::string> lit;
    if (g_pad_hover_input >= 0 && g_pad_hover_input < int(std::size(kPadInputs))) {
        const PadInput& in = kPadInputs[g_pad_hover_input];
        const std::string key = std::string("pad.") + in.id;
        lit = SplitKeys(a.game.values.count(key) ? a.game.Get(key) : in.fallback);
    }
    const auto is_lit = [&](const std::string& sdl) {
        return sdl == hovered || std::find(lit.begin(), lit.end(), sdl) != lit.end();
    };
    const ImU32 glow = IM_COL32(255, 205, 90, 255);

    for (const auto& s : spots) {
        const std::string sdl = s.sdl;
        const ImVec2 c = at(s.u, s.v);
        const float hw = s.hw * side, hh = s.hh * side;
        const bool hot = is_lit(sdl);
        if (picture) {  // the image has the buttons: only the light on the ones in play
            if (hot && s.round) d->AddCircle(c, hw + 3.0f, glow, 0, 3.0f);
            else if (hot) d->AddRect(ImVec2(c.x - hw - 3, c.y - hh - 3), ImVec2(c.x + hw + 3, c.y + hh + 3), glow, 5.0f, 0, 3.0f);
            continue;
        }
        const bool shoulder = sdl.find("trigger") != std::string::npos || sdl.find("shoulder") != std::string::npos;
        if (sdl == "leftstick" || sdl == "rightstick") {
            d->AddCircleFilled(c, hw, xbox ? IM_COL32(22, 24, 28, 255) : IM_COL32(40, 43, 52, 255));
            d->AddCircle(c, hw, xbox ? IM_COL32(90, 96, 108, 255) : IM_COL32(236, 238, 244, 255), 0, stroke * 1.4f);
            d->AddCircle(c, hw * 0.66f, xbox ? IM_COL32(70, 76, 88, 255) : IM_COL32(120, 126, 140, 255), 0, stroke);
        } else if (s.round) {
            d->AddCircleFilled(c, hw, key_fill);
            d->AddCircle(c, hw, key_edge, 0, stroke);
            if (sdl.size() == 1) DrawPadGlyph(d, sdl, c, hw, xbox);
        } else if (sdl == "touchpad") {
            d->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), key_fill, side * 0.03f);
            d->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), ink, side * 0.03f, 0, stroke * 1.4f);
        } else if (!shoulder) {  // d-pad arms, Share and Options
            const bool small = sdl == "back" || sdl == "start";
            d->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), small && !xbox ? ink : key_fill,
                             std::min(hw, hh) * (small ? 1.0f : 0.45f));
            if (!small || xbox)
                d->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), key_edge, std::min(hw, hh) * 0.45f, 0, stroke);
        }
        if (shoulder || sdl == "leftstick" || sdl == "rightstick") {
            const std::string name = PhysicalButtonName(sdl, xbox);
            ImFont* font = ImGui::GetFont();
            const float fs = std::min(ImGui::GetFontSize() * 0.8f, std::max(hh * 1.5f, 10.0f));
            const ImVec2 t = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, name.c_str());
            d->AddText(font, fs, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), part_text, name.c_str());
        }
        if (hot) {
            if (s.round) d->AddCircle(c, hw + 3.0f, glow, 0, 2.5f);
            else d->AddRect(ImVec2(c.x - hw - 3, c.y - hh - 3), ImVec2(c.x + hw + 3, c.y + hh + 3), glow, 4.0f, 0, 2.5f);
        }
    }

    // Labels: one per button (one for the whole d-pad), stacked on its side without overlapping.
    struct Callout {
        std::vector<std::string> sdls;
        ImVec2 anchor;
        bool left;
        std::string text;
    };
    std::vector<Callout> callouts;
    for (const auto& s : spots) {
        const std::string sdl = s.sdl;
        if (sdl.rfind("dp", 0) == 0) continue;
        Callout c;
        c.sdls = {sdl};
        c.anchor = at(s.u, s.v);
        c.left = s.u < 0.5f || (sdl == "touchpad");
        const std::string what = actions(sdl, false);
        c.text = PhysicalButtonName(sdl, xbox) + (what.empty() ? "" : "  ·  " + what);
        if (by_button.find(sdl) == by_button.end()) c.text += T("  ·  (nada)", "  ·  (nothing)");
        callouts.push_back(c);
    }
    {
        Callout c;
        c.sdls = {"dpup", "dpdown", "dpleft", "dpright"};
        float u = 0, v = 0;  // the d-pad's centre: the middle of its four arms
        for (const auto& s : spots)
            if (std::string(s.sdl).rfind("dp", 0) == 0) { u += s.u * 0.25f; v += s.v * 0.25f; }
        c.anchor = at(u, v);
        c.left = true;
        std::string text;
        for (const char* dir : {"dpup", "dpdown", "dpleft", "dpright"}) {
            const std::string what = actions(dir, false);
            if (!what.empty()) text += (text.empty() ? "" : ", ") + PhysicalButtonName(dir, false) + " " + what;
        }
        c.text = std::string(T("Cruceta", "D-pad")) + (text.empty() ? "" : "  ·  " + text);
        callouts.push_back(c);
    }
    const float line_h = ImGui::GetTextLineHeight() + 8.0f;
    for (const bool left : {true, false}) {
        std::vector<Callout*> column;
        for (auto& c : callouts)
            if (c.left == left) column.push_back(&c);
        // top to bottom; at the same height the button nearer the labels first, so lines do not cross
        std::sort(column.begin(), column.end(), [left](const Callout* x, const Callout* y) {
            if (std::fabs(x->anchor.y - y->anchor.y) > 2.0f) return x->anchor.y < y->anchor.y;
            return left ? x->anchor.x < y->anchor.x : x->anchor.x > y->anchor.x;
        });
        float y = o.y;
        std::vector<float> ys;
        for (const Callout* c : column) {
            y = std::max(y, c->anchor.y - line_h * 0.5f);
            ys.push_back(y);
            y += line_h + 3.0f;
        }
        // pushed past the bottom: slide the column up
        const float over = (ys.empty() ? 0.0f : ys.back() + line_h) - (o.y + height);
        if (over > 0)
            for (float& v : ys) v -= over;
        const float edge = left ? p0.x - side * 0.02f : p0.x + side * 1.02f;
        for (size_t k = 0; k < column.size(); ++k) {
            const Callout& c = *column[k];
            const ImVec2 t = ImGui::CalcTextSize(c.text.c_str());
            const float x0 = left ? std::max(o.x, edge - t.x - 16.0f) : edge;
            const float x1 = left ? edge : std::min(o.x + width, edge + t.x + 16.0f);
            const ImVec2 r0{x0, ys[k]}, r1{x1, ys[k] + line_h};
            bool hot = false;
            for (const auto& sdl : c.sdls) hot = hot || is_lit(sdl);
            const bool label_hovered = picture_hovered && mouse.x >= r0.x && mouse.x <= r1.x && mouse.y >= r0.y && mouse.y <= r1.y;
            if (label_hovered && c.sdls.size() == 1) hovered = c.sdls[0];
            hot = hot || label_hovered;
            const ImVec2 joint{left ? x1 : x0, ys[k] + line_h * 0.5f};
            d->AddLine(joint, c.anchor, hot ? glow : IM_COL32(120, 210, 245, 150), hot ? 2.0f : 1.2f);
            d->AddCircleFilled(c.anchor, 3.0f, hot ? glow : IM_COL32(120, 210, 245, 220));
            d->AddRectFilled(r0, r1, IM_COL32(6, 36, 74, 235), line_h * 0.5f);
            d->AddRect(r0, r1, hot ? glow : IM_COL32(110, 200, 240, 140), line_h * 0.5f, 0, hot ? 2.0f : 1.0f);
            d->PushClipRect(r0, r1, true);
            d->AddText(ImVec2(r0.x + 8.0f, r0.y + 4.0f), IM_COL32(232, 246, 255, 255), c.text.c_str());
            d->PopClipRect();
        }
    }

    if (!hovered.empty()) {
        const std::string what = actions(hovered, true);
        ImGui::SetTooltip("%s: %s", PhysicalButtonName(hovered, xbox).c_str(),
                          what.empty() ? T("no hace nada en el juego", "does nothing in the game") : what.c_str());
        if (capturing >= 0 && clicked) {
            a.game.Set(std::string("pad.") + kPadInputs[capturing].id, hovered);
            SaveGameIni();
            g_pad_capture = -1;
        } else if (clicked) {  // no row waiting: choose what this button does
            g_pad_menu_button = hovered;
            ImGui::OpenPopup("pad_button_menu");
        }
    }
    // The button's menu: each action of the game; the one picked moves to this button (the action's
    // other buttons are dropped, as "Change" in the table does).
    if (ImGui::BeginPopup("pad_button_menu")) {
        const std::string sdl = g_pad_menu_button;
        ImGui::TextColored(ImVec4(0.42f, 0.88f, 1.0f, 1.0f), "%s",
                           Format(T("¿Qué hace %s?", "What does %s do?"), PhysicalButtonName(sdl, xbox)).c_str());
        ImGui::Separator();
        for (int i = 0; i < int(std::size(kPadInputs)); ++i) {
            const PadInput& in = kPadInputs[i];
            const std::string key = std::string("pad.") + in.id;
            const auto now = SplitKeys(a.game.values.count(key) ? a.game.Get(key) : in.fallback);
            const bool here = std::find(now.begin(), now.end(), sdl) != now.end();
            if (ImGui::Selectable(T(in.es, in.en), here) && !here) {
                a.game.Set(key, sdl);
                SaveGameIni();
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable(T("Nada (quitarle sus acciones)", "Nothing (drop its actions)"))) {
            for (const auto& in : kPadInputs) {
                const std::string key = std::string("pad.") + in.id;
                auto now = SplitKeys(a.game.values.count(key) ? a.game.Get(key) : in.fallback);
                const auto end = std::remove(now.begin(), now.end(), sdl);
                if (end == now.end()) continue;
                now.erase(end, now.end());
                std::string value;
                for (const auto& b : now) value += (value.empty() ? "" : ",") + b;
                a.game.Set(key, value);
            }
            SaveGameIni();
        }
        ImGui::EndPopup();
    }
    FormNote(capturing >= 0 ? T("Esperando botón: púlsalo en tu mando o haz clic en el dibujo.",
                                "Waiting for a button: press it on your controller or click it on the picture.")
                            : xbox ? T("Mando de Xbox (según tu mando o los iconos de botones). Haz clic en un botón del dibujo para elegir qué hace.",
                                       "Xbox controller (from your controller or the button prompts). Click a button on the picture to choose what it does.")
                                   : T("Mando de PlayStation (según tu mando o los iconos de botones). Haz clic en un botón del dibujo para elegir qué hace.",
                                       "PlayStation controller (from your controller or the button prompts). Click a button on the picture to choose what it does."));
}

void GamepadTab() {
    App& a = g_app;
    FormBegin();
    FormSection(T("Mando", "Controller"));
    ControllerSettings();
    FormEnd();
    FormSection(T("Mapa del mando", "Controller map"));
    PadDiagram();
    FormSection(T("Botones del mando", "Gamepad buttons"));
    FormNote(T("Botones del jugador 1. Sirve con mandos de Xbox, PlayStation (DualShock / DualSense), Switch Pro y genéricos. "
               "Pulsa \"Cambiar\" y presiona el botón en tu mando o elígelo en la pantalla. Se aplica en el próximo inicio.",
               "Player 1's buttons. Works with Xbox, PlayStation (DualShock / DualSense), Switch Pro and generic controllers. "
               "Press \"Change\" and press the button on your controller or choose it on screen. Applies on next launch."));

    PadWatchStart();
    const int rows = int(std::size(kPadInputs));
    for (std::string pressed; PadWatchTake(pressed);) {
        const int waiting = g_pad_capture;
        if (waiting >= 0) {  // any button, the d-pad and Circle / B included, is the new one
            a.game.Set(std::string("pad.") + kPadInputs[waiting].id, pressed);
            SaveGameIni();
            g_pad_capture = -1;
            g_pad_cursor = waiting;
        } else if (pressed == "dpdown" || pressed == "dpup") {
            g_pad_cursor = g_pad_cursor < 0 ? 0 : std::clamp(g_pad_cursor + (pressed == "dpdown" ? 1 : -1), 0, rows - 1);
            g_pad_cursor_moved = true;
        } else if (g_pad_cursor >= 0 && pressed == "a") {  // Change
            g_pad_capture = g_pad_cursor;
        } else if (g_pad_cursor >= 0 && pressed == "y") {  // Default
            a.game.Erase(std::string("pad.") + kPadInputs[g_pad_cursor].id);
            SaveGameIni();
        } else if (g_pad_cursor >= 0 && pressed == "x") {  // Clear
            a.game.Set(std::string("pad.") + kPadInputs[g_pad_cursor].id, "");
            SaveGameIni();
        }
    }
    FormNote(PadWatchRunning()
                 ? T("Con el mando: cruceta ↑ ↓ elige una fila, A / Cruz la cambia (luego pulsa el botón nuevo), "
                     "Y / Triángulo vuelve al original, X / Cuadrado la deja sin botón.",
                     "With the controller: d-pad ↑ ↓ picks a row, A / Cross changes it (then press the new button), "
                     "Y / Triangle restores the default, X / Square clears it.")
                 : T("No se pudo leer el mando con SDL: solo funcionan los mandos de Xbox (XInput).",
                     "Could not read the controller through SDL: only Xbox (XInput) controllers work."));

    const int capturing = g_pad_capture;
    if (capturing >= 0 && !PadWatchRunning()) {
        const char* polled = PollGamepadButton();
        if (g_pad_waiting_release) {
            if (!polled) g_pad_waiting_release = false;
        } else if (polled) {
            const PadInput& in = kPadInputs[capturing];
            a.game.Set(std::string("pad.") + in.id, polled);
            SaveGameIni();
            g_pad_capture = -1;
        }
    }

    if (capturing >= 0 && capturing < int(std::size(kPadInputs))) {
        const PadInput& in = kPadInputs[capturing];
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Border, kYellow);
        ImGui::BeginChild("pad_assign_box", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kYellow, "%s: %s", T("ASIGNAR BOTÓN PARA", "ASSIGN BUTTON FOR"), T(in.es, in.en));
        ImGui::TextColored(kGrey, "%s", T("Presiona cualquier botón o gatillo en tu mando, o haz clic en uno de abajo:",
                                          "Press any button or trigger on your controller, or click one below:"));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        bool have_button = false;
        auto AddBtn = [&](const char* id, const char* label) {
            const float next_right = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
                                     ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
            if (have_button && next_right <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
                ImGui::SameLine();
            have_button = true;
            if (ImGui::Button(label)) {
                a.game.Set(std::string("pad.") + in.id, id);
                SaveGameIni();
                g_pad_capture = -1;
            }
        };

        AddBtn("a", T("Cruz / A", "Cross / A"));
        AddBtn("b", T("Círculo / B", "Circle / B"));
        AddBtn("x", T("Cuadrado / X", "Square / X"));
        AddBtn("y", T("Triángulo / Y", "Triangle / Y"));
        AddBtn("leftshoulder", "L1 / LB");
        AddBtn("rightshoulder", "R1 / RB");
        AddBtn("lefttrigger", T("Gatillo L2 / LT", "Trigger L2 / LT"));
        AddBtn("righttrigger", T("Gatillo R2 / RT", "Trigger R2 / RT"));
        AddBtn("dpup", T("Cruceta Arriba", "D-pad Up"));
        AddBtn("dpdown", T("Cruceta Abajo", "D-pad Down"));
        AddBtn("dpleft", T("Cruceta Izq.", "D-pad Left"));
        AddBtn("dpright", T("Cruceta Der.", "D-pad Right"));
        AddBtn("leftstick", T("L3 (Stick Izq.)", "L3 (Left Stick)"));
        AddBtn("rightstick", T("R3 (Stick Der.)", "R3 (Right Stick)"));
        AddBtn("start", "Start / Options");
        AddBtn("back", "Select / Back");
        AddBtn("touchpad", "Touchpad");
        if (ImGui::Button(T("Cancelar (Esc)", "Cancel (Esc)"))) {
            g_pad_capture = -1;
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    int pad_row = -1;
    if (LauncherUI::Table table{"pad_table", 3}) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Botón asignado", "Assigned button"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, RowButtonsWidth());
        ImGui::TableHeadersRow();

        for (int i = 0; i < int(std::size(kPadInputs)); ++i) {
            const PadInput& in = kPadInputs[i];
            const std::string key = std::string("pad.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto buttons = SplitKeys(custom ? a.game.Get(key) : in.fallback);

            ImGui::PushID(i + 200);
            ImGui::TableNextRow();
            if (i == g_pad_cursor) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(40, 150, 210, 110));
            }
            ImGui::TableNextColumn();
            const float row_top = ImGui::GetCursorScreenPos().y;
            if (i == g_pad_cursor && g_pad_cursor_moved) {
                ImGui::SetScrollHereY(0.5f);
                g_pad_cursor_moved = false;
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));

            table.Cell();
            if (capturing == i) {
                ImGui::TextColored(kYellow, "%s", T("Esperando botón...", "Waiting for button..."));
            } else if (buttons.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin botón asignado)", "(unbound)"));
            } else {
                std::string text;
                for (const auto& b : buttons) text += (text.empty() ? "" : "  /  ") + PadButtonLabel(b);
                if (custom) ImGui::TextUnformatted(text.c_str());
                else ImGui::TextColored(kGrey, "%s", text.c_str());
            }

            table.Cell();
            ImGui::BeginDisabled(capturing >= 0);
            if (table.Action(T("Cambiar", "Change"))) {
                g_pad_capture = i;
                g_pad_waiting_release = (PollGamepadButton() != nullptr);
            }
            if (table.Action(T("Quitar", "Clear"))) {
                a.game.Set(key, "");
                SaveGameIni();
            }
            ImGui::BeginDisabled(!custom);
            if (table.Action(T("Original", "Default"))) {
                a.game.Erase(key);
                SaveGameIni();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (ImGui::IsWindowHovered() && ImGui::GetIO().MousePos.y >= row_top &&
                ImGui::GetIO().MousePos.y < ImGui::GetItemRectMax().y + ImGui::GetStyle().CellPadding.y)
                pad_row = i;
            ImGui::PopID();
        }
    }
    g_pad_hover_input = pad_row >= 0 ? pad_row : g_pad_cursor;

    FormNote(T("Gris: asignación por defecto. Blanco: botón personalizado.",
               "Grey: default binding. White: custom binding."));
    FormSection(T("Preajustes rápidos", "Quick presets"));
    ImGui::BeginDisabled(capturing >= 0);
    if (ImGui::Button(T("Cruceta Abajo = L2 (Usar Ítem)", "D-pad Down = L2 (Use Item)"))) {
        a.game.Set("pad.l2", "dpdown");
        a.game.Set("pad.down", "lefttrigger");
        SaveGameIni();
        a.notice = T("Preajuste aplicado: Cruceta Abajo configurada como L2 (Ítem) y Gatillo L2 como Cruceta Abajo.",
                     "Preset applied: D-pad Down set to L2 (Item) and Trigger L2 set to D-pad Down.");
    }
    HelpMarker(T("Intercambia la Cruceta Abajo con el Gatillo L2. Ideal para disparar objetos con la flecha de abajo.",
                 "Swaps D-pad Down with Trigger L2. Ideal for using items with down arrow."));
    if (ImGui::Button(T("Restaurar todos los botones", "Restore all pad buttons"))) {
        for (const auto& in : kPadInputs) a.game.Erase(std::string("pad.") + in.id);
        SaveGameIni();
        a.notice = T("Todos los botones del mando se restauraron a los valores originales.",
                     "All gamepad buttons restored to defaults.");
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------------------------
// Shortcuts & Steam integration (Desktop shortcut and Steam custom artwork / shortcuts.vdf)

uint32_t Crc32(const void* data, size_t size) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

fs::path FindSteamPath();

struct SteamShortcut {
    uint32_t appid = 0;
    std::string app_name;
    std::string exe;
    std::string start_dir;
    std::string icon;
    std::string shortcut_path;
    std::string launch_options;
    int32_t is_hidden = 0;
    int32_t allow_desktop_config = 1;
    int32_t allow_overlay = 1;
    int32_t open_vr = 0;
    int32_t devkit = 0;
    std::string devkit_game_id;
    int32_t devkit_override_app_id = 0;
    int32_t last_play_time = 0;
    std::string flatpak_app_id;
    std::string sortas;
    std::vector<std::string> tags;
};

std::vector<SteamShortcut> ParseShortcuts(const std::vector<uint8_t>& buf) {
    std::vector<SteamShortcut> list;
    size_t i = 0;
    auto read_sz = [&]() -> std::string {
        size_t start = i;
        while (i < buf.size() && buf[i] != 0) ++i;
        std::string s(reinterpret_cast<const char*>(&buf[start]), i - start);
        if (i < buf.size()) ++i;
        return s;
    };
    if (buf.size() < 12 || buf[i++] != 0) return list;
    std::string root = read_sz();
    if (root != "shortcuts") return list;
    while (i < buf.size() && buf[i] != 8) {
        if (buf[i++] != 0) break;
        std::string idx = read_sz();
        SteamShortcut sc;
        auto iequals = [](const std::string& s1, const std::string& s2) {
            if (s1.size() != s2.size()) return false;
            for (size_t k = 0; k < s1.size(); ++k)
                if (::tolower((unsigned char)s1[k]) != ::tolower((unsigned char)s2[k])) return false;
            return true;
        };
        while (i < buf.size() && buf[i] != 8) {
            uint8_t type = buf[i++];
            std::string key = read_sz();
            if (type == 1) { // String
                std::string val = read_sz();
                if (iequals(key, "appname")) sc.app_name = val;
                else if (iequals(key, "exe")) sc.exe = val;
                else if (iequals(key, "startdir")) sc.start_dir = val;
                else if (iequals(key, "icon")) sc.icon = val;
                else if (iequals(key, "shortcutpath")) sc.shortcut_path = val;
                else if (iequals(key, "launchoptions")) sc.launch_options = val;
                else if (iequals(key, "devkitgameid")) sc.devkit_game_id = val;
                else if (iequals(key, "flatpakappid")) sc.flatpak_app_id = val;
                else if (iequals(key, "sortas")) sc.sortas = val;
            } else if (type == 2) { // Int32
                if (i + 4 > buf.size()) break;
                uint32_t val = 0;
                std::memcpy(&val, &buf[i], 4);
                i += 4;
                if (iequals(key, "appid")) sc.appid = val;
                else if (iequals(key, "ishidden")) sc.is_hidden = val;
                else if (iequals(key, "allowdesktopconfig")) sc.allow_desktop_config = val;
                else if (iequals(key, "allowoverlay")) sc.allow_overlay = val;
                else if (iequals(key, "openvr")) sc.open_vr = val;
                else if (iequals(key, "devkit")) sc.devkit = val;
                else if (iequals(key, "devkitoverrideappid")) sc.devkit_override_app_id = val;
                else if (iequals(key, "lastplaytime")) sc.last_play_time = val;
            } else if (type == 0) { // Submap, e.g. tags
                while (i < buf.size() && buf[i] != 8) {
                    uint8_t subtype = buf[i++];
                    std::string tag_idx = read_sz();
                    if (subtype == 1) sc.tags.push_back(read_sz());
                    else if (subtype == 2) i += 4;
                }
                if (i < buf.size() && buf[i] == 8) ++i;
            }
        }
        if (i < buf.size() && buf[i] == 8) ++i;
        list.push_back(sc);
    }
    return list;
}

/// The shortcuts a shortcuts.vdf holds, counted from its raw "appname" keys: a file ParseShortcuts
/// reads differently is left alone rather than rewritten without the player's other games.
size_t CountShortcuts(const std::vector<uint8_t>& buf) {
    static constexpr char key[] = "\x01appname";
    size_t count = 0;
    for (size_t i = 0; i + sizeof key <= buf.size(); ++i) {
        size_t k = 0;
        while (k < sizeof key - 1 && ::tolower(buf[i + k]) == key[k]) ++k;
        if (k == sizeof key - 1 && buf[i + k] == 0) ++count;
    }
    return count;
}

std::vector<uint8_t> SerializeShortcuts(const std::vector<SteamShortcut>& list) {
    std::vector<uint8_t> out;
    auto write_byte = [&](uint8_t b) { out.push_back(b); };
    auto write_sz = [&](const std::string& s) {
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0);
    };
    auto write_str = [&](const std::string& key, const std::string& val) {
        write_byte(1);
        write_sz(key);
        write_sz(val);
    };
    auto write_int = [&](const std::string& key, int32_t val) {
        write_byte(2);
        write_sz(key);
        uint8_t b[4];
        std::memcpy(b, &val, 4);
        out.insert(out.end(), b, b + 4);
    };

    write_byte(0);
    write_sz("shortcuts");
    for (size_t idx = 0; idx < list.size(); ++idx) {
        const auto& sc = list[idx];
        write_byte(0);
        write_sz(std::to_string(idx));
        write_int("appid", sc.appid);
        write_str("appname", sc.app_name);
        write_str("exe", sc.exe);
        write_str("StartDir", sc.start_dir);
        write_str("icon", sc.icon);
        write_str("ShortcutPath", sc.shortcut_path);
        write_str("LaunchOptions", sc.launch_options);
        write_int("IsHidden", sc.is_hidden);
        write_int("AllowDesktopConfig", sc.allow_desktop_config);
        write_int("AllowOverlay", sc.allow_overlay);
        write_int("OpenVR", sc.open_vr);
        write_int("Devkit", sc.devkit);
        write_str("DevkitGameID", sc.devkit_game_id);
        write_int("DevkitOverrideAppID", sc.devkit_override_app_id);
        write_int("LastPlayTime", sc.last_play_time);
        write_str("FlatpakAppID", sc.flatpak_app_id);
        write_str("sortas", sc.sortas);
        write_byte(0);
        write_sz("tags");
        for (size_t t = 0; t < sc.tags.size(); ++t) {
            write_byte(1);
            write_sz(std::to_string(t));
            write_sz(sc.tags[t]);
        }
        write_byte(8);
        write_byte(8);
    }
    write_byte(8);  // the "shortcuts" map
    write_byte(8);  // the file's root: Steam discards a file without it
    return out;
}

/// The launcher's name: the packaged executable, the desktop shortcut, Steam's entry and the
/// window all use it. Releases before v0.5.1 shipped CTR-NitroFueled-PC.exe and a desktop
/// shortcut named "CTR Nitro-Fueled".
constexpr const wchar_t* kLauncherName = L"Crash\u2122 Team Racing Nitro-Fueled";
constexpr const wchar_t* kOldLauncherExe = L"CTR-NitroFueled-PC.exe";
constexpr const wchar_t* kOldShortcut = L"CTR Nitro-Fueled.lnk";

/// Where a .lnk points (empty when it cannot be read).
fs::path ShortcutTarget(const fs::path& link);

/// This executable, whatever it is called.
fs::path LauncherExe();

bool CreateDesktopShortcut(std::string& out_error);

/// Releases before v0.5.1 called the launcher CTR-NitroFueled-PC.exe. An update unzipped over
/// one keeps that file and its shortcuts: the old executable becomes a copy of this one, so a
/// Steam entry or any other shortcut naming it opens this launcher, and the desktop shortcut
/// made by the old launcher is made again with the new name.
void MigrateOldLauncher();

/// Adds the launcher to Steam as a non-Steam game started with --launch (the game directly, no
/// launcher window: Steam's recording and overlay see the game) with the steamgrid artwork.
/// Steam is closed first when it runs, as it rewrites shortcuts.vdf on exit.
bool AddToSteam(std::string& out_msg);

// ---------------------------------------------------------------------------------------------
// Updates (port_update.cpp): the latest published release's Windows ZIP, downloaded into
// update\, checked (size, SHA-256), unpacked with the bundled Python and copied over the port.
// Settings, the online key, friends, saves, backups, DLC, logs and the player's own patches are
// never in the package and never written; replaced files go to update\rollback\ first.

#ifdef _WIN32
#include "windows/update.inc"
#else
#include "linux/update.inc"
#endif
// ---------------------------------------------------------------------------------------------
// Achievements tab (v0.4): the game's trophies (sce_sys/trophy/trophy00.trp), with the ones
// unlocked while playing (<user>/trophies/<TITLE_ID>/unlocked.txt, written by the runtime:
// src/runtime_services.c). Icons are plain PNGs in the TRP. Names, descriptions and grades are
// in its ESFM files, AES-encrypted with the console's trophy key, which is not part of this
// port: with the player's own key (the one shadPS4 uses: entered here or imported from
// shadPS4's keys.json) they are decrypted as shadPS4 does (src/core/file_format/trp.cpp) and
// written to trophies.tsv, which the runtime reads for its in-game notice and the platinum.

LauncherTexture* MakeTexture(const uint32_t* bgra, int width, int height);  // after g_device

// The 48 trophies' names, grades and what to do (src/ctr_trophies.h, shared with the runtime).
#include "../src/ctr_trophies.h"
#define kCtrTrophies ctr_trophies


struct Trophy {
    int id = 0;
    char grade = 0;  // 'P' 'G' 'S' 'B', 0 unknown
    bool hidden = false;
    std::string name, detail;
    LauncherTexture* icon = nullptr;
    uint64_t unlocked = 0;  // unix time, 0 locked
};

struct TrophySet {
    fs::path game;              // the game folder it was read from
    std::string language;       // the game language its names are in
    std::string title_id, np_comm_id, title_name;
    std::vector<Trophy> list;
    LauncherTexture* icon = nullptr;
    bool names = false;         // names and grades known
    std::string error, note;
    fs::file_time_type unlocked_time{};
    double next_poll = 0.0;
};
TrophySet g_trophies;
bool g_trophies_reload = true;
int g_trophy_filter = 0;  // 0 all, 1 unlocked, 2 locked
bool g_trophy_reset_confirm = false;
char g_trophy_key_input[64] = "";

fs::path TrophyDir(const std::string& title) { return UserRoot() / L"trophies" / Widen(title); }

void ReleaseTrophies(TrophySet& t) {
    for (auto& tr : t.list)
        ReleaseTexture(tr.icon);
    ReleaseTexture(t.icon);
    t = TrophySet{};
}

/// A PNG to a texture, through WIC.
LauncherTexture* PngTexture(const std::string& png);

/// The 16-byte trophy key from hex ("" when it is not 32 hex digits).
std::string TrophyKeyBytes(const std::string& hex) {
    std::string clean;
    for (char c : hex)
        if (std::isxdigit(static_cast<unsigned char>(c))) clean += c;
    if (clean.size() != 32) return {};
    std::string out;
    for (size_t i = 0; i < 32; i += 2) out += char(std::stoi(clean.substr(i, 2), nullptr, 16));
    return out;
}

/// AES-128-CBC without padding (Windows CNG). encrypt: true to encrypt.
bool AesCbc(const std::string& key, const unsigned char iv_in[16], const unsigned char* in, size_t size, unsigned char* out,
            bool encrypt);

/// One ESFM entry decrypted to its XML (shadPS4's DecryptEFSM): the key for the file is the
/// NP communication ID (16 bytes, zero-padded) encrypted with the trophy key (CBC, zero IV); the
/// entry is IV (16 bytes) + data, CBC-decrypted with that key and IV.
std::string DecryptEsfm(const std::string& trophy_key, const std::string& np_comm_id, const std::string& entry) {
    if (entry.size() <= 32 || (entry.size() - 16) % 16) return {};
    unsigned char comm[16]{}, zero[16]{}, file_key[16];
    std::memcpy(comm, np_comm_id.data(), std::min<size_t>(np_comm_id.size(), 16));
    if (!AesCbc(trophy_key, zero, comm, 16, file_key, true)) return {};
    std::string xml(entry.size() - 16, '\0');
    if (!AesCbc(std::string(reinterpret_cast<char*>(file_key), 16), reinterpret_cast<const unsigned char*>(entry.data()),
                reinterpret_cast<const unsigned char*>(entry.data()) + 16, entry.size() - 16,
                reinterpret_cast<unsigned char*>(xml.data()), false))
        return {};
    const size_t end = xml.rfind('>');
    if (end == std::string::npos) return {};
    xml.resize(end + 1);
    return xml.rfind("<?xml", 0) == 0 || xml.find("<trophyconf") != std::string::npos ? xml : std::string{};
}

std::string XmlTag(const std::string& xml, const std::string& tag, size_t from = 0, size_t until = std::string::npos) {
    const size_t a = xml.find("<" + tag + ">", from);
    if (a == std::string::npos || a >= until) return {};
    const size_t b = xml.find("</" + tag + ">", a);
    if (b == std::string::npos || b > until) return {};
    return XmlUnescape(xml.substr(a + tag.size() + 2, b - a - tag.size() - 2));
}

/// The trophies in a decrypted TROP*.XML: id -> (grade, hidden, name, detail).
void ParseTrophyXml(const std::string& xml, TrophySet& t) {
    t.title_name = XmlTag(xml, "title-name");
    for (size_t at = 0; (at = xml.find("<trophy ", at)) != std::string::npos;) {
        const size_t close = xml.find('>', at), end = xml.find("</trophy>", at);
        if (close == std::string::npos || end == std::string::npos) break;
        const auto attr = XmlAttributes(xml.substr(at + 8, close - at - 8));
        auto get = [&](const char* k) {
            const auto it = attr.find(k);
            return it == attr.end() ? std::string() : it->second;
        };
        const int id = std::atoi(get("id").c_str());
        for (auto& tr : t.list) {
            if (tr.id != id) continue;
            const std::string type = get("ttype");
            tr.grade = type.empty() ? 0 : type[0];
            tr.hidden = get("hidden") == "yes";
            tr.name = XmlTag(xml, "name", close, end);
            tr.detail = XmlTag(xml, "detail", close, end);
        }
        at = end;
    }
}

std::string Sanitize(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

/// trophies.tsv: "#lang<TAB>code" then id, grade, hidden, name, detail per line.
bool ReadTrophyTsv(const fs::path& file, const std::string& language, TrophySet& t) {
    std::stringstream ss(ReadText(file));
    std::string line;
    bool right_language = false, any = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream ls(line);
        std::string field;
        while (std::getline(ls, field, '\t')) f.push_back(field);
        if (f.size() >= 2 && f[0] == "#lang") right_language = f[1] == language;
        if (f.size() >= 2 && f[0] == "#title") t.title_name = f[1];
        if (f.size() < 5 || f[0].empty() || !std::isdigit(static_cast<unsigned char>(f[0][0]))) continue;
        for (auto& tr : t.list)
            if (tr.id == std::atoi(f[0].c_str())) {
                tr.grade = f[1].empty() ? 0 : f[1][0];
                tr.hidden = f[2] == "1";
                tr.name = f[3];
                tr.detail = f[4];
                any = true;
            }
    }
    return any && right_language;
}

void WriteTrophyTsv(const TrophySet& t) {
    std::string out = "#lang\t" + t.language + "\n#title\t" + Sanitize(t.title_name) + "\n";
    for (const auto& tr : t.list)
        if (tr.id < 100)  // OCTR's own (ctr_trophies.h) are not the game's: the runtime counts these
        out += std::to_string(tr.id) + "\t" + std::string(1, tr.grade ? tr.grade : '?') + "\t" + (tr.hidden ? "1" : "0") + "\t" +
               Sanitize(tr.name) + "\t" + Sanitize(tr.detail) + "\n";
    std::error_code ec;
    fs::create_directories(TrophyDir(t.title_id), ec);
    WriteTextAtomic(TrophyDir(t.title_id) / L"trophies.tsv", out);
}

void ReadUnlocked(TrophySet& t) {
    for (auto& tr : t.list) tr.unlocked = 0;
    std::stringstream ss(ReadText(TrophyDir(t.title_id) / L"unlocked.txt"));
    std::string line;
    while (std::getline(ss, line)) {
        int id = -1;
        unsigned long long at = 0;
        if (std::sscanf(line.c_str(), "%d %llu", &id, &at) != 2) continue;
        for (auto& tr : t.list)
            if (tr.id == id && !tr.unlocked) tr.unlocked = at ? at : 1;
    }
    std::error_code ec;
    t.unlocked_time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
}

/// The trophy key the launcher keeps, else shadPS4's (keys.json "ReleaseTrophyKey").
std::string ShadPs4TrophyKey();

void LoadTrophies() {
    App& a = g_app;
    ReleaseTrophies(g_trophies);
    TrophySet& t = g_trophies;
    t.game = a.game_dir;
    t.language = GameLanguage();
    t.title_id = Narrow(GameTitle());
    const fs::path trp = a.game_dir / L"sce_sys" / L"trophy" / L"trophy00.trp";
    const std::string data = ReadText(trp);
    auto be32 = [&](size_t o) {
        return uint32_t(uint8_t(data[o])) << 24 | uint32_t(uint8_t(data[o + 1])) << 16 | uint32_t(uint8_t(data[o + 2])) << 8 |
               uint32_t(uint8_t(data[o + 3]));
    };
    auto be64 = [&](size_t o) { return uint64_t(be32(o)) << 32 | be32(o + 4); };
    if (data.size() < 0x60 || be32(0) != 0xDCA24D00) {
        t.error = a.game_dir.empty() ? T("Elige primero el juego en la pestaña Jugar.", "Choose the game in the Play tab first.")
                                     : T("Este juego no trae el archivo de trofeos (sce_sys\\trophy\\trophy00.trp).",
                                         "This game has no trophy file (sce_sys\\trophy\\trophy00.trp).");
        return;
    }
    const uint32_t count = be32(0x10), entry_size = be32(0x14);
    std::map<std::string, std::string> entries;
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        const size_t e = 0x60 + size_t(i) * entry_size;
        if (e + 48 > data.size()) break;
        const std::string name(data.c_str() + e, strnlen(data.c_str() + e, 32));
        const uint64_t pos = be64(e + 32), size = be64(e + 40);
        if (pos + size <= data.size()) entries[name] = data.substr(size_t(pos), size_t(size));
    }
    for (const auto& [name, bytes] : entries) {
        int id = -1;
        if (name.size() == 11 && name.rfind("TROP", 0) == 0 && name.substr(7) == ".PNG" && std::sscanf(name.c_str() + 4, "%3d", &id) == 1) {
            Trophy tr;
            tr.id = id;
            tr.icon = PngTexture(bytes);
            t.list.push_back(std::move(tr));
        }
    }
    std::sort(t.list.begin(), t.list.end(), [](const Trophy& x, const Trophy& y) { return x.id < y.id; });
    if (entries.count("ICON0.PNG")) t.icon = PngTexture(entries["ICON0.PNG"]);
    // The NP communication ID (npbind.dat), needed to decrypt.
    const std::string npbind = ReadText(a.game_dir / L"sce_sys" / L"npbind.dat");
    for (size_t i = 0; i + 12 <= npbind.size(); ++i)
        if (npbind.compare(i, 4, "NPWR") == 0 && npbind[i + 9] == '_') {
            t.np_comm_id = npbind.substr(i, 12);
            break;
        }
    // Names: trophies.tsv in this language, else decrypt with the key.
    if (ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, t)) {
        t.names = true;
    } else {
        const std::string key = TrophyKeyBytes(a.launcher.Get("trophy_key"));
        if (!key.empty() && !t.np_comm_id.empty()) {
            char localized[32];
            std::snprintf(localized, sizeof localized, "TROP_%02d.ESFM", std::atoi(t.language.c_str()));
            const std::string file = entries.count(localized) ? localized : "TROP.ESFM";
            const std::string xml = entries.count(file) ? DecryptEsfm(key, t.np_comm_id, entries[file]) : std::string{};
            if (!xml.empty()) {
                ParseTrophyXml(xml, t);
                t.names = true;
                WriteTrophyTsv(t);
            } else {
                t.note = T("La clave de trofeos no descifra este juego: revisa que sea la de tu PS4 (32 cifras hexadecimales).",
                           "The trophy key does not decrypt this game: check it is your PS4's (32 hex digits).");
            }
        }
    }
    // Without the PS4's trophy key: CTR's own list (src/ctr_trophies.h), as the runtime's notice
    // uses it: official names, grades and what to do. Until now these stayed "Trophy #n" with no
    // grade, and the progress counted only OCTR's trophy below.
    if (!t.names && (t.title_id == "CUSA13795" || t.title_id == "CUSA14876")) {
        for (auto& tr : t.list)
            for (const auto& c : kCtrTrophies)
                if (c.id == tr.id) {
                    tr.grade = c.grade;
                    tr.name = c.name;
                    tr.detail = T(c.es, c.en);
                }
    }
    // OCTR's own trophies, past the game's (src/ctr_trophies.h; the server gives them).
    if (!t.list.empty() && (t.title_id == "CUSA13795" || t.title_id == "CUSA14876")) {
        for (const auto& o : octr_trophies) {
            Trophy tr;
            tr.id = o.id;
            tr.grade = o.grade;
            tr.name = T(o.name_es, o.name_en);
            tr.detail = T(o.es, o.en);
            t.list.push_back(std::move(tr));
        }
    }
    ReadUnlocked(t);
}

void TrophiesTab() {
    App& a = g_app;
    const std::string language = GameLanguage();
    if (g_trophies_reload || g_trophies.game != a.game_dir || g_trophies.language != language) {
        g_trophies_reload = false;
        LoadTrophies();
    }
    TrophySet& t = g_trophies;
    // The game writes unlocked.txt while it runs: re-read it when it changes.
    if (ImGui::GetTime() > t.next_poll && !t.title_id.empty()) {
        t.next_poll = ImGui::GetTime() + 1.0;
        std::error_code ec;
        const auto time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
        if (!ec && time != t.unlocked_time) ReadUnlocked(t);
    }
    if (!t.error.empty()) {
        BeginCard("trophy_error");
        ImGui::TextColored(kYellow, "%s  %s", ICON_INFO, t.error.c_str());
        EndCard();
        return;
    }
    auto points = [](char g) { return g == 'P' ? 180 : g == 'G' ? 90 : g == 'S' ? 30 : g == 'B' ? 15 : 0; };
    int unlocked = 0, earned = 0, total_points = 0;
    int by_grade[4][2]{};  // B S G P: unlocked, total
    for (const auto& tr : t.list) {
        unlocked += tr.unlocked != 0;
        total_points += points(tr.grade);
        if (tr.unlocked) earned += points(tr.grade);
        const int g = tr.grade == 'B' ? 0 : tr.grade == 'S' ? 1 : tr.grade == 'G' ? 2 : tr.grade == 'P' ? 3 : -1;
        if (g >= 0) {
            by_grade[g][1]++;
            if (tr.unlocked) by_grade[g][0]++;
        }
    }
    const int total = int(t.list.size());
    // By grade points, as a PS4 shows it, only when every trophy has its grade; else by count (one
    // graded trophy among unknown ones made the progress that trophy's alone: 0 % with 20 of 49).
    const bool every_graded = std::all_of(t.list.begin(), t.list.end(), [&](const Trophy& tr) { return points(tr.grade) > 0; });
    const float progress = every_graded && total_points ? float(earned) / float(total_points) : total ? float(unlocked) / float(total) : 0.0f;
    static const ImVec4 grade_colors[] = {ImVec4(0.82f, 0.54f, 0.32f, 1.0f), ImVec4(0.80f, 0.83f, 0.88f, 1.0f),
                                          ImVec4(1.0f, 0.80f, 0.29f, 1.0f), ImVec4(0.55f, 0.83f, 1.0f, 1.0f)};
    auto grade_index = [](char g) { return g == 'B' ? 0 : g == 'S' ? 1 : g == 'G' ? 2 : g == 'P' ? 3 : -1; };
    auto grade_name = [](int g) {
        static const char* es[] = {"Bronce", "Plata", "Oro", "Platino"};
        static const char* en[] = {"Bronze", "Silver", "Gold", "Platinum"};
        return g_spanish ? es[g] : en[g];
    };

    // Summary.
    BeginCard("trophy_summary");
    const float big = 72.0f * g_ui_scale;
    if (t.icon) {
        ImGui::Image(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(t.icon))), ImVec2(big * 1.78f, big));
        ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
    }
    ImGui::BeginGroup();
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(t.title_name.empty() ? "Crash Team Racing Nitro-Fueled" : t.title_name.c_str());
    if (g_font_heading) ImGui::PopFont();
    ImGui::Text(T("%d de %d logros  ·  %d %%", "%d of %d trophies  ·  %d %%"), unlocked, total, int(progress * 100.0f + 0.5f));
    ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 8.0f * g_ui_scale), "");
    if (t.names) {
        for (int g = 3; g >= 0; --g) {
            if (!by_grade[g][1]) continue;
            ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F");
            ImGui::SameLine(0.0f, 4.0f * g_ui_scale);
            ImGui::Text("%s %d/%d", grade_name(g), by_grade[g][0], by_grade[g][1]);
            ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
        }
        ImGui::NewLine();
    }
    ImGui::EndGroup();
    EndCard();

    // Names: the player's trophy key.
    if (!t.names) {
        ImGui::Spacing();
        BeginCard("trophy_key");
        ImGui::TextColored(kBlue, "%s", ICON_INFO);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(T("Los logros se guardan igual al jugar y aquí se ven con su imagen. Sus nombres y descripciones "
                                 "vienen cifrados en el juego con la clave de trofeos de la PS4, que no se incluye: si la tienes "
                                 "(es la misma que usa shadPS4), pégala o impórtala para verlos.",
                                 "Trophies are recorded while you play and shown here with their pictures. Their names and "
                                 "descriptions are encrypted in the game with the PS4's trophy key, which is not included: if "
                                 "you have it (the same shadPS4 uses), paste or import it to see them."));
        ImGui::PopTextWrapPos();
        if (!t.note.empty()) ImGui::TextColored(kRed, "%s", t.note.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        ImGui::InputTextWithHint("##trophykey", T("Clave de trofeos (32 cifras hex)", "Trophy key (32 hex digits)"), g_trophy_key_input,
                                 sizeof g_trophy_key_input, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button(T("Guardar", "Save"))) {
            if (TrophyKeyBytes(g_trophy_key_input).empty()) {
                a.notice = T("La clave tiene que tener 32 cifras hexadecimales.", "The key must be 32 hex digits.");
            } else {
                a.launcher.Set("trophy_key", g_trophy_key_input);
                SaveLauncher();
                g_trophies_reload = true;
            }
            std::memset(g_trophy_key_input, 0, sizeof g_trophy_key_input);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Importar de shadPS4", "Import from shadPS4"))) {
            const std::string key = ShadPs4TrophyKey();
            if (TrophyKeyBytes(key).empty()) {
                a.notice = T("shadPS4 no tiene una clave de trofeos guardada (keys.json).",
                             "shadPS4 has no trophy key saved (keys.json).");
            } else {
                a.launcher.Set("trophy_key", key);
                SaveLauncher();
                g_trophies_reload = true;
            }
        }
        EndCard();
    }

    // Filter and actions.
    ImGui::Spacing();
    ImGui::RadioButton(T("Todos", "All"), &g_trophy_filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton(T("Desbloqueados", "Unlocked"), &g_trophy_filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton(T("Por conseguir", "Locked"), &g_trophy_filter, 2);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13.0f);
    if (ImGui::Button((std::string(ICON_FOLDER "  ") + T("Carpeta", "Folder")).c_str())) {
        std::error_code ec;
        fs::create_directories(TrophyDir(t.title_id), ec);
        OpenInShell(TrophyDir(t.title_id));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!unlocked || a.game_running);
    if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Reiniciar", "Reset")).c_str())) g_trophy_reset_confirm = true;
    ImGui::EndDisabled();

    // The list.
    ImGui::Spacing();
    const float icon = 60.0f * g_ui_scale;
    if (LauncherUI::Table table{"trophies", 3}) {
        table.FixedColumn("icon", icon);
        table.StretchColumn("text");
        table.FixedColumn("state", ImGui::GetFontSize() * 9.0f);
        // The latest unlocked first, then the rest in the game's order.
        std::vector<const Trophy*> order;
        for (const auto& tr : t.list) order.push_back(&tr);
        std::stable_sort(order.begin(), order.end(), [](const Trophy* x, const Trophy* y) {
            if (bool(x->unlocked) != bool(y->unlocked)) return bool(x->unlocked);
            return x->unlocked > y->unlocked;
        });
        for (const Trophy* item : order) {
            const Trophy& tr = *item;
            if ((g_trophy_filter == 1 && !tr.unlocked) || (g_trophy_filter == 2 && tr.unlocked)) continue;
            const bool secret = tr.hidden && !tr.unlocked;
            table.Row(icon + 8.0f * g_ui_scale);
            table.Cell();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (tr.icon && !secret) {
                dl->AddImageRounded(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(tr.icon))), p, ImVec2(p.x + icon, p.y + icon),
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    tr.unlocked ? IM_COL32_WHITE : IM_COL32(110, 110, 120, 255), 8.0f * g_ui_scale);
            } else {
                dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), Col(kFrame), 8.0f * g_ui_scale);
                const ImVec2 q = ImGui::CalcTextSize("?");
                dl->AddText(ImVec2(p.x + (icon - q.x) * 0.5f, p.y + (icon - q.y) * 0.5f), Col(kGrey), "?");
            }
            if (!tr.unlocked) dl->AddRect(p, ImVec2(p.x + icon, p.y + icon), Col(kBorder), 8.0f * g_ui_scale);
            ImGui::Dummy(ImVec2(icon, icon));
            table.Cell();
            if (g_font_heading) ImGui::PushFont(g_font_heading, 0.0f);
            std::string name = secret ? T("Logro oculto", "Hidden trophy")
                               : tr.name.empty() ? std::string(T("Logro #", "Trophy #")) + std::to_string(tr.id)
                                                 : tr.name;
            ImGui::TextColored(tr.unlocked ? kText : kGrey, "%s", name.c_str());
            if (g_font_heading) ImGui::PopFont();
            if (secret) ImGui::TextColored(kGrey, "%s", T("Sigue jugando para descubrirlo.", "Keep playing to find it."));
            else if (!tr.detail.empty()) ImGui::TextColored(kGrey, "%s", tr.detail.c_str());
            table.Cell();
            const int g = grade_index(tr.grade);
            if (g >= 0) {
                ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F %s", grade_name(g));
            }
            if (tr.unlocked > 1) {
                const std::time_t when = std::time_t(tr.unlocked);
                std::tm local{};
                LocalTime(&local, &when);
                char date[32];
                std::strftime(date, sizeof date, "%d/%m/%Y %H:%M", &local);
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, date);
            } else if (tr.unlocked) {
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, T("Conseguido", "Unlocked"));
            } else {
                ImGui::TextColored(kGrey, "%s", T("Por conseguir", "Locked"));
            }
        }
    }
    ImGui::TextColored(kGrey, "%s", T("Se desbloquean jugando, como en la PS4; el platino llega solo al tener todos los demás.",
                                      "They unlock as you play, as on a PS4; the platinum comes once all the others are."));

    if (g_trophy_reset_confirm) {
        ImGui::OpenPopup("trophy_reset");
        g_trophy_reset_confirm = false;
    }
    if (ImGui::BeginPopupModal("trophy_reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("¿Volver a bloquear todos los logros? Se guarda una copia por si acaso.",
                                 "Lock every trophy again? A copy is kept just in case."));
        if (ImGui::Button(T("Reiniciar", "Reset"))) {
            std::error_code ec;
            const fs::path file = TrophyDir(t.title_id) / L"unlocked.txt";
            fs::path backup = file;
            backup += L"." + std::to_wstring(std::time(nullptr)) + L".bak";
            fs::rename(file, backup, ec);
            if (ec) a.notice = std::string(T("No se pudo reiniciar: ", "Could not reset: ")) + ec.message();
            ReadUnlocked(t);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/// Restarts the launcher (after an update or bringing another installation's settings): the
/// packaged name when it is next to the running one. False when it could not be started.
bool RestartLauncher(const std::wstring& args);

/// Asked on the first start of a new folder when another installation is found (and from the
/// Tools tab): copies its settings and data here (settings_import.h), then restarts.
void ImportPopup() {
    App& a = g_app;
    if (g_import.ask && !a.game_running) {
        ImGui::OpenPopup("import_settings");
        g_import.ask = false;
    }
    if (!ImGui::BeginPopupModal("import_settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    const ImportState state = g_import.state;
    ImGui::TextUnformatted(T("¿Traer tus ajustes de otra instalación?", "Bring your settings from another installation?"));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextColored(kGrey, "%s", T("Se copian a esta carpeta tus ajustes, controles, apodo, carpeta del juego, partidas "
                                      "guardadas, logros y DLC. La otra carpeta no se toca.",
                                      "Your settings, controls, nickname, game folder, saves, trophies and DLC are copied into "
                                      "this folder. The other folder is left as it is."));
    ImGui::PopTextWrapPos();
    if (g_import.choice >= int(g_import.from.size())) g_import.choice = 0;
    ImGui::BeginDisabled(state == ImportState::Copying);
    if (g_import.from.size() == 1) {
        ImGui::TextUnformatted(PathText(g_import.from[0]).c_str());
    } else {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
        if (ImGui::BeginCombo("##import_from", PathText(g_import.from[size_t(g_import.choice)]).c_str())) {
            for (int i = 0; i < int(g_import.from.size()); ++i)
                if (ImGui::Selectable(PathText(g_import.from[size_t(i)]).c_str(), i == g_import.choice)) g_import.choice = i;
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    if (state == ImportState::Copying) {
        ImGui::TextColored(kYellow, "%s", T("Copiando...", "Copying..."));
    } else if (state == ImportState::Failed) {
        ImGui::TextColored(kRed, T("No se pudo copiar: %s", "Could not copy: %s"), g_import.error.c_str());
    } else if (state == ImportState::Done) {
        ImGui::CloseCurrentPopup();
        if (!RestartLauncher(L""))
            a.notice = T("Ajustes copiados. Cierra y vuelve a abrir el launcher.", "Settings copied. Close and open the launcher again.");
    }
    ImGui::BeginDisabled(state == ImportState::Copying);
    if (ImGui::Button(T("Traer mis ajustes", "Bring my settings"))) {
        g_import.state = ImportState::Copying;
        const fs::path from = g_import.from[size_t(g_import.choice)], to = a.data;
        std::thread([from, to] {
            const std::string error = SettingsImport::Copy(from, to);
            g_import.error = error;
            g_import.state = error.empty() ? ImportState::Done : ImportState::Failed;
        }).detach();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("No, empezar de cero", "No, start fresh"))) {
        a.launcher.Set("import_offered", "no");
        SaveLauncher();
        g_import.state = ImportState::Idle;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

fs::path LatestRunLog() {
    fs::path latest;
    fs::file_time_type newest = fs::file_time_type::min();
    for (const auto& folder : {g_app.data, g_app.data / L"logs"}) {
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = PathText(it->path().filename());
            if (it->path().extension() != L".log" ||
                (folder == g_app.data && name != "last_run.log" && name.rfind("last_run_", 0) != 0)) continue;
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec)) continue;
            const auto modified = it->last_write_time(file_ec);
            if (!file_ec && (latest.empty() || modified > newest)) {
                latest = it->path();
                newest = modified;
            }
        }
    }
    return latest;
}

void OpenRunLog() {
    const auto path = LatestRunLog();
    if (!path.empty()) OpenInShell(path);
    else g_app.notice = T("Todavía no hay registro. Inicia el juego primero.",
                          "No run log yet. Start the game first.");
}

/// Tools > Data: another installation's settings, the folders, the log and the shader cache.
void ToolsDataPage() {
    App& a = g_app;
    FormSection(T("Otra instalación", "Another installation"));
    ImGui::BeginDisabled(g_import.state == ImportState::Copying || a.game_running);
    if (ImGui::Button(T("Traer ajustes de otra instalación...", "Bring settings from another installation..."))) {
        if (const auto dir = PickFolder(a.hwnd, L"Otra instalación del port / Another installation of the port",
                                        a.root.parent_path())) {
            std::error_code ec;
            if (SettingsImport::IsUsedInstall(*dir) && !fs::equivalent(*dir, a.root, ec)) {
                g_import.from = {*dir};
                g_import.choice = 0;
                g_import.ask = true;
            } else {
                a.notice = T("Esa carpeta no es otra instalación del port usada (le falta run.py o sus ajustes).",
                             "That folder is not another used installation of the port (no run.py or settings).");
            }
        }
    }
    ImGui::EndDisabled();
    HelpMarker(T("Copia a esta carpeta los ajustes, controles, apodo, carpeta del juego, partidas guardadas, logros y DLC de "
                 "otra carpeta del port (por ejemplo, la de una versión anterior). La otra carpeta no se toca.",
                 "Copies the settings, controls, nickname, game folder, saves, trophies and DLC of another folder of the "
                 "port (an older release's, say) into this one. The other folder is left as it is."));
    FormSection(T("Carpetas y registro", "Folders and log"));
    if (ImGui::Button(T("Abrir last_run.log", "Open last_run.log"))) OpenRunLog();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir la carpeta del port", "Open the port folder"))) OpenInShell(PortFolder());
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir partidas guardadas", "Open saves"))) {
        std::error_code ec;
        fs::create_directories(UserRoot(), ec);
        OpenInShell(UserRoot());
    }
    LauncherFlag("save_log", T("Registro detallado", "Detailed log"), false,
                 T("Guarda en logs\\ un registro con estadísticas de fotogramas. Para reportar fallos o tirones: "
                   "adjunta ese archivo en un issue.",
                   "Saves a log with frame statistics in logs\\. To report crashes or stutter: attach that file to an issue."));

    FormSection(T("Caché de shaders", "Shader cache"));
    FormNote(T("Los shaders se guardan para que los siguientes arranques vayan fluidos. Bórrala solo si "
               "actualizaste el driver de la GPU o ves fallos gráficos.",
               "Shaders are kept so later sessions run smoothly. Clear it only after a GPU driver "
               "update or if you see graphics glitches."));
    ImGui::BeginDisabled(a.game_running);
    if (ImGui::Button(T("Borrar la caché de shaders", "Clear the shader cache"))) a.confirm_clear_cache = true;
    ImGui::EndDisabled();
}

/// Tools > Shortcuts: the Desktop and Steam.
void ToolsShortcutsPage() {
    App& a = g_app;
    FormSection(T("Accesos directos", "Shortcuts"));
    if (ImGui::Button(T("Crear acceso directo en el Escritorio", "Create Desktop Shortcut"))) {
        std::string err;
        if (CreateDesktopShortcut(err)) {
            a.notice = T("Acceso directo creado en el Escritorio con éxito (inicia directo sin el launcher).",
                         "Desktop shortcut created successfully (launches directly without launcher).");
        } else {
            a.notice = err;
        }
    }
    HelpMarker(T("Crea un acceso directo en tu Escritorio para iniciar directamente el juego con su icono sin abrir la ventana del launcher.",
                 "Creates a shortcut on your Desktop to start the game directly with its icon without the launcher window."));
    SteamButton();
    HelpMarker(T("Lo añade a tu biblioteca de Steam como juego no-Steam, con sus portadas. Abre el juego directo, "
                 "sin la ventana del launcher, así que la grabación y el overlay de Steam ven el juego. Tus "
                 "ajustes, el online y los mods se mantienen.",
                 "Adds it to your Steam library as a non-Steam game, with its artwork. It opens the game "
                 "directly, without the launcher window, so Steam's recording and overlay see the game. Your "
                 "settings, online and mods still apply."));
}

/// Tools > Language: the launcher's own language.
void ToolsLanguagePage() {
    FormSection(T("Idioma del launcher", "Launcher language"));
    const char* language_names[] = {"Auto (Windows)", "English", "Español", "Português (Brasil)", "Français",
                                    "Deutsch", "Italiano", "日本語", "简体中文", "Русский", "Bahasa Indonesia"};
    const char* language_codes[] = {"auto", "en", "es", "pt_BR", "fr", "de", "it", "ja", "zh_CN", "ru", "id"};
    constexpr int language_count = int(sizeof language_codes / sizeof *language_codes);
    int selected_language = 0;
    for (int i = 0; i < language_count; ++i) if (g_language == language_codes[i]) selected_language = i;
    if (ImGui::Combo(FormLabel(T("Idioma", "Language")), &selected_language, language_names, language_count)) {
        g_language = language_codes[selected_language];
        ApplyLauncherLanguage();
        SaveLauncher();
    }
    FormNote(T("Solo el launcher. El idioma del juego está en Ajustes > Juego.",
               "The launcher only. The game's language is in Settings > Game."));
}

/// Tools > About: credits, licences and links.
void ToolsAboutPage() {
    App& a = g_app;
    FormSection(T("Acerca de", "About"));
    ImGui::Text("CTR-NitroFueled-PC %s", a.version.c_str());
    ImGui::PushTextWrapPos(0.0f);
#ifdef _WIN32
    ImGui::TextUnformatted(T("Fork de Windows de SacredFire (T3CHNOLOG1C), basado en bbport (deadinside28), el port de Windows "
                             "de Supermedo y shadPS4. Parches de illusion y TroyWarez. Sin afiliación con Sony, Activision "
                             "ni Beenox. Licencia GPL v2 o posterior.",
                             "Windows fork of SacredFire (T3CHNOLOG1C), built on bbport (deadinside28), Supermedo's Windows "
                             "port and shadPS4. Patches by illusion and TroyWarez. Not affiliated with Sony, Activision or "
                             "Beenox. GPL v2 or later."));
    ImGui::Spacing();
    ImGui::TextUnformatted(T("Escalado temporal: AMD FidelityFX FSR 3.1 (MIT) y NVIDIA DLSS. DLSS usa el SDK de "
                             "NVIDIA DLSS (nvngx_dlss.dll, redistribuido según su licencia, en licenses\\) a través del "
                             "puente de IFreemz (MIT). NVIDIA, DLSS y GeForce RTX son marcas de NVIDIA Corporation.",
                             "Temporal upscaling: AMD FidelityFX FSR 3.1 (MIT) and NVIDIA DLSS. DLSS uses the NVIDIA DLSS "
                             "SDK (nvngx_dlss.dll, redistributed under its license, in licenses\\) through IFreemz's "
                             "bridge (MIT). NVIDIA, DLSS and GeForce RTX are trademarks of NVIDIA Corporation."));
#else
    ImGui::TextUnformatted(T("SacredFire para Windows y Linux, basado en bbport (deadinside28), SacredFire (T3CHNOLOG1C), "
                             "el port de Supermedo y shadPS4. Parches de illusion y TroyWarez. Sin afiliación con Sony, "
                             "Activision ni Beenox. GPL v2 o posterior. Escalado temporal: AMD FidelityFX FSR 3.1 (MIT).",
                             "SacredFire for Windows and Linux, built on bbport (deadinside28), SacredFire (T3CHNOLOG1C), "
                             "Supermedo's port and shadPS4. Patches by illusion and TroyWarez. Not affiliated with Sony, "
                             "Activision or Beenox. GPL v2 or later. Temporal upscaling: AMD FidelityFX FSR 3.1 (MIT)."));
#endif
    ImGui::PopTextWrapPos();
    {
        const bool dlss = DlssFilesPresent();
        ImGui::TextColored(kGrey, "%s", dlss ? T("DLSS: archivos presentes (necesita una tarjeta NVIDIA RTX)",
                                                 "DLSS: files present (needs an NVIDIA RTX card)")
                                             : T("DLSS: archivos no incluidos (se usa FSR 3)",
                                                 "DLSS: files not included (FSR 3 is used)"));

        ImGui::TextColored(kGrey, "%s", Fsr4FilesPresent()
                                            ? T("FSR 4: archivos presentes (necesita una gráfica con soporte INT8)",
                                                "FSR 4: files present (needs a GPU with INT8 support)")
                                            : T("FSR 4: archivos no incluidos (carpeta fsr4_shaders)",
                                                "FSR 4: files not included (fsr4_shaders folder)"));
    }
    ImGui::Spacing();
    ImGui::TextColored(kGrey, "%s", T("Página oficial:", "Official site:"));
    ImGui::SameLine();
    if (ImGui::TextLink(kSiteUrl)) OpenInShell(fs::path(Widen(kSiteUrl)));
    ImGui::TextColored(kGrey, "%s", T("Código fuente:", "Source code:"));
    ImGui::SameLine();
    if (ImGui::TextLink(kPortRepoUrl)) OpenInShell(fs::path(Widen(kPortRepoUrl)));
    ImGui::TextColored(kGrey, "%s", T("Proyecto original:", "Original project:"));
    ImGui::SameLine();
    if (ImGui::TextLink(kRepoUrl)) OpenInShell(fs::path(Widen(kRepoUrl)));
    if (a.python) ImGui::TextColored(kGrey, "Python: %s", a.python->description.c_str());
}

void ToolsTab() {
    if (!ImGui::BeginTabBar("tools_pages")) return;
    SettingsPage(T("Actualizaciones", "Updates"), UpdateSection);
    SettingsPage(T("Datos y carpetas", "Data and folders"), ToolsDataPage);
    SettingsPage(T("Accesos directos", "Shortcuts"), ToolsShortcutsPage);
    SettingsPage(T("Idioma", "Language"), ToolsLanguagePage);
    SettingsPage(T("Acerca de", "About"), ToolsAboutPage);
    ImGui::EndTabBar();
}

void ClearShaderCache() {
    std::error_code ec;
    const fs::path cache = UserRoot() / L"cache" / GameTitle();
    const auto removed = fs::remove_all(cache, ec);
    g_app.notice = ec ? std::string(T("No se pudo borrar la caché: ", "Could not clear the cache: ")) + ec.message()
                      : std::string(T("Caché borrada (", "Cache cleared (")) + std::to_string(removed) +
                            T(" archivos).", " files).");
}

void HeroHeader() {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 270.0f;
    const ImVec2 end{origin.x + width, origin.y + height};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (g_hero_art.texture) {
        const float image_aspect = float(g_hero_art.width) / float(g_hero_art.height);
        const float view_aspect = width / height;
        ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
        if (image_aspect < view_aspect) {
            const float visible = image_aspect / view_aspect;
            uv0.y = (1.0f - visible) * 0.5f;
            uv1.y = 1.0f - uv0.y;
        } else {
            const float visible = view_aspect / image_aspect;
            uv0.x = (1.0f - visible) * 0.5f;
            uv1.x = 1.0f - uv0.x;
        }
        draw->AddImage((ImTextureID)(intptr_t)g_hero_art.texture, origin, end, uv0, uv1);
    } else {
        draw->AddRectFilled(origin, end, IM_COL32(48, 32, 20, 255));
    }
    if (g_logo_art.texture) {
        const float max_w = std::min(width * 0.34f, 390.0f);
        const float max_h = height * 0.78f;
        const float aspect = float(g_logo_art.width) / float(g_logo_art.height);
        float logo_w = max_w, logo_h = logo_w / aspect;
        if (logo_h > max_h) { logo_h = max_h; logo_w = logo_h * aspect; }
        const ImVec2 lo{origin.x + 28.0f, origin.y + (height - logo_h) * 0.5f};
        const ImVec2 hi{lo.x + logo_w, lo.y + logo_h};
        draw->AddImage((ImTextureID)(intptr_t)g_logo_art.texture, lo, hi);
    }
    draw->AddRectFilled(ImVec2(origin.x, end.y - 4.0f), end, IM_COL32(238, 169, 48, 255));
    ImGui::Dummy(ImVec2(width, height));
}

void WindowBar(float width) {
    const float height = 40.0f;
    ImGui::SetCursorPos(ImVec2(0, 0));
    const ImVec2 p = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(7, 72, 145, 255));
    draw->AddRectFilled(ImVec2(p.x, p.y + height - 1), ImVec2(p.x + width, p.y + height),
                        IM_COL32(44, 166, 228, 255));
    const float bw = 42.0f, bh = 30.0f, gap = 3.0f;
    const float left = width - 3.0f * bw - 2.0f * gap - 8.0f;
    auto button = [&](const char* id, int kind) {
        ImGui::SetCursorPos(ImVec2(left + kind * (bw + gap), 5.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.25f, 0.47f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.40f, 0.66f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.55f, 0.78f, 1));
        const bool clicked = ImGui::Button(id, ImVec2(bw, bh));
        ImGui::PopStyleColor(3);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImVec2 c{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const ImU32 ink = IM_COL32(255, 234, 198, 255);
        if (kind == 0) draw->AddLine(ImVec2(c.x - 5, c.y), ImVec2(c.x + 5, c.y), ink, 1.6f);
        else if (kind == 1) draw->AddRect(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), ink, 0, 0, 1.4f);
        else {
            draw->AddLine(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), ink, 1.6f);
            draw->AddLine(ImVec2(c.x + 5, c.y - 5), ImVec2(c.x - 5, c.y + 5), ink, 1.6f);
        }
        return clicked;
    };
    if (button("##minimize", 0)) HideLauncher(false);
    if (button("##maximize", 1)) ToggleMaximize();
    if (button("##close", 2)) RequestClose();
    g_titlebar_drag = DragRect{0, 0, int(left), int(height)};
    ImGui::SetCursorPos(ImVec2(14, height + 10));
}

bool LoadArtworkFile(const fs::path& path, LauncherArtwork& artwork);

void LoadArtwork() {
    const fs::path art = g_app.root / L"art";
    LoadArtworkFile(art / L"ctr-hero.png", g_hero_art);
    LoadArtworkFile(art / L"ctr-logo.png", g_logo_art);
    LoadArtworkFile(art / L"ctr-menu-background.png", g_menu_background);
    LoadArtworkFile(art / L"pad-xbox.png", g_pad_xbox_art);
    LoadArtworkFile(art / L"pad-playstation.png", g_pad_ps_art);
}

void ReleaseArtwork() {
    for (auto* a : {&g_hero_art, &g_logo_art, &g_menu_background, &g_pad_xbox_art, &g_pad_ps_art}) {
        ReleaseTexture(a->texture);
        *a = {};
    }
}

void DrawMenuIcon(ImDrawList* d, int id, ImVec2 c, ImU32 ink, float s, ImU32 cut) {
    const ImU32 navy = IM_COL32(6, 39, 89, 255);
    auto line = [&](ImVec2 a, ImVec2 b, float t = 2.2f) { d->AddLine(a, b, navy, t * s); d->AddLine(a, b, ink, t * s * 0.58f); };
    // icons 1-9: solid shapes in a 20 x 20 box, details cut out in the entry's own colour
    const auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    if (id == 0) { // checkered flag / race
        line({c.x-7*s,c.y+8*s},{c.x-7*s,c.y-8*s},2.5f); d->AddRectFilled({c.x-5*s,c.y-8*s},{c.x+8*s,c.y+1*s},ink,1.5f*s);
        d->AddRectFilled({c.x-2*s,c.y-8*s},{c.x+1*s,c.y-3.5f*s},navy); d->AddRectFilled({c.x+4*s,c.y-3.5f*s},{c.x+7*s,c.y+1*s},navy);
    } else if (id == 1) { // gear
        for (int i = 0; i < 8; ++i) {
            const float a = float(i) * 0.785398f, ca = std::cos(a), sa = std::sin(a);
            const auto R = [&](float r, float t) { return P(ca * r - sa * t, sa * r + ca * t); };
            d->AddQuadFilled(R(5.0f, -1.9f), R(9.2f, -1.5f), R(9.2f, 1.5f), R(5.0f, 1.9f), ink);
        }
        d->AddCircleFilled(c, 6.8f * s, ink, 20);
        d->AddCircleFilled(c, 2.8f * s, cut, 16);
    } else if (id == 2) { // globe
        const float t = 1.7f * s;
        d->AddCircle(c, 8.5f * s, ink, 24, t);
        d->AddEllipse(c, ImVec2(3.6f * s, 8.5f * s), ink, 0.0f, 20, t);
        d->AddLine(P(-8.5f, 0), P(8.5f, 0), ink, t);
        d->AddLine(P(-7.2f, -4.4f), P(7.2f, -4.4f), ink, t * 0.8f);
        d->AddLine(P(-7.2f, 4.4f), P(7.2f, 4.4f), ink, t * 0.8f);
    } else if (id == 3) { // gamepad
        d->AddRectFilled(P(-9.5f, -5.5f), P(9.5f, 3.0f), ink, 4.5f * s);
        d->AddEllipseFilled(P(-6.3f, 3.6f), ImVec2(3.6f * s, 4.4f * s), ink, 0.35f);
        d->AddEllipseFilled(P(6.3f, 3.6f), ImVec2(3.6f * s, 4.4f * s), ink, -0.35f);
        d->AddRectFilled(P(-7.6f, -1.9f), P(-3.0f, -0.5f), cut);  // d-pad
        d->AddRectFilled(P(-6.0f, -3.5f), P(-4.6f, 1.1f), cut);
        d->AddCircleFilled(P(4.6f, -2.6f), 1.2f * s, cut, 10);
        d->AddCircleFilled(P(6.8f, -0.4f), 1.2f * s, cut, 10);
    } else if (id == 4) { // keyboard
        d->AddRectFilled(P(-10, -6.5f), P(10, 6.5f), ink, 2.2f * s);
        for (int r = 0; r < 2; ++r)
            for (int k = 0; k < 6; ++k) {
                const float x = -7.6f + k * 2.9f - r * 0.0f, y = -4.2f + r * 3.0f;
                d->AddRectFilled(P(x, y), P(x + 1.9f, y + 1.9f), cut, 0.4f * s);
            }
        d->AddRectFilled(P(-5.0f, 2.4f), P(5.0f, 4.2f), cut, 0.5f * s);  // space bar
    } else if (id == 5) { // puzzle piece (mods)
        d->AddRectFilled(P(-8, -5), P(5, 8), ink, 1.5f * s);
        d->AddCircleFilled(P(-1.5f, -6.2f), 3.0f * s, ink, 14);
        d->AddCircleFilled(P(6.2f, 1.5f), 3.0f * s, ink, 14);
        d->AddCircleFilled(P(-8.0f, 1.5f), 2.4f * s, cut, 14);  // the left socket
    } else if (id == 6) { // floppy disk
        const ImVec2 body[] = {P(-8.5f, -8.5f), P(5.5f, -8.5f), P(8.5f, -5.5f), P(8.5f, 8.5f), P(-8.5f, 8.5f)};
        d->AddConvexPolyFilled(body, 5, ink);
        d->AddRectFilled(P(-5.0f, -8.5f), P(4.0f, -3.0f), cut, 0.6f * s);   // shutter
        d->AddRectFilled(P(1.0f, -7.5f), P(3.0f, -4.0f), ink);
        d->AddRectFilled(P(-5.5f, 1.0f), P(5.5f, 8.5f), cut, 0.8f * s);     // label
        d->AddLine(P(-3.5f, 3.8f), P(3.5f, 3.8f), ink, 1.0f * s);
        d->AddLine(P(-3.5f, 6.0f), P(3.5f, 6.0f), ink, 1.0f * s);
    } else if (id == 7) { // gift box (DLC)
        d->AddRectFilled(P(-7.5f, -1.5f), P(7.5f, 8.5f), ink, 1.0f * s);
        d->AddRectFilled(P(-9.0f, -5.0f), P(9.0f, -1.0f), ink, 1.0f * s);
        d->AddRectFilled(P(-1.1f, -5.0f), P(1.1f, 8.5f), cut);              // ribbon
        d->AddLine(P(-9.0f, -1.2f), P(9.0f, -1.2f), cut, 0.9f * s);
        d->AddEllipse(P(-3.2f, -7.0f), ImVec2(3.0f * s, 1.8f * s), ink, 0.35f, 14, 1.6f * s);  // bow
        d->AddEllipse(P(3.2f, -7.0f), ImVec2(3.0f * s, 1.8f * s), ink, -0.35f, 14, 1.6f * s);
    } else if (id == 8) { // trophy
        const ImVec2 cup[] = {P(-6.5f, -8.5f), P(6.5f, -8.5f), P(6.0f, -4.0f), P(3.5f, 0.5f), P(0, 1.8f), P(-3.5f, 0.5f), P(-6.0f, -4.0f)};
        d->AddConvexPolyFilled(cup, 7, ink);
        d->PathArcTo(P(-6.2f, -4.8f), 3.2f * s, 1.57f, 4.71f, 12);
        d->PathStroke(ink, 0, 1.7f * s);
        d->PathArcTo(P(6.2f, -4.8f), 3.2f * s, -1.57f, 1.57f, 12);
        d->PathStroke(ink, 0, 1.7f * s);
        d->AddRectFilled(P(-1.3f, 1.0f), P(1.3f, 5.5f), ink);
        d->AddRectFilled(P(-5.5f, 5.5f), P(5.5f, 8.8f), ink, 1.0f * s);
        d->AddCircleFilled(P(0, -4.5f), 1.6f * s, cut, 10);                 // a gem on the cup
    } else { // wrench (tools)
        d->AddLine(P(-6.5f, 6.5f), P(3.0f, -3.0f), ink, 3.6f * s);
        d->AddCircleFilled(P(-6.5f, 6.5f), 1.8f * s, ink, 12);
        d->AddCircleFilled(P(4.6f, -4.6f), 4.6f * s, ink, 18);
        const ImVec2 jaw[] = {P(4.2f, -5.0f), P(7.0f, -10.0f), P(11.0f, -6.0f), P(5.0f, -4.2f)};
        d->AddConvexPolyFilled(jaw, 4, cut);                                 // the open jaw
        d->AddCircleFilled(P(-6.5f, 6.5f), 0.8f * s, cut, 8);
    }
}

bool MenuEntry(int id, const char* label, bool selected, float width) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 size{width, 44.0f * g_ui_scale};
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 a{start.x, start.y}, b{start.x + size.x, start.y + size.y};
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float scale = g_ui_scale;
    ImVec2 p0{a.x,a.y+4*scale}, p1{b.x-10*scale,a.y+4*scale}, p2{b.x,a.y+size.y-4*scale}, p3{a.x+10*scale,a.y+size.y-4*scale};
    const ImU32 shadow=IM_COL32(0,31,79,95), dark=hovered?IM_COL32(7,65,130,218):IM_COL32(6,55,113,178);
    const ImVec2 sh[]={ImVec2(p0.x,p0.y+3),ImVec2(p1.x,p1.y+3),ImVec2(p2.x,p2.y+3),ImVec2(p3.x,p3.y+3)};
    d->AddConvexPolyFilled(sh,4,shadow);
    const ImVec2 poly[]={p0,p1,p2,p3};
    d->AddConvexPolyFilled(poly,4,selected?IM_COL32(234,250,255,248):dark);
    if (selected) {
        d->AddPolyline(poly,4,IM_COL32(255,255,255,215),ImDrawFlags_Closed,1.6f);
        d->AddRectFilled({p3.x+3,p3.y-1},{p3.x+8,p3.y+1},IM_COL32(29,211,255,255));
    }
    const ImVec2 icon{a.x+27.0f*scale,a.y+size.y*0.5f};
    DrawMenuIcon(d,id,icon,selected?IM_COL32(9,64,112,255):IM_COL32(234,249,255,235),0.9f*g_ui_scale,
                 selected?IM_COL32(234,250,255,248):(hovered?IM_COL32(7,65,130,255):IM_COL32(6,55,113,255)));
    ImFont* label_font = g_ctr_font ? g_ctr_font : ImGui::GetFont();
    const float label_font_size = ImGui::GetFontSize();
    const ImVec2 text_size=label_font->CalcTextSizeA(label_font_size, FLT_MAX, 0.0f, label);
    const ImVec2 text{a.x+49.0f*scale,a.y+(size.y-text_size.y)*0.5f};
    d->AddText(label_font,label_font_size,text,selected?IM_COL32(4,46,84,255):IM_COL32(237,249,255,245),label);
    return clicked;
}

void Frame() {
    App& a = g_app;
    PollJobs();
    PollUpdate();
    EosBridgePoll();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (g_menu_background.texture) {
        const float wa=vp->WorkSize.x/vp->WorkSize.y;
        const float ia=float(g_menu_background.width)/float(g_menu_background.height);
        ImVec2 uv0{0,0},uv1{1,1};
        if (ia>wa) { const float v=wa/ia; uv0.x=(1-v)*0.5f; uv1.x=1-uv0.x; }
        else { const float v=ia/wa; uv0.y=(1-v)*0.5f; uv1.y=1-uv0.y; }
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)g_menu_background.texture,
                                             vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                                                                 vp->WorkPos.y + vp->WorkSize.y),uv0,uv1);
    }
    const float width = ImGui::GetWindowSize().x;
    WindowBar(width);
    HeroHeader();

    if (!a.notice.empty()) {
        ImGui::TextColored(kYellow, "%s", a.notice.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("OK")) a.notice.clear();
    }
    UpdateBanner();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025f, 0.19f, 0.40f, 0.78f));
    ImGui::BeginChild("menu_body", ImVec2(0, 0), ImGuiChildFlags_None);
    const float menu_w = 238.0f * g_ui_scale;
    const float nav_padding = 12.0f * g_ui_scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(nav_padding, 14.0f * g_ui_scale));
    ImGui::BeginChild("menu_navigation", ImVec2(menu_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::TextColored(ImVec4(0.76f, 0.92f, 1.0f, 0.9f), "%s", T("MENÚ PRINCIPAL", "MAIN MENU"));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    const char* pages[] = {T("Jugar", "Play"), T("Ajustes", "Settings"), "Online", T("Mando", "Gamepad"),
                           T("Teclado", "Keyboard"), "Mods", T("Guardados", "Saves"), "DLC",
                           T("Logros", "Achievements"), T("Herramientas", "Tools")};
    const float row_gap = 4.0f * g_ui_scale;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, row_gap));
    for (int i=0; i<10; ++i) {
        ImGui::SetCursorPosX(nav_padding);
        if (MenuEntry(i, pages[i], a.active_page == i, ImGui::GetContentRegionAvail().x)) {
            a.active_page = i;
            if (i != 3) g_pad_capture = -1;
            if (i != 3) PadWatchStop();
            if (i != 4) g_key_capture = -1;
        }
    }
    ImGui::PopStyleVar();
    // The official page and the source, always in sight (v0.8.8)
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 6.0f, ImGui::GetWindowHeight() - 64.0f));
    ImGui::SetCursorPosX(nav_padding);
    ImGui::PushStyleColor(ImGuiCol_TextLink, ImVec4(0.55f, 0.88f, 1.0f, 1.0f));
    if (ImGui::TextLink(T("Web oficial", "Official site"))) OpenInShell(fs::path(Widen(kSiteUrl)));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kSiteUrl);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(ImVec4(0.77f, 0.91f, 1.0f, 0.5f), "•");
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::TextLink("GitHub")) OpenInShell(fs::path(Widen(kPortRepoUrl)));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPortRepoUrl);
    ImGui::PopStyleColor();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 38.0f));
    ImGui::SetCursorPosX(nav_padding);
    ImGui::TextColored(ImVec4(0.77f, 0.91f, 1.0f, 0.84f), "CTR  •  %s", a.version.c_str());
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.015f, 0.13f, 0.32f, 0.82f));
    // Real child padding applies to every row, table, wrap edge and tab.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 17.0f));
    ImGui::BeginChild("menu_content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.91f, 0.97f, 1.0f, 1.0f));
    if (a.active_page > 0 && a.active_page < 10) {  // the page's name on top (Play has its banner)
        ImFont* title_font = g_ctr_font ? g_ctr_font : g_font_heading;
        if (title_font) ImGui::PushFont(title_font, ImGui::GetStyle().FontSizeBase * 1.6f);
        ImGui::TextUnformatted(pages[a.active_page]);
        if (title_font) ImGui::PopFont();
        ImGui::Spacing();
    }
    if (a.active_page == 3) ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    switch (a.active_page) {
    case 0: PlayTab(); break;
    case 1: SettingsTab(); break;
    case 2: OnlineTab(); break;
    case 3: GamepadTab(); break;
    case 4: KeyboardTab(); break;
    case 5: ModsTab(); break;
    case 6: SavesTab(); break;
    case 7: DlcTab(); break;
    case 8: TrophiesTab(); break;
    default: ToolsTab(); break;
    }
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    SaveConfirmations();
    NickPopup();
    JoinServerPopup();
    ImportPopup();  // before the diagnostic's question, which waits for it
    PerfReportsPopup();
    ShaderProgressPopup();
    if (a.confirm_clear_cache) {
        ImGui::OpenPopup("clear_cache");
        a.confirm_clear_cache = false;
    }
    if (ImGui::BeginPopupModal("clear_cache", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("¿Borrar la caché de shaders? El próximo arranque volverá a compilarlos.",
                                 "Clear the shader cache? The next start compiles them again."));
        if (ImGui::Button(T("Borrar", "Clear"))) {
            ClearShaderCache();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (a.want_close) {
        ImGui::OpenPopup("close");
        a.want_close = false;
    }
    if (ImGui::BeginPopupModal("close", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("El juego sigue abierto.", "The game is still running."));
        if (ImGui::Button(T("Cerrar el juego y el launcher", "Stop the game and quit"))) {
            StopGame();
            QuitLauncher();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

fs::path DataRoot(const fs::path& root);
void PrepareData();
void LoadState() {
    App& a = g_app;
    a.root = FindRoot();
    a.data = DataRoot(a.root);
    a.python = FindPython(a.root);
    PrepareData();
    std::error_code ec;
    a.bin = fs::is_directory(a.root / L"bin", ec) ? a.root / L"bin" : a.root / L"out";
    for (const fs::path& v : {a.bin / L"VERSION", a.root / L"VERSION", a.root / L"out" / L"VERSION"}) {
        if (fs::is_regular_file(v, ec)) {
            a.version = Trim(ReadText(v));
            break;
        }
    }
    // A folder never used (no launcher.ini): another installation's settings are offered once.
    const bool new_folder = !fs::exists(LauncherIni(), ec);
    // The portable package ships bbport.default.ini (an update unzipped over it keeps bbport.ini).
    if (!fs::exists(GameIni(), ec) && fs::is_regular_file(a.root / L"bbport.default.ini", ec)) {
        fs::copy_file(a.root / L"bbport.default.ini", GameIni(), ec);
    }
    a.launcher.Load(LauncherIni());
    a.game.Load(GameIni());
    ApplyPlatformCapabilities();
    if (const fs::path backup = BackupKeyPath(); !backup.empty()) {
        const fs::path installs = backup.parent_path() / L"installs.txt";
        if (new_folder && a.launcher.Get("import_offered").empty()) {
            g_import.from = SettingsImport::Previous(installs, a.data);
            g_import.ask = !g_import.from.empty();
        }
        SettingsImport::Remember(installs, a.data);
    }
    SyncOnlineBuffers();
    const std::string lang = a.launcher.Get("ui_language");
    g_language = lang.empty() ? "auto" : lang;
    ApplyLauncherLanguage();
    a.game_dir = TextPath(a.launcher.Get("game_dir"));
    a.base_dir = TextPath(a.launcher.Get("base_dir"));
    a.update_dir = TextPath(a.launcher.Get("update_dir"));
    // First start of a portable package: a merged folder next to the launcher is picked up.
    if (a.game_dir.empty()) {
        for (const wchar_t* name : {L"CUSA14876-1.21", L"CUSA13795-1.21", L"CUSA14876", L"CUSA13795"}) {
            if (Analyze(a.root / name).kind == Kind::Complete) {
                a.game_dir = a.root / name;
                break;
            }
        }
        for (const wchar_t* title : {L"CUSA14876", L"CUSA13795"}) {
            const std::wstring base = title, update = base + L"-patch";
            if (a.base_dir.empty() && Analyze(a.root / base).kind == Kind::Base) a.base_dir = a.root / base;
            if (a.update_dir.empty() && Analyze(a.root / update).kind == Kind::Update) a.update_dir = a.root / update;
        }
    }
    a.base_info = Analyze(a.base_dir);
    a.update_info = Analyze(a.update_dir);
    a.patches = LoadPatches();
    ReadPatchConfig(a.patch_enabled, a.patch_disabled);
    StartCheck();
    StartProbe();
}

// ---------------------------------------------------------------------------------------------
// Win32 + Direct3D 11 (Dear ImGui's example_win32_directx11)


void SetupStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.98f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.65f, 0.83f, 0.94f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.10f, 0.24f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.02f, 0.18f, 0.36f, 0.78f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.02f, 0.13f, 0.30f, 0.98f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.48f, 0.84f, 0.97f, 0.35f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.04f, 0.24f, 0.43f, 0.90f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.07f, 0.38f, 0.60f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.10f, 0.48f, 0.68f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.04f, 0.27f, 0.49f, 0.95f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.10f, 0.50f, 0.70f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.17f, 0.71f, 0.87f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.38f, 0.91f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.05f, 0.35f, 0.60f, 0.9f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.04f, 0.24f, 0.43f, 0.90f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.11f, 0.55f, 0.76f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.12f, 0.58f, 0.80f, 1.0f);
    style.Colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.38f, 0.91f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TabDimmed] = ImVec4(0.04f, 0.24f, 0.43f, 0.90f);
    style.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.12f, 0.58f, 0.80f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.11f, 0.55f, 0.76f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.68f, 0.84f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.12f, 0.26f, 0.70f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.12f, 0.53f, 0.73f, 0.85f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.24f, 0.76f, 0.92f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.90f, 1.0f, 1.0f);
    LauncherUI::Table::SetupStyle(style);
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabMinSize = 28.0f;
    style.FontSizeBase = 17.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowPadding = ImVec2(16 * scale, 14 * scale);
    style.FramePadding = ImVec2(10 * scale, 6 * scale);
    style.ItemSpacing = ImVec2(10 * scale, 8 * scale);
    style.FrameRounding = 4 * scale;
    style.TabRounding = 4 * scale;
    style.WindowRounding = 9 * scale;
}

std::vector<fs::path> PlatformFonts();
ImFont* AddFontFile(const fs::path& path, float size = 0, bool merge = false) {
    static std::map<fs::path, std::string> files;
    auto [it, inserted] = files.try_emplace(path);
    if (inserted) it->second = ReadText(path);
    auto& data = it->second;
    if (data.empty() || data.size() > INT_MAX) return nullptr;
    ImFontConfig config;
    config.MergeMode = merge;
    config.FontDataOwnedByAtlas = false; // The body and heading share one copy of each CJK font.
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data.data(), int(data.size()), size, &config);
}
void SetupFonts() {
    ImGuiIO& io = ImGui::GetIO();
#ifndef _WIN32
    io.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
#endif
    const auto files = PlatformFonts();
    const fs::path segoe = files.empty() ? fs::path{} : files.front();
    std::error_code ec;
    // Japanese and Chinese, which Segoe UI and the CTR font lack: Windows' own CJK fonts merged in
    // as fallbacks (ImGui 1.92 loads their glyphs only when a text uses them).
    const auto merge_fallbacks = [&](bool with_segoe) {
        for (size_t i = with_segoe ? 0 : 1; i < files.size(); ++i) {
            const fs::path& file = files[i];
            if (!fs::is_regular_file(file, ec)) continue;
            AddFontFile(file, 0, true);
        }
    };
    if (fs::is_regular_file(segoe, ec)) AddFontFile(segoe);
    else io.Fonts->AddFontDefault();
    merge_fallbacks(false);
    const fs::path ctr_font = g_app.root / L"art" / L"ctr-menu.ttf";
    if (fs::is_regular_file(ctr_font, ec)) {
        g_ctr_font = AddFontFile(ctr_font, ImGui::GetStyle().FontSizeBase);
        merge_fallbacks(true);  // Cyrillic and CJK labels in the CTR font's places
    }
}

#ifdef _WIN32
#include "windows/platform.inc"
#include "windows/main.inc"
#else
#include "linux/platform.inc"
#include "linux/main.inc"
#endif
