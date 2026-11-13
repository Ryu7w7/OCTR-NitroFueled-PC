// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what the Direct3D 11 renderer (renderer.cpp, texture.cpp: FrankHUN88453's, see LICENSE)
// asks of its host, here the runtime and BbMem instead of that port's own loader.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

#include "common.h"

extern u64 g_image_base, g_image_size;

/// With the image loaded, and BbMem installed before the first draw: the write watch's hooks.
void GlueInstall(unsigned char* image, u64 image_size);

int ConfigInt(const char* name, int fallback);
unsigned ConfigGeneration();

/// The range is the game's image or mapped guest memory.
bool GuestReadable(u64 addr, u64 size);
/// The game wrote the range since GuestResetWritten; `reset` watches it again.
bool GuestWrittenSince(u64 addr, u64 size, bool reset);
void GuestResetWritten(u64 addr, u64 size);
/// Counts the writes the watch saw: what was clean stays clean while this does not change, so a
/// frame without writes checks no page.
u64 GuestWriteCount();
/// `written` goes up when the game writes or unmaps the memory of a render target (a video decoded
/// where a menu's target was): what is there is no longer what the target holds.
void GuestWatchTarget(u64 addr, u64 size, const std::shared_ptr<std::atomic<bool>>& written);

std::filesystem::path UserRoot();
