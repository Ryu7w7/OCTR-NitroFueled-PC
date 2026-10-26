// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: command recording for the native Vulkan renderer (bbport_native_vk.cpp).
//
// The renderer records into its own command buffers as the game issues draws, and submits them
// when the game submits its own command buffers (sceGnmSubmit*), on the graphics queue under
// shadPS4's submit lock. Per submission it keeps a span of an upload ring (constants, generated
// indices), descriptor pools, and the objects to destroy once the GPU is done with it.
//
// Image layouts are tracked per image. Any layout change, and the start of every rendering pass,
// goes with one full memory barrier: simple and safe; finer barriers can come later.
//
// Not thread safe: the renderer calls all of this under its own lock.
#pragma once

#include <cstdint>
#include <functional>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbNativeVk::Gpu {
/// An image the recorder tracks the layout of (render targets).
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView attach = VK_NULL_HANDLE; // the attachment view: color, or depth + stencil
    vk::Format format = vk::Format::eUndefined;
    std::uint32_t width = 0, height = 0;
    bool depth = false, stencil = false;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};

/// Attachments of a rendering pass. `depth_read_only`: the depth image is also sampled.
struct Pass {
    Image* color[8]{};
    Image* depth = nullptr;
    bool depth_read_only = false;
};

/// Ring memory for one submission.
struct Span {
    VkBuffer buffer = VK_NULL_HANDLE;
    std::uint64_t offset = 0;
    void* data = nullptr;
};

struct Stats {
    std::uint64_t submits, early_submits, waits, passes, barriers, ring_bytes, sets, pools;
};

bool Init(const Vulkan::Instance& instance);

/// The open command buffer.
vk::CommandBuffer Cmd();

/// Ends the open command buffer and submits it. Nothing happens when nothing was recorded.
void Submit();

/// Runs `f` once the GPU has finished everything recorded so far.
void Defer(std::function<void()> f);

/// `size` bytes of the upload ring for the open command buffer (submits early when the ring is
/// full of this command buffer's own data). data == nullptr: larger than the ring.
Span Upload(std::uint64_t size, std::uint64_t align);

/// The image in `layout` for what follows (a barrier when it changes; ends rendering).
void Use(Image& image, vk::ImageLayout layout);

/// Starts rendering to `pass` unless it is the pass already open.
void Render(const Pass& pass);
void EndRendering();
bool Rendering();

void ClearColor(Image& image, const float color[4]);
void ClearDepthStencil(Image& image, bool depth, float z, bool stencil, std::uint32_t s);

/// Emits the pending layout changes (and a full memory barrier) outside rendering.
void Barrier();

/// A descriptor set of `layout`, valid for the open command buffer only.
vk::DescriptorSet AllocateSet(vk::DescriptorSetLayout layout);

vk::PipelineCache PipelineCache();

Stats GetStats();
} // namespace BbNativeVk::Gpu
