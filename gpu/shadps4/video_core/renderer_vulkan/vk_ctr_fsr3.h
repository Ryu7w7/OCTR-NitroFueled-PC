// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire: FSR 3.1 temporal upscaling for CTR Nitro-Fueled (docs/sacredfire/14-fsr3.md).
//
// bbport's TemporalUpscaler is built around Bloodborne's frame. CTR's is simpler to feed: its
// G-buffer pass (four RGBA8 targets and the scene depth, 2x MSAA) also writes a velocity target
// for the game's motion blur. This upscaler takes the finished frame where the display pass
// stretches it (the FSR 1 spot, before the HUD), the scene depth and that velocity, and gives
// the display pass an output-size image instead. Scene draws (the ones with the scene depth that
// are not full-screen passes) are jittered by a sub-pixel viewport offset.

#pragma once

#include <array>
#include <chrono>
#include <mutex>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
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

    /// What frame generation (vk_presenter) needs from the frame FSR 3 or DLSS just upscaled.
    /// The images are this object's and stay in General layout; they hold this frame until the
    /// next Upscale, which comes after the presenter's commands for this frame on the same queue.
    struct FrameGenInputs {
        vk::Image hudless{};          ///< the upscaled frame before the HUD (display size)
        vk::ImageView hudless_view{}; ///< sampled like the display pass does, alpha forced to 1
        vk::Extent2D display{};
        vk::Image depth{}, motion{};  ///< R32F depth and RG16F pixel motion (render size)
        /// R8 masks of the moving objects (karts), this frame's and the previous frame's
        /// (render size, General; the previous one equals this one after a reset).
        vk::Image mask{}, previous_mask{};
        vk::ImageView mask_view{}, previous_mask_view{};
        vk::ImageView motion_view{}; ///< the motion above, sampled (General)
        vk::Extent2D render{};
        std::array<float, 2> jitter{};
        float frame_ms = 16.6f;
        float near_plane = 16.0f, far_plane = 150000.0f, fov = 1.287f;
        std::array<float, 3> position{}, up{0.0f, 1.0f, 0.0f}, right{1.0f, 0.0f, 0.0f},
            forward{0.0f, 0.0f, -1.0f};
        bool reset = true;
        u64 sequence = 0; ///< one per upscaled frame
        /// The display buffer after the display pass's first draw (the game's stretch of the
        /// upscaled frame) and before its HUD: a copy, in General layout, of the display
        /// buffer's own format. Null when the pass had no HUD draw.
        vk::Image display_hudless{};
        u64 display_hudless_id = 0; ///< changes with the image (handles can be reused)
        vk::Format display_hudless_format{};
        vk::Extent2D display_hudless_size{};
    };
    /// The inputs of the last upscaled frame not taken yet: false when there is none (no 3D scene,
    /// a debug view, FSR off).
    bool TakeFrameGenInputs(FrameGenInputs& out);

    /// The display pass's second draw (CTR's first HUD draw) is about to draw into `display`:
    /// copies the display buffer, the frame without its HUD as the presenter will see it, for
    /// frame generation (FrameGenInputs::display_hudless).
    void CaptureHudless(VideoCore::Image& display);

private:
    /// near, far and vertical FOV from the projection (floats 48-63), with CTR's defaults.
    void CameraPlanes(float& near_plane, float& far_plane, float& fov) const;
    /// Adds to this frame's moving-objects mask where the previous frame, moved by this frame's
    /// motion, does not match (the karts when the velocity target is empty, wheels, the
    /// background next to them), then keeps this frame for the next. `source` is in shader-read
    /// layout and is left so.
    void ErrorMask(VideoCore::Image& source, u32 w, u32 h);
    void PublishFrameGen(vk::ImageView hudless_view, vk::Extent2D out, float frame_ms,
                         bool upscaler_reset);
    bool EnsureResources(u32 w, u32 h, u32 ow, u32 oh, vk::Format format);
    void CreatePipelines();
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
    /// Moving objects' masks for frame generation, alternating: mask_index is this frame's.
    std::array<VideoCore::UniqueImage, 2> mask_images;
    std::array<vk::UniqueImageView, 2> mask_views;
    u32 mask_index = 0;
    bool previous_mask_valid = false;
    /// Frame generation: the previous frame (render size, before the upscaler) for the mask of
    /// what the motion does not explain (ctr_framegen_mask.comp).
    VideoCore::UniqueImage previous_color;
    vk::UniqueImageView previous_color_view;
    vk::Format previous_color_format{};
    vk::Extent2D previous_color_size{};
    bool previous_color_valid = false;
    vk::UniqueDescriptorSetLayout error_desc_layout;
    vk::UniquePipelineLayout error_layout;
    vk::UniquePipeline error_pipeline;
    /// The mask before ctr_framegen_mask_clean.comp keeps its dense parts (R8, render size).
    VideoCore::UniqueImage raw_mask;
    vk::UniqueImageView raw_mask_view;
    vk::UniqueDescriptorSetLayout clean_desc_layout;
    vk::UniquePipelineLayout clean_layout;
    vk::UniquePipeline clean_pipeline;
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
    /// depth, velocity, the frame (DLSS), the frame (frame generation's error mask)
    std::array<SourceViewEntry, 4> source_views{};
    vk::UniqueImageView output_storage_view;       ///< DLSS writes the output through it
    bool dlss_failed = false, dlss_warned = false, last_dlss = false;

    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    /// By multisampling: bit 0 the depth, bit 1 the velocity.
    std::array<vk::UniquePipeline, 4> pipelines;

    std::mutex framegen_mutex; ///< Upscale (GPU thread) and the presenter's recording thread
    FrameGenInputs framegen{};
    bool framegen_ready = false;
    bool framegen_reset = true; ///< the next published frame starts a new sequence
    bool framegen_published = false; ///< the last Upscale published its frame
    u64 framegen_sequence = 0;
    VideoCore::UniqueImage hudless_copy; ///< CaptureHudless's copy of the display buffer
    vk::Format hudless_copy_format{};
    vk::Extent2D hudless_copy_size{};
    u64 hudless_copy_generation = 0;
};

} // namespace Vulkan
