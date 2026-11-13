// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the in-game menu on the Direct3D 11 engine (bbport_d3d11.cpp). Apart from
// bbport_overlay.h, which brings Vulkan's headers along.
#pragma once

#include <cstdint>

namespace BbOverlay {
/// Once: the ImGui context and its Direct3D 11 backend (an ID3D11Device, an ID3D11DeviceContext).
void InitD3d11(void* device, void* context);

/// Swap thread: draws into the render target bound to the context.
void RenderD3d11(std::uint32_t width, std::uint32_t height);
} // namespace BbOverlay
