#include "appearance.hpp"

#include <imgui.h>
#ifndef __EMSCRIPTEN__
#include <SDL.h>
#include <fstream>
#include <memory>
#include <string>
#endif

namespace editor {
  Appearance::Appearance() {
#ifndef __EMSCRIPTEN__
    const std::unique_ptr<char, decltype(&SDL_free)> dir(SDL_GetPrefPath("32blit", "Editor"), SDL_free);
    if(dir) {
      theme_file_ = std::filesystem::u8path(dir.get()) / "theme.txt";
      std::ifstream input(theme_file_);
      std::string theme;
      dark_ = (input >> theme) && theme == "dark";
    }
#endif
    apply_scale(1.0f);
  }

  void Appearance::apply_scale(float scale) const {
    // Start from logical units every time; repeatedly scaling rounded sizes drifts across monitors.
    auto &s = ImGui::GetStyle();
    s = ImGuiStyle{};
    s.WindowPadding = {10, 9};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {7, 6};
    s.WindowRounding = s.FrameRounding = s.TabRounding = 3;
    s.WindowBorderSize = 1;
    s.ScaleAllSizes(scale);
    apply_colors();
  }

  void Appearance::apply_colors() const {
    struct Palette { ImVec4 ink, muted, paper, chrome, line, accent, hover, selected; };
    static const Palette light = {
      {.16f,.20f,.23f,1}, {.35f,.40f,.43f,1}, {.98f,.97f,.94f,1}, {.88f,.89f,.86f,1},
      {.75f,.78f,.75f,1}, {.24f,.45f,.40f,1}, {.77f,.87f,.82f,1}, {.77f,.87f,.82f,1}
    }, dark = {
      {.94f,.96f,.96f,1}, {.70f,.76f,.79f,1}, {.09f,.12f,.14f,1}, {.16f,.20f,.23f,1},
      {.42f,.49f,.51f,1}, {.48f,.91f,.76f,1}, {.21f,.33f,.33f,1}, {.16f,.40f,.34f,1}
    };
    const auto &p = dark_ ? dark : light;
    auto &c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = p.ink;
    c[ImGuiCol_TextDisabled] = p.muted;
    c[ImGuiCol_WindowBg] = c[ImGuiCol_PopupBg] = c[ImGuiCol_ChildBg] = p.paper;
    c[ImGuiCol_MenuBarBg] = c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = p.chrome;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = p.chrome;
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = p.selected;
    c[ImGuiCol_Header] = dark_ ? p.selected : p.chrome;
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = p.chrome;
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_TabHovered] = p.hover;
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_FrameBgActive] = p.selected;
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_CheckMark] = c[ImGuiCol_NavHighlight] = p.accent;
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = c[ImGuiCol_ScrollbarGrab] = p.line;
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_DockingPreview] = p.accent;
  }

  void Appearance::draw_menu() {
    if(!ImGui::BeginMenu("View")) return;
    const bool changed = ImGui::MenuItem("Dark Mode", nullptr, &dark_);
    ImGui::EndMenu();
    if(!changed) return;
    apply_colors(); // Do not touch the current monitor's scaled sizes or fonts.
#ifndef __EMSCRIPTEN__
    std::ofstream output(theme_file_);
    output << (dark_ ? "dark\n" : "light\n");
    output.close();
    if(!output) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Appearance",
      "The theme changed for this session, but the editor could not save your preference.", SDL_GetKeyboardFocus());
#endif
  }
}
