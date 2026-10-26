// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h" // first: sets up the Vulkan headers

#include <vk_mem_alloc.h>

#include "bbport_hook.h"
#include "bbport_native_vk_buffer.h"
#include "bbport_native_vk_blit.h"
#include "bbport_native_vk_geometry.h"
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
    u64 hash = 0; // FNV-1a 64 of the GCN program (the key in `shaders`)
    std::string name;
    std::string kind; // vs, ps, es (a VS feeding a GS), gs
    std::vector<std::pair<int, int>> textures; // slot, dims
    std::vector<std::pair<int, bool>> samplers; // slot, compare
    std::vector<int> cbuffers;
    std::vector<std::pair<int, int>> inputs; // semantic, size
    // the SPIR-V with its descriptors in the stage's set, and what it declares
    std::vector<u32> code;
    std::vector<Spirv::Binding> bindings;
    std::vector<u32> ubo_slots; // b# of its uniform buffers, ascending (dynamic offset order)
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

/// Buffer data of a draw: a fixed buffer (resident, imported), or bytes captured on the game's
/// thread (`data`, in the batch's arena) that go into the upload ring when the draw is recorded.
struct Ref {
    VkBuffer buffer = VK_NULL_HANDLE;
    u64 offset = 0;
    u8* data = nullptr;
    u64 size = 0, align = 16;
};

/// The vertex streams and attributes of a draw.
struct VertexInput {
    std::vector<vk::VertexInputBindingDescription2EXT> bindings;
    std::vector<vk::VertexInputAttributeDescription2EXT> attributes;
    Ref buffers[16]{}; // by stream slot
    u32 uint_mask = 0, sint_mask = 0;
    u32 vin[32]{}; // ctr_vin: per semantic, 1 = uscaled, 2 = sscaled (the shader converts)
    u64 key = 0;
    bool ok = true;
    bool copy = false; // streams that cannot be imported are copied (drawing)
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
std::atomic<bool> alone{false}; // BB_VK_ALONE=1: shadPS4 draws nothing
// Vertex streams are copied at each draw (default): the game rewrites its vertex memory before
// our recorded frame runs on the GPU, so reading in place shows later data. BB_VK_VERTEX_COPY=0
// reads in place (imported) instead.
bool force_copy = true;
bool dynamic_vertex_input = false;
u64 ubo_align = 256;
u32 max_dynamic_ubos = 8; // maxDescriptorSetUniformBuffersDynamic
fs::path shader_root;
std::atomic<const Vulkan::Instance*> instance{nullptr};

std::mutex mutex; // draws can come from more than one game thread
std::unordered_map<u64, ShaderInfo> shaders;          // FNV-1a 64 of the GCN program
std::unordered_map<u64, ShaderInfo*> shader_by_code;  // program address -> entry (nullptr: unknown)
std::unordered_set<u64> pipeline_keys;
std::unordered_map<u64, VkPipeline> pipelines;        // key -> pipeline (null: failed)
std::unordered_map<u64, VkPipelineLayout> pipeline_layouts;
VkDescriptorSetLayout empty_set_layout = VK_NULL_HANDLE;
std::unordered_map<u64, std::unique_ptr<Target>> targets; // GPU base address -> image

/// Display buffers the presenter asked for (BB_VK_SHOW): address -> our image, once submitted.
struct Shown {
    VkImage image = VK_NULL_HANDLE;
    vk::Format format = vk::Format::eUndefined;
    u32 width = 0, height = 0;
};
std::mutex display_mutex;
std::unordered_map<u64, Shown> shown;

std::atomic<u64> draws{0}, resolved{0}, missing_shader{0}, no_target{0}, no_context{0};
std::atomic<u64> wrong_stage{0}; // a program bound in a stage it is not for (stale context state)
std::atomic<u64> modules_made{0}, modules_failed{0}, unknown_programs{0};
std::atomic<u64> targets_made{0}, targets_remade{0}, targets_failed{0}, unusable_targets{0};
std::atomic<u64> target_bytes{0};
std::atomic<u64> texture_binds{0}, target_binds{0}, unset_textures{0}, texture_misses{0},
    bad_tsharps{0}, sampler_binds{0}, sampler_misses{0}, textures_not_ready{0};
std::atomic<u64> vb_binds{0}, vb_in_place{0}, vb_resident{0}, vb_copy_bytes{0}, ib_draws{0},
    ib_in_place{0}, ib_resident{0},
    ib_copy_bytes{0}, generated_index_draws{0}, missing_attributes{0}, unknown_vertex_formats{0},
    integer_attributes{0}, bad_vertex_buffers{0};
std::atomic<u64> drawn{0}, skipped_gs{0}, skipped_vertices{0}, skipped_pipeline{0},
    skipped_memory{0}, skipped_indices{0}, color_feedback{0}, depth_dropped{0}, depth_read_only{0},
    pipelines_failed{0}, variants_failed{0}, clears{0}, copies{0}, copies_skipped{0},
    submits_seen{0}, depth_copies{0}, draw_copies{0};
// copies not done, by reason
enum CopySkip { SkipMip, SkipSource, SkipSame, SkipDest, SkipFormat, SkipCount };
std::atomic<u64> copy_skips[SkipCount]{};
constexpr const char* CopySkipNames[SkipCount] = {"mip level", "source not ours", "same image",
                                                  "unknown destination", "format size"};
// CPU time in the hooks (ns), frames the presenter asked for
std::atomic<u64> draw_ns{0}, clear_ns{0}, copy_ns{0}, submit_ns{0}, frames_asked{0};
std::atomic<u64> copies_replaced{0}; // engine copies not run (BB_VK_ALONE=1)

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
/// What the open command buffer has bound (Gpu::StateEpoch): a draw sets only what changed.
struct Bound {
    u64 epoch = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vk::Viewport viewport{};
    vk::Rect2D scissor{};
    float bias[2]{};
    u32 stencil_ref = ~0u;
    u64 vertex_key = 0;
    VkBuffer vb[16]{};
    u64 vb_offset[16]{};
    VkBuffer ib = VK_NULL_HANDLE;
    u64 ib_offset = 0;
    vk::IndexType ib_type{};
    /// Forgets everything when the command buffer or its state changed under us.
    void Sync() {
        if (epoch != Gpu::StateEpoch()) {
            *this = Bound{};
            epoch = Gpu::StateEpoch();
        }
    }
};
Bound bound;
std::atomic<u64> state_skips{0}; // dynamic state and bindings left as they were

/// CPU time of a draw by section: each call adds the time since the previous one to section i.
// game's thread: resolve .. constants+indices; recorder: ring .. record
enum DrawSection { SecResolve, SecTextures, SecVertices, SecPipeline, SecConstants, SecRing,
                   SecPass, SecSets, SecRecord, SecCount };
constexpr const char* DrawSectionNames[SecCount] = {
    "resolve", "textures", "vertices", "pipeline", "constants+indices", "| ring copies", "pass",
    "sets", "record"};
std::atomic<u64> section_ns[SecCount]{};
struct Lap {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    void Start() {
        t = std::chrono::steady_clock::now();
    }
    void operator()(DrawSection i) {
        const auto now = std::chrono::steady_clock::now();
        section_ns[i].fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count()),
                                std::memory_order_relaxed);
        t = now;
    }
};
thread_local Lap lap;
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
            cur->hash = key;
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
        return vk::DescriptorType::eUniformBufferDynamic; // offsets given when bound (StageSet)
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
        if (b.kind == Spirv::Kind::UniformBuffer) {
            for (u32 i = 0; i < b.count; ++i) {
                s.ubo_slots.push_back(b.binding + i);
            }
        }
        lb.push_back({.binding = b.binding,
                      .descriptorType = DescriptorType(b.kind),
                      .descriptorCount = b.count,
                      .stageFlags = StageBit(s)});
    }
    std::sort(s.ubo_slots.begin(), s.ubo_slots.end());
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

// ------------------------------------------------------------------ the recording thread
// The game's thread only captures its draws, clears, copies and submits (engine state, and the
// guest data that can change after the call: constants, vertices, indices) into batches; the
// recorder thread records them in order: ring uploads, layouts, passes, descriptor sets,
// commands, submits. Gpu is only used there (and before it starts). BB_VK_RECORD_THREAD=0 records
// on the game's thread instead, as before.

/// Bump memory for the bytes a batch captures; its pointers stay valid until the batch is reset.
struct Arena {
    static constexpr std::size_t ChunkSize = 1 << 20;
    std::vector<std::pair<std::unique_ptr<u8[]>, std::size_t>> chunks; // memory, size
    std::size_t chunk = 0, used = 0;
    u8* Take(std::size_t n) {
        n = (n + 15) & ~std::size_t(15);
        while (chunk < chunks.size() && used + n > chunks[chunk].second) {
            ++chunk;
            used = 0;
        }
        if (chunk == chunks.size()) {
            const std::size_t size = std::max(n, ChunkSize);
            chunks.emplace_back(std::unique_ptr<u8[]>(new u8[size]), size);
            used = 0;
        }
        u8* p = chunks[chunk].first.get() + used;
        used += n;
        return p;
    }
    void Reset() {
        // large one-off chunks (a resident buffer's first upload) are not kept
        std::erase_if(chunks, [](const auto& c) { return c.second > ChunkSize; });
        if (chunks.size() > 4) {
            chunks.resize(4);
        }
        chunk = used = 0;
    }
};

/// A draw as captured: everything its recording needs, by value.
struct DrawPacket {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* ps = nullptr;
    const ShaderInfo* gs = nullptr;
    StageRes res[3]; // vs, ps, gs
    Ref cb[3][14];
    Gpu::Pass pass;
    const Target* depth_ro = nullptr;
    vk::Viewport viewport{};
    vk::Rect2D scissor{};
    float bias_units = 0.0f, bias_slope = 0.0f;
    u32 stencil_ref = 0;
    u64 vertex_key = 0;
    u32 binding_count = 0, attribute_count = 0;
    vk::VertexInputBindingDescription2EXT bindings[17];
    vk::VertexInputAttributeDescription2EXT attributes[32];
    Ref vb[16];
    bool indexed = false;
    Ref ib;
    vk::IndexType ib_type = vk::IndexType::eUint32;
    u32 count = 0;
};

struct ClearPacket {
    Target* target = nullptr;
    bool color = false, depth = false, stencil = false;
    float c[4]{};
    float z = 0.0f;
    u32 s = 0;
};

/// A texture copy between our images, in logical texels counted from the top.
struct CopyPacket {
    Target* src = nullptr;
    Target* dst = nullptr;
    bool from_depth = false, same_format = false;
    u32 sx = 0, dx = 0, w = 0, h = 0, src_bytes = 0;
    s32 s_top = 0, d_top = 0;
};

struct GeoUploadPacket {
    Ref data;
    VkBuffer dst = VK_NULL_HANDLE;
    u64 dst_offset = 0;
};

struct GeoFreePacket {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
};

/// A submission: the display buffers the presenter asked for go to General layout first.
struct SubmitPacket {
    std::vector<std::pair<u64, Target*>> shown;
};

enum class Op : u8 { Draw, Clear, Copy, GeoUpload, GeoFree, Retire, Submit };

struct Batch {
    std::vector<std::pair<Op, u32>> ops; // in order; the index into the op's own list
    std::vector<DrawPacket> draws;
    std::vector<ClearPacket> clears;
    std::vector<CopyPacket> copies;
    std::vector<GeoUploadPacket> geo_uploads;
    std::vector<GeoFreePacket> geo_frees;
    std::vector<std::unique_ptr<Target>> retired;
    std::vector<SubmitPacket> submits;
    Arena arena;
    void Clear() {
        ops.clear();
        draws.clear();
        clears.clear();
        copies.clear();
        geo_uploads.clear();
        geo_frees.clear();
        retired.clear();
        submits.clear();
        arena.Reset();
    }
};

constexpr std::size_t BatchCount = 16; // queued at most: the game waits beyond (~1000 draws)
constexpr std::size_t BatchOps = 64;   // operations a batch takes before it is handed over
std::array<Batch, BatchCount> batch_pool;
Batch* open_batch = nullptr; // the game's side, under `mutex`
bool record_thread = true;   // BB_VK_RECORD_THREAD=0: recorded on the game's thread
std::mutex queue_mutex;
std::condition_variable queue_cv, free_cv;
std::deque<Batch*> queued_batches, free_batches;
std::mutex gpu_mutex; // held while recording (Gpu is not thread safe; Report reads its times)
std::atomic<u64> submits_queued{0}, submits_recorded{0}, batch_waits{0};
std::mutex recorded_mutex;
std::condition_variable recorded_cv;

void Flush();
void StartRecorder();
u64 GeoSerial();
void GeoUpload(const void* src, u64 n, VkBuffer dst, u64 dst_offset);
void GeoFree(VkBuffer buffer, VmaAllocation allocation);

/// Captured bytes for the ring.
Ref Stage(u64 size, u64 align) {
    Ref r;
    r.data = open_batch->arena.Take(size);
    r.size = size;
    r.align = align;
    return r;
}

template <class T>
T& Push(Op op, std::vector<T>& list) {
    open_batch->ops.push_back({op, u32(list.size())});
    return list.emplace_back();
}

/// After each whole operation (never between a draw's captured bytes and the draw).
void EndOp() {
    if (!record_thread || open_batch->ops.size() >= BatchOps) {
        Flush();
    }
}

std::atomic<u64> set_generation{0}; // bumped when a view a cached set may hold is destroyed

void DestroyTarget(const Vulkan::Instance& inst, Target& t) {
    const vk::Device dev = inst.GetDevice();
    const VmaAllocator vma = inst.GetAllocator();
    const VkImage image = t.img.image;
    const VmaAllocation allocation = t.allocation;
    set_generation.fetch_add(1, std::memory_order_release); // cached sets may hold its views
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
    std::unique_ptr<Target>& slot = targets[base];
    if (slot && slot->img.width == w && slot->img.height == h && slot->img.format == format) {
        return slot.get();
    }
    if (slot) {
        targets_remade.fetch_add(1, std::memory_order_relaxed);
        if (drawing) {
            // work queued before still draws to the old image: it goes after that work
            Push(Op::Retire, open_batch->retired) = std::move(slot);
        } else {
            DestroyTarget(*inst, *slot);
        }
    }
    slot = std::make_unique<Target>();
    Target& t = *slot;
    using U = vk::ImageUsageFlagBits;
    // only RGBA8 targets are viewed in another format (the presenter's sRGB-ness of the display
    // buffer); naming both formats keeps them compressible (AMD drops DCC on an image whose views
    // may take any format), and every other target is made immutable
    const bool rgba8 = format == vk::Format::eR8G8B8A8Unorm || format == vk::Format::eR8G8B8A8Srgb;
    static constexpr std::array<vk::Format, 2> rgba8_views{vk::Format::eR8G8B8A8Unorm,
                                                          vk::Format::eR8G8B8A8Srgb};
    const vk::ImageFormatListCreateInfo view_formats{.viewFormatCount = u32(rgba8_views.size()),
                                                     .pViewFormats = rgba8_views.data()};
    const vk::ImageCreateInfo ici{
        .pNext = rgba8 ? &view_formats : nullptr,
        .flags = rgba8 ? vk::ImageCreateFlags{vk::ImageCreateFlagBits::eMutableFormat} : vk::ImageCreateFlags{},
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
            out.target = it->second.get();
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
/// `vertex_end`: one past the highest vertex the draw reads (made only when a stream is copied).
void FetchVertices(u64 ctx, u64 vsh, const ShaderInfo& vs, VertexInput& vi,
                   const std::function<u32()>& vertex_end) {
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
        } else if (const Geo::Binding g = vi.copy ? Geo::Use(base, size) : Geo::Binding{};
                   g.buffer) {
            vb_resident.fetch_add(1, std::memory_order_relaxed);
            vi.buffers[slot] = Ref{.buffer = g.buffer, .offset = g.offset + first};
        } else if (const Buf::Slice s = force_copy ? Buf::Slice{} : Buf::Import(base, size);
                   s.buffer) {
            vb_in_place.fetch_add(1, std::memory_order_relaxed);
            vi.buffers[slot] = Ref{.buffer = s.buffer, .offset = s.offset + first};
        } else if (!vi.copy) {
            vi.ok = false; // stage 4 counting only
        } else {
            // the part the draw reads, through the upload ring (only memory that is readable)
            const u64 bytes = std::min<u64>(size, u64(first) + u64(vertex_end()) * stride);
            const Ref span = bytes > first ? Stage(bytes - first, 16) : Ref{};
            if (!span.data || !Buf::Readable(base + first, bytes - first)) {
                bad_vertex_buffers.fetch_add(1, std::memory_order_relaxed);
                vi.ok = false;
                continue;
            }
            std::memcpy(span.data, reinterpret_cast<const void*>(base + first), bytes - first);
            vb_copy_bytes.fetch_add(bytes - first, std::memory_order_relaxed);
            vi.buffers[slot] = span;
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
    ShaderInfo* gs = nullptr; // behind an export shader (vs then is the ES)
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
        Mix(k, gs);
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

/// Sets: VS (or ES) 0, PS 1 (empty without one), GS 2 when there is one.
VkPipelineLayout PipelineLayout(const ShaderInfo& vs, const ShaderInfo* ps,
                                const ShaderInfo* gs = nullptr) {
    const VkDescriptorSetLayout sets[3] = {vs.set_layout, ps ? ps->set_layout : empty_set_layout,
                                           gs ? gs->set_layout : VK_NULL_HANDLE};
    u64 key = 0xcbf29ce484222325ull;
    Mix(key, sets);
    if (const auto it = pipeline_layouts.find(key); it != pipeline_layouts.end()) {
        return it->second;
    }
    const std::size_t dynamic = vs.ubo_slots.size() + (ps ? ps->ubo_slots.size() : 0) +
                                (gs ? gs->ubo_slots.size() : 0);
    if (dynamic > max_dynamic_ubos) {
        std::printf("Native Vulkan: %s: %zu uniform buffers, over the device's %u\n", vs.name.c_str(),
                    dynamic, max_dynamic_ubos);
        return pipeline_layouts[key] = VK_NULL_HANDLE;
    }
    const vk::DescriptorSetLayout vk_sets[3] = {sets[0], sets[1], sets[2]};
    auto [r, layout] = instance.load()->GetDevice().createPipelineLayout(
        {.setLayoutCount = gs ? 3u : 2u, .pSetLayouts = vk_sets});
    return pipeline_layouts[key] = r == vk::Result::eSuccess ? VkPipelineLayout(layout) : VK_NULL_HANDLE;
}

/// A graphics pipeline for the engine state of a draw (the D3D11 port's GetBlend, GetDepthState
/// and GetRaster, in Vulkan terms). Viewport, scissor, depth bias values, stencil reference and,
/// where supported, the vertex layout are dynamic.
VkPipeline MakePipeline(const PipelineState& s, VkPipelineLayout layout) {
    const Vulkan::Instance* inst = instance.load();
    std::vector<vk::PipelineShaderStageCreateInfo> stages{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = s.vs_module, .pName = "main"}};
    if (s.gs) {
        stages.push_back(
            {.stage = vk::ShaderStageFlagBits::eGeometry, .module = s.gs->module, .pName = "main"});
    }
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
            std::printf("Native Vulkan: pipeline %s%s%s + %s not created (%s)\n",
                        s.vs->name.c_str(), s.gs ? " + " : "", s.gs ? s.gs->name.c_str() : "",
                        s.ps ? s.ps->name.c_str() : "-", vk::to_string(r).c_str());
        }
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// ------------------------------------------------------------------ pipeline compiles
// Pipelines compile on their own threads (a compile takes ~100 ms on the AMD Windows driver,
// whose VkPipelineCache stays empty); a draw whose pipeline is still compiling is left out.
// Every pipeline's description goes to pipelines.bin, and the next run compiles all of them
// while the game starts, so a pipeline seen once is ready before its first draw.
// BB_VK_SYNC_PIPELINES=1 compiles at the draw instead (nothing left out).

/// What a pipeline is made from, by value (one record of pipelines.bin).
struct PipelineDesc {
    u64 vs = 0, ps = 0, gs = 0; // program hashes; 0: none
    u64 vertex_key = 0;
    u32 uint_mask = 0, sint_mask = 0;
    vk::Format colors[8]{};
    u32 color_count = 0;
    vk::Format depth = vk::Format::eUndefined;
    u32 stencil = 0, depth_read_only = 0, bias = 0;
    u32 blend = 0, mask = 0, dctl = 0, refmask = 0, sops = 0, setup = 0;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    u32 binding_count = 0, attribute_count = 0;
    u32 bindings[16][4]{};   // binding, stride, input rate, divisor
    u32 attributes[32][4]{}; // location, binding, format, offset
};
static_assert(std::is_trivially_copyable_v<PipelineDesc>);
constexpr u32 PipelineFileMagic = 0x44504242; // "BBPD"

struct CompileJob {
    u64 key = 0;
    PipelineState st;
    VertexInput vi; // st.vertices
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

bool async_pipelines = true;
std::mutex compile_mutex;
std::condition_variable compile_cv;
std::deque<std::unique_ptr<CompileJob>> compile_queue;
std::vector<std::pair<u64, VkPipeline>> compiled; // done, not yet in `pipelines`
std::unordered_set<u64> compiling;                // queued or compiling (under `mutex`)
std::vector<PipelineDesc> known;                  // every description this run (under `mutex`)
std::unordered_set<u64> known_keys;
bool known_dirty = false;
std::atomic<u64> compiles_done{0}, compile_ns{0}, preloaded{0}, skipped_compiling{0};

void CompileThread() {
    for (;;) {
        std::unique_ptr<CompileJob> job;
        {
            std::unique_lock lk(compile_mutex);
            compile_cv.wait(lk, [] { return !compile_queue.empty(); });
            job = std::move(compile_queue.front());
            compile_queue.pop_front();
        }
        const auto t0 = std::chrono::steady_clock::now();
        const VkPipeline pipeline = MakePipeline(job->st, job->layout);
        compile_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count()),
                             std::memory_order_relaxed);
        compiles_done.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lk(compile_mutex);
        compiled.emplace_back(job->key, pipeline);
    }
}

/// Starts the compile threads: a few, leaving cores to the game.
void StartCompileThreads() {
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
    for (unsigned i = 0; i < n; ++i) {
        std::thread(CompileThread).detach();
    }
    std::printf("Native Vulkan: %u pipeline compile threads\n", n);
}

/// Moves finished compiles into `pipelines` (holding `mutex`).
void TakeCompiled() {
    std::lock_guard lk(compile_mutex);
    for (const auto& [key, pipeline] : compiled) {
        pipelines[key] = pipeline;
        compiling.erase(key);
    }
    compiled.clear();
}

/// Queues `st` for a compile thread (holding `mutex`).
void Enqueue(u64 key, const PipelineState& st, VkPipelineLayout layout) {
    auto job = std::make_unique<CompileJob>();
    job->key = key;
    job->st = st;
    job->vi.bindings = st.vertices->bindings;
    job->vi.attributes = st.vertices->attributes;
    job->vi.key = st.vertices->key;
    job->st.vertices = &job->vi;
    job->layout = layout;
    compiling.insert(key);
    {
        std::lock_guard lk(compile_mutex);
        compile_queue.push_back(std::move(job));
    }
    compile_cv.notify_one();
}

/// Moves a queued compile a draw is waiting for to the front, once (holding `mutex`).
void Hurry(u64 key) {
    static std::unordered_set<u64> hurried;
    if (!hurried.insert(key).second) {
        return;
    }
    std::lock_guard lk(compile_mutex);
    const auto it = std::find_if(compile_queue.begin(), compile_queue.end(),
                                 [key](const auto& job) { return job->key == key; });
    if (it != compile_queue.end() && it != compile_queue.begin()) {
        auto job = std::move(*it);
        compile_queue.erase(it);
        compile_queue.push_front(std::move(job));
    }
}

/// Records the description of a pipeline seen for the first time (holding `mutex`).
void Remember(u64 key, const PipelineState& st, const VertexInput& vi) {
    if (!known_keys.insert(key).second || vi.bindings.size() > 16 || vi.attributes.size() > 32) {
        return;
    }
    PipelineDesc d{};
    d.vs = st.vs->hash;
    d.ps = st.ps ? st.ps->hash : 0;
    d.gs = st.gs ? st.gs->hash : 0;
    d.vertex_key = vi.key;
    d.uint_mask = vi.uint_mask;
    d.sint_mask = vi.sint_mask;
    std::copy(std::begin(st.colors), std::end(st.colors), d.colors);
    d.color_count = st.color_count;
    d.depth = st.depth;
    d.stencil = st.stencil;
    d.depth_read_only = st.depth_read_only;
    d.bias = st.bias;
    d.blend = st.blend;
    d.mask = st.mask;
    d.dctl = st.dctl;
    d.refmask = st.refmask;
    d.sops = st.sops;
    d.setup = st.setup;
    d.topology = st.topology;
    d.binding_count = u32(vi.bindings.size());
    for (u32 i = 0; i < d.binding_count; ++i) {
        const auto& b = vi.bindings[i];
        d.bindings[i][0] = b.binding;
        d.bindings[i][1] = b.stride;
        d.bindings[i][2] = u32(b.inputRate);
        d.bindings[i][3] = b.divisor;
    }
    d.attribute_count = u32(vi.attributes.size());
    for (u32 i = 0; i < d.attribute_count; ++i) {
        const auto& a = vi.attributes[i];
        d.attributes[i][0] = a.location;
        d.attributes[i][1] = a.binding;
        d.attributes[i][2] = u32(a.format);
        d.attributes[i][3] = a.offset;
    }
    known.push_back(d);
    known_dirty = true;
}

fs::path PipelineFile() {
    return shader_root / "pipelines.bin";
}

/// Writes every description to pipelines.bin (holding `mutex`).
void SaveKnown() {
    if (!known_dirty || shader_root.empty()) {
        return;
    }
    known_dirty = false;
    const fs::path tmp = PipelineFile().string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const u32 head[2] = {PipelineFileMagic, u32(sizeof(PipelineDesc))};
        out.write(reinterpret_cast<const char*>(head), sizeof(head));
        out.write(reinterpret_cast<const char*>(known.data()),
                  std::streamsize(known.size() * sizeof(PipelineDesc)));
        if (!out) {
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, PipelineFile(), ec);
}

/// Queues every pipeline of pipelines.bin (holding `mutex`, at start).
void PreloadKnown() {
    std::ifstream in(PipelineFile(), std::ios::binary);
    u32 head[2]{};
    if (!in.read(reinterpret_cast<char*>(head), sizeof(head)) || head[0] != PipelineFileMagic ||
        head[1] != sizeof(PipelineDesc)) {
        return; // none yet, or an older layout: made again from the draws
    }
    PipelineDesc d{};
    u64 missing = 0;
    while (in.read(reinterpret_cast<char*>(&d), sizeof(d))) {
        const auto vs_it = shaders.find(d.vs);
        const auto ps_it = d.ps ? shaders.find(d.ps) : shaders.end();
        const auto gs_it = d.gs ? shaders.find(d.gs) : shaders.end();
        if (vs_it == shaders.end() || (d.ps && ps_it == shaders.end()) ||
            (d.gs && gs_it == shaders.end()) || d.binding_count > 16 || d.attribute_count > 32) {
            ++missing;
            continue;
        }
        ShaderInfo& vs = vs_it->second;
        ShaderInfo* ps = d.ps ? &ps_it->second : nullptr;
        ShaderInfo* gs = d.gs ? &gs_it->second : nullptr;
        if ((vs.kind != "vs" && vs.kind != "es") || (ps && ps->kind != "ps") || (gs && gs->kind != "gs")) {
            ++missing; // a program in the wrong stage (recorded before draws were checked)
            continue;
        }
        LoadModule(vs);
        if (ps) {
            LoadModule(*ps);
        }
        if (gs) {
            LoadModule(*gs);
        }
        if (!vs.module || !vs.set_layout || (ps && (!ps->module || !ps->set_layout)) ||
            (gs && (!gs->module || !gs->set_layout))) {
            ++missing;
            continue;
        }
        VertexInput vi;
        vi.key = d.vertex_key;
        vi.uint_mask = d.uint_mask;
        vi.sint_mask = d.sint_mask;
        for (u32 i = 0; i < d.binding_count; ++i) {
            vi.bindings.push_back({.binding = d.bindings[i][0],
                                   .stride = d.bindings[i][1],
                                   .inputRate = vk::VertexInputRate(d.bindings[i][2]),
                                   .divisor = d.bindings[i][3]});
        }
        for (u32 i = 0; i < d.attribute_count; ++i) {
            vi.attributes.push_back({.location = d.attributes[i][0],
                                     .binding = d.attributes[i][1],
                                     .format = vk::Format(d.attributes[i][2]),
                                     .offset = d.attributes[i][3]});
        }
        PipelineState st;
        st.vs = &vs;
        st.vs_module = Variant(vs, d.uint_mask, d.sint_mask);
        st.ps = ps;
        st.gs = gs;
        std::copy(std::begin(d.colors), std::end(d.colors), st.colors);
        st.color_count = d.color_count;
        st.depth = d.depth;
        st.stencil = d.stencil;
        st.depth_read_only = d.depth_read_only;
        st.bias = d.bias;
        st.blend = d.blend;
        st.mask = d.mask;
        st.dctl = d.dctl;
        st.refmask = d.refmask;
        st.sops = d.sops;
        st.setup = d.setup;
        st.topology = d.topology;
        st.vertices = &vi;
        const VkPipelineLayout layout = st.vs_module ? PipelineLayout(vs, ps, gs) : VK_NULL_HANDLE;
        if (!layout) {
            ++missing;
            continue;
        }
        const u64 key = st.Key();
        if (!known_keys.insert(key).second) {
            continue;
        }
        known.push_back(d);
        Enqueue(key, st, layout);
        preloaded.fetch_add(1, std::memory_order_relaxed);
    }
    std::printf("Native Vulkan: %llu pipelines from %s compiling (%llu no longer made)\n",
                (unsigned long long)preloaded.load(), PipelineFile().string().c_str(),
                (unsigned long long)missing);
}

// ------------------------------------------------------------------ constants
/// Copies a stage's constant buffers (the shader header's vectors) into the upload ring; the
/// spans go to `cbs` by slot. False: the ring could not take them.
bool StageConstants(u64 shader, Ref cbs[14]) {
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
        const Ref span = Stage((size + 15) & ~15u, ubo_align);
        std::memcpy(span.data, reinterpret_cast<const void*>(data), size);
        cbs[slot] = span;
    }
    return true;
}

bool InfoConstants(const StageRes& res, const u32* vin, Ref* span) {
    *span = Stage(sizeof(InfoCB), ubo_align);
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

// ------------------------------------------------------------------ descriptor sets
// A stage's set holds its textures and samplers, and its constant buffers as dynamic uniform
// buffers over the upload ring (each draw gives its offsets when it binds the set), so a set
// depends only on what the stage samples: sets are made once and kept across command buffers.
// Destroying a target view drops them all, their pools going once the GPU is done with them.
// All of it on the recording side (under gpu_mutex).

struct CachedSet {
    std::vector<u64> key;
    VkDescriptorSet set = VK_NULL_HANDLE;
};
std::unordered_map<u64, CachedSet> set_cache;
std::vector<VkDescriptorPool> set_pools; // set_cache's; the last one takes new sets
u64 set_cache_generation = 0;           // set_generation when set_cache was last valid
std::atomic<u64> sets_made{0}, sets_reused{0}, sets_dropped{0};
constexpr std::size_t MaxCachedSets = 16384;
constexpr u32 SetsPerCachePool = 1024;

/// Forgets every cached set; their pools go once the GPU has finished what uses them.
void DropSets() {
    sets_dropped.fetch_add(set_cache.size(), std::memory_order_relaxed);
    set_cache.clear();
    if (!set_pools.empty()) {
        const vk::Device dev = instance.load()->GetDevice();
        Gpu::Defer([dev, pools = std::move(set_pools)] {
            for (const VkDescriptorPool pool : pools) {
                dev.destroyDescriptorPool(pool);
            }
        });
        set_pools.clear();
    }
}

/// A set of `layout` from the cache's pools (a new pool when the last one is full).
VkDescriptorSet AllocateKept(VkDescriptorSetLayout layout) {
    const vk::Device dev = instance.load()->GetDevice();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!set_pools.empty()) {
            const vk::DescriptorSetLayout l{layout};
            vk::DescriptorSet set;
            const vk::DescriptorSetAllocateInfo ai{.descriptorPool = set_pools.back(),
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &l};
            if (dev.allocateDescriptorSets(&ai, &set) == vk::Result::eSuccess) {
                return set;
            }
        }
        static constexpr std::array<vk::DescriptorPoolSize, 7> sizes{{
            {vk::DescriptorType::eUniformBufferDynamic, SetsPerCachePool * 4},
            {vk::DescriptorType::eSampledImage, SetsPerCachePool * 16},
            {vk::DescriptorType::eSampler, SetsPerCachePool * 16},
            {vk::DescriptorType::eStorageBuffer, 256},
            {vk::DescriptorType::eStorageImage, 256},
            {vk::DescriptorType::eUniformTexelBuffer, 256},
            {vk::DescriptorType::eStorageTexelBuffer, 256},
        }};
        auto [r, pool] = dev.createDescriptorPool(
            {.maxSets = SetsPerCachePool, .poolSizeCount = u32(sizes.size()), .pPoolSizes = sizes.data()});
        if (r != vk::Result::eSuccess) {
            return VK_NULL_HANDLE;
        }
        set_pools.push_back(pool);
    }
    return VK_NULL_HANDLE;
}

/// A stage's descriptor set for what it samples (bindings b# 0.., t# 64.., s# 128..): the
/// cached one, else written now and kept.
vk::DescriptorSet StageSet(const ShaderInfo& s, const StageRes& res, const Target* depth_ro) {
    if (const u64 g = set_generation.load(std::memory_order_acquire); g != set_cache_generation) {
        DropSets();
        set_cache_generation = g;
    }
    static const u32 default_ss[4] = {0, 0xfff000, 0x5000000, 0};
    // the image and sampler descriptors, in binding order: with the layout, the cache key
    static std::vector<vk::DescriptorImageInfo> images;
    static std::vector<u64> key;
    images.clear();
    key.clear();
    key.push_back(u64(s.set_layout));
    for (const Spirv::Binding& b : s.bindings) {
        switch (b.kind) {
        case Spirv::Kind::SampledImage:
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
                key.push_back(u64(VkImageView(ii.imageView)));
                key.push_back(u64(ii.imageLayout));
            }
            break;
        case Spirv::Kind::Sampler:
            for (u32 i = 0; i < b.count; ++i) {
                const u32 slot = b.binding + i - 128;
                VkSampler smp = slot < 16 ? res.samplers[slot] : VK_NULL_HANDLE;
                if (!smp) {
                    smp = Tex::Sampler(default_ss, false); // samplers cannot be null
                }
                images.push_back({.sampler = smp});
                key.push_back(u64(smp));
            }
            break;
        case Spirv::Kind::StorageImage:
            for (u32 i = 0; i < b.count; ++i) {
                images.push_back({.imageLayout = vk::ImageLayout::eGeneral});
            }
            break;
        default:
            break;
        }
    }
    u64 h = 0xcbf29ce484222325ull;
    for (const u64 w : key) {
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    const auto it = set_cache.find(h);
    if (it != set_cache.end() && it->second.key == key) {
        sets_reused.fetch_add(1, std::memory_order_relaxed);
        return it->second.set;
    }
    // kept unless another key has this hash (that set is written for this command buffer only)
    const bool keep = it == set_cache.end();
    if (keep && set_cache.size() >= MaxCachedSets) {
        DropSets();
    }
    const vk::DescriptorSet set = keep ? vk::DescriptorSet{AllocateKept(s.set_layout)}
                                       : Gpu::AllocateSet(s.set_layout);
    if (!set) {
        return set;
    }
    static std::vector<vk::DescriptorBufferInfo> buffers;
    static std::vector<vk::WriteDescriptorSet> writes;
    buffers.clear();
    writes.clear();
    for (const Spirv::Binding& b : s.bindings) {
        if (b.kind == Spirv::Kind::UniformBuffer || b.kind == Spirv::Kind::StorageBuffer) {
            for (u32 i = 0; i < b.count; ++i) {
                // uniform buffers: the ring, moved by the draw's offset; storage: none bound
                buffers.push_back(b.kind == Spirv::Kind::UniformBuffer
                                      ? vk::DescriptorBufferInfo{Gpu::UniformBuffer(), 0, Gpu::UniformRange()}
                                      : vk::DescriptorBufferInfo{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE});
            }
        }
    }
    std::size_t next_image = 0, next_buffer = 0;
    for (const Spirv::Binding& b : s.bindings) {
        vk::WriteDescriptorSet w{.dstSet = set,
                                 .dstBinding = b.binding,
                                 .descriptorCount = b.count,
                                 .descriptorType = DescriptorType(b.kind)};
        switch (b.kind) {
        case Spirv::Kind::UniformBuffer:
        case Spirv::Kind::StorageBuffer:
            w.pBufferInfo = &buffers[next_buffer];
            next_buffer += b.count;
            break;
        case Spirv::Kind::SampledImage:
        case Spirv::Kind::Sampler:
        case Spirv::Kind::StorageImage:
            w.pImageInfo = &images[next_image];
            next_image += b.count;
            break;
        default: // texel buffers: none bound
            continue;
        }
        writes.push_back(w);
    }
    instance.load()->GetDevice().updateDescriptorSets(writes, {});
    sets_made.fetch_add(1, std::memory_order_relaxed);
    if (keep) {
        set_cache.emplace(h, CachedSet{key, set});
    }
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

/// Captures a draw (the game's thread): its state, constants and data, into the open batch.
void Draw(u64 ctx, u64 dev, u64 vsh, ShaderInfo& vs, u64 psh, ShaderInfo* ps, u64 gsh,
          ShaderInfo* gs, Target* colors[8], Target* depth_target, bool indexed, u32 count,
          u64 index_addr) {
    if (gs && (!gs->module || !gs->set_layout)) {
        skipped_gs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!count || !vs.module || !vs.set_layout || (ps && (!ps->module || !ps->set_layout))) {
        skipped_pipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    lap(SecResolve);
    StageRes vres, pres;
    BindStage(true, vs, ctx, dev, vres);
    if (ps) {
        BindStage(false, *ps, ctx, dev, pres);
    }
    lap(SecTextures);
    VertexInput vi;
    vi.copy = true;
    u32 vertex_end = 0; // made once, when a stream has to be copied
    const u32 isize_ = R<u32>(ctx + CtxIndexSize) == 2 ? 2 : 4;
    // the draw's indices, copied with its vertices: the engine rewrites index memory (rings)
    // before the GPU runs the frame, so they are read now, and the copy gives the vertex range
    Ref index_copy{};
    bool index_unreadable = false;
    FetchVertices(ctx, vsh, vs, vi, [&]() -> u32 {
        if (vertex_end) {
            return vertex_end;
        }
        vertex_end = count;
        if (indexed && index_addr) {
            const u64 bytes = u64(count) * isize_;
            index_copy = Stage(bytes, 4);
            // scanned in cached memory: the ring is write-combined, slow to read back. Guest
            // memory is read directly when it is readable, else through a scratch copy
            static thread_local std::vector<std::uint8_t> scratch;
            const void* from = reinterpret_cast<const void*>(index_addr);
            if (!Buf::Readable(index_addr, bytes)) {
                scratch.resize(bytes);
                from = scratch.data();
                if (!Read(index_addr, scratch.data(), bytes)) {
                    from = nullptr;
                }
            }
            if (!from) {
                index_copy = {};
                index_unreadable = true;
                return vertex_end;
            }
            std::memcpy(index_copy.data, from, bytes);
            u32 top = 0;
            if (isize_ == 2) {
                const auto* p = static_cast<const std::uint16_t*>(from);
                for (u32 i = 0; i < count; ++i) {
                    top = p[i] != 0xffff ? std::max<u32>(top, p[i]) : top;
                }
            } else {
                const auto* p = static_cast<const u32*>(from);
                for (u32 i = 0; i < count; ++i) {
                    top = p[i] != 0xffffffffu ? std::max(top, p[i]) : top;
                }
            }
            vertex_end = top + 1;
        }
        return vertex_end;
    });
    if (index_unreadable) {
        skipped_indices.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!vi.ok) {
        skipped_vertices.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    lap(SecVertices);

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
    st.gs = gs;
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
    const VkPipelineLayout layout = PipelineLayout(vs, ps, gs);
    auto found = pipelines.find(key);
    if (found == pipelines.end() && compiling.count(key)) {
        TakeCompiled();
        found = pipelines.find(key);
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (found != pipelines.end()) {
        pipeline = found->second;
    } else if (!compiling.count(key)) {
        if (!layout) {
            pipelines[key] = VK_NULL_HANDLE;
        } else {
            Remember(key, st, vi);
            if (async_pipelines) {
                Enqueue(key, st, layout);
            } else {
                pipeline = pipelines[key] = MakePipeline(st, layout);
            }
        }
    }
    if (!pipeline && compiling.count(key)) {
        Hurry(key);
        skipped_compiling.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!pipeline) {
        skipped_pipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    lap(SecPipeline);
    DrawPacket& d = Push(Op::Draw, open_batch->draws); // value-initialized
    Ref* const vcb = d.cb[0];
    Ref* const pcb = d.cb[1];
    Ref* const gcb = d.cb[2];
    const StageRes gres{}; // the geometry shader samples nothing (as the port binds it)
    // a draw given up from here is left in the batch as nothing to record
    auto drop = [&] { d.pipeline = VK_NULL_HANDLE; };
    if (!StageConstants(vsh, vcb) || !InfoConstants(vres, vi.vin, &vcb[13]) ||
        (ps && (!StageConstants(psh, pcb) || !InfoConstants(pres, nullptr, &pcb[13]))) ||
        (gs && (!StageConstants(gsh, gcb) || !InfoConstants(gres, nullptr, &gcb[13])))) {
        skipped_memory.fetch_add(1, std::memory_order_relaxed);
        drop();
        return;
    }
    Ref index_ref{};
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
            drop();
            return;
        }
        const Ref span = Stage(gen.size() * 4, 4);
        std::memcpy(span.data, gen.data(), gen.size() * 4);
        ib_copy_bytes.fetch_add(gen.size() * 4, std::memory_order_relaxed);
        index_ref = span;
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
        const Buf::Slice s = force_copy ? Buf::Slice{}
                             : inside   ? Buf::Import(ib_base, ib_size)
                                        : Buf::Import(index_addr, bytes);
        const u64 offset = s.offset + (inside ? index_addr - ib_base : 0);
        const Geo::Binding g = inside ? Geo::Use(ib_base, ib_size) : Geo::Binding{};
        if (g.buffer && (index_addr - ib_base) % isize == 0) {
            ib_resident.fetch_add(1, std::memory_order_relaxed);
            index_ref = Ref{.buffer = g.buffer, .offset = g.offset + (index_addr - ib_base)};
        } else if (index_copy.data) {
            ib_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
            index_ref = index_copy;
        } else if (s.buffer && offset % isize == 0) {
            ib_in_place.fetch_add(1, std::memory_order_relaxed);
            index_ref = Ref{.buffer = s.buffer, .offset = offset};
        } else {
            const Ref span = Stage(bytes, 4);
            if (!Read(index_addr, span.data, bytes)) {
                skipped_indices.fetch_add(1, std::memory_order_relaxed);
                drop();
                return;
            }
            ib_copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
            index_ref = span;
        }
        index_type = isize == 2 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    }

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
    d.viewport = vk::Viewport{float(vp.x), float(top + vp.h), float(vp.w), -float(vp.h),
                              std::clamp(vp.zmin, 0.0f, 1.0f), std::clamp(vp.zmax, 0.0f, 1.0f)};
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
    d.scissor = vk::Rect2D{{x0, y0}, {u32(x1 - x0), u32(y1 - y0)}};
    // the engine's packed rasterizer state (ctx+0x190): bit 4 sign + bits 5-30 polygon offset
    // scale (1/16 units), bits 31-62 the offset (float bits)
    if (st.bias) {
        const u64 rkey = Q(ctx + CtxRasterState);
        d.bias_slope = float((rkey >> 5) & 0x3ffffff) * ((rkey & 0x10) ? -1.0f : 1.0f) / 16.0f;
        const u32 obits = u32(rkey >> 31);
        float units;
        std::memcpy(&units, &obits, 4);
        d.bias_units = float(s32(units));
    }
    const bool uses_opval = (st.sops & 0xf) == 4 || ((st.sops >> 4) & 0xf) == 4 ||
                            ((st.sops >> 8) & 0xf) == 4;
    d.stencil_ref = uses_opval ? st.refmask >> 24 : st.refmask & 0xff;
    d.vertex_key = vi.key;
    d.binding_count = u32(std::min<std::size_t>(vi.bindings.size(), 17));
    d.attribute_count = u32(std::min<std::size_t>(vi.attributes.size(), 32));
    std::copy_n(vi.bindings.begin(), d.binding_count, d.bindings);
    std::copy_n(vi.attributes.begin(), d.attribute_count, d.attributes);
    std::copy(std::begin(vi.buffers), std::end(vi.buffers), d.vb);
    d.indexed = indexed;
    d.ib = index_ref;
    d.ib_type = index_type;
    d.count = indexed ? index_count : count;
    d.vs = &vs;
    d.ps = ps;
    d.gs = gs;
    d.res[0] = vres;
    d.res[1] = pres;
    d.pass = pass;
    d.depth_ro = depth_ro;
    d.layout = layout;
    d.pipeline = pipeline;
    lap(SecConstants);
}

/// Ring bytes of a captured Ref (the recorder). False: the ring could not take them.
bool ToRing(Ref& r) {
    if (!r.data) {
        return true;
    }
    const Gpu::Span span = Gpu::Upload(r.size, r.align);
    if (!span.data) {
        return false;
    }
    std::memcpy(span.data, r.data, r.size);
    r.buffer = span.buffer;
    r.offset = span.offset;
    r.data = nullptr;
    return true;
}

/// Records a captured draw (the recorder thread).
void RecordDraw(DrawPacket& d) {
    if (!d.pipeline) {
        return; // given up while it was captured
    }
    lap.Start();
    // everything taken from the upload ring first: a full ring submits early, which ends the
    // command buffer, so nothing may be bound before
    bool ok = true;
    for (auto& stage : d.cb) {
        for (Ref& r : stage) {
            ok = ok && ToRing(r);
        }
    }
    for (Ref& r : d.vb) {
        ok = ok && ToRing(r);
    }
    if (d.indexed) {
        ok = ok && ToRing(d.ib);
    }
    if (!ok) {
        skipped_memory.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    lap(SecRing);
    // layouts of the sampled targets, then the pass
    for (const StageRes* res : {&d.res[0], &d.res[1]}) {
        for (const Slot& slot : res->slots) {
            if (slot.target && slot.target != d.depth_ro) {
                Gpu::Use(const_cast<Target*>(slot.target)->img, vk::ImageLayout::eShaderReadOnlyOptimal);
            }
        }
    }
    Gpu::Render(d.pass);
    if (!Gpu::Rendering()) {
        skipped_pipeline.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    lap(SecPass);
    const ShaderInfo* ps = d.ps;
    const ShaderInfo* gs = d.gs;
    const vk::DescriptorSet vset = StageSet(*d.vs, d.res[0], d.depth_ro);
    // set 1 is bound even without a PS when the GS's set 2 follows it (an empty set)
    const vk::DescriptorSet pset = ps   ? StageSet(*ps, d.res[1], d.depth_ro)
                                   : gs ? Gpu::AllocateSet(empty_set_layout)
                                        : vk::DescriptorSet{};
    const vk::DescriptorSet gset = gs ? StageSet(*gs, d.res[2], nullptr) : vk::DescriptorSet{};
    if (!vset || ((ps || gs) && !pset) || (gs && !gset)) {
        skipped_memory.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // the constant buffers' ring offsets, by set and then binding (PipelineLayout keeps the
    // count within the device's limit)
    u32 offsets[64];
    u32 offset_count = 0;
    const VkBuffer ubo = Gpu::UniformBuffer();
    const u32 zero = u32(Gpu::ZeroUniform());
    auto add_offsets = [&](const ShaderInfo& s, const Ref* cbs) {
        for (const u32 slot : s.ubo_slots) {
            if (offset_count < 64) {
                offsets[offset_count++] =
                    slot < 14 && cbs[slot].buffer == ubo ? u32(cbs[slot].offset) : zero;
            }
        }
    };
    add_offsets(*d.vs, d.cb[0]);
    if (ps) {
        add_offsets(*ps, d.cb[1]);
    }
    if (gs) {
        add_offsets(*gs, d.cb[2]);
    }

    lap(SecSets);
    const vk::CommandBuffer cmd = Gpu::Cmd();
    bound.Sync();
    if (bound.pipeline != d.pipeline) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, d.pipeline);
        bound.pipeline = d.pipeline;
    } else {
        state_skips.fetch_add(1, std::memory_order_relaxed);
    }
    const vk::DescriptorSet sets[3] = {vset, pset, gset};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, d.layout, 0, gs ? 3 : ps ? 2 : 1, sets,
                           offset_count, offsets);
    if (std::memcmp(&bound.viewport, &d.viewport, sizeof(d.viewport)) != 0) {
        cmd.setViewport(0, d.viewport);
        bound.viewport = d.viewport;
    }
    if (bound.scissor != d.scissor) {
        cmd.setScissor(0, d.scissor);
        bound.scissor = d.scissor;
    }
    if (bound.bias[0] != d.bias_units || bound.bias[1] != d.bias_slope) {
        cmd.setDepthBias(d.bias_units, 0.0f, d.bias_slope);
        bound.bias[0] = d.bias_units;
        bound.bias[1] = d.bias_slope;
    }
    if (bound.stencil_ref != d.stencil_ref) {
        cmd.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, d.stencil_ref);
        bound.stencil_ref = d.stencil_ref;
    }
    // the vertex layout key covers the bindings and attributes (+1: never 0, the unset value)
    if (dynamic_vertex_input && bound.vertex_key != d.vertex_key + 1) {
        cmd.setVertexInputEXT(vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT>(d.binding_count, d.bindings),
                              vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT>(d.attribute_count, d.attributes));
        bound.vertex_key = d.vertex_key + 1;
    }
    for (u32 k = 0; k < d.binding_count; ++k) {
        const u32 binding = d.bindings[k].binding;
        const VkBuffer buffer = binding == ZeroBinding || binding >= 16 ? VK_NULL_HANDLE : d.vb[binding].buffer;
        const u64 offset = binding == ZeroBinding || binding >= 16 ? 0 : d.vb[binding].offset;
        if (binding < 16 && bound.vb[binding] == buffer && bound.vb_offset[binding] == offset && buffer) {
            continue;
        }
        const vk::Buffer vb{buffer};
        const vk::DeviceSize vo = offset;
        cmd.bindVertexBuffers(binding, 1, &vb, &vo);
        if (binding < 16) {
            bound.vb[binding] = buffer;
            bound.vb_offset[binding] = offset;
        }
    }
    if (d.indexed) {
        if (bound.ib != d.ib.buffer || bound.ib_offset != d.ib.offset || bound.ib_type != d.ib_type) {
            cmd.bindIndexBuffer(d.ib.buffer, d.ib.offset, d.ib_type);
            bound.ib = d.ib.buffer;
            bound.ib_offset = d.ib.offset;
            bound.ib_type = d.ib_type;
        }
        cmd.drawIndexed(d.count, 1, 0, 0, 0);
    } else {
        cmd.draw(d.count, 1, 0, 0);
    }
    Gpu::NoteDraw();
    drawn.fetch_add(1, std::memory_order_relaxed);
    lap(SecRecord);
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
                ClearPacket& c = Push(Op::Clear, open_batch->clears);
                c.target = d;
                c.depth = flags & 2;
                c.z = depth;
                c.stencil = flags & 4;
                c.s = stencil & 0xff;
            }
        }
    }
    if ((flags & 1) && color) {
        for (int i = 0; i < 8; ++i) {
            if (const u64 h = Q(ctx + CtxColorTargets + i * 8)) {
                if (Target* t = ColorTarget(h)) {
                    ClearPacket& c = Push(Op::Clear, open_batch->clears);
                    c.target = t;
                    c.color = true;
                    std::memcpy(c.c, color, sizeof(c.c));
                }
            }
        }
    }
    EndOp();
}

void RunClear(const ClearPacket& c) {
    if (c.color) {
        Gpu::ClearColor(c.target->img, c.c);
    } else {
        Gpu::ClearDepthStencil(c.target->img, c.depth, c.z, c.stencil, c.s);
    }
}

/// kCopyTexture(ctx, const T# src, const T# dst, const s32 p[8]): p = src x, y, dst x, y, width,
/// height (-1: all), src mip, dst mip, with y counted from the bottom like the viewport. The PS4
/// runs a copy compute shader; here it is an image copy between our images, the destination
/// becoming one of them (the D3D11 port's CopyTextureHook). Mip 0 only; a depth source goes to a
/// color destination of the same texel size (depth -> R32F) through the upload ring.
/// True: the copy is done (between our images).
bool CopyTexture(u64 ctx, const u32* src, const u32* dst, const s32* p) {
    (void)ctx;
    if (!drawing || !src || !dst || !p) {
        return false;
    }
    s32 a[8];
    std::memcpy(a, p, sizeof(a));
    if (a[0] < 0 || a[1] < 0 || a[2] < 0 || a[3] < 0) {
        return false;
    }
    Timer timer(copy_ns);
    std::lock_guard lk(mutex);
    const Tex::Info s = Tex::Describe(src), d = Tex::Describe(dst);
    const auto it = targets.find(s.base);
    const bool from_depth = it != targets.end() && it->second->img.depth;
    const u32 src_bytes = it == targets.end() ? 0
                          : from_depth ? (it->second->img.format == vk::Format::eD16Unorm ? 2 : 4)
                                       : FormatBytes(it->second->img.format);
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
    auto skip = [&](int reason) -> void {
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
        return false;
    }
    Target& st = *it->second;
    Target* dt = MakeTarget(d.base, d.width, d.height, d.format, false,
                            from_depth ? 0 : st.comp_swap, "copy target");
    if (!dt || dt == &st) {
        skip(SkipDest);
        return false;
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
        return false;
    }
    const s32 s_top = s32(sh - sy - h), d_top = s32(dh - dy - h);
    CopyPacket& c = Push(Op::Copy, open_batch->copies);
    c = CopyPacket{.src = &st, .dst = dt, .from_depth = from_depth, .sx = sx, .dx = dx, .w = w,
                   .h = h, .src_bytes = src_bytes, .s_top = s_top, .d_top = d_top};
    EndOp();
    return true;
}

/// Records a captured texture copy (the recorder thread).
void RunCopy(const CopyPacket& c) {
    Target& st = *c.src;
    Target* const dt = c.dst;
    const bool from_depth = c.from_depth;
    const u32 sx = c.sx, dx = c.dx, w = c.w, h = c.h, src_bytes = c.src_bytes;
    const s32 s_top = c.s_top, d_top = c.d_top;
    // a draw sampling the source into the destination (both stay compressed), else transfers:
    // depth through the upload ring
    const bool same_format = st.img.format == dt->img.format && !IntegerFormat(dt->img.format);
    if ((from_depth || same_format) &&
        Blit::Copy(st.img, st.view, {s32(sx), s_top}, dt->img, {{s32(dx), d_top}, {w, h}})) {
        depth_copies.fetch_add(from_depth, std::memory_order_relaxed);
        draw_copies.fetch_add(1, std::memory_order_relaxed);
        copies.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Gpu::Span span{};
    if (from_depth) { // the ring first: a full ring submits early
        span = Gpu::Upload(u64(w) * h * src_bytes, 16);
        if (!span.data) {
            copies_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    Gpu::Use(st.img, vk::ImageLayout::eTransferSrcOptimal);
    Gpu::Use(dt->img, vk::ImageLayout::eTransferDstOptimal);
    Gpu::EndRendering();
    Gpu::Mark(from_depth ? Gpu::Work::DepthCopy : Gpu::Work::Copy, w, h, dt->img.format);
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

/// kCopyTexture's hook: with BB_VK_ALONE=1 a copy done here replaces the engine's (its compute
/// copy would only run in shadPS4's images, which nothing shows), else the engine's runs too.
__attribute__((sysv_abi)) bool OnCopy(u64 ctx, const u32* src, const u32* dst, const s32* p) {
    const bool done = CopyTexture(ctx, src, dst, p);
    if (done && alone.load(std::memory_order_relaxed)) {
        copies_replaced.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
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
            Blit::Init(*inst);
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
            max_dynamic_ubos =
                inst->GetPhysicalDevice().getProperties().limits.maxDescriptorSetUniformBuffersDynamic;
            dynamic_vertex_input = inst->IsVertexInputDynamicState();
            std::lock_guard lk(mutex);
            drawing = empty_set_layout &&
                      Gpu::Init(*inst, shader_root.empty()
                                           ? std::string()
                                           : (shader_root / "pipeline_cache.bin").string());
            const char* sh = std::getenv("BB_VK_SHOW");
            show = drawing && sh && sh[0] == '1';
            const char* al = std::getenv("BB_VK_ALONE");
            if (al && al[0] == '1') {
                alone.store(show, std::memory_order_release);
                std::printf("Native Vulkan: BB_VK_ALONE=1: %s\n",
                            show ? "shadPS4 draws nothing, this renderer draws the frame alone"
                                 : "ignored (needs BB_VK_SHOW=1)");
            }
            const char* fc = std::getenv("BB_VK_VERTEX_COPY");
            force_copy = !(fc && fc[0] == '0');
            const char* sp = std::getenv("BB_VK_SYNC_PIPELINES");
            async_pipelines = !(sp && sp[0] == '1');
            if (drawing) {
                StartRecorder();
                Geo::Init(*inst, Geo::Host{GeoSerial, GeoUpload, GeoFree});
                StartCompileThreads();
                PreloadKnown();
            }
            std::printf("Native Vulkan: vertex streams %s\n",
                        force_copy ? "copied at each draw (BB_VK_VERTEX_COPY=0 reads them in place)"
                                   : "read in place (BB_VK_VERTEX_COPY=0)");
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
    std::size_t saved_pipelines = 0;
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
            if (drawing && made_pipelines != saved_pipelines) {
                Gpu::SaveCache(); // new pipelines compile from the file next run
                saved_pipelines = made_pipelines;
            }
            if (drawing) {
                SaveKnown();
            }
            live_targets = targets.size();
            tx = Tex::GetStats();
            bf = Buf::GetStats();
            if (drawing) {
                std::lock_guard g(gpu_mutex);
                gp = Gpu::GetStats();
            }
            layouts = vertex_layouts.size();
        }
        {
            std::lock_guard lk(display_mutex);
            for (const auto& [a, s] : shown) {
                displayed += s.image != VK_NULL_HANDLE;
            }
        }
        std::printf("Native Vulkan: %.0f draws/s; in all %llu draws, %llu resolved, %llu with a "
                    "shader missing, %llu with a program in the wrong stage, %llu without a target, %llu without a context; %zu pipeline "
                    "states; %llu shader modules (%llu failed), %llu programs without a "
                    "translation\n",
                    (d - last) / 5.0, (unsigned long long)d, (unsigned long long)resolved.load(),
                    (unsigned long long)missing_shader.load(), (unsigned long long)wrong_stage.load(),
                    (unsigned long long)no_target.load(),
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
        std::printf("Native Vulkan: %zu vertex layouts; vertex streams %llu (%llu resident, %llu read "
                    "in place, %.1f MiB copied, %llu unusable); indexed draws %llu (%llu resident, "
                    "%llu in place, %.1f MiB "
                    "copied), %llu fan/quad draws; attributes: %llu missing, %llu unknown format, "
                    "%llu integer. Imported guest memory: %llu windows (%.0f MiB), %llu not "
                    "importable; %llu of %llu lookups in place\n",
                    layouts, (unsigned long long)vb_binds.load(),
                    (unsigned long long)vb_resident.load(), (unsigned long long)vb_in_place.load(),
                    vb_copy_bytes.load() / 1048576.0, (unsigned long long)bad_vertex_buffers.load(),
                    (unsigned long long)ib_draws.load(), (unsigned long long)ib_resident.load(),
                    (unsigned long long)ib_in_place.load(), ib_copy_bytes.load() / 1048576.0,
                    (unsigned long long)generated_index_draws.load(),
                    (unsigned long long)missing_attributes.load(),
                    (unsigned long long)unknown_vertex_formats.load(),
                    (unsigned long long)integer_attributes.load(), (unsigned long long)bf.windows,
                    bf.bytes / 1048576.0, (unsigned long long)bf.failed_windows,
                    (unsigned long long)bf.hits, (unsigned long long)bf.lookups);
        if (const Geo::Stats g = Geo::GetStats(); g.on) {
            std::printf("Native Vulkan: resident vertex/index buffers: %llu (%.0f MiB), %llu waiting "
                        "to stay unwritten, %llu rewritten too often, %llu unusable, %llu over "
                        "budget, %llu freed unused; %.1f MiB uploaded, %llu partial re-uploads, %llu "
                        "page writes heard; %llu uses, %llu copied (rewritten mid-submission)\n",
                        (unsigned long long)g.buffers, g.bytes / 1048576.0,
                        (unsigned long long)g.candidates, (unsigned long long)g.volatile_buffers,
                        (unsigned long long)g.unusable, (unsigned long long)g.over_budget,
                        (unsigned long long)g.evicted, g.uploaded_bytes / 1048576.0,
                        (unsigned long long)g.reuploads, (unsigned long long)g.writes_heard,
                        (unsigned long long)g.resident_uses, (unsigned long long)g.busy_uses);
        }
        if (drawing) {
            std::printf("Native Vulkan: drawn %llu; not drawn: %llu geometry shader, %llu vertex "
                        "data, %llu pipeline, %llu ring/descriptors, %llu indices. %zu pipelines "
                        "(%llu failed), %llu integer-input variants failed. Feedback: %llu color "
                        "slots unbound, %llu depth targets dropped, %llu read-only. %llu clears (%llu as "
                        "load ops, %llu in the open pass, %llu recorded), "
                        "%llu copies (%llu not done). GPU: %llu submits (%llu early), %llu waits, "
                        "%llu passes, %llu barriers, %.0f MiB constants, %llu descriptor sets in "
                        "%llu pools; stage sets: %llu made, %llu reused, %llu dropped. Display "
                        "buffers shown: %zu\n",
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
                        (unsigned long long)gp.load_clears, (unsigned long long)gp.pass_clears,
                        (unsigned long long)gp.recorded_clears, (unsigned long long)copies.load(), (unsigned long long)copies_skipped.load(),
                        (unsigned long long)gp.submits, (unsigned long long)gp.early_submits,
                        (unsigned long long)gp.waits, (unsigned long long)gp.passes,
                        (unsigned long long)gp.barriers, gp.ring_bytes / 1048576.0,
                        (unsigned long long)gp.sets, (unsigned long long)gp.pools,
                        (unsigned long long)sets_made.load(), (unsigned long long)sets_reused.load(),
                        (unsigned long long)sets_dropped.load(), displayed);
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
                        "%.0f ms. Copies: %llu depth -> color, %llu as draws, %llu in place of the engine's; not done: %s\n",
                        (fr - last_frames) / 5.0, (gp.submits - last_submits) / 5.0,
                        (dn - last_draw_ns) / 5e6, nd ? (dn - last_draw_ns) / 1e3 / nd : 0.0,
                        (cn - last_clear_ns) / 5e6, (pn - last_copy_ns) / 5e6,
                        (sn - last_submit_ns) / 5e6, (gp.gpu_ns - last_gpu_ns) / 5e6,
                        (unsigned long long)depth_copies.load(),
                        (unsigned long long)draw_copies.load(),
                        (unsigned long long)copies_replaced.load(), reasons.c_str());
            {
                std::size_t queued = 0;
                {
                    std::lock_guard cl(compile_mutex);
                    queued = compile_queue.size();
                }
                const u64 done = compiles_done.load();
                std::printf("Native Vulkan: pipelines: %llu compiled (%.0f ms each, %llu from the "
                            "last run), %zu waiting; %llu draws left out while compiling\n",
                            (unsigned long long)done, done ? compile_ns.load() / 1e6 / done : 0.0,
                            (unsigned long long)preloaded.load(), queued,
                            (unsigned long long)skipped_compiling.load());
            }
            std::vector<Gpu::WorkTime> work;
            {
                std::lock_guard g(gpu_mutex);
                work = Gpu::TakeWorkTimes();
            }
            std::string top;
            for (std::size_t i = 0; i < work.size() && i < 10; ++i) {
                char b[192];
                std::snprintf(b, sizeof(b), "%s%s %.1f ms (%.0f/s, %llu draws)", i ? "; " : "",
                              work[i].what.c_str(), work[i].ns / 5e6, work[i].count / 5.0,
                              (unsigned long long)work[i].draws);
                top += b;
            }
            std::printf("Native Vulkan: GPU per second, most first: %s\n", top.c_str());
            {
                static u64 last_sec[SecCount]{};
                std::string secs;
                const u64 n = d - last;
                for (int i = 0; i < SecCount; ++i) {
                    const u64 v = section_ns[i].load();
                    char b[64];
                    std::snprintf(b, sizeof(b), "%s%s %.1f", i ? ", " : "", DrawSectionNames[i],
                                  n ? (v - last_sec[i]) / 1e3 / n : 0.0);
                    secs += b;
                    last_sec[i] = v;
                }
                std::printf("Native Vulkan: CPU per draw (us): %s\n", secs.c_str());
            }
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
    BbHook::Replace(image, size, BbHook::CopyFunction, BbHook::CopyPrologue,
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
    lap.Start();
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
    if ((vs->kind != "vs" && vs->kind != "es") || (ps && ps->kind != "ps") || (gs && gs->kind != "gs")) {
        wrong_stage.fetch_add(1, std::memory_order_relaxed);
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
    if (drawing) {
        // with a GS, the ES runs as the vertex stage and its outputs feed the GS directly (the
        // ES -> GS ring of the PS4 becomes the pipeline's own interface, as in the D3D11 port)
        Draw(ctx, dev, vsh, *vs, psh, ps, gsh, gs, colors, depth_target, indexed, count, index_addr);
        EndOp();
        return;
    }
    // stage 4: count only
    StageRes vres, pres;
    BindStage(true, *vs, ctx, dev, vres);
    if (ps) {
        BindStage(false, *ps, ctx, dev, pres);
    }
    VertexInput vi;
    FetchVertices(ctx, vsh, *vs, vi, [] { return 0u; });
}

void OnSubmit() {
    if (!drawing) {
        return;
    }
    Timer timer(submit_ns);
    std::lock_guard lk(mutex);
    submits_seen.fetch_add(1, std::memory_order_relaxed);
    Geo::Collect();
    SubmitPacket& sp = Push(Op::Submit, open_batch->submits);
    sp.shown.clear();
    if (show) {
        // the display buffers the presenter asked for go to General layout for it
        std::lock_guard dl(display_mutex);
        for (const auto& [address, s] : shown) {
            const auto it = targets.find(address);
            if (it != targets.end() && !it->second->img.depth) {
                sp.shown.push_back({address, it->second.get()});
            }
        }
    }
    submits_queued.fetch_add(1, std::memory_order_release);
    Flush();
}

namespace {
void RunSubmit(const SubmitPacket& sp) {
    for (const auto& [address, t] : sp.shown) {
        Gpu::Use(t->img, vk::ImageLayout::eGeneral);
        std::lock_guard dl(display_mutex);
        shown[address] = Shown{t->img.image, t->img.format, t->img.width, t->img.height};
    }
    Gpu::Submit();
    {
        std::lock_guard rl(recorded_mutex);
        submits_recorded.fetch_add(1, std::memory_order_release);
    }
    recorded_cv.notify_all();
}

void RunRetire(std::unique_ptr<Target>& t) {
    Gpu::EndRendering(); // the open pass may be drawing to it
    DestroyTarget(*instance.load(), *t); // its image goes once the GPU is done with it
    t.reset();
}

/// Records a batch in order (the recorder thread, or the game's with BB_VK_RECORD_THREAD=0).
void RunBatch(Batch& b) {
    std::lock_guard g(gpu_mutex);
    for (const auto [op, i] : b.ops) {
        switch (op) {
        case Op::Draw:
            RecordDraw(b.draws[i]);
            break;
        case Op::Clear:
            RunClear(b.clears[i]);
            break;
        case Op::Copy:
            RunCopy(b.copies[i]);
            break;
        case Op::GeoUpload: {
            GeoUploadPacket& u = b.geo_uploads[i];
            if (ToRing(u.data)) {
                const vk::BufferCopy region{.srcOffset = u.data.offset, .dstOffset = u.dst_offset,
                                            .size = u.data.size};
                Gpu::Prelude().copyBuffer(u.data.buffer, u.dst, region);
            }
            break;
        }
        case Op::GeoFree: {
            const GeoFreePacket f = b.geo_frees[i];
            const VmaAllocator vma = instance.load()->GetAllocator();
            Gpu::Defer([vma, f] { vmaDestroyBuffer(vma, f.buffer, f.allocation); });
            break;
        }
        case Op::Retire:
            RunRetire(b.retired[i]);
            break;
        case Op::Submit:
            RunSubmit(b.submits[i]);
            break;
        }
    }
}

/// Hands the open batch to the recorder (the game's thread, under `mutex`): waits when the
/// recorder is a whole queue behind.
void Flush() {
    if (open_batch->ops.empty()) {
        return;
    }
    if (!record_thread) {
        RunBatch(*open_batch);
        open_batch->Clear();
        return;
    }
    std::unique_lock lk(queue_mutex);
    queued_batches.push_back(open_batch);
    queue_cv.notify_one();
    if (free_batches.empty()) {
        batch_waits.fetch_add(1, std::memory_order_relaxed);
        free_cv.wait(lk, [] { return !free_batches.empty(); });
    }
    open_batch = free_batches.front();
    free_batches.pop_front();
}

void RecorderThread() {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), L"bb:VkRecorder");
#endif
    for (;;) {
        Batch* b = nullptr;
        {
            std::unique_lock lk(queue_mutex);
            queue_cv.wait(lk, [] { return !queued_batches.empty(); });
            b = queued_batches.front();
            queued_batches.pop_front();
        }
        RunBatch(*b);
        b->Clear();
        {
            std::lock_guard lk(queue_mutex);
            free_batches.push_back(b);
        }
        free_cv.notify_one();
    }
}

// resident geometry's uploads and frees go through the queue too (Geo::Host)
u64 GeoSerial() {
    return submits_queued.load(std::memory_order_relaxed);
}
void GeoUpload(const void* src, u64 n, VkBuffer dst, u64 dst_offset) {
    GeoUploadPacket& u = Push(Op::GeoUpload, open_batch->geo_uploads);
    u.data = Stage(n, 16);
    std::memcpy(u.data.data, src, n);
    u.dst = dst;
    u.dst_offset = dst_offset;
}
void GeoFree(VkBuffer buffer, VmaAllocation allocation) {
    Push(Op::GeoFree, open_batch->geo_frees) = GeoFreePacket{buffer, allocation};
}

/// The batches and, unless BB_VK_RECORD_THREAD=0, the recorder thread (once Gpu is up).
void StartRecorder() {
    for (Batch& b : batch_pool) {
        free_batches.push_back(&b);
    }
    open_batch = free_batches.front();
    free_batches.pop_front();
    const char* rt = std::getenv("BB_VK_RECORD_THREAD");
    record_thread = !(rt && rt[0] == '0');
    if (record_thread) {
        std::thread(RecorderThread).detach();
    }
    std::printf("Native Vulkan: %s\n",
                record_thread ? "draws are captured on the game's thread and recorded on their own "
                                "thread (BB_VK_RECORD_THREAD=0: on the game's thread)"
                              : "draws are recorded on the game's thread (BB_VK_RECORD_THREAD=0)");
}
} // namespace

bool DrawsAlone() {
    return alone.load(std::memory_order_relaxed);
}

bool Display(std::uint64_t address, std::uint32_t wanted, std::uint64_t* image,
             std::uint32_t* view_format, std::uint32_t* width, std::uint32_t* height) {
    frames_asked.fetch_add(1, std::memory_order_relaxed);
    if (!show) {
        return false;
    }
    if (record_thread) {
        // the submissions so far (this frame's among them) are recorded and on the queue first
        const u64 want = submits_queued.load(std::memory_order_acquire);
        std::unique_lock rl(recorded_mutex);
        recorded_cv.wait_for(rl, std::chrono::milliseconds(200),
                             [want] { return submits_recorded.load(std::memory_order_acquire) >= want; });
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
