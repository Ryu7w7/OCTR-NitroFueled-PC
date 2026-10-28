// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk_gpu.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <atomic>
#include <map>
#include <string>
#include <tuple>
#include <mutex>
#include <thread>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace BbNativeVk::Gpu {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Vertex data is copied into it every draw. BB_VK_RING_MB (lab, default 256): a dedicated GPU
// without resizable BAR has a ~256 MiB device-local host-visible heap, so a 256 MiB ring lands in
// system memory and every draw's vertices cross PCIe; a smaller one can sit in video memory.
u64 RingSize = 256ull << 20;
constexpr u32 MaxMarks = 1024;         // timestamps per command buffer

/// One timed piece of a command buffer: from its timestamp to the next one.
struct Label {
    Work kind = Work::Other;
    u32 width = 0, height = 0;
    vk::Format format = vk::Format::eUndefined;
    u32 draws = 0;
    const char* note = nullptr;
};
struct WorkAcc {
    u64 ns = 0, count = 0, draws = 0;
};
constexpr u32 SetsPerPool = 1024;
constexpr std::size_t FrameCount = 6; // command buffers in flight at most

/// One command buffer and what its GPU work holds on to.
struct Frame {
    vk::CommandPool pool;
    vk::CommandBuffer cmd;
    vk::CommandBuffer prelude; // uploads into resident buffers, run before `cmd`
    bool prelude_open = false;
    vk::Fence fence;
    vk::QueryPool timestamps; // GPU start and end of the command buffer (null: not measured)
    std::vector<Label> labels; // label i runs from timestamp i to timestamp i + 1
    bool in_flight = false;
    bool recorded = false;    // anything recorded since it was begun
    bool traced = false;      // BB_VK_PASS_TRACE: its GPU times are logged when it retires
    u64 ring_begin = 0, ring_end = 0; // its part of the ring, [begin, end) circularly
    std::vector<vk::DescriptorPool> pools;
    std::size_t pools_used = 0;
    std::vector<std::function<void()>> deferred;
};

const Vulkan::Instance* instance = nullptr;
vk::Device device;
std::array<Frame, FrameCount> frames;
std::size_t current = 0;

// lab: 16 bytes of zeros, then a float4 of ones, for vertex inputs no engine element feeds
VkBuffer constant_vertices = VK_NULL_HANDLE;
VmaAllocation constant_vertices_allocation = nullptr;
VkBuffer ring = VK_NULL_HANDLE; // RingSize, then uniform_range of zeros (UniformBuffer)
u64 uniform_range = 65536;
VmaAllocation ring_allocation = nullptr;
std::uint8_t* ring_data = nullptr;
u64 ring_head = 0;

vk::PipelineCache pipeline_cache;
std::string cache_file;

bool rendering = false;
Pass open_pass{};
vk::Extent2D open_area{};
std::vector<vk::ImageMemoryBarrier2> pending;
bool full_barriers = false; // BB_VK_FULL_BARRIERS=1

// BB_VK_PASS_TRACE=1: every 1000th command buffer logs its passes, their draws and what ended
// each (the image changing layout and to which, the next pass, a clear, the submit)
bool trace_passes = false;
u64 trace_every = 1000; // BB_VK_PASS_TRACE_EVERY (lab): one command buffer in this many is traced
bool tracing = false; // the open command buffer is traced
// lab: GPU time of every draw's shader in the traced command buffers, for the whole run
std::map<std::string, WorkAcc> draw_times;
u64 traced_ns = 0, traced_buffers = 0;
std::atomic<u64> last_gpu_ns{0}; // GPU time of the last finished command buffer
u32 trace_draws = 0;
// the traced command buffer's lines, collected here and written to their own file by a thread of
// their own: a thousand lines on the console fill its pipe, and the game's next print waits
std::string trace_text;
std::mutex trace_mutex;
std::condition_variable trace_cv;
std::deque<std::string> trace_queue;

/// Writes finished traces to logs/<time>.trace.txt (beside BB_FRAME_LOG), else pass_trace.txt.
void TraceWriter() {
    std::string path = "pass_trace.txt";
    if (const char* frames = std::getenv("BB_FRAME_LOG"); frames && *frames) {
        path = frames;
        const auto dot = path.rfind(".frames.csv");
        path = (dot != std::string::npos ? path.substr(0, dot) : path) + ".trace.txt";
    }
    std::FILE* out = std::fopen(path.c_str(), "w");
    std::printf("Native Vulkan: pass traces go to %s\n", out ? path.c_str() : "nowhere (not opened)");
    for (;;) {
        std::string text;
        {
            std::unique_lock lk(trace_mutex);
            trace_cv.wait(lk, [] { return !trace_queue.empty(); });
            text = std::move(trace_queue.front());
            trace_queue.pop_front();
        }
        if (out) {
            std::fwrite(text.data(), 1, text.size(), out);
            std::fflush(out);
        }
    }
}

/// Hands the collected trace to the writer.
void FlushTrace() {
    if (trace_text.empty()) {
        return;
    }
    {
        std::lock_guard lk(trace_mutex);
        trace_queue.push_back(std::move(trace_text));
    }
    trace_text.clear();
    trace_cv.notify_one();
}
const char* end_kind = "renderer"; // what ends the open pass, for the trace
const Image* end_image = nullptr;
vk::ImageLayout end_layout{};

} // namespace

std::string Describe(const Image* i) {
    if (!i) {
        return "-";
    }
    char b[96];
    std::snprintf(b, sizeof(b), "%ux%u %s #%04x", i->width, i->height, vk::to_string(i->format).c_str(),
                  unsigned((reinterpret_cast<std::uintptr_t>(i) >> 4) & 0xffff));
    return b;
}

bool Tracing() {
    return tracing;
}

void Trace(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0) {
        trace_text.append(line, std::min<std::size_t>(std::size_t(n), sizeof(line) - 1));
    }
}

namespace {
u64 state_epoch = 1;        // StateEpoch

Stats stats{};
double tick_ns = 0; // nanoseconds per timestamp tick; 0: no timestamps
std::map<std::string, WorkAcc> work_times;

std::string WorkName(const Label& l) {
    static constexpr const char* names[] = {"other", "pass", "clear", "depth clear", "copy",
                                            "depth copy", "  draw"};
    std::string s = names[int(l.kind)];
    if (l.note) {
        s += std::string(" ") + l.note;
    }
    if (l.width) {
        s += " " + std::to_string(l.width) + "x" + std::to_string(l.height) + " " +
             vk::to_string(l.format);
    }
    return s;
}

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

/// The stages an image in `layout` is used by, and the accesses there.
struct LayoutUse {
    vk::PipelineStageFlags2 stages;
    vk::AccessFlags2 read, write;
};
LayoutUse UseOf(vk::ImageLayout layout) {
    using S = vk::PipelineStageFlagBits2;
    using A = vk::AccessFlagBits2;
    constexpr vk::PipelineStageFlags2 tests = S::eEarlyFragmentTests | S::eLateFragmentTests;
    constexpr vk::PipelineStageFlags2 shaders = S::eVertexShader | S::eFragmentShader;
    switch (layout) {
    case vk::ImageLayout::eUndefined:
        return {S::eNone, {}, {}};
    case vk::ImageLayout::eColorAttachmentOptimal:
        return {S::eColorAttachmentOutput, A::eColorAttachmentRead, A::eColorAttachmentWrite};
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
        return {tests, A::eDepthStencilAttachmentRead, A::eDepthStencilAttachmentWrite};
    case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
        return {tests | shaders, A::eDepthStencilAttachmentRead | A::eShaderSampledRead, {}};
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return {shaders, A::eShaderSampledRead, {}};
    case vk::ImageLayout::eTransferSrcOptimal:
        return {S::eAllTransfer, A::eTransferRead, {}};
    case vk::ImageLayout::eTransferDstOptimal:
        return {S::eAllTransfer, {}, A::eTransferWrite};
    default: // General: the presenter's, read outside this command buffer
        return {S::eAllCommands, A::eMemoryRead, A::eMemoryWrite};
    }
}

/// The image in `layout` for what follows. A layout that can be written gets a barrier even when
/// it does not change (a second pass or copy into it waits for the first).
void Transition(Image& image, vk::ImageLayout layout) {
    const LayoutUse from = UseOf(image.layout), to = UseOf(layout);
    if (image.layout == layout && !from.write && !image.discard) {
        return;
    }
    end_kind = "layout";
    end_image = &image;
    end_layout = layout;
    EndRendering();
    pending.push_back(vk::ImageMemoryBarrier2{
        .srcStageMask = from.stages,
        .srcAccessMask = from.write,
        .dstStageMask = to.stages,
        .dstAccessMask = to.read | to.write,
        .oldLayout = image.discard ? vk::ImageLayout::eUndefined : image.layout,
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
    image.discard = false;
}

/// Records the image's pending clear with a transfer command.
void FlushClear(Image& image) {
    const vk::ImageAspectFlags aspects = image.clear;
    image.clear = {};
    image.discard = aspects == Aspects(image);
    Transition(image, vk::ImageLayout::eTransferDstOptimal);
    Mark(image.depth ? Work::ClearDepth : Work::ClearColor, image.width, image.height, image.format);
    Barrier();
    ++stats.recorded_clears;
    const vk::ImageSubresourceRange range{aspects, 0, 1, 0, 1};
    if (image.depth) {
        Cmd().clearDepthStencilImage(image.image, vk::ImageLayout::eTransferDstOptimal,
                                     image.clear_value.depthStencil, range);
    } else {
        Cmd().clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal,
                              image.clear_value.color, range);
    }
}

/// Whether the open pass renders to `image` (writably) over all of it.
bool InOpenPass(const Image& image) {
    if (!rendering) {
        return false;
    }
    if (&image == open_pass.depth) {
        return !open_pass.depth_read_only;
    }
    return std::find(std::begin(open_pass.color), std::end(open_pass.color), &image) !=
           std::end(open_pass.color);
}

/// Clears `aspects` of an image of the open pass inside it, over the whole render area.
void ClearInPass(const Image& image, vk::ImageAspectFlags aspects, const vk::ClearValue& value) {
    u32 slot = 0;
    if (!image.depth) {
        slot = u32(std::find(std::begin(open_pass.color), std::end(open_pass.color), &image) -
                   std::begin(open_pass.color));
    }
    const vk::ClearAttachment at{.aspectMask = aspects, .colorAttachment = slot, .clearValue = value};
    const vk::ClearRect rect{.rect = {{0, 0}, open_area}, .baseArrayLayer = 0, .layerCount = 1};
    Cmd().clearAttachments(at, rect);
}

void Begin(Frame& f) {
    (void)f.cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    if (f.timestamps) {
        f.cmd.resetQueryPool(f.timestamps, 0, MaxMarks + 1);
        f.cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, f.timestamps, 0);
    }
    f.labels.assign(1, Label{});
    f.recorded = false;
    ++state_epoch;
    tracing = trace_passes && stats.submits % trace_every == trace_every / 2;
    f.traced = tracing;
    if (tracing) {
        std::printf("Pass trace: submission %llu (in the trace file)\n", (unsigned long long)stats.submits);
        Trace("Pass trace: submission %llu\n", (unsigned long long)stats.submits);
    }
    f.prelude_open = false;
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
    if (f.timestamps && !f.labels.empty()) {
        const std::size_t n = f.labels.size() + 1; // the last one ends the command buffer
        std::vector<u64> t(n);
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(
                device, f.timestamps, 0, u32(n), n * sizeof(u64), t.data(), sizeof(u64),
                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            t[n - 1] > t[0]) {
            stats.gpu_ns += u64(double(t[n - 1] - t[0]) * tick_ns);
            last_gpu_ns.store(u64(double(t[n - 1] - t[0]) * tick_ns), std::memory_order_relaxed);
            if (f.traced) {
                traced_ns += u64(double(t[n - 1] - t[0]) * tick_ns);
                ++traced_buffers;
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    if (f.labels[i].kind == Work::Draw && t[i + 1] >= t[i]) {
                        WorkAcc& a = draw_times[WorkName(f.labels[i])];
                        a.ns += u64(double(t[i + 1] - t[i]) * tick_ns);
                        ++a.count;
                        a.draws += f.labels[i].draws;
                    }
                }
                Trace("Pass trace GPU times (%.3f ms in all):\n", double(t[n - 1] - t[0]) * tick_ns / 1e6);
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    Trace("  %7.3f ms  %s, %u draws\n", double(t[i + 1] - t[i]) * tick_ns / 1e6,
                                WorkName(f.labels[i]).c_str(), f.labels[i].draws);
                }
            }
            for (std::size_t i = 0; i + 1 < n; ++i) {
                if (t[i + 1] < t[i] || f.labels[i].kind == Work::Draw) {
                    continue; // single draws are for the pass trace only
                }
                WorkAcc& a = work_times[WorkName(f.labels[i])];
                a.ns += u64(double(t[i + 1] - t[i]) * tick_ns);
                ++a.count;
                a.draws += f.labels[i].draws;
            }
        }
    }
    if (f.traced) {
        FlushTrace();
        f.traced = false;
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
    end_kind = "submit";
    EndRendering();
    if (!pending.empty()) {
        Barrier(); // leftover layout changes (records, so the frame is submitted)
    }
    if (!f.recorded && !f.prelude_open) {
        return;
    }
    std::array<vk::CommandBuffer, 2> cmds{f.cmd};
    u32 count = 1;
    if (f.prelude_open) {
        // the uploads become visible to vertex and index fetch of everything after them
        const vk::MemoryBarrier2 written{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput |
                            vk::PipelineStageFlagBits2::eIndexInput,
            .dstAccessMask = vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eIndexRead};
        f.prelude.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written});
        (void)f.prelude.end();
        cmds = {f.prelude, f.cmd};
        count = 2;
    }
    if (f.timestamps) {
        f.cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, f.timestamps,
                              u32(f.labels.size()));
    }
    (void)f.cmd.end();
    f.ring_end = ring_head;
    const vk::SubmitInfo submit{.commandBufferCount = count, .pCommandBuffers = cmds.data()};
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

bool Init(const Vulkan::Instance& vulkan, const std::string& cache_path) {
    instance = &vulkan;
    device = vulkan.GetDevice();
    const char* pt = std::getenv("BB_VK_PASS_TRACE");
    trace_passes = pt && pt[0] == '1';
    if (const char* every = std::getenv("BB_VK_PASS_TRACE_EVERY"); every && std::atoi(every) >= 2) {
        trace_every = u64(std::atoi(every));
    }
    if (trace_passes) {
        std::thread(TraceWriter).detach();
    }
    const char* fb = std::getenv("BB_VK_FULL_BARRIERS");
    full_barriers = fb && fb[0] == '1';
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
            {.commandPool = pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 2});
        if (ar != vk::Result::eSuccess) {
            return false;
        }
        f.cmd = cmds[0];
        f.prelude = cmds[1];
        if (tick_ns > 0) {
            auto [qr, qp] = device.createQueryPool(
                {.queryType = vk::QueryType::eTimestamp, .queryCount = MaxMarks + 1});
            f.timestamps = qr == vk::Result::eSuccess ? qp : vk::QueryPool{};
        }
    }
    uniform_range = std::min<u64>(65536, vulkan.GetPhysicalDevice().getProperties().limits.maxUniformBufferRange);
    if (const char* mb = std::getenv("BB_VK_RING_MB"); mb && std::atoi(mb) >= 16) {
        RingSize = u64(std::min(std::atoi(mb), 1024)) << 20;
    }
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = RingSize + uniform_range,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci{};
    // AUTO takes a TRANSFER_SRC buffer the CPU writes for staging and puts it in system memory;
    // PREFER_DEVICE puts it in video memory the CPU can write when it fits (else system memory).
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vulkan.GetAllocator(), &bci, &aci, &ring, &ring_allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    VkMemoryPropertyFlags ring_flags = 0;
    vmaGetAllocationMemoryProperties(vulkan.GetAllocator(), ring_allocation, &ring_flags);
    std::printf("Native Vulkan: vertex ring %llu MiB in %s memory (BB_VK_RING_MB)\n",
                static_cast<unsigned long long>(RingSize >> 20),
                ring_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "video (device-local)" : "system");
    ring_data = static_cast<std::uint8_t*>(info.pMappedData);
    {
        const VkBufferCreateInfo cbci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = 32,
                                      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo caci{};
        caci.usage = VMA_MEMORY_USAGE_AUTO;
        caci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo cinfo{};
        if (vmaCreateBuffer(vulkan.GetAllocator(), &cbci, &caci, &constant_vertices,
                            &constant_vertices_allocation, &cinfo) == VK_SUCCESS) {
            const float values[8] = {0, 0, 0, 0, 1, 1, 1, 1};
            std::memcpy(cinfo.pMappedData, values, sizeof(values));
        }
    }
    std::memset(ring_data + RingSize, 0, uniform_range);
    // a cache from an earlier run (the driver ignores data from another driver or GPU)
    cache_file = cache_path;
    std::vector<char> data;
    if (!cache_file.empty()) {
        std::ifstream in(cache_file, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto [cr, cache] = device.createPipelineCache(
        {.initialDataSize = data.size(), .pInitialData = data.empty() ? nullptr : data.data()});
    if (cr != vk::Result::eSuccess && !data.empty()) {
        std::tie(cr, cache) = device.createPipelineCache({});
    }
    pipeline_cache = cr == vk::Result::eSuccess ? cache : vk::PipelineCache{};
    std::printf("Native Vulkan: pipeline cache %s (%zu KiB from %s)\n",
                pipeline_cache ? "on" : "not made", data.size() / 1024,
                cache_file.empty() ? "nowhere" : cache_file.c_str());
    Begin(Cur());
    return true;
}

vk::CommandBuffer Cmd() {
    Cur().recorded = true;
    return Cur().cmd;
}

vk::CommandBuffer Prelude() {
    Frame& f = Cur();
    if (!f.prelude_open) {
        (void)f.prelude.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        // earlier submissions' vertex and index reads finish before the buffers are rewritten
        const vk::MemoryBarrier2 read{
            .srcStageMask = vk::PipelineStageFlagBits2::eVertexAttributeInput |
                            vk::PipelineStageFlagBits2::eIndexInput,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy};
        f.prelude.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &read});
        f.prelude_open = true;
    }
    return f.prelude;
}

VkBuffer UniformBuffer() {
    return ring;
}

VkBuffer ConstantVertices() {
    return constant_vertices;
}

u64 UniformRange() {
    return uniform_range;
}

u64 ZeroUniform() {
    return RingSize;
}

u64 Serial() {
    return stats.submits;
}

u64 StateEpoch() {
    return state_epoch;
}

void Invalidate() {
    ++state_epoch;
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
    if (image.clear) {
        FlushClear(image);
    }
    Transition(image, layout);
}

void Barrier() {
    const vk::MemoryBarrier2 all{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    if (pending.empty() && !full_barriers) {
        return;
    }
    Cmd().pipelineBarrier2({.memoryBarrierCount = full_barriers ? 1u : 0u,
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
    end_kind = "next pass";
    EndRendering();
    u32 w = ~0u, h = ~0u, count = 0;
    for (const Image* c : pass.color) {
        if (c) {
            w = std::min(w, c->width);
            h = std::min(h, c->height);
        }
    }
    if (pass.depth) {
        w = std::min(w, pass.depth->width);
        h = std::min(h, pass.depth->height);
    }
    if (w == ~0u) {
        return;
    }
    // pending clears: the load op when the pass covers the image, else recorded first
    auto load_clear = [&](Image& image, bool writable) {
        if (image.clear && (!writable || image.width != w || image.height != h)) {
            FlushClear(image);
        }
        const vk::ImageAspectFlags cleared = image.clear;
        image.clear = {};
        image.discard = cleared && cleared == Aspects(image);
        stats.load_clears += bool(cleared);
        return cleared;
    };
    std::array<vk::RenderingAttachmentInfo, 8> colors{};
    for (u32 i = 0; i < 8; ++i) {
        if (Image* c = pass.color[i]) {
            const bool clear = bool(load_clear(*c, true));
            Transition(*c, vk::ImageLayout::eColorAttachmentOptimal);
            colors[i] = {.imageView = c->attach,
                         .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                         .loadOp = clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
                         .storeOp = vk::AttachmentStoreOp::eStore,
                         .clearValue = c->clear_value};
            count = i + 1;
        }
    }
    vk::RenderingAttachmentInfo depth{}, stencil{};
    if (Image* d = pass.depth) {
        const vk::ImageLayout l = pass.depth_read_only ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                       : vk::ImageLayout::eDepthStencilAttachmentOptimal;
        const vk::ImageAspectFlags cleared = load_clear(*d, !pass.depth_read_only);
        Transition(*d, l);
        const auto op = [&](vk::ImageAspectFlagBits aspect) {
            return cleared & aspect ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
        };
        depth = {.imageView = d->attach,
                 .imageLayout = l,
                 .loadOp = op(vk::ImageAspectFlagBits::eDepth),
                 .storeOp = pass.depth_read_only ? vk::AttachmentStoreOp::eNone
                                                 : vk::AttachmentStoreOp::eStore,
                 .clearValue = d->clear_value};
        if (d->stencil) {
            stencil = depth;
            stencil.loadOp = op(vk::ImageAspectFlagBits::eStencil);
        }
    }
    {
        const Image* first = pass.depth;
        for (const Image* c : pass.color) {
            if (c) {
                first = c;
                break;
            }
        }
        Mark(Work::Pass, first->width, first->height, first->format);
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
    open_area = {w, h};
    ++stats.passes;
    if (tracing) {
        std::string a;
        for (u32 i = 0; i < count; ++i) {
            a += " c" + std::to_string(i) + "=" + Describe(pass.color[i]) +
                 (colors[i].loadOp == vk::AttachmentLoadOp::eClear ? " (clear)" : "");
        }
        if (pass.depth) {
            a += " d=" + Describe(pass.depth) + (pass.depth_read_only ? " (read-only)" : "") +
                 (depth.loadOp == vk::AttachmentLoadOp::eClear ? " (clear)" : "");
        }
        Trace("  pass%s\n", a.c_str());
        trace_draws = 0;
    }
}

void EndRendering() {
    if (rendering) {
        Cur().cmd.endRendering();
        rendering = false;
        if (tracing) {
            Trace("    %u draws; ended by %s%s%s%s\n", trace_draws, end_kind,
                        end_image ? ": " : "", end_image ? Describe(end_image).c_str() : "",
                        end_image ? (" -> " + vk::to_string(end_layout)).c_str() : "");
        }
    }
    end_kind = "renderer";
    end_image = nullptr;
}

bool Rendering() {
    return rendering;
}

void ClearColor(Image& image, const float color[4]) {
    vk::ClearColorValue value{};
    std::copy(color, color + 4, value.float32.begin());
    if (InOpenPass(image) && image.width == open_area.width && image.height == open_area.height) {
        ClearInPass(image, vk::ImageAspectFlagBits::eColor, vk::ClearValue{.color = value});
        ++stats.pass_clears;
        return;
    }
    if (InOpenPass(image)) {
        end_kind = "partial color clear";
        EndRendering(); // a pass over part of it: the clear waits for the next use
    }
    image.clear = vk::ImageAspectFlagBits::eColor;
    image.clear_value.color = value;
}

void ClearDepthStencil(Image& image, bool depth, float z, bool stencil, u32 s) {
    stencil = stencil && image.stencil;
    if (!depth && !stencil) {
        return;
    }
    vk::ImageAspectFlags aspects{};
    if (depth) {
        aspects |= vk::ImageAspectFlagBits::eDepth;
    }
    if (stencil) {
        aspects |= vk::ImageAspectFlagBits::eStencil;
    }
    if (InOpenPass(image) && image.width == open_area.width && image.height == open_area.height) {
        ClearInPass(image, aspects, vk::ClearValue{.depthStencil = {z, s}});
        ++stats.pass_clears;
        return;
    }
    if (rendering && &image == open_pass.depth) {
        end_kind = "partial depth clear";
        EndRendering();
    }
    // a second clear before the image is used adds its aspects to the first
    vk::ClearDepthStencilValue& v = image.clear_value.depthStencil;
    if (!(image.clear & vk::ImageAspectFlagBits::eDepth) || depth) {
        v.depth = depth ? z : v.depth;
    }
    if (stencil) {
        v.stencil = s;
    }
    image.clear |= aspects;
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
            static constexpr std::array<vk::DescriptorPoolSize, 8> sizes{{
                {vk::DescriptorType::eUniformBuffer, SetsPerPool * 6},
                {vk::DescriptorType::eUniformBufferDynamic, SetsPerPool * 4},
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

void Mark(Work kind, u32 width, u32 height, vk::Format format, const char* note) {
    Frame& f = Cur();
    if (!f.timestamps || f.labels.size() >= MaxMarks) {
        return;
    }
    // after everything before it: the pieces are serial anyway (full barriers)
    Cmd().writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, f.timestamps,
                          u32(f.labels.size()));
    f.labels.push_back({kind, width, height, format, 0, note});
}

void NoteDraw() {
    ++trace_draws;
    if (!Cur().labels.empty()) {
        ++Cur().labels.back().draws;
    }
}

std::vector<WorkTime> DrawTimes(std::uint64_t& all_ns, std::uint64_t& buffers) {
    all_ns = traced_ns;
    buffers = traced_buffers;
    std::vector<WorkTime> out;
    for (const auto& [what, a] : draw_times) {
        out.push_back({what, a.ns, a.count, a.draws});
    }
    std::sort(out.begin(), out.end(), [](const WorkTime& a, const WorkTime& b) { return a.ns > b.ns; });
    return out;
}

std::uint64_t LastGpuNs() {
    return last_gpu_ns.load(std::memory_order_relaxed);
}

std::vector<WorkTime> TakeWorkTimes() {
    std::vector<WorkTime> out;
    for (const auto& [what, a] : work_times) {
        out.push_back({what, a.ns, a.count, a.draws});
    }
    work_times.clear();
    std::sort(out.begin(), out.end(), [](const WorkTime& a, const WorkTime& b) { return a.ns > b.ns; });
    return out;
}

void SaveCache() {
    if (!pipeline_cache || cache_file.empty()) {
        return;
    }
    auto [r, data] = device.getPipelineCacheData(pipeline_cache);
    if (r != vk::Result::eSuccess || data.empty()) {
        return;
    }
    const std::string tmp = cache_file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, cache_file, ec);
}

vk::PipelineCache PipelineCache() {
    return pipeline_cache;
}

Stats GetStats() {
    return stats;
}
} // namespace BbNativeVk::Gpu
