#pragma once

#include <imgui_internal.h>

namespace editor {
  inline bool begin_tab(const char *label, bool *open, ImGuiTabItemFlags flags = 0) {
    const bool selected = ImGui::BeginTabItem(label, open, flags);
    if(!selected || !open || !*open) return selected;
    const auto id = ImGui::GetItemID();
    if(GImGui->HoveredId != ImGui::GetIDWithSeed("#CLOSE", nullptr, id)) return selected;

    // Match the pinned ImGui tab close geometry without replacing its hit testing.
    const auto *bar = GImGui->CurrentTabBar;
    const float size = ImGui::GetFontSize();
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 a(ImMax(min.x, max.x - bar->FramePadding.x - size), min.y + bar->FramePadding.y);
    const ImVec2 b(a.x + size, a.y + size);
    auto *draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({bar->ScrollingRectMinX, min.y}, {bar->ScrollingRectMaxX, max.y}, true);
    draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_Tab));
    const float x = a.x + size * .5f - .5f, y = a.y + size * .5f - .5f;
    const float d = size * .35355f - 1;
    const auto foreground = ImGui::GetColorU32(ImGuiCol_Text);
    draw->AddLine({x-d, y-d}, {x+d, y+d}, foreground);
    draw->AddLine({x-d, y+d}, {x+d, y-d}, foreground);
    draw->PopClipRect();
    return selected;
  }
}
