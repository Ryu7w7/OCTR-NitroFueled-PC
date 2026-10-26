// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native Vulkan renderer for the Alchemy render layer (milestone 2, opt-in with
// BB_NATIVE_VK=1). Like the D3D11 port, it reads the engine's own render state at every GnmDriver
// draw (igPS4GraphicsDeviceContext, docs/d3d11_engine_hooks_1.21.md) and uses the shaders
// translated ahead of time (tools/shaders/build_spirv.py: shaders.idx + SPIR-V).
//
// This first stage is the front end only: each draw's shaders are resolved to SPIR-V and loaded as
// VkShaderModules on the presenter's device, and the distinct pipeline states are counted. Nothing
// is drawn by it yet; shadPS4 still renders the frame.
#pragma once

#include <cstdint>

namespace BbNativeVk {
/// Notes the image base (the loader calls it once, before any game code runs). Reads the switch:
/// BB_NATIVE_VK=1, BB_VK_SHADERS=<build_spirv.py output folder: hlsl/shaders.idx, spirv/*.spv>.
void PatchImage(unsigned char* image, std::uint64_t size);

/// The presenter's Vulkan device (VkDevice), once it exists.
void Attach(void* device);

/// sceGnmDrawIndex / sceGnmDrawIndexAuto, while the engine records the draw.
void OnDraw(bool indexed, std::uint32_t count, std::uint64_t index_addr);
} // namespace BbNativeVk
