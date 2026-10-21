// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"

extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

#include <vk_mem_alloc.h>

namespace Vulkan::Breadcrumbs {

namespace {

constexpr u32 MaxStreams = 16;
constexpr u32 StreamStride = 64; // bytes of the marker buffer per stream (a cache line)
constexpr u64 RingSize = 1u << 15;
constexpr u32 ArgSlots = 4096;                     // indirect dispatch arguments, 16 bytes each
constexpr u64 ArgsOffset = MaxStreams * StreamStride; // where they start in the buffer

struct Entry {
    u64 id;
    u64 time; ///< steady clock, ns: when it was recorded
    Crumb crumb;
};

u64 Now() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

/// Images and buffers destroyed lately (NoteDestroyed), oldest overwritten first.
struct Destroyed {
    const char* what;
    u64 handle, gpu_address, size, time;
    u32 width, height, format;
};
constexpr u32 DestroyedSize = 4096;
std::mutex destroyed_mutex;
std::array<Destroyed, DestroyedSize> destroyed{};
u64 destroyed_count = 0;

/// Live images by handle (under destroyed_mutex).
struct LiveImage {
    u32 width, height, format;
    u64 size;
};
std::unordered_map<u64, LiveImage> live_images;

/// The latest destruction of `handle`, if it is still kept.
const Destroyed* FindDestroyed(u64 handle) {
    if (!handle) {
        return nullptr;
    }
    const u64 kept = std::min<u64>(destroyed_count, DestroyedSize);
    for (u64 i = 0; i < kept; ++i) {
        const Destroyed& d = destroyed[(destroyed_count - 1 - i) % DestroyedSize];
        if (d.handle == handle) {
            return &d;
        }
    }
    return nullptr;
}

double Ms(u64 from, u64 to) {
    return double(s64(to - from)) / 1e6;
}

struct Stream {
    const char* name = nullptr;
    std::atomic<u64> next{1};
    std::unique_ptr<Entry[]> ring;
};

std::array<Stream, MaxStreams> streams;
std::atomic<u32> num_streams{0};
bool enabled = false;
std::once_flag init_once;
vk::Buffer marker_buffer{};
VmaAllocation marker_allocation{};
volatile u32* markers = nullptr;
volatile u32* args = nullptr;
std::atomic<u32> next_args_slot{0};
/// What each argument slot was last used for.
struct ArgsNote {
    u64 id;
    u32 stream;
    u64 hash;
};
std::array<ArgsNote, ArgSlots> args_notes{};

vk::Device fault_device{};

/// VK_EXT_device_fault: what the driver knows about the loss (a page fault's address and kind,
/// vendor fault codes). Printed with the breadcrumbs, whether or not those are available.
void ReportFault() {
    const auto get = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT;
    if (!fault_device || !get) {
        return;
    }
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult result = get(fault_device, &counts, nullptr);
    if (result != VK_SUCCESS) {
        std::printf("GPU fault info: not available (%d)\n", int(result));
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = addresses.data(),
        .pVendorInfos = vendor.data(),
    };
    result = get(fault_device, &counts, &info);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        std::printf("GPU fault info: not available (%d)\n", int(result));
        return;
    }
    std::printf("GPU fault info: %s (%u addresses, %u vendor records)\n", info.description,
                counts.addressInfoCount, counts.vendorInfoCount);
    static const char* const kinds[] = {"none", "invalid read", "invalid write", "invalid execute",
                                        "instruction pointer unknown",
                                        "instruction pointer invalid",
                                        "instruction pointer fault"};
    const u64 now = Now();
    for (u32 i = 0; i < counts.addressInfoCount; ++i) {
        const VkDeviceFaultAddressInfoEXT& a = addresses[i];
        const u64 precision = a.addressPrecision ? a.addressPrecision : 1;
        const u64 low = a.reportedAddress & ~(precision - 1);
        std::printf("  %s at GPU address %#llx (range %#llx-%#llx)\n",
                    u32(a.addressType) < std::size(kinds) ? kinds[a.addressType] : "unknown",
                    (unsigned long long)a.reportedAddress, (unsigned long long)low,
                    (unsigned long long)(low + precision - 1));
        // A buffer freed lately that held that address: the GPU still used it.
        std::scoped_lock lock{destroyed_mutex};
        const u64 kept = std::min<u64>(destroyed_count, DestroyedSize);
        for (u64 k = 0; k < kept; ++k) {
            const Destroyed& d = destroyed[(destroyed_count - 1 - k) % DestroyedSize];
            if (d.gpu_address && low < d.gpu_address + std::max<u64>(d.size, 1) &&
                d.gpu_address <= low + precision - 1) {
                std::printf("    inside %s %#llx (GPU %#llx, %llu bytes) destroyed %.1f ms "
                            "before this report\n",
                            d.what, (unsigned long long)d.handle,
                            (unsigned long long)d.gpu_address, (unsigned long long)d.size,
                            Ms(d.time, now));
                break;
            }
        }
    }
    for (u32 i = 0; i < counts.vendorInfoCount; ++i) {
        std::printf("  vendor: %s (code %#llx, data %#llx)\n", vendor[i].description,
                    (unsigned long long)vendor[i].vendorFaultCode,
                    (unsigned long long)vendor[i].vendorFaultData);
    }
    std::fflush(stdout);
}

void Init(const Instance& instance) {
    if (instance.IsDeviceFaultSupported()) {
        fault_device = instance.GetDevice();
        std::printf("GPU fault info: VK_EXT_device_fault on, a device loss reports its cause\n");
    }
    const char* env = std::getenv("BB_BREADCRUMBS");
    if (env && env[0] == '0') {
        return;
    }
    if (!instance.IsBufferMarkerSupported()) {
        std::printf("GPU breadcrumbs: VK_AMD_buffer_marker unavailable, GPU hangs will not be "
                    "located\n");
        return;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ArgsOffset + ArgSlots * 16,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBuffer buffer{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer,
                        &marker_allocation, &info) != VK_SUCCESS ||
        !info.pMappedData) {
        std::printf("GPU breadcrumbs: no host-visible buffer, GPU hangs will not be located\n");
        return;
    }
    marker_buffer = vk::Buffer{buffer};
    markers = static_cast<volatile u32*>(info.pMappedData);
    for (u32 i = 0; i < (ArgsOffset + ArgSlots * 16) / sizeof(u32); ++i) {
        markers[i] = 0;
    }
    args = markers + ArgsOffset / sizeof(u32);
    enabled = true;
}

/// Guest memory that may be gone by now: read only where the runtime has it mapped (its direct
/// memory is a GPU driver mapping, which process_vm_readv cannot read).
size_t ReadGuest(u64 address, void* out, size_t size) {
    int prot = 0, type = 0;
    uintptr_t end = 0;
    if (!address || !runtime_memory_vma_info(address, &prot, &type, &end) || end <= address) {
        return 0;
    }
    size = std::min<size_t>(size, end - address);
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return size;
}

const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Pass:
        return "pass";
    case Kind::Draw:
        return "draw";
    case Kind::DrawIndexed:
        return "indexed draw";
    case Kind::DrawIndirect:
        return "indirect draw";
    case Kind::Dispatch:
        return "dispatch";
    case Kind::DispatchIndirect:
        return "indirect dispatch";
    case Kind::Transfer:
        return "transfer";
    case Kind::Barrier:
        return "barrier";
    }
    return "?";
}

/// A transfer's image or buffer that was destroyed after the transfer was recorded.
void PrintIfDestroyed(const char* role, u64 handle, u64 recorded, u64 now) {
    std::scoped_lock lock{destroyed_mutex};
    const Destroyed* d = FindDestroyed(handle);
    if (!d || s64(d->time - recorded) < 0) {
        return;
    }
    std::printf("      its %s (%s %#llx", role, d->what, (unsigned long long)handle);
    if (d->width) {
        std::printf(", %ux%u format %u", d->width, d->height, d->format);
    }
    std::printf(") was DESTROYED %.1f ms after this was recorded, %.1f ms before this report\n",
                Ms(recorded, d->time), Ms(d->time, now));
}

void PrintEntry(const Stream& stream, u64 id, const char* label) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        std::printf("    #%llu %s: no longer kept\n", (unsigned long long)id, label);
        return;
    }
    const Crumb& c = entry.crumb;
    std::printf("    #%llu %s: %s", (unsigned long long)id, label, KindName(c.kind));
    switch (c.kind) {
    case Kind::Pass:
        std::printf(" %s", c.name ? c.name : "?");
        break;
    case Kind::Draw:
    case Kind::DrawIndexed:
        std::printf(" vs %016llx ps %016llx, %u %s x %u instances", (unsigned long long)c.hash[0],
                    (unsigned long long)c.hash[1], c.count[0],
                    c.kind == Kind::Draw ? "vertices" : "indices", c.count[1]);
        break;
    case Kind::DrawIndirect: {
        std::printf(" vs %016llx ps %016llx, up to %u draws, stride %u, arguments at %#llx",
                    (unsigned long long)c.hash[0], (unsigned long long)c.hash[1], c.count[0],
                    c.count[1], (unsigned long long)c.address);
        u32 now[5]{};
        if (ReadGuest(c.address, now, sizeof(now)) == sizeof(now)) {
            std::printf(" (guest memory now %u %u %u %u %u)", now[0], now[1], now[2], now[3], now[4]);
        }
        break;
    }
    case Kind::Dispatch:
        std::printf(" cs %016llx, %ux%ux%u groups", (unsigned long long)c.hash[0], c.count[0],
                    c.count[1], c.count[2]);
        break;
    case Kind::DispatchIndirect: {
        std::printf(" cs %016llx, arguments at %#llx", (unsigned long long)c.hash[0],
                    (unsigned long long)c.address);
        if (c.args_slot < ArgSlots) {
            const volatile u32* read = args + c.args_slot * 4;
            std::printf(" (the GPU read %ux%ux%u groups)", read[0], read[1], read[2]);
        }
        std::printf(c.count[0] ? ", in place" : ", a copy");
        u32 now[3]{};
        if (ReadGuest(c.address, now, sizeof(now)) == sizeof(now)) {
            std::printf(" (guest memory now %ux%ux%u)", now[0], now[1], now[2]);
        }
        break;
    }
    case Kind::Transfer:
        std::printf(" %s", c.name ? c.name : "?");
        if (c.count[0]) {
            std::printf(", image %ux%u", c.count[0], c.count[1]);
            if (c.count[2]) {
                std::printf(" format %u", c.count[2]);
            }
        }
        if (c.address) {
            std::printf(", guest %#llx", (unsigned long long)c.address);
        }
        if (c.size) {
            std::printf(", %llu bytes", (unsigned long long)c.size);
        }
        std::printf(", dst %#llx", (unsigned long long)c.dst);
        if (c.dst_gpu) {
            std::printf(" (GPU %#llx)", (unsigned long long)c.dst_gpu);
        }
        if (c.src) {
            std::printf(", src %#llx", (unsigned long long)c.src);
        }
        break;
    case Kind::Barrier: {
        std::printf(", %llu image transitions%s", (unsigned long long)c.size,
                    c.count[2] ? " and a memory barrier" : "");
        if (c.size) {
            std::printf(", first layout %u -> %u, images", c.count[0], c.count[1]);
            const u64 images[4] = {c.dst, c.src, c.hash[0], c.hash[1]};
            std::scoped_lock lock{destroyed_mutex};
            for (u64 image : images) {
                if (!image) {
                    continue;
                }
                std::printf(" %#llx", (unsigned long long)image);
                if (const auto it = live_images.find(image); it != live_images.end()) {
                    std::printf(" (%ux%u format %u, %llu bytes)", it->second.width,
                                it->second.height, it->second.format,
                                (unsigned long long)it->second.size);
                } else {
                    std::printf(" (not a live image)");
                }
            }
            if (c.size > 4) {
                std::printf(" ...");
            }
        }
        break;
    }
    }
    for (u32 i = 0; i < 2 && c.kind != Kind::Barrier; ++i) {
        if (c.program[i]) {
            std::printf(", code %#llx", (unsigned long long)c.program[i]);
        }
    }
    const u64 now = Now();
    std::printf(" [recorded %.1f ms before this report]\n", Ms(entry.time, now));
    if (c.kind == Kind::Transfer) {
        PrintIfDestroyed("destination", c.dst, entry.time, now);
        PrintIfDestroyed("source", c.src, entry.time, now);
    }
    if (c.kind == Kind::Barrier) {
        const u64 images[4] = {c.dst, c.src, c.hash[0], c.hash[1]};
        for (u64 image : images) {
            PrintIfDestroyed("image", image, entry.time, now);
        }
    }
}

/// The guest code of the shaders of the stuck command, for disassembly (up to 64 KiB each:
/// the program and what follows, its OrbShdr header among it).
void DumpPrograms(const Stream& stream, u64 id) {
    const Entry& entry = stream.ring[id & (RingSize - 1)];
    if (entry.id != id) {
        return;
    }
    const char* dir = access("out", W_OK) == 0 ? "out/" : "";
    for (u32 i = 0; i < 2; ++i) {
        const u64 program = entry.crumb.program[i];
        if (!program) {
            continue;
        }
        std::array<u8, 64 * 1024> code{};
        size_t size = 0;
        // Page by page: the read stops at the first page that is not mapped.
        while (size < code.size()) {
            const size_t chunk = std::min<size_t>(4096 - ((program + size) & 4095), code.size() - size);
            const size_t read = ReadGuest(program + size, code.data() + size, chunk);
            size += read;
            if (read != chunk) {
                break;
            }
        }
        char path[256];
        std::snprintf(path, sizeof(path), "%shang_shader_%016llx.bin", dir,
                      (unsigned long long)entry.crumb.hash[i]);
        if (FILE* file = std::fopen(path, "wb")) {
            std::fwrite(code.data(), 1, size, file);
            std::fclose(file);
            std::printf("    shader %016llx code (%zu bytes from %#llx) saved to %s\n",
                        (unsigned long long)entry.crumb.hash[i], size,
                        (unsigned long long)program, path);
        }
    }
}

} // Anonymous namespace

u32 NewStream(const Instance& instance, const char* name) {
    std::call_once(init_once, [&] { Init(instance); });
    const u32 index = num_streams.fetch_add(1, std::memory_order_relaxed);
    if (index >= MaxStreams) {
        return MaxStreams;
    }
    streams[index].name = name;
    streams[index].ring = std::make_unique<Entry[]>(RingSize);
    return index;
}

bool Enabled() noexcept {
    return enabled;
}

u32 Note(u32 stream_index, const Crumb& crumb) {
    if (stream_index >= MaxStreams) {
        return 0;
    }
    Stream& stream = streams[stream_index];
    const u64 id = stream.next.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = stream.ring[id & (RingSize - 1)];
    entry.crumb = crumb;
    entry.time = Now();
    entry.id = id;
    if (crumb.args_slot < ArgSlots) {
        args_notes[crumb.args_slot] = {id, stream_index, crumb.hash[0]};
    }
    return u32(id);
}

void Mark(vk::CommandBuffer cmdbuf, u32 stream, u32 id, bool after) {
    if (!enabled || stream >= MaxStreams) {
        return;
    }
    cmdbuf.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, marker_buffer,
                                stream * StreamStride, id * 2 + (after ? 1 : 0));
}

void NoteDestroyed(const char* what, u64 handle, u64 gpu_address, u64 size, u32 width,
                   u32 height, u32 format) {
    if (!handle) {
        return;
    }
    const u64 time = Now();
    std::scoped_lock lock{destroyed_mutex};
    destroyed[destroyed_count++ % DestroyedSize] = {what,  handle, gpu_address, size,
                                                    time,  width,  height,      format};
    live_images.erase(handle);
}

void NoteImageCreated(u64 handle, u32 width, u32 height, u32 format, u64 size) {
    if (!handle) {
        return;
    }
    std::scoped_lock lock{destroyed_mutex};
    live_images[handle] = {width, height, format, size};
}

u32 ArgsSlot() {
    return enabled ? next_args_slot.fetch_add(1, std::memory_order_relaxed) % ArgSlots : ~0u;
}

void CopyArgs(vk::CommandBuffer cmdbuf, vk::Buffer source, u64 offset, u32 slot) {
    if (!enabled || slot >= ArgSlots) {
        return;
    }
    cmdbuf.copyBuffer(source, marker_buffer,
                      vk::BufferCopy{offset, ArgsOffset + u64(slot) * 16, 3 * sizeof(u32)});
}

namespace {
void Report(const char* title, const char* where);
}

void ReportDeviceLost(const char* where) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set()) {
        if (enabled) {
            Report("at device lost", where);
        }
        ReportFault();
    }
}

void ReportStuck(const char* where) {
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (enabled && !reported.test_and_set()) {
        Report("while the GPU makes no progress", where);
    }
}

namespace {
void Report(const char* title, const char* where) {
    std::printf("GPU breadcrumbs %s (%s): what each command stream finished\n", title, where);
    const u32 count = std::min(num_streams.load(), MaxStreams);
    for (u32 s = 0; s < count; ++s) {
        const Stream& stream = streams[s];
        const u64 last = stream.next.load() - 1;
        const u32 marker = markers[s * StreamStride / sizeof(u32)];
        std::printf("  stream %u (%s): %llu commands noted", s, stream.name,
                    (unsigned long long)last);
        if (last == 0) {
            std::printf("\n");
            continue;
        }
        if (marker == 0) {
            std::printf(", the GPU finished none\n");
            continue;
        }
        // The marker holds the low 32 bits of 2 * id + after.
        const u64 newest = last * 2 + 1;
        const u64 value = newest - u32(u32(newest) - marker);
        const u64 id = value / 2;
        if (value & 1) {
            if (id == last) {
                std::printf(", the GPU finished all\n");
                continue;
            }
            std::printf(", the GPU finished #%llu but did not start #%llu: stuck in the work "
                        "recorded between them (barriers, the start of a render pass) or before "
                        "it\n",
                        (unsigned long long)id, (unsigned long long)(id + 1));
            for (u64 earlier = id > 4 ? id - 4 : 1; earlier < id; ++earlier) {
                PrintEntry(stream, earlier, "earlier");
            }
            PrintEntry(stream, id, "finished");
            PrintEntry(stream, id + 1, "next");
        } else {
            std::printf(", the GPU started #%llu and did not finish it (%llu noted after it)\n",
                        (unsigned long long)id, (unsigned long long)(last - id));
            for (u64 earlier = id > 4 ? id - 4 : 1; earlier + 1 < id; ++earlier) {
                PrintEntry(stream, earlier, "earlier");
            }
            if (id > 1) {
                PrintEntry(stream, id - 1, "finished");
            }
            PrintEntry(stream, id, "STUCK");
            std::fflush(stdout);
            DumpPrograms(stream, id);
        }
        for (u64 next = id + 2; next <= std::min(last, id + 4); ++next) {
            PrintEntry(stream, next, "later");
        }
    }
    // The last images and buffers destroyed: one the GPU still used shows up here.
    const u64 now = Now();
    std::scoped_lock lock{destroyed_mutex};
    const u64 kept = std::min<u64>(destroyed_count, 8);
    std::printf("  destroyed lately (%llu images and buffers in all):\n",
                (unsigned long long)destroyed_count);
    for (u64 i = 0; i < kept; ++i) {
        const Destroyed& d = destroyed[(destroyed_count - 1 - i) % DestroyedSize];
        std::printf("    %s %#llx", d.what, (unsigned long long)d.handle);
        if (d.width) {
            std::printf(" %ux%u format %u", d.width, d.height, d.format);
        }
        if (d.gpu_address) {
            std::printf(" GPU %#llx", (unsigned long long)d.gpu_address);
        }
        if (d.size) {
            std::printf(" %llu bytes", (unsigned long long)d.size);
        }
        std::printf(", %.1f ms before this report\n", Ms(d.time, now));
    }
    std::fflush(stdout);
}
} // Anonymous namespace


void PrintProgress() {
    if (!enabled) {
        return;
    }
    std::printf("GPU breadcrumbs:");
    const u32 count = std::min(num_streams.load(), MaxStreams);
    std::array<u64, MaxStreams> finished{};
    for (u32 s = 0; s < count; ++s) {
        const u64 last = streams[s].next.load(std::memory_order_relaxed) - 1;
        if (last == 0) {
            continue;
        }
        const u32 marker = markers[s * StreamStride / sizeof(u32)];
        const u64 newest = last * 2 + 1;
        const u64 value = marker ? newest - u32(u32(newest) - marker) : 0;
        // 2n + 1: n finished; 2n: n started, n - 1 finished.
        finished[s] = value ? value / 2 - (value & 1 ? 0 : 1) : 0;
        std::printf(" stream %u (%s) %llu noted, %llu unfinished;", s, streams[s].name,
                    (unsigned long long)last, (unsigned long long)(last - finished[s]));
    }
    // The largest indirect dispatches among the last ArgSlots the GPU finished, one per shader:
    // counts that only grow from frame to frame end in a dispatch that never finishes.
    struct Largest {
        u64 hash;
        u64 groups;
        u32 dims[3];
    };
    std::array<Largest, 3> largest{};
    for (u32 slot = 0; slot < ArgSlots; ++slot) {
        const ArgsNote note = args_notes[slot];
        if (note.id == 0 || note.stream >= count || note.id > finished[note.stream]) {
            continue;
        }
        const u32 x = args[slot * 4], y = args[slot * 4 + 1], z = args[slot * 4 + 2];
        const u64 groups = u64(x) * y * z;
        auto it = std::find_if(largest.begin(), largest.end(),
                               [&](const Largest& l) { return l.hash == note.hash; });
        if (it == largest.end()) {
            it = std::min_element(largest.begin(), largest.end(),
                                  [](const Largest& a, const Largest& b) { return a.groups < b.groups; });
            if (it->groups >= groups) {
                continue;
            }
            *it = {note.hash, 0, {}};
        }
        if (groups >= it->groups) {
            *it = {note.hash, groups, {x, y, z}};
        }
    }
    std::sort(largest.begin(), largest.end(),
              [](const Largest& a, const Largest& b) { return a.groups > b.groups; });
    std::printf(" largest indirect dispatches:");
    for (const auto& l : largest) {
        if (l.groups != 0) {
            std::printf(" cs %016llx %ux%ux%u;", (unsigned long long)l.hash, l.dims[0], l.dims[1],
                        l.dims[2]);
        }
    }
    std::printf("\n");
}

} // namespace Vulkan::Breadcrumbs
