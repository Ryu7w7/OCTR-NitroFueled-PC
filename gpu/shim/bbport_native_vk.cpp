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

#include "video_core/renderer_vulkan/vk_common.h"

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

u64 image_base = 0;
bool enabled = false;
fs::path shader_root;
std::atomic<VkDevice> device{VK_NULL_HANDLE};

std::mutex mutex; // draws can come from more than one game thread
std::unordered_map<u64, ShaderInfo> shaders;          // FNV-1a 64 of the GCN program
std::unordered_map<u64, ShaderInfo*> shader_by_code;  // program address -> entry (nullptr: unknown)
std::unordered_set<u64> pipeline_keys;

std::atomic<u64> draws{0}, resolved{0}, missing_shader{0}, no_target{0}, no_context{0};
std::atomic<u64> modules_made{0}, modules_failed{0}, unknown_programs{0};

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
    const VkDevice dev = device.load();
    if (s.load_tried || dev == VK_NULL_HANDLE) {
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
    auto [result, module] = vk::Device(dev).createShaderModule(info);
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

void Report() {
    u64 last = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const u64 d = draws.load();
        if (d == last) {
            continue;
        }
        std::size_t keys = 0;
        {
            std::lock_guard lk(mutex);
            keys = pipeline_keys.size();
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
    std::printf("Native Vulkan: on (front end only: shadPS4 still draws); %zu translated shaders "
                "from %s\n",
                shaders.size(), shader_root.string().c_str());
    std::fflush(stdout);
    enabled = true;
    std::thread(Report).detach();
}

void Attach(void* dev) {
    device.store(static_cast<VkDevice>(dev));
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
    u32 targets = 0;
    for (int i = 0; i < 8; ++i) {
        targets |= Q(ctx + CtxColorTargets + i * 8) ? 1u << i : 0;
    }
    const bool depth = Q(ctx + CtxDepthTarget) != 0;
    if (!targets && !depth) {
        no_target.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    resolved.fetch_add(1, std::memory_order_relaxed);
    // what a VkPipeline would be keyed on (target formats join once targets are mapped)
    u64 key = 0xcbf29ce484222325ull;
    Mix(key, vs);
    Mix(key, ps);
    Mix(key, gs);
    Mix(key, targets);
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
