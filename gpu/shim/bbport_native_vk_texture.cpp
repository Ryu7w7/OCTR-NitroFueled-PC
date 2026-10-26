// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk_texture.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace BbNativeVk::Tex {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Textures stop being made past this much memory (BB_VK_TEXTURE_MB, default 1024): they sit
/// beside shadPS4's own copies until the native path draws.
const u64 budget = [] {
    const char* env = std::getenv("BB_VK_TEXTURE_MB");
    const long mb = env ? std::strtol(env, nullptr, 10) : 1024;
    return u64(mb > 0 ? mb : 1024) << 20;
}();
constexpr u64 MaxTextureBytes = 256ull << 20; // a single guest image larger than this is skipped

const Vulkan::Instance* instance = nullptr;
bool anisotropy = false, mirror_clamp = false;

std::unordered_map<u64, Texture> textures; // FNV-1a of the T#'s first 6 dwords
std::unordered_map<u64, VkSampler> samplers;
std::unordered_set<u64> warned;             // T# keys logged as not converted

std::atomic<u64> total_bytes{0}, uploads_done{0}, uploads_pending{0}, upload_batches{0};
std::atomic<u64> unsupported{0}, unreadable{0}, over_budget{0}, failed{0};

bool Read(u64 address, void* out, std::size_t size) {
    if (address < 0x10000) {
        return false;
    }
#ifdef _WIN32
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size,
                             &done) &&
           done == size;
#else
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
#endif
}

u64 Fnv(const u8* p, std::size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

u32 Align(u32 v, u32 a) {
    return (v + a - 1) / a * a;
}

/// Decoded T# (8 dwords), as the D3D11 port's DecodeTSharp (port/src/gfx/formats.cpp).
struct TSharp {
    u64 base;
    u32 dfmt, nfmt, width, height, pitch, dst_sel, base_level, last_level, tiling, type;
    u32 base_array, last_array;
    bool pow2pad;
};

TSharp Decode(const u32* t) {
    TSharp s{};
    s.base = (u64(t[0]) << 8) | (u64(t[1] & 0xff) << 40);
    s.dfmt = (t[1] >> 20) & 0x3f;
    s.nfmt = (t[1] >> 26) & 0xf;
    s.width = (t[2] & 0x3fff) + 1;
    s.height = ((t[2] >> 14) & 0x3fff) + 1;
    s.dst_sel = t[3] & 0xfff;
    s.base_level = (t[3] >> 12) & 0xf;
    s.last_level = (t[3] >> 16) & 0xf;
    s.tiling = (t[3] >> 20) & 0x1f;
    s.pow2pad = (t[3] >> 25) & 1;
    s.type = (t[3] >> 28) & 0xf;
    s.pitch = ((t[4] >> 13) & 0x3fff) + 1;
    s.base_array = t[5] & 0x1fff;
    s.last_array = (t[5] >> 13) & 0x1fff;
    return s;
}

/// T# DATA_FORMAT / NUM_FORMAT (0 unorm, 1 snorm, 4 uint, 5 sint, 7 float, 9 srgb) -> Vulkan.
/// The channel order (dst_sel) is applied in the translated shaders, as in the D3D11 port.
vk::Format TextureFormat(u32 dfmt, u32 nfmt) {
    using F = vk::Format;
    const bool sn = nfmt == 1, ui = nfmt == 4, fl = nfmt == 7, srgb = nfmt == 9;
    switch (dfmt) {
    case 1: // 8
        return sn ? F::eR8Snorm : ui ? F::eR8Uint : F::eR8Unorm;
    case 2: // 16
        return fl ? F::eR16Sfloat : ui ? F::eR16Uint : sn ? F::eR16Snorm : F::eR16Unorm;
    case 3: // 8_8
        return sn ? F::eR8G8Snorm : F::eR8G8Unorm;
    case 4: // 32
        return fl ? F::eR32Sfloat : F::eR32Uint;
    case 5: // 16_16
        return fl ? F::eR16G16Sfloat : sn ? F::eR16G16Snorm : F::eR16G16Unorm;
    case 6: // 10_11_11
    case 7: // 11_11_10
        return F::eB10G11R11UfloatPack32;
    case 8: // 10_10_10_2
    case 9: // 2_10_10_10
        return F::eA2B10G10R10UnormPack32;
    case 10: // 8_8_8_8
        return sn ? F::eR8G8B8A8Snorm : ui ? F::eR8G8B8A8Uint : srgb ? F::eR8G8B8A8Srgb
                                                                     : F::eR8G8B8A8Unorm;
    case 11: // 32_32
        return fl ? F::eR32G32Sfloat : F::eR32G32Uint;
    case 12: // 16_16_16_16
        return fl ? F::eR16G16B16A16Sfloat : sn ? F::eR16G16B16A16Snorm
               : ui ? F::eR16G16B16A16Uint : F::eR16G16B16A16Unorm;
    case 14: // 32_32_32_32
        return fl ? F::eR32G32B32A32Sfloat : F::eR32G32B32A32Uint;
    case 16: // 5_6_5
        return F::eR5G6B5UnormPack16;
    case 17: // 1_5_5_5
    case 18: // 5_5_5_1
        return F::eA1R5G5B5UnormPack16;
    case 19: // 4_4_4_4
        return F::eA4R4G4B4UnormPack16;
    case 35:
        return srgb ? F::eBc1RgbaSrgbBlock : F::eBc1RgbaUnormBlock;
    case 36:
        return srgb ? F::eBc2SrgbBlock : F::eBc2UnormBlock;
    case 37:
        return srgb ? F::eBc3SrgbBlock : F::eBc3UnormBlock;
    case 38:
        return sn ? F::eBc4SnormBlock : F::eBc4UnormBlock;
    case 39:
        return sn ? F::eBc5SnormBlock : F::eBc5UnormBlock;
    case 40:
        return sn ? F::eBc6HSfloatBlock : F::eBc6HUfloatBlock;
    case 41:
        return srgb ? F::eBc7SrgbBlock : F::eBc7UnormBlock;
    default:
        return F::eUndefined; // includes 32_32_32: rarely sampleable in Vulkan
    }
}

/// Bytes per element (a texel, or a 4x4 block for BC formats).
u32 ElementBytes(u32 dfmt, bool* block) {
    *block = dfmt >= 35 && dfmt <= 41;
    switch (dfmt) {
    case 1:
        return 1;
    case 2:
    case 3:
    case 16:
    case 17:
    case 18:
    case 19:
        return 2;
    case 11:
    case 12:
    case 35:
    case 38:
        return 8;
    case 14:
    case 36:
    case 37:
    case 39:
    case 40:
    case 41:
        return 16;
    default:
        return 4;
    }
}

struct Mip {
    u32 ew, eh;       // elements to copy (texels or 4x4 blocks)
    u32 row_e;        // tiled pitch in elements
    u64 offset;       // byte offset of the level (all slices)
    u64 slice_bytes;  // bytes per slice of this level
};

/// Guest memory layout of a T#'s image (GCN addrlib rules; the D3D11 port's GetLayout).
struct Layout {
    vk::Format format;
    u32 eb, bw; // element bytes, block width (1 or 4)
    u32 slices, levels;
    bool cube;
    u32 width0, height0;
    Mip mips[16];
    u64 total;
};

bool TilingSupported(u32 tiling) {
    // 8: linear aligned; 5, 9, 13: 1D thin micro tiling
    return tiling == 8 || tiling == 5 || tiling == 9 || tiling == 13;
}

bool GetLayout(const TSharp& t, Layout* l) {
    l->format = TextureFormat(t.dfmt, t.nfmt);
    if (l->format == vk::Format::eUndefined || !TilingSupported(t.tiling)) {
        return false;
    }
    bool bc = false;
    l->eb = ElementBytes(t.dfmt, &bc);
    l->bw = bc ? 4 : 1;
    l->cube = t.type == 11;
    const bool array = t.type == 12 || t.type == 13 || l->cube;
    l->slices = l->cube ? 6 : array ? (t.last_array - t.base_array + 1) : 1;
    if (t.type == 10) {
        l->slices = 1; // 3D textures: only the first slice for now
    }
    l->levels = std::min<u32>(t.last_level + 1, 16);
    l->width0 = bc ? Align(t.width, 4) : t.width;
    l->height0 = bc ? Align(t.height, 4) : t.height;
    u64 offset = 0;
    for (u32 m = 0; m < l->levels; ++m) {
        Mip& mp = l->mips[m];
        const u32 w = std::max(1u, l->width0 >> m), h = std::max(1u, l->height0 >> m);
        mp.ew = (w + l->bw - 1) / l->bw;
        mp.eh = (h + l->bw - 1) / l->bw;
        u32 pw = std::max(1u, t.pitch >> m), ph = std::max(1u, t.height >> m);
        pw = (pw + l->bw - 1) / l->bw;
        ph = (ph + l->bw - 1) / l->bw;
        if (t.pow2pad) {
            pw = std::bit_ceil(pw);
            ph = std::bit_ceil(ph);
        }
        if (t.tiling == 8) {
            const u32 pitch_align = std::max(8u, 64u / l->eb);
            u32 row = Align(pw, pitch_align);
            const u32 slice_align = std::max(64u, 256u / l->eb);
            while ((u64(row) * ph) % slice_align) {
                row += pitch_align;
            }
            mp.row_e = row;
            mp.slice_bytes = u64(row) * ph * l->eb;
        } else {
            u32 row = Align(pw, 8);
            const u32 rows = Align(ph, 8);
            while ((u64(row) * rows * l->eb) % 256) {
                row += 8;
            }
            mp.row_e = row;
            mp.slice_bytes = u64(row) * rows * l->eb;
        }
        mp.ew = std::min(mp.ew, mp.row_e);
        mp.offset = offset;
        offset += mp.slice_bytes * l->slices;
    }
    l->total = offset;
    return true;
}

u32 MortonInTile(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
           ((y & 4) << 3);
}

/// One level of one slice into tightly packed rows (8x8-element micro tiles stored row-major,
/// elements in Morton order inside a tile).
void DetileLevel(const TSharp& t, const Layout& l, const u8* guest, u32 m, u32 s, u8* dst) {
    const Mip& mp = l.mips[m];
    const u8* sb = guest + mp.offset + s * mp.slice_bytes;
    const std::size_t row_bytes = std::size_t(mp.ew) * l.eb;
    if (t.tiling == 8) {
        for (u32 y = 0; y < mp.eh; ++y) {
            std::memcpy(dst + y * row_bytes, sb + u64(y) * mp.row_e * l.eb, row_bytes);
        }
        return;
    }
    for (u32 y = 0; y < mp.eh; ++y) {
        u8* drow = dst + y * row_bytes;
        const u64 tile_row = u64(y >> 3) * (mp.row_e >> 3);
        for (u32 x = 0; x < mp.ew; ++x) {
            const u64 off = ((tile_row + (x >> 3)) * 64 + MortonInTile(x & 7, y & 7)) * l.eb;
            std::memcpy(drow + std::size_t(x) * l.eb, sb + off, l.eb);
        }
    }
}

// ------------------------------------------------------------------ uploads
struct Job {
    std::atomic<bool>* ready; // the texture's flag, set once the batch is submitted
    VkImage image;
    u32 levels, slices;
    std::vector<u8> data;                   // untiled levels, each region 16-byte aligned
    std::vector<vk::BufferImageCopy> regions; // offsets into `data`
};

std::mutex upload_mutex;
std::condition_variable upload_cv;
std::vector<Job> upload_queue;

void SubmitBatch(vk::CommandPool pool, vk::Fence fence, std::vector<Job>& jobs) {
    const vk::Device dev = instance->GetDevice();
    u64 size = 0;
    for (const Job& j : jobs) {
        size += (j.data.size() + 15) & ~u64(15);
    }
    const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = std::max<u64>(size, 16),
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance->GetAllocator(), &bci, &aci, &staging, &allocation, &info) !=
        VK_SUCCESS) {
        failed.fetch_add(jobs.size(), std::memory_order_relaxed);
        std::printf("Native Vulkan: no staging buffer for %zu texture uploads (%llu bytes)\n",
                    jobs.size(), (unsigned long long)size);
        return;
    }
    auto [ar, cmds] = dev.allocateCommandBuffers(
        {.commandPool = pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
    if (ar != vk::Result::eSuccess) {
        vmaDestroyBuffer(instance->GetAllocator(), staging, allocation);
        failed.fetch_add(jobs.size(), std::memory_order_relaxed);
        return;
    }
    const vk::CommandBuffer cmd = cmds[0];
    (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    u64 at = 0;
    for (Job& j : jobs) {
        std::memcpy(static_cast<u8*>(info.pMappedData) + at, j.data.data(), j.data.size());
        const vk::ImageSubresourceRange all{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                            .baseMipLevel = 0,
                                            .levelCount = j.levels,
                                            .baseArrayLayer = 0,
                                            .layerCount = j.slices};
        const vk::ImageMemoryBarrier to_dst{.srcAccessMask = {},
                                            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                                            .oldLayout = vk::ImageLayout::eUndefined,
                                            .newLayout = vk::ImageLayout::eTransferDstOptimal,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .image = j.image,
                                            .subresourceRange = all};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_dst);
        for (vk::BufferImageCopy& r : j.regions) {
            r.bufferOffset += at;
        }
        cmd.copyBufferToImage(staging, j.image, vk::ImageLayout::eTransferDstOptimal, j.regions);
        const vk::ImageMemoryBarrier to_read{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                             .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                                             .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                                             .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .image = j.image,
                                             .subresourceRange = all};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, to_read);
        at += (j.data.size() + 15) & ~u64(15);
    }
    (void)cmd.end();
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
    vk::Result sr;
    {
        // every graphics-queue submit and present in shadPS4 holds this lock
        std::scoped_lock lk{Vulkan::Scheduler::submit_mutex};
        sr = instance->GetGraphicsQueue().submit(submit, fence);
    }
    if (sr == vk::Result::eSuccess) {
        for (const Job& j : jobs) {
            j.ready->store(true, std::memory_order_release);
        }
        (void)dev.waitForFences(fence, VK_TRUE, ~0ull);
        uploads_done.fetch_add(jobs.size(), std::memory_order_relaxed);
        upload_batches.fetch_add(1, std::memory_order_relaxed);
    } else {
        failed.fetch_add(jobs.size(), std::memory_order_relaxed);
        std::printf("Native Vulkan: texture upload submit failed (%s)\n", vk::to_string(sr).c_str());
    }
    (void)dev.resetFences(fence);
    dev.freeCommandBuffers(pool, cmd);
    vmaDestroyBuffer(instance->GetAllocator(), staging, allocation);
}

/// Uploads queued textures in batches of up to 64 MiB.
void UploadThread() {
    const vk::Device dev = instance->GetDevice();
    auto [pr, pool] = dev.createCommandPool(
        {.flags = vk::CommandPoolCreateFlagBits::eTransient,
         .queueFamilyIndex = instance->GetGraphicsQueueFamilyIndex()});
    auto [fr, fence] = dev.createFence({});
    if (pr != vk::Result::eSuccess || fr != vk::Result::eSuccess) {
        std::printf("Native Vulkan: no command pool or fence for texture uploads\n");
        return;
    }
    for (;;) {
        std::vector<Job> jobs;
        {
            std::unique_lock lk(upload_mutex);
            upload_cv.wait(lk, [] { return !upload_queue.empty(); });
            u64 size = 0;
            std::size_t n = 0;
            while (n < upload_queue.size() && (n == 0 || size + upload_queue[n].data.size() <= (64ull << 20))) {
                size += upload_queue[n].data.size();
                ++n;
            }
            jobs.assign(std::make_move_iterator(upload_queue.begin()),
                        std::make_move_iterator(upload_queue.begin() + n));
            upload_queue.erase(upload_queue.begin(), upload_queue.begin() + n);
        }
        SubmitBatch(pool, fence, jobs);
        uploads_pending.fetch_sub(jobs.size(), std::memory_order_relaxed);
    }
}

/// Logs a T# that was not converted (each texture once, at most 200).
void Warn(u64 key, const TSharp& t, const char* why) {
    if (warned.size() >= 200 || !warned.insert(key).second) {
        return;
    }
    std::printf("Native Vulkan: texture %llx not converted (%s): tiling %u type %u dfmt %u nfmt %u "
                "%ux%u pitch %u levels %u\n",
                (unsigned long long)t.base, why, t.tiling, t.type, t.dfmt, t.nfmt, t.width, t.height,
                t.pitch, t.last_level + 1);
}

vk::SamplerAddressMode AddressMode(u32 c) {
    switch (c & 7) {
    case 0:
        return vk::SamplerAddressMode::eRepeat;
    case 1:
        return vk::SamplerAddressMode::eMirroredRepeat;
    case 2:
        return vk::SamplerAddressMode::eClampToEdge;
    case 4:
    case 6:
        return vk::SamplerAddressMode::eClampToBorder;
    default: // 3, 5, 7: mirror once
        return mirror_clamp ? vk::SamplerAddressMode::eMirrorClampToEdge
                            : vk::SamplerAddressMode::eClampToEdge;
    }
}
} // namespace

void Init(const Vulkan::Instance& vulkan) {
    instance = &vulkan;
    anisotropy = vulkan.IsAnisotropicFilteringSupported();
    const auto chain = vulkan.GetPhysicalDevice()
                           .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
    mirror_clamp = chain.get<vk::PhysicalDeviceVulkan12Features>().samplerMirrorClampToEdge;
    std::thread(UploadThread).detach();
}

std::uint64_t TSharpBase(const std::uint32_t* tsharp) {
    return Decode(tsharp).base;
}

Info Describe(const std::uint32_t* tsharp) {
    const TSharp t = Decode(tsharp);
    return Info{t.base,    t.width,      t.height,     t.dfmt, t.nfmt,
                t.dst_sel, t.last_level, TextureFormat(t.dfmt, t.nfmt)};
}

const Texture* Get(const std::uint32_t* tsharp) {
    if (!instance) {
        return nullptr;
    }
    const u64 key = Fnv(reinterpret_cast<const u8*>(tsharp), 24);
    if (const auto it = textures.find(key); it != textures.end()) {
        return it->second.image ? &it->second : nullptr;
    }
    Texture& tex = textures[key]; // an empty entry remembers a texture that could not be made
    const TSharp t = Decode(tsharp);
    Layout l{};
    if (!GetLayout(t, &l)) {
        unsupported.fetch_add(1, std::memory_order_relaxed);
        Warn(key, t, "format or tiling");
        return nullptr;
    }
    if (l.total > MaxTextureBytes) {
        unsupported.fetch_add(1, std::memory_order_relaxed);
        Warn(key, t, "too large");
        return nullptr;
    }
    u64 bytes = 0;
    for (u32 m = 0; m < l.levels; ++m) {
        bytes += u64(l.mips[m].ew) * l.mips[m].eh * l.eb * l.slices;
    }
    if (total_bytes.load() + bytes > budget) {
        over_budget.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    std::vector<u8> guest(l.total);
    if (!Read(t.base, guest.data(), guest.size())) {
        unreadable.fetch_add(1, std::memory_order_relaxed);
        Warn(key, t, "memory not readable");
        return nullptr;
    }

    const vk::ImageCreateInfo ici{
        .flags = l.cube ? vk::ImageCreateFlags{vk::ImageCreateFlagBits::eCubeCompatible}
                        : vk::ImageCreateFlags{},
        .imageType = vk::ImageType::e2D,
        .format = l.format,
        .extent = {l.width0, l.height0, 1},
        .mipLevels = l.levels,
        .arrayLayers = l.slices,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    const VkImageCreateInfo& raw = ici;
    const VkResult r =
        vmaCreateImage(instance->GetAllocator(), &raw, &aci, &image, &allocation, nullptr);
    if (r != VK_SUCCESS) {
        failed.fetch_add(1, std::memory_order_relaxed);
        Warn(key, t, vk::to_string(vk::Result(r)).c_str());
        return nullptr;
    }
    const u32 first = std::min(t.base_level, l.levels - 1);
    auto view_of = [&](vk::ImageViewType type) {
        const vk::ImageViewCreateInfo vci{
            .image = image,
            .viewType = type,
            .format = l.format,
            .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .baseMipLevel = first,
                                 .levelCount = l.levels - first,
                                 .baseArrayLayer = 0,
                                 .layerCount = l.slices},
        };
        auto [vr, view] = instance->GetDevice().createImageView(vci);
        return vr == vk::Result::eSuccess ? VkImageView(view) : VK_NULL_HANDLE;
    };
    tex.image = image;
    tex.view_array = view_of(vk::ImageViewType::e2DArray);
    tex.view = l.cube ? view_of(vk::ImageViewType::eCube)
               : l.slices == 1 ? view_of(vk::ImageViewType::e2D)
                               : tex.view_array;
    tex.width = l.width0;
    tex.height = l.height0;
    tex.levels = l.levels;
    tex.slices = l.slices;
    tex.format = l.format;
    tex.bytes = bytes;
    total_bytes.fetch_add(bytes, std::memory_order_relaxed);

    // untile every slice and level (slice-major) into the upload job
    Job job{&tex.ready, image, l.levels, l.slices, {}, {}};
    u64 size = 0;
    for (u32 s = 0; s < l.slices; ++s) {
        for (u32 m = 0; m < l.levels; ++m) {
            size = (size + 15) & ~u64(15);
            size += u64(l.mips[m].ew) * l.mips[m].eh * l.eb;
        }
    }
    job.data.resize(size);
    u64 at = 0;
    for (u32 s = 0; s < l.slices; ++s) {
        for (u32 m = 0; m < l.levels; ++m) {
            at = (at + 15) & ~u64(15);
            const Mip& mp = l.mips[m];
            DetileLevel(t, l, guest.data(), m, s, job.data.data() + at);
            job.regions.push_back(vk::BufferImageCopy{
                .bufferOffset = at,
                .bufferRowLength = mp.ew * l.bw,
                .bufferImageHeight = mp.eh * l.bw,
                .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                     .mipLevel = m,
                                     .baseArrayLayer = s,
                                     .layerCount = 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {std::max(1u, l.width0 >> m), std::max(1u, l.height0 >> m), 1},
            });
            at += u64(mp.ew) * mp.eh * l.eb;
        }
    }
    uploads_pending.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lk(upload_mutex);
        upload_queue.push_back(std::move(job));
    }
    upload_cv.notify_one();
    return &tex;
}

VkSampler Sampler(const std::uint32_t ss[4], bool compare) {
    if (!instance) {
        return VK_NULL_HANDLE;
    }
    const u64 key = Fnv(reinterpret_cast<const u8*>(ss), 16) ^ (compare ? 0x5bd1e995ull : 0);
    if (const auto it = samplers.find(key); it != samplers.end()) {
        return it->second;
    }
    // S# fields as the D3D11 port's GetSampler (port/src/gfx/renderer.cpp)
    const u32 max_aniso = 1u << std::min<u32>((ss[0] >> 9) & 7, 4);
    const u32 depth_func = (ss[0] >> 12) & 7;
    const u32 mag = (ss[2] >> 20) & 3, min = (ss[2] >> 22) & 3, mipf = (ss[2] >> 26) & 3;
    const bool lin_mag = mag == 1 || mag == 3, lin_min = min == 1 || min == 3;
    // PC extra, as the port: 16x anisotropy for filtered, mipmapped samplers
    const bool aniso = anisotropy && !compare && ((mag >= 2 || min >= 2) || (lin_min && lin_mag && mipf != 0));
    const float min_lod = (ss[1] & 0xfff) / 256.0f;
    float max_lod = mipf == 0 ? min_lod : ((ss[1] >> 12) & 0xfff) / 256.0f;
    if (max_lod >= 15.9f) {
        max_lod = VK_LOD_CLAMP_NONE;
    }
    const u32 border = ss[3] >> 30; // 0 transparent black, 1 opaque black, 2 opaque white
    static constexpr vk::CompareOp ops[] = {
        vk::CompareOp::eNever,   vk::CompareOp::eLess,     vk::CompareOp::eEqual,
        vk::CompareOp::eLessOrEqual, vk::CompareOp::eGreater, vk::CompareOp::eNotEqual,
        vk::CompareOp::eGreaterOrEqual, vk::CompareOp::eAlways};
    const vk::SamplerCreateInfo sci{
        .magFilter = lin_mag || aniso ? vk::Filter::eLinear : vk::Filter::eNearest,
        .minFilter = lin_min || aniso ? vk::Filter::eLinear : vk::Filter::eNearest,
        .mipmapMode = mipf == 2 || aniso ? vk::SamplerMipmapMode::eLinear
                                         : vk::SamplerMipmapMode::eNearest,
        .addressModeU = AddressMode(ss[0]),
        .addressModeV = AddressMode(ss[0] >> 3),
        .addressModeW = AddressMode(ss[0] >> 6),
        .mipLodBias = float(std::int32_t((ss[2] & 0x3fff) << 18) >> 18) / 256.0f,
        .anisotropyEnable = aniso,
        .maxAnisotropy = aniso ? float(std::max<u32>(max_aniso, 16)) : 1.0f,
        .compareEnable = compare,
        .compareOp = compare ? (depth_func ? ops[depth_func] : vk::CompareOp::eLessOrEqual)
                             : vk::CompareOp::eNever,
        .minLod = min_lod,
        .maxLod = max_lod,
        .borderColor = border == 0   ? vk::BorderColor::eFloatTransparentBlack
                       : border == 1 ? vk::BorderColor::eFloatOpaqueBlack
                                     : vk::BorderColor::eFloatOpaqueWhite,
        .unnormalizedCoordinates = VK_FALSE,
    };
    auto [r, sampler] = instance->GetDevice().createSampler(sci);
    if (r != vk::Result::eSuccess) {
        std::printf("Native Vulkan: sampler %08x %08x %08x %08x not created (%s)\n", ss[0], ss[1],
                    ss[2], ss[3], vk::to_string(r).c_str());
        return samplers[key] = VK_NULL_HANDLE;
    }
    return samplers[key] = sampler;
}

Stats GetStats() {
    return Stats{textures.size(),          total_bytes.load(),   uploads_done.load(),
                 uploads_pending.load(),   upload_batches.load(), unsupported.load(),
                 unreadable.load(),        over_budget.load(),   failed.load(),
                 samplers.size()};
}
} // namespace BbNativeVk::Tex
