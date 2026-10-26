// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native Vulkan renderer for the Alchemy render layer (milestone 2, opt-in with
// BB_NATIVE_VK=1). Like the D3D11 port, it reads the engine's own render state at every GnmDriver
// draw (igPS4GraphicsDeviceContext, docs/d3d11_engine_hooks_1.21.md) and uses the shaders
// translated ahead of time (tools/shaders/build_spirv.py: shaders.idx + SPIR-V).
//
// Stages 1-4 (docs/vulkan_shaders_spirv.md): shaders, render and depth targets, textures and
// samplers, vertex and index data read in place from guest memory.
// Stage 5, drawing: every draw is recorded into the renderer's own command buffers (pipelines,
// descriptor sets, constants) into its own targets, with the engine's clears and texture copies
// (hooked), and submitted when the game submits. shadPS4 still renders the frame too; with
// BB_VK_SHOW=1 the presenter shows the renderer's image of the display buffer instead.
#pragma once

#include <cstdint>

namespace Vulkan {
class Instance;
}

namespace BbNativeVk {
/// Notes the image base and hooks the engine's clear and copy (the loader calls it once, before
/// any game code runs). Reads the switch: BB_NATIVE_VK=1, BB_VK_SHADERS=<build_spirv.py output
/// folder: hlsl/shaders.idx, spirv/*.spv>; BB_VK_DRAW=0 keeps stage 4 (nothing drawn).
void PatchImage(unsigned char* image, std::uint64_t size);

/// The presenter's Vulkan instance (device and allocator), once it exists.
void Attach(const Vulkan::Instance& instance);

/// sceGnmDrawIndex / sceGnmDrawIndexAuto, while the engine records the draw.
void OnDraw(bool indexed, std::uint32_t count, std::uint64_t index_addr);

/// sceGnmSubmit(AndFlip)CommandBuffers: submits what was recorded since the last one.
void OnSubmit();

/// For the presenter (BB_VK_SHOW=1): the renderer's image (a VkImage) of the display buffer at
/// `address`, in General layout, and the VkFormat to view it with (`wanted`: the presenter's own
/// view format for that buffer).
bool Display(std::uint64_t address, std::uint32_t wanted, std::uint64_t* image,
             std::uint32_t* view_format, std::uint32_t* width, std::uint32_t* height);
} // namespace BbNativeVk
