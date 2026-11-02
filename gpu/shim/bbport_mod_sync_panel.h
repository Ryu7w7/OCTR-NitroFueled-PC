// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_mod_sync.h"
#include <algorithm>
#include <cstdio>
#include <imgui.h>

// Original Nitro sync bar geometry, colours and typography. Display is independent
// of keyboard/mouse focus and never captures input. Draw on the GAME viewport.
namespace BbModSync {
inline void DrawPanel(const State& state, ImFont* sync_heading_font = nullptr) {
    if (!state.visible) return;
    const auto* viewport = ImGui::GetMainViewport();
    const float scale = std::min(viewport->Size.x / 1280.0f, viewport->Size.y / 720.0f);
    const float x = viewport->Pos.x + viewport->Size.x * 0.5f - 350 * scale;
    const float y = viewport->Pos.y + viewport->Size.y * 0.62f;
    auto point = [&](float a, float b) { return ImVec2(x + a * scale, y + b * scale); };
    auto* draw = ImGui::GetForegroundDrawList(); // pinned ImGui exposes only the main viewport
    auto panel = [&](float a, float b, float w, float h, ImU32 color) {
        draw->AddQuadFilled(point(a + 12, b), point(a + w, b),
                            point(a + w - 12, b + h), point(a, b + h), color);
    };
    panel(4, 8, 700, 190, IM_COL32(0, 18, 38, 210));
    panel(0, 0, 700, 190, IM_COL32(3, 62, 108, 245));
    panel(0, 0, 700, 62, IM_COL32(18, 111, 174, 255));
    // Restrained checker motif, clipped to the banner rather than covering gameplay.
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 20; ++col)
            if ((row + col) % 2 == 0)
                draw->AddRectFilled(point(20 + col * 32, row * 30),
                                    point(52 + col * 32, row * 30 + 30), IM_COL32(104, 192, 235, 23));
    const bool error = state.phase == "error";
    const char* heading = error ? "MOD SYNC PAUSED" : state.phase == "fallback" ? "MOD SYNC OPTIONAL" :
                          state.phase == "ready" ? "MODS READY!" : "SYNCING MODS...";
    ImFont* font = sync_heading_font ? sync_heading_font : ImGui::GetFont();
    draw->AddText(font, 36 * scale, point(32, 11), IM_COL32(0, 30, 58, 255), heading);
    draw->AddText(font, 36 * scale, point(30, 8), IM_COL32(245, 253, 255, 255), heading);
    const char* phase = state.phase == "checking" ? "CHECKING REQUIRED MODS" :
                        state.phase == "downloading" ? "DOWNLOADING" :
                        state.phase == "verifying" ? "VERIFYING FILES" :
                        state.phase == "fallback" ? "RACE CONTINUES WITH FALLBACK" :
                        error ? "PLEASE CHECK THE LAUNCHER" : "MODS LOADED";
    draw->AddText(ImGui::GetFont(), 16 * scale, point(30, 76), IM_COL32(255, 211, 45, 255), phase);
    draw->PushClipRect(point(30, 100), point(665, 129), true);
    draw->AddText(ImGui::GetFont(), 20 * scale, point(30, 102),
                  IM_COL32(221, 242, 255, 255), state.label.c_str());
    draw->PopClipRect();
    panel(30, 143, 635, 19, IM_COL32(0, 29, 56, 255));
    if (state.total > 0) {
        const float fraction = float(state.completed / state.total);
        if (fraction > 0) {
            // Clip a full-width slanted fill to preserve its shape at very low progress.
            draw->PushClipRect(point(30, 143), point(30 + 635 * fraction, 162), true);
            panel(30, 143, 635, 19, IM_COL32(255, 207, 39, 255));
            draw->PopClipRect();
        }
        char percent[24];
        std::snprintf(percent, sizeof percent, "%.0f%%", fraction * 100);
        draw->AddText(ImGui::GetFont(), 16 * scale, point(610, 77), IM_COL32(255, 255, 255, 255), percent);
    }
}

} // namespace BbModSync
