// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the port's Xbox button glyphs are not used here (OCTR has its own button prompts).
#pragma once

#include <d3d11.h>

#include "common.h"

inline bool IsButtonAtlas(u32, u32, const void*) {
    return false;
}
inline void NoteButtonAtlas(const void*) {}
inline ID3D11ShaderResourceView* ButtonAtlasOverride(ID3D11Device*, DXGI_FORMAT) {
    return nullptr;
}
