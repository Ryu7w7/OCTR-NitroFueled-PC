// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace BbGraphicsTab {
/// At load, before the image is made executable: the Options menu's Graphics tab
/// (bbport_graphics_tab.cpp). Off with graphics_tab=0 in bbport.ini.
void PatchImage(unsigned char* image, std::uint64_t size);
/// Refresh native widgets on their guest thread after changing settings in ImGui.
void RequestRefresh();
} // namespace BbGraphicsTab
