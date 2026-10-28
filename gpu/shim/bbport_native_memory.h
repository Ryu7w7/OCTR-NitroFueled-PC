// SPDX-License-Identifier: GPL-2.0-or-later
// bbport lab: which guest memory the GPU side uses and the write watch on its pages, for the native
// renderer (textures and resident geometry go stale when the game writes them).
//
// With shadPS4's Rasterizer this is the Rasterizer's (mapped ranges, page manager, the runtime's
// memory hooks). BB_VK_NATIVE_ONLY=1 makes no Rasterizer at all: this module then keeps the
// mapped ranges and its own page manager, takes the runtime's hooks and tells the native renderer
// of writes (Vulkan::Rasterizer::native_written / native_unmapped).
#pragma once

#include <cstdint>

namespace BbMem {
/// BB_VK_NATIVE_ONLY=1: no shadPS4 Rasterizer; the native renderer does everything.
bool NativeOnly();

/// NativeOnly: takes over memory tracking (once, before the game maps GPU memory; the runtime
/// replays the mappings made before).
void Install();

/// Install ran: a write the CPU made outside page tracking (MemoryManager::InvalidateMemory,
/// decoded video frames) reaches the native renderer.
void NoteWrite(std::uint64_t addr, std::uint64_t size);

/// A tracker exists (the Rasterizer, or Install ran).
bool Ready();

/// The range is GPU mapped memory.
bool IsMapped(std::uint64_t addr, std::uint64_t size);

/// One more (or one less) watcher on the pages of the range: a write to a watched page reaches
/// the native renderer once and takes no further writes (it unwatches what it hears of).
void Watch(std::uint64_t addr, std::uint64_t size, bool watch);
} // namespace BbMem
