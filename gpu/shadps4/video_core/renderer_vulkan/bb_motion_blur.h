// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// SacredFire: no motion blur at all with the "Disable Motion Blur" patch (bbport.ini
// patch_no_motion_blur=1, run.py sets BB_NO_MOTION_BLUR=1). That eboot patch stops the camera's
// blur, but objects that move on their own (the crowd, plants in the wind) still write velocities
// and the game's MotionBlur pixel shaders smear them. Those shaders skip the blur where the largest
// velocity near the pixel is at most half a pixel (v_cmp_ge_f32 vcc, 0.5, vN) and then only apply
// the colour-grading LUT; here that test always passes, so the colours stay and the blur goes.
//
// The change is made to the SPIR-V a module is made from, never to the shader cache (as the HUD
// scale, bb_hud_scale.h): the one comparison "0.5 >= x" becomes "0.5 >= 0.5". A shader of another
// shape is left as it is.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace BbBlur {

// MotionBlur_21a0fd55_ps and MotionBlur_ea382dda_ps (the guest programs' hashes, OrbShdr)
inline constexpr std::uint64_t Shaders[] = {0xd027e2ebull, 0xc3af77aeull};

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("BB_NO_MOTION_BLUR");
        return v && v[0] == '1';
    }();
    return on;
}

inline std::optional<std::vector<std::uint32_t>> Unblur(std::span<const std::uint32_t> words) {
    if (words.size() < 5 || words[0] != 0x07230203u) return std::nullopt;
    std::uint32_t float_type = 0;
    std::unordered_set<std::uint32_t> half;  // OpConstant %float 0.5
    for (std::size_t at = 5; at < words.size();) {
        const std::size_t length = words[at] >> 16;
        if (!length || at + length > words.size()) return std::nullopt;
        const std::uint32_t op = words[at] & 0xffffu;
        if (op == 22 && length == 3 && words[at + 2] == 32 && !float_type) float_type = words[at + 1];
        if (op == 43 && length == 4 && words[at + 1] == float_type && words[at + 3] == 0x3f000000u) {
            half.insert(words[at + 2]);
        }
        at += length;
    }
    if (half.empty()) return std::nullopt;
    std::vector<std::uint32_t> out(words.begin(), words.end());
    int found = 0;
    for (std::size_t at = 5; at < out.size();) {
        const std::size_t length = out[at] >> 16;
        const std::uint32_t op = out[at] & 0xffffu;
        if (length == 5) {
            const std::uint32_t a = out[at + 3], b = out[at + 4];
            if ((op == 190 || op == 191) && half.count(a)) {  // OpF*GreaterThanEqual %bool 0.5 x
                out[at + 4] = a;
                ++found;
            } else if ((op == 188 || op == 189) && half.count(b)) {  // OpF*LessThanEqual %bool x 0.5
                out[at + 3] = b;
                ++found;
            }
        }
        at += length;
    }
    if (found != 1) return std::nullopt;
    return out;
}

// The SPIR-V to make the module of this shader from: the blur's test made to always pass.
inline std::optional<std::vector<std::uint32_t>> ForModule(std::uint64_t pgm_hash,
                                                           std::span<const std::uint32_t> spv) {
    if (!Enabled()) return std::nullopt;
    for (const std::uint64_t hash : Shaders) {
        if (hash != pgm_hash) continue;
        auto out = Unblur(spv);
        std::printf(out ? "Motion blur: shader %#llx without the blur\n"
                        : "Motion blur: shader %#llx has an unexpected shape; left as it is\n",
                    static_cast<unsigned long long>(pgm_hash));
        return out;
    }
    return std::nullopt;
}

} // namespace BbBlur
