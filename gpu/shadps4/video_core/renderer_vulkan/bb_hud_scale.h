// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// SacredFire: the centered HUD of the corrected 21:9 and 32:9 layouts (scripts/widescreen.py, by
// tcosi). The HUD's vertex shaders draw for 16:9; scaling their output x by 16:9 over the screen's
// aspect keeps the HUD in the middle. BB_HUD_SCALE lists the shaders, "hash:permutation:factor"
// separated by commas (hash in hex, the guest program's). The scale is applied to the SPIR-V a
// module is made from, never to the shader cache, so the same cache serves 16:9 and every layout.
//
// The transform is nitro_components.position_scale, word for word: a float constant `factor`
// before the first function, and before the shader's single OpReturn
//   v = load Position; x = v.x * factor; store Position, v with x replaced.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/bb_motion_blur.h"

namespace BbHud {

struct Entry {
    std::uint64_t hash;
    std::size_t perm;
    float factor;
};

inline const std::vector<Entry>& Entries() {
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> out;
        const char* list = std::getenv("BB_HUD_SCALE");
        for (const char* p = list; p && *p;) {
            char* end{};
            const std::uint64_t hash = std::strtoull(p, &end, 16);
            if (*end != ':') break;
            const std::size_t perm = std::strtoull(end + 1, &end, 10);
            if (*end != ':') break;
            const double factor = std::strtod(end + 1, &end);
            if (factor > 0.0 && factor <= 2.0) out.push_back({hash, perm, float(factor)});
            p = *end == ',' ? end + 1 : end + std::strlen(end);
        }
        if (!out.empty()) std::printf("HUD: %zu shader(s) scaled for the widescreen layout\n", out.size());
        return out;
    }();
    return entries;
}

// The factor's float bits are those of struct.pack('<f', factor) (round to nearest).
inline std::optional<std::vector<std::uint32_t>> Scale(std::span<const std::uint32_t> words, float factor) {
    if (words.size() < 5 || words[0] != 0x07230203u) return std::nullopt;
    struct Ins {
        std::uint32_t op;
        std::size_t at, length;
    };
    std::vector<Ins> ins;
    for (std::size_t at = 5; at < words.size();) {
        const std::size_t length = words[at] >> 16;
        if (!length || at + length > words.size()) return std::nullopt;
        ins.push_back({words[at] & 0xffffu, at, length});
        at += length;
    }
    const auto arg = [&](const Ins& i, std::size_t n) { return words[i.at + 1 + n]; };
    std::optional<std::uint32_t> position, ft, vt;
    for (const auto& i : ins) {  // OpDecorate %v BuiltIn Position
        if (i.op == 71 && i.length == 4 && arg(i, 1) == 11 && arg(i, 2) == 0) { position = arg(i, 0); break; }
    }
    for (const auto& i : ins) {  // OpTypeFloat %f 32
        if (i.op == 22 && i.length == 3 && arg(i, 1) == 32) { ft = arg(i, 0); break; }
    }
    for (const auto& i : ins) {  // OpTypeVector %v4 %f 4
        if (ft && i.op == 23 && i.length == 4 && arg(i, 1) == *ft && arg(i, 2) == 4) { vt = arg(i, 0); break; }
    }
    if (!position || !ft || !vt) return std::nullopt;
    const std::uint32_t half = words[3], vector = half + 1, x = half + 2, scaled = half + 3, result = half + 4;
    std::uint32_t bits;
    std::memcpy(&bits, &factor, sizeof(bits));
    std::vector<std::uint32_t> out(words.begin(), words.begin() + 5);
    out[3] += 5;
    const auto emit = [&](std::uint32_t op, std::initializer_list<std::uint32_t> args) {
        out.push_back(std::uint32_t(args.size() + 1) << 16 | op);
        out.insert(out.end(), args);
    };
    bool declared = false;
    int returns = 0;
    for (const auto& i : ins) {
        if (i.op == 54 && !declared) {  // OpFunction: the constant goes before it
            emit(43, {*ft, half, bits});
            declared = true;
        }
        if (i.op == 253) {  // OpReturn
            emit(61, {*vt, vector, *position});
            emit(81, {*ft, x, vector, 0});
            emit(133, {*ft, scaled, x, half});
            emit(82, {*vt, result, scaled, vector, 0});
            emit(62, {*position, result});
            ++returns;
        }
        out.insert(out.end(), words.begin() + i.at, words.begin() + i.at + i.length);
    }
    if (returns != 1) return std::nullopt;
    return out;
}

// The SPIR-V to make the module of this shader from: scaled when BB_HUD_SCALE names it (and the
// motion blur shaders without their blur with BB_NO_MOTION_BLUR, bb_motion_blur.h).
inline std::optional<std::vector<std::uint32_t>> ForModule(std::uint64_t pgm_hash, std::size_t perm,
                                                           std::span<const std::uint32_t> spv) {
    for (const auto& e : Entries()) {
        if (e.hash != pgm_hash || e.perm != perm) continue;
        auto scaled = Scale(spv, e.factor);
        if (!scaled) {
            std::printf("HUD: shader %#llx_%zu has an unexpected shape; left as it is\n",
                        static_cast<unsigned long long>(pgm_hash), perm);
        }
        return scaled;
    }
    return BbBlur::ForModule(pgm_hash, spv); // with the motion blur patch: no blur shader either
}

} // namespace BbHud
