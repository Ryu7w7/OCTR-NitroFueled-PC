// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk_gpu.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace BbNativeVk::Gpu {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 RingSize = 64ull << 20;
constexpr u32 SetsPerPool = 1024;
constexpr std::size_t FrameCount = 6; // command buffers in flight at most

/// One command buffer and what its GPU work holds on to.
struct Frame {
    vk::CommandPool pool;
    vk::CommandBuffer cmd;
    vk::Fence fence;
    vk::QueryPool timestamps; // GPU start and end of the command buffer (null: not measured)
    bool in_flight = false;
    bool recorded = false;    // anything recorded since it was begun
    u64 ring_begin = 0, ring_end = 0; // its part of the ring, [begin, end) circularly
    std::vector<vk::DescriptorPool> pools;
    std::size_t pools_used = 0;
    std::vector<std::function<void()>> deferred;
};

const Vulkan::Instance* instance = nullptr;
vk::Device device;
std::array<Frame, FrameCount> frames;
std::size_t current = 0;

VkBuffer ring = VK_NULL_HANDLE;
VmaAllocation ring_allocation = nullptr;
std::uint8_t* ring_data = nullptr;
u64 ring_head = 0;

vk::PipelineCache pipeline_cache;

bool rendering = false;
Pass open_pass{};
std::vector<vk::ImageMemoryBarrier2> pending;

Stats stats{};
double tick_ns = 0; // nanoseconds per timestamp tick; 0: no timestamps

Frame& Cur() {
    return frames[current];
}

vk::ImageAspectFlags Aspects(const Image& image) {
    if (!image.depth) {
        return vk::ImageAspectFlagBits::eColor;
    }
    return image.stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                         : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth};
}

void Begin(Frame& f) {
    (void)f.cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    if (f.timestamps) {
        f.cmd.resetQueryPool(f.timestamps, 0, 2);
        f.cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, f.timestamps, 0);
    }
    f.recorded = false;
    f.ring_begin = f.ring_end = ring_head;
}

/// Waits for a frame's GPU work, then frees what it held.
void Retire(Frame& f) {
    if (!f.in_flight) {
        return;
    }
    if (device.getFenceStatus(f.fence) != vk::Result::eSuccess) {
        ++stats.waits;
        (void)device.waitForFences(f.fence, VK_TRUE, ~0ull);
    }
    (void)device.resetFences(f.fence);
    if (f.timestamps) {
        u64 t[2]{};
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(
                device, f.timestamps, 0, 2, sizeof(t), t, sizeof(u64), VK_QUERY_RESULT_64_BIT) ==
                VK_SUCCESS &&
            t[1] > t[0]) {
            stats.gpu_ns += u64(double(t[1] - t[0]) * tick_ns);
        }
    }
    for (auto& d : f.deferred) {
        d();
    }
    f.deferred.clear();
    for (std::size_t i = 0; i < f.pools_used; ++i) {
        (void)device.resetDescriptorPool(f.pools[i]);
    }
    f.pools_used = 0;
    (void)device.resetCommandPool(f.pool);
    f.in_flight = false;
}

/// Frees finished frames without waiting.
void Poll() {
    for (Frame& f : frames) {
        if (f.in_flight && device.getFenceStatus(f.fence) == vk::Result::eSuccess) {
            Retire(f);
        }
    }
}

/// Whether [a, a + n) overlaps the circular range [b, e) of the ring.
bool Overlaps(u64 a, u64 n, u64 b, u64 e) {
    if (b == e) {
        return false;
    }
    auto hit = [&](u64 lo, u64 hi) { return a < hi && lo < a + n; };
    return b < e ? hit(b, e) : hit(b, RingSize) || hit(0, e);
}

void SubmitFrame(bool early) {
    Frame& f = Cur();
    EndRendering();
    if (!pending.empty()) {
        Barrier(); // leftover layout changes (records, so the frame is submitted)
    }
    if (!f.recorded) {
        return;
    }
    if (f.timestamps) {
        f.cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, f.timestamps, 1);
    }
    (void)f.cmd.end();
    f.ring_end = ring_head;
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &f.cmd};
    vk::Result r;
    {
        // every graphics-queue submit and present in shadPS4 holds this lock
        std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
        r = instance->GetGraphicsQueue().submit(submit, f.fence);
    }
    if (r != vk::Result::eSuccess) {
        std::printf("Native Vulkan: submit failed (%s)\n", vk::to_string(r).c_str());
    }
    f.in_flight = r == vk::Result::eSuccess;
    ++stats.submits;
    stats.early_submits += early;
    current = (current + 1) % FrameCount;
    Retire(Cur());
    Poll();
    Begin(Cur());
}
} // namespace

bool Init(const Vulkan::Instance& vulkan) {
    instance = &vulkan;
    device = vulkan.GetDevice();
    const auto families = vulkan.GetPhysicalDevice().getQueueFamilyProperties();
    const u32 family = vulkan.GetGraphicsQueueFamilyIndex();
    if (family < families.size() && families[family].timestampValidBits) {
        tick_ns = vulkan.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    }
    for (Frame& f : frames) {
        auto [pr, pool] = device.createCommandPool(
            {.flags = vk::CommandPoolCreateFlagBits::eTransient,
             .queueFamilyIndex = vulkan.GetGraphicsQueueFamilyIndex()});
        auto [fr, fence] = device.createFence({});
        if (pr != vk::Result::eSuccess || fr != vk::Result::eSuccess) {
            return false;
        }
        f.pool = pool;
        f.fence = fence;
        auto [ar, cmds] = device.allocateCommandBuffers(
            {.commandPool = pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
        if (ar != vk::Result::eSuccess) {
            return false;
        }
        f.cmd = cmds[0];
        if (tick_ns > 0) {
            auto [qr, qp] = device.createQueryPool(
                {.queryType = vk::QueryType::eTimestamp, .queryCount = 2});
            f.timestamps = qr == vk::Result::eSuccess ? qp : vk::QueryPool{};
        }
    }
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = RingSize,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vulkan.GetAllocator(), &bci, &aci, &ring, &ring_allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    ring_data = static_cast<std::uint8_t*>(info.pMappedData);
    auto [cr, cache] = device.createPipelineCache({});
    pipeline_cache = cr == vk::Result::eSuccess ? cache : vk::PipelineCache{};
    Begin(Cur());
    return true;
}

vk::CommandBuffer Cmd() {
    Cur().recorded = true;
    return Cur().cmd;
}

void Submit() {
    SubmitFrame(false);
}

void Defer(std::function<void()> f) {
    Cur().deferred.push_back(std::move(f));
}

Span Upload(u64 size, u64 align) {
    if (size > RingSize / 2) {
        return {};
    }
    for (;;) {
        u64 pos = (ring_head + align - 1) / align * align;
        if (pos + size > RingSize) {
            pos = 0;
        }
        // not over the open command buffer's own data, nor over a frame still on the GPU
        bool free = !Overlaps(pos, size, Cur().ring_begin, ring_head);
        Frame* blocking = nullptr;
        for (std::size_t k = 1; k < FrameCount && free; ++k) {
            Frame& f = frames[(current + k) % FrameCount]; // oldest first
            if (f.in_flight && Overlaps(pos, size, f.ring_begin, f.ring_end)) {
                blocking = &f;
                free = false;
            }
        }
        if (free) {
            ring_head = pos + size;
            stats.ring_bytes += size;
            return {ring, pos, ring_data + pos};
        }
        if (blocking) {
            Retire(*blocking);
        } else {
            SubmitFrame(true); // the ring is full of this command buffer's data
        }
    }
}

void Use(Image& image, vk::ImageLayout layout) {
    if (image.layout == layout) {
        return;
    }
    EndRendering();
    pending.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = image.layout,
        .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = {.aspectMask = Aspects(image),
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
    });
    image.layout = layout;
}

void Barrier() {
    const vk::MemoryBarrier2 all{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    Cmd().pipelineBarrier2({.memoryBarrierCount = 1,
                            .pMemoryBarriers = &all,
                            .imageMemoryBarrierCount = u32(pending.size()),
                            .pImageMemoryBarriers = pending.data()});
    pending.clear();
    ++stats.barriers;
}

void Render(const Pass& pass) {
    if (rendering && std::equal(std::begin(pass.color), std::end(pass.color),
                                std::begin(open_pass.color)) &&
        pass.depth == open_pass.depth && pass.depth_read_only == open_pass.depth_read_only) {
        return;
    }
    EndRendering();
    u32 w = ~0u, h = ~0u, count = 0;
    std::array<vk::RenderingAttachmentInfo, 8> colors{};
    for (u32 i = 0; i < 8; ++i) {
        if (Image* c = pass.color[i]) {
            Use(*c, vk::ImageLayout::eColorAttachmentOptimal);
            colors[i] = {.imageView = c->attach,
                         .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                         .loadOp = vk::AttachmentLoadOp::eLoad,
                         .storeOp = vk::AttachmentStoreOp::eStore};
            w = std::min(w, c->width);
            h = std::min(h, c->height);
            count = i + 1;
        }
    }
    vk::RenderingAttachmentInfo depth{}, stencil{};
    if (Image* d = pass.depth) {
        const vk::ImageLayout l = pass.depth_read_only ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                       : vk::ImageLayout::eDepthStencilAttachmentOptimal;
        Use(*d, l);
        depth = {.imageView = d->attach,
                 .imageLayout = l,
                 .loadOp = vk::AttachmentLoadOp::eLoad,
                 .storeOp = pass.depth_read_only ? vk::AttachmentStoreOp::eNone
                                                 : vk::AttachmentStoreOp::eStore};
        if (d->stencil) {
            stencil = depth;
        }
        w = std::min(w, d->width);
        h = std::min(h, d->height);
    }
    if (w == ~0u) {
        return;
    }
    Barrier();
    Cmd().beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, {w, h}},
        .layerCount = 1,
        .colorAttachmentCount = count,
        .pColorAttachments = colors.data(),
        .pDepthAttachment = pass.depth ? &depth : nullptr,
        .pStencilAttachment = pass.depth && pass.depth->stencil ? &stencil : nullptr,
    });
    rendering = true;
    open_pass = pass;
    ++stats.passes;
}

void EndRendering() {
    if (rendering) {
        Cur().cmd.endRendering();
        rendering = false;
    }
}

bool Rendering() {
    return rendering;
}

void ClearColor(Image& image, const float color[4]) {
    Use(image, vk::ImageLayout::eTransferDstOptimal);
    EndRendering();
    Barrier();
    vk::ClearColorValue value{};
    std::copy(color, color + 4, value.float32.begin());
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    Cmd().clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal, value, range);
}

void ClearDepthStencil(Image& image, bool depth, float z, bool stencil, u32 s) {
    stencil = stencil && image.stencil;
    if (!depth && !stencil) {
        return;
    }
    Use(image, vk::ImageLayout::eTransferDstOptimal);
    EndRendering();
    Barrier();
    vk::ImageAspectFlags aspects{};
    if (depth) {
        aspects |= vk::ImageAspectFlagBits::eDepth;
    }
    if (stencil) {
        aspects |= vk::ImageAspectFlagBits::eStencil;
    }
    const vk::ImageSubresourceRange range{aspects, 0, 1, 0, 1};
    Cmd().clearDepthStencilImage(image.image, vk::ImageLayout::eTransferDstOptimal,
                                 vk::ClearDepthStencilValue{z, s}, range);
}

vk::DescriptorSet AllocateSet(vk::DescriptorSetLayout layout) {
    Frame& f = Cur();
    for (bool fresh = false;; fresh = true) {
        // pools [0, pools_used) are in use; allocate from the last, else take the next one
        if (f.pools_used && !fresh) {
            auto [r, sets] = device.allocateDescriptorSets({.descriptorPool = f.pools[f.pools_used - 1],
                                                            .descriptorSetCount = 1,
                                                            .pSetLayouts = &layout});
            if (r == vk::Result::eSuccess) {
                ++stats.sets;
                return sets[0];
            }
            if (r != vk::Result::eErrorOutOfPoolMemory && r != vk::Result::eErrorFragmentedPool) {
                return {};
            }
            continue;
        }
        if (f.pools_used == f.pools.size()) {
            static constexpr std::array<vk::DescriptorPoolSize, 7> sizes{{
                {vk::DescriptorType::eUniformBuffer, SetsPerPool * 6},
                {vk::DescriptorType::eSampledImage, SetsPerPool * 8},
                {vk::DescriptorType::eSampler, SetsPerPool * 8},
                {vk::DescriptorType::eStorageBuffer, 64},
                {vk::DescriptorType::eStorageImage, 64},
                {vk::DescriptorType::eUniformTexelBuffer, 64},
                {vk::DescriptorType::eStorageTexelBuffer, 64},
            }};
            auto [r, pool] = device.createDescriptorPool(
                {.maxSets = SetsPerPool, .poolSizeCount = u32(sizes.size()), .pPoolSizes = sizes.data()});
            if (r != vk::Result::eSuccess) {
                return {};
            }
            f.pools.push_back(pool);
            ++stats.pools;
        }
        const vk::DescriptorPool pool = f.pools[f.pools_used++];
        auto [r, sets] = device.allocateDescriptorSets(
            {.descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &layout});
        if (r == vk::Result::eSuccess) {
            ++stats.sets;
            return sets[0];
        }
        return {}; // not even an empty pool holds it
    }
}

vk::PipelineCache PipelineCache() {
    return pipeline_cache;
}

Stats GetStats() {
    return stats;
}
} // namespace BbNativeVk::Gpu
