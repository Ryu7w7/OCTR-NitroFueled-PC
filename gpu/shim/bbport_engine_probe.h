// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_ENGINE_PROBE=1 watches the Alchemy render layer the native backend will replace
// (docs/d3d11_engine_hooks_1.21.md): the engine's kClearRenderTarget and kCopyTexture, the
// igPS4GraphicsDevice global and its context, and the GnmDriver draw calls. Log only: the hooked
// functions run unchanged. Each hook site is checked byte for byte (another game version: not
// installed).
#pragma once

#include <cstdint>

namespace BbEngineProbe {
/// Patches the image (the loader calls it once, before any game code runs, while the image is
/// still writable). Does nothing unless BB_ENGINE_PROBE=1.
void PatchImage(unsigned char* image, std::uint64_t size);

/// sceGnmDrawIndex / sceGnmDrawIndexAuto: one draw recorded by the game.
void NoteDraw(bool indexed);
} // namespace BbEngineProbe
