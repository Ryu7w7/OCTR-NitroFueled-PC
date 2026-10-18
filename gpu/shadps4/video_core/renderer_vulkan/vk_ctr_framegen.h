// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire: FSR 3 frame generation for CTR Nitro-Fueled (docs/sacredfire/15-frame-generation.md).
//
// CtrFsr3 publishes, for each frame it upscales (FSR 3 or DLSS), the frame before the HUD, the
// depth, the motion and the camera. The presenter's post-processing pass draws that HUD-less
// frame into an image of the same size and format as the real frame; FSR 3's frame interpolation
// (FireBurn/FSR-Vulkan's portable API: optical flow plus the game's motion) then makes the frame
// halfway between the previous real frame and this one, with the HUD taken from the real frame.
// The presenter shows it first and the real frame half a frame later.

#pragma once

#include <array>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_ctr_fsr3.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableFrameGenerationContext;

namespace Vulkan {

class Instance;
struct Frame;

class CtrFrameGen {
public:
    CtrFrameGen(const Instance& instance, u32 frame_count);
    ~CtrFrameGen();

    /// The setting (bbport.ini frame_generation, BB_FRAMEGEN=1/0) is on and frame generation
    /// has not failed in this session.
    [[nodiscard]] bool Enabled() const;
    /// The setting alone (for the renderer's HUD-less copy).
    [[nodiscard]] static bool Wanted();

    /// Records, after the real frame's post-processing into `frame` (General layout): the
    /// HUD-less frame through the same pass, then the interpolated frame into Generated(frame)
    /// (General layout). False when there is none for this frame (the caller presents only the
    /// real one).
    /// `view_format`: the format the presenter samples the display buffer in (for the HUD-less
    /// copy, sampled the same way).
    bool Record(vk::CommandBuffer cmdbuf, Frame& frame, vk::Format frame_format,
                vk::Format view_format, const CtrFsr3::FrameGenInputs& in,
                HostPasses::PostProcessingPass& pp,
                HostPasses::PostProcessingPass::Settings pp_settings);

    /// The interpolated frame of `frame`'s slot (R8G8B8A8 unorm, the frame's size).
    [[nodiscard]] vk::Image Generated(const Frame& frame) const;

    /// A frame without interpolation (a menu, a load, the setting off): the next one restarts.
    void Break() {
        broken = true;
    }

private:
    struct Slot {
        u32 width = 0, height = 0;
        vk::Format format{};
        VideoCore::UniqueImage hudless;
        vk::UniqueImageView hudless_view;
        VideoCore::UniqueImage generated;
        vk::UniqueImageView generated_view; ///< storage view for ComposeMovingObjects
        /// A view of the HUD-less display buffer copy in the presenter's view format.
        vk::Image source{};
        u64 source_id = 0; ///< FrameGenInputs::display_hudless_id the view was made for
        vk::Format source_format{};
        vk::UniqueImageView source_view;
    };
    bool EnsureContext(vk::Extent2D render, vk::Extent2D display, vk::Format format);
    bool EnsureSlot(Slot& slot, u32 width, u32 height, vk::Format format);
    void Fail(const char* what);
    void RetireOld();
    /// The moving objects (karts) from the real frame instead of interpolated (no ghosting).
    void ComposeMovingObjects(vk::CommandBuffer cmdbuf, Frame& frame, Slot& slot,
                              const CtrFsr3::FrameGenInputs& in);

    const Instance& instance;
    std::vector<Slot> slots;
    FfxVkPortableFrameGenerationContext* context = nullptr;
    vk::Extent2D context_render{}, context_display{};
    vk::Format context_format{};
    /// Contexts replaced after a resize, destroyed once the frames in flight cannot use them:
    /// (context, the record count after which it goes).
    std::vector<std::pair<FfxVkPortableFrameGenerationContext*, u64>> retired;
    u64 records = 0;
    u64 frame_id = 0;
    u64 last_sequence = 0;
    bool broken = true;
    bool failed = false;
    bool announced = false;

    vk::UniqueDescriptorSetLayout compose_desc_layout;
    vk::UniquePipelineLayout compose_layout;
    vk::UniquePipeline compose_pipeline;
};

} // namespace Vulkan
