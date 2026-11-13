#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include "formats.h"

struct TextureObject {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv, srv_array;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_cube;  // of a cube map: TextureCube slots
    u64 mem_base = 0, mem_size = 0;  // guest memory the texture was built from
    u64 checked_frame = 0;           // last frame the guest copy was checked for writes
    u64 seen_writes = 0;             // GuestWriteCount of the last check that found it clean
    u64 used_frame = 0;              // last frame a draw asked for it
    u32 levels = 1;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool button_atlas = false;  // the font's button glyphs (see glyphs.h)
};

bool TextureSupported(const TSharp& t);
// Detiles the texture memory described by `t` and creates a D3D11 texture (top mip level).
bool BuildTexture(ID3D11Device* dev, const TSharp& t, TextureObject* out);
// Re-uploads the guest data of a texture built from the same T# (CPU-updated textures).
bool RefreshTexture(ID3D11DeviceContext* ctx, const TSharp& t, TextureObject* obj);
