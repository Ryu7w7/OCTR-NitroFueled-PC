// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire: from IFreemz/shadPS4-Bloodborne-DLSS-FSR (vk_dlss_ngx), one process-wide instance
// (DlssNgx::Get) and the bridge named sacredfire_dlss.dll.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// Loads the optional DLSS bridge (sacredfire_dlss.dll, dlss_bridge/) next to the executable. The
// port itself contains no NVIDIA code; without the bridge (or on other GPUs) nothing changes.
// Callers own synchronization and must drain recorded work before releasing a feature.
class DlssNgx {
public:
    /// The process's instance when the bridge and nvngx_dlss.dll are present (and BB_DLSS is not
    /// 0), else null. Created on first use: before the Vulkan instance.
    static DlssNgx* Get();
    ~DlssNgx();

    void AppendInstanceExtensions(std::vector<const char*>& enabled);
    /// Device extensions NGX needs that `enabled` lacks; their strings live as long as this.
    std::vector<const char*> DeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                              const std::vector<std::string>& already);
    void Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
    void Shutdown();
    bool IsAvailable() const;
    /// Why DLSS cannot run, or empty.
    const std::string& Problem() const;

    struct Resource {
        vk::Image image;
        vk::ImageView view;
        vk::ImageSubresourceRange range;
        vk::Format format;
        vk::Extent2D extent;
    };

    // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance.
    static int QualityForScale(float output_over_input);
    struct FeatureDesc {
        u32 input_width, input_height, output_width, output_height;
        int quality;
        bool depth_inverted;
        u32 preset; // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = the DLL's default
        bool hdr;   // linear HDR colour (automatic exposure)
    };
    // Releases an existing feature first; the caller must have drained the GPU if one exists.
    bool CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc);
    bool HasFeature(const FeatureDesc& desc) const;
    struct EvalDesc {
        float jitter_x, jitter_y; // render pixels, same space as the motion vectors
        bool reset;
        float frame_ms;
    };
    // Inputs in shader-read layout (sampled), output in General (storage).
    bool Evaluate(vk::CommandBuffer command, const Resource& color, const Resource& depth,
                  const Resource& motion, const Resource& output, const EvalDesc& eval);
    void ReleaseFeatureAfterGpuDrain();

private:
    DlssNgx();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
