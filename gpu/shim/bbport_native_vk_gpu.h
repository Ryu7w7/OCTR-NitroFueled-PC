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
#include <string>
#include <vector>

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
    // a clear not recorded yet: the next pass does it as its load op, any other use before it
    vk::ImageAspectFlags clear{};
    vk::ClearValue clear_value{};
    bool discard = false; // the next layout change may drop the contents (all of them are cleared)
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

/// What GPU time is spent on (timestamps around each pass, clear and copy).
enum class Work : std::uint8_t { Other, Pass, ClearColor, ClearDepth, Copy, DepthCopy };
struct WorkTime {
    std::string what;
    std::uint64_t ns, count, draws;
};

struct Stats {
    std::uint64_t submits, early_submits, waits, passes, barriers, ring_bytes, sets, pools;
    // clears: done as a pass's load op, inside the open pass, recorded as a transfer command
    std::uint64_t load_clears, pass_clears, recorded_clears;
    std::uint64_t gpu_ns; // GPU time of the finished command buffers (timestamps)
};

/// `cache_path`: where the pipeline cache is loaded from and saved to (empty: not kept).
bool Init(const Vulkan::Instance& instance, const std::string& cache_path);
/// Starts timing `kind` on the GPU (until the next mark), for an image of this size and format.
void Mark(Work kind, std::uint32_t width, std::uint32_t height, vk::Format format);
/// One more draw in the work being timed.
void NoteDraw();
/// GPU time by work since the last call, most first.
std::vector<WorkTime> TakeWorkTimes();
/// Writes the pipeline cache to its file.
void SaveCache();

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

/// Clears are kept until the image is next used: a pass covering the whole image clears it as
/// its load op (a fast clear), any other use records the clear first. An image of the open pass
/// is cleared there (vkCmdClearAttachments).
void ClearColor(Image& image, const float color[4]);
void ClearDepthStencil(Image& image, bool depth, float z, bool stencil, std::uint32_t s);

/// Emits the pending layout changes outside rendering (each waits only on the stages its old
/// layout is used by; BB_VK_FULL_BARRIERS=1 adds a full memory barrier, for comparison).
void Barrier();

/// A descriptor set of `layout`, valid for the open command buffer only.
vk::DescriptorSet AllocateSet(vk::DescriptorSetLayout layout);

vk::PipelineCache PipelineCache();

Stats GetStats();
} // namespace BbNativeVk::Gpu
