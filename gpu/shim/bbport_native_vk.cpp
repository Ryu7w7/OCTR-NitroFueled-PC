// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk.h"

#include <algorithm>
#include <array>
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

#include "bbport_hook.h"
#include "bbport_native_vk_buffer.h"
#include "bbport_native_vk_gpu.h"
#include "bbport_native_vk_spirv.h"
#include "bbport_native_vk_texture.h"

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
using s32 = std::int32_t;
namespace fs = std::filesystem;

/// CUSA13795 1.21, from docs/d3d11_engine_hooks_1.21.md (struct offsets as in the D3D11 port's
/// port/src/gfx/engine.h: unchanged between 1.00 and 1.21).
constexpr u64 DeviceGlobal = 0x2aebce0; // igPS4GraphicsDevice*
constexpr u64 DevContext = 0x5b0;       // igPS4GraphicsDevice -> igPS4GraphicsDeviceContext
constexpr u64 DevMissingTexture = 0x580; // the device's own T# for unset texture slots

// igPS4GraphicsDeviceContext
constexpr u64 CtxViewport = 0x0c, CtxScissorEnabled = 0x29, CtxScissor = 0x2c,
              CtxAlphaToMask = 0x100, CtxBlendControl = 0x104,
              CtxClipControl = 0x108, CtxPrimSetup = 0x10c, CtxDepthStencilControl = 0x110,
              CtxStencilControl = 0x114, CtxStencilOpControl = 0x118, CtxColorTargets = 0x130,
              CtxDepthTarget = 0x170, CtxRasterState = 0x190, CtxRtMask = 0x198,
              CtxPsSamplers = 0x1a0, CtxPsTextures = 0x220, CtxVsSamplers = 0x2a0,
              CtxVsTextures = 0x320, CtxVs = 0x5a8, CtxGs = 0x5b0, CtxPs = 0x5b8,
              CtxPrimType = 0x5c0, CtxIndexSize = 0x5c4, CtxVertexBuffers = 0x4a0,
              CtxVertexFormats = 0x520, CtxIndexBuffer = 0x5a0, CtxVbOffsets = 0x5d0,
              CtxVbStrides = 0x610;
// shader resource (pool object): igPS4ShaderHeader at +0x18, Gnm shader struct at +0x30 (common
// word, then PGM_LO/PGM_HI at +8)
constexpr u64 ShHeader = 0x18, ShGnm = 0x30;
// igPS4ShaderHeader vectors (count at +0, data at +0x10): the VS input elements {usage, usage
// index, semantic}, and the constant buffers (data pointers, sizes, slots)
constexpr u64 HdrInputElements = 0x30, HdrCbMemory = 0x48, HdrCbSize = 0x60, HdrCbIndex = 0x78;

struct ViewportI {
    s32 x, y, w, h;
    float zmin, zmax;
};

struct ShaderInfo {
    std::string name;
    std::string kind; // vs, ps, es (a VS feeding a GS), gs
    std::vector<std::pair<int, int>> textures; // slot, dims
    std::vector<std::pair<int, bool>> samplers; // slot, compare
    std::vector<int> cbuffers;
    std::vector<std::pair<int, int>> inputs; // semantic, size
    // the SPIR-V with its descriptors in the stage's set, and what it declares
    std::vector<u32> code;
    std::vector<Spirv::Binding> bindings;
    u32 input_locations = 0;
    VkShaderModule module = VK_NULL_HANDLE;
    std::unordered_map<u64, VkShaderModule> variants; // integer inputs: uint mask | sint mask << 32
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    bool load_tried = false;
};

/// A Vulkan image standing in for an engine render or depth target (or a texture copy's
/// destination), keyed by its GPU address. Same size as the engine's for now.
struct Target {
    Gpu::Image img; // image, attachment view, format, size, tracked layout
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;       // sampled: color, or the depth aspect
    VkImageView view_array = VK_NULL_HANDLE; // sampled as a one-slice 2D array
    u32 comp_swap = 0; // CB_COLOR_INFO.COMP_SWAP: channel order the engine writes in
    u64 bytes = 0;
};

/// What a draw binds at one texture slot.
struct Slot {
    Target* target = nullptr;
    const Tex::Texture* texture = nullptr;
};

/// One shader stage's textures, samplers and channel-order codes.
struct StageRes {
    Slot slots[32];
    VkSampler samplers[16]{};
    u32 swz[32]{};
};

/// The vertex streams and attributes of a draw.
struct VertexInput {
    std::vector<vk::VertexInputBindingDescription2EXT> bindings;
    std::vector<vk::VertexInputAttributeDescription2EXT> attributes;
    VkBuffer buffers[16]{};
    u64 offsets[16]{};
    u32 uint_mask = 0, sint_mask = 0;
    u32 vin[32]{}; // ctr_vin: per semantic, 1 = uscaled, 2 = sscaled (the shader converts)
    u64 key = 0;
    bool ok = true;
};

/// b13 of every stage (the D3D11 port's InfoCB).
struct InfoCB {
    u32 swz[32];      // ctr_swz: texture channel order per slot
    u32 vin[32];      // ctr_vin: VS input conversion per semantic
    float scale[4];   // ctr_scale: x = 1 / scale of the bound render target, y = scale
    float tscale[32]; // ctr_tscale: scale of the texture bound per slot
    u32 pad[28];
};
static_assert(sizeof(InfoCB) == 512);

u64 image_base = 0;
bool enabled = false;
bool drawing = false; // stage 5 on (BB_VK_DRAW != 0) and the recorder is up
bool show = false;    // BB_VK_SHOW=1
bool dynamic_vertex_input = false;
u64 ubo_align = 256;
fs::path shader_root;
std::atomic<const Vulkan::Instance*> instance{nullptr};

std::mutex mutex; // draws can come from more than one game thread
std::unordered_map<u64, ShaderInfo> shaders;          // FNV-1a 64 of the GCN program
std::unordered_map<u64, ShaderInfo*> shader_by_code;  // program address -> entry (nullptr: unknown)
std::unordered_set<u64> pipeline_keys;
std::unordered_map<u64, VkPipeline> pipelines;        // key -> pipeline (null: failed)
std::unordered_map<u64, VkPipelineLayout> pipeline_layouts;
VkDescriptorSetLayout empty_set_layout = VK_NULL_HANDLE;
std::unordered_map<u64, Target> targets; // GPU base address -> image

/// Display buffers the presenter asked for (BB_VK_SHOW): address -> our image, once submitted.
struct Shown {
    VkImage image = VK_NULL_HANDLE;
    vk::Format format = vk::Format::eUndefined;
    u32 width = 0, height = 0;
};
std::mutex display_mutex;
std::unordered_map<u64, Shown> shown;

std::atomic<u64> draws{0}, resolved{0}, missing_shader{0}, no_target{0}, no_context{0};
std::atomic<u64> modules_made{0}, modules_failed{0}, unknown_programs{0};
std::atomic<u64> targets_made{0}, targets_remade{0}, targets_failed{0}, unusable_targets{0};
std::atomic<u64> target_bytes{0};
std::atomic<u64> texture_binds{0}, target_binds{0}, unset_textures{0}, texture_misses{0},
    bad_tsharps{0}, sampler_binds{0}, sampler_misses{0}, textures_not_ready{0};
std::atomic<u64> vb_binds{0}, vb_in_place{0}, vb_copy_bytes{0}, ib_draws{0}, ib_in_place{0},
    ib_copy_bytes{0}, generated_index_draws{0}, missing_attributes{0}, unknown_vertex_formats{0},
    integer_attributes{0}, bad_vertex_buffers{0};
std::atomic<u64> drawn{0}, skipped_gs{0}, skipped_vertices{0}, skipped_pipeline{0},
    skipped_memory{0}, skipped_indices{0}, color_feedback{0}, depth_dropped{0}, depth_read_only{0},
    pipelines_failed{0}, variants_failed{0}, clears{0}, copies{0}, copies_skipped{0},
    submits_seen{0}, depth_copies{0};
// copies not done, by reason
enum CopySkip { SkipMip, SkipSource, SkipSame, SkipDest, SkipFormat, SkipCount };
std::atomic<u64> copy_skips[SkipCount]{};
constexpr const char* CopySkipNames[SkipCount] = {"mip level", "source not ours", "same image",
                                                  "unknown destination", "format size"};
// CPU time in the hooks (ns), frames the presenter asked for
std::atomic<u64> draw_ns{0}, clear_ns{0}, copy_ns{0}, submit_ns{0}, frames_asked{0};

/// Adds the time since construction to a counter.
struct Timer {
    std::atomic<u64>& out;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit Timer(std::atomic<u64>& o) : out(o) {}
    ~Timer() {
        out.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count()),
                      std::memory_order_relaxed);
    }
};
std::unordered_set<u64> vertex_layouts; // distinct attribute/binding layouts
std::unordered_set<u32> logged_prims;
std::unordered_set<u64> good_tsharps; // T# addresses checked readable once

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

/// Descriptor set of a stage: VS (and ES) 0, PS 1, GS 2.
u32 StageSet(const ShaderInfo& s) {
    return s.kind == "ps" ? 1 : s.kind == "gs" ? 2 : 0;
}

vk::ShaderStageFlagBits StageBit(const ShaderInfo& s) {
    return s.kind == "ps"   ? vk::ShaderStageFlagBits::eFragment
           : s.kind == "gs" ? vk::ShaderStageFlagBits::eGeometry
                            : vk::ShaderStageFlagBits::eVertex;
}

vk::DescriptorType DescriptorType(Spirv::Kind k) {
    switch (k) {
    case Spirv::Kind::UniformBuffer:
        return vk::DescriptorType::eUniformBuffer;
    case Spirv::Kind::StorageBuffer:
        return vk::DescriptorType::eStorageBuffer;
    case Spirv::Kind::SampledImage:
        return vk::DescriptorType::eSampledImage;
    case Spirv::Kind::StorageImage:
        return vk::DescriptorType::eStorageImage;
    case Spirv::Kind::Sampler:
        return vk::DescriptorType::eSampler;
    case Spirv::Kind::UniformTexelBuffer:
        return vk::DescriptorType::eUniformTexelBuffer;
    default:
        return vk::DescriptorType::eStorageTexelBuffer;
    }
}

VkShaderModule MakeModule(const Vulkan::Instance& inst, const std::vector<u32>& code) {
    auto [result, module] = inst.GetDevice().createShaderModule(
        {.codeSize = code.size() * 4, .pCode = code.data()});
    return result == vk::Result::eSuccess ? VkShaderModule(module) : VK_NULL_HANDLE;
}

/// spirv/<name>.spv as a VkShaderModule on the presenter's device (once per shader), its
/// descriptors moved to the stage's set, and the stage's descriptor set layout.
void LoadModule(ShaderInfo& s) {
    const Vulkan::Instance* inst = instance.load();
    if (s.load_tried || !inst) {
        return;
    }
    s.load_tried = true;
    std::ifstream f(shader_root / "spirv" / (s.name + ".spv"), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    s.code.resize(bytes.size() / 4);
    std::memcpy(s.code.data(), bytes.data(), s.code.size() * 4);
    if (bytes.empty() || bytes.size() % 4 ||
        !Spirv::Prepare(s.code, StageSet(s), &s.bindings, &s.input_locations)) {
        modules_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("Native Vulkan: %s.spv missing or damaged\n", s.name.c_str());
        return;
    }
    s.module = MakeModule(*inst, s.code);
    if (!s.module) {
        modules_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("Native Vulkan: %s: vkCreateShaderModule failed\n", s.name.c_str());
        return;
    }
    std::vector<vk::DescriptorSetLayoutBinding> lb;
    for (const Spirv::Binding& b : s.bindings) {
        lb.push_back({.binding = b.binding,
                      .descriptorType = DescriptorType(b.kind),
                      .descriptorCount = b.count,
                      .stageFlags = StageBit(s)});
    }
    auto [lr, layout] = inst->GetDevice().createDescriptorSetLayout(
        {.bindingCount = u32(lb.size()), .pBindings = lb.data()});
    s.set_layout = lr == vk::Result::eSuccess ? VkDescriptorSetLayout(layout) : VK_NULL_HANDLE;
    modules_made.fetch_add(1, std::memory_order_relaxed);
}

/// The VS module with the inputs at `uint_mask` / `sint_mask` taking integer data.
VkShaderModule Variant(ShaderInfo& s, u32 uint_mask, u32 sint_mask) {
    uint_mask &= s.input_locations;
    sint_mask &= s.input_locations;
    if (!(uint_mask | sint_mask)) {
        return s.module;
    }
    const u64 key = uint_mask | u64(sint_mask) << 32;
    if (const auto it = s.variants.find(key); it != s.variants.end()) {
        return it->second;
    }
    std::vector<u32> code = s.code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (Spirv::IntegerInputs(code, uint_mask, sint_mask)) {
        m = MakeModule(*instance.load(), code);
    }
    if (!m) {
        variants_failed.fetch_add(1, std::memory_order_relaxed);
        std::printf("Native Vulkan: %s: no module for integer inputs %x/%x\n", s.name.c_str(),
                    uint_mask, sint_mask);
    }
    return s.variants[key] = m;
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

/// Integer color formats cannot blend.
bool IntegerFormat(vk::Format f) {
    using F = vk::Format;
    switch (f) {
    case F::eR8Uint:
    case F::eR8Sint:
    case F::eR16Uint:
    case F::eR16Sint:
    case F::eR8G8Uint:
    case F::eR32Uint:
    case F::eR32Sint:
    case F::eR16G16Uint:
    case F::eA2B10G10R10UintPack32:
    case F::eR8G8B8A8Uint:
    case F::eR32G32Uint:
    case F::eR16G16B16A16Uint:
    case F::eR32G32B32A32Uint:
        return true;
    default:
        return false;
    }
}

void DestroyTarget(const Vulkan::Instance& inst, Target& t) {
    const vk::Device dev = inst.GetDevice();
    const VmaAllocator vma = inst.GetAllocator();
    const VkImage image = t.img.image;
    const VmaAllocation allocation = t.allocation;
    std::vector<VkImageView> views{t.view};
    if (t.view_array) {
        views.push_back(t.view_array);
    }
    if (t.img.attach && t.img.attach != t.view) {
        views.push_back(t.img.attach);
    }
    auto destroy = [dev, vma, image, allocation, views] {
        for (const VkImageView v : views) {
            if (v) {
                dev.destroyImageView(v);
            }
        }
        if (image) {
            vmaDestroyImage(vma, image, allocation);
        }
    };
    if (drawing) {
        Gpu::Defer(destroy); // the GPU may still use it
    } else {
        destroy();
    }
    target_bytes.fetch_sub(t.bytes, std::memory_order_relaxed);
    t = Target{};
}

/// The image for the target at `base`, made again when its size or format changes.
Target* MakeTarget(u64 base, u32 w, u32 h, vk::Format format, bool depth, u32 comp_swap,
                   const char* what) {
    const Vulkan::Instance* inst = instance.load();
    if (!inst) {
        return nullptr;
    }
    Target& t = targets[base];
    if (t.img.image && t.img.width == w && t.img.height == h && t.img.format == format) {
        return &t;
    }
    if (t.img.image) {
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
        .usage = U::eSampled | U::eTransferDst | U::eTransferSrc |
                 (depth ? vk::ImageUsageFlags{U::eDepthStencilAttachment}
                        : vk::ImageUsageFlags{U::eColorAttachment}),
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
        std::printf("Native Vulkan: %s %llx %ux%u %s: image not created (%s)\n", what,
                    (unsigned long long)base, w, h, vk::to_string(format).c_str(),
                    vk::to_string(vk::Result(r)).c_str());
        targets.erase(base);
        return nullptr;
    }
    const bool stencil = format == vk::Format::eD32SfloatS8Uint ||
                         format == vk::Format::eD24UnormS8Uint;
    auto view_of = [&](vk::ImageViewType type, vk::ImageAspectFlags aspects) {
        auto [vr, view] = inst->GetDevice().createImageView({
            .image = image,
            .viewType = type,
            .format = format,
            .subresourceRange = {.aspectMask = aspects,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
        });
        return vr == vk::Result::eSuccess ? VkImageView(view) : VK_NULL_HANDLE;
    };
    const vk::ImageAspectFlags sampled = depth ? vk::ImageAspectFlagBits::eDepth
                                               : vk::ImageAspectFlagBits::eColor;
    t.img.image = image;
    t.allocation = allocation;
    t.view = view_of(vk::ImageViewType::e2D, sampled);
    t.view_array = view_of(vk::ImageViewType::e2DArray, sampled);
    t.img.attach = stencil ? view_of(vk::ImageViewType::e2D, vk::ImageAspectFlagBits::eDepth |
                                                                  vk::ImageAspectFlagBits::eStencil)
                           : t.view;
    t.img.width = w;
    t.img.height = h;
    t.img.format = format;
    t.img.depth = depth;
    t.img.stencil = stencil;
    t.img.layout = vk::ImageLayout::eUndefined;
    t.comp_swap = comp_swap;
    t.bytes = u64(w) * h * FormatBytes(format);
    target_bytes.fetch_add(t.bytes, std::memory_order_relaxed);
    targets_made.fetch_add(1, std::memory_order_relaxed);
    std::printf("Native Vulkan: %s %llx %ux%u %s swap %u%s\n", what, (unsigned long long)base, w, h,
                vk::to_string(format).c_str(), comp_swap, t.view && t.img.attach ? "" : " (no view)");
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
    return MakeTarget(base, w, h, format, false, (info >> 11) & 3, "color target");
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
    return MakeTarget(base, w, h, format, true, 0, "depth target");
}

/// Effective channel order the shader sees (the port's SwizzleCode): T# dst_sel applied to the
/// memory order of the data (render targets may have been written with a component swap).
/// Per component 0 = zero, 1 = one, 2..5 = logical x..w; 0 overall = identity.
u32 SwizzleCode(const u32* tsharp, const Target* target) {
    const Tex::Info t = Tex::Describe(tsharp);
    static constexpr u8 MemToLogical[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
    u32 swap = 0;
    // one- and two-channel formats (8, 16, 32, 8_8, 16_16, 32_32) have no swap to undo
    if (target && t.dfmt != 1 && t.dfmt != 2 && t.dfmt != 4 && t.dfmt != 3 && t.dfmt != 5 &&
        t.dfmt != 11) {
        swap = target->comp_swap;
    }
    u32 code = 0;
    for (int c = 0; c < 4; ++c) {
        const u32 d = (t.dst_sel >> (3 * c)) & 7;
        const u32 v = d == 0 ? 0 : d == 1 ? 1 : d >= 4 ? 2 + MemToLogical[swap][d - 4] : 2 + c;
        code |= v << (3 * c);
    }
    return code == (2 | 3 << 3 | 4 << 6 | 5 << 9) ? 0 : code;
}

/// The textures and samplers a shader stage samples. Texture slots hold T# pointers (unset: the
/// device's missing texture); a T# whose image is a render target uses that target's image.
/// Sampler slots hold packed keys, expanded into an S# the way the engine's commit code does
/// (the D3D11 port's BindTextures).
void BindStage(bool vertex, const ShaderInfo& s, u64 ctx, u64 dev, StageRes& res) {
    const u64 tex_slots = ctx + (vertex ? CtxVsTextures : CtxPsTextures);
    for (const auto& [slot, dims] : s.textures) {
        if (slot >= 16) {
            continue;
        }
        u64 t = Q(tex_slots + slot * 8);
        if (!t) {
            t = dev + DevMissingTexture;
            unset_textures.fetch_add(1, std::memory_order_relaxed);
        }
        u32 tsharp[8];
        if (!good_tsharps.count(t)) {
            if (!Read(t, tsharp, sizeof(tsharp))) {
                bad_tsharps.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            good_tsharps.insert(t);
        } else {
            std::memcpy(tsharp, reinterpret_cast<const void*>(t), sizeof(tsharp));
        }
        texture_binds.fetch_add(1, std::memory_order_relaxed);
        Slot& out = res.slots[slot];
        if (const auto it = targets.find(Tex::TSharpBase(tsharp)); it != targets.end()) {
            target_binds.fetch_add(1, std::memory_order_relaxed);
            out.target = &it->second;
        } else if (const Tex::Texture* tex = Tex::Get(tsharp)) {
            if (tex->ready.load(std::memory_order_acquire)) {
                out.texture = tex;
            } else {
                textures_not_ready.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            texture_misses.fetch_add(1, std::memory_order_relaxed);
        }
        res.swz[slot] = SwizzleCode(tsharp, out.target);
    }
    const u64 sampler_slots = ctx + (vertex ? CtxVsSamplers : CtxPsSamplers);
    for (const auto& [slot, compare] : s.samplers) {
        if (slot >= 16) {
            continue;
        }
        const u32 k = u32(Q(sampler_slots + slot * 8));
        u32 ss[4] = {0, 0xfff000, 0x5000000, 0};
        if (k && vertex) {
            ss[0] = (k >> 8) & 0xfff;
            ss[2] = (k & 0xf) << 20 | 0x4000000 | (k & 0xc0) << 18;
        } else if (k) {
            ss[0] = (k >> 8) & 0x7fff;
            ss[2] = (k & 0x30) << 22 | (k & 0xc0) << 18 | (k & 0xf) << 20;
        }
        sampler_binds.fetch_add(1, std::memory_order_relaxed);
        res.samplers[slot] = Tex::Sampler(ss, compare);
        if (!res.samplers[slot]) {
            sampler_misses.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

/// Gnm vertex DataFormat (surface format | channel type << 8) -> Vulkan, as the D3D11 port's
/// VertexFormat: integer data (uint/sint, and uscaled/sscaled) is fetched raw, and the shaders
/// convert scaled data themselves (their conversion modes, b13). `sign`: 1 unsigned, 2 signed
/// integer data, 0 otherwise.
vk::Format VertexFormat(u32 df, int* sign) {
    using F = vk::Format;
    const u32 sf = df & 0xff, ch = (df >> 8) & 0xf; // 0 unorm 1 snorm 2 uscaled 3 sscaled 4 uint
                                                    // 5 sint 6 snorm_nz 7 float
    const bool u = ch == 2 || ch == 4, s = ch == 3 || ch == 5, sn = ch == 1 || ch == 6, fl = ch == 7;
    *sign = u ? 1 : s ? 2 : 0;
    switch (sf) {
    case 4: // 32
        return fl ? F::eR32Sfloat : u ? F::eR32Uint : s ? F::eR32Sint : F::eUndefined;
    case 11: // 32_32
        return fl ? F::eR32G32Sfloat : u ? F::eR32G32Uint : s ? F::eR32G32Sint : F::eUndefined;
    case 13: // 32_32_32
        return fl ? F::eR32G32B32Sfloat : u ? F::eR32G32B32Uint : s ? F::eR32G32B32Sint
                                                                    : F::eUndefined;
    case 14: // 32_32_32_32
        return fl ? F::eR32G32B32A32Sfloat : u ? F::eR32G32B32A32Uint : s ? F::eR32G32B32A32Sint
                                                                          : F::eUndefined;
    case 2: // 16
        return fl ? F::eR16Sfloat : u ? F::eR16Uint : s ? F::eR16Sint : sn ? F::eR16Snorm
                                                                           : F::eR16Unorm;
    case 5: // 16_16
        return fl ? F::eR16G16Sfloat : u ? F::eR16G16Uint : s ? F::eR16G16Sint
               : sn ? F::eR16G16Snorm : F::eR16G16Unorm;
    case 12: // 16_16_16_16
        return fl ? F::eR16G16B16A16Sfloat : u ? F::eR16G16B16A16Uint : s ? F::eR16G16B16A16Sint
               : sn ? F::eR16G16B16A16Snorm : F::eR16G16B16A16Unorm;
    case 1: // 8
        return u ? F::eR8Uint : s ? F::eR8Sint : sn ? F::eR8Snorm : F::eR8Unorm;
    case 3: // 8_8
        return u ? F::eR8G8Uint : s ? F::eR8G8Sint : sn ? F::eR8G8Snorm : F::eR8G8Unorm;
    case 10: // 8_8_8_8
        return u ? F::eR8G8B8A8Uint : s ? F::eR8G8B8A8Sint : sn ? F::eR8G8B8A8Snorm
                                                               : F::eR8G8B8A8Unorm;
    case 8: // 10_10_10_2
    case 9: // 2_10_10_10
        return u ? F::eA2B10G10R10UintPack32 : s ? F::eA2B10G10R10SintPack32
                                                 : F::eA2B10G10R10UnormPack32;
    case 6: // 10_11_11
    case 7: // 11_11_10
        *sign = 0;
        return F::eB10G11R11UfloatPack32;
    default:
        return F::eUndefined;
    }
}

/// Binding of the inputs no engine vertex element feeds: a null buffer (zeros), as the port's
/// zero buffer in slot 15.
constexpr u32 ZeroBinding = 15;

/// Vertex streams and attributes of a draw, as the D3D11 port's GetLayout: each VS input
/// semantic is matched (by usage and usage index from the shader header) to an element of the
/// engine's vertex formats, which gives its stream slot, offset and data format. The streams are
/// slices of imported guest memory, read in place.
void FetchVertices(u64 ctx, u64 vsh, const ShaderInfo& vs, VertexInput& vi) {
    const u64 hdr = Q(vsh + ShHeader);
    const s32 nin = hdr ? R<s32>(hdr + HdrInputElements) : 0;
    const u64 ins = hdr ? Q(hdr + HdrInputElements + 0x10) : 0;
    u64 key = 0xcbf29ce484222325ull;
    bool used[16]{};
    u32 locations = vs.input_locations;
    if (!locations) { // a module that did not load: the index's inputs
        for (const auto& [sem, size] : vs.inputs) {
            locations |= sem < 32 ? 1u << sem : 0;
        }
    }
    bool zero_binding = false;
    for (u32 sem = 0; sem < 32; ++sem) {
        if (!(locations >> sem & 1)) {
            continue;
        }
        u32 usage = 0xff, uidx = 0xff;
        for (s32 k = 0; ins && k < nin && k < 64; ++k) {
            if (R<u32>(ins + k * 12 + 8) == sem) {
                usage = R<u32>(ins + k * 12);
                uidx = R<u32>(ins + k * 12 + 4);
            }
        }
        vk::Format format = vk::Format::eUndefined;
        int binding = -1;
        u32 offset = 0;
        for (int slot = 0; slot < 15 && usage != 0xff && binding < 0; ++slot) {
            const u64 vf = Q(ctx + CtxVertexFormats + slot * 8);
            const u64 el = vf ? Q(vf + 0x18) : 0, fm = vf ? Q(vf + 0x28) : 0;
            for (int k = 0; el && fm && k < 32 && R<u8>(el + k * 12) != ','; ++k) {
                if (R<u8>(el + k * 12 + 4) != usage || R<u8>(el + k * 12 + 5) != uidx) {
                    continue;
                }
                const u32 df = R<u32>(fm + k * 4);
                int sign = 0;
                format = VertexFormat(df, &sign);
                if (format == vk::Format::eUndefined) {
                    unknown_vertex_formats.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                if (sign) {
                    integer_attributes.fetch_add(1, std::memory_order_relaxed);
                    (sign == 1 ? vi.uint_mask : vi.sint_mask) |= 1u << sem;
                }
                const u32 ch = (df >> 8) & 0xf;
                vi.vin[sem] = ch == 2 ? 1 : ch == 3 ? 2 : 0;
                binding = slot;
                offset = R<u16>(el + k * 12 + 8);
                break;
            }
        }
        if (binding < 0) {
            missing_attributes.fetch_add(1, std::memory_order_relaxed); // the port feeds zeros
            binding = ZeroBinding;
            format = vk::Format::eR32G32B32A32Sfloat;
            offset = 0;
            zero_binding = true;
        } else {
            used[binding] = true;
        }
        vi.attributes.push_back({.location = sem, .binding = u32(binding), .format = format,
                                 .offset = offset});
        Mix(key, sem);
        Mix(key, format);
        Mix(key, binding);
        Mix(key, offset);
    }
    for (int slot = 0; slot < 15; ++slot) {
        if (!used[slot]) {
            continue;
        }
        vb_binds.fetch_add(1, std::memory_order_relaxed);
        // engine buffer resource: {u32 size; u64 address at +8}
        const u64 vb = Q(ctx + CtxVertexBuffers + slot * 8);
        const u64 base = vb ? Q(vb + 8) : 0;
        const u32 size = vb ? R<u32>(vb) : 0;
        const u32 stride = R<u32>(ctx + CtxVbStrides + slot * 4);
        const u32 first = R<u32>(ctx + CtxVbOffsets + slot * 4);
        Mix(key, stride);
        vi.bindings.push_back({.binding = u32(slot), .stride = stride,
                               .inputRate = vk::VertexInputRate::eVertex, .divisor = 1});
        if (!base || !size || !stride) {
            bad_vertex_buffers.fetch_add(1, std::memory_order_relaxed);
            vi.ok = false;
        } else if (const Buf::Slice s = Buf::Import(base, size); s.buffer) {
            vb_in_place.fetch_add(1, std::memory_order_relaxed);
            vi.buffers[slot] = s.buffer;
            vi.offsets[slot] = s.offset + first;
        } else {
            vb_copy_bytes.fetch_add(size, std::memory_order_relaxed);
            vi.ok = false; // no copy path yet: the import has not failed in any run so far
        }
    }
    if (zero_binding) {
        vi.bindings.push_back({.binding = ZeroBinding, .stride = 0,
                               .inputRate = vk::VertexInputRate::eVertex, .divisor = 1});
    }
    vi.key = key;
    vertex_layouts.insert(key);
}

// ------------------------------------------------------------------ fixed function state
vk::BlendFactor Blend(u32 f) {
    using B = vk::BlendFactor;
    static constexpr B t[] = {
        B::eZero,          B::eOne,                   B::eSrcColor,       B::eOneMinusSrcColor,
        B::eSrcAlpha,      B::eOneMinusSrcAlpha,      B::eDstAlpha,       B::eOneMinusDstAlpha,
        B::eDstColor,      B::eOneMinusDstColor,      B::eSrcAlphaSaturate, B::eOne,
        B::eOne,           B::eConstantColor,         B::eOneMinusConstantColor, B::eSrc1Color,
        B::eOneMinusSrc1Color, B::eSrc1Alpha,         B::eOneMinusSrc1Alpha, B::eConstantColor,
        B::eOneMinusConstantColor};
    return f < 21 ? t[f] : B::eOne;
}

/// Alpha factors may not reference color.
vk::BlendFactor AlphaFactor(vk::BlendFactor f) {
    using B = vk::BlendFactor;
    switch (f) {
    case B::eSrcColor:
        return B::eSrcAlpha;
    case B::eOneMinusSrcColor:
        return B::eOneMinusSrcAlpha;
    case B::eDstColor:
        return B::eDstAlpha;
    case B::eOneMinusDstColor:
        return B::eOneMinusDstAlpha;
    case B::eSrc1Color:
        return B::eSrc1Alpha;
    case B::eOneMinusSrc1Color:
        return B::eOneMinusSrc1Alpha;
    case B::eConstantColor:
        return B::eConstantAlpha;
    case B::eOneMinusConstantColor:
        return B::eOneMinusConstantAlpha;
    default:
        return f;
    }
}

vk::BlendOp BlendOp(u32 f) {
    switch (f) {
    case 1:
        return vk::BlendOp::eSubtract;
    case 2:
        return vk::BlendOp::eMin;
    case 3:
        return vk::BlendOp::eMax;
    case 4:
        return vk::BlendOp::eReverseSubtract;
    default:
        return vk::BlendOp::eAdd;
    }
}

vk::CompareOp Cmp(u32 f) {
    static constexpr vk::CompareOp t[] = {
        vk::CompareOp::eNever,   vk::CompareOp::eLess,     vk::CompareOp::eEqual,
        vk::CompareOp::eLessOrEqual, vk::CompareOp::eGreater, vk::CompareOp::eNotEqual,
        vk::CompareOp::eGreaterOrEqual, vk::CompareOp::eAlways};
    return t[f & 7];
}

vk::StencilOp StencilOp(u32 op) {
    switch (op) {
    case 1:
        return vk::StencilOp::eZero;
    case 2: // ONES
    case 3: // REPLACE_TEST
    case 4: // REPLACE_OP
        return vk::StencilOp::eReplace;
    case 5:
        return vk::StencilOp::eIncrementAndClamp;
    case 6:
        return vk::StencilOp::eDecrementAndClamp;
    case 7:
        return vk::StencilOp::eInvert;
    case 8:
        return vk::StencilOp::eIncrementAndWrap;
    case 9:
        return vk::StencilOp::eDecrementAndWrap;
    default:
        return vk::StencilOp::eKeep;
    }
}

/// What a draw's pipeline is made from.
struct PipelineState {
    ShaderInfo* vs = nullptr;
    VkShaderModule vs_module = VK_NULL_HANDLE;
    ShaderInfo* ps = nullptr;
    vk::Format colors[8]{};
    u32 color_count = 0;
    vk::Format depth = vk::Format::eUndefined;
    bool stencil = false, depth_read_only = false;
    u32 blend = 0, mask = 0, dctl = 0, refmask = 0, sops = 0, setup = 0;
    bool bias = false;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    const VertexInput* vertices = nullptr;

    u64 Key() const {
        u64 k = 0xcbf29ce484222325ull;
        Mix(k, vs_module);
        Mix(k, ps);
        Mix(k, colors);
        Mix(k, color_count);
        Mix(k, depth);
        Mix(k, depth_read_only);
        Mix(k, blend);
        Mix(k, mask);
        Mix(k, depth != vk::Format::eUndefined ? dctl : 0u);
        Mix(k, (dctl & 1) && depth != vk::Format::eUndefined ? (refmask & 0xffff00) | (u64(sops) << 32)
                                                            : 0ull);
        Mix(k, setup & 0x1807); // cull, front face, polygon offset enables
        Mix(k, topology);
        if (!dynamic_vertex_input) {
            Mix(k, vertices->key);
        }
        return k;
    }
};

VkPipelineLayout PipelineLayout(const ShaderInfo& vs, const ShaderInfo* ps) {
    const VkDescriptorSetLayout sets[2] = {vs.set_layout, ps ? ps->set_layout : empty_set_layout};
    u64 key = 0xcbf29ce484222325ull;
    Mix(key, sets);
    if (const auto it = pipeline_layouts.find(key); it != pipeline_layouts.end()) {
        return it->second;
    }
    const vk::DescriptorSetLayout vk_sets[2] = {sets[0], sets[1]};
    auto [r, layout] = instance.load()->GetDevice().createPipelineLayout(
        {.setLayoutCount = 2, .pSetLayouts = vk_sets});
    return pipeline_layouts[key] = r == vk::Result::eSuccess ? VkPipelineLayout(layout) : VK_NULL_HANDLE;
}

/// A graphics pipeline for the engine state of a draw (the D3D11 port's GetBlend, GetDepthState
/// and GetRaster, in Vulkan terms). Viewport, scissor, depth bias values, stencil reference and,
/// where supported, the vertex layout are dynamic.
VkPipeline MakePipeline(const PipelineState& s, VkPipelineLayout layout) {
    const Vulkan::Instance* inst = instance.load();
    std::vector<vk::PipelineShaderStageCreateInfo> stages{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = s.vs_module, .pName = "main"}};
    if (s.ps) {
        stages.push_back(
            {.stage = vk::ShaderStageFlagBits::eFragment, .module = s.ps->module, .pName = "main"});
    }
    std::vector<vk::VertexInputBindingDescription> vb;
    std::vector<vk::VertexInputAttributeDescription> va;
    if (!dynamic_vertex_input) {
        for (const auto& b : s.vertices->bindings) {
            vb.push_back({b.binding, b.stride, b.inputRate});
        }
        for (const auto& a : s.vertices->attributes) {
            va.push_back({a.location, a.binding, a.format, a.offset});
        }
    }
    const vk::PipelineVertexInputStateCreateInfo vertex_input{
        .vertexBindingDescriptionCount = u32(vb.size()),
        .pVertexBindingDescriptions = vb.data(),
        .vertexAttributeDescriptionCount = u32(va.size()),
        .pVertexAttributeDescriptions = va.data()};
    const bool strip = s.topology == vk::PrimitiveTopology::eLineStrip ||
                       s.topology == vk::PrimitiveTopology::eTriangleStrip;
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = s.topology,
                                                            .primitiveRestartEnable = strip};
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    // PA_SU_SC_MODE_CNTL: CULL_FRONT [0] CULL_BACK [1] FACE [2] (0: counter-clockwise is front),
    // POLY_OFFSET_FRONT/BACK_ENABLE [11-12]. With a negative-height viewport, winding is as in D3D.
    const bool cf = s.setup & 1, cb = (s.setup >> 1) & 1;
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = cf && cb ? vk::CullModeFlagBits::eNone
                    : cf     ? vk::CullModeFlagBits::eFront
                    : cb     ? vk::CullModeFlagBits::eBack
                             : vk::CullModeFlagBits::eNone,
        .frontFace = (s.setup >> 2) & 1 ? vk::FrontFace::eClockwise : vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = s.bias,
        .lineWidth = 1.0f};
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1};
    // DB_DEPTH_CONTROL: STENCIL_ENABLE [0] Z_ENABLE [1] Z_WRITE_ENABLE [2] ZFUNC [6:4]
    //   BACKFACE_ENABLE [7] STENCILFUNC [10:8] STENCILFUNC_BF [22:20]
    // DB_STENCILREFMASK (ctx+0x114): TESTVAL [7:0] MASK [15:8] WRITEMASK [23:16] OPVAL [31:24]
    // DB_STENCIL_CONTROL (ctx+0x118): FAIL [3:0] ZPASS [7:4] ZFAIL [11:8] and back-face copies
    const bool has_depth = s.depth != vk::Format::eUndefined;
    const bool stencil_on = has_depth && s.stencil && (s.dctl & 1);
    const u32 write_mask = s.depth_read_only ? 0 : (s.refmask >> 16) & 0xff;
    const vk::StencilOpState front{.failOp = StencilOp(s.sops & 0xf),
                                   .passOp = StencilOp((s.sops >> 4) & 0xf),
                                   .depthFailOp = StencilOp((s.sops >> 8) & 0xf),
                                   .compareOp = Cmp((s.dctl >> 8) & 7),
                                   .compareMask = (s.refmask >> 8) & 0xff,
                                   .writeMask = write_mask};
    const vk::StencilOpState back = (s.dctl >> 7) & 1
                                        ? vk::StencilOpState{.failOp = StencilOp((s.sops >> 12) & 0xf),
                                                             .passOp = StencilOp((s.sops >> 16) & 0xf),
                                                             .depthFailOp = StencilOp((s.sops >> 20) & 0xf),
                                                             .compareOp = Cmp((s.dctl >> 20) & 7),
                                                             .compareMask = (s.refmask >> 8) & 0xff,
                                                             .writeMask = write_mask}
                                        : front;
    const vk::PipelineDepthStencilStateCreateInfo depth{
        .depthTestEnable = has_depth && ((s.dctl >> 1) & 1),
        .depthWriteEnable = has_depth && !s.depth_read_only && ((s.dctl >> 2) & 1),
        .depthCompareOp = Cmp((s.dctl >> 4) & 7),
        .stencilTestEnable = stencil_on,
        .front = front,
        .back = back};
    // CB_BLEND_CONTROL: SRC [4:0] OP [7:5] DST [12:8], alpha copies at 16/21/24, SEPARATE_ALPHA
    // [29], ENABLE [30]; the same for every target. Write masks: 4 bits per target.
    std::array<vk::PipelineColorBlendAttachmentState, 8> blends{};
    for (u32 i = 0; i < s.color_count; ++i) {
        auto& b = blends[i];
        b.blendEnable = ((s.blend >> 30) & 1) && s.colors[i] != vk::Format::eUndefined &&
                        !IntegerFormat(s.colors[i]);
        b.srcColorBlendFactor = Blend(s.blend & 0x1f);
        b.colorBlendOp = BlendOp((s.blend >> 5) & 7);
        b.dstColorBlendFactor = Blend((s.blend >> 8) & 0x1f);
        if ((s.blend >> 29) & 1) {
            b.srcAlphaBlendFactor = Blend((s.blend >> 16) & 0x1f);
            b.alphaBlendOp = BlendOp((s.blend >> 21) & 7);
            b.dstAlphaBlendFactor = Blend((s.blend >> 24) & 0x1f);
        } else {
            b.srcAlphaBlendFactor = b.srcColorBlendFactor;
            b.alphaBlendOp = b.colorBlendOp;
            b.dstAlphaBlendFactor = b.dstColorBlendFactor;
        }
        b.srcAlphaBlendFactor = AlphaFactor(b.srcAlphaBlendFactor);
        b.dstAlphaBlendFactor = AlphaFactor(b.dstAlphaBlendFactor);
        b.colorWriteMask = vk::ColorComponentFlags((s.mask >> (i * 4)) & 0xf);
    }
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = s.color_count,
                                                      .pAttachments = blends.data(),
                                                      .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f}};
    std::vector<vk::DynamicState> dyn{vk::DynamicState::eViewport, vk::DynamicState::eScissor,
                                      vk::DynamicState::eDepthBias,
                                      vk::DynamicState::eStencilReference};
    if (dynamic_vertex_input) {
        dyn.push_back(vk::DynamicState::eVertexInputEXT);
    }
    const vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount = u32(dyn.size()),
                                                     .pDynamicStates = dyn.data()};
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = s.color_count,
        .pColorAttachmentFormats = s.colors,
        .depthAttachmentFormat = s.depth,
        .stencilAttachmentFormat = s.stencil ? s.depth : vk::Format::eUndefined};
    const vk::GraphicsPipelineCreateInfo info{
        .pNext = &rendering,
        .stageCount = u32(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout};
    auto [r, pipeline] = inst->GetDevice().createGraphicsPipeline(Gpu::PipelineCache(), info);
    if (r != vk::Result::eSuccess) {
        if (pipelines_failed.fetch_add(1, std::memory_order_relaxed) < 16) {
            std::printf("Native Vulkan: pipeline %s + %s not created (%s)\n", s.vs->name.c_str(),
                        s.ps ? s.ps->name.c_str() : "-", vk::to_string(r).c_str());
        }
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// ------------------------------------------------------------------ constants
/// Copies a stage's constant buffers (the shader header's vectors) into the upload ring; the
/// spans go to `cbs` by slot. False: the ring could not take them.
bool StageConstants(u64 shader, Gpu::Span cbs[14], u32 sizes[14]) {
    const u64 hdr = Q(shader + ShHeader);
    if (!hdr) {
        return true;
    }
    const s32 n = R<s32>(hdr + HdrCbMemory);
    const u64 mem = Q(hdr + HdrCbMemory + 0x10), size_of = Q(hdr + HdrCbSize + 0x10),
              idx = Q(hdr + HdrCbIndex + 0x10);
    for (s32 k = 0; k < n && k < 16; ++k) {
        const u64 data = Q(mem + k * 8);
        const u32 size = u32(R<u64>(size_of + k * 8));
        const u32 slot = R<u32>(idx + k * 4);
        if (slot >= 13 || !data || !size || size > 65536) {
            continue;
        }
        const Gpu::Span span = Gpu::Upload((size + 15) & ~15u, ubo_align);
        if (!span.data) {
            return false;
        }
        std::memcpy(span.data, reinterpret_cast<const void*>(data), size);
        cbs[slot] = span;
        sizes[slot] = (size + 15) & ~15u;
    }
    return true;
}

bool InfoConstants(const StageRes& res, const u32* vin, Gpu::Span* span) {
    *span = Gpu::Upload(sizeof(InfoCB), ubo_align);
    if (!span->data) {
        return false;
    }
    InfoCB info{};
    std::memcpy(info.swz, res.swz, sizeof(info.swz));
    if (vin) {
        std::memcpy(info.vin, vin, sizeof(info.vin));
    }
    info.scale[0] = info.scale[1] = 1.0f; // render scaling comes with presentation
    std::fill(std::begin(info.tscale), std::end(info.tscale), 1.0f);
    std::memcpy(span->data, &info, sizeof(info));
    return true;
}

/// Writes a stage's descriptor set from what it declares (bindings b# 0.., t# 64.., s# 128..).
vk::DescriptorSet WriteStageSet(const ShaderInfo& s, const StageRes& res, const Gpu::Span cbs[14],
                           const u32 sizes[14], const Target* depth_ro) {
    const vk::DescriptorSet set = Gpu::AllocateSet(s.set_layout);
    if (!set) {
        return set;
    }
    static const u32 default_ss[4] = {0, 0xfff000, 0x5000000, 0};
    std::vector<vk::DescriptorBufferInfo> buffers;
    std::vector<vk::DescriptorImageInfo> images;
    buffers.reserve(s.bindings.size() * 4);
    images.reserve(s.bindings.size() * 4);
    std::vector<vk::WriteDescriptorSet> writes;
    for (const Spirv::Binding& b : s.bindings) {
        vk::WriteDescriptorSet w{.dstSet = set,
                                 .dstBinding = b.binding,
                                 .descriptorCount = b.count,
                                 .descriptorType = DescriptorType(b.kind)};
        switch (b.kind) {
        case Spirv::Kind::UniformBuffer: {
            const std::size_t first = buffers.size();
            for (u32 i = 0; i < b.count; ++i) {
                const u32 slot = b.binding + i;
                if (slot < 14 && cbs[slot].buffer) {
                    buffers.push_back({cbs[slot].buffer, cbs[slot].offset, sizes[slot]});
                } else {
                    buffers.push_back({VK_NULL_HANDLE, 0, VK_WHOLE_SIZE}); // reads zeros
                }
            }
            w.pBufferInfo = &buffers[first];
            break;
        }
        case Spirv::Kind::SampledImage: {
            const std::size_t first = images.size();
            for (u32 i = 0; i < b.count; ++i) {
                const u32 slot = b.binding + i - 64;
                vk::DescriptorImageInfo ii{.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
                if (slot < 32 && b.dim == 1) { // 2D only: no 3D or cube targets/textures yet
                    if (const Target* t = res.slots[slot].target) {
                        ii.imageView = b.arrayed ? t->view_array : t->view;
                        if (t == depth_ro) {
                            ii.imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
                        }
                    } else if (const Tex::Texture* tex = res.slots[slot].texture;
                               tex && (b.arrayed || tex->slices == 1)) {
                        // a plain 2D binding takes only one-slice textures (view types must match)
                        ii.imageView = b.arrayed ? tex->view_array : tex->view;
                    }
                }
                images.push_back(ii); // no image: a null descriptor, which reads zeros
            }
            w.pImageInfo = &images[first];
            break;
        }
        case Spirv::Kind::Sampler: {
            const std::size_t first = images.size();
            for (u32 i = 0; i < b.count; ++i) {
                const u32 slot = b.binding + i - 128;
                VkSampler smp = slot < 16 ? res.samplers[slot] : VK_NULL_HANDLE;
                if (!smp) {
                    smp = Tex::Sampler(default_ss, false); // samplers cannot be null
                }
                images.push_back({.sampler = smp});
            }
            w.pImageInfo = &images[first];
            break;
        }
        case Spirv::Kind::StorageBuffer: {
            const std::size_t first = buffers.size();
            for (u32 i = 0; i < b.count; ++i) {
                buffers.push_back({VK_NULL_HANDLE, 0, VK_WHOLE_SIZE});
            }
            w.pBufferInfo = &buffers[first];
            break;
        }
        case Spirv::Kind::StorageImage: {
            const std::size_t first = images.size();
            for (u32 i = 0; i < b.count; ++i) {
                images.push_back({.imageLayout = vk::ImageLayout::eGeneral});
            }
            w.pImageInfo = &images[first];
            break;
        }
        default: // texel buffers: none bound
            continue;
        }
        writes.push_back(w);
    }
    instance.load()->GetDevice().updateDescriptorSets(writes, {});
    return set;
}

// ------------------------------------------------------------------ the draw
vk::PrimitiveTopology Topology(u32 prim) {
    switch (prim) {
    case 0:
        return vk::PrimitiveTopology::ePointList;
    case 1:
        return vk::PrimitiveTopology::eLineList;
    case 2:
        return vk::PrimitiveTopology::eLineStrip;
    case 4:
        return vk::PrimitiveTopology::eTriangleStrip;
    default: // 3 list, 5 fan and 6 quads (as generated lists), anything else as the port
        return vk::PrimitiveTopology::eTriangleList;
    }
}

void Draw(u64 ctx, u64 dev, u64 vsh, ShaderInfo& vs, u64 psh, ShaderInfo* ps, Target* colors[8],
          Target* depth_target, bool indexed, u32 count, u64 index_addr) {
    if (!count || !vs.module || !vs.set_layout || (ps && (!ps->module || !ps->set_layout))) {
        skipped_pipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    StageRes vres, pres;
    BindStage(true, vs, ctx, dev, vres);
    if (ps) {
        BindStage(false, *ps, ctx, dev, pres);
    }
    VertexInput vi;
    FetchVertices(ctx, vsh, vs, vi);
    if (!vi.ok) {
        skipped_vertices.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // attachments; a sampled target cannot be a color attachment of the same draw, and a sampled
    // depth target is bound read-only (or, when the draw writes depth, not at all, as the port)
    Gpu::Pass pass;
    const u32 dctl = R<u32>(ctx + CtxDepthStencilControl);
    const bool zwrite = (dctl >> 2) & 1;
    for (int i = 0; i < 8; ++i) {
        pass.color[i] = colors[i] ? &colors[i]->img : nullptr;
    }
    pass.depth = depth_target ? &depth_target->img : nullptr;
    const Target* depth_ro = nullptr;
    for (StageRes* res : {&vres, &pres}) {
        for (Slot& slot : res->slots) {
            Target* t = slot.target;
            if (!t) {
                continue;
            }
            if (std::find(colors, colors + 8, t) != colors + 8) {
                color_feedback.fetch_add(1, std::memory_order_relaxed);
                slot.target = nullptr;
            } else if (t == depth_target) {
                if (zwrite) {
                    pass.depth = nullptr;
                    depth_dropped.fetch_add(1, std::memory_order_relaxed);
                } else {
                    pass.depth_read_only = true;
                    depth_ro = t;
                    depth_read_only.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    if (!pass.depth) {
        depth_ro = nullptr;
        pass.depth_read_only = false;
    }

    // pipeline
    PipelineState st;
    st.vs = &vs;
    st.vs_module = Variant(vs, vi.uint_mask, vi.sint_mask);
    st.ps = ps;
    for (u32 i = 0; i < 8; ++i) {
        if (pass.color[i]) {
            st.colors[i] = pass.color[i]->format;
            st.color_count = i + 1;
        }
    }
    if (pass.depth) {
        st.depth = pass.depth->format;
        st.stencil = pass.depth->stencil;
        st.depth_read_only = pass.depth_read_only;
    }
    st.blend = R<u32>(ctx + CtxBlendControl);
    st.mask = R<u32>(ctx + CtxRtMask);
    st.dctl = dctl;
    st.refmask = R<u32>(ctx + CtxStencilControl);
    st.sops = R<u32>(ctx + CtxStencilOpControl);
    st.setup = R<u32>(ctx + CtxPrimSetup);
    st.bias = (st.setup >> 11) & 3;
    const u32 prim = R<u32>(ctx + CtxPrimType);
    if (prim > 6 && logged_prims.insert(prim).second) {
        std::printf("Native Vulkan: primitive type %u (drawn as a triangle list, as the port)\n", prim);
    }
    st.topology = Topology(prim);
    st.vertices = &vi;
    if (!st.vs_module) {
        skipped_pipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 key = st.Key();
    pipeline_keys.insert(key);
    const VkPipelineLayout layout = PipelineLayout(vs, ps);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (const auto it = pipelines.find(key); it != pipelines.end()) {
        pipeline = it->second;
    } else {
        pipeline = pipelines[key] = layout ? MakePipeline(st, layout) : VK_NULL_HANDLE;
    }
    if (!pipeline) {
        skipped_pipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // everything taken from the upload ring first: a full ring submits early, which ends the
    // command buffer, so nothing may be bound before
    Gpu::Span vcb[14]{}, pcb[14]{};
    u32 vsz[14]{}, psz[14]{};
    if (!StageConstants(vsh, vcb, vsz) || !InfoConstants(vres, vi.vin, &vcb[13]) ||
        (ps && (!StageConstants(psh, pcb, psz) || !InfoConstants(pres, nullptr, &pcb[13])))) {
        skipped_memory.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    vsz[13] = psz[13] = sizeof(InfoCB);
    VkBuffer index_buffer = VK_NULL_HANDLE;
    u64 index_offset = 0;
    vk::IndexType index_type = vk::IndexType::eUint32;
    u32 index_count = count;
    const u32 isize = R<u32>(ctx + CtxIndexSize) == 2 ? 2 : 4;
    if (prim == 5 || prim == 6) { // fans and quads: triangle-list indices made here
        generated_index_draws.fetch_add(1, std::memory_order_relaxed);
        auto src = [&](u32 i) -> u32 {
            if (!indexed) {
                return i;
            }
            return isize == 2 ? R<std::uint16_t>(index_addr + i * 2) : R<u32>(index_addr + i * 4);
        };
        std::vector<u32> gen;
        if (prim == 5) {
            for (u32 i = 1; i + 1 < count; ++i) {
                gen.insert(gen.end(), {src(0), src(i), src(i + 1)});
            }
        } else {
            for (u32 i = 0; i + 3 < count; i += 4) {
                gen.insert(gen.end(), {src(i), src(i + 1), src(i + 2), src(i), src(i + 2), src(i + 3)});
            }
        }
        if (gen.empty()) {
            return;
        }
        const Gpu::Span span = Gpu::Upload(gen.size() * 4, 4);
        if (!span.data) {
            skipped_memory.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::memcpy(span.data, gen.data(), gen.size() * 4);
        ib_copy_bytes.fetch_add(gen.size() * 4, std::memory_order_relaxed);
        index_buffer = span.buffer;
        index_offset = span.offset;
        index_count = u32(gen.size());
        indexed = true;
    } else if (indexed) {
        ib_draws.fetch_add(1, std::memory_order_relaxed);
        const u64 bytes = u64(count) * isize;
        const u64 ib = Q(ctx + CtxIndexBuffer);
        const u64 ib_base = ib ? Q(ib + 8) : 0;
        const u32 ib_size = ib ? R<u32>(ib) : 0;
        // the whole engine index buffer when the draw's indices lie in it (one window for all
        // its draws), else just the draw's indices
        const bool inside = ib_base && index_addr >= ib_base && index_addr + bytes <= ib_base + ib_size;
        const Buf::Slice s = inside ? Buf::Import(ib_base, ib_size) : Buf::Import(index_addr, bytes);
        const u64 offset = s.offset + (inside ? index_addr - ib_base : 0);
        if (s.buffer && offset % isize == 0) {
            ib_in_place.fetch_add(1, std::memory_order_relaxed);
            index_buffer = s.buffer;
            index_offset = offset;
        } else {
            const Gpu::Span span = Gpu::Upload(bytes, 4);
            if (!span.data || !Read(index_addr, span.data, bytes)) {
                skipped_indices.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            ib_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
            index_buffer = span.buffer;
            index_offset = span.offset;
        }
        index_type = isize == 2 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    }

    // layouts of the sampled targets, then the pass
    for (const StageRes* res : {&vres, &pres}) {
        for (const Slot& slot : res->slots) {
            if (slot.target && slot.target != depth_ro) {
                Gpu::Use(slot.target->img, vk::ImageLayout::eShaderReadOnlyOptimal);
            }
        }
    }
    Gpu::Render(pass);
    if (!Gpu::Rendering()) {
        skipped_pipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const vk::DescriptorSet vset = WriteStageSet(vs, vres, vcb, vsz, depth_ro);
    const vk::DescriptorSet pset = ps ? WriteStageSet(*ps, pres, pcb, psz, depth_ro) : vk::DescriptorSet{};
    if (!vset || (ps && !pset)) {
        skipped_memory.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const vk::CommandBuffer cmd = Gpu::Cmd();
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    const vk::DescriptorSet sets[2] = {vset, pset};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, ps ? 2 : 1, sets, 0, nullptr);
    // viewport and scissor: the engine counts y from the bottom of the target (the shadow
    // cascades of the 2048x8192 atlas are at y 6144, 4096, 2048); a negative height flips y as
    // in D3D
    const ViewportI vp = R<ViewportI>(ctx + CtxViewport);
    const u32 target_h = depth_target ? depth_target->img.height
                         : pass.color[0] ? pass.color[0]->height
                                         : 0;
    // the rendering area: the smallest attachment (as Gpu::Render)
    u32 area_w = ~0u, area_h = ~0u;
    for (const Gpu::Image* c : pass.color) {
        if (c) {
            area_w = std::min(area_w, c->width);
            area_h = std::min(area_h, c->height);
        }
    }
    if (pass.depth) {
        area_w = std::min(area_w, pass.depth->width);
        area_h = std::min(area_h, pass.depth->height);
    }
    const s32 top = target_h && vp.y + vp.h <= s32(target_h) ? s32(target_h) - vp.y - vp.h : vp.y;
    const vk::Viewport viewport{float(vp.x), float(top + vp.h), float(vp.w), -float(vp.h),
                                std::clamp(vp.zmin, 0.0f, 1.0f), std::clamp(vp.zmax, 0.0f, 1.0f)};
    cmd.setViewport(0, viewport);
    // drawing outside the rendering area is undefined in Vulkan: the scissor stays inside it
    s32 x0 = 0, y0 = 0, x1 = s32(area_w), y1 = s32(area_h);
    if (R<u8>(ctx + CtxScissorEnabled)) {
        // bottom-up too (the GUI clips the sliding values of its option selectors with it)
        const ViewportI sc = R<ViewportI>(ctx + CtxScissor);
        const s32 sy = target_h ? s32(target_h) - sc.y - sc.h : sc.y;
        x0 = std::clamp(sc.x, 0, x1);
        y0 = std::clamp(sy, 0, y1);
        x1 = std::clamp(sc.x + sc.w, x0, x1);
        y1 = std::clamp(sy + sc.h, y0, y1);
    }
    const vk::Rect2D scissor{{x0, y0}, {u32(x1 - x0), u32(y1 - y0)}};
    cmd.setScissor(0, scissor);
    // the engine's packed rasterizer state (ctx+0x190): bit 4 sign + bits 5-30 polygon offset
    // scale (1/16 units), bits 31-62 the offset (float bits)
    float bias_units = 0.0f, bias_slope = 0.0f;
    if (st.bias) {
        const u64 rkey = Q(ctx + CtxRasterState);
        bias_slope = float((rkey >> 5) & 0x3ffffff) * ((rkey & 0x10) ? -1.0f : 1.0f) / 16.0f;
        const u32 obits = u32(rkey >> 31);
        float units;
        std::memcpy(&units, &obits, 4);
        bias_units = float(s32(units));
    }
    cmd.setDepthBias(bias_units, 0.0f, bias_slope);
    const bool uses_opval = (st.sops & 0xf) == 4 || ((st.sops >> 4) & 0xf) == 4 ||
                            ((st.sops >> 8) & 0xf) == 4;
    cmd.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                            uses_opval ? st.refmask >> 24 : st.refmask & 0xff);
    if (dynamic_vertex_input) {
        cmd.setVertexInputEXT(vi.bindings, vi.attributes);
    }
    for (const auto& b : vi.bindings) {
        const vk::Buffer buffer = b.binding == ZeroBinding ? vk::Buffer{} : vk::Buffer{vi.buffers[b.binding]};
        const vk::DeviceSize offset = b.binding == ZeroBinding ? 0 : vi.offsets[b.binding];
        cmd.bindVertexBuffers(b.binding, 1, &buffer, &offset);
    }
    if (indexed) {
        cmd.bindIndexBuffer(index_buffer, index_offset, index_type);
        cmd.drawIndexed(index_count, 1, 0, 0, 0);
    } else {
        cmd.draw(count, 1, 0, 0);
    }
    drawn.fetch_add(1, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ clears and copies
/// kClearRenderTarget(ctx, flags, float4* color, stencil, float depth): flags 1 color (every
/// bound target), 2 depth, 4 stencil. Runs before the engine's own clear.
__attribute__((sysv_abi)) void OnClear(u64 ctx, u32 flags, const float* color, u32 stencil,
                                       float depth) {
    if (!drawing || !ctx) {
        return;
    }
    Timer timer(clear_ns);
    std::lock_guard lk(mutex);
    clears.fetch_add(1, std::memory_order_relaxed);
    if (flags & 6) {
        if (const u64 dh = Q(ctx + CtxDepthTarget)) {
            if (Target* d = DepthTarget(dh)) {
                Gpu::ClearDepthStencil(d->img, flags & 2, depth, flags & 4, stencil & 0xff);
            }
        }
    }
    if ((flags & 1) && color) {
        float c[4];
        std::memcpy(c, color, sizeof(c));
        for (int i = 0; i < 8; ++i) {
            if (const u64 h = Q(ctx + CtxColorTargets + i * 8)) {
                if (Target* t = ColorTarget(h)) {
                    Gpu::ClearColor(t->img, c);
                }
            }
        }
    }
}

/// kCopyTexture(ctx, const T# src, const T# dst, const s32 p[8]): p = src x, y, dst x, y, width,
/// height (-1: all), src mip, dst mip, with y counted from the bottom like the viewport. The PS4
/// runs a copy compute shader; here it is an image copy between our images, the destination
/// becoming one of them (the D3D11 port's CopyTextureHook). Mip 0 only; a depth source goes to a
/// color destination of the same texel size (depth -> R32F) through the upload ring.
__attribute__((sysv_abi)) void OnCopy(u64 ctx, const u32* src, const u32* dst, const s32* p) {
    (void)ctx;
    if (!drawing || !src || !dst || !p) {
        return;
    }
    s32 a[8];
    std::memcpy(a, p, sizeof(a));
    if (a[0] < 0 || a[1] < 0 || a[2] < 0 || a[3] < 0) {
        return;
    }
    Timer timer(copy_ns);
    std::lock_guard lk(mutex);
    const Tex::Info s = Tex::Describe(src), d = Tex::Describe(dst);
    const auto it = targets.find(s.base);
    const bool from_depth = it != targets.end() && it->second.img.depth;
    const u32 src_bytes = it == targets.end() ? 0
                          : from_depth ? (it->second.img.format == vk::Format::eD16Unorm ? 2 : 4)
                                       : FormatBytes(it->second.img.format);
    int why = SkipCount;
    if (a[6] || a[7]) {
        why = SkipMip;
    } else if (it == targets.end()) {
        why = SkipSource;
    } else if (d.base == s.base) {
        why = SkipSame;
    } else if (d.format == vk::Format::eUndefined) {
        why = SkipDest;
    } else if (FormatBytes(d.format) != src_bytes) {
        why = SkipFormat;
    }
    auto skip = [&](int reason) {
        copies_skipped.fetch_add(1, std::memory_order_relaxed);
        if (copy_skips[reason].fetch_add(1, std::memory_order_relaxed) < 4) {
            std::printf("Native Vulkan: copy %llx (%ux%u mip %d) -> %llx (%ux%u mip %d %s) not done: "
                        "%s\n",
                        (unsigned long long)s.base, s.width, s.height, a[6],
                        (unsigned long long)d.base, d.width, d.height, a[7],
                        vk::to_string(d.format).c_str(), CopySkipNames[reason]);
        }
    };
    if (why != SkipCount) {
        skip(why);
        return;
    }
    Target& st = it->second;
    Target* dt = MakeTarget(d.base, d.width, d.height, d.format, false,
                            from_depth ? 0 : st.comp_swap, "copy target");
    if (!dt || dt == &st) {
        skip(SkipDest);
        return;
    }
    const u32 sw = st.img.width, sh = st.img.height, dw = dt->img.width, dh = dt->img.height;
    u32 w = a[4] >= 0 ? u32(a[4]) : sw, h = a[5] >= 0 ? u32(a[5]) : sh;
    const u32 sx = std::min<u32>(a[0], sw - 1), sy = std::min<u32>(a[1], sh - 1);
    w = std::min(w, sw - sx);
    h = std::min(h, sh - sy);
    const u32 dx = std::min<u32>(a[2], dw - 1), dy = std::min<u32>(a[3], dh - 1);
    w = std::min(w, dw - dx);
    h = std::min(h, dh - dy);
    if (!w || !h) {
        return;
    }
    const s32 s_top = s32(sh - sy - h), d_top = s32(dh - dy - h);
    Gpu::Span span{};
    if (from_depth) { // the ring first: a full ring submits early
        span = Gpu::Upload(u64(w) * h * src_bytes, 16);
        if (!span.data) {
            skip(SkipFormat);
            return;
        }
    }
    Gpu::Use(st.img, vk::ImageLayout::eTransferSrcOptimal);
    Gpu::Use(dt->img, vk::ImageLayout::eTransferDstOptimal);
    Gpu::EndRendering();
    Gpu::Barrier();
    if (from_depth) {
        const vk::BufferImageCopy out{.bufferOffset = span.offset,
                                      .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
                                      .imageOffset = {s32(sx), s_top, 0},
                                      .imageExtent = {w, h, 1}};
        Gpu::Cmd().copyImageToBuffer(st.img.image, vk::ImageLayout::eTransferSrcOptimal,
                                     span.buffer, out);
        const vk::BufferMemoryBarrier2 ready{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = span.buffer,
            .offset = span.offset,
            .size = u64(w) * h * src_bytes};
        Gpu::Cmd().pipelineBarrier2({.bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &ready});
        const vk::BufferImageCopy in{.bufferOffset = span.offset,
                                     .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                     .imageOffset = {s32(dx), d_top, 0},
                                     .imageExtent = {w, h, 1}};
        Gpu::Cmd().copyBufferToImage(span.buffer, dt->img.image,
                                     vk::ImageLayout::eTransferDstOptimal, in);
        depth_copies.fetch_add(1, std::memory_order_relaxed);
    } else {
        const vk::ImageCopy region{.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .srcOffset = {s32(sx), s_top, 0},
                                   .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                   .dstOffset = {s32(dx), d_top, 0},
                                   .extent = {w, h, 1}};
        Gpu::Cmd().copyImage(st.img.image, vk::ImageLayout::eTransferSrcOptimal, dt->img.image,
                             vk::ImageLayout::eTransferDstOptimal, region);
    }
    copies.fetch_add(1, std::memory_order_relaxed);
}

/// Starts the texture, buffer and drawing side once both the switch is on and the presenter's
/// instance exists (the loader and the GnmDriver registration can come in either order).
void StartTextures() {
    static std::once_flag once;
    const Vulkan::Instance* inst = instance.load();
    if (enabled && inst) {
        std::call_once(once, [inst] {
            Tex::Init(*inst);
            Buf::Init(*inst);
            const char* env = std::getenv("BB_VK_DRAW");
            if (env && env[0] == '0') {
                std::printf("Native Vulkan: BB_VK_DRAW=0: nothing drawn (stage 4)\n");
                return;
            }
            const vk::Device dev = inst->GetDevice();
            auto [er, empty] = dev.createDescriptorSetLayout({});
            empty_set_layout = er == vk::Result::eSuccess ? VkDescriptorSetLayout(empty) : VK_NULL_HANDLE;
            ubo_align = std::max<u64>(
                inst->GetPhysicalDevice().getProperties().limits.minUniformBufferOffsetAlignment, 16);
            dynamic_vertex_input = inst->IsVertexInputDynamicState();
            std::lock_guard lk(mutex);
            drawing = empty_set_layout && Gpu::Init(*inst);
            const char* sh = std::getenv("BB_VK_SHOW");
            show = drawing && sh && sh[0] == '1';
            std::printf("Native Vulkan: drawing %s (dynamic vertex input %s, constant alignment "
                        "%llu)%s\n",
                        drawing ? "on" : "NOT available", dynamic_vertex_input ? "yes" : "no",
                        (unsigned long long)ubo_align,
                        show ? "; BB_VK_SHOW=1: its picture is shown" : "");
        });
    }
}

void Report() {
    u64 last = 0, last_draw_ns = 0, last_clear_ns = 0, last_copy_ns = 0, last_submit_ns = 0,
        last_gpu_ns = 0, last_frames = 0, last_submits = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const u64 d = draws.load();
        if (d == last) {
            continue;
        }
        std::size_t keys = 0, live_targets = 0, made_pipelines = 0;
        Tex::Stats tx{};
        Buf::Stats bf{};
        Gpu::Stats gp{};
        std::size_t layouts = 0, displayed = 0;
        {
            std::lock_guard lk(mutex);
            keys = pipeline_keys.size();
            made_pipelines = pipelines.size();
            live_targets = targets.size();
            tx = Tex::GetStats();
            bf = Buf::GetStats();
            gp = drawing ? Gpu::GetStats() : Gpu::Stats{};
            layouts = vertex_layouts.size();
        }
        {
            std::lock_guard lk(display_mutex);
            for (const auto& [a, s] : shown) {
                displayed += s.image != VK_NULL_HANDLE;
            }
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
        std::printf("Native Vulkan: %llu textures (%.0f MiB), %llu uploaded in %llu batches, %llu "
                    "waiting; not made: %llu unsupported, %llu unreadable, %llu over budget, %llu "
                    "failed; %llu samplers. Binds: %llu textures (%llu render targets, %llu unset "
                    "slots, %llu without an image, %llu not uploaded yet, %llu unreadable T#), %llu "
                    "samplers (%llu failed)\n",
                    (unsigned long long)tx.textures, tx.bytes / 1048576.0,
                    (unsigned long long)tx.uploads_done, (unsigned long long)tx.upload_batches,
                    (unsigned long long)tx.uploads_pending, (unsigned long long)tx.unsupported,
                    (unsigned long long)tx.unreadable, (unsigned long long)tx.over_budget,
                    (unsigned long long)tx.failed, (unsigned long long)tx.samplers,
                    (unsigned long long)texture_binds.load(), (unsigned long long)target_binds.load(),
                    (unsigned long long)unset_textures.load(), (unsigned long long)texture_misses.load(),
                    (unsigned long long)textures_not_ready.load(),
                    (unsigned long long)bad_tsharps.load(), (unsigned long long)sampler_binds.load(),
                    (unsigned long long)sampler_misses.load());
        std::printf("Native Vulkan: %zu vertex layouts; vertex streams %llu (%llu read in place, "
                    "%.1f MiB to copy, %llu unusable); indexed draws %llu (%llu in place, %.1f MiB "
                    "copied), %llu fan/quad draws; attributes: %llu missing, %llu unknown format, "
                    "%llu integer. Imported guest memory: %llu windows (%.0f MiB), %llu not "
                    "importable; %llu of %llu lookups in place\n",
                    layouts, (unsigned long long)vb_binds.load(),
                    (unsigned long long)vb_in_place.load(), vb_copy_bytes.load() / 1048576.0,
                    (unsigned long long)bad_vertex_buffers.load(), (unsigned long long)ib_draws.load(),
                    (unsigned long long)ib_in_place.load(), ib_copy_bytes.load() / 1048576.0,
                    (unsigned long long)generated_index_draws.load(),
                    (unsigned long long)missing_attributes.load(),
                    (unsigned long long)unknown_vertex_formats.load(),
                    (unsigned long long)integer_attributes.load(), (unsigned long long)bf.windows,
                    bf.bytes / 1048576.0, (unsigned long long)bf.failed_windows,
                    (unsigned long long)bf.hits, (unsigned long long)bf.lookups);
        if (drawing) {
            std::printf("Native Vulkan: drawn %llu; not drawn: %llu geometry shader, %llu vertex "
                        "data, %llu pipeline, %llu ring/descriptors, %llu indices. %zu pipelines "
                        "(%llu failed), %llu integer-input variants failed. Feedback: %llu color "
                        "slots unbound, %llu depth targets dropped, %llu read-only. %llu clears, "
                        "%llu copies (%llu not done). GPU: %llu submits (%llu early), %llu waits, "
                        "%llu passes, %llu barriers, %.0f MiB constants, %llu descriptor sets in "
                        "%llu pools. Display buffers shown: %zu\n",
                        (unsigned long long)drawn.load(), (unsigned long long)skipped_gs.load(),
                        (unsigned long long)skipped_vertices.load(),
                        (unsigned long long)skipped_pipeline.load(),
                        (unsigned long long)skipped_memory.load(),
                        (unsigned long long)skipped_indices.load(), made_pipelines,
                        (unsigned long long)pipelines_failed.load(),
                        (unsigned long long)variants_failed.load(),
                        (unsigned long long)color_feedback.load(),
                        (unsigned long long)depth_dropped.load(),
                        (unsigned long long)depth_read_only.load(), (unsigned long long)clears.load(),
                        (unsigned long long)copies.load(), (unsigned long long)copies_skipped.load(),
                        (unsigned long long)gp.submits, (unsigned long long)gp.early_submits,
                        (unsigned long long)gp.waits, (unsigned long long)gp.passes,
                        (unsigned long long)gp.barriers, gp.ring_bytes / 1048576.0,
                        (unsigned long long)gp.sets, (unsigned long long)gp.pools, displayed);
            std::string reasons;
            for (int r = 0; r < SkipCount; ++r) {
                char b[64];
                std::snprintf(b, sizeof(b), "%s%llu %s", r ? ", " : "",
                              (unsigned long long)copy_skips[r].load(), CopySkipNames[r]);
                reasons += b;
            }
            // per second over the last 5 s: CPU time in the hooks, GPU time of our command buffers
            const u64 dn = draw_ns.load(), cn = clear_ns.load(), pn = copy_ns.load(),
                      sn = submit_ns.load(), fr = frames_asked.load();
            const u64 nd = d - last;
            std::printf("Native Vulkan: per second: %.1f frames shown, %.1f submits; CPU %.0f ms in "
                        "draws (%.1f us each), %.0f ms clears, %.0f ms copies, %.0f ms submits; GPU "
                        "%.0f ms. Copies: %llu depth -> color; not done: %s\n",
                        (fr - last_frames) / 5.0, (gp.submits - last_submits) / 5.0,
                        (dn - last_draw_ns) / 5e6, nd ? (dn - last_draw_ns) / 1e3 / nd : 0.0,
                        (cn - last_clear_ns) / 5e6, (pn - last_copy_ns) / 5e6,
                        (sn - last_submit_ns) / 5e6, (gp.gpu_ns - last_gpu_ns) / 5e6,
                        (unsigned long long)depth_copies.load(), reasons.c_str());
            last_draw_ns = dn;
            last_clear_ns = cn;
            last_copy_ns = pn;
            last_submit_ns = sn;
            last_gpu_ns = gp.gpu_ns;
            last_frames = fr;
            last_submits = gp.submits;
        }
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
    std::printf("Native Vulkan: on (shadPS4 still draws too); %zu translated shaders from %s\n",
                shaders.size(), shader_root.string().c_str());
    BbHook::PassThrough(image, size, BbHook::ClearFunction, BbHook::ClearPrologue,
                        reinterpret_cast<u64>(&OnClear), "Native Vulkan", "kClearRenderTarget");
    BbHook::PassThrough(image, size, BbHook::CopyFunction, BbHook::CopyPrologue,
                        reinterpret_cast<u64>(&OnCopy), "Native Vulkan", "kCopyTexture");
    std::fflush(stdout);
    enabled = true;
    std::thread(Report).detach();
    StartTextures();
}

void Attach(const Vulkan::Instance& vulkan) {
    instance.store(&vulkan);
    StartTextures();
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
    Timer timer(draw_ns);
    std::lock_guard lk(mutex);
    const u64 vsh = Q(ctx + CtxVs);
    ShaderInfo* vs = Resolve(vsh, "vertex");
    const u64 psh = Q(ctx + CtxPs);
    ShaderInfo* ps = psh ? Resolve(psh, "pixel") : nullptr;
    // an export shader feeds a geometry shader (which carries its own copy VS)
    const u64 gsh = vs && vs->kind == "es" ? Q(ctx + CtxGs) : 0;
    ShaderInfo* gs = gsh ? Resolve(gsh, "geometry") : nullptr;
    if (!vs || (psh && !ps) || (gsh && !gs)) {
        missing_shader.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Target* colors[8]{};
    bool any_color = false;
    for (int i = 0; i < 8; ++i) {
        if (const u64 h = Q(ctx + CtxColorTargets + i * 8)) {
            colors[i] = ColorTarget(h);
            any_color |= colors[i] != nullptr;
        }
    }
    const u64 dh = Q(ctx + CtxDepthTarget);
    Target* depth_target = dh ? DepthTarget(dh) : nullptr;
    if (!any_color && !depth_target) {
        no_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    resolved.fetch_add(1, std::memory_order_relaxed);
    if (gs) {
        skipped_gs.fetch_add(1, std::memory_order_relaxed); // the ES -> GS ring comes later
        return;
    }
    if (drawing) {
        Draw(ctx, dev, vsh, *vs, psh, ps, colors, depth_target, indexed, count, index_addr);
        return;
    }
    // stage 4: count only
    StageRes vres, pres;
    BindStage(true, *vs, ctx, dev, vres);
    if (ps) {
        BindStage(false, *ps, ctx, dev, pres);
    }
    VertexInput vi;
    FetchVertices(ctx, vsh, *vs, vi);
}

void OnSubmit() {
    if (!drawing) {
        return;
    }
    Timer timer(submit_ns);
    std::lock_guard lk(mutex);
    submits_seen.fetch_add(1, std::memory_order_relaxed);
    if (show) {
        // the display buffers the presenter asked for go to General layout for it
        std::lock_guard dl(display_mutex);
        for (auto& [address, s] : shown) {
            const auto it = targets.find(address);
            if (it == targets.end() || it->second.img.depth) {
                continue;
            }
            Gpu::Use(it->second.img, vk::ImageLayout::eGeneral);
            s = Shown{it->second.img.image, it->second.img.format, it->second.img.width,
                      it->second.img.height};
        }
    }
    Gpu::Submit();
}

bool Display(std::uint64_t address, std::uint32_t wanted, std::uint64_t* image,
             std::uint32_t* view_format, std::uint32_t* width, std::uint32_t* height) {
    frames_asked.fetch_add(1, std::memory_order_relaxed);
    if (!show) {
        return false;
    }
    std::lock_guard lk(display_mutex);
    const Shown& s = shown[address]; // asks OnSubmit to hand this buffer over from now on
    if (!s.image) {
        return false;
    }
    vk::Format f = s.format;
    if (f == vk::Format::eR8G8B8A8Unorm || f == vk::Format::eR8G8B8A8Srgb) {
        // our image holds the shader's logical channels; view it with the presenter's sRGB-ness
        const vk::Format w = vk::Format(wanted);
        const bool srgb = w == vk::Format::eB8G8R8A8Srgb || w == vk::Format::eR8G8B8A8Srgb ||
                          w == vk::Format::eA8B8G8R8SrgbPack32;
        f = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
    }
    *image = reinterpret_cast<std::uint64_t>(s.image);
    *view_format = u32(f);
    *width = s.width;
    *height = s.height;
    return true;
}
} // namespace BbNativeVk
