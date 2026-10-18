// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_ctr_fsr3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vk_mem_alloc.h>

#include "bbport_settings.h"
#include "ffx_vk_portable.h"
#include "video_core/host_shaders/ctr_fsr3_inputs_comp.h"
#include "video_core/host_shaders/ctr_fsr3_inputs_d_comp.h"
#include "video_core/host_shaders/ctr_fsr3_inputs_dv_comp.h"
#include "video_core/host_shaders/ctr_fsr3_inputs_v_comp.h"
#include "video_core/host_shaders/ctr_framegen_mask_clean_comp.h"
#include "video_core/host_shaders/ctr_framegen_mask_comp.h"
#include "video_core/renderer_vulkan/vk_ctr_framegen.h"
#include "video_core/renderer_vulkan/vk_dlss_ngx.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

// Moving objects (CTR's velocity target, memory channel 2 marks them; their own motion in
// channels 1 and 0): BB_FSR3_MV="sx,sy,bias,mode,cx,cy" adds pixels = v * s to the camera's
// motion, with e = channel - bias and v = e (mode 1) or sign(e) e^2 (mode 2); mode 0 (default)
// leaves them still on the screen, which measured best. BB_FSR3_MV=nocamera drops the camera's
// motion (tests).
struct Decode {
    float sx = 0.0f, sy = 0.0f, bias = 127.0f / 255.0f;
    u32 mode = 0, cx = 1, cy = 0;
    bool camera = true;
};

const Decode& MotionDecode() {
    static const Decode decode = [] {
        Decode d;
        if (const char* env = std::getenv("BB_FSR3_MV")) {
            if (std::strcmp(env, "nocamera") == 0) {
                d.camera = false;
            } else {
                std::sscanf(env, "%f,%f,%f,%u,%u,%u", &d.sx, &d.sy, &d.bias, &d.mode, &d.cx, &d.cy);
                d.cx &= 3;
                d.cy &= 3;
                d.mode = std::min(d.mode, 2u);
            }
        }
        std::printf("FSR 3: motion: camera %s; moving objects %s (channels %u/%u - %.4f) * %g/%g px\n",
                    d.camera ? "reprojected" : "off",
                    d.mode == 0 ? "still" : d.mode == 1 ? "linear" : "squared", d.cx, d.cy, d.bias,
                    d.sx, d.sy);
        return d;
    }();
    return decode;
}

int DebugMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_FSR3_DEBUG");
        return env ? std::atoi(env) : 0;
    }();
    return mode;
}

/// BB_FSR3_DUMP_TRIGGER=<file>: creating it saves the FSR 3 inputs of the next BB_FSR3_DUMP_FRAMES
/// (3) frames (BB_FSR3_DUMP_DIR, default the current folder) and prints the camera constants.
/// Returns the frame number to save or -1.
int DumpFrame() {
    static const char* trigger = std::getenv("BB_FSR3_DUMP_TRIGGER");
    static u32 polls = 0;
    static int remaining = 0, index = 0;
    if (!trigger) {
        return -1;
    }
    if (remaining == 0) {
        if (++polls % 30 != 0 || std::remove(trigger) != 0) {
            return -1;
        }
        const char* frames = std::getenv("BB_FSR3_DUMP_FRAMES");
        remaining = frames ? std::clamp(std::atoi(frames), 1, 16) : 3;
    }
    --remaining;
    return index++;
}

/// Column-major 4x4 matrices (the game's layout).
using Mat4 = std::array<double, 16>;
Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            r[col * 4 + row] = sum;
        }
    }
    return r;
}
bool Invert(const Mat4& m, Mat4& out) {
    // Gauss-Jordan on a row-major copy.
    double a[4][8];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            a[row][col] = m[col * 4 + row];
            a[row][col + 4] = row == col ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 4; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
        }
        if (std::abs(a[pivot][col]) < 1e-12) return false;
        if (pivot != col) {
            for (int k = 0; k < 8; ++k) std::swap(a[pivot][k], a[col][k]);
        }
        const double inv = 1.0 / a[col][col];
        for (int k = 0; k < 8; ++k) a[col][k] *= inv;
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            const double f = a[row][col];
            for (int k = 0; k < 8; ++k) a[row][k] -= f * a[col][k];
        }
    }
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[col * 4 + row] = a[row][col + 4];
        }
    }
    return true;
}
Mat4 Load(const float* f) {
    Mat4 m{};
    for (int i = 0; i < 16; ++i) m[i] = f[i];
    return m;
}

float Halton(u32 index, u32 base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

FfxVkPortableImage Describe(vk::Image image, vk::Format format, u32 w, u32 h,
                            vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                            FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = image;
    out.format = static_cast<VkFormat>(format);
    out.extent = {w, h};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(usage);
    out.aspect = static_cast<VkImageAspectFlags>(aspect);
    out.state = state;
    return out;
}

void PrintIssues(const char* what, u64 issues) {
    std::printf("FSR 3: %s invalid:", what);
    for (u32 bit = 0; bit < 64; ++bit) {
        if (issues & (1ull << bit)) {
            std::printf(" %s", ffxVkPortableValidationIssueName(1ull << bit));
        }
    }
    std::printf("\n");
}

vk::Format Linear(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    default:
        return format;
    }
}

} // namespace

CtrFsr3::CtrFsr3(const Instance& instance_, Scheduler& scheduler_,
                 VideoCore::TextureCache& texture_cache_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_} {}

CtrFsr3::~CtrFsr3() {
    if (context) {
        ffxVkPortableUpscaleContextDestroy(context);
    }
}

bool CtrFsr3::Enabled() const {
    const int temporal = BbSettings::Get().temporal;
    return !failed && (temporal == BbSettings::TemporalFsr3 || temporal == BbSettings::TemporalDlss);
}

void CtrFsr3::OnGBufferPass(VideoCore::ImageId depth, VideoCore::ImageId velocity_,
                            const Viewport& viewport_) {
    scene_depth = depth;
    velocity = velocity_;
    // The texture cache may delete either before the display pass (loading screens reuse their
    // memory): the ids are only trusted with these uids.
    scene_depth_uid = texture_cache.GetImage(depth).image_uid;
    velocity_uid = texture_cache.GetImage(velocity_).image_uid;
    viewport = viewport_;
    scene_this_frame = true;
}

void CtrFsr3::OnCameraConstants(const float* data) {
    std::memcpy(camera.data(), data, sizeof(camera));
    camera_this_frame = true;
    camera_valid = true;
}

void CtrFsr3::OnFrameEnd() {
    frame_jitter = jitter;
    if (!Enabled()) {
        jitter = {};
        reset = true;
        return;
    }
    jitter_index = jitter_index % std::max(jitter_phases, 1u) + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    if (std::getenv("BB_FSR3_NO_JITTER")) {
        jitter = {};
    }
}

std::array<float, 2> CtrFsr3::DrawJitter(VideoCore::ImageId depth, bool full_screen) const {
    if (full_screen || !depth || depth != scene_depth || !Enabled()) {
        return {};
    }
    return jitter;
}

vk::ImageView CtrFsr3::SourceView(const VideoCore::Image& image, vk::Format format,
                                  vk::ImageAspectFlags aspect, int slot) {
    auto& entry = source_views[slot];
    const vk::Image handle = image.GetImage();
    if (entry.view && entry.image == handle && entry.uid == image.image_uid &&
        entry.format == format) {
        return *entry.view;
    }
    if (entry.view) {
        scheduler.DeferOperation([view = std::move(entry.view)] {});
    }
    entry.image = handle;
    entry.uid = image.image_uid;
    entry.format = format;
    entry.view = Check(instance.GetDevice().createImageViewUnique({
        .image = handle,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    }));
    return *entry.view;
}

// The display pass reads the game's frame through a view with its own format and channel
// swizzle (CTR's swaps red and blue); the upscaled frame is read the same way.
vk::ImageView CtrFsr3::OutputView(vk::Format format, vk::ComponentMapping mapping) {
    for (auto& entry : output_views) {
        if (entry.view && entry.format == format && entry.mapping == mapping) {
            return *entry.view;
        }
    }
    auto& entry = output_views[output_view_next++ % output_views.size()];
    if (entry.view) {
        scheduler.DeferOperation([view = std::move(entry.view)] {});
    }
    const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eSampled};
    entry.format = format;
    entry.mapping = mapping;
    entry.view = Check(instance.GetDevice().createImageViewUnique({
        .pNext = &usage,
        .image = vk::Image(output_image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    return *entry.view;
}

void CtrFsr3::Skip(const char* reason) {
    reset = true;
    if (reason != last_skip) {
        last_skip = reason;
        std::printf("FSR 3: skipped (%s)\n", reason);
        std::fflush(stdout);
    }
}

void CtrFsr3::CreatePipelines() {
    if (pipelines[0]) {
        return;
    }
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 6> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {5, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 128};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    }));
    const auto compute = [&](std::span<const u32> code) {
        const auto module = CompileSPV(code, device);
        auto result = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *pipeline_layout,
                }));
        device.destroyShaderModule(module);
        return result;
    };
    pipelines[0] = compute(CTR_FSR3_INPUTS_COMP);
    pipelines[1] = compute(CTR_FSR3_INPUTS_D_COMP);
    pipelines[2] = compute(CTR_FSR3_INPUTS_V_COMP);
    pipelines[3] = compute(CTR_FSR3_INPUTS_DV_COMP);
}

bool CtrFsr3::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, vk::Format format) {
    if (context && w == width && h == height && ow == out_width && oh == out_height &&
        format == output_format) {
        return true;
    }
    const auto device = instance.GetDevice();
    scheduler.Finish();
    if (context) {
        ffxVkPortableUpscaleContextDestroy(context);
        context = nullptr;
    }
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    output_format = format;
    reset = true;

    FfxVkPortableDeviceInfo device_info{};
    device_info.structSize = sizeof(device_info);
    device_info.instance = instance.GetInstance();
    device_info.physicalDevice = instance.GetPhysicalDevice();
    device_info.device = device;
    device_info.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device_info.queue = instance.GetGraphicsQueue();
    device_info.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    device_info.shaderFloat16Enabled = instance.IsShaderFloat16Enabled();
    device_info.subgroupSizeControlEnabled = instance.IsSubgroupSizeControlEnabled();
    device_info.synchronization2Enabled = VK_TRUE;
    device_info.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    // The game's finished, tonemapped frame (8 bits a channel): no HDR, no exposure.
    create_info.flags = std::getenv("BB_FSR3_DEPTH_INVERTED") ? FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED : 0;
    create_info.maxRenderSize = {w, h};
    create_info.maxOutputSize = {ow, oh};
    if (const u64 issues = ffxVkPortableValidateUpscaleCreateInfo(&create_info)) {
        PrintIssues("create info", issues);
        return false;
    }
    if (ffxVkPortableUpscaleContextCreate(&device_info, &create_info, &context) !=
        FFX_VK_PORTABLE_OK) {
        std::printf("FSR 3: context creation failed\n");
        context = nullptr;
        return false;
    }

    const auto allocator = instance.GetAllocator();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView* view,
                          vk::Format format, u32 iw, u32 ih, vk::ImageUsageFlags usage,
                          const void* next = nullptr, vk::ImageCreateFlags flags = {}) {
        if (view) {
            view->reset();
        }
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .pNext = next,
            .flags = flags,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw, ih, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        if (view) {
            *view = Check(device.createImageViewUnique({
                .image = vk::Image(image),
                .viewType = vk::ImageViewType::e2D,
                .format = format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
            }));
        }
    };
    const auto inputs = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                        vk::ImageUsageFlagBits::eTransferSrc;
    make(depth_image, &depth_view, vk::Format::eR32Sfloat, w, h, inputs);
    make(motion_image, &motion_view, vk::Format::eR16G16Sfloat, w, h, inputs);
    make(debug_image, &debug_view, vk::Format::eR8G8B8A8Unorm, w, h, inputs);
    for (u32 i = 0; i < 2; ++i) {
        make(mask_images[i], &mask_views[i], vk::Format::eR8Unorm, w, h, inputs);
    }
    make(raw_mask, &raw_mask_view, vk::Format::eR8Unorm, w, h, inputs);
    previous_mask_valid = false;
    // The output has the frame's memory layout (RGBA or BGRA) and is sampled through views like
    // the game's (sRGB or not, swizzled).
    static constexpr std::array<vk::Format, 4> formats{
        vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Unorm,
        vk::Format::eB8G8R8A8Srgb};
    const vk::ImageFormatListCreateInfo list{.viewFormatCount = u32(formats.size()),
                                             .pViewFormats = formats.data()};
    for (auto& entry : output_views) {
        entry.view.reset();
    }
    make(output_image, nullptr, format, ow, oh,
         vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
             vk::ImageUsageFlagBits::eTransferSrc,
         &list, vk::ImageCreateFlagBits::eMutableFormat);
    {
        const vk::ImageViewUsageCreateInfo storage{.usage = vk::ImageUsageFlagBits::eStorage};
        output_storage_view = Check(device.createImageViewUnique({
            .pNext = &storage,
            .image = vk::Image(output_image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    }
    CreatePipelines();

    // The images start in General: FSR 3 reads and writes them there.
    const auto cmdbuf = scheduler.CommandBuffer();
    std::array<vk::ImageMemoryBarrier2, 7> barriers{};
    const std::array<vk::Image, 7> images{vk::Image(depth_image),    vk::Image(motion_image),
                                          vk::Image(debug_image),    vk::Image(output_image),
                                          vk::Image(mask_images[0]), vk::Image(mask_images[1]),
                                          vk::Image(raw_mask)};
    for (u32 i = 0; i < barriers.size(); ++i) {
        barriers[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = images[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(barriers.size()),
                             .pImageMemoryBarriers = barriers.data()});

    // FSR's recommended jitter sequence length: 8 times the area ratio.
    const float ratio = float(ow) / float(w);
    jitter_phases = std::clamp(u32(std::ceil(8.0f * ratio * ratio)), 8u, 72u);
    FfxVkPortableMemoryUsage usage{};
    usage.structSize = sizeof(usage);
    ffxVkPortableUpscaleContextGetMemoryUsage(context, &usage);
    std::printf("FSR 3: %ux%u -> %ux%u (%s), %u jitter phases, %.1f MB\n", w, h, ow, oh,
                vk::to_string(format).c_str(), jitter_phases, usage.totalUsageInBytes / 1e6);
    return true;
}

void CtrFsr3::CameraPlanes(float& near_plane, float& far_plane, float& fov) const {
    near_plane = 16.0f;
    far_plane = 150000.0f;
    fov = 1.2870f;
    if (camera_valid) {
        // Projection at floats 48-63 (column-major, OpenGL style): [5] = 1/tan(fov/2),
        // [10] = A, [14] = B with near = B/(A-1), far = B/(A+1).
        const float* p = &camera[48];
        const float a = p[10], b = p[14];
        if (p[5] > 0.0f && std::isfinite(a) && std::isfinite(b) && a != 1.0f && a != -1.0f) {
            fov = 2.0f * std::atan(1.0f / p[5]);
            near_plane = std::abs(b / (a - 1.0f));
            far_plane = std::abs(b / (a + 1.0f));
        }
    }
    if (!(near_plane > 0.0f) || !(far_plane > near_plane) || !(fov > 0.01f && fov < 3.1f)) {
        near_plane = 16.0f;
        far_plane = 150000.0f;
        fov = 1.2870f;
    }
}

void CtrFsr3::PublishFrameGen(vk::ImageView hudless_view, vk::Extent2D out, float frame_ms,
                              bool upscaler_reset) {
    FrameGenInputs in{};
    in.hudless = vk::Image(output_image);
    in.hudless_view = hudless_view;
    in.display = out;
    in.depth = vk::Image(depth_image);
    in.motion = vk::Image(motion_image);
    const u32 previous = previous_mask_valid && !upscaler_reset ? mask_index ^ 1u : mask_index;
    in.motion_view = *motion_view;
    in.mask = vk::Image(mask_images[mask_index]);
    in.mask_view = *mask_views[mask_index];
    in.previous_mask = vk::Image(mask_images[previous]);
    in.previous_mask_view = *mask_views[previous];
    previous_mask_valid = true;
    in.render = {width, height};
    in.jitter = frame_jitter;
    in.frame_ms = frame_ms;
    CameraPlanes(in.near_plane, in.far_plane, in.fov);
    // The camera: the inverse of the view (floats 32-47, OpenGL style, the camera looks down -z)
    // has the right, up and backward axes and the position in its columns.
    Mat4 inverse{};
    if (camera_valid && Invert(Load(&camera[32]), inverse)) {
        for (int i = 0; i < 3; ++i) {
            in.right[i] = float(inverse[0 * 4 + i]);
            in.up[i] = float(inverse[1 * 4 + i]);
            in.forward[i] = float(-inverse[2 * 4 + i]);
            in.position[i] = float(inverse[3 * 4 + i]);
        }
    }
    const auto finite = [](const std::array<float, 3>& v) {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    };
    if (!finite(in.right) || !finite(in.up) || !finite(in.forward) || !finite(in.position)) {
        in.position = {};
        in.up = {0.0f, 1.0f, 0.0f};
        in.right = {1.0f, 0.0f, 0.0f};
        in.forward = {0.0f, 0.0f, -1.0f};
    }
    std::scoped_lock lock{framegen_mutex};
    in.reset = framegen_reset || upscaler_reset;
    in.sequence = ++framegen_sequence;
    framegen = in;
    framegen_ready = true;
    framegen_reset = false;
    framegen_published = true;
}

void CtrFsr3::ErrorMask(VideoCore::Image& source, u32 w, u32 h) {
    // BB_FRAMEGEN_ERROR: the largest channel difference (0..1) still taken as the same surface.
    static const float threshold = [] {
        const char* env = std::getenv("BB_FRAMEGEN_ERROR");
        return env && env[0] ? std::clamp(float(std::atof(env)), 0.01f, 1.0f) : 0.12f;
    }();
    const auto device = instance.GetDevice();
    const vk::Format format = Linear(source.info.pixel_format);
    constexpr vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    if (!error_pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 5> bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {4, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        error_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(bindings.size()),
            .pBindings = bindings.data(),
        }));
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 16};
        error_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*error_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
        const auto module = CompileSPV(CTR_FRAMEGEN_MASK_COMP, device);
        error_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *error_layout,
                }));
        device.destroyShaderModule(module);

        const std::array<vk::DescriptorSetLayoutBinding, 2> clean_bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        clean_desc_layout = Check(device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = u32(clean_bindings.size()),
            .pBindings = clean_bindings.data(),
        }));
        clean_layout = Check(device.createPipelineLayoutUnique({
            .setLayoutCount = 1,
            .pSetLayouts = &*clean_desc_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        }));
        const auto clean_module = CompileSPV(CTR_FRAMEGEN_MASK_CLEAN_COMP, device);
        clean_pipeline = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = clean_module,
                              .pName = "main"},
                    .layout = *clean_layout,
                }));
        device.destroyShaderModule(clean_module);
    }
    if (!previous_color || previous_color_format != format || previous_color_size.width != w ||
        previous_color_size.height != h) {
        if (previous_color) {
            scheduler.DeferOperation(
                [old = std::move(previous_color), view = std::move(previous_color_view)] {});
        }
        previous_color = VideoCore::UniqueImage(device, instance.GetAllocator());
        previous_color.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        previous_color_view = Check(device.createImageViewUnique({
            .image = vk::Image(previous_color),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = range,
        }));
        previous_color_format = format;
        previous_color_size = {w, h};
        previous_color_valid = false;
    }
    // 1. The comparison (when the previous frame continues into this one) and the velocity
    //    target's mask into the raw mask, then its dense parts into this frame's mask.
    {
        const auto cmdbuf = scheduler.CommandBuffer();
        const bool compare = previous_color_valid && !reset;
        const vk::DescriptorImageInfo infos[5] = {
            {{}, SourceView(source, format, vk::ImageAspectFlagBits::eColor, 3),
             vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, *previous_color_view, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, *motion_view, vk::ImageLayout::eGeneral},
            {{}, *mask_views[mask_index], vk::ImageLayout::eGeneral},
            {{}, *raw_mask_view, vk::ImageLayout::eGeneral},
        };
        std::array<vk::WriteDescriptorSet, 5> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = i >= 3 ? vk::DescriptorType::eStorageImage
                                                  : vk::DescriptorType::eSampledImage,
                         .pImageInfo = &infos[i]};
        }
        struct {
            s32 size[2];
            float threshold;
            u32 flags;
        } params{{s32(w), s32(h)}, threshold, compare ? 1u : 0u};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *error_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *error_layout, 0, writes);
        cmdbuf.pushConstants(*error_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
                             &params);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});

        // BB_FRAMEGEN_DENSITY: the share of set pixels within 2 that keeps a pixel (0.35).
        static const float density = [] {
            const char* env = std::getenv("BB_FRAMEGEN_DENSITY");
            return env && env[0] ? std::clamp(float(std::atof(env)), 0.0f, 1.0f) : 0.35f;
        }();
        const vk::DescriptorImageInfo clean_infos[2] = {
            {{}, *raw_mask_view, vk::ImageLayout::eGeneral},
            {{}, *mask_views[mask_index], vk::ImageLayout::eGeneral},
        };
        const std::array<vk::WriteDescriptorSet, 2> clean_writes{{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &clean_infos[0]},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageImage,
             .pImageInfo = &clean_infos[1]},
        }};
        struct {
            s32 size[2];
            s32 radius;
            float density;
        } clean_params{{s32(w), s32(h)}, 2, density};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *clean_pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *clean_layout, 0,
                                    clean_writes);
        cmdbuf.pushConstants(*clean_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(clean_params), &clean_params);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    }
    // 2. This frame, kept for the next.
    runtime.Transit(&source, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    {
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::ImageMemoryBarrier2 to_dst{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = vk::Image(previous_color),
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
        const vk::ImageCopy region{
            .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .extent = {w, h, 1},
        };
        cmdbuf.copyImage(source.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                         vk::Image(previous_color), vk::ImageLayout::eTransferDstOptimal, region);
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = vk::Image(previous_color),
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    }
    previous_color_valid = true;
    runtime.Transit(&source, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
}

void CtrFsr3::CaptureHudless(VideoCore::Image& display) {
    {
        std::scoped_lock lock{framegen_mutex};
        // Only for the frame just published and not taken yet, once.
        if (!framegen_ready || framegen.display_hudless) {
            return;
        }
    }
    const vk::Format format = display.info.pixel_format;
    const vk::Extent2D size{display.info.size.width, display.info.size.height};
    if (display.info.num_samples > 1 || display.info.props.is_depth || size.width == 0 ||
        size.height == 0) {
        return;
    }
    // Viewed by the presenter in the format it views the display buffer in (sRGB or not, RGBA or
    // BGRA): every 32-bit colour format of its class. CTR's two display buffers differ in that
    // (one RGBA8 unorm, one sRGB): a copy of the class's own format serves both (a copy between
    // them keeps the bytes). Remade for every frame it crashed the GPU: the new image came back
    // with the old one's handle, and views made for the old one were used on it (code 23).
    static constexpr std::array<vk::Format, 4> views{
        vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb, vk::Format::eB8G8R8A8Unorm,
        vk::Format::eB8G8R8A8Srgb};
    const bool rgba8 = std::find(views.begin(), views.end(), format) != views.end();
    const vk::Format copy_format = rgba8 ? vk::Format::eR8G8B8A8Unorm : format;
    if (!hudless_copy || hudless_copy_format != copy_format || hudless_copy_size != size) {
        if (hudless_copy) {
            scheduler.DeferOperation([old = std::move(hudless_copy)] {});
        }
        ++hudless_copy_generation;
        const vk::ImageFormatListCreateInfo list{.viewFormatCount = u32(views.size()),
                                                 .pViewFormats = views.data()};
        hudless_copy = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        hudless_copy.Create(vk::ImageCreateInfo{
            .pNext = rgba8 ? &list : nullptr,
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = copy_format,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        hudless_copy_format = copy_format;
        hudless_copy_size = size;
        if (!hudless_copy) {
            return;
        }
        std::printf("Frame generation: HUD-less copy of the display buffer %ux%u (%s)\n",
                    size.width, size.height, vk::to_string(copy_format).c_str());
        std::fflush(stdout);
    }
    // The first draw's rendering ends here (the next draw resumes it).
    scheduler.EndRendering();
    runtime.Transit(&display, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    constexpr vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 to_dst{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image(hudless_copy),
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {size.width, size.height, 1},
    };
    cmdbuf.copyImage(display.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                     vk::Image(hudless_copy), vk::ImageLayout::eTransferDstOptimal, region);
    const vk::ImageMemoryBarrier2 to_general{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(hudless_copy),
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
    std::scoped_lock lock{framegen_mutex};
    if (framegen_ready && !framegen.display_hudless) {
        framegen.display_hudless = vk::Image(hudless_copy);
        framegen.display_hudless_id = hudless_copy_generation;
        framegen.display_hudless_format = copy_format;
        framegen.display_hudless_size = size;
    }
}

bool CtrFsr3::TakeFrameGenInputs(FrameGenInputs& out) {
    std::scoped_lock lock{framegen_mutex};
    if (!framegen_ready) {
        return false;
    }
    out = framegen;
    framegen_ready = false;
    return true;
}

vk::ImageView CtrFsr3::Upscale(VideoCore::ImageId source_id, vk::Format view_format,
                               vk::ComponentMapping mapping, vk::Extent2D out,
                               vk::ImageLayout& layout) {
    {
        // A frame without the upscaler (a menu, a load) breaks the interpolated sequence.
        std::scoped_lock lock{framegen_mutex};
        if (!framegen_published) {
            framegen_reset = true;
        }
        framegen_published = false;
    }
    const bool scene = scene_this_frame;
    scene_this_frame = false;
    camera_this_frame = false;
    if (!Enabled()) {
        reset = true;
        return {};
    }
    if (!scene || !source_id || !scene_depth || !velocity) {
        Skip("no 3D scene this frame");
        return {};
    }
    VideoCore::Image* depth_ptr = texture_cache.TryGetImage(scene_depth, scene_depth_uid);
    VideoCore::Image* vel_ptr = texture_cache.TryGetImage(velocity, velocity_uid);
    constexpr auto registered = VideoCore::ImageFlagBits::Registered;
    if (!depth_ptr || !vel_ptr || False(depth_ptr->flags & registered) ||
        False(vel_ptr->flags & registered)) {
        Skip("the G-buffer was released");
        return {};
    }
    auto& source = texture_cache.GetImage(source_id);
    auto& depth = *depth_ptr;
    auto& vel = *vel_ptr;
    const u32 w = source.info.size.width, h = source.info.size.height;
    // The finished frame, the depth and the velocity share the render size; FSR 3 does not
    // downscale.
    if (depth.info.size.width != w || depth.info.size.height != h ||
        vel.info.size.width != w || vel.info.size.height != h || out.width < w ||
        out.height < h || w * 4 < out.width ||
        std::abs(double(w) / h - double(out.width) / out.height) > 0.02 ||
        !depth.info.props.is_depth) {
        static u32 logged = 0;
        if (logged++ < 4) {
            std::printf("FSR 3: not used: frame %ux%u, depth %ux%u, velocity %ux%u, output %ux%u\n",
                        w, h, depth.info.size.width, depth.info.size.height, vel.info.size.width,
                        vel.info.size.height, out.width, out.height);
        }
        Skip("sizes do not match");
        return {};
    }
    const u32 variant = (depth.info.num_samples > 1 ? 1u : 0u) | (vel.info.num_samples > 1 ? 2u : 0u);
    // Inside the display pass's draw: its rendering ends here (and resumes when it draws).
    scheduler.EndRendering();
    const vk::Format frame_format = Linear(source.info.pixel_format);
    if (frame_format != vk::Format::eR8G8B8A8Unorm && frame_format != vk::Format::eB8G8R8A8Unorm) {
        Skip("the frame is not 8-bit RGBA");
        return {};
    }
    if (!EnsureResources(w, h, out.width, out.height, frame_format)) {
        failed = true;
        std::printf("FSR 3: off for this session\n");
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = 16.6f;
    if (last_frame.time_since_epoch().count() != 0) {
        frame_ms = std::clamp(
            std::chrono::duration<float, std::milli>(now - last_frame).count(), 1.0f, 100.0f);
        if (frame_ms >= 100.0f) {
            reset = true;  // a pause, a load, a menu
        }
    }
    last_frame = now;

    // DLSS when it is chosen and can run (an RTX GPU, the bridge and nvngx_dlss.dll next to the
    // game); FSR 3 otherwise. A new DLSS feature is made with the GPU idle, before this frame's
    // commands.
    bool use_dlss = false;
    if (BbSettings::Get().temporal == BbSettings::TemporalDlss && !dlss_failed) {
        auto* dlss = DlssNgx::Get();
        if (dlss && dlss->IsAvailable()) {
            // The model: NVIDIA's transformer presets (the DLL's defaults, K/L/M by mode) or the
            // CNN one (preset E, deprecated by NVIDIA but still in DLSS 310). BB_DLSS_PRESET
            // picks any preset number.
            static const char* preset_env = std::getenv("BB_DLSS_PRESET");
            const u32 preset = preset_env ? u32(std::atoi(preset_env))
                               : BbSettings::Get().dlss_model == 1 ? 5u : 0u;
            const DlssNgx::FeatureDesc desc{w, h, out.width, out.height,
                                            DlssNgx::QualityForScale(float(out.width) / float(w)),
                                            false, preset, false};
            if (!dlss->HasFeature(desc)) {
                scheduler.Finish();
                if (dlss->CreateFeature(scheduler.CommandBuffer(), desc)) {
                    reset = true;
                    std::printf("DLSS: %ux%u -> %ux%u, quality %d, preset %u\n", w, h, out.width,
                                out.height, desc.quality, preset);
                } else {
                    dlss_failed = true;
                    std::printf("DLSS: the feature could not be created; FSR 3 from now on\n");
                }
                std::fflush(stdout);
            }
            use_dlss = !dlss_failed;
        } else if (!dlss_warned) {
            dlss_warned = true;
            std::printf("DLSS: not available (%s); FSR 3 instead\n",
                        dlss ? dlss->Problem().c_str()
                             : "sacredfire_dlss.dll or nvngx_dlss.dll is not next to the game");
            std::fflush(stdout);
        }
    }
    if (use_dlss != last_dlss) {
        last_dlss = use_dlss;
        reset = true;
        std::printf("Temporal upscaling: %s\n", use_dlss ? "DLSS" : "FSR 3");
    }

    const bool upscaler_reset = reset; // for frame generation (the dispatch clears it)
    // Frame generation samples the upscaled frame the way the display pass does, without alpha.
    const auto publish = [&] {
        vk::ComponentMapping opaque = mapping;
        opaque.a = vk::ComponentSwizzle::eOne;
        PublishFrameGen(OutputView(view_format, opaque), out, frame_ms, upscaler_reset);
    };
    runtime.Transit(&source, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&depth, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.Transit(&vel, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();

    // 1. Inputs: sample 0 of the depth and the motion.
    const int dump_frame = DumpFrame();
    const bool dump = dump_frame >= 0;
    const int debug = DebugMode();
    const auto& decode = MotionDecode();
    const vk::ImageView depth_src =
        SourceView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth, 0);
    const vk::ImageView vel_src =
        SourceView(vel, Linear(vel.info.pixel_format), vk::ImageAspectFlagBits::eColor, 1);
    {
        // This frame's moving-objects mask goes to the other image than the last frame's.
        mask_index ^= 1u;
        const vk::DescriptorImageInfo infos[6] = {
            {{}, depth_src, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, vel_src, vk::ImageLayout::eShaderReadOnlyOptimal},
            {{}, *depth_view, vk::ImageLayout::eGeneral},
            {{}, *motion_view, vk::ImageLayout::eGeneral},
            {{}, *debug_view, vk::ImageLayout::eGeneral},
            {{}, *mask_views[mask_index], vk::ImageLayout::eGeneral},
        };
        std::array<vk::WriteDescriptorSet, 6> writes{};
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
        // The camera: previous projection * previous view * inverse current view (floats 48, 64
        // and 32 of the G-buffer constants).
        bool camera_ok = false;
        if (camera_valid && decode.camera && viewport.xscale != 0.0f && viewport.yscale != 0.0f &&
            viewport.zscale != 0.0f) {
            const Mat4 view = Load(&camera[32]), proj = Load(&camera[48]),
                       prev_view = Load(&camera[64]);
            Mat4 inverse{};
            if (Invert(view, inverse)) {
                const Mat4 m = Multiply(Multiply(proj, prev_view), inverse);
                constexpr int rows[3] = {0, 1, 3};
                for (int r = 0; r < 3; ++r) {
                    for (int col = 0; col < 4; ++col) {
                        params.prev[r][col] = float(m[col * 4 + rows[r]]);
                    }
                }
                params.proj[0] = camera[48];
                params.proj[1] = camera[48 + 5];
                params.proj[2] = camera[48 + 10];
                params.proj[3] = camera[48 + 14];
                camera_ok = params.proj[0] != 0.0f && params.proj[1] != 0.0f;
            }
        }
        params.viewport[0] = viewport.xscale;
        params.viewport[1] = viewport.yscale;
        params.viewport[2] = viewport.xoffset;
        params.viewport[3] = viewport.yoffset;
        params.depth_jitter[0] = viewport.zscale;
        params.depth_jitter[1] = viewport.zoffset;
        params.depth_jitter[2] = frame_jitter[0];
        params.depth_jitter[3] = frame_jitter[1];
        params.object[0] = decode.sx;
        params.object[1] = decode.sy;
        params.object[2] = decode.bias;
        params.object[3] = float(decode.mode);
        params.flags = decode.cx | (decode.cy << 2) | (u32(dump ? 3 : std::clamp(debug, 0, 15)) << 4) |
                       (dump || debug > 0 ? 0x100u : 0u) | (camera_ok ? 0x200u : 0u);
        if (!camera_ok && !camera_warned) {
            camera_warned = true;
            std::printf("FSR 3: no camera this frame: world motion left out\n");
        }
        const vk::MemoryBarrier2 before{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[variant]);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
                             sizeof(params), &params);
        cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
        const vk::MemoryBarrier2 after{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    }
    if (CtrFrameGen::Wanted()) {
        ErrorMask(source, w, h);
    }
    if (dump) {
        Dump(cmdbuf, dump_frame, source);
        runtime.Transit(&source, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    if (debug > 0) {
        // The inputs instead of the upscaled frame (stretched by the display pass).
        layout = vk::ImageLayout::eGeneral;
        return *debug_view;
    }

    // 2. DLSS: its inputs in shader-read layout, the output in General.
    if (use_dlss) {
        const auto transition = [&](vk::Image image, vk::ImageLayout from, vk::ImageLayout to) {
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
        };
        const auto ro = vk::ImageLayout::eShaderReadOnlyOptimal, general = vk::ImageLayout::eGeneral;
        const std::array to_read{transition(vk::Image(depth_image), general, ro),
                                 transition(vk::Image(motion_image), general, ro)};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(to_read.size()),
                                 .pImageMemoryBarriers = to_read.data()});
        constexpr vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const DlssNgx::Resource color{source.GetImage(),
                                      SourceView(source, source.info.pixel_format,
                                                 vk::ImageAspectFlagBits::eColor, 2),
                                      color_range, source.info.pixel_format, {w, h}};
        const DlssNgx::Resource depth_res{vk::Image(depth_image), *depth_view, color_range,
                                          vk::Format::eR32Sfloat, {w, h}};
        const DlssNgx::Resource motion_res{vk::Image(motion_image), *motion_view, color_range,
                                           vk::Format::eR16G16Sfloat, {w, h}};
        const DlssNgx::Resource output_res{vk::Image(output_image), *output_storage_view,
                                           color_range, output_format, {out.width, out.height}};
        const float sign = std::getenv("BB_DLSS_JITTER_FLIP") ? -1.0f : 1.0f;
        const DlssNgx::EvalDesc eval{sign * frame_jitter[0], sign * frame_jitter[1], reset,
                                     frame_ms};
        const bool ok = DlssNgx::Get()->Evaluate(cmdbuf, color, depth_res, motion_res, output_res,
                                                eval);
        scheduler.GetDynamicState().Invalidate(); // NGX records its own Vulkan state
        const std::array back{transition(vk::Image(depth_image), ro, general),
                              transition(vk::Image(motion_image), ro, general),
                              transition(vk::Image(output_image), general, general)};
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = u32(back.size()),
                                 .pImageMemoryBarriers = back.data()});
        if (!ok) {
            dlss_failed = true;
            std::printf("DLSS: evaluation failed; FSR 3 from now on\n");
            std::fflush(stdout);
            return {};
        }
        reset = false;
        if (last_skip) {
            last_skip = nullptr;
            std::printf("DLSS: upscaling again\n");
        }
        publish();
        layout = vk::ImageLayout::eGeneral;
        return OutputView(view_format, mapping);
    }

    // 3. FSR 3.
    float near_plane, far_plane, fov;
    CameraPlanes(near_plane, far_plane, fov);
    const auto& settings = BbSettings::Get();
    FfxVkPortableUpscaleDispatchInfo info{};
    info.structSize = sizeof(info);
    info.commandBuffer = cmdbuf;
    info.color = Describe(source.GetImage(), source.info.pixel_format, w, h, source.usage_flags,
                          vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
    const auto inputs = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                        vk::ImageUsageFlagBits::eTransferSrc;
    info.depth = Describe(vk::Image(depth_image), vk::Format::eR32Sfloat, w, h, inputs,
                          vk::ImageAspectFlagBits::eColor,
                          FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.motionVectors = Describe(vk::Image(motion_image), vk::Format::eR16G16Sfloat, w, h, inputs,
                                  vk::ImageAspectFlagBits::eColor,
                                  FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    info.output = Describe(vk::Image(output_image), output_format, out.width,
                           out.height,
                           vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                               vk::ImageUsageFlagBits::eTransferSrc,
                           vk::ImageAspectFlagBits::eColor,
                           FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    info.exposure.structSize = sizeof(info.exposure);
    info.reactiveMask.structSize = sizeof(info.reactiveMask);
    info.transparencyAndCompositionMask.structSize = sizeof(info.transparencyAndCompositionMask);
    const float sign = std::getenv("BB_FSR3_JITTER_FLIP") ? -1.0f : 1.0f;
    info.jitterOffset = {sign * frame_jitter[0], sign * frame_jitter[1]};
    info.motionVectorScale = {1.0f, 1.0f};
    info.renderSize = {w, h};
    info.outputSize = {out.width, out.height};
    info.frameTimeMilliseconds = frame_ms;
    info.preExposure = 1.0f;
    info.cameraNear = near_plane;
    info.cameraFar = far_plane;
    info.cameraVerticalFovRadians = fov;
    info.viewSpaceToMeters = 0.01f;
    info.sharpness = std::clamp(settings.fsr3_sharpness.load(), 0.0f, 1.0f);
    info.enableSharpening = info.sharpness > 0.0f ? VK_TRUE : VK_FALSE;
    info.reset = reset ? VK_TRUE : VK_FALSE;
    info.frameId = frame_id++;

    FfxVkPortableUpscaleCreateInfo create_info{};
    create_info.structSize = sizeof(create_info);
    create_info.maxRenderSize = {width, height};
    create_info.maxOutputSize = {out_width, out_height};
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&create_info, &info)) {
        PrintIssues("dispatch", issues);
        failed = true;
        return {};
    }
    if (ffxVkPortableUpscaleContextRecordDispatch(context, &info) != FFX_VK_PORTABLE_OK) {
        std::printf("FSR 3: dispatch failed; off for this session\n");
        failed = true;
        return {};
    }
    scheduler.GetDynamicState().Invalidate(); // FSR records its own Vulkan state
    reset = false;
    if (last_skip) {
        last_skip = nullptr;
        std::printf("FSR 3: upscaling again\n");
    }
    const vk::MemoryBarrier2 done{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
    if (!announced) {
        announced = true;
        std::printf("FSR 3: upscaling %ux%u -> %ux%u (near %.2f, far %.0f, fov %.1f deg; depth %ux, "
                    "velocity %ux samples)\n",
                    w, h, out.width, out.height, near_plane, far_plane, fov * 57.29578f,
                    depth.info.num_samples, vel.info.num_samples);
    }
    publish();
    layout = vk::ImageLayout::eGeneral;
    return OutputView(view_format, mapping);
}

void CtrFsr3::Dump(vk::CommandBuffer cmdbuf, int frame, VideoCore::Image& source) {
    static const std::string dir = [] {
        const char* env = std::getenv("BB_FSR3_DUMP_DIR");
        return std::string{env && env[0] ? env : "."};
    }();
    // The frame FSR 3 reads, for matching moving objects between frames.
    runtime.Transit(&source, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    struct Item {
        vk::Image image;
        vk::ImageLayout layout;
        const char* name;
        std::string format;
    };
    const Item items[] = {
        {vk::Image(depth_image), vk::ImageLayout::eGeneral, "depth", "r32f"},
        {vk::Image(debug_image), vk::ImageLayout::eGeneral, "velocity", "rgba8"},
        {vk::Image(motion_image), vk::ImageLayout::eGeneral, "motion", "rg16f"},
        {source.GetImage(), vk::ImageLayout::eTransferSrcOptimal, "color",
         vk::to_string(source.info.pixel_format)},
    };
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    for (const auto& item : items) {
        const VkDeviceSize size = VkDeviceSize(width) * height * 4;
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer buffer{};
        VmaAllocation allocation{};
        VmaAllocationInfo alloc_info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                            &alloc_info) != VK_SUCCESS) {
            continue;
        }
        const vk::BufferImageCopy region{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyImageToBuffer(item.image, item.layout, buffer, region);
        char path[512];
        std::snprintf(path, sizeof(path), "%s/fsr3_f%03d_%s_%ux%u_%s.raw", dir.c_str(), frame,
                      item.name, width, height, item.format.c_str());
        scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation,
                                          alloc_info, size, file = std::string{path}] {
            vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
            if (FILE* f = std::fopen(file.c_str(), "wb")) {
                std::fwrite(alloc_info.pMappedData, 1, size, f);
                std::fclose(f);
            }
            vmaDestroyBuffer(allocator, buffer, allocation);
        });
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
    };
    cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after});
    std::printf("FSR 3 dump %d (%s), jitter %.4f %.4f, viewport scale %g %g offset %g %g, depth "
                "%g + %g ndc, camera constants%s:\n",
                frame, dir.c_str(), frame_jitter[0], frame_jitter[1], viewport.xscale,
                viewport.yscale, viewport.xoffset, viewport.yoffset, viewport.zoffset,
                viewport.zscale, camera_valid ? "" : " (none)");
    for (u32 row = 0; camera_valid && row < camera.size() / 4; ++row) {
        std::printf("  [%3u] %12g %12g %12g %12g\n", row * 4, camera[row * 4], camera[row * 4 + 1],
                    camera[row * 4 + 2], camera[row * 4 + 3]);
    }
    std::fflush(stdout);
}

} // namespace Vulkan
