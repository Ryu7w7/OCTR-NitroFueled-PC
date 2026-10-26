// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_hook.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace BbHook {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

constexpr std::size_t StubArea = 4096;

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

/// Stub: saves the argument registers, calls the handler with the function's own arguments,
/// restores them, runs the moved prologue and jumps back past it. At entry rsp is 8 mod 16;
/// 6 pushes and 0x88 bytes make the call 16-aligned. `replace`: when the handler returns true
/// (al), the function returns 0 instead of running.
u8* EmitStub(u8* at, u64 function, const u8* prologue, u64 handler, bool replace) {
    Emit(at, {0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51});
    Emit(at, {0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00}); // sub rsp, 0x88
    for (u8 n = 0; n < 8; ++n) {
        Emit(at, {0xf3, 0x0f, 0x7f, u8(0x44 | n << 3), 0x24, u8(n * 0x10)}); // movdqu [rsp+n*16], xmmN
    }
    Emit(at, {0x48, 0xb8}); // mov rax, handler
    Emit64(at, handler);
    Emit(at, {0xff, 0xd0}); // call rax
    if (replace) {
        Emit(at, {0x49, 0x89, 0xc3}); // mov r11, rax (scratch, not an argument register)
    }
    for (u8 n = 0; n < 8; ++n) {
        Emit(at, {0xf3, 0x0f, 0x6f, u8(0x44 | n << 3), 0x24, u8(n * 0x10)}); // movdqu xmmN, [rsp+n*16]
    }
    Emit(at, {0x48, 0x81, 0xc4, 0x88, 0x00, 0x00, 0x00}); // add rsp, 0x88
    Emit(at, {0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f});
    if (replace) {
        Emit(at, {0x45, 0x84, 0xdb}); // test r11b, r11b
        Emit(at, {0x74, 0x03});       // jz: run the function
        Emit(at, {0x31, 0xc0, 0xc3}); // xor eax, eax; ret
    }
    std::memcpy(at, prologue, PrologueSize);
    at += PrologueSize;
    EmitJump(at, function + PrologueSize);
    return at;
}
} // namespace

namespace {
bool Install(unsigned char* image, std::uint64_t size, std::uint64_t offset,
             const std::uint8_t* prologue, std::uint64_t handler, const char* who, const char* name,
             bool replace) {
    static std::mutex mutex;
    static u8* area = nullptr;
    static std::size_t used = 0;
    std::lock_guard lk(mutex);
    if (offset + PrologueSize > size || std::memcmp(image + offset, prologue, PrologueSize) != 0) {
        std::printf("%s: %s at +%#llx does not match (another game version, or hooked already): "
                    "not hooked\n",
                    who, name, (unsigned long long)offset);
        return false;
    }
    if (!area) {
        area = AllocCode(StubArea);
        if (!area) {
            std::printf("%s: no memory for the hook stubs\n", who);
            return false;
        }
    }
    if (used + 256 > StubArea) {
        std::printf("%s: no room left for the %s stub\n", who, name);
        return false;
    }
    const u64 function = reinterpret_cast<u64>(image) + offset;
    u8* entry = area + used;
    u8* end = EmitStub(entry, function, prologue, handler, replace);
    used = end - area;
    u8* at = image + offset;
    EmitJump(at, reinterpret_cast<u64>(entry));
    std::memset(at, 0xcc, PrologueSize - 14); // never reached: the stub returns past the prologue
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), entry, end - entry);
    FlushInstructionCache(GetCurrentProcess(), image + offset, PrologueSize);
#endif
    std::printf("%s: %s hooked at +%#llx%s\n", who, name, (unsigned long long)offset,
                replace ? " (replaced when handled)" : "");
    return true;
}
} // namespace

bool PassThrough(unsigned char* image, std::uint64_t size, std::uint64_t offset,
                 const std::uint8_t* prologue, std::uint64_t handler, const char* who,
                 const char* name) {
    return Install(image, size, offset, prologue, handler, who, name, false);
}

bool Replace(unsigned char* image, std::uint64_t size, std::uint64_t offset,
             const std::uint8_t* prologue, std::uint64_t handler, const char* who,
             const char* name) {
    return Install(image, size, offset, prologue, handler, who, name, true);
}
} // namespace BbHook
