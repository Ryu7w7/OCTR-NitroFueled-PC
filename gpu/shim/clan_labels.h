#pragma once
#include "imgui.h"
#include <string_view>

namespace ClanLabels {
// Draw a UTF-8 label with a bracketed tag. Preserve the caller's font and tint.
void Text(std::string_view label, ImVec4 color, const char* nickname = nullptr);
void Draw(ImDrawList* list, ImFont* font, float size, ImVec2 pos, ImU32 color, std::string_view label, const char* nickname = nullptr);
void Reset();
}
