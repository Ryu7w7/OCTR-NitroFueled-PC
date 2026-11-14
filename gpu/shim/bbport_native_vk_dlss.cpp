// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: DLSS for the native Vulkan renderer (see bbport_native_vk_dlss.h).
#include "bbport_native_vk_dlss.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "video_core/host_shaders/ctr_fsr3_inputs_comp.h"
#include "video_core/renderer_vulkan/vk_dlss_ngx.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace BbNativeVk::Dlss {
namespace {
using Vulkan::DlssNgx;

const Vulkan::Instance* inst = nullptr;
std::atomic<bool> failed{false};
bool warned = false;

vk::UniqueDescriptorSetLayout set_layout;
vk::UniquePipelineLayout pipeline_layout;
vk::UniquePipeline inputs_pipeline;

/// The inputs DLSS reads: the depth as R32F, the motion in pixels as RG16F, and the shader's
/// debug image (unused here, the shader writes it only when asked).
struct Work {
    u32 width = 0, height = 0;
    VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE, debug = VK_NULL_HANDLE;
    VmaAllocation depth_mem = nullptr, motion_mem = nullptr, debug_mem = nullptr;
    VkImageView depth_view = VK_NULL_HANDLE, motion_view = VK_NULL_HANDLE,
                debug_view = VK_NULL_HANDLE;
    bool fresh = true; // still in Undefined layout
};
Work work;
std::chrono::steady_clock::time_point last_frame{};
u32 jitter_index = 0;

float Halton(u32 index, u32 base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

bool MakeImage(vk::Format format, u32 w, u32 h, VkImage& image, VmaAllocation& mem,
               VkImageView& view) {
    const vk::ImageCreateInfo ici{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    const VkImageCreateInfo& raw = ici;
    if (vmaCreateImage(inst->GetAllocator(), &raw, &aci, &image, &mem, nullptr) != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        return false;
    }
    auto [r, v] = inst->GetDevice().createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    view = r == vk::Result::eSuccess ? VkImageView(v) : VK_NULL_HANDLE;
    return view != VK_NULL_HANDLE;
}

void DropWork() {
    Work old = work;
    work = {};
    if (!old.depth && !old.motion && !old.debug) {
        return;
    }
    Gpu::Defer([old, dev = inst->GetDevice(), vma = inst->GetAllocator()] {
        for (const VkImageView v : {old.depth_view, old.motion_view, old.debug_view}) {
            if (v) {
                dev.destroyImageView(v);
            }
        }
        if (old.depth) vmaDestroyImage(vma, old.depth, old.depth_mem);
        if (old.motion) vmaDestroyImage(vma, old.motion, old.motion_mem);
        if (old.debug) vmaDestroyImage(vma, old.debug, old.debug_mem);
    });
}

bool EnsureWork(u32 w, u32 h) {
    if (work.width == w && work.height == h && work.depth) {
        return true;
    }
    DropWork();
    work.width = w;
    work.height = h;
    if (!MakeImage(vk::Format::eR32Sfloat, w, h, work.depth, work.depth_mem, work.depth_view) ||
        !MakeImage(vk::Format::eR16G16Sfloat, w, h, work.motion, work.motion_mem,
                   work.motion_view) ||
        !MakeImage(vk::Format::eR8G8B8A8Unorm, w, h, work.debug, work.debug_mem,
                   work.debug_view)) {
        std::printf("Native Vulkan: DLSS: its %ux%u working images could not be made\n", w, h);
        DropWork();
        return false;
    }
    return true;
}

bool EnsurePipeline() {
    if (inputs_pipeline) {
        return true;
    }
    const vk::Device device = inst->GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 5> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    auto [r1, layout] = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    });
    if (r1 != vk::Result::eSuccess) {
        return false;
    }
    set_layout = std::move(layout);
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 128};
    auto [r2, pl] = device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    });
    if (r2 != vk::Result::eSuccess) {
        return false;
    }
    pipeline_layout = std::move(pl);
    const vk::ShaderModule module =
        Vulkan::CompileSPV(std::span<const u32>(CTR_FSR3_INPUTS_COMP), device);
    if (!module) {
        return false;
    }
    auto [r3, pipeline] = device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            });
    device.destroyShaderModule(module);
    if (r3 != vk::Result::eSuccess) {
        return false;
    }
    inputs_pipeline = std::move(pipeline);
    return true;
}

vk::ImageMemoryBarrier2 Transition(VkImage image, vk::ImageLayout from, vk::ImageLayout to) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
}

void Fail(const char* why) {
    failed.store(true, std::memory_order_relaxed);
    std::printf("Native Vulkan: DLSS: %s; off for this session\n", why);
    std::fflush(stdout);
}
} // namespace

void Init(const Vulkan::Instance& instance) {
    inst = &instance;
}

bool Wanted() {
    static const bool env = [] {
        const char* e = std::getenv("BB_VK_DLSS");
        return e && e[0] == '1';
    }();
    if (failed.load(std::memory_order_relaxed) || !inst) {
        return false;
    }
    return env || BbSettings::Get().temporal.load(std::memory_order_relaxed) ==
                      BbSettings::TemporalDlss;
}

std::array<float, 2> NextJitter() {
    jitter_index = jitter_index % 8 + 1;
    if (std::getenv("BB_FSR3_NO_JITTER")) {
        return {};
    }
    return {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
}

bool Upscale(const Frame& f) {
    if (!Wanted() || !f.color || !f.depth || !f.velocity || !f.output) {
        return false;
    }
    DlssNgx* dlss = DlssNgx::Get();
    if (!dlss || !dlss->IsAvailable()) {
        if (!warned) {
            warned = true;
            std::printf("Native Vulkan: DLSS: not available (%s)\n",
                        dlss ? dlss->Problem().c_str()
                             : "sacredfire_dlss.dll or nvngx_dlss.dll is not next to the game");
            std::fflush(stdout);
        }
        failed.store(true, std::memory_order_relaxed); // no jitter left on the scene either
        return false;
    }
    const u32 w = f.color->width, h = f.color->height;
    const u32 ow = f.output->width, oh = f.output->height;
    if (f.depth->width != w || f.depth->height != h || f.velocity->width != w ||
        f.velocity->height != h || ow < w || oh < h) {
        static u32 logged = 0;
        if (logged++ < 4) {
            std::printf("Native Vulkan: DLSS: not used: scene %ux%u, depth %ux%u, velocity %ux%u, "
                        "output %ux%u\n",
                        w, h, f.depth->width, f.depth->height, f.velocity->width,
                        f.velocity->height, ow, oh);
        }
        return false;
    }
    if (!EnsurePipeline()) {
        Fail("its input shader could not be made");
        return false;
    }
    if (!EnsureWork(w, h)) {
        return false;
    }

    static const char* preset_env = std::getenv("BB_DLSS_PRESET");
    const u32 preset = preset_env ? u32(std::atoi(preset_env))
                       : BbSettings::Get().dlss_model == 1 ? 5u : 0u;
    const DlssNgx::FeatureDesc desc{w,      h,     ow,    oh, DlssNgx::QualityForScale(float(ow) / float(w)),
                                    false, preset, f.hdr};
    bool reset = f.reset;
    if (!dlss->HasFeature(desc)) {
        // a feature made before goes first: the GPU must be done with it
        Gpu::EndRendering();
        Gpu::Submit();
        Gpu::WaitDone(Gpu::Submitted(), 2'000'000'000ull);
        if (!dlss->CreateFeature(Gpu::Cmd(), desc)) {
            Fail("the feature could not be created");
            return false;
        }
        Gpu::Invalidate();
        reset = true;
        std::printf("Native Vulkan: DLSS: %ux%u -> %ux%u, quality %d, preset %u%s\n", w, h, ow,
                    oh, desc.quality, preset, f.hdr ? ", HDR (linear colour)" : "");
        std::fflush(stdout);
    }

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = 16.6f;
    if (last_frame.time_since_epoch().count() != 0) {
        frame_ms = std::clamp(std::chrono::duration<float, std::milli>(now - last_frame).count(),
                              1.0f, 100.0f);
        reset |= frame_ms >= 100.0f; // a pause, a load
    }
    last_frame = now;

    // the game's images for sampling, then the inputs
    Gpu::Use(*f.color, vk::ImageLayout::eShaderReadOnlyOptimal);
    Gpu::Use(*f.depth, vk::ImageLayout::eShaderReadOnlyOptimal);
    Gpu::Use(*f.velocity, vk::ImageLayout::eShaderReadOnlyOptimal);
    Gpu::Use(*f.output, vk::ImageLayout::eGeneral);
    Gpu::Barrier();
    const vk::CommandBuffer cmd = Gpu::Cmd();
    const auto general = vk::ImageLayout::eGeneral, ro = vk::ImageLayout::eShaderReadOnlyOptimal;
    {
        const vk::ImageLayout from = work.fresh ? vk::ImageLayout::eUndefined : general;
        const std::array to_write{Transition(work.depth, from, general),
                                  Transition(work.motion, from, general),
                                  Transition(work.debug, from, general)};
        work.fresh = false;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_write.size()),
                              .pImageMemoryBarriers = to_write.data()});
    }
    {
        const vk::DescriptorImageInfo infos[5] = {
            {{}, f.depth_view, ro},
            {{}, f.velocity_view, ro},
            {{}, work.depth_view, general},
            {{}, work.motion_view, general},
            {{}, work.debug_view, general},
        };
        std::array<vk::WriteDescriptorSet, 5> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i < 2 ? vk::DescriptorType::eSampledImage
                                                 : vk::DescriptorType::eStorageImage,
                         .pImageInfo = &infos[i]};
        }
        struct {
            float prev[3][4];
            float proj[4];
            float viewport[4];
            float depth_jitter[4];
            float object[4];
            u32 flags;
        } params{};
        if (f.camera) {
            constexpr int rows[3] = {0, 1, 3};
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 4; ++col) {
                    params.prev[r][col] = f.reproject[col * 4 + rows[r]];
                }
            }
            params.proj[0] = f.projection[0];
            params.proj[1] = f.projection[5];
            params.proj[2] = f.projection[10];
            params.proj[3] = f.projection[14];
        }
        params.viewport[0] = f.xscale;
        params.viewport[1] = f.yscale;
        params.viewport[2] = f.xoffset;
        params.viewport[3] = f.yoffset;
        params.depth_jitter[0] = f.zscale;
        params.depth_jitter[1] = f.zoffset;
        params.depth_jitter[2] = f.jitter[0];
        params.depth_jitter[3] = f.jitter[1];
        // moving objects stay still on the screen (vk_ctr_fsr3's default, measured best);
        // BB_FSR3_MV="sx,sy,bias,mode,cx,cy" adds their own motion, as under shadPS4
        static const auto decode = [] {
            struct {
                float sx = 0, sy = 0, bias = 127.0f / 255.0f;
                u32 mode = 0, cx = 1, cy = 0;
            } d;
            if (const char* env = std::getenv("BB_FSR3_MV")) {
                std::sscanf(env, "%f,%f,%f,%u,%u,%u", &d.sx, &d.sy, &d.bias, &d.mode, &d.cx, &d.cy);
                d.cx &= 3;
                d.cy &= 3;
                d.mode = std::min(d.mode, 2u);
            }
            std::printf("Native Vulkan: DLSS: moving objects %s (channels %u/%u - %.4f) * %g/%g px; "
                        "jitter %s\n",
                        d.mode == 0 ? "still" : d.mode == 1 ? "linear" : "squared", d.cx, d.cy,
                        d.bias, d.sx, d.sy, std::getenv("BB_FSR3_NO_JITTER") ? "off" : "on");
            return d;
        }();
        params.object[0] = decode.sx;
        params.object[1] = decode.sy;
        params.object[2] = decode.bias;
        params.object[3] = float(decode.mode);
        const bool camera = f.camera && params.proj[0] != 0.0f && params.proj[1] != 0.0f;
        params.flags = decode.cx | (decode.cy << 2) | (camera ? 0x200u : 0u);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *inputs_pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
        cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                          &params);
        cmd.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }
    const std::array to_read{Transition(work.depth, general, ro), Transition(work.motion, general, ro)};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_read.size()),
                          .pImageMemoryBarriers = to_read.data()});

    constexpr vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const DlssNgx::Resource color{f.color->image, f.color_view, color_range, f.color->format,
                                  {w, h}};
    const DlssNgx::Resource depth{work.depth, work.depth_view, color_range,
                                  vk::Format::eR32Sfloat, {w, h}};
    const DlssNgx::Resource motion{work.motion, work.motion_view, color_range,
                                   vk::Format::eR16G16Sfloat, {w, h}};
    const DlssNgx::Resource output{f.output->image, f.output_view, color_range, f.output->format,
                                   {ow, oh}};
    const float sign = std::getenv("BB_DLSS_JITTER_FLIP") ? -1.0f : 1.0f;
    const DlssNgx::EvalDesc eval{sign * f.jitter[0], sign * f.jitter[1], reset, frame_ms};
    const bool ok = dlss->Evaluate(cmd, color, depth, motion, output, eval);
    Gpu::Invalidate(); // NGX records its own Vulkan state
    const std::array back{Transition(work.depth, ro, general), Transition(work.motion, ro, general),
                          Transition(f.output->image, general, general)};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = u32(back.size()),
                          .pImageMemoryBarriers = back.data()});
    if (!ok) {
        Fail("its evaluation failed");
        return false;
    }
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::printf("Native Vulkan: DLSS: upscaling the scene before the HUD (%ux%u -> %ux%u, "
                    "camera %s)\n",
                    w, h, ow, oh, f.camera ? "found" : "missing");
        std::fflush(stdout);
    }
    return true;
}

} // namespace BbNativeVk::Dlss
