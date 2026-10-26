// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: textures and samplers of the native Vulkan renderer (bbport_native_vk.cpp).
//
// A T# (GCN image descriptor) the engine binds becomes a VkImage: its guest memory is read and
// untiled on the CPU (linear aligned and 1D thin micro tiling, as the D3D11 port's
// port/src/gfx/texture.cpp), then an upload thread copies it in on the graphics queue, under
// shadPS4's submit lock. Engine sampler keys become VkSamplers.
//
// Once drawing is up (SetHost), a texture's guest pages are watched: a write or unmap makes it
// stale, and its next bind makes it again (a new track loaded where the last one was). Past the
// memory budget the textures unused longest are evicted; a texture is never refused for memory.
// Replaced textures are retired through the renderer, after the draws captured before.
#pragma once

#include <atomic>
#include <cstdint>

#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

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
    VmaAllocation allocation = nullptr;
    std::uint64_t base = 0, size = 0; // the guest memory it was made from
    std::uint64_t last_used = 0;      // Host::serial at its last bind
    bool watched = false;             // its pages are watched (Written makes it stale)
    std::atomic<bool> stale{false};   // written or unmapped since it was made
    /// Set once its upload is submitted: a draw submitted later sees the data (same queue, and
    /// the upload ends in a barrier). Draws recorded before that bind no image instead.
    std::atomic<bool> ready{false};
};

/// The fields of a T# the renderer needs besides the image.
struct Info {
    std::uint64_t base;
    std::uint32_t width, height, dfmt, nfmt, dst_sel, last_level;
    vk::Format format; // Undefined: not a format this renderer makes
};

struct Stats {
    std::uint64_t textures, bytes, uploads_done, uploads_pending, upload_batches, unsupported,
        unreadable, over_budget, failed, samplers, evicted, remade, watched_pages;
};

/// What replacing textures needs from the renderer: the game-side submission number, and the
/// retiring of a texture after everything captured before it (then Destroy, once the GPU is done).
struct Host {
    std::uint64_t (*serial)();
    void (*retire)(Texture* texture);
};

/// The presenter's Vulkan instance; starts the upload thread.
void Init(const Vulkan::Instance& instance);

/// Drawing is up: textures are watched, evicted and made again from now on (Get's thread).
void SetHost(const Host& host);

/// The guest wrote or unmapped [addr, addr + size): textures made from it go stale (the
/// renderer's write hook, any thread).
void Written(std::uint64_t addr, std::uint64_t size);

/// Frees a retired texture's image, views and the texture itself.
void Destroy(Texture* texture);

/// Base address of the image a T# (8 dwords) describes.
std::uint64_t TSharpBase(const std::uint32_t* tsharp);

/// A T#'s address, size, data / number format, channel selects and its Vulkan format.
Info Describe(const std::uint32_t* tsharp);

/// The texture for a T#, built (and queued for upload) the first time, and again once stale.
/// nullptr: not supported (format, tiling), not readable, or not created; each logged once.
const Texture* Get(const std::uint32_t* tsharp);

/// The sampler for an S# (4 dwords); `compare`: a depth comparison sampler.
VkSampler Sampler(const std::uint32_t ss[4], bool compare);

Stats GetStats();
} // namespace BbNativeVk::Tex
