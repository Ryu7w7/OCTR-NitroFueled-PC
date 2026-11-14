// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the experimental Direct3D 11 engine (BB_D3D11=1, Windows). It takes the place of
// shadPS4's Vulkan presenter: no Vulkan instance is made. BbSignals (bbport_native_signals.h) runs
// what the game's command buffers ask of a GPU, as with the native Vulkan renderer alone.
//
// Its renderer is in d3d11/ (FrankHUN88453's, MIT): it reads the engine's render state at each
// draw. BB_D3D11_SHADERS names the folder of translated shaders (tools/shaders/build_d3d11.py);
// without it the game runs behind an empty picture.
#pragma once

#include <cstdint>

namespace BbD3d11 {
/// BB_D3D11=1, on Windows.
bool On();

/// With the image loaded: the engine's texture clears and copies, which it runs as compute
/// shaders on the PS4, come here. Nothing else would run them, and the game then waits for them
/// for ever.
void PatchImage(unsigned char* image, std::uint64_t size);

/// Once, where the Vulkan presenter would be made: the device and the swap chain of this window
/// (an HWND), and the menu. Stops the game when the PC has no Direct3D 11.
void Start(void* window);

/// sceVideoOutAdjustColor.
void SetGamma(float gamma);

/// The game draws (the GNM draw entry points).
void OnDraw(bool indexed, std::uint32_t count, std::uint64_t index_addr);

/// sceGnmDrawIndexOffset: `count` indices from `index_offset` in the bound index buffer.
void OnDrawOffset(std::uint32_t count, std::uint32_t index_offset);

/// The draws not made yet (the indirect ones), by GNM entry point: counted, named in a capture.
void OnDrawUnsupported(const char* kind);

/// The VideoOut buffer at `address` as the game flips it (VideoOutDriver::SubmitFlipInternal): a
/// copy that Present shows, whatever the game draws there meanwhile. 0: none.
std::uint32_t Snapshot(std::uint64_t address);

/// One picture on screen: the VideoOut buffer at `address` (its `snapshot`, when taken), 0 the
/// last one again. On the swap thread (VideoOutDriver::RunPresenter).
void Present(std::uint64_t address, std::uint32_t snapshot = 0);
} // namespace BbD3d11
