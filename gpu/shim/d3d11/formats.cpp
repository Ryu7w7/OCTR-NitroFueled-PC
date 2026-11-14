#include "formats.h"

TSharp DecodeTSharp(const u32* t) {
    TSharp s{};
    s.base = ((u64)t[0] << 8) | ((u64)(t[1] & 0xFF) << 40);
    s.dfmt = (t[1] >> 20) & 0x3F;
    s.nfmt = (t[1] >> 26) & 0xF;
    s.width = (t[2] & 0x3FFF) + 1;
    s.height = ((t[2] >> 14) & 0x3FFF) + 1;
    s.dst_sel = t[3] & 0xFFF;
    s.base_level = (t[3] >> 12) & 0xF;
    s.last_level = (t[3] >> 16) & 0xF;
    s.tiling_index = (t[3] >> 20) & 0x1F;
    s.pow2pad = (t[3] >> 25) & 1;
    s.type = (t[3] >> 28) & 0xF;
    s.depth = (t[4] & 0x1FFF) + 1;
    s.pitch = ((t[4] >> 13) & 0x3FFF) + 1;
    s.base_array = t[5] & 0x1FFF;
    s.last_array = (t[5] >> 13) & 0x1FFF;
    return s;
}

DXGI_FORMAT TextureFormat(u32 dfmt, u32 nfmt, u32 dst_sel) {
    const bool srgb = nfmt == NUM_SRGB;
    const bool bgra = (dst_sel & 7) == 6 && ((dst_sel >> 6) & 7) == 4;  // x<-Z, z<-X
    switch (dfmt) {
    case FMT_8:
        return nfmt == NUM_SNORM ? DXGI_FORMAT_R8_SNORM : nfmt == NUM_UINT ? DXGI_FORMAT_R8_UINT
                                                                       : DXGI_FORMAT_R8_UNORM;
    case FMT_16:
        return nfmt == NUM_FLOAT ? DXGI_FORMAT_R16_FLOAT : nfmt == NUM_UINT ? DXGI_FORMAT_R16_UINT
                                                                         : DXGI_FORMAT_R16_UNORM;
    case FMT_8_8:
        return nfmt == NUM_SNORM ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_UNORM;
    case FMT_32:
        return nfmt == NUM_FLOAT ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT;
    case FMT_16_16:
        return nfmt == NUM_FLOAT ? DXGI_FORMAT_R16G16_FLOAT : nfmt == NUM_SNORM ? DXGI_FORMAT_R16G16_SNORM
                                                                             : DXGI_FORMAT_R16G16_UNORM;
    case FMT_10_11_11:
    case FMT_11_11_10:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case FMT_2_10_10_10:
    case FMT_10_10_10_2:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case FMT_8_8_8_8:
        // channel order (dst_sel) is applied in the translated shaders
        (void)bgra;
        if (nfmt == NUM_SNORM) return DXGI_FORMAT_R8G8B8A8_SNORM;
        if (nfmt == NUM_UINT) return DXGI_FORMAT_R8G8B8A8_UINT;
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case FMT_32_32:
        return nfmt == NUM_FLOAT ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32_UINT;
    case FMT_16_16_16_16:
        return nfmt == NUM_FLOAT ? DXGI_FORMAT_R16G16B16A16_FLOAT
               : nfmt == NUM_SNORM ? DXGI_FORMAT_R16G16B16A16_SNORM
               : nfmt == NUM_UINT  ? DXGI_FORMAT_R16G16B16A16_UINT
                                   : DXGI_FORMAT_R16G16B16A16_UNORM;
    case FMT_32_32_32:
        return nfmt == NUM_FLOAT ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32_UINT;
    case FMT_32_32_32_32:
        return nfmt == NUM_FLOAT ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32A32_UINT;
    case FMT_5_6_5:
        return DXGI_FORMAT_B5G6R5_UNORM;
    case FMT_1_5_5_5:
    case FMT_5_5_5_1:
        return DXGI_FORMAT_B5G5R5A1_UNORM;
    case FMT_4_4_4_4:
        return DXGI_FORMAT_B4G4R4A4_UNORM;
    case FMT_BC1:
        return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case FMT_BC2:
        return srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
    case FMT_BC3:
        return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case FMT_BC4:
        return nfmt == NUM_SNORM ? DXGI_FORMAT_BC4_SNORM : DXGI_FORMAT_BC4_UNORM;
    case FMT_BC5:
        return nfmt == NUM_SNORM ? DXGI_FORMAT_BC5_SNORM : DXGI_FORMAT_BC5_UNORM;
    case FMT_BC6:
        return nfmt == NUM_SNORM ? DXGI_FORMAT_BC6H_SF16 : DXGI_FORMAT_BC6H_UF16;
    case FMT_BC7:
        return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT TypelessOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
        return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default:
        return f;
    }
}

DXGI_FORMAT ColorTargetFormat(u32 info) {
    u32 fmt = (info >> 2) & 0x1F;
    u32 num = (info >> 8) & 7;  // 0 UNORM 1 SNORM 4 UINT 5 SINT 6 SRGB 7 FLOAT
    u32 tex_num = num == 6 ? NUM_SRGB : num;
    return TextureFormat(fmt, tex_num, 0xFAC);
}

// Vertex element formats. Integer data (uint/sint, and uscaled/sscaled which D3D11 lacks) is
// fetched as raw integers; VertexConversion() tells the shader to turn scaled data into floats.
DXGI_FORMAT VertexFormat(u32 df) {
    u32 sf = df & 0xFF;
    u32 ch = (df >> 8) & 0xF;  // 0 unorm 1 snorm 2 uscaled 3 sscaled 4 uint 5 sint 6 snorm_nz 7 float
    bool u = ch == 2 || ch == 4, s = ch == 3 || ch == 5, sn = ch == 1 || ch == 6;
    switch (sf) {
    case FMT_32:
        return ch == 7 ? DXGI_FORMAT_R32_FLOAT : u ? DXGI_FORMAT_R32_UINT : s ? DXGI_FORMAT_R32_SINT
                                                                         : DXGI_FORMAT_UNKNOWN;
    case FMT_32_32:
        return ch == 7 ? DXGI_FORMAT_R32G32_FLOAT : u ? DXGI_FORMAT_R32G32_UINT
                                                  : s ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_UNKNOWN;
    case FMT_32_32_32:
        return ch == 7 ? DXGI_FORMAT_R32G32B32_FLOAT : u ? DXGI_FORMAT_R32G32B32_UINT
                                                     : s ? DXGI_FORMAT_R32G32B32_SINT : DXGI_FORMAT_UNKNOWN;
    case FMT_32_32_32_32:
        return ch == 7 ? DXGI_FORMAT_R32G32B32A32_FLOAT : u ? DXGI_FORMAT_R32G32B32A32_UINT
                                                        : s ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_UNKNOWN;
    case FMT_16:
        return ch == 7 ? DXGI_FORMAT_R16_FLOAT : u ? DXGI_FORMAT_R16_UINT : s ? DXGI_FORMAT_R16_SINT
                                               : sn ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_UNORM;
    case FMT_16_16:
        return ch == 7 ? DXGI_FORMAT_R16G16_FLOAT : u ? DXGI_FORMAT_R16G16_UINT : s ? DXGI_FORMAT_R16G16_SINT
                                                  : sn ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM;
    case FMT_16_16_16_16:
        return ch == 7 ? DXGI_FORMAT_R16G16B16A16_FLOAT
               : u     ? DXGI_FORMAT_R16G16B16A16_UINT
               : s     ? DXGI_FORMAT_R16G16B16A16_SINT
               : sn    ? DXGI_FORMAT_R16G16B16A16_SNORM
                       : DXGI_FORMAT_R16G16B16A16_UNORM;
    case FMT_8:
        return u ? DXGI_FORMAT_R8_UINT : s ? DXGI_FORMAT_R8_SINT : sn ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_UNORM;
    case FMT_8_8_8_8:
        return u ? DXGI_FORMAT_R8G8B8A8_UINT : s ? DXGI_FORMAT_R8G8B8A8_SINT
               : sn ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    case FMT_8_8:
        return u ? DXGI_FORMAT_R8G8_UINT : s ? DXGI_FORMAT_R8G8_SINT
               : sn ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_UNORM;
    case FMT_2_10_10_10:
    case FMT_10_10_10_2:
        return u ? DXGI_FORMAT_R10G10B10A2_UINT : DXGI_FORMAT_R10G10B10A2_UNORM;
    case FMT_10_11_11:
    case FMT_11_11_10:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

u32 VertexConversion(u32 df) {
    u32 ch = (df >> 8) & 0xF;
    return ch == 2 ? 1 : ch == 3 ? 2 : 0;
}

u32 FormatBytes(DXGI_FORMAT f, bool* bc) {
    *bc = false;
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        *bc = true;
        return 8;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        *bc = true;
        return 16;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_UINT:
        return 1;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
        return 8;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
        return 12;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
        return 16;
    default:
        return 4;
    }
}
