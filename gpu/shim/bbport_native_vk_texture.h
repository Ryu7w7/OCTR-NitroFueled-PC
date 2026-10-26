// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: textures and samplers of the native Vulkan renderer (bbport_native_vk.cpp).
//
// A T# (GCN image descriptor) the engine binds becomes a VkImage: its guest memory is read and
// untiled on the CPU (linear aligned and 1D thin micro tiling, as the D3D11 port's
// port/src/gfx/texture.cpp), then an upload thread copies it in on the graphics queue, under
// shadPS4's submit lock. Engine sampler keys become VkSamplers. Textures are built once; data
// the CPU writes later (videos, runtime atlases) is not refreshed yet.
#pragma once

#include <cstdint>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbNativeVk::Tex {
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;       // 2D (one slice) or 2D array
    VkImageView view_array = VK_NULL_HANDLE; // always a 2D array (shaders that index slices)
    std::uint32_t width = 0, height = 0, levels = 0, slices = 0;
    vk::Format format = vk::Format::eUndefined;
    std::uint64_t bytes = 0;
};

struct Stats {
    std::uint64_t textures, bytes, uploads_done, uploads_pending, upload_batches, unsupported,
        unreadable, over_budget, failed, samplers;
};

/// The presenter's Vulkan instance; starts the upload thread.
void Init(const Vulkan::Instance& instance);

/// Base address of the image a T# (8 dwords) describes.
std::uint64_t TSharpBase(const std::uint32_t* tsharp);

/// The texture for a T#, built (and queued for upload) the first time. nullptr: not supported
/// (format, tiling), not readable, over the memory budget, or not created; each logged once.
const Texture* Get(const std::uint32_t* tsharp);

/// The sampler for an S# (4 dwords); `compare`: a depth comparison sampler.
VkSampler Sampler(const std::uint32_t ss[4], bool compare);

Stats GetStats();
} // namespace BbNativeVk::Tex
