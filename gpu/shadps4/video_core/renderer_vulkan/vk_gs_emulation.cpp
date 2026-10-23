// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_gs_emulation.h"

#include <cstdio>
#include <cstring>

#include <vk_mem_alloc.h>
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

namespace {

constexpr u32 ParamSlots = 4096;
constexpr u32 ParamBytes = 48; // emit_spirv_gs_emulation.cpp, ParamsStride
constexpr u64 DataVec4s = u64{4} << 20; // 64 MiB: a few frames of every emulated draw
constexpr u64 IndexWords = u64{4} << 20; // 16 MiB of 32-bit indices

bool CreateBuffer(const Instance& instance, vk::DeviceSize size, bool host, vk::Buffer& buffer,
                  VmaAllocation& allocation, void** mapped, u64& address) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    buffer = raw;
    if (mapped) {
        *mapped = info.pMappedData;
    }
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return address != 0;
}

} // namespace

GsEmulationBuffers::GsEmulationBuffers(const Instance& instance_) : instance{instance_} {
    if (instance.IsGeometryStageSupported()) {
        return;
    }
    if (!instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics) {
        std::printf("Geometry shaders: no geometry stage and no vertex stores, their draws are "
                    "skipped\n");
        return;
    }
    void* mapped = nullptr;
    u64 params_address = 0, data_address = 0;
    void* index_memory = nullptr;
    if (!CreateBuffer(instance, vk::DeviceSize(ParamSlots) * ParamBytes, true, params_buffer,
                      params_allocation, &mapped, params_address) ||
        !CreateBuffer(instance, vk::DeviceSize(DataVec4s) * 16, false, data_buffer,
                      data_allocation, nullptr, data_address) ||
        !CreateBuffer(instance, vk::DeviceSize(IndexWords) * 4, true, index_buffer,
                      index_allocation, &index_memory, index_address)) {
        std::printf("Geometry shaders: emulation buffers not created, their draws are skipped\n");
        return;
    }
    params_mapped = static_cast<u32*>(mapped);
    index_mapped = static_cast<u32*>(index_memory);
    std::memset(params_mapped, 0, ParamBytes);
    Shader::GsEmulation::params_address = params_address;
    Shader::GsEmulation::data_address = data_address;
    enabled = true;
    std::printf("Geometry shaders: emulated with vertex shaders (no geometry stage)\n");
}

GsEmulationBuffers::~GsEmulationBuffers() {
    const auto allocator = instance.GetAllocator();
    if (params_buffer) {
        vmaDestroyBuffer(allocator, params_buffer, params_allocation);
    }
    if (data_buffer) {
        vmaDestroyBuffer(allocator, data_buffer, data_allocation);
    }
    if (index_buffer) {
        vmaDestroyBuffer(allocator, index_buffer, index_allocation);
    }
}

u32 GsEmulationBuffers::Prepare(const Draw& draw) {
    if (!enabled || draw.entries == 0 || draw.stride == 0 || draw.slots == 0) {
        return 0;
    }
    const u64 vec4s = u64(draw.entries) * draw.stride;
    if (vec4s > DataVec4s / 4 || (draw.index_size && draw.count > IndexWords / 4)) {
        return 0;
    }
    u64 indices_at = 0;
    if (draw.index_size) {
        if (next_index + draw.count > IndexWords) {
            next_index = 0; // as the data ring: those draws have long finished
        }
        u32* out = index_mapped + next_index;
        if (draw.index_size == 2) {
            const auto* in = static_cast<const u16*>(draw.indices);
            for (u32 i = 0; i < draw.count; ++i) {
                out[i] = in[i];
            }
        } else {
            std::memcpy(out, draw.indices, size_t(draw.count) * 4);
        }
        indices_at = index_address + next_index * 4;
        next_index += draw.count;
    }
    if (next_vec4 + vec4s > DataVec4s) {
        next_vec4 = 0; // the ring wraps: the draws a full ring ago have long finished
    }
    const u32 base = u32(next_vec4);
    next_vec4 += vec4s;
    const u32 slot = next_param;
    next_param = next_param + 1 < ParamSlots ? next_param + 1 : 1;
    u32* p = params_mapped + slot * (ParamBytes / 4);
    p[0] = base;
    p[1] = u32(draw.entry_bias);
    p[2] = draw.index_size ? 4u : 0u; // copied as 32-bit
    p[3] = draw.slots;
    p[4] = u32(indices_at);
    p[5] = u32(indices_at >> 32);
    p[6] = draw.stride;
    p[7] = u32(draw.es_bias);
    p[8] = draw.entries;
    p[9] = p[10] = p[11] = 0;
    return slot;
}

} // namespace Vulkan
