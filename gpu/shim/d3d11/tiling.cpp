// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: see tiling.h. MacroTiling and the micro tile copies are bbport_native_vk_texture.cpp's.
#include "tiling.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "video_core/amdgpu/tiling.h"
#include "video_core/texture_cache/tile.h"

namespace D3dTiling {
namespace {

struct MacroTiling {
    u32 bpp, micro_mode, pipe_config, num_pipes, pipe_bits, bank_w, bank_h, banks, bank_bits,
        tile_split, aspect;

    MacroTiling(u32 tiling, u32 element_bytes) {
        const auto tile_mode = AmdGpu::TileMode(tiling);
        const auto array_mode = AmdGpu::GetArrayMode(tile_mode);
        const auto micro = AmdGpu::GetMicroTileMode(tile_mode);
        bpp = element_bytes * 8;
        micro_mode = u32(micro);
        pipe_config = u32(AmdGpu::GetPipeConfig(tile_mode));
        num_pipes = pipe_config == 0 ? 2 : 8;
        pipe_bits = pipe_config == 0 ? 1 : 3;
        const auto macro_mode = AmdGpu::CalculateMacrotileMode(tile_mode, bpp, 1);
        bank_w = AmdGpu::GetBankWidth(macro_mode);
        bank_h = AmdGpu::GetBankHeight(macro_mode);
        banks = AmdGpu::GetNumBanks(macro_mode);
        bank_bits = std::bit_width(banks) - 1;
        tile_split = AmdGpu::CalculateTileSplit(tile_mode, array_mode, micro, bpp);
        aspect = AmdGpu::GetMacrotileAspect(macro_mode);
    }

    static u32 Bit(u32 v, u32 b) {
        return (v >> b) & 1;
    }

    u32 PixelIndex(u32 x, u32 y) const {
        u32 p[6];
        const u32 x0 = Bit(x, 0), x1 = Bit(x, 1), x2 = Bit(x, 2);
        const u32 y0 = Bit(y, 0), y1 = Bit(y, 1), y2 = Bit(y, 2);
        if (micro_mode == 0) { // display micro tiling: by element size
            switch (bpp) {
            case 8: p[0] = x0; p[1] = x1; p[2] = x2; p[3] = y1; p[4] = y0; p[5] = y2; break;
            case 16: p[0] = x0; p[1] = x1; p[2] = x2; p[3] = y0; p[4] = y1; p[5] = y2; break;
            case 32: p[0] = x0; p[1] = x1; p[2] = y0; p[3] = x2; p[4] = y1; p[5] = y2; break;
            case 64: p[0] = x0; p[1] = y0; p[2] = x1; p[3] = x2; p[4] = y1; p[5] = y2; break;
            default: p[0] = x0; p[1] = y0; p[2] = x1; p[3] = y1; p[4] = x2; p[5] = y2; break;
            }
        } else { // thin and depth micro tiling (rotated is not used by these textures)
            p[0] = x0; p[1] = y0; p[2] = x1; p[3] = y1; p[4] = x2; p[5] = y2;
        }
        return p[0] | (p[1] << 1) | (p[2] << 2) | (p[3] << 3) | (p[4] << 4) | (p[5] << 5);
    }

    u32 Pipe(u32 x, u32 y) const {
        const u32 tx = x / 8, ty = y / 8;
        const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2);
        const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2);
        u32 p0 = 0, p1 = 0, p2 = 0;
        if (pipe_config == 0) { // P2
            p0 = x3 ^ y3;
        } else if (pipe_config == 10) { // P8_32x32_8x16
            p0 = x4 ^ y3 ^ x5;
            p1 = x3 ^ y4;
            p2 = x5 ^ y5;
        } else if (pipe_config == 12) { // P8_32x32_16x16
            p0 = x3 ^ y3 ^ x4;
            p1 = x4 ^ y4;
            p2 = x5 ^ y5;
        }
        return (p0 | (p1 << 1) | (p2 << 2)) & (num_pipes - 1);
    }

    u32 Bank(u32 x, u32 y, u32 tile_split_slice) const {
        const u32 tx = x / 8 / (bank_w * num_pipes), ty = y / 8 / bank_h;
        const u32 x3 = Bit(tx, 0), x4 = Bit(tx, 1), x5 = Bit(tx, 2), x6 = Bit(tx, 3);
        const u32 y3 = Bit(ty, 0), y4 = Bit(ty, 1), y5 = Bit(ty, 2), y6 = Bit(ty, 3);
        u32 b0 = 0, b1 = 0, b2 = 0, b3 = 0;
        if (banks == 16) {
            b0 = x3 ^ y6;
            b1 = x4 ^ y5 ^ y6;
            b2 = x5 ^ y4;
            b3 = x6 ^ y3;
        } else if (banks == 8) {
            b0 = x3 ^ y5;
            b1 = x4 ^ y4 ^ y5;
            b2 = x5 ^ y3;
        } else if (banks == 4) {
            b0 = x3 ^ y4;
            b1 = x4 ^ y3;
        } else if (banks == 2) {
            b0 = x3 ^ y3;
        }
        u32 bank = b0 | (b1 << 1) | (b2 << 2) | (b3 << 3);
        bank ^= ((banks / 2) + 1) * tile_split_slice; // 2D thin: tile split rotation
        return bank & (banks - 1);
    }

    u64 Address(u32 x, u32 y, u32 pitch, u32 height) const {
        u32 element_offset = PixelIndex(x, y) * bpp / 8;
        const u32 micro_tile_bytes_full = 64 * bpp / 8;
        u32 micro_tile_bytes = micro_tile_bytes_full;
        u32 slices_per_tile = 1, tile_split_slice = 0;
        if (micro_tile_bytes_full > tile_split) {
            slices_per_tile = micro_tile_bytes_full / tile_split;
            tile_split_slice = element_offset / tile_split;
            element_offset %= tile_split;
            micro_tile_bytes = tile_split;
        }
        const u32 macro_pitch = 8 * bank_w * num_pipes * aspect;
        const u32 macro_height = 8 * bank_h * banks / aspect;
        const u64 macro_bytes =
            u64(micro_tile_bytes) * (macro_pitch / 8) * (macro_height / 8) / (num_pipes * banks);
        const u32 macro_per_row = pitch / macro_pitch;
        const u64 macro_offset = (u64(y / macro_height) * macro_per_row + x / macro_pitch) * macro_bytes;
        const u64 macro_per_slice = u64(macro_per_row) * (height / macro_height);
        const u64 slice_offset = macro_per_slice * macro_bytes * tile_split_slice;
        const u32 tile_row = (y / 8) % bank_h;
        const u32 tile_column = ((x / 8) / num_pipes) % bank_w;
        const u64 tile_offset = u64(tile_row * bank_w + tile_column) * micro_tile_bytes;
        const u64 total = slice_offset + macro_offset + element_offset + tile_offset;
        (void)slices_per_tile;
        const u64 interleave = total & 0xff; // 256-byte pipe interleave
        const u64 rest = total >> 8;
        return interleave | (u64(Pipe(x, y)) << 8) | (u64(Bank(x, y, tile_split_slice)) << (8 + pipe_bits)) |
               (rest << (8 + pipe_bits + bank_bits));
    }
};

/// The 64 elements of one 8x8 micro tile (contiguous at `tile`, element i of the tile at
/// index[y * 8 + x]) into rows of `dst` (row_bytes apart), clipped to w x h.
template <u32 EB>
void CopyMicroTile(const u8* tile, const u8* index, u8* dst, std::size_t row_bytes, u32 w, u32 h) {
    for (u32 y = 0; y < h; ++y) {
        u8* d = dst + y * row_bytes;
        const u8* ix = index + y * 8;
        for (u32 x = 0; x < w; ++x) {
            std::memcpy(d + x * EB, tile + u32(ix[x]) * EB, EB);
        }
    }
}

void CopyMicroTileAny(u32 eb, const u8* tile, const u8* index, u8* dst, std::size_t row_bytes, u32 w,
                      u32 h) {
    switch (eb) {
    case 1: return CopyMicroTile<1>(tile, index, dst, row_bytes, w, h);
    case 2: return CopyMicroTile<2>(tile, index, dst, row_bytes, w, h);
    case 4: return CopyMicroTile<4>(tile, index, dst, row_bytes, w, h);
    case 8: return CopyMicroTile<8>(tile, index, dst, row_bytes, w, h);
    case 16: return CopyMicroTile<16>(tile, index, dst, row_bytes, w, h);
    default:
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                std::memcpy(dst + y * row_bytes + x * eb, tile + u32(index[y * 8 + x]) * eb, eb);
            }
        }
    }
}

u32 MortonInTile(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

} // namespace

bool MacroSupported(std::uint32_t tiling) {
    return tiling <= 26 && AmdGpu::GetArrayMode(AmdGpu::TileMode(tiling)) == AmdGpu::ArrayMode::Array2DTiledThin1;
}

void MacroSize(std::uint32_t tiling, std::uint32_t pitch, std::uint32_t height, std::uint32_t element_bytes,
               std::uint32_t* row, std::uint32_t* rows, std::uint64_t* bytes) {
    const auto [pa, ha, size] =
        VideoCore::ImageSizeMacroTiled(pitch, height, element_bytes * 8, 1, AmdGpu::TileMode(tiling), false);
    *row = pa;
    *rows = ha;
    *bytes = size;
}

void DetileMacro(std::uint32_t tiling, std::uint32_t eb, const std::uint8_t* src, std::uint64_t slice_bytes,
                 std::uint32_t row, std::uint32_t rows, std::uint32_t width, std::uint32_t height, std::uint8_t* dst) {
    const MacroTiling mt(tiling, eb);
    const std::size_t row_bytes = std::size_t(width) * eb;
    // A micro tile of at most 256 bytes and no tile split lies whole inside one 256-byte pipe
    // interleave: its address is worked out once per tile instead of per element.
    const u32 micro_bytes = 64 * eb;
    if (micro_bytes <= 256 && micro_bytes <= mt.tile_split) {
        u8 index[64];
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                index[y * 8 + x] = u8(mt.PixelIndex(x, y));
            }
        }
        for (u32 ty = 0; ty * 8 < height; ++ty) {
            for (u32 tx = 0; tx * 8 < width; ++tx) {
                const u64 base = mt.Address(tx * 8, ty * 8, row, rows);
                if (base + micro_bytes <= slice_bytes) {
                    CopyMicroTileAny(eb, src + base, index,
                                     dst + std::size_t(ty) * 8 * row_bytes + std::size_t(tx) * 8 * eb, row_bytes,
                                     std::min(8u, width - tx * 8), std::min(8u, height - ty * 8));
                }
            }
        }
        return;
    }
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u64 off = mt.Address(x, y, row, rows);
            if (off + eb <= slice_bytes) {
                std::memcpy(dst + y * row_bytes + std::size_t(x) * eb, src + off, eb);
            }
        }
    }
}

void DetileMicro(std::uint32_t eb, const std::uint8_t* src, std::uint32_t row, std::uint32_t width,
                 std::uint32_t height, std::uint8_t* dst) {
    static const std::array<u8, 64> morton = [] {
        std::array<u8, 64> m{};
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                m[y * 8 + x] = u8(MortonInTile(x, y));
            }
        }
        return m;
    }();
    const std::size_t row_bytes = std::size_t(width) * eb;
    for (u32 ty = 0; ty * 8 < height; ++ty) {
        const u64 tile_row = u64(ty) * (row >> 3);
        for (u32 tx = 0; tx * 8 < width; ++tx) {
            CopyMicroTileAny(eb, src + (tile_row + tx) * 64 * eb, morton.data(),
                             dst + std::size_t(ty) * 8 * row_bytes + std::size_t(tx) * 8 * eb, row_bytes,
                             std::min(8u, width - tx * 8), std::min(8u, height - ty * 8));
        }
    }
}

} // namespace D3dTiling
