// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers

#include <vk_mem_alloc.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace BbNativeVk {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace fs = std::filesystem;

/// CUSA13795 1.21, from docs/d3d11_engine_hooks_1.21.md (struct offsets as in the D3D11 port's
/// port/src/gfx/engine.h: unchanged between 1.00 and 1.21).
constexpr u64 DeviceGlobal = 0x2aebce0; // igPS4GraphicsDevice*
constexpr u64 DevContext = 0x5b0;       // igPS4GraphicsDevice -> igPS4GraphicsDeviceContext

// igPS4GraphicsDeviceContext
constexpr u64 CtxAlphaToMask = 0x100, CtxBlendControl = 0x104,
              CtxClipControl = 0x108, CtxPrimSetup = 0x10c, CtxDepthStencilControl = 0x110,
              CtxStencilControl = 0x114, CtxStencilOpControl = 0x118, CtxColorTargets = 0x130,
              CtxDepthTarget = 0x170, CtxRasterState = 0x190, CtxRtMask = 0x198, CtxVs = 0x5a8,
              CtxGs = 0x5b0, CtxPs = 0x5b8, CtxPrimType = 0x5c0, CtxIndexSize = 0x5c4;
// shader resource (pool object) -> Gnm shader struct: common word, then PGM_LO/PGM_HI at +8
constexpr u64 ShGnm = 0x30;

struct ShaderInfo {
    std::string name;
    std::string kind; // vs, ps, es (a VS feeding a GS), gs
    std::vector<std::pair<int, int>> textures; // slot, dims
    std::vector<std::pair<int, bool>> samplers; // slot, compare
    std::vector<int> cbuffers;
    std::vector<std::pair<int, int>> inputs; // semantic, size
    VkShaderModule module = VK_NULL_HANDLE;
    bool load_tried = false;
};

/// A Vulkan image standing in for an engine render or depth target, keyed by its GPU address.
/// Same size as the engine's for now (render scaling comes with presentation).
struct Target {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    u32 width = 0, height = 0;
    vk::Format format = vk::Format::eUndefined;
    bool depth = false;
    u32 comp_swap = 0; // CB_COLOR_INFO.COMP_SWAP: channel order the engine writes in
    u64 bytes = 0;
};

u64 image_base = 0;
bool enabled = false;
fs::path shader_root;
std::atomic<const Vulkan::Instance*> instance{nullptr};

std::mutex mutex; // draws can come from more than one game thread
std::unordered_map<u64, ShaderInfo> shaders;          // FNV-1a 64 of the GCN program
std::unordered_map<u64, ShaderInfo*> shader_by_code;  // program address -> entry (nullptr: unknown)
std::unordered_set<u64> pipeline_keys;
std::unordered_map<u64, Target> targets; // GPU base address -> image

std::atomic<u64> draws{0}, resolved{0}, missing_shader{0}, no_target{0}, no_context{0};
std::atomic<u64> modules_made{0}, modules_failed{0}, unknown_programs{0};
std::atomic<u64> targets_made{0}, targets_remade{0}, targets_failed{0}, unusable_targets{0};
std::atomic<u64> target_bytes{0};

/// Reads guest memory without faulting on a bad pointer (only for things not seen before).
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

/// Plain reads of the engine's context, which the engine probe found readable at every draw.
template <class T>
T R(u64 address) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(T));
    return v;
}
u64 Q(u64 address) {
    return address ? R<u64>(address) : 0;
}

u64 Fnv(const u8* p, std::size_t n, u64 h = 0xcbf29ce484222325ull) {
    for (std::size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

template <class T>
void Mix(u64& h, const T& v) {
    h = Fnv(reinterpret_cast<const u8*>(&v), sizeof(T), h);
}

/// shaders.idx (tools/gcn2hlsl.py): S <hash> <name> <kind>, then T/M/B/I lines for that shader.
bool LoadIndex() {
    std::ifstream f(shader_root / "hlsl" / "shaders.idx");
    if (!f) {
        return false;
    }
    std::string line;
    ShaderInfo* cur = nullptr;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        char tag = 0;
        is >> tag;
        if (tag == 'S') {
            std::string hash, name, kind;
            is >> hash >> name >> kind;
            const u64 key = std::stoull(hash, nullptr, 16);
            if (shaders.count(key)) {
                cur = nullptr; // the same program listed under another effect
                continue;
            }
            cur = &shaders[key];
            cur->name = name;
            cur->kind = kind;
        } else if (cur && tag == 'T') {
            int slot = 0, dims = 0;
            is >> slot >> dims;
            cur->textures.push_back({slot, dims});
        } else if (cur && tag == 'M') {
            int slot = 0;
            std::string k;
            is >> slot >> k;
            cur->samplers.push_back({slot, k == "cmp"});
        } else if (cur && tag == 'B') {
            int slot = 0;
            is >> slot;
            cur->cbuffers.push_back(slot);
        } else if (cur && tag == 'I') {
            int sem = 0, size = 0;
            is >> sem >> size;
            cur->inputs.push_back({sem, size});
        }
    }
    return !shaders.empty();
}

/// spirv/<name>.spv as a VkShaderModule on the presenter's device (once per shader).
void LoadModule(ShaderInfo& s) {
    const Vulkan::Instance* inst = instance.load();
    if (s.load_tried || !inst) {
        return;
    }
    s.load_tried = true;
    std::ifstream f(shader_root / "spirv" / (s.name + ".spv"), std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (code.empty() || code.size() % 4) {
        modules_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("Native Vulkan: %s.spv missing or damaged\n", s.name.c_str());
        return;
    }
    const vk::ShaderModuleCreateInfo info{.codeSize = code.size(),
                                          .pCode = reinterpret_cast<const u32*>(code.data())};
    auto [result, module] = inst->GetDevice().createShaderModule(info);
    if (result != vk::Result::eSuccess) {
        modules_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("Native Vulkan: %s: vkCreateShaderModule failed (%s)\n", s.name.c_str(),
                    vk::to_string(result).c_str());
        return;
    }
    s.module = module;
    modules_made.fetch_add(1, std::memory_order_relaxed);
}

/// The translated shader for a bound engine shader (cached by program address).
ShaderInfo* Resolve(u64 handle, const char* stage) {
    if (!handle) {
        return nullptr;
    }
    const u64 gnm = Q(handle + ShGnm);
    if (!gnm) {
        return nullptr;
    }
    const u32 size = R<u32>(gnm) & 0x7fffff;
    const u64 code = (u64(R<u32>(gnm + 8)) << 8) | (u64(R<u32>(gnm + 12)) << 40);
    if (const auto it = shader_by_code.find(code); it != shader_by_code.end()) {
        return it->second;
    }
    ShaderInfo* info = nullptr;
    std::vector<u8> bytes(size);
    if (size && Read(code, bytes.data(), size)) {
        const u64 hash = Fnv(bytes.data(), size);
        if (const auto f = shaders.find(hash); f != shaders.end()) {
            info = &f->second;
            LoadModule(*info);
        } else {
            unknown_programs.fetch_add(1, std::memory_order_relaxed);
            std::printf("Native Vulkan: no translation for the %s program at %llx (%u bytes, hash "
                        "%016llx)\n",
                        stage, (unsigned long long)code, size, (unsigned long long)hash);
        }
    } else {
        unknown_programs.fetch_add(1, std::memory_order_relaxed);
        std::printf("Native Vulkan: %s program at %llx (%u bytes) is not readable\n", stage,
                    (unsigned long long)code, size);
    }
    shader_by_code[code] = info;
    return info;
}

/// CB_COLOR_INFO FORMAT (GCN data format) and NUMBER_TYPE (0 unorm, 1 snorm, 4 uint, 5 sint,
/// 6 srgb, 7 float) -> Vulkan, as the D3D11 port's ColorTargetFormat (port/src/gfx/formats.cpp).
vk::Format ColorFormat(u32 info) {
    using F = vk::Format;
    const u32 fmt = (info >> 2) & 0x1f, num = (info >> 8) & 7;
    const bool sn = num == 1, ui = num == 4, si = num == 5, fl = num == 7;
    switch (fmt) {
    case 1: // 8
        return sn ? F::eR8Snorm : ui ? F::eR8Uint : si ? F::eR8Sint : F::eR8Unorm;
    case 2: // 16
        return fl ? F::eR16Sfloat : sn ? F::eR16Snorm : ui ? F::eR16Uint : si ? F::eR16Sint
                                                                          : F::eR16Unorm;
    case 3: // 8_8
        return sn ? F::eR8G8Snorm : ui ? F::eR8G8Uint : F::eR8G8Unorm;
    case 4: // 32
        return fl ? F::eR32Sfloat : si ? F::eR32Sint : F::eR32Uint;
    case 5: // 16_16
        return fl ? F::eR16G16Sfloat : sn ? F::eR16G16Snorm : ui ? F::eR16G16Uint : F::eR16G16Unorm;
    case 6: // 10_11_11
    case 7: // 11_11_10
        return F::eB10G11R11UfloatPack32;
    case 8: // 10_10_10_2
    case 9: // 2_10_10_10
        return ui ? F::eA2B10G10R10UintPack32 : F::eA2B10G10R10UnormPack32;
    case 10: // 8_8_8_8
        return num == 6 ? F::eR8G8B8A8Srgb : sn ? F::eR8G8B8A8Snorm : ui ? F::eR8G8B8A8Uint
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
    default:
        return F::eUndefined;
    }
}

u32 FormatBytes(vk::Format f) {
    using F = vk::Format;
    switch (f) {
    case F::eR8Unorm:
    case F::eR8Snorm:
    case F::eR8Uint:
    case F::eR8Sint:
        return 1;
    case F::eR16Sfloat:
    case F::eR16Snorm:
    case F::eR16Uint:
    case F::eR16Sint:
    case F::eR16Unorm:
    case F::eR8G8Snorm:
    case F::eR8G8Uint:
    case F::eR8G8Unorm:
    case F::eR5G6B5UnormPack16:
    case F::eD16Unorm:
        return 2;
    case F::eR32G32Sfloat:
    case F::eR32G32Uint:
    case F::eR16G16B16A16Sfloat:
    case F::eR16G16B16A16Snorm:
    case F::eR16G16B16A16Uint:
    case F::eR16G16B16A16Unorm:
    case F::eD32SfloatS8Uint:
        return 8;
    case F::eR32G32B32A32Sfloat:
    case F::eR32G32B32A32Uint:
        return 16;
    default:
        return 4;
    }
}

void DestroyTarget(const Vulkan::Instance& inst, Target& t) {
    // safe while nothing records with these images; once draws are issued this must wait for the GPU
    if (t.view) {
        inst.GetDevice().destroyImageView(t.view);
    }
    if (t.image) {
        vmaDestroyImage(inst.GetAllocator(), t.image, t.allocation);
    }
    target_bytes.fetch_sub(t.bytes, std::memory_order_relaxed);
    t = Target{};
}

/// The image for the target at `base`, made again when its size or format changes.
Target* MakeTarget(u64 base, u32 w, u32 h, vk::Format format, bool depth, u32 comp_swap) {
    const Vulkan::Instance* inst = instance.load();
    if (!inst) {
        return nullptr;
    }
    Target& t = targets[base];
    if (t.image && t.width == w && t.height == h && t.format == format) {
        return &t;
    }
    if (t.image) {
        DestroyTarget(*inst, t);
        targets_remade.fetch_add(1, std::memory_order_relaxed);
    }
    using U = vk::ImageUsageFlagBits;
    const vk::ImageCreateInfo ici{
        .flags = depth ? vk::ImageCreateFlags{} : vk::ImageCreateFlags{vk::ImageCreateFlagBits::eMutableFormat},
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = U::eSampled | U::eTransferDst |
                 (depth ? vk::ImageUsageFlags{U::eDepthStencilAttachment}
                        : vk::ImageUsageFlags{U::eColorAttachment | U::eTransferSrc}),
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    const VkImageCreateInfo& raw = ici;
    const VkResult r =
        vmaCreateImage(inst->GetAllocator(), &raw, &aci, &image, &allocation, nullptr);
    if (r != VK_SUCCESS) {
        targets_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("Native Vulkan: target %llx %ux%u %s: image not created (%s)\n",
                    (unsigned long long)base, w, h, vk::to_string(format).c_str(),
                    vk::to_string(vk::Result(r)).c_str());
        targets.erase(base);
        return nullptr;
    }
    const vk::ImageViewCreateInfo vci{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {.aspectMask = depth ? vk::ImageAspectFlagBits::eDepth
                                                 : vk::ImageAspectFlagBits::eColor,
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
    };
    auto [vr, view] = inst->GetDevice().createImageView(vci);
    t.image = image;
    t.allocation = allocation;
    t.view = vr == vk::Result::eSuccess ? VkImageView(view) : VK_NULL_HANDLE;
    t.width = w;
    t.height = h;
    t.format = format;
    t.depth = depth;
    t.comp_swap = comp_swap;
    t.bytes = u64(w) * h * FormatBytes(format);
    target_bytes.fetch_add(t.bytes, std::memory_order_relaxed);
    targets_made.fetch_add(1, std::memory_order_relaxed);
    std::printf("Native Vulkan: %s target %llx %ux%u %s swap %u%s\n", depth ? "depth" : "color",
                (unsigned long long)base, w, h, vk::to_string(format).c_str(), comp_swap,
                t.view ? "" : " (no view)");
    return &t;
}

/// Gnm::RenderTarget: registers from +0x10 (CB_COLOR_BASE, PITCH, SLICE, VIEW, INFO, ATTRIB),
/// width | height << 16 at +0x4c.
Target* ColorTarget(u64 handle) {
    const u64 base = u64(R<u32>(handle + 0x10)) << 8;
    const u32 info = R<u32>(handle + 0x20), wh = R<u32>(handle + 0x4c);
    const u32 w = wh & 0xffff, h = wh >> 16;
    const vk::Format format = ColorFormat(info);
    if (!base || !w || !h || format == vk::Format::eUndefined) {
        if (unusable_targets.fetch_add(1, std::memory_order_relaxed) < 8) {
            std::printf("Native Vulkan: unusable color target %llx: base %llx info %x size %ux%u\n",
                        (unsigned long long)handle, (unsigned long long)base, info, w, h);
        }
        return nullptr;
    }
    return MakeTarget(base, w, h, format, false, (info >> 11) & 3);
}

/// Gnm::DepthRenderTarget: +0x50 DB_Z_INFO, +0x54 DB_STENCIL_INFO, +0x58 DB_Z_READ_BASE,
/// width | height << 16 at +0x80. Z_16 without stencil is a shadow map (its depth bias is in
/// 16-bit units, so it stays 16-bit); everything else is 32-bit float depth with 8-bit stencil.
Target* DepthTarget(u64 handle) {
    const u64 base = u64(R<u32>(handle + 0x58)) << 8;
    const u32 wh = R<u32>(handle + 0x80);
    const u32 w = wh & 0xffff, h = wh >> 16;
    if (!base || !w || !h) {
        unusable_targets.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const bool z16 = (R<u32>(handle + 0x50) & 3) == 1 && (R<u32>(handle + 0x54) & 1) == 0;
    vk::Format format = vk::Format::eD16Unorm;
    if (!z16) {
        const Vulkan::Instance* inst = instance.load();
        format = inst ? inst->GetSupportedFormat(vk::Format::eD32SfloatS8Uint,
                                                 vk::FormatFeatureFlagBits2::eDepthStencilAttachment)
                      : vk::Format::eD32SfloatS8Uint;
    }
    return MakeTarget(base, w, h, format, true, 0);
}

void Report() {
    u64 last = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const u64 d = draws.load();
        if (d == last) {
            continue;
        }
        std::size_t keys = 0, live_targets = 0;
        {
            std::lock_guard lk(mutex);
            keys = pipeline_keys.size();
            live_targets = targets.size();
        }
        std::printf("Native Vulkan: %.0f draws/s; in all %llu draws, %llu resolved, %llu with a "
                    "shader missing, %llu without a target, %llu without a context; %zu pipeline "
                    "states; %llu shader modules (%llu failed), %llu programs without a "
                    "translation\n",
                    (d - last) / 5.0, (unsigned long long)d, (unsigned long long)resolved.load(),
                    (unsigned long long)missing_shader.load(), (unsigned long long)no_target.load(),
                    (unsigned long long)no_context.load(), keys,
                    (unsigned long long)modules_made.load(), (unsigned long long)modules_failed.load(),
                    (unsigned long long)unknown_programs.load());
        std::printf("Native Vulkan: %zu target images (%.0f MiB); %llu made, %llu made again at a "
                    "new size or format, %llu failed, %llu unusable target handles\n",
                    live_targets, target_bytes.load() / 1048576.0,
                    (unsigned long long)targets_made.load(), (unsigned long long)targets_remade.load(),
                    (unsigned long long)targets_failed.load(),
                    (unsigned long long)unusable_targets.load());
        std::fflush(stdout);
        last = d;
    }
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    const char* env = std::getenv("BB_NATIVE_VK");
    if (!env || env[0] != '1') {
        return;
    }
    image_base = reinterpret_cast<u64>(image);
    if (DeviceGlobal + 8 > size) {
        std::printf("Native Vulkan: image too small for the 1.21 layout: off\n");
        return;
    }
    const char* dir = std::getenv("BB_VK_SHADERS");
    shader_root = dir ? fs::path(dir) : fs::path();
    if (!dir || !LoadIndex()) {
        std::printf("Native Vulkan: no shaders.idx under BB_VK_SHADERS (%s): off. Make them with "
                    "tools/shaders/build_spirv.py\n",
                    dir ? dir : "not set");
        std::fflush(stdout);
        return;
    }
    std::printf("Native Vulkan: on (shaders and targets only: shadPS4 still draws); %zu translated "
                "shaders from %s\n",
                shaders.size(), shader_root.string().c_str());
    std::fflush(stdout);
    enabled = true;
    std::thread(Report).detach();
}

void Attach(const Vulkan::Instance& vulkan) {
    instance.store(&vulkan);
}

void OnDraw(bool indexed, std::uint32_t count, std::uint64_t index_addr) {
    if (!enabled) {
        return;
    }
    draws.fetch_add(1, std::memory_order_relaxed);
    const u64 dev = Q(image_base + DeviceGlobal);
    const u64 ctx = dev ? Q(dev + DevContext) : 0;
    if (!ctx) {
        no_context.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::lock_guard lk(mutex);
    ShaderInfo* vs = Resolve(Q(ctx + CtxVs), "vertex");
    const u64 psh = Q(ctx + CtxPs);
    ShaderInfo* ps = psh ? Resolve(psh, "pixel") : nullptr;
    // an export shader feeds a geometry shader (which carries its own copy VS)
    const u64 gsh = vs && vs->kind == "es" ? Q(ctx + CtxGs) : 0;
    ShaderInfo* gs = gsh ? Resolve(gsh, "geometry") : nullptr;
    if (!vs || (psh && !ps) || (gsh && !gs)) {
        missing_shader.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    vk::Format formats[8]{};
    bool any_color = false;
    for (int i = 0; i < 8; ++i) {
        if (const u64 h = Q(ctx + CtxColorTargets + i * 8)) {
            if (const Target* t = ColorTarget(h)) {
                formats[i] = t->format;
                any_color = true;
            }
        }
    }
    const u64 dh = Q(ctx + CtxDepthTarget);
    const Target* depth_target = dh ? DepthTarget(dh) : nullptr;
    const vk::Format depth = depth_target ? depth_target->format : vk::Format::eUndefined;
    if (!any_color && !depth_target) {
        no_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    resolved.fetch_add(1, std::memory_order_relaxed);
    // what a VkPipeline is keyed on
    u64 key = 0xcbf29ce484222325ull;
    Mix(key, vs);
    Mix(key, ps);
    Mix(key, gs);
    Mix(key, formats);
    Mix(key, depth);
    Mix(key, indexed);
    for (const u64 off : {CtxAlphaToMask, CtxBlendControl, CtxClipControl, CtxPrimSetup,
                          CtxDepthStencilControl, CtxStencilControl, CtxStencilOpControl,
                          CtxRasterState, CtxRtMask, CtxPrimType, CtxIndexSize}) {
        Mix(key, R<u32>(ctx + off));
    }
    pipeline_keys.insert(key);
    (void)count; // used once draws are issued
    (void)index_addr;
}
} // namespace BbNativeVk
