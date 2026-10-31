// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire: temporal upscaling for CTR Nitro-Fueled (docs/sacredfire/14-fsr3.md):
// FSR 3.1, NVIDIA DLSS or AMD FSR 4 (v07 INT8) at the finished frame.
//
// bbport's TemporalUpscaler is built around Bloodborne's frame. CTR's is simpler to feed: its
// G-buffer pass (four RGBA8 targets and the scene depth, 2x MSAA) also writes a velocity target
// for the game's motion blur. This upscaler takes the finished frame where the display pass
// stretches it (the FSR 1 spot, before the HUD), the scene depth and that velocity, and gives
// the display pass an output-size image instead. Scene draws (the ones with the scene depth that
// are not full-screen passes) are jittered by a sub-pixel viewport offset.
//
// FSR 4 takes linear FP16: the frame is decoded into one (fsr4_color.comp), upscaled, encoded
// back (ENCODE) and packed into the 8-bit output (ctr_fsr4_pack.comp).

#pragma once

#include <array>
#include <chrono>
#include <mutex>
#include <thread>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableUpscaleContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;

class CtrFsr3 {
public:
    CtrFsr3(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
            Runtime& runtime);
    ~CtrFsr3();

    /// The setting is on and FSR 3 has not failed in this session.
    [[nodiscard]] bool Enabled() const;

    /// The G-buffer pass's viewport transform: pixels = offset + scale * ndc, depth = zoffset +
    /// zscale * ndc z.
    struct Viewport {
        float xscale = 0, yscale = 0, xoffset = 0, yoffset = 0, zscale = 0, zoffset = 0;
    };
    /// A pass with the G-buffer's targets (four colors and depth): remembers the scene depth, the
    /// velocity target and the viewport of this frame.
    void OnGBufferPass(VideoCore::ImageId depth, VideoCore::ImageId velocity,
                       const Viewport& viewport);
    /// The vertex constants of the first G-buffer draw of a frame (528 bytes: view, projection
    /// and the previous frame's view at floats 32, 48 and 64).
    void OnCameraConstants(const float* data);
    [[nodiscard]] bool WantsCameraConstants() const {
        return Enabled() && !camera_this_frame;
    }

    /// The first draw of a display pass: the scene of this frame is finished.
    void OnFrameEnd();

    /// The viewport offset, in pixels, of a draw into `depth` (zero for other draws).
    [[nodiscard]] std::array<float, 2> DrawJitter(VideoCore::ImageId depth, bool full_screen) const;

    /// Upscales `source` (the finished frame the display pass samples through a view of
    /// `view_format`) to `out`. Returns the view to sample instead and its layout, or a null view
    /// when FSR 3 does not apply (no scene this frame, sizes that do not match).
    vk::ImageView Upscale(VideoCore::ImageId source, vk::Format view_format,
                          vk::ComponentMapping mapping, vk::Extent2D out, vk::ImageLayout& layout);

private:
    bool EnsureResources(u32 w, u32 h, u32 ow, u32 oh, vk::Format format);
    /// The FSR 3 context for the current sizes (see context_retry_in).
    bool EnsureContext();
    void CreatePipelines();
    void EnsureFsr4Pipelines();
    /// True when the FSR 4 backend for this size is already built. Never builds.
    bool Fsr4BackendReady(int preset, u32 ow, u32 oh);
    /// Starts the background backend build (FSR 3 meanwhile). No-op while one runs.
    void Fsr4KickPreload(int preset, u32 ow, u32 oh);
    vk::ImageView SourceView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect, int slot);
    vk::ImageView OutputView(vk::Format format, vk::ComponentMapping mapping);
    /// Logs why a frame is not upscaled, once per change of reason.
    void Skip(const char* reason);
    const char* last_skip = nullptr;
    void Dump(vk::CommandBuffer cmdbuf, int frame, VideoCore::Image& source);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;

    bool failed = false;
    /// The working images exist for (width, height) -> (out_width, out_height).
    bool resources_ready = false;
    /// The FSR 3 context is made the first time FSR 3 itself runs (DLSS and FSR 4 never
    /// need it). Creation can fail on a transient memory spike (streaming at a level
    /// start): FSR 1 meanwhile, and another try after this many FSR 3 frames instead of
    /// FSR 3 being off for the session (dispatch failures stay sticky).
    u32 context_retry_in = 0;
    bool announced = false;
    VideoCore::ImageId scene_depth{}, velocity{};
    u64 scene_depth_uid = 0, velocity_uid = 0;
    bool scene_this_frame = false;
    bool camera_this_frame = false;
    std::array<float, 132> camera{};
    bool camera_valid = false;
    bool camera_warned = false;
    Viewport viewport{};

    std::array<float, 2> jitter{};      ///< scene draws from now on
    std::array<float, 2> frame_jitter{}; ///< the scene that just finished
    u32 jitter_index = 0;
    u32 jitter_phases = 8;
    bool reset = true;
    u64 frame_id = 0;
    std::chrono::steady_clock::time_point last_frame{};

    FfxVkPortableUpscaleContext* context = nullptr;
    u32 width = 0, height = 0, out_width = 0, out_height = 0;
    vk::Format output_format = vk::Format::eUndefined; ///< the frame's own format (linear)
    VideoCore::UniqueImage depth_image, motion_image, debug_image, output_image;
    vk::UniqueImageView depth_view, motion_view, debug_view;
    struct OutputViewEntry {
        vk::Format format{};
        vk::ComponentMapping mapping{};
        vk::UniqueImageView view;
    };
    std::array<OutputViewEntry, 4> output_views{};
    u32 output_view_next = 0;
    struct SourceViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::UniqueImageView view;
    };
    std::array<SourceViewEntry, 3> source_views{}; ///< depth, velocity, the frame (DLSS)
    vk::UniqueImageView output_storage_view;       ///< DLSS writes the output through it
    bool dlss_failed = false, dlss_warned = false, last_dlss = false;
    /// FSR 4 (temporal=fsr4): linear FP16 working images, the v07 backend and the
    /// decode/encode/pack pipelines (fsr4_decode/encode_comp, ctr_fsr4_pack_comp).
    Fsr4Upscaler fsr4;
    VideoCore::UniqueImage fsr4_linear_image, fsr4_output_image;
    vk::UniqueImageView fsr4_linear_view, fsr4_output_view;
    vk::UniqueDescriptorSetLayout fsr4_io_layout;
    vk::UniquePipelineLayout fsr4_io_pipeline_layout;
    vk::UniquePipeline fsr4_decode_pipeline, fsr4_encode_pipeline, fsr4_pack_pipeline;
    bool fsr4_failed = false, fsr4_warned = false, last_fsr4 = false;
    /// Background FSR 4 backend build (file reads + driver compiles): FSR 3 runs
    /// meanwhile, the switch happens with reset when the worker finishes. The worker
    /// owns the FSR 4 backend until it is joined; the frame path must not touch it.
    std::thread fsr4_preload_thread;
    std::mutex fsr4_preload_mutex;
    bool fsr4_preloading = false, fsr4_preload_done = false, fsr4_preload_ok = false;
    int fsr4_preload_preset = 0;
    u32 fsr4_preload_ow = 0, fsr4_preload_oh = 0;
    /// The backend currently built (by preload or a frame), if any.
    bool fsr4_built = false;
    int fsr4_built_preset = 0;
    u32 fsr4_built_ow = 0, fsr4_built_oh = 0;

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    /// By multisampling: bit 0 the depth, bit 1 the velocity.
    std::array<vk::UniquePipeline, 4> pipelines;
};

} // namespace Vulkan
