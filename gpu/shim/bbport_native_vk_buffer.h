// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: guest vertex and index data for the native Vulkan renderer (bbport_native_vk.cpp).
//
// The D3D11 port copies this data: large buffers into GPU buffers refreshed on guest writes,
// small ones into upload rings at every draw. Here the game's memory itself is imported as
// VkBuffers (VK_EXT_external_memory_host), in windows of up to 16 MiB around the data, so the GPU
// reads vertices and indices in place, as the PS4's GPU does: no copy and no staleness tracking.
// Memory that cannot be imported falls back to copying (counted until draws are issued).
#pragma once

#include <cstdint>

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace BbNativeVk::Buf {
/// A guest range inside an imported buffer; buffer VK_NULL_HANDLE: not importable.
struct Slice {
    VkBuffer buffer = VK_NULL_HANDLE;
    std::uint64_t offset = 0;
};

struct Stats {
    bool supported;
    std::uint64_t alignment, windows, bytes, failed_windows, lookups, hits;
};

void Init(const Vulkan::Instance& instance);

/// Guest bytes [address, address + size) as a slice of an imported window, made the first time.
Slice Import(std::uint64_t address, std::uint64_t size);

Stats GetStats();
} // namespace BbNativeVk::Buf
