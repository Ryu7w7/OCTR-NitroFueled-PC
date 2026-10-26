// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_engine_probe.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace BbEngineProbe {
namespace {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// CUSA13795 1.21 (both update builds), from docs/d3d11_engine_hooks_1.21.md.
constexpr u64 ClearFunction = 0x13c9e50;   // kClearRenderTarget(ctx, flags, float4*, stencil, depth)
constexpr u64 CopyFunction = 0x13cb1d0;    // kCopyTexture(ctx, T# src, T# dst, s32 p[8])
constexpr u64 DeviceGlobal = 0x2aebce0;    // igPS4GraphicsDevice*
constexpr u64 DevContext = 0x5b0;          // igPS4GraphicsDevice -> igPS4GraphicsDeviceContext
constexpr u64 CtxViewport = 0x0c, CtxColorTargets = 0x130, CtxDepthTarget = 0x170;

/// The first 17 bytes of both functions (`push rbp; mov rbp, rsp; push r15..rbx`, then the frame
/// setup): whole instructions, position independent, moved into the stub as they are.
constexpr std::size_t PrologueSize = 17;
constexpr u8 ClearPrologue[PrologueSize] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                            0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
constexpr u8 CopyPrologue[PrologueSize] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                           0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xe4, 0xe0};

u64 image_base = 0;
std::atomic<bool> enabled{false};
std::atomic<u64> clears{0}, copies{0}, draws_indexed{0}, draws_auto{0};
std::atomic<u64> ctx_match{0}, ctx_mismatch{0}, ctx_unreadable{0};
std::atomic<int> clear_logs_left{8}, copy_logs_left{4};

/// Reads guest memory without faulting on a bad pointer.
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

u64 Q(u64 address) {
    u64 v = 0;
    return Read(address, &v, 8) ? v : 0;
}

/// The context the device global leads to, 0 if a pointer on the way is unreadable.
u64 ContextFromGlobal() {
    const u64 device = Q(image_base + DeviceGlobal);
    return device ? Q(device + DevContext) : 0;
}

void CheckContext(u64 ctx, u64* from_global) {
    *from_global = ContextFromGlobal();
    if (!*from_global) {
        ctx_unreadable.fetch_add(1, std::memory_order_relaxed);
    } else if (*from_global == ctx) {
        ctx_match.fetch_add(1, std::memory_order_relaxed);
    } else {
        ctx_mismatch.fetch_add(1, std::memory_order_relaxed);
    }
}

/// Base address of a Gnm render target (+0x10: CB_COLOR_BASE, 256-byte units), 0 if none.
u64 TargetBase(u64 handle, u64 base_field) {
    u32 v = 0;
    return handle && Read(handle + base_field, &v, 4) ? u64(v) << 8 : 0;
}

__attribute__((sysv_abi)) void OnClear(u64 ctx, u32 flags, const float* color, u32 stencil,
                                       float depth) {
    clears.fetch_add(1, std::memory_order_relaxed);
    u64 from_global = 0;
    CheckContext(ctx, &from_global);
    if (clear_logs_left.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    float c[4]{};
    if (flags & 1) {
        Read(reinterpret_cast<u64>(color), c, sizeof(c));
    }
    struct {
        std::int32_t x, y, w, h;
    } vp{};
    Read(ctx + CtxViewport, &vp, sizeof(vp));
    std::printf("Engine probe: clear ctx %llx (global -> %llx %s) flags %x color %.2f %.2f %.2f %.2f "
                "depth %.3f stencil %u rt0 %llx depth target %llx viewport %d,%d %dx%d\n",
                (unsigned long long)ctx, (unsigned long long)from_global,
                from_global == ctx ? "same" : "DIFFERENT", flags, c[0], c[1], c[2], c[3], depth,
                stencil, (unsigned long long)TargetBase(Q(ctx + CtxColorTargets), 0x10),
                (unsigned long long)TargetBase(Q(ctx + CtxDepthTarget), 0x58), vp.x, vp.y, vp.w, vp.h);
    std::fflush(stdout);
}

__attribute__((sysv_abi)) void OnCopy(u64 ctx, const u32* src, const u32* dst, const std::int32_t* p) {
    copies.fetch_add(1, std::memory_order_relaxed);
    u64 from_global = 0;
    CheckContext(ctx, &from_global);
    if (copy_logs_left.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    std::int32_t a[8]{};
    Read(reinterpret_cast<u64>(p), a, sizeof(a));
    std::printf("Engine probe: copy ctx %llx (global -> %s) src %llx dst %llx src %d,%d dst %d,%d "
                "size %dx%d mips %d->%d\n",
                (unsigned long long)ctx, from_global == ctx ? "same" : "DIFFERENT",
                (unsigned long long)reinterpret_cast<u64>(src),
                (unsigned long long)reinterpret_cast<u64>(dst), a[0], a[1], a[2], a[3], a[4], a[5],
                a[6], a[7]);
    std::fflush(stdout);
}

/// Executable memory for the stubs.
u8* AllocCode(std::size_t size) {
#ifdef _WIN32
    return static_cast<u8*>(
        VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#endif
}

void Emit(u8*& at, std::initializer_list<u8> bytes) {
    for (const u8 b : bytes) {
        *at++ = b;
    }
}

void Emit64(u8*& at, u64 v) {
    std::memcpy(at, &v, 8);
    at += 8;
}

/// `jmp [rip + 0]` followed by the absolute target: 14 bytes, no register touched.
void EmitJump(u8*& at, u64 target) {
    Emit(at, {0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
    Emit64(at, target);
}

/// Stub: saves the argument registers (rdi, rsi, rdx, rcx, r8, r9, xmm0-7), calls the handler
/// with the function's own arguments, restores them, runs the moved prologue and jumps back
/// past it. At entry rsp is 8 mod 16; 6 pushes and 0x88 bytes make the call 16-aligned.
u8* EmitStub(u8* at, u64 function, const u8* prologue, u64 handler) {
    Emit(at, {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51});
    Emit(at, {0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00}); // sub rsp, 0x88
    for (u8 n = 0; n < 8; ++n) {
        Emit(at, {0xf3, 0x0f, 0x7f, u8(0x44 | n << 3), 0x24, u8(n * 0x10)}); // movdqu [rsp+n*16], xmmN
    }
    Emit(at, {0x48, 0xb8}); // mov rax, handler
    Emit64(at, handler);
    Emit(at, {0xff, 0xd0}); // call rax
    for (u8 n = 0; n < 8; ++n) {
        Emit(at, {0xf3, 0x0f, 0x6f, u8(0x44 | n << 3), 0x24, u8(n * 0x10)}); // movdqu xmmN, [rsp+n*16]
    }
    Emit(at, {0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00}); // add rsp, 0x88
    Emit(at, {0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f});
    std::memcpy(at, prologue, PrologueSize);
    at += PrologueSize;
    EmitJump(at, function + PrologueSize);
    return at;
}

bool Hook(unsigned char* image, std::uint64_t size, u64 offset, const u8* prologue, u64 handler,
          u8*& stub, const char* name) {
    if (offset + PrologueSize > size || std::memcmp(image + offset, prologue, PrologueSize) != 0) {
        std::printf("Engine probe: %s at +%#llx does not match (another game version?): not hooked\n",
                    name, (unsigned long long)offset);
        return false;
    }
    const u64 function = reinterpret_cast<u64>(image) + offset;
    u8* entry = stub;
    stub = EmitStub(stub, function, prologue, handler);
    u8* at = image + offset;
    EmitJump(at, reinterpret_cast<u64>(entry));
    std::memset(at, 0xcc, PrologueSize - 14); // never reached: the stub returns past the prologue
    std::printf("Engine probe: %s hooked at +%#llx\n", name, (unsigned long long)offset);
    return true;
}

void Report() {
    u64 last_draws = 0, last_clears = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const u64 d = draws_indexed.load() + draws_auto.load(), c = clears.load();
        if (d == last_draws && c == last_clears) {
            continue;
        }
        std::printf("Engine probe: %.0f draws/s (%llu indexed, %llu auto in all), %.1f clears/s, "
                    "%llu copies; context from the global: %llu same, %llu different, %llu unreadable\n",
                    (d - last_draws) / 5.0, (unsigned long long)draws_indexed.load(),
                    (unsigned long long)draws_auto.load(), (c - last_clears) / 5.0,
                    (unsigned long long)copies.load(), (unsigned long long)ctx_match.load(),
                    (unsigned long long)ctx_mismatch.load(), (unsigned long long)ctx_unreadable.load());
        std::fflush(stdout);
        last_draws = d;
        last_clears = c;
    }
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    const char* env = std::getenv("BB_ENGINE_PROBE");
    if (!env || env[0] != '1') {
        return;
    }
    image_base = reinterpret_cast<u64>(image);
    u8* stub = AllocCode(4096);
    if (!stub) {
        std::printf("Engine probe: no memory for the stubs\n");
        return;
    }
    u8* const stubs = stub;
    const bool clear = Hook(image, size, ClearFunction, ClearPrologue,
                            reinterpret_cast<u64>(&OnClear), stub, "kClearRenderTarget");
    const bool copy = Hook(image, size, CopyFunction, CopyPrologue,
                           reinterpret_cast<u64>(&OnCopy), stub, "kCopyTexture");
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), stubs, stub - stubs);
    FlushInstructionCache(GetCurrentProcess(), image, size);
#endif
    std::printf("Engine probe: on (%s, %s); device global at +%#llx, context at device +%#llx\n",
                clear ? "clear" : "no clear", copy ? "copy" : "no copy",
                (unsigned long long)DeviceGlobal, (unsigned long long)DevContext);
    std::fflush(stdout);
    enabled.store(true);
    std::thread(Report).detach();
}

void NoteDraw(bool indexed) {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    (indexed ? draws_indexed : draws_auto).fetch_add(1, std::memory_order_relaxed);
}
} // namespace BbEngineProbe
