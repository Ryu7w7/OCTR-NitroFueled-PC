// The native Gameplay selector uses the existing patch key; both values must survive reload.
#include "gpu/shim/bbport_settings.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

int main() {
    char path[] = "/tmp/camera-settings-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    BbSettings::Load();
    auto& settings = BbSettings::Get();
    int camera = -1;
    for (int i = 0; i < BbSettings::GamePatchCount; ++i)
        if (std::string(BbSettings::GamePatches[i].key) == "default_camera_setting") camera = i;
    assert(camera >= 0);
    for (bool far : {true, false}) {
        settings.game_patches[camera] = far;
        BbSettings::Save();
        std::ifstream file(path);
        std::string text{std::istreambuf_iterator<char>(file), {}};
        assert(text.find(far ? "default_camera_setting=1\n" : "default_camera_setting=0\n") != std::string::npos);
        settings.game_patches[camera] = !far;
        BbSettings::Load();
        assert(settings.game_patches[camera] == far);
        assert(settings.startup_game_patches[camera] == far);
    }
    unlink(path);
}
