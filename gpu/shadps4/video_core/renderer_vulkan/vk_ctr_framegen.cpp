// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_ctr_framegen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "bbport_settings.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/ctr_framegen_compose_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {

/// The interpolated frame: a storage image (FSR 3 writes it) the presenter blits to the
/// swapchain like the real frame. 8-bit unorm keeps it storable on every GPU; the blit converts
/// to the swapchain's channel order.
constexpr vk::Format GeneratedFormat = vk::Format::eR8G8B8A8Unorm;

/// BB_FRAMEGEN=1/0 overrides the setting.
int EnvOverride() {
    static const int value = [] {
        const char* env = std::getenv("BB_FRAMEGEN");
        return env && env[0] ? (env[0] == '0' ? 0 : 1) : -1;
    }();
    return value;
}

/// BB_FRAMEGEN_HUDLESS=0: interpolate the frame with its HUD (no HUD-less frame); the HUD then
/// moves with the scene in the interpolated frame. A test for the HUD detection.
bool UseHudless() {
    static const bool value = [] {
        const char* env = std::getenv("BB_FRAMEGEN_HUDLESS");
        return !(env && env[0] == '0');
    }();
    return value;
}

/// BB_FRAMEGEN_MASK: 0 interpolates the moving objects too (FSR 3 alone, ghosting on the karts);
/// 2 shows where the real frame is used (magenta). Otherwise the karts come from the real frame.
int MaskMode() {
    static const int value = [] {
        const char* env = std::getenv("BB_FRAMEGEN_MASK");
        return env && env[0] ? std::atoi(env) : 1;
    }();
    return value;
}

/// BB_FRAMEGEN_MASK_RADIUS: how far around the moving objects (render pixels) the real frame is
/// used, for their edges and the wheels' outline (default 2).
float MaskRadius() {
    static const float value = [] {
        const char* env = std::getenv("BB_FRAMEGEN_MASK_RADIUS");
        return env && env[0] ? std::clamp(float(std::atof(env)), 0.0f, 8.0f) : 3.0f;
    }();
    return value;
}

/// BB_FRAMEGEN_MASK_MOTION: how far along each pixel's motion (in frames of motion) the karts
/// are looked for, for the background next to them that FSR 3 cannot place (default 0.6: the
/// interpolated frame is half a frame from each real one).
float MotionReach() {
    static const float value = [] {
        const char* env = std::getenv("BB_FRAMEGEN_MASK_MOTION");
        return env && env[0] ? std::clamp(float(std::atof(env)), 0.0f, 4.0f) : 0.6f;
    }();
    return value;
}

FfxVkPortableImage Describe(vk::Image image, vk::Format format, vk::Extent2D extent,
                            vk::ImageUsageFlags usage, FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {extent.width, extent.height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    out.state = state;
    return out;
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("Frame generation: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

FfxVkPortableFloat3 Float3(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

vk::ImageMemoryBarrier2 Transition(vk::Image image, vk::ImageLayout from, vk::ImageLayout to) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

} // namespace

CtrFrameGen::CtrFrameGen(const Instance& instance_, u32 frame_count)
    : instance{instance_}, slots(frame_count) {}

CtrFrameGen::~CtrFrameGen() {
    // The presenter waits for its schedulers before it is destroyed.
    for (auto& [old, when] : retired) {
        ffxVkPortableFrameGenerationContextDestroy(old);
    }
    if (context) {
        ffxVkPortableFrameGenerationContextDestroy(context);
    }
}

bool CtrFrameGen::Enabled() const {
    return !failed && Wanted();
}

bool CtrFrameGen::Wanted() {
    const int env = EnvOverride();
    return env >= 0 ? env == 1 : BbSettings::Get().frame_generation.load(std::memory_order_relaxed);
}

void CtrFrameGen::Fail(const char* what) {
    failed = true;
    std::printf("Frame generation: %s; off for this session\n", what);
    std::fflush(stdout);
}

void CtrFrameGen::RetireOld() {
    std::erase_if(retired, [this](const auto& entry) {
        if (records < entry.second) {
            return false;
        }
        ffxVkPortableFrameGenerationContextDestroy(entry.first);
        return true;
    });
}

bool CtrFrameGen::EnsureContext(vk::Extent2D render, vk::Extent2D display, vk::Format format) {
    if (context && context_render == render && context_display == display &&
        context_format == format) {
        return true;
    }
    // Commands of earlier frames may still use the old context: it goes once every presentation
    // frame has come round again (each waits for its last use before it is recorded).
    if (context) {
        retired.emplace_back(context, records + slots.size() + 2);
        context = nullptr;
    }
    context_render = render;
    context_display = display;
    context_format = format;
    broken = true;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = instance.GetDevice();
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableFrameGenerationCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.flags = 0; // CTR's depth: not inverted, finite far plane; motion without jitter
    create_info.maxRenderSize = {render.width, render.height};
    create_info.displaySize = {display.width, display.height};
    create_info.interpolationSourceFormat = static_cast<VkFormat>(format);
    create_info.outputFormat = static_cast<VkFormat>(GeneratedFormat);
    if (const u64 issues = ffxVkPortableValidateFrameGenerationCreateInfo(&create_info)) {
        PrintIssues("create info", issues);
        return false;
    }
    const FfxVkPortableResult result =
        ffxVkPortableFrameGenerationContextCreate(&device_info, &create_info, &context);
    if (result != FFX_VK_PORTABLE_OK) {
        std::printf("Frame generation: context creation failed (%d)\n", int(result));
        context = nullptr;
        return false;
    }
    std::printf("Frame generation: render %ux%u, display %ux%u (%s)\n", render.width,
                render.height, display.width, display.height, vk::to_string(format).c_str());
    std::fflush(stdout);
    return true;
}

bool CtrFrameGen::EnsureSlot(Slot& slot, u32 width, u32 height, vk::Format format) {
    if (slot.hudless && slot.width == width && slot.height == height && slot.format == format) {
        return true;
    }
    // This slot's earlier frame is finished: the presenter waited for its fence before it gave
    // the slot out again.
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    slot.hudless_view.reset();
    slot.generated_view.reset();
    slot.hudless = VideoCore::UniqueImage(device, allocator);
    slot.generated = VideoCore::UniqueImage(device, allocator);
    slot.width = width;
    slot.height = height;
    slot.format = format;
    const auto create = [&](VideoCore::UniqueImage& image, vk::Format image_format,
                            vk::ImageUsageFlags usage) {
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = image_format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
    };
    // The HUD-less frame: drawn by the post-processing pass like the frame, read by FSR 3.
    create(slot.hudless, format,
           vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferSrc);
    create(slot.generated, GeneratedFormat,
           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eTransferSrc);
    if (!slot.hudless || !slot.generated) {
        return false;
    }
    slot.hudless_view = Check(device.createImageViewUnique({
        .image = vk::Image(slot.hudless),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = ColorRange,
    }));
    broken = true;
    return true;
}

void CtrFrameGen::ComposeMovingObjects(vk::CommandBuffer cmdbuf, Frame& frame, Slot& slot,
                                       const CtrFsr3::FrameGenInputs& in) {
    const auto device = instance.GetDevice();
    if (!compose_pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 5> bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {4, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        compose_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 32};
        compose_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*compose_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
        const auto module = CompileSPV(CTR_FRAMEGEN_COMPOSE_COMP, device);
        compose_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *compose_layout,
                }));
        device.destroyShaderModule(module);
    }
    if (!slot.generated_view) {
        slot.generated_view = Check(device.createImageViewUnique({
            .image = vk::Image(slot.generated),
            .viewType = vk::ImageViewType::e2D,
            .format = GeneratedFormat,
            .subresourceRange = ColorRange,
        }));
    }
    // All in General: the real frame after its post-processing, the masks after the upscaler's
    // inputs pass, the interpolated frame after FSR 3.
    const vk::DescriptorImageInfo infos[5] = {
        {{}, frame.image_view, vk::ImageLayout::eGeneral},
        {{}, in.mask_view, vk::ImageLayout::eGeneral},
        {{}, in.previous_mask_view, vk::ImageLayout::eGeneral},
        {{}, *slot.generated_view, vk::ImageLayout::eGeneral},
        {{}, in.motion_view, vk::ImageLayout::eGeneral},
    };
    std::array<vk::WriteDescriptorSet, 5> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = i == 3 ? vk::DescriptorType::eStorageImage
                                              : vk::DescriptorType::eSampledImage,
                     .pImageInfo = &infos[i]};
    }
    struct {
        s32 display[2];
        s32 render[2];
        float radius;
        u32 flags;
        float motion_reach;
    } params{{s32(frame.width), s32(frame.height)},
             {s32(in.render.width), s32(in.render.height)},
             MaskRadius(),
             MaskMode() == 2 ? 1u : 0u,
             MotionReach()};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *compose_pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *compose_layout, 0, writes);
    cmdbuf.pushConstants(*compose_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                         &params);
    cmdbuf.dispatch((frame.width + 7) / 8, (frame.height + 7) / 8, 1);
}

vk::Image CtrFrameGen::Generated(const Frame& frame) const {
    return frame.id < slots.size() ? vk::Image(slots[frame.id].generated) : vk::Image{};
}

bool CtrFrameGen::Record(vk::CommandBuffer cmdbuf, Frame& frame, vk::Format frame_format,
                         vk::Format view_format, const CtrFsr3::FrameGenInputs& in,
                         HostPasses::PostProcessingPass& pp,
                         HostPasses::PostProcessingPass::Settings pp_settings) {
    ++records;
    RetireOld();
    if (frame.id >= slots.size() || !in.depth || !in.motion || in.render.width == 0 ||
        in.render.height == 0) {
        broken = true;
        return false;
    }
    // Two frames in a row that the upscaler did not publish in sequence: start again.
    const bool reset = broken || in.reset || in.sequence != last_sequence + 1;
    last_sequence = in.sequence;
    broken = false;

    const vk::Extent2D display{frame.width, frame.height};
    auto& slot = slots[frame.id];
    if (!EnsureSlot(slot, frame.width, frame.height, frame_format)) {
        Fail("the frame images could not be created");
        return false;
    }
    if (!EnsureContext(in.render, display, frame_format)) {
        Fail("FSR 3 frame interpolation could not start");
        return false;
    }

    // 1. The HUD-less frame: the display buffer as the game's stretch left it before the HUD
    //    (CtrFsr3::CaptureHudless), through the same post-processing pass as the real frame, so
    //    the two are equal outside the HUD (FSR 3 takes the pixels that differ as HUD). Without
    //    that copy (no HUD draw) the frame is interpolated with its HUD.
    const bool use_hudless = UseHudless() && in.display_hudless;
    if (use_hudless) {
        // By id, not by handle: a new copy can come back with a destroyed one's handle.
        if (!slot.source_view || slot.source_id != in.display_hudless_id ||
            slot.source != in.display_hudless || slot.source_format != view_format) {
            slot.source = in.display_hudless;
            slot.source_id = in.display_hudless_id;
            slot.source_format = view_format;
            slot.source_view = Check(instance.GetDevice().createImageViewUnique({
                .image = in.display_hudless,
                .viewType = vk::ImageViewType::e2D,
                .format = view_format,
                .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                               vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
                .subresourceRange = ColorRange,
            }));
        }
        const std::array before{
            Transition(in.display_hudless, vk::ImageLayout::eGeneral,
                       vk::ImageLayout::eShaderReadOnlyOptimal),
            Transition(vk::Image(slot.hudless), vk::ImageLayout::eUndefined,
                       vk::ImageLayout::eColorAttachmentOptimal),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(before.size()),
                                 .pImageMemoryBarriers = before.data()});
        Frame target{};
        target.width = frame.width;
        target.height = frame.height;
        target.image = vk::Image(slot.hudless);
        target.image_view = *slot.hudless_view;
        target.id = frame.id;
        pp.Render(cmdbuf, *slot.source_view, in.display_hudless_size, target, pp_settings);
        // The pass leaves its target in General; the copy goes back there too.
        const std::array after{
            Transition(in.display_hudless, vk::ImageLayout::eShaderReadOnlyOptimal,
                       vk::ImageLayout::eGeneral),
            Transition(vk::Image(slot.hudless), vk::ImageLayout::eGeneral,
                       vk::ImageLayout::eGeneral),
            Transition(vk::Image(slot.generated), vk::ImageLayout::eUndefined,
                       vk::ImageLayout::eGeneral),
            Transition(frame.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral),
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(after.size()),
                                 .pImageMemoryBarriers = after.data()});
    }

    // 2. Optical flow and the interpolation's own preparation from this frame's inputs.
    const auto frame_usage = vk::ImageUsageFlagBits::eColorAttachment |
                             vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;
    const auto input_usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                             vk::ImageUsageFlagBits::eTransferSrc;
    const FfxVkPortableImage current = Describe(frame.image, frame_format, display,
                                                frame_usage | vk::ImageUsageFlagBits::eTransferDst,
                                                FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    FfxVkPortableImage hudless{};
    hudless.structSize = sizeof(hudless);
    if (use_hudless) {
        hudless = Describe(vk::Image(slot.hudless), frame_format, display, frame_usage,
                           FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    } else {
        const vk::ImageMemoryBarrier2 generated_general = Transition(
            vk::Image(slot.generated), vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral);
        cmdbuf.pipelineBarrier2(
            {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &generated_general});
    }
    // Optical flow and the interpolation read the same frame: the HUD-less one when there is one.
    const FfxVkPortableImage& source = use_hudless ? hudless : current;
    const float frame_ms = std::clamp(in.frame_ms, 1.0f, 100.0f);
    constexpr float min_luminance = 0.0001f, max_luminance = 1000.0f;
    constexpr float meters = 0.01f; // as the upscaler: CTR's units are centimetres

    FfxVkPortableFrameGenerationCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {context_render.width, context_render.height};
    create_info.displaySize = {display.width, display.height};
    create_info.interpolationSourceFormat = static_cast<VkFormat>(frame_format);
    create_info.outputFormat = static_cast<VkFormat>(GeneratedFormat);

    FfxVkPortableFrameGenerationPrepareInfo prepare{};
    prepare.structSize = sizeof(prepare);
    prepare.commandBuffer = cmdbuf;
    prepare.depth = Describe(in.depth, vk::Format::eR32Sfloat, in.render, input_usage,
                             FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.motionVectors = Describe(in.motion, vk::Format::eR16G16Sfloat, in.render, input_usage,
                                     FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    prepare.renderSize = {in.render.width, in.render.height};
    prepare.jitterOffset = {in.jitter[0], in.jitter[1]};
    prepare.motionVectorScale = {1.0f, 1.0f}; // pixels, previous minus current (as the upscaler)
    prepare.frameTimeMilliseconds = frame_ms;
    prepare.cameraNear = in.near_plane;
    prepare.cameraFar = in.far_plane;
    prepare.cameraVerticalFovRadians = in.fov;
    prepare.viewSpaceToMeters = meters;
    prepare.minLuminance = min_luminance;
    prepare.maxLuminance = max_luminance;
    prepare.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    prepare.cameraPosition = Float3(in.position);
    prepare.cameraUp = Float3(in.up);
    prepare.cameraRight = Float3(in.right);
    prepare.cameraForward = Float3(in.forward);
    prepare.reset = reset ? VK_TRUE : VK_FALSE;
    prepare.frameId = ++frame_id;
    if (const u64 issues = ffxVkPortableValidateFrameGenerationPrepareInfo(&create_info, &prepare)) {
        PrintIssues("prepare", issues);
        broken = true;
        return false;
    }
    if (ffxVkPortableFrameGenerationContextPrepare(context, &prepare, &source) !=
        FFX_VK_PORTABLE_OK) {
        Fail("the interpolation could not be prepared");
        return false;
    }

    // 3. The frame between the previous real frame and this one, HUD from the real frame.
    FfxVkPortableFrameGenerationDispatchInfo dispatch{};
    dispatch.structSize = sizeof(dispatch);
    dispatch.commandBuffer = cmdbuf;
    dispatch.currentColor = current;
    dispatch.hudlessColor = hudless;
    dispatch.distortionField.structSize = sizeof(dispatch.distortionField);
    dispatch.output = Describe(vk::Image(slot.generated), GeneratedFormat, display, input_usage,
                               FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.displaySize = {display.width, display.height};
    dispatch.interpolationRect = {0, 0, display.width, display.height}; // the whole frame
    dispatch.frameTimeMilliseconds = frame_ms;
    dispatch.cameraNear = in.near_plane;
    dispatch.cameraFar = in.far_plane;
    dispatch.cameraVerticalFovRadians = in.fov;
    dispatch.viewSpaceToMeters = meters;
    dispatch.minLuminance = min_luminance;
    dispatch.maxLuminance = max_luminance;
    dispatch.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    dispatch.reset = prepare.reset;
    dispatch.frameId = prepare.frameId;
    if (const u64 issues =
            ffxVkPortableValidateFrameGenerationDispatchInfo(&create_info, &dispatch)) {
        PrintIssues("dispatch", issues);
        broken = true;
        return false;
    }
    if (ffxVkPortableFrameGenerationContextRecordDispatch(context, &dispatch) !=
        FFX_VK_PORTABLE_OK) {
        Fail("the interpolation dispatch failed");
        return false;
    }
    const vk::MemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    if (!reset && in.mask_view && in.previous_mask_view && in.motion_view && MaskMode() != 0) {
        ComposeMovingObjects(cmdbuf, frame, slot, in);
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    }
    if (!announced) {
        announced = true;
        std::printf("Frame generation: interpolating %ux%u (scene %ux%u)\n", display.width,
                    display.height, in.render.width, in.render.height);
        std::fflush(stdout);
    }
    // The first frame of a sequence has nothing to interpolate from: only the real one shows.
    return !reset;
}

} // namespace Vulkan
