// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: DLSS for the native Vulkan renderer (bbport_native_vk.cpp).
//
// The same inputs vk_ctr_fsr3 gives DLSS under shadPS4, from the native renderer's own images:
// the finished scene the display pass stretches (before the HUD), the scene depth, CTR's
// velocity target, and the camera's motion (from the G-buffer draws' constants). The depth and
// the motion are made by ctr_fsr3_inputs.comp, and NVIDIA's DLSS (vk_dlss_ngx, the optional
// bridge) upscales or anti-aliases into an image the display pass samples instead.
//
// Recorder thread only (the renderer's lock), like Gpu.
#pragma once

#include <array>
#include <cstdint>

#include "bbport_native_vk_gpu.h"

namespace Vulkan {
class Instance;
}

namespace BbNativeVk::Dlss {

/// Once, after Gpu::Init.
void Init(const Vulkan::Instance& instance);

/// BB_VK_DLSS=1 or temporal=dlss, and the bridge has not failed. Thread safe.
bool Wanted();

/// What one frame gives DLSS (images in any tracked layout; Upscale moves them).
struct Frame {
    Gpu::Image* color = nullptr;   // the finished scene (render size)
    VkImageView color_view = VK_NULL_HANDLE;
    bool hdr = false;              // the scene is linear HDR colour (the game's HDR output)
    Gpu::Image* depth = nullptr;   // the scene depth
    VkImageView depth_view = VK_NULL_HANDLE; // its depth aspect
    Gpu::Image* velocity = nullptr; // CTR's velocity target (G-buffer color 3)
    VkImageView velocity_view = VK_NULL_HANDLE;
    // the camera's motion: current view space -> previous clip space, and the projection
    // (column-major); camera false: no motion of the world
    std::array<float, 16> reproject{}, projection{};
    bool camera = false;
    // pixels = offset + scale * ndc (y flipped as drawn); depth = zoffset + zscale * ndc z
    float xscale = 0, yscale = 0, xoffset = 0, yoffset = 0, zscale = 0, zoffset = 0;
    std::array<float, 2> jitter{}; // pixels the scene's draws were moved by
    bool reset = false;            // no history (a cut, a load)
    Gpu::Image* output = nullptr;  // output size, storage + sampled
    VkImageView output_view = VK_NULL_HANDLE;
};

/// Records the inputs and DLSS into the open command buffer (ends rendering). Leaves the
/// output in General layout. False: not done this frame (the display pass samples the scene).
bool Upscale(const Frame& frame);

/// The scene draws' sub-pixel offset for the next frame (Halton 2,3, 8 phases), then the next.
std::array<float, 2> NextJitter();

} // namespace BbNativeVk::Dlss
