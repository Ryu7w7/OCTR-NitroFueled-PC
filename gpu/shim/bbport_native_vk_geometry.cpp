// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk_geometry.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers

#include <vk_mem_alloc.h>

#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#include "bbport_native_vk_buffer.h"
#include "bbport_native_vk_gpu.h"

namespace BbNativeVk::Geo {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 PageBits = 12; // the page manager's pages
constexpr u64 PageSize = u64(1) << PageBits;
constexpr u64 MinSize = 16 << 10;         // smaller buffers are copied at each draw
constexpr u64 Budget = 1536ull << 20;     // resident bytes at most
constexpr u64 EvictAfter = 900;           // submissions without a draw using it
constexpr u64 RewriteWindow = 120;        // rewrites closer than this (submissions) add up...
constexpr u32 VolatileAfter = 3;          // ...to a buffer that stays copied at each draw
constexpr u64 CollectEvery = 64;          // submissions between eviction passes

enum class State : u8 {
    Candidate, // watched, not copied yet: resident if nothing writes it until a later submission
    Resident,
    Volatile,  // written again and again: copied at each draw
    Unusable,  // not all readable, or not GPU memory (no write faults there)
};

struct Entry {
    u64 base = 0, size = 0;
    State state = State::Candidate;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    u64 seen = 0;                  // submission it became a candidate in
    u64 last_used = ~u64(0);       // submission of the last draw using it
    u64 last_rewrite = 0;
    u32 rewrites = 0;
    bool gone = false;             // its memory was unmapped: made again at the next use
    std::vector<u64> dirty;        // a bit per page from base's page: the copy is out of date
    u64 dirty_pages = 0;

    u64 FirstPage() const {
        return base >> PageBits;
    }
    u64 Pages() const {
        return ((base + size - 1) >> PageBits) - FirstPage() + 1;
    }
    bool Dirty(u64 i) const {
        return dirty[i >> 6] >> (i & 63) & 1;
    }
    void SetDirty(u64 i) {
        if (!Dirty(i)) {
            dirty[i >> 6] |= u64(1) << (i & 63);
            ++dirty_pages;
        }
    }
    void ClearDirty(u64 i) {
        if (Dirty(i)) {
            dirty[i >> 6] &= ~(u64(1) << (i & 63));
            --dirty_pages;
        }
    }
};

/// A page some entry was copied from. Unwatched pages are dirty in all their entries.
struct Page {
    bool watched = false;
    std::vector<Entry*> entries;
};

bool on = false;
VmaAllocator allocator = nullptr;
Vulkan::Rasterizer* rasterizer = nullptr;

// the write hook comes from the faulting thread while a draw may be uploading; recursive: a
// fault taken while the lock is held (reading guest memory) comes back to it on the same thread
std::recursive_mutex lock;
std::unordered_map<u64, Entry*> entries; // by base and size
std::unordered_map<u64, Page> pages;     // by page number
std::atomic<bool> any_pages{false};
u64 resident_bytes = 0, last_collect = 0;

std::atomic<u64> n_buffers{0}, n_bytes{0}, n_candidates{0}, n_volatile{0}, n_unusable{0},
    n_over_budget{0}, n_uploaded{0}, n_reuploads{0}, n_resident_uses{0}, n_busy_uses{0},
    n_writes{0}, n_evicted{0};

u64 Key(u64 base, u64 size) {
    return base ^ (size * 0x9e3779b97f4a7c15ull);
}

/// Watches pages [first, first + count) not watched yet, in runs.
void Watch(u64 first, u64 count) {
    u64 run = 0, run_start = 0;
    auto flush = [&] {
        if (run) {
            rasterizer->WatchWrites(run_start << PageBits, run << PageBits, true);
            run = 0;
        }
    };
    for (u64 p = first; p < first + count; ++p) {
        Page& page = pages[p];
        if (page.watched) {
            flush();
            continue;
        }
        page.watched = true;
        if (!run) {
            run_start = p;
        }
        ++run;
    }
    flush();
}

/// The guest wrote (or unmapped) pages [first, last]: each one watched stops being watched (the
/// write goes ahead), and is dirty in every entry copied from it.
void Written(u64 first, u64 last) {
    u64 run = 0, run_start = 0;
    auto flush = [&] {
        if (run) {
            rasterizer->WatchWrites(run_start << PageBits, run << PageBits, false);
            run = 0;
        }
    };
    auto hit = [&](u64 p, Page& page) {
        if (!page.watched) {
            flush();
            return;
        }
        page.watched = false;
        n_writes.fetch_add(1, std::memory_order_relaxed);
        for (Entry* e : page.entries) {
            e->SetDirty(p - e->FirstPage());
        }
        if (run && run_start + run != p) {
            flush();
        }
        if (!run) {
            run_start = p;
        }
        ++run;
    };
    if (last - first + 1 > pages.size()) { // a large range (an unmap): the pages there are fewer
        std::vector<u64> found;
        for (const auto& [p, page] : pages) {
            if (p >= first && p <= last && page.watched) {
                found.push_back(p);
            }
        }
        std::sort(found.begin(), found.end());
        for (const u64 p : found) {
            hit(p, pages[p]);
        }
    } else {
        for (u64 p = first; p <= last; ++p) {
            if (const auto it = pages.find(p); it != pages.end()) {
                hit(p, it->second);
            } else {
                flush();
            }
        }
    }
    flush();
}

void OnWritten(VAddr addr, u64 size) {
    if (!size || !any_pages.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lk{lock};
    Written(addr >> PageBits, (addr + size - 1) >> PageBits);
}

void OnUnmapped(VAddr addr, u64 size) {
    if (!size || !any_pages.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lk{lock};
    Written(addr >> PageBits, (addr + size - 1) >> PageBits);
    for (auto& [key, e] : entries) {
        if (e->base < addr + size && addr < e->base + e->size) {
            e->gone = true;
        }
    }
}

/// Takes `e` off its pages; pages no entry is on any more stop being watched.
void Unregister(Entry* e) {
    const u64 first = e->FirstPage(), count = e->Pages();
    for (u64 p = first; p < first + count; ++p) {
        const auto it = pages.find(p);
        if (it == pages.end()) {
            continue;
        }
        auto& list = it->second.entries;
        std::erase(list, e);
        if (list.empty()) {
            if (it->second.watched) {
                rasterizer->WatchWrites(p << PageBits, PageSize, false);
            }
            pages.erase(it);
        }
    }
    any_pages.store(!pages.empty(), std::memory_order_release);
}

void FreeBuffer(Entry* e) {
    if (!e->buffer) {
        return;
    }
    resident_bytes -= e->size;
    n_buffers.fetch_sub(1, std::memory_order_relaxed);
    n_bytes.store(resident_bytes, std::memory_order_relaxed);
    Gpu::Defer([buffer = e->buffer, allocation = e->allocation] {
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
    e->buffer = VK_NULL_HANDLE;
    e->allocation = nullptr;
}

/// Written too often to keep a copy of: the draws copy it.
void MakeVolatile(Entry* e) {
    if (e->state == State::Candidate) {
        n_candidates.fetch_sub(1, std::memory_order_relaxed);
    }
    FreeBuffer(e);
    Unregister(e);
    e->state = State::Volatile;
    n_volatile.fetch_add(1, std::memory_order_relaxed);
}

/// Copies the entry's pages [i, j) (indices from its first page) from guest memory into its
/// buffer, watching them first (a write from now on marks them dirty again).
bool UploadPages(Entry* e, u64 i, u64 j) {
    Watch(e->FirstPage() + i, j - i);
    for (u64 k = i; k < j; ++k) {
        e->ClearDirty(k);
    }
    const u64 from = std::max(e->base, (e->FirstPage() + i) << PageBits);
    const u64 to = std::min(e->base + e->size, (e->FirstPage() + j) << PageBits);
    for (u64 at = from; at < to;) {
        const u64 n = std::min<u64>(to - at, 32ull << 20);
        const Gpu::Span span = Gpu::Upload(n, 16);
        if (!span.data) {
            for (u64 k = i; k < j; ++k) {
                e->SetDirty(k); // tried again at the next use
            }
            return false;
        }
        std::memcpy(span.data, reinterpret_cast<const void*>(at), n);
        const vk::BufferCopy region{.srcOffset = span.offset, .dstOffset = at - e->base, .size = n};
        Gpu::Prelude().copyBuffer(span.buffer, e->buffer, region);
        n_uploaded.fetch_add(n, std::memory_order_relaxed);
        at += n;
    }
    return true;
}

/// Uploads every dirty page of the entry, in runs.
bool UploadDirty(Entry* e) {
    const u64 count = e->Pages();
    for (u64 i = 0; i < count && e->dirty_pages;) {
        if (!e->Dirty(i)) {
            ++i;
            continue;
        }
        u64 j = i + 1;
        while (j < count && e->Dirty(j)) {
            ++j;
        }
        if (!UploadPages(e, i, j)) {
            return false;
        }
        i = j;
    }
    return true;
}

/// A candidate that stayed unwritten: its buffer, then all of it uploaded.
bool Promote(Entry* e) {
    if (resident_bytes + e->size > Budget) {
        n_over_budget.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = e->size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateBuffer(allocator, &bci, &aci, &e->buffer, &e->allocation, nullptr) != VK_SUCCESS) {
        e->buffer = VK_NULL_HANDLE;
        n_over_budget.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    resident_bytes += e->size;
    n_buffers.fetch_add(1, std::memory_order_relaxed);
    n_bytes.store(resident_bytes, std::memory_order_relaxed);
    n_candidates.fetch_sub(1, std::memory_order_relaxed);
    e->state = State::Resident;
    // the pages are watched and clean since it became a candidate: memory is as it was then
    if (!UploadPages(e, 0, e->Pages())) {
        FreeBuffer(e);
        e->state = State::Candidate;
        n_candidates.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

/// Frees all the entry holds (not the entry).
void Drop(Entry* e) {
    switch (e->state) {
    case State::Candidate:
        n_candidates.fetch_sub(1, std::memory_order_relaxed);
        Unregister(e);
        break;
    case State::Resident:
        FreeBuffer(e);
        Unregister(e);
        break;
    case State::Volatile:
        n_volatile.fetch_sub(1, std::memory_order_relaxed);
        break;
    case State::Unusable:
        n_unusable.fetch_sub(1, std::memory_order_relaxed);
        break;
    }
}

/// A new entry: watched from now on, all of it clean.
void Begin(Entry* e, u64 now) {
    if (!Buf::Readable(e->base, e->size) || !rasterizer->IsMapped(e->base, e->size)) {
        e->state = State::Unusable;
        n_unusable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 first = e->FirstPage(), count = e->Pages();
    e->dirty.assign((count + 63) / 64, 0);
    for (u64 p = first; p < first + count; ++p) {
        pages[p].entries.push_back(e);
    }
    any_pages.store(true, std::memory_order_release);
    Watch(first, count);
    e->seen = now;
    n_candidates.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

void Init(const Vulkan::Instance& instance) {
    const char* env = std::getenv("BB_VK_RESIDENT");
    on = !(env && env[0] == '0');
    allocator = instance.GetAllocator();
    if (on) {
        Vulkan::Rasterizer::native_written = OnWritten;
        Vulkan::Rasterizer::native_unmapped = OnUnmapped;
    }
    std::printf("Native Vulkan: vertex and index buffers of %llu KiB and more %s\n",
                (unsigned long long)(MinSize >> 10),
                on ? "stay on the GPU, uploaded again where the game writes them "
                     "(BB_VK_RESIDENT=0: copied at each draw)"
                   : "are copied at each draw (BB_VK_RESIDENT=0)");
}

Binding Use(std::uint64_t base, std::uint64_t size) {
    if (!on || size < MinSize || !base) {
        return {};
    }
    if (!rasterizer) {
        rasterizer = Core::Memory::Instance()->GetRasterizer();
        if (!rasterizer) {
            return {};
        }
    }
    std::scoped_lock lk{lock};
    const u64 now = Gpu::Serial();
    Entry*& slot = entries[Key(base, size)];
    if (slot && slot->gone) {
        Drop(slot);
        delete slot;
        slot = nullptr;
    }
    if (!slot) {
        slot = new Entry{.base = base, .size = size};
        Begin(slot, now);
        slot->last_used = now;
        return {};
    }
    Entry* e = slot;
    if (e->base != base || e->size != size) {
        return {}; // another buffer with the same key
    }
    const u64 last_used = e->last_used;
    e->last_used = now;
    switch (e->state) {
    case State::Volatile:
    case State::Unusable:
        return {};
    case State::Candidate:
        if (now == e->seen) {
            return {}; // not unwritten across a submission yet
        }
        if (e->dirty_pages) {
            // written since: a candidate again from now, or copied for good
            if (++e->rewrites >= 2 || !rasterizer->IsMapped(base, size)) {
                MakeVolatile(e);
                return {};
            }
            Watch(e->FirstPage(), e->Pages());
            for (u64 i = 0, n = e->Pages(); i < n; ++i) {
                e->ClearDirty(i);
            }
            e->seen = now;
            return {};
        }
        if (!Promote(e)) {
            return {};
        }
        e->rewrites = 0;
        break;
    case State::Resident:
        if (e->dirty_pages) {
            if (last_used == now) {
                // this submission draws from the copy already: this draw copies its data
                n_busy_uses.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
            e->rewrites = now - e->last_rewrite < RewriteWindow ? e->rewrites + 1 : 1;
            e->last_rewrite = now;
            if (e->rewrites >= VolatileAfter || !rasterizer->IsMapped(base, size)) {
                MakeVolatile(e);
                return {};
            }
            n_reuploads.fetch_add(1, std::memory_order_relaxed);
            if (!UploadDirty(e)) {
                return {};
            }
        }
        break;
    }
    n_resident_uses.fetch_add(1, std::memory_order_relaxed);
    return {e->buffer, 0};
}

void Collect() {
    if (!on) {
        return;
    }
    const u64 now = Gpu::Serial();
    if (now - last_collect < CollectEvery) {
        return;
    }
    last_collect = now;
    std::scoped_lock lk{lock};
    for (auto it = entries.begin(); it != entries.end();) {
        Entry* e = it->second;
        if (e->last_used != ~u64(0) && now - e->last_used < EvictAfter) {
            ++it;
            continue;
        }
        Drop(e);
        n_evicted.fetch_add(1, std::memory_order_relaxed);
        delete e;
        it = entries.erase(it);
    }
}

Stats GetStats() {
    auto l = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    return Stats{on, l(n_buffers), l(n_bytes), l(n_candidates), l(n_volatile), l(n_unusable),
                 l(n_over_budget), l(n_uploaded), l(n_reuploads), l(n_resident_uses),
                 l(n_busy_uses), l(n_writes), l(n_evicted)};
}
} // namespace BbNativeVk::Geo
