// SPDX-License-Identifier: GPL-2.0-or-later
// Headless ImGui geometry test: loss of focus must not suppress the original bar.
#include "../gpu/shim/bbport_mod_sync_panel.h"
#include <cassert>

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    io.Fonts->AddFontDefault();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    BbModSync::State state{"downloading", "Fixture mod", 25, 100, 100, true};
    int focused_vertices = 0;
    for (int frame = 0; frame < 5; ++frame) {
        io.AddFocusEvent(frame == 0 || frame == 4);
        ImGui::NewFrame();
        BbModSync::DrawPanel(state);
        ImGui::Render();
        const auto* draw = ImGui::GetDrawData();
        assert(draw->TotalVtxCount > 100);
        if (frame == 0) focused_vertices = draw->TotalVtxCount;
        else assert(draw->TotalVtxCount == focused_vertices);
        assert(!io.WantCaptureMouse && !io.WantCaptureKeyboard);
    }
    state.visible = false;
    ImGui::NewFrame();
    BbModSync::DrawPanel(state);
    ImGui::Render();
    assert(ImGui::GetDrawData()->TotalVtxCount == 0);
    ImGui::DestroyContext();
}
