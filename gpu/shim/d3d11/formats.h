#pragma once
#include <dxgiformat.h>

#include "common.h"

// GCN data formats (T# DATA_FORMAT, Gnm SurfaceFormat, CB_COLOR_INFO FORMAT share numbering)
enum GcnFmt : u32 {
    FMT_INVALID = 0, FMT_8 = 1, FMT_16 = 2, FMT_8_8 = 3, FMT_32 = 4, FMT_16_16 = 5,
    FMT_10_11_11 = 6, FMT_11_11_10 = 7, FMT_10_10_10_2 = 8, FMT_2_10_10_10 = 9,
    FMT_8_8_8_8 = 10, FMT_32_32 = 11, FMT_16_16_16_16 = 12, FMT_32_32_32 = 13,
    FMT_32_32_32_32 = 14, FMT_5_6_5 = 16, FMT_1_5_5_5 = 17, FMT_5_5_5_1 = 18, FMT_4_4_4_4 = 19,
    FMT_8_24 = 20, FMT_24_8 = 21, FMT_X24_8_32 = 22, FMT_BC1 = 35, FMT_BC2 = 36, FMT_BC3 = 37,
    FMT_BC4 = 38, FMT_BC5 = 39, FMT_BC6 = 40, FMT_BC7 = 41,
};

// texture/buffer NUM_FORMAT
enum GcnNum : u32 {
    NUM_UNORM = 0, NUM_SNORM = 1, NUM_USCALED = 2, NUM_SSCALED = 3, NUM_UINT = 4, NUM_SINT = 5,
    NUM_SNORM_NZ = 6, NUM_FLOAT = 7, NUM_SRGB = 9,
};

// Decoded 8-dword image resource descriptor (T#).
struct TSharp {
    u64 base;
    u32 dfmt, nfmt;
    u32 width, height, depth, pitch;
    u32 base_level, last_level, base_array, last_array;
    u32 tiling_index, type;
    bool pow2pad;
    u32 dst_sel;  // 12 bits: x[2:0] y[5:3] z[8:6] w[11:9]
};
TSharp DecodeTSharp(const u32* t);

DXGI_FORMAT TextureFormat(u32 dfmt, u32 nfmt, u32 dst_sel);
DXGI_FORMAT TypelessOf(DXGI_FORMAT f);
// CB_COLOR_INFO -> DXGI
DXGI_FORMAT ColorTargetFormat(u32 cb_color_info);
// Gnm vertex DataFormat (u32) -> DXGI, 0 if unsupported
DXGI_FORMAT VertexFormat(u32 data_format);
// 0: none, 1: uscaled (raw uint -> float), 2: sscaled (raw sint -> float)
u32 VertexConversion(u32 df);
// bytes per element (or per 4x4 block for BC formats)
u32 FormatBytes(DXGI_FORMAT f, bool* block_compressed);
