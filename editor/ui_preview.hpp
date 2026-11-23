#pragma once

#include "ui_layout.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace editor {
  inline bool is_ui_layout(std::string_view name) {
    constexpr std::string_view suffix = ".ui.json";
    return name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
  }

  class UiPreview {
  public:
    void invalidate() { needs_parse_ = true; }
    bool draw(std::string &source);

  private:
    void select(std::size_t index);
    bool draw_canvas();
    bool draw_inspector();
    void add_widget(sdk::ui::Kind kind);

    // The document text is authoritative. Reparse after source edits and write back
    // any visual edit before leaving draw(); the tab owns saving and dirty state.
    sdk::ui::Layout layout_;
    std::vector<std::size_t> paint_order_;
    float zoom_ = 1.f;
    float pan_x_ = 0.f, pan_y_ = 0.f;
    enum class Drag { None, Pan, Widget };
    Drag drag_ = Drag::None;
    int drag_x_ = 0, drag_y_ = 0;
    std::size_t selected_ = std::string::npos;
    std::string id_draft_;
    const char *error_ = nullptr;
    bool needs_parse_ = true;
  };
}
