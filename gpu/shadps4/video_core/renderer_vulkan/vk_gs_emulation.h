// SPDX-License-Identifier: GPL-2.0-or-later

// bbport: geometry shader draws without a geometry shader stage (MoltenVK has none). A draw runs
// as an export pass (the export stage stores each vertex's outputs, rasterizer discard on) and an
// expansion pass (the geometry stage as a vertex shader, one invocation per output vertex slot of
// each primitive, drawn as a triangle list). The shader side is
// shader_recompiler/backend/spirv/emit_spirv_gs_emulation.cpp; Shader::GsEmulation holds the
// buffer addresses the shaders embed.

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"
#include "common/types.h"

using VmaAllocation = struct VmaAllocation_T*;

namespace Vulkan {

class Instance;

class GsEmulationBuffers {
public:
    explicit GsEmulationBuffers(const Instance& instance);
    ~GsEmulationBuffers();

    [[nodiscard]] bool Enabled() const {
        return enabled;
    }

    struct Draw {
        u32 entries;      ///< vertices the export pass stores
        u32 stride;       ///< vec4s per vertex (ESGS ring item size / 4)
        s32 es_bias;      ///< export pass: entry = vertex index + es_bias
        s32 entry_bias;   ///< expansion pass: entry = index value (or vertex number) + entry_bias
        u32 index_size;   ///< 0 (not indexed), 2, 4
        const void* indices; ///< the draw's indices (guest memory), copied as 32-bit
        u32 count;           ///< indices
        u32 slots; ///< output vertex slots per primitive
    };
    /// The parameter slot for push constant motion_param, or 0 when the draw does not fit.
    u32 Prepare(const Draw& draw);

private:
    const Instance& instance;
    bool enabled = false;
    vk::Buffer params_buffer{};
    VmaAllocation params_allocation{};
    u32* params_mapped = nullptr;
    vk::Buffer data_buffer{};
    VmaAllocation data_allocation{};
    // The indices are copied here: MoltenVK does not make the buffer cache's stream buffer
    // resident for device address loads (kIOGPUCommandBufferCallbackErrorInvalidResource).
    vk::Buffer index_buffer{};
    VmaAllocation index_allocation{};
    u32* index_mapped = nullptr;
    u64 index_address = 0;
    u64 next_index = 0;
    u32 next_param = 1;
    u64 next_vec4 = 0;
};

} // namespace Vulkan
