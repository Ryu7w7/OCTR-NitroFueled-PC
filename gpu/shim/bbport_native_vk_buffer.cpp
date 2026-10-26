// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk_buffer.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace BbNativeVk::Buf {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 WindowBits = 24; // 16 MiB

struct Window {
    u64 start = 0, end = 0;
    VkBuffer buffer = VK_NULL_HANDLE; // VK_NULL_HANDLE: import failed (not tried again)
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

const Vulkan::Instance* instance = nullptr;
bool supported = false;
u64 alignment = 4096;
std::vector<Window*> windows_list;
std::unordered_map<u64, std::vector<Window*>> buckets; // 16 MiB index -> windows touching it
u64 imported_bytes = 0, failed_windows = 0, lookups = 0, hits = 0;

/// The committed, readable host region holding `address`: [start, end). False when there is none.
bool Region(u64 address, u64* start, u64* end) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }
    *start = reinterpret_cast<u64>(mbi.BaseAddress);
    *end = *start + mbi.RegionSize;
    return true;
#else
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        u64 s = 0, e = 0;
        char dash = 0;
        std::istringstream is(line);
        is >> std::hex >> s >> dash >> e;
        if (address >= s && address < e) {
            *start = s;
            *end = e;
            return true;
        }
    }
    return false;
#endif
}

/// Imports [start, end) (aligned) as one VkBuffer.
bool ImportWindow(Window& w) {
    const vk::Device dev = instance->GetDevice();
    void* ptr = reinterpret_cast<void*>(w.start);
    const u64 size = w.end - w.start;
    auto [pr, props] = dev.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, ptr);
    if (pr != vk::Result::eSuccess || !props.memoryTypeBits) {
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo ext{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT};
    const vk::BufferCreateInfo bci{
        .pNext = &ext,
        .size = size,
        .usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    auto [br, buffer] = dev.createBuffer(bci);
    if (br != vk::Result::eSuccess) {
        return false;
    }
    const vk::MemoryRequirements req = dev.getBufferMemoryRequirements(buffer);
    const u32 bits = req.memoryTypeBits & props.memoryTypeBits;
    if (!bits || req.size > size) {
        dev.destroyBuffer(buffer);
        return false;
    }
    const vk::ImportMemoryHostPointerInfoEXT import{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = ptr};
    const vk::MemoryAllocateInfo mai{
        .pNext = &import, .allocationSize = size, .memoryTypeIndex = u32(std::countr_zero(bits))};
    auto [mr, memory] = dev.allocateMemory(mai);
    if (mr != vk::Result::eSuccess) {
        dev.destroyBuffer(buffer);
        return false;
    }
    if (dev.bindBufferMemory(buffer, memory, 0) != vk::Result::eSuccess) {
        dev.freeMemory(memory);
        dev.destroyBuffer(buffer);
        return false;
    }
    w.buffer = buffer;
    w.memory = memory;
    return true;
}
} // namespace

void Init(const Vulkan::Instance& vulkan) {
    instance = &vulkan;
    supported = vulkan.IsExternalMemoryHostSupported();
    if (supported) {
        const auto chain = vulkan.GetPhysicalDevice()
                               .getProperties2<vk::PhysicalDeviceProperties2,
                                               vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>();
        alignment = std::max<u64>(
            chain.get<vk::PhysicalDeviceExternalMemoryHostPropertiesEXT>().minImportedHostPointerAlignment,
            4096);
    }
    std::printf("Native Vulkan: guest memory import %s (alignment %llu)\n",
                supported ? "available" : "not available: vertex/index data will be copied",
                (unsigned long long)alignment);
}

Slice Import(std::uint64_t address, std::uint64_t size) {
    ++lookups;
    if (!supported || !size) {
        return {};
    }
    const u64 last = address + size - 1;
    for (const Window* w : buckets[address >> WindowBits]) {
        if (address >= w->start && last < w->end) {
            if (!w->buffer) {
                return {};
            }
            ++hits;
            return {w->buffer, address - w->start};
        }
    }
    // a new window: the 16 MiB block(s) around the data, clipped to its committed region
    u64 rs = 0, re = 0;
    auto w = new Window{};
    if (!Region(address, &rs, &re) || last >= re) {
        w->start = address & ~(alignment - 1);
        w->end = (last + alignment) & ~(alignment - 1);
    } else {
        w->start = std::max(address & ~((u64(1) << WindowBits) - 1), rs) & ~(alignment - 1);
        w->end = (std::min((last >> WindowBits << WindowBits) + (u64(1) << WindowBits), re) +
                  alignment - 1) & ~(alignment - 1);
        if (!ImportWindow(*w) && failed_windows < 8) {
            std::printf("Native Vulkan: guest memory %llx-%llx (region %llx-%llx) not importable\n",
                        (unsigned long long)w->start, (unsigned long long)w->end,
                        (unsigned long long)rs, (unsigned long long)re);
        }
    }
    if (w->buffer) {
        imported_bytes += w->end - w->start;
    } else {
        ++failed_windows;
    }
    windows_list.push_back(w);
    for (u64 b = w->start >> WindowBits; b <= (w->end - 1) >> WindowBits; ++b) {
        buckets[b].push_back(w);
    }
    if (!w->buffer) {
        return {};
    }
    ++hits;
    return {w->buffer, address - w->start};
}

Stats GetStats() {
    return Stats{supported, alignment, windows_list.size() - failed_windows, imported_bytes,
                 failed_windows, lookups, hits};
}
} // namespace BbNativeVk::Buf
