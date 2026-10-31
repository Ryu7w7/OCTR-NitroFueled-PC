// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace BbGraphicsTab {
/// At load, before the image is made executable: the Options menu's Graphics tab
/// (bbport_graphics_tab.cpp). Installed automatically for supported 1.21 builds.
void PatchImage(unsigned char* image, std::uint64_t size);
} // namespace BbGraphicsTab
