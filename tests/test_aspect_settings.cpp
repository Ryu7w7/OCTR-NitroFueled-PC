// SPDX-License-Identifier: GPL-2.0-or-later
// Aspect ratios survive settings load/save; existing widescreen settings still load.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include "gpu/shim/bbport_settings.h"

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    const auto temporary = std::filesystem::temp_directory_path() /
        ("bbport-aspect-settings-" + std::to_string(
#ifdef _WIN32
            _getpid()
#else
            getpid()
#endif
        ));
    const std::string filename = temporary.string();
    const char* path = filename.c_str();
#ifdef _WIN32
    _putenv_s("BB_CONFIG", path);
#else
    setenv("BB_CONFIG", path, 1);
#endif
    auto& s = BbSettings::Get();
    // New keys win in either file order, including after saving and reloading.
    for (bool modern_first : {false, true}) {
        const std::string modern = "aspect_ratio=16x10\naspect_output=1280x800\n";
        const std::string legacy = "widescreen_layout=32x9\nwidescreen_output=5120x1440\n";
        std::ofstream(path) << (modern_first ? modern + legacy : legacy + modern);
        BbSettings::Load();
        assert(s.aspect_ratio == BbSettings::Aspect16x10 && s.aspect_w == 1280 && s.aspect_h == 800);
        assert(s.startup_aspect_ratio == BbSettings::Aspect16x10);
        BbSettings::Save();
        const std::string saved = Read(path);
        assert(saved.find("aspect_ratio=16x10\n") != std::string::npos);
        assert(saved.find("aspect_output=1280x800\n") != std::string::npos);
        assert(saved.find("widescreen_layout=off\n") != std::string::npos);
        BbSettings::Load();
        assert(s.aspect_ratio == BbSettings::Aspect16x10 && s.aspect_w == 1280 && s.aspect_h == 800);
        const std::string invalid = "aspect_ratio=16x10\naspect_output=invalid\n";
        std::ofstream(path) << (modern_first ? invalid + legacy : legacy + invalid);
        BbSettings::Load();
        assert(s.aspect_ratio == BbSettings::Aspect16x10 && s.aspect_w == 0 && s.aspect_h == 0);
    }
    for (const auto& mode : {std::pair{"21x9", BbSettings::Aspect21x9},
                             std::pair{"32x9", BbSettings::Aspect32x9}}) {
        const int width = mode.second == BbSettings::Aspect21x9 ? 2560 : 3840;
        std::ofstream(path) << "widescreen_layout=" << mode.first << "\nwidescreen_output=" << width << "x1080\n";
        BbSettings::Load();
        assert(s.aspect_ratio == mode.second && s.aspect_w == width && s.aspect_h == 1080);
        BbSettings::Save();
        const std::string saved = Read(path);
        assert(saved.find(std::string("aspect_ratio=") + mode.first + "\n") != std::string::npos);
        assert(saved.find(std::string("widescreen_layout=") + mode.first + "\n") != std::string::npos);
    }
    std::ofstream(path) << "aspect_ratio=16x9\nwidescreen_layout=21x9\n";
    BbSettings::Load();
    assert(s.aspect_ratio == BbSettings::Aspect16x9);
    std::filesystem::remove(temporary);
    std::puts("PASS: aspect ratios, key precedence, load/save and legacy widescreen compatibility");
}
