// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace BbLiveGraphics {
inline unsigned FrameLimit(unsigned requested, bool unlocked) {
    return unlocked ? requested : requested ? std::min(requested, 30u) : 30u;
}

inline float SceneScale(unsigned w, unsigned h, unsigned base_height, unsigned wanted_height,
                        bool display, bool shadow, unsigned base_width = 0) {
    if (!base_width) base_width = unsigned(std::lround(base_height * 16.0 / 9.0));
    if (display || shadow || !base_height || !wanted_height || h < 16 || h > base_height ||
        w > base_width || std::abs(double(w) * base_height - double(h) * base_width) >
                         std::max(base_width, base_height)) return 1.0f;
    return float(wanted_height) / float(base_height);
}

inline unsigned Extent(unsigned size, float scale) {
    return std::max(1u, unsigned(std::lround(size * scale)));
}

inline bool CameraBlurAvailable(std::span<const unsigned char> image) {
    constexpr unsigned char prefix[] = {0x41,0x80,0xbc,0x24,0x50,0x01,0,0,0,0x75,0x10,
        0x49,0x8b,0xb4,0x24,0x68,0x01,0,0,0x4c,0x89,0xe7};
    unsigned matches = 0;
    for (std::size_t i = 0; i + sizeof(prefix) < image.size(); ++i)
        if (!std::memcmp(image.data()+i, prefix, sizeof(prefix)) && image[i+sizeof(prefix)] == 0xe8)
            ++matches;
    return matches == 1;
}

inline bool UnlockPatch(std::span<unsigned char> image) {
    constexpr unsigned char prefix[] = {0x41,0x8b,0x44,0x24,0x18};
    constexpr unsigned char stock[] = {0x41,0x89,0x86,0x88,0x04,0,0};
    constexpr unsigned char unlocked[] = {0x41,0x89,0x96,0x88,0x04,0,0};
    std::size_t match = image.size();
    for (std::size_t i = 0; i + sizeof(prefix) + sizeof(stock) <= image.size(); ++i) {
        if (std::memcmp(image.data()+i, prefix, sizeof(prefix)) ||
            (std::memcmp(image.data()+i+sizeof(prefix), stock, sizeof(stock)) &&
             std::memcmp(image.data()+i+sizeof(prefix), unlocked, sizeof(unlocked)))) continue;
        if (match != image.size()) return false;
        match = i + sizeof(prefix);
    }
    if (match == image.size()) return false;
    std::memcpy(image.data()+match, unlocked, sizeof(unlocked));
    return true;
}
} // namespace BbLiveGraphics
