// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: resident vertex and index data for the native Vulkan renderer (bbport_native_vk.cpp).
//
// The engine's vertex and index buffers (records {u32 size; u64 address}) of at least 16 KiB get
// a device-local copy, made once and kept current from write tracking: the pages a copy was made
// from are watched (write faults, through shadPS4's page manager, beside its own caches), and a
// write marks them dirty, so only those pages are uploaded again, at the next draw that uses the
// buffer. Uploads are captured with the draws and go through the upload ring into the prelude
// command buffer, run before the submission's draws.
//
// A buffer is copied only once it has stayed unwritten from one submission to a later one: data
// the game writes every frame (rings, particles) stays on the per-draw copy path, as does a
// resident buffer rewritten often (volatile), and one rewritten while the open submission
// already draws from it (that draw gets its own copy).
//
// BB_VK_RESIDENT=0 turns it off (every draw copies its vertices and indices).
//
// Use is called under the renderer's lock; the write and unmap hooks come from any thread.
#pragma once

#include <cstdint>

#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace Vulkan {
class Instance;
}

namespace BbNativeVk::Geo {
/// Where a resident buffer is; buffer VK_NULL_HANDLE: copy the data for this draw.
struct Binding {
    VkBuffer buffer = VK_NULL_HANDLE;
    std::uint64_t offset = 0;
};

struct Stats {
    bool on;
    std::uint64_t buffers, bytes, candidates, volatile_buffers, unusable, over_budget;
    std::uint64_t uploaded_bytes, reuploads, resident_uses, busy_uses, writes_heard, evicted;
};

/// What Geo needs from the renderer: the game-side submission number, and uploads and frees
/// queued in order with the draws (recorded later on the recorder thread).
struct Host {
    std::uint64_t (*serial)();
    void (*upload)(const void* src, std::uint64_t size, VkBuffer dst, std::uint64_t dst_offset);
    void (*free)(VkBuffer buffer, VmaAllocation allocation);
};

void Init(const Vulkan::Instance& instance, const Host& host);

/// The engine buffer [base, base + size) on the GPU, uploads of it recorded first.
Binding Use(std::uint64_t base, std::uint64_t size);

/// Once per submission: frees buffers no draw used for a while.
void Collect();

/// The guest wrote / unmapped [addr, addr + size) (the renderer's hooks, any thread).
void Written(std::uint64_t addr, std::uint64_t size);
void Unmapped(std::uint64_t addr, std::uint64_t size);

Stats GetStats();
} // namespace BbNativeVk::Geo
