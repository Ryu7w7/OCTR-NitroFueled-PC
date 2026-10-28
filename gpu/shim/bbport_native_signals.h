// SPDX-License-Identifier: GPL-2.0-or-later
// bbport lab: BB_VK_OWN_SIGNALS=1 (with BB_VK_ALONE=1): the native renderer runs what the game's
// command buffers still need from a GPU once it draws the frame alone - fence labels, waits on
// memory, DMA writes, the flip and the end of each frame - instead of shadPS4's command
// processor (Liverpool), which then gets no graphics submissions at all.
//
// Each submission is scanned as the game submits it (after the native renderer has taken its
// draws: BbNativeVk::OnSubmit). What it signals is run in stream order on one thread, once the
// GPU has finished the native work submitted up to that point (a timeline semaphore), as the
// hardware's end-of-pipe events do.
#pragma once

#include <cstdint>
#include <span>

namespace BbSignals {
/// Decided once, at the game's first submission (Decide): BB_VK_OWN_SIGNALS=1, the native
/// renderer draws alone and its GPU has a timeline semaphore. On() is false until then.
bool Decide();
bool On();

/// BB_VK_OWN_PRESENT=1 (with On()): flips prepare the presenter's frame on the signal thread from
/// the native image (Presenter::PrepareNativeFrame), not on shadPS4's GPU thread.
bool OwnPresent();

/// One command buffer of a game submission (sceGnmSubmit*: after BbNativeVk::OnSubmit and the
/// flip patch).
void Submit(std::span<const std::uint32_t> dcb);

/// sceGnmSubmitDone: frame `frame` is retired (GnmDriver::NoteFramesRetired) and the guest's
/// submission lock released once the GPU has finished what was submitted until now.
void FrameDone(std::uint64_t frame);
} // namespace BbSignals
