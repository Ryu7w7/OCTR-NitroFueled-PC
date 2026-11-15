// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what the Direct3D 11 renderer asks of its host (glue.h).
#include "glue.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>
#include <unordered_map>

#include "../bbport_native_memory.h"
#include "core/libraries/kernel/process.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
extern "C" const uint64_t* runtime_memory_generation(void);

u64 g_image_base = 0, g_image_size = 0;

namespace {
constexpr u64 kPage = 0x1000;

struct Page {
    bool watched = false, written = false;
    u64 first = 0, last = 0;                    // the range it was watched for
    std::shared_ptr<std::atomic<bool>> target;  // the render target here, if any
};

// Guest pages whose GPU copies go stale on a write: the page is write protected until the first
// write, which marks it (the write itself goes on), and watched again when the copy is refreshed.
std::mutex g_pages_mutex;
std::unordered_map<u64, Page> g_pages;
std::set<u64> g_written_pages;  // the pages with `written` set: a check looks here, not page by page
std::atomic<u64> g_writes{1};

// Consecutive pages in one call.
struct WatchRun {
    bool on;
    u64 start = 0, end = 0;
    void Add(u64 page) {
        if (start && page == end) {
            end += kPage;
            return;
        }
        Flush();
        start = page;
        end = page + kPage;
    }
    void Flush() {
        if (start) {
            BbMem::Watch(start, end - start, on);
        }
        start = 0;
    }
};

void Written(Page& page, u64 p, WatchRun& run) {
    if (page.watched) {
        page.watched = false;
        run.Add(p);
    }
    page.written = true;
    g_written_pages.insert(p);
    if (page.target) {
        page.target->store(true, std::memory_order_release);
    }
}

// The first write takes the watch off everything watched with that page at once: a video frame
// costs one fault, not one per page, as the native Vulkan renderer's watch.
void OnWritten(u64 addr, u64 size) {
    const u64 first = addr & ~(kPage - 1), last = (addr + size - 1) & ~(kPage - 1);
    std::lock_guard lk(g_pages_mutex);
    WatchRun run{false};
    for (u64 p = first; p <= last; p += kPage) {
        auto it = g_pages.find(p);
        if (it == g_pages.end() || !it->second.watched) {
            continue;
        }
        const u64 from = it->second.first, to = it->second.last;
        for (u64 q = from; q <= to; q += kPage) {
            if (auto other = g_pages.find(q); other != g_pages.end() && other->second.watched) {
                Written(other->second, q, run);
            }
        }
        g_writes.fetch_add(1, std::memory_order_relaxed);
    }
    run.Flush();
}

void OnUnmapped(u64 addr, u64 size) {
    const u64 first = addr & ~(kPage - 1), last = (addr + size - 1) & ~(kPage - 1);
    std::lock_guard lk(g_pages_mutex);
    WatchRun run{false};
    bool any = false;
    for (u64 p = first; p <= last; p += kPage) {
        // what was there is gone, and what comes at that address is something else (the next
        // track where the last one was): stale, not forgotten
        if (auto it = g_pages.find(p); it != g_pages.end()) {
            Written(it->second, p, run);
            any = true;
        }
    }
    run.Flush();
    if (any) {
        g_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

// As the write watch of the port this came from: a range is watched only while it is mapped.
void Arm(u64 addr, u64 size) {
    const u64 first = addr & ~(kPage - 1), last = (addr + size - 1) & ~(kPage - 1);
    WatchRun run{true};
    for (u64 p = first; p <= last; p += kPage) {
        Page& page = g_pages[p];
        if (page.written) {
            page.written = false;
            g_written_pages.erase(p);
        }
        page.first = first;
        page.last = last;
        if (!page.watched && BbMem::IsMapped(p, kPage)) {
            page.watched = true;
            run.Add(p);
        }
    }
    run.Flush();
}
} // namespace

void GlueInstall(unsigned char* image, u64 image_size) {
    g_image_base = reinterpret_cast<u64>(image);
    g_image_size = image_size;
    Vulkan::Rasterizer::native_written = [](VAddr addr, u64 size) { OnWritten(addr, size); };
    Vulkan::Rasterizer::native_unmapped = [](VAddr addr, u64 size) { OnUnmapped(addr, size); };
}

int ConfigInt(const char* name, int fallback) {
    const std::string_view key{name};
    if (key == "PS4Pro") {
        const char* neo = std::getenv("BB_NEO");
        return neo && neo[0] == '1';
    }
    if (key == "RenderScale") {
        return 100; // the render settings choose the game's own size
    }
    if (key == "Anisotropy") {
        const char* v = std::getenv("BB_ANISO"); // as the native Vulkan renderer: 0 or 1, the game's
        return v ? std::atoi(v) : fallback;
    }
    if (key == "MotionBlur") {
        const char* off = std::getenv("BB_NO_MOTION_BLUR"); // patch_no_motion_blur, run.py
        return !(off && off[0] == '1');
    }
    return fallback;
}

unsigned ConfigGeneration() {
    return 0;
}

// A draw asks about some forty addresses, and the runtime's answer takes its lock and a search of
// the mapping table. Pages known to be mapped, against the table's generation (odd while it
// changes): any map, unmap or protect forgets them all. One word per page, so a reader never sees
// half an entry; the renderer calls this holding its own mutex.
namespace {
constexpr u32 kKnownPages = 1u << 14;
u64 g_known_pages[kKnownPages];

// Whole ranges too: a vertex buffer of sixteen pages is asked about at every draw of its mesh.
struct KnownRange {
    u64 addr, size, generation;
};
KnownRange g_known_ranges[1024];

bool PageMapped(u64 page, u64 generation) {
    const u64 entry = (page << 24) | (generation & 0xFFFFFF);
    u64& known = g_known_pages[(page ^ (page >> 14)) & (kKnownPages - 1)];
    if (known == entry) {
        return true;
    }
    if (!runtime_memory_is_mapped(page << 12, kPage)) {
        return false;
    }
    known = entry;
    return true;
}
} // namespace

bool GuestReadable(u64 addr, u64 size) {
    if (addr >= g_image_base && addr + size <= g_image_base + g_image_size) {
        return true;
    }
    if (addr < 0x10000) {
        return false;
    }
    const u64 generation = __atomic_load_n(runtime_memory_generation(), __ATOMIC_ACQUIRE);
    const u64 first = addr >> 12, last = (addr + (size ? size : 1) - 1) >> 12;
    if (generation & 1) {
        return runtime_memory_is_mapped(addr, size) != 0;
    }
    if (last - first > 3) {
        KnownRange& known = g_known_ranges[((addr >> 12) ^ (addr >> 22) ^ (size >> 8)) & 1023];
        if (known.addr == addr && known.size == size && known.generation == generation) {
            return true;
        }
        if (!runtime_memory_is_mapped(addr, size)) {
            return false;
        }
        known = KnownRange{addr, size, generation};
        return true;
    }
    for (u64 page = first; page <= last; page++) {
        if (!PageMapped(page, generation)) {
            return runtime_memory_is_mapped(addr, size) != 0;
        }
    }
    return true;
}

bool GuestWrittenSince(u64 addr, u64 size, bool reset) {
    const u64 first = addr & ~(kPage - 1), last = (addr + size - 1) & ~(kPage - 1);
    std::lock_guard lk(g_pages_mutex);
    const auto it = g_written_pages.lower_bound(first);
    const bool written = it != g_written_pages.end() && *it <= last;
    if (written && reset) {
        Arm(addr, size);
    }
    return written;
}

void GuestResetWritten(u64 addr, u64 size) {
    std::lock_guard lk(g_pages_mutex);
    Arm(addr, size);
}

u64 GuestWriteCount() {
    return g_writes.load(std::memory_order_acquire);
}

void GuestWatchTarget(u64 addr, u64 size, const std::shared_ptr<std::atomic<bool>>& written) {
    const u64 first = addr & ~(kPage - 1), last = (addr + size - 1) & ~(kPage - 1);
    std::lock_guard lk(g_pages_mutex);
    WatchRun run{true};
    for (u64 p = first; p <= last; p += kPage) {
        Page& page = g_pages[p];
        page.target = written;
        if (!page.watched) {
            page.first = first;
            page.last = last;
            if (BbMem::IsMapped(p, kPage)) {
                page.watched = true;
                run.Add(p);
            }
        }
    }
    run.Flush();
}

std::filesystem::path UserRoot() {
    const char* dir = std::getenv("BB_GPU_USER_DIR");
    return dir ? std::filesystem::path(dir) : std::filesystem::path("user");
}
