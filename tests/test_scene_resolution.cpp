// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises the production target/resolve path on Vulkan (Lavapipe works, no game/window).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/texture_cache/blit_helper.h"
#include <vk_mem_alloc.h>

int main(int argc, char** argv) {
    using namespace Vulkan;
    Instance instance(0, false);
    // Calls emitted by this executable use a local dispatcher. libbbgpu.so initializes
    // its own default dispatcher while constructing Instance.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(instance.GetDevice());
    Scheduler scheduler(instance);
    Runtime runtime(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    Common::SlotVector<VideoCore::Image> images;
    VideoCore::ImageInfo ci;
    ci.size = {argc > 3 ? unsigned(std::atoi(argv[2])) : 1920,
               argc > 3 ? unsigned(std::atoi(argv[3])) : 1080,1};
    ci.resources = {1,1};
    ci.type = AmdGpu::ImageType::Color2D;
    ci.pixel_format = vk::Format::eR8G8B8A8Unorm;
    ci.num_bits = 32;
    ci.num_samples = argc > 1 ? unsigned(std::atoi(argv[1])) : 1;
    const bool multisample = ci.num_samples > 1;
    const auto color_id = images.insert(instance, runtime, views, ci);
    auto di = ci;
    di.pixel_format = vk::Format::eD32SfloatS8Uint;
    di.props.is_depth = di.props.has_stencil = true;
    const auto depth_id = images.insert(instance, runtime, views, di);
    SceneTargets targets(instance, scheduler, runtime, [&](VideoCore::ImageId id, u64 uid) {
        if (!images.is_allocated(id)) return static_cast<VideoCore::Image*>(nullptr);
        return !uid || images[id].image_uid == uid ? &images[id] : nullptr;
    });
    targets.SetNativeSize({ci.size.width, ci.size.height});
    const bool color_eligible = targets.Eligible(images[color_id]);
    const bool depth_eligible = targets.Eligible(images[depth_id]);
    images[color_id].usage.vo_surface = true;
    assert(!targets.Eligible(images[color_id]));
    images[color_id].usage.vo_surface = false;
    std::fprintf(stderr, "Scene target eligibility: color=%d depth=%d\n",
                 color_eligible, depth_eligible);
    if (!color_eligible || !depth_eligible) {
        std::puts("Scene targets: unsupported format blit, live scaling skipped");
        return 0;
    }

    VkBuffer readback{};
    VmaAllocation allocation{};
    VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=16, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage=VMA_MEMORY_USAGE_AUTO,
        .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &readback, &allocation, &ai) == VK_SUCCESS);
    const auto pixel = [&](vk::Image image, vk::ImageAspectFlagBits aspect,
                           vk::ImageLayout layout, u32 w, u32 h) {
        scheduler.EndRendering();
        auto cmd = scheduler.CommandBuffer();
        VideoCore::UniqueImage resolved(instance.GetDevice(), instance.GetAllocator());
        vk::UniqueImageView source_view, resolved_view;
        const auto source = image;
        const auto source_layout = layout;
        const bool depth = aspect != vk::ImageAspectFlagBits::eColor;
        const auto aspects = depth ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil :
                                     vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
        if (multisample) {
            auto info = images[depth ? depth_id : color_id].backing->image.image_ci;
            info.extent = vk::Extent3D{w,h,1};
            info.samples = vk::SampleCountFlagBits::e1;
            resolved.Create(info);
            const auto view = [&](vk::Image target) {
                return instance.GetDevice().createImageViewUnique({.image=target,
                    .viewType=vk::ImageViewType::e2D,.format=info.format,
                    .subresourceRange={aspects,0,1,0,1}}, nullptr, dispatch).value;
            };
            source_view = view(source);
            resolved_view = view(resolved);
            const auto attach_layout = depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal :
                                               vk::ImageLayout::eColorAttachmentOptimal;
            const std::array barriers{
                vk::ImageMemoryBarrier2{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                    .srcAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask=vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                    .oldLayout=layout,.newLayout=attach_layout,.image=source,.subresourceRange={aspects,0,1,0,1}},
                vk::ImageMemoryBarrier2{.dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                    .dstAccessMask=vk::AccessFlagBits2::eMemoryWrite,
                    .oldLayout=vk::ImageLayout::eUndefined,.newLayout=attach_layout,
                    .image=resolved,.subresourceRange={aspects,0,1,0,1}}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=2,.pImageMemoryBarriers=barriers.data()},dispatch);
            const vk::RenderingAttachmentInfo attachment{.imageView=*source_view,.imageLayout=attach_layout,
                .resolveMode=depth ? vk::ResolveModeFlagBits::eSampleZero : vk::ResolveModeFlagBits::eAverage,
                .resolveImageView=*resolved_view,.resolveImageLayout=attach_layout,
                .loadOp=vk::AttachmentLoadOp::eLoad,.storeOp=vk::AttachmentStoreOp::eStore};
            cmd.beginRendering({.renderArea={{0,0},{w,h}},.layerCount=1,
                .colorAttachmentCount=depth ? 0u : 1u,.pColorAttachments=depth ? nullptr : &attachment,
                .pDepthAttachment=depth ? &attachment : nullptr,.pStencilAttachment=depth ? &attachment : nullptr},dispatch);
            cmd.endRendering(dispatch);
            auto after=barriers;
            after[0].oldLayout=attach_layout;after[0].newLayout=source_layout;
            after[1].srcStageMask=vk::PipelineStageFlagBits2::eAllCommands;
            after[1].srcAccessMask=vk::AccessFlagBits2::eMemoryWrite;
            after[1].dstStageMask=vk::PipelineStageFlagBits2::eTransfer;
            after[1].dstAccessMask=vk::AccessFlagBits2::eTransferRead;
            after[1].oldLayout=attach_layout;after[1].newLayout=vk::ImageLayout::eTransferSrcOptimal;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=2,.pImageMemoryBarriers=after.data()},dispatch);
            image=resolved;layout=vk::ImageLayout::eTransferSrcOptimal;
        }
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b}, dispatch);
        const vk::BufferImageCopy region{.imageSubresource={aspect,0,0,1},
            .imageOffset={s32(w-1),s32(h-1),0},.imageExtent={1,1,1}};
        cmd.copyImageToBuffer(image, layout, readback, region, dispatch);
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host}, dispatch);
        scheduler.Finish();
        u32 result;
        std::memcpy(&result, ai.pMappedData, 4);
        return result;
    };
    VideoCore::ImageViewInfo cv;
    cv.format = ci.pixel_format;
    auto dv = cv;
    dv.format = di.pixel_format;
    const auto clear_native = [&](u32 rgba, u32 stencil = 7) {
        vk::ClearValue clear{};
        clear.color.float32 = std::array{float(rgba & 255)/255, float((rgba>>8)&255)/255,
                                        float((rgba>>16)&255)/255, 1.0f};
        runtime.ClearImage(&images[color_id], {}, clear);
        runtime.Transit(&images[depth_id], vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        runtime.FlushBarriers();
        scheduler.CommandBuffer().clearDepthStencilImage(
            images[depth_id].GetImage(), vk::ImageLayout::eTransferDstOptimal,
            vk::ClearDepthStencilValue{0.25f, stencil},
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth |
                vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1}, dispatch);
    };
    constexpr u32 stencil_values[] = {0, 255, 128, 85, 170, 7, 13, 255, 0, 170};
    u32 stencil_index = 0;
    for (int preset : {3,1,4,2,0,3,0,1,0,3}) {
        const u32 copied_stencil = stencil_values[stencil_index++];
        const u32 resolved_stencil = copied_stencil ^ 255;
        const auto output = stencil_index > 7 ? SceneResolution::Size{ci.size.width * 2, ci.size.height * 2}
                                             : SceneResolution::Size{ci.size.width, ci.size.height};
        const auto size = SceneResolution::ForPreset(preset, output);
        targets.SetSize(size);
        assert(SceneResolution::Unpack(SceneResolution::Pack(size)) == size);
        clear_native(0xffff0000, copied_stencil); // blue
        auto c = targets.Read(color_id,cv);
        assert(pixel(c.image,vk::ImageAspectFlagBits::eColor,c.layout,size.width,size.height)==0xffff0000);
        auto d = targets.Read(depth_id,dv);
        const u32 depth_bits = pixel(d.image,vk::ImageAspectFlagBits::eDepth,d.layout,size.width,size.height);
        assert(std::bit_cast<float>(depth_bits)==0.25f);
        assert((pixel(d.image,vk::ImageAspectFlagBits::eStencil,d.layout,size.width,size.height)&255)==copied_stencil);

        c = targets.Attachment(color_id,cv);
        d = targets.Attachment(depth_id,dv);
        auto cmd = scheduler.CommandBuffer();
        const vk::RenderingAttachmentInfo color{.imageView=c.view,.imageLayout=c.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.color={.float32=std::array{1.f,0.f,0.f,1.f}}}};
        const vk::RenderingAttachmentInfo depth{.imageView=d.view,.imageLayout=d.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.depthStencil={0.75f,resolved_stencil}}};
        cmd.beginRendering({.renderArea={{0,0},{size.width,size.height}},.layerCount=1,
            .colorAttachmentCount=1,.pColorAttachments=&color,
            .pDepthAttachment=&depth,.pStencilAttachment=&depth}, dispatch);
        cmd.endRendering(dispatch);
        runtime.Transit(&images[color_id],vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        assert(pixel(images[color_id].GetImage(),vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eTransferSrcOptimal,ci.size.width,ci.size.height)==0xff0000ff);
        runtime.Transit(&images[depth_id],vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        assert(std::bit_cast<float>(pixel(images[depth_id].GetImage(),vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eTransferSrcOptimal,ci.size.width,ci.size.height))==0.75f);
        assert((pixel(images[depth_id].GetImage(),vk::ImageAspectFlagBits::eStencil,
                     vk::ImageLayout::eTransferSrcOptimal,ci.size.width,ci.size.height)&255)==resolved_stencil);
        clear_native(0xff00ff00); // overwrite after reduced render: proxy must be invalidated
        c=targets.Read(color_id,cv);
        assert(pixel(c.image,vk::ImageAspectFlagBits::eColor,c.layout,size.width,size.height)==0xff00ff00);
        std::printf("Scene targets: %ux%u color/depth/stencil roundtrip PASS\n",size.width,size.height);
    }

    if (multisample || argc > 3) {
        scheduler.Finish();
        vmaDestroyBuffer(instance.GetAllocator(),readback,allocation);
        std::printf("Scene targets: color/depth/stencil live resizing PASS (%u samples)\n", ci.num_samples);
        return 0;
    }

    // Half-resolution mip chain (the game's bloom pyramid): one proxy per level, scaled by the
    // scene's factor, resolved into its own native level.
    auto hi = ci;
    hi.size = {960, 540, 1};
    hi.num_samples = 1;
    hi.resources = {4, 1};
    const auto half_id = images.insert(instance, runtime, views, hi);
    const auto scene = SceneResolution::ForPreset(4); // 640x360 (x3 of 1080p)
    targets.SetSize(scene);
    auto& half = images[half_id];
    assert(targets.Eligible(half) && !targets.EligibleScene(half));
    assert((targets.ProxySize(half, 0) == SceneResolution::Size{320, 180}));
    assert((targets.ProxySize(half, 2) == SceneResolution::Size{80, 45}));
    auto lv = cv;
    lv.range.base.level = 2;
    const auto proxy = targets.Attachment(half_id, lv);
    {
        auto cmd = scheduler.CommandBuffer();
        const vk::RenderingAttachmentInfo color{.imageView=proxy.view,.imageLayout=proxy.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.color={.float32=std::array{0.f,1.f,0.f,1.f}}}};
        cmd.beginRendering({.renderArea={{0,0},{80,45}},.layerCount=1,
            .colorAttachmentCount=1,.pColorAttachments=&color}, dispatch);
        cmd.endRendering(dispatch);
    }
    assert(targets.ProxyCurrent(half, 2, 1) && !targets.ProxyCurrent(half, 0, 4));
    // A native access resolves the level: its last texel (239, 134) is the proxy's colour.
    runtime.Transit(&half, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    {
        scheduler.EndRendering();
        const auto cmd = scheduler.CommandBuffer();
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b}, dispatch);
        const vk::BufferImageCopy region{.imageSubresource={vk::ImageAspectFlagBits::eColor,2,0,1},
            .imageOffset={239,134,0},.imageExtent={1,1,1}};
        cmd.copyImageToBuffer(half.GetImage(), vk::ImageLayout::eTransferSrcOptimal, readback,
                              region, dispatch);
        scheduler.Finish();
        u32 result;
        std::memcpy(&result, ai.pMappedData, 4);
        assert(result == 0xff00ff00);
    }
    std::puts("Scene targets: half-resolution mip level proxy roundtrip PASS");
    scheduler.Finish();
    vmaDestroyBuffer(instance.GetAllocator(),readback,allocation);
}
