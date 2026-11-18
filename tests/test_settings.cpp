// SPDX-License-Identifier: GPL-2.0-or-later
// bbport_settings.cpp: the menu saves its keys into bbport.ini and keeps the launcher's (controls).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include "gpu/shim/bbport_settings.h"

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    unsetenv("BB_LAUNCHER_CONFIG");
    char path[] = "/tmp/bbport-settings-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    const char ini[] = "# launcher\ntemporal=fsr3\nkey.cross=X, Space\npad.circle=a\nshow_fps=0\n"
                       "fullscreen_hint=1\n";
    assert(write(fd, ini, sizeof(ini) - 1) == ssize_t(sizeof(ini) - 1));
    close(fd);
    setenv("BB_CONFIG", path, 1);

    BbSettings::Load();
    auto& s = BbSettings::Get();
    assert(s.temporal == BbSettings::TemporalFsr3 && !s.show_fps && s.menu_x < 0.0f);
    s.show_fps = true;
    s.temporal = BbSettings::TemporalOff;
    s.menu_x = 0.625f;
    s.menu_y = 0.125f;
    BbSettings::Save();

    const std::string saved = Read(path);
    // The launcher's controls, its other keys and comments stay; the menu's keys are replaced
    // in place, new ones appended.
    assert(saved.find("# launcher\ntemporal=off\nkey.cross=X, Space\npad.circle=a\nshow_fps=1\n"
                      "fullscreen_hint=1\n") == 0);
    assert(saved.find("menu_pos=0.6250,0.1250\n") != std::string::npos);
    assert(saved.find("temporal=fsr3") == std::string::npos);

    s.menu_x = -1.0f;
    s.menu_y = -1.0f;
    BbSettings::Load();
    assert(s.menu_x == 0.625f && s.menu_y == 0.125f && s.temporal == BbSettings::TemporalOff);

    // External changes appear without reloading startup state; local edits survive the merge.
    const int startup_temporal = s.startup_temporal;
    s.native_live_graphics = true;
    s.volume = 71;
    std::ofstream(path) << "# launcher\ntemporal=dlss\nvolume=23\nmenu_language=es\n"
                          "key.cross=Space\nunknown=future\nrender_res=1440\nupscaler=fsr3\n";
    assert(BbSettings::Refresh(true));
    assert(s.temporal == BbSettings::TemporalDlss && s.volume == 71);
    assert(s.startup_temporal == startup_temporal && s.upscaler == BbSettings::UpscalerOff);
    assert(s.menu_language == BbSettings::MenuLanguage::Spanish);
    assert(!s.show_fps && s.menu_x < 0.0f);
    assert(s.render_res >= 0 && BbSettings::RenderResHeights[s.render_res] == 1440);
    assert(s.native_resolution_requested);
    BbSettings::Save();
    assert(Read(path).find("volume=71\n") != std::string::npos);
    assert(Read(path).find("key.cross=Space\nunknown=future\n") != std::string::npos);
    assert(!BbSettings::Refresh(true));
    std::ofstream(path) << "volume=39\nmenu_language=es\n";
    assert(BbSettings::Refresh(true));
    assert(s.volume == 39 && s.render_res == -1);
    setenv("BB_MENU_LANGUAGE", "en", 1);
    BbSettings::Load();
    assert(s.menu_language == BbSettings::MenuLanguage::Spanish);

    // Two processes saving different fields must preserve both edits under the sidecar lock.
    BbSettings::Save();
    int gate[2];
    assert(pipe(gate) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(gate[1]);
        char start;
        assert(read(gate[0], &start, 1) == 1);
        s.volume = 83;
        BbSettings::Save();
        _exit(0);
    }
    close(gate[0]);
    s.show_fps = true;
    assert(write(gate[1], "x", 1) == 1);
    close(gate[1]);
    BbSettings::Save();
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    BbSettings::Refresh(true);
    assert(s.volume == 83 && s.show_fps);

    // Mod snapshots keep their overrides, but user edits must reach the launcher's preferences.
    const std::string session = std::string(path) + ".session";
    std::ofstream(path) << "# launcher\nvolume=60\nrender_res=720\ntemporal=off\nonline=1\nkey.cross=Q\n";
    std::ofstream(session) << "# mod session\nvolume=20\nrender_res=720\ntemporal=off\nonline=0\n";
    setenv("BB_CONFIG", session.c_str(), 1);
    setenv("BB_LAUNCHER_CONFIG", path, 1);
    BbSettings::Load();
    assert(s.volume == 20 && !BbSettings::Refresh(true));
    for (int i = 0; i < int(std::size(BbSettings::RenderResHeights)); ++i)
        if (BbSettings::RenderResHeights[i] == 480) s.render_res = i;
    s.temporal = BbSettings::TemporalDlss;
    BbSettings::Save();
    assert(Read(path).find("render_res=480\n") != std::string::npos);
    assert(Read(path).find("temporal=dlss\n") != std::string::npos);
    assert(Read(path).find("volume=60\n") != std::string::npos);
    assert(Read(path).find("online=1\nkey.cross=Q\n") != std::string::npos);
    assert(Read(session.c_str()).find("render_res=480\n") != std::string::npos);
    assert(Read(session.c_str()).find("volume=20\n") != std::string::npos);
    assert(Read(session.c_str()).find("online=0\n") != std::string::npos);
    std::ofstream(path) << "# launcher\nvolume=75\nrender_res=1080\ntemporal=fsr3\nonline=1\nkey.cross=W\n";
    s.show_fps = false;
    assert(BbSettings::Refresh(true));
    assert(s.volume == 75 && s.temporal == BbSettings::TemporalFsr3 && !s.show_fps);
    assert(Read(session.c_str()).find("volume=75\n") != std::string::npos);
    assert(BbSettings::RenderResHeights[s.render_res] == 1080);
    BbSettings::Save();
    BbSettings::Load();
    assert(s.volume == 75 && s.temporal == BbSettings::TemporalFsr3 && !s.show_fps);
    assert(BbSettings::RenderResHeights[s.render_res] == 1080);
    assert(Read(path).find("online=1\nkey.cross=W\n") != std::string::npos);
    assert(Read(session.c_str()).find("online=0\n") != std::string::npos);
    unsetenv("BB_LAUNCHER_CONFIG");
    unlink(session.c_str());
    unlink((session + ".lock").c_str());
    unlink(path);
    unlink((std::string(path) + ".lock").c_str());
    std::puts("PASS: live settings merge, deletion, startup state, concurrent writers, mod session preferences");
}
