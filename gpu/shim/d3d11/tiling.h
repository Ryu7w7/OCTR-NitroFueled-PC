// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: detiling as the native Vulkan renderer does it, for the Direct3D 11 one: no Direct3D or
// shadPS4 header on either side of this one.
#pragma once

#include <cstddef>
#include <cstdint>

namespace D3dTiling {

/// 2D thin macro tiling (render target tiling: tiling 14 and the other 2D thin modes).
bool MacroSupported(std::uint32_t tiling);
/// Pitch and height aligned to whole macro tiles, and the bytes of a slice.
void MacroSize(std::uint32_t tiling, std::uint32_t pitch, std::uint32_t height, std::uint32_t element_bytes,
               std::uint32_t* row, std::uint32_t* rows, std::uint64_t* bytes);
/// One level of a macro tiled surface into tightly packed rows.
void DetileMacro(std::uint32_t tiling, std::uint32_t element_bytes, const std::uint8_t* src, std::uint64_t slice_bytes,
                 std::uint32_t row, std::uint32_t rows, std::uint32_t width, std::uint32_t height, std::uint8_t* dst);
/// One level of a 1D thin surface: 8x8 micro tiles one after another, Morton order inside.
void DetileMicro(std::uint32_t element_bytes, const std::uint8_t* src, std::uint32_t row, std::uint32_t width,
                 std::uint32_t height, std::uint8_t* dst);

} // namespace D3dTiling
