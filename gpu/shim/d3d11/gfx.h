// SPDX-License-Identifier: GPL-2.0-or-later
// The renderer (renderer.cpp, FrankHUN88453's port, see LICENSE) as this engine calls it.
//
// The engine's PS4 device context (igPS4GraphicsDeviceContext) keeps the complete render state as
// engine objects; at every draw this backend reads that state and issues the equivalent D3D11 draw
// with the translated shaders (third_party/gcn2hlsl, tools/shaders/build_d3d11.py).
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mutex>

#include "common.h"

/// The device of bbport_d3d11.cpp, and the folder with shaders.idx and the .dxbc files.
void GfxStart(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain1* swap_chain, const char* shaders);

/// Held around everything on the device context: the game draws on its thread, Present on another.
std::mutex& GfxMutex();

/// The gamma the game asks for (sceVideoOutAdjustColor), 1 none.
void GfxSetGamma(float gamma);

/// Called from the GNM draw entry points while the engine records a draw.
void GfxDraw(u32 count, u64 index_addr, bool indexed);

/// sceGnmDrawIndexOffset: `count` indices from `index_offset` in the engine's bound index buffer.
void GfxDrawOffset(u32 count, u32 index_offset);

/// A draw this renderer cannot make yet (the indirect ones: their arguments are in GPU memory):
/// counted, and named with its shaders in a frame capture.
void GfxDrawUnsupported(const char* kind);

/// A copy of the surface at `addr` (a VideoOut buffer) as the game flips it, which GfxPresent shows
/// later: 1..6, 0 none (no such surface). Takes GfxMutex.
u32 GfxSnapshot(u64 addr);

/// The surface at `addr` (a VideoOut buffer), or its `snapshot` (GfxSnapshot) when it still holds
/// that buffer, into `target`, holding GfxMutex.
void GfxPresent(u64 addr, ID3D11RenderTargetView* target, u32 snapshot = 0);

/// Something else used the device context (the overlay), holding GfxMutex: the renderer's record
/// of what is bound no longer holds.
void GfxForgetState();
/// How long a Present took (no lock needed).
void GfxNotePresent(double ms);

/// The engine's hooks (BbHook::Replace): they do the function's work.
SYSV bool GfxClearHook(u64 ctx, u32 flags, const float* color, u32 stencil, float depth);
SYSV bool GfxCopyHook(u64 ctx, const u32* src, const u32* dst, const s32* p);
