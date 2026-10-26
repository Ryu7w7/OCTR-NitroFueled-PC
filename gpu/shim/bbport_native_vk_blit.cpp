// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk_blit.h"

#include <cstdio>
#include <unordered_map>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers

#include "bbport_native_vk_host_shaders.h"

namespace BbNativeVk::Blit {
namespace {
using u32 = std::uint32_t;

vk::Device device;
vk::ShaderModule vertex, depth_fragment, color_fragment;
vk::DescriptorSetLayout set_layout;
vk::PipelineLayout layout;
std::unordered_map<vk::Format, vk::Pipeline> pipelines[2]; // [from depth] by destination format

template <std::size_t N>
vk::ShaderModule Module(const std::uint32_t (&code)[N]) {
    auto [r, m] = device.createShaderModule({.codeSize = sizeof(code), .pCode = code});
    return r == vk::Result::eSuccess ? m : vk::ShaderModule{};
}

/// Formats a float written by the shader lands in unchanged (as the engine's R32F copies).
bool FloatColor(vk::Format f) {
    switch (f) {
    case vk::Format::eR32Sfloat:
    case vk::Format::eR16Sfloat:
    case vk::Format::eR16Unorm:
    case vk::Format::eR32G32Sfloat:
    case vk::Format::eR16G16Sfloat:
        return true;
    default:
        return false;
    }
}

vk::Pipeline CopyPipeline(bool from_depth, vk::Format format) {
    auto& cache = pipelines[from_depth];
    if (const auto it = cache.find(format); it != cache.end()) {
        return it->second;
    }
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = vertex, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment,
         .module = from_depth ? depth_fragment : color_fragment,
         .pName = "main"},
    }};
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList};
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster{.cullMode = vk::CullModeFlagBits::eNone,
                                                          .lineWidth = 1.0f};
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineDepthStencilStateCreateInfo depth{};
    const vk::PipelineColorBlendAttachmentState write_all{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &write_all};
    const std::array dyn{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount = u32(dyn.size()),
                                                     .pDynamicStates = dyn.data()};
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount = 1,
                                                    .pColorAttachmentFormats = &format};
    const vk::GraphicsPipelineCreateInfo info{.pNext = &rendering,
                                              .stageCount = u32(stages.size()),
                                              .pStages = stages.data(),
                                              .pVertexInputState = &vertex_input,
                                              .pInputAssemblyState = &assembly,
                                              .pViewportState = &viewport,
                                              .pRasterizationState = &raster,
                                              .pMultisampleState = &multisample,
                                              .pDepthStencilState = &depth,
                                              .pColorBlendState = &blend,
                                              .pDynamicState = &dynamic,
                                              .layout = layout};
    auto [r, pipeline] = device.createGraphicsPipeline(Gpu::PipelineCache(), info);
    if (r != vk::Result::eSuccess) {
        std::printf("Native Vulkan: %s copy pipeline for %s not created (%s)\n",
                    from_depth ? "depth" : "color", vk::to_string(format).c_str(), vk::to_string(r).c_str());
        pipeline = vk::Pipeline{};
    }
    cache[format] = pipeline;
    return pipeline;
}
} // namespace

void Init(const Vulkan::Instance& instance) {
    device = instance.GetDevice();
    vertex = Module(HostShaders::fullscreen_vert);
    depth_fragment = Module(HostShaders::depth_to_color_frag);
    color_fragment = Module(HostShaders::copy_color_frag);
    const vk::DescriptorSetLayoutBinding binding{.binding = 0,
                                                 .descriptorType = vk::DescriptorType::eSampledImage,
                                                 .descriptorCount = 1,
                                                 .stageFlags = vk::ShaderStageFlagBits::eFragment};
    auto [sr, sl] = device.createDescriptorSetLayout({.bindingCount = 1, .pBindings = &binding});
    set_layout = sr == vk::Result::eSuccess ? sl : vk::DescriptorSetLayout{};
    const vk::PushConstantRange push{.stageFlags = vk::ShaderStageFlagBits::eFragment,
                                     .offset = 0,
                                     .size = 8};
    auto [lr, pl] = device.createPipelineLayout({.setLayoutCount = set_layout ? 1u : 0u,
                                                 .pSetLayouts = &set_layout,
                                                 .pushConstantRangeCount = 1,
                                                 .pPushConstantRanges = &push});
    layout = lr == vk::Result::eSuccess ? pl : vk::PipelineLayout{};
    if (!vertex || !depth_fragment || !color_fragment || !set_layout || !layout) {
        std::printf("Native Vulkan: copy shaders not set up: copies are transfers\n");
        layout = vk::PipelineLayout{};
    }
}

bool Copy(Gpu::Image& src, VkImageView src_view, vk::Offset2D from, Gpu::Image& dst,
          vk::Rect2D rect) {
    if (!layout || !src_view || dst.depth || (src.depth && !FloatColor(dst.format))) {
        return false;
    }
    const vk::Pipeline pipeline = CopyPipeline(src.depth, dst.format);
    if (!pipeline) {
        return false;
    }
    Gpu::Use(src, vk::ImageLayout::eShaderReadOnlyOptimal);
    Gpu::Pass pass{};
    pass.color[0] = &dst;
    Gpu::Render(pass);
    if (!Gpu::Rendering()) {
        return false;
    }
    const vk::DescriptorSet set = Gpu::AllocateSet(set_layout);
    if (!set) {
        return false;
    }
    const vk::DescriptorImageInfo image{.imageView = src_view,
                                        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    device.updateDescriptorSets(vk::WriteDescriptorSet{.dstSet = set,
                                                       .dstBinding = 0,
                                                       .descriptorCount = 1,
                                                       .descriptorType = vk::DescriptorType::eSampledImage,
                                                       .pImageInfo = &image},
                                {});
    Gpu::Mark(src.depth ? Gpu::Work::DepthCopy : Gpu::Work::Copy, rect.extent.width, rect.extent.height, dst.format);
    const vk::CommandBuffer cmd = Gpu::Cmd();
    const std::int32_t shift[2] = {from.x - rect.offset.x, from.y - rect.offset.y};
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, set, {});
    cmd.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(shift), shift);
    cmd.setViewport(0, vk::Viewport{0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f});
    cmd.setScissor(0, rect);
    cmd.draw(3, 1, 0, 0);
    return true;
}
} // namespace BbNativeVk::Blit
