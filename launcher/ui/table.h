// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <initializer_list>
#include "imgui.h"

namespace LauncherUI {

class Table {
public:
    Table(const char* id, int columns, bool visible = true)
        : columns_(columns), open_(visible && ImGui::BeginTable(id, columns,
              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter)) {
        if (open_) ImGui::PushTextWrapPos(0.0f);
    }
    ~Table() {
        if (open_) {
            ImGui::PopTextWrapPos();
            ImGui::EndTable();
        }
    }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    explicit operator bool() const { return open_; }

    void StretchColumn(const char* label, float weight = 1.0f) {
        ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthStretch, weight);
    }
    void FixedColumn(const char* label, float width) {
        ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, width);
    }
    void Headers() {
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < columns_; ++column) {
            Cell();
            ImGui::TextUnformatted(ImGui::TableGetColumnName(column));
        }
    }
    void Row(float minimum_height = 0.0f) {
        ImGui::TableNextRow(0, minimum_height);
        has_action_ = false;
    }
    void Cell() {
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        has_action_ = false;
    }
    bool Action(const char* label) {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (has_action_ && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ButtonWidth(label) <= right)
            ImGui::SameLine();
        has_action_ = true;
        return ImGui::Button(label);
    }
    static float ButtonWidth(const char* label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    }
    static float ActionsWidth(std::initializer_list<const char*> labels) {
        float total = 0, widest = 0;
        for (const char* label : labels) {
            const float width = ButtonWidth(label);
            if (total > 0) total += ImGui::GetStyle().ItemSpacing.x;
            total += width;
            widest = std::max(widest, width);
        }
        return ImGui::GetContentRegionAvail().x < total + ImGui::GetFontSize() * 22 ? widest : total;
    }
    static void SetupStyle(ImGuiStyle& style) {
        style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.04f, 0.29f, 0.49f, 1.0f);
        style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.26f, 0.63f, 0.81f, 0.40f);
        style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.56f, 0.74f, 0.20f);
        style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.02f, 0.15f, 0.30f, 0.65f);
        style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.06f, 0.25f, 0.43f, 0.65f);
        style.CellPadding = ImVec2(8.0f, 7.0f);
    }

private:
    int columns_;
    bool open_;
    bool has_action_ = false;
};

}  // namespace LauncherUI
