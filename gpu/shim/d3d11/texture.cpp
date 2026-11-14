// Converts PS4 texture memory (GCN tiled layouts) into D3D11 textures, with their mip chains.
//
// Supported: linear aligned (tile mode 8) and 1D thin micro tiling (5, 9, 13): 8x8-element micro
// tiles stored row-major, elements in Morton order inside a tile (non-displayable / depth micro
// tile ordering: x0 y0 x1 y1 x2 y2). For block-compressed formats an "element" is one 4x4 block.
//
// Mip layout (GCN addrlib rules, as in shadPS4's ImageInfo::UpdateSize): level m starts from
// pitch >> m (and pow2-padded when the T# asks for it), the micro-tiled pitch is aligned to 8
// elements and grown until a slice is a multiple of 256 bytes; levels follow each other, each
// level holding all array slices / cube faces.
#include "texture.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <vector>

#include "glue.h"
#include "glyphs.h"
#include "tiling.h"

namespace {

u32 Align(u32 v, u32 a) { return (v + a - 1) / a * a; }

bool IsMicroTiled1D(u32 mode) { return mode == 5 || mode == 9 || mode == 13; }

struct Mip {
    u32 ew, eh;        // elements to copy (texels or 4x4 blocks)
    u32 row_e;         // tiled pitch in elements
    u64 offset;        // byte offset of the level (all slices)
    u64 slice_bytes;   // bytes per slice of this level
};

struct Layout {
    DXGI_FORMAT fmt;
    u32 eb, bw;  // element bytes, block width (1 or 4)
    u32 slices, levels;
    bool cube;
    bool macro;           // 2D thin macro tiling (one level, one slice)
    u32 macro_height;     // the macro-tile-aligned height of level 0
    u32 width0, height0;  // D3D level 0 size
    Mip mips[16];
    u64 total;
};

bool GetLayout(const TSharp& t, Layout* l) {
    l->fmt = TextureFormat(t.dfmt, t.nfmt, t.dst_sel);
    if (l->fmt == DXGI_FORMAT_UNKNOWN || !TextureSupported(t)) return false;
    bool bc;
    l->eb = FormatBytes(l->fmt, &bc);
    l->bw = bc ? 4 : 1;
    l->cube = t.type == 11;
    bool array = t.type == 12 || t.type == 13 || l->cube;
    l->slices = l->cube ? 6 : array ? (t.last_array - t.base_array + 1) : 1;
    if (t.type == 10) l->slices = 1;  // 3D textures: only the first slice for now
    l->levels = std::min<u32>(t.last_level + 1, 16);
    l->width0 = bc ? Align(t.width, 4) : t.width;
    l->height0 = bc ? Align(t.height, 4) : t.height;
    l->macro = !IsMicroTiled1D(t.tiling_index) && t.tiling_index != 8;
    const bool pow2 = t.pow2pad;
    // each level holds all slices; with pow2pad the count in memory is a power of two too (a cube
    // map with mips has 8 slices per level, as shadPS4's NumLayers)
    const u32 stored_slices = pow2 ? std::bit_ceil(l->slices) : l->slices;
    u64 offset = 0;
    for (u32 m = 0; m < l->levels; m++) {
        Mip& mp = l->mips[m];
        u32 w = std::max(1u, l->width0 >> m), h = std::max(1u, l->height0 >> m);
        mp.ew = (w + l->bw - 1) / l->bw;
        mp.eh = (h + l->bw - 1) / l->bw;
        // padded size in memory
        u32 pw = std::max(1u, t.pitch >> m), ph = std::max(1u, t.height >> m);
        pw = (pw + l->bw - 1) / l->bw;
        ph = (ph + l->bw - 1) / l->bw;
        if (pow2) {
            pw = std::bit_ceil(pw);
            ph = std::bit_ceil(ph);
        }
        if (l->macro) {
            D3dTiling::MacroSize(t.tiling_index, pw, ph, l->eb, &mp.row_e, &l->macro_height, &mp.slice_bytes);
        } else if (t.tiling_index == 8) {
            u32 bytes = l->eb;
            u32 pitch_align = std::max(8u, 64u / bytes);
            u32 row = Align(pw, pitch_align);
            u32 slice_align = std::max(64u, 256u / bytes);
            while (((u64)row * ph) % slice_align) row += pitch_align;
            mp.row_e = row;
            mp.slice_bytes = (u64)row * ph * bytes;
        } else {
            u32 row = Align(pw, 8), rows = Align(ph, 8);
            while (((u64)row * rows * l->eb) % 256) row += 8;
            mp.row_e = row;
            mp.slice_bytes = (u64)row * rows * l->eb;
        }
        mp.ew = std::min(mp.ew, mp.row_e);
        mp.offset = offset;
        offset += mp.slice_bytes * stored_slices;
    }
    l->total = offset;
    return GuestReadable(t.base, l->total);
}

// Detiles one level of one slice into tightly packed rows.
void DetileLevel(const TSharp& t, const Layout& l, u32 m, u32 s, u8* dst) {
    const Mip& mp = l.mips[m];
    const u8* sb = (const u8*)t.base + mp.offset + s * mp.slice_bytes;
    const size_t row_bytes = (size_t)mp.ew * l.eb;
    if (l.macro) {
        D3dTiling::DetileMacro(t.tiling_index, l.eb, sb, mp.slice_bytes, mp.row_e, l.macro_height, mp.ew, mp.eh, dst);
        return;
    }
    if (t.tiling_index == 8) {
        for (u32 y = 0; y < mp.eh; y++) memcpy(dst + y * row_bytes, sb + (u64)y * mp.row_e * l.eb, row_bytes);
        return;
    }
    D3dTiling::DetileMicro(l.eb, sb, mp.row_e, mp.ew, mp.eh, dst);
}

// All subresources (slice-major, mips inside: D3D11CalcSubresource order) into one buffer.
void Detile(const TSharp& t, const Layout& l, std::vector<u8>& linear, std::vector<D3D11_SUBRESOURCE_DATA>& init) {
    size_t total = 0;
    for (u32 m = 0; m < l.levels; m++) total += (size_t)l.mips[m].ew * l.mips[m].eh * l.eb * l.slices;
    linear.resize(total);
    init.resize((size_t)l.levels * l.slices);
    size_t pos = 0;
    for (u32 s = 0; s < l.slices; s++) {
        for (u32 m = 0; m < l.levels; m++) {
            const Mip& mp = l.mips[m];
            DetileLevel(t, l, m, s, linear.data() + pos);
            init[(size_t)s * l.levels + m] = {linear.data() + pos, mp.ew * l.eb, mp.ew * mp.eh * l.eb};
            pos += (size_t)mp.ew * mp.eh * l.eb;
        }
    }
}

// Debugging aid: CTR_DUMP_TEXTURES=1 saves uncompressed 32-bit textures (level 0) as BMP files
// to %APPDATA%/CTRNF/texdump/<address>_<w>x<h>.bmp
void DumpTexture(const TSharp& t, const Layout& l, const std::vector<D3D11_SUBRESOURCE_DATA>& init) {
    static const bool on = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("CTR_DUMP_TEXTURES", v, sizeof(v)) && v[0] == '1';
    }();
    if (!on || l.eb != 4 || l.bw != 1) return;
    std::filesystem::path dir = UserRoot() / "texdump";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[64];
    snprintf(name, sizeof(name), "%llx_%ux%u.bmp", (unsigned long long)t.base, t.width, t.height);
    FILE* f = _wfopen((dir / name).c_str(), L"wb");
    if (!f) return;
    u32 w = l.mips[0].ew, h = l.mips[0].eh, size = w * h * 4;
    u8 hdr[54] = {'B', 'M'};
    auto put32 = [&](int o, u32 v) { memcpy(hdr + o, &v, 4); };
    put32(2, 54 + size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, (u32)-(s32)h);  // top-down
    hdr[26] = 1;
    hdr[28] = 32;
    put32(34, size);
    fwrite(hdr, 1, 54, f);
    const u8* src = (const u8*)init[0].pSysMem;
    std::vector<u8> row(w * 4);
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {  // RGBA -> BGRA
            const u8* p = src + ((size_t)y * w + x) * 4;
            row[x * 4 + 0] = p[2], row[x * 4 + 1] = p[1], row[x * 4 + 2] = p[0], row[x * 4 + 3] = p[3];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

}  // namespace

bool TextureSupported(const TSharp& t) {
    if (t.tiling_index == 8 || IsMicroTiled1D(t.tiling_index)) return true;
    // one level of one 2D slice (video frames, menu layers): the rest has rules not done here
    return D3dTiling::MacroSupported(t.tiling_index) && (t.type == 9 || t.type == 8) && t.last_level == 0 &&
           t.base_level == 0;
}

bool BuildTexture(ID3D11Device* dev, const TSharp& t, TextureObject* out) {
    Layout l;
    if (!GetLayout(t, &l)) return false;
    out->mem_base = t.base;
    out->mem_size = l.total;
    GuestResetWritten(out->mem_base, out->mem_size);  // later guest writes mark the copy stale
    std::vector<u8> linear;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    Detile(t, l, linear, init);
    DumpTexture(t, l, init);
    out->fmt = l.fmt;
    if (l.eb == 4 && l.bw == 1 && l.slices == 1 && IsButtonAtlas(l.mips[0].ew, l.mips[0].eh, init[0].pSysMem)) {
        out->button_atlas = true;
        NoteButtonAtlas(init[0].pSysMem);
        LOG("gfx: button glyph atlas at %llx (format %d, %u levels, dst_sel %03o)", (unsigned long long)t.base,
            (int)l.fmt, l.levels, t.dst_sel);
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = l.width0;
    td.Height = l.height0;
    td.MipLevels = l.levels;
    td.ArraySize = l.slices;
    td.Format = l.fmt;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = l.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    if (FAILED(dev->CreateTexture2D(&td, init.data(), &out->tex))) {
        // fall back to the top level alone (odd mip chains)
        td.MipLevels = 1;
        std::vector<D3D11_SUBRESOURCE_DATA> top(l.slices);
        for (u32 s = 0; s < l.slices; s++) top[s] = init[(size_t)s * l.levels];
        if (FAILED(dev->CreateTexture2D(&td, top.data(), &out->tex))) return false;
        l.levels = 1;
    }
    out->levels = l.levels;
    u32 first = std::min(t.base_level, l.levels - 1);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = l.fmt;
    if (l.slices == 1) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MostDetailedMip = first;
        sv.Texture2D.MipLevels = l.levels - first;
        dev->CreateShaderResourceView(out->tex.Get(), &sv, &out->srv);
    }
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MostDetailedMip = first;
    sv.Texture2DArray.MipLevels = l.levels - first;
    sv.Texture2DArray.ArraySize = l.slices;
    dev->CreateShaderResourceView(out->tex.Get(), &sv, &out->srv_array);
    if (!out->srv) out->srv = out->srv_array;
    if (l.cube) {
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MostDetailedMip = first;
        sv.TextureCube.MipLevels = l.levels - first;
        dev->CreateShaderResourceView(out->tex.Get(), &sv, &out->srv_cube);
    }
    return true;
}

bool RefreshTexture(ID3D11DeviceContext* ctx, const TSharp& t, TextureObject* obj) {
    Layout l;
    if (!obj->tex || !GetLayout(t, &l)) return false;
    GuestResetWritten(obj->mem_base, obj->mem_size);
    static thread_local std::vector<u8> linear;
    static thread_local std::vector<D3D11_SUBRESOURCE_DATA> init;
    Detile(t, l, linear, init);
    u32 levels = std::min(obj->levels, l.levels);
    for (u32 s = 0; s < l.slices; s++)
        for (u32 m = 0; m < levels; m++) {
            const auto& d = init[(size_t)s * l.levels + m];
            ctx->UpdateSubresource(obj->tex.Get(), D3D11CalcSubresource(m, s, obj->levels), nullptr, d.pSysMem,
                                   d.SysMemPitch, d.SysMemSlicePitch);
        }
    return true;
}
