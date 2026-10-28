// SPDX-License-Identifier: GPL-2.0-or-later
// bbport lab: BbMem (see bbport_native_memory.h).
#include "bbport_native_memory.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <boost/icl/interval_set.hpp>

#include "core/memory.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
void runtime_memory_set_note_write_hook(RuntimeGpuRange note);
void runtime_memory_set_cpu_write_hook(RuntimeGpuRange hook);
}

namespace BbMem {
namespace {
std::shared_mutex ranges_mutex;
boost::icl::interval_set<VAddr> mapped;
std::unique_ptr<VideoCore::PageManager> pages;
bool installed = false;

bool MappedNow(VAddr addr, u64 size) {
    if (size == 0 || addr > ~VAddr(0) - size) {
        return false;
    }
    std::shared_lock lk(ranges_mutex);
    return boost::icl::contains(mapped, boost::icl::interval<VAddr>::right_open(addr, addr + size));
}

void Written(VAddr addr, u64 size) {
    if (auto hook = Vulkan::Rasterizer::native_written; hook && MappedNow(addr, size)) {
        hook(addr, size);
    }
}

bool OnWriteFault(VAddr addr) {
    if (!MappedNow(addr, 8)) {
        return false; // not GPU memory: another handler's fault
    }
    // the native renderer takes its watch off the written pages (Tex::Written, Geo::Written)
    if (auto hook = Vulkan::Rasterizer::native_written) {
        hook(addr, 8);
    }
    return true;
}

const VideoCore::PageOwner owner{&OnWriteFault, &MappedNow};
} // namespace

bool NativeOnly() {
    static const bool on = [] {
        const char* env = std::getenv("BB_VK_NATIVE_ONLY");
        return env && env[0] == '1';
    }();
    return on;
}

void Install() {
    static std::once_flag once;
    std::call_once(once, [] {
        pages = std::make_unique<VideoCore::PageManager>(&owner);
        installed = true;
        // the runtime replays the mappings made so far
        runtime_memory_set_gpu_hooks(
            [](uintptr_t address, uint64_t size) {
                {
                    std::unique_lock lk(ranges_mutex);
                    mapped += boost::icl::interval<VAddr>::right_open(address, address + size);
                }
                pages->OnGpuMap(address, size);
            },
            [](uintptr_t address, uint64_t size) {
                if (auto hook = Vulkan::Rasterizer::native_unmapped) {
                    hook(address, size);
                }
                std::unique_lock lk(ranges_mutex);
                mapped -= boost::icl::interval<VAddr>::right_open(address, address + size);
            },
            [](uintptr_t address, uint64_t size) { Written(address, size); });
        // data written by paths the GPU side hears of (file reads, resource loaders, libc copies)
        runtime_memory_set_note_write_hook([](uintptr_t address, uint64_t size) { Written(address, size); });
        runtime_memory_set_cpu_write_hook([](uintptr_t address, uint64_t size) { Written(address, size); });
        std::printf("Native memory: BB_VK_NATIVE_ONLY=1: guest memory tracked by the native renderer "
                    "(no shadPS4 Rasterizer)\n");
    });
}

void NoteWrite(std::uint64_t addr, std::uint64_t size) {
    if (installed) {
        Written(addr, size);
    }
}

bool Ready() {
    return installed || Core::Memory::Instance()->GetRasterizer() != nullptr;
}

bool IsMapped(std::uint64_t addr, std::uint64_t size) {
    if (installed) {
        return MappedNow(addr, size);
    }
    auto* r = Core::Memory::Instance()->GetRasterizer();
    return r && r->IsMapped(addr, size);
}

void Watch(std::uint64_t addr, std::uint64_t size, bool watch) {
    if (installed) {
        if (watch) {
            pages->UpdatePageWatchers<true>(addr, size);
        } else {
            pages->UpdatePageWatchers<false>(addr, size);
        }
        return;
    }
    if (auto* r = Core::Memory::Instance()->GetRasterizer()) {
        r->WatchWrites(addr, size, watch);
    }
}
} // namespace BbMem
