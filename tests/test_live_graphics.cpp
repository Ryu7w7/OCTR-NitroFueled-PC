// SPDX-License-Identifier: GPL-2.0-or-later
// c++ -std=c++20 -I. tests/test_live_graphics.cpp -o /tmp/live-graphics && /tmp/live-graphics
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>
#include "gpu/shim/bbport_live_graphics.h"
#include "gpu/shadps4/video_core/renderer_vulkan/bb_motion_blur.h"

int main(int argc, char** argv) {
    using namespace BbLiveGraphics;
    for (unsigned requested : {0u, 15u, 30u, 60u, 144u}) {
        assert(FrameLimit(requested, true) == requested);
        assert(FrameLimit(requested, false) == (requested && requested < 30 ? requested : 30));
    }
    for (unsigned height : {480u, 720u, 1080u, 1440u, 1620u, 2160u}) {
        const float scale = SceneScale(1920, 1080, 1080, height, false, false);
        assert(Extent(1080, scale) == height);
        assert(Extent(540, scale) == height / 2);
        assert(SceneScale(1920, 1080, 1080, height, true, false) == 1);
        assert(SceneScale(1920, 1080, 1080, height, false, true) == 1);
        assert(SceneScale(2048, 8192, 1080, height, false, false) == 1);
        assert(SceneScale(1024, 1024, 1080, height, false, false) == 1);
    }
    assert(Extent(1440, SceneScale(2560, 1440, 1440, 720, false, false)) == 720);
    for (auto [w, h] : {std::pair{1920u, 1200u}, std::pair{3440u, 1440u}, std::pair{5120u, 1440u}}) {
        const float scale = SceneScale(w, h, h, 720, false, false, w);
        assert(Extent(h, scale) == 720);
        assert(SceneScale(1024, 1024, h, 720, false, true, w) == 1);
        assert(SceneScale(w, h, h, 720, true, false, w) == 1);
    }
    std::vector<unsigned char> original{0x41,0x8b,0x44,0x24,0x18,0x41,0x89,0x86,0x88,0x04,0,0};
    auto patch = original;
    assert(UnlockPatch(patch) && patch[7] == 0x96);
    assert(UnlockPatch(patch));
    original.insert(original.end(), patch.begin(), patch.end());
    const auto duplicate = original;
    assert(!UnlockPatch(original) && original == duplicate);
    std::array<unsigned char, 3> truncated{};
    assert(!UnlockPatch(truncated));
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        assert(input);
        std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
        std::uint32_t magic = 0;
        assert(bytes.size() >= 4);
        std::memcpy(&magic, bytes.data(), 4);
        if (magic == 0x07230203) {
            assert(bytes.size() % 4 == 0);
            std::vector<std::uint32_t> words(bytes.size() / 4);
            std::memcpy(words.data(), bytes.data(), bytes.size());
            auto disabled = BbBlur::Unblur(words);
            assert(disabled && *disabled != words);
            assert(BbBlur::Unblur(*disabled) == disabled);
        } else {
            assert(CameraBlurAvailable(bytes));
            assert(UnlockPatch(bytes));
        }
    }
}
