// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: pass-through hooks on functions of the game's image (the engine probe and the native
// Vulkan renderer). The handler runs with the function's own arguments, then the function runs
// as usual.
#pragma once

#include <cstddef>
#include <cstdint>

namespace BbHook {
/// The first 17 bytes of kClearRenderTarget and kCopyTexture (`push rbp; mov rbp, rsp;
/// push r15..rbx`, then the frame setup): whole, position independent instructions.
constexpr std::size_t PrologueSize = 17;

/// CUSA13795 1.21 (both update builds), from docs/d3d11_engine_hooks_1.21.md.
constexpr std::uint64_t ClearFunction = 0x13c9e50; // kClearRenderTarget(ctx, flags, float4*, stencil, depth)
constexpr std::uint64_t CopyFunction = 0x13cb1d0;  // kCopyTexture(ctx, T# src, T# dst, s32 p[8])
constexpr std::uint8_t ClearPrologue[PrologueSize] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                      0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                      0x53, 0x48, 0x83, 0xec, 0x48};
constexpr std::uint8_t CopyPrologue[PrologueSize] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                     0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                     0x53, 0x48, 0x83, 0xe4, 0xe0};

/// CUSA14876 (EU) 1.21 has both functions 0x10 earlier; the engine global each loads right after
/// its prologue (RIP-relative, at +0x11 and +0x18) is at the same address in both editions.
constexpr std::uint64_t EuShift = 0x10;
constexpr std::uint64_t ClearCopyGlobal = 0x2807098;
constexpr std::uint64_t UnknownEdition = ~std::uint64_t{0};

/// What to subtract from ClearFunction and CopyFunction for this executable: 0 (USA 1.21),
/// EuShift (EU 1.21) or UnknownEdition when neither layout matches.
std::uint64_t EditionShift(const unsigned char* image, std::uint64_t size);

/// Hooks the function at `offset` of the image when its first bytes are `prologue`: a stub saves
/// the argument registers (rdi, rsi, rdx, rcx, r8, r9, xmm0-7), calls `handler` (System V ABI)
/// with them, restores them and runs the function. `who` prefixes the log lines.
bool PassThrough(unsigned char* image, std::uint64_t size, std::uint64_t offset,
                 const std::uint8_t* prologue, std::uint64_t handler, const char* who,
                 const char* name);

/// As PassThrough, but `handler` returns bool: true means it did the function's work, and the
/// function returns 0 without running (the D3D11 port replaces kCopyTexture the same way).
bool Replace(unsigned char* image, std::uint64_t size, std::uint64_t offset,
             const std::uint8_t* prologue, std::uint64_t handler, const char* who,
             const char* name);
} // namespace BbHook
