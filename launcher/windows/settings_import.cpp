// SPDX-License-Identifier: GPL-2.0-or-later
// Bringing the player's settings from another installation (settings_import.h).
#include "settings_import.h"

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace fs = std::filesystem;

namespace SettingsImport {

namespace {

const wchar_t* const kFiles[] = {L"bbport.ini", L"launcher.ini", L"ryunet_online.key", L"patches.json"};
const wchar_t* const kFolders[] = {L"user", L"dlc", L"backups"};

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

/// The same folder, whatever the spelling.
std::string Key(const fs::path& dir) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(dir, ec);
    fs::path key = (ec ? dir : canonical).lexically_normal();
#ifdef _WIN32
    key = Lower(key.wstring());
#endif
    const auto text = key.generic_u8string();
    return std::string(text.begin(), text.end());
}

/// installs.txt: one folder per line, in UTF-8.
std::vector<fs::path> ReadList(const fs::path& file) {
    std::vector<fs::path> dirs;
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) dirs.emplace_back(std::u8string(line.begin(), line.end()));
    }
    return dirs;
}

fs::file_time_type LastUse(const fs::path& dir) {
    auto newest = fs::file_time_type::min();
    for (const wchar_t* name : {L"bbport.ini", L"launcher.ini"}) {
        std::error_code ec;
        const auto t = fs::last_write_time(dir / name, ec);
        if (!ec && t > newest) newest = t;
    }
    return newest;
}

}  // namespace

bool IsUsedInstall(const fs::path& dir) {
    std::error_code ec;
#ifdef _WIN32
    if (!fs::is_regular_file(dir / L"run.py", ec)) return false;
#endif
    return
           (fs::is_regular_file(dir / L"bbport.ini", ec) || fs::is_regular_file(dir / L"launcher.ini", ec));
}

void Remember(const fs::path& installs_file, const fs::path& root) {
    const std::string key = Key(root);
    for (const fs::path& dir : ReadList(installs_file))
        if (Key(dir) == key) return;
    std::error_code ec;
    fs::create_directories(installs_file.parent_path(), ec);
    const std::u8string text = root.lexically_normal().u8string();
    std::ofstream(installs_file, std::ios::binary | std::ios::app)
        << std::string(text.begin(), text.end()) << "\n";
}

std::vector<fs::path> Previous(const fs::path& installs_file, const fs::path& root) {
    std::vector<fs::path> found;
    std::vector<std::string> keys{Key(root)};
    auto consider = [&](const fs::path& dir) {
        const std::string key = Key(dir);
        if (std::find(keys.begin(), keys.end(), key) != keys.end() || !IsUsedInstall(dir)) return;
        keys.push_back(key);
        found.push_back(dir);
    };
    for (const fs::path& dir : ReadList(installs_file)) consider(dir);
    // Releases before the list: unpacked next to this one ("...-v0.5.2-windows\CTR-NitroFueled-PC-portable").
    for (const fs::path& parent : {root.parent_path(), root.parent_path().parent_path()}) {
        std::error_code ec;
        if (parent.empty() || parent == root.root_path()) continue;
        for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            auto name = it->path().filename().generic_u8string();
            for (auto& c : name) if (c >= u8'A' && c <= u8'Z') c += u8'a' - u8'A';
            if (name.find(u8"ctr") == std::u8string::npos) continue;
            consider(it->path());
            consider(it->path() / L"CTR-NitroFueled-PC-portable");
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const fs::path& a, const fs::path& b) { return LastUse(a) > LastUse(b); });
    return found;
}

std::vector<fs::path> Items(const fs::path& from) {
    std::vector<fs::path> items;
    std::error_code ec;
    for (const wchar_t* name : kFiles)
        if (fs::is_regular_file(from / name, ec)) items.emplace_back(name);
    for (const wchar_t* folder : kFolders) {
        for (fs::recursive_directory_iterator it(from / folder, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path relative = it->path().lexically_relative(from);
            std::error_code e2;
            // The shader cache is rebuilt for each release.
#ifdef _WIN32
            const bool cache = Lower(relative.lexically_normal().generic_wstring()) == L"user/cache";
#else
            const bool cache = relative.lexically_normal() == fs::path("user/cache");
#endif
            if (it->is_directory(e2) && cache) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(e2)) items.push_back(relative);
        }
        ec.clear();
    }
    return items;
}

std::string Copy(const fs::path& from, const fs::path& to) {
    for (const fs::path& relative : Items(from)) {
        std::error_code ec;
        fs::create_directories((to / relative).parent_path(), ec);
        fs::copy_file(from / relative, to / relative, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            const std::u8string name = relative.u8string();
            return std::string(name.begin(), name.end()) + ": " + ec.message();
        }
    }
    return {};
}

}  // namespace SettingsImport
