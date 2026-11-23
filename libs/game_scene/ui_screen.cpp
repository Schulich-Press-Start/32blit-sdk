#include "ui_screen.hpp"

#include "graphics/font.hpp"
#include "graphics/surface.hpp"
#include "ui_layout.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace sdk {
  namespace {
    constexpr blit::Pen pen(ui::Rgba color) {
      return {color.r, color.g, color.b, color.a};
    }

  }

  struct UiScreen::State {
    ui::Layout layout;
    std::vector<std::size_t> paint_order;
    std::size_t initial = 0, focus = 0;

    void step(int dx, int dy) {
      const bool horizontal = dx != 0;
      const bool forward = horizontal ? dx > 0 : dy > 0;
      const auto &current = layout.widgets[focus];
      const int center_x = 2 * current.x + current.w;
      const int center_y = 2 * current.y + current.h;
      const int lane_start = horizontal ? current.y : current.x;
      const int lane_end = lane_start + (horizontal ? current.h : current.w);
      auto best = layout.widgets.size();
      bool best_outside_lane = true;
      int64_t best_distance = 0;
      for(std::size_t i = 0; i < layout.widgets.size(); ++i) {
        const auto &widget = layout.widgets[i];
        if(!ui::is_focusable(layout, widget)) continue;
        const int64_t x = 2 * widget.x + widget.w - center_x;
        const int64_t y = 2 * widget.y + widget.h - center_y;
        const auto along = horizontal ? x : y;
        if(forward ? along <= 0 : along >= 0) continue;

        // Stay in the current row/column if possible, then pick the nearest center.
        const int start = horizontal ? widget.y : widget.x;
        const int end = start + (horizontal ? widget.h : widget.w);
        const bool outside_lane = end <= lane_start || start >= lane_end;
        if(outside_lane && !best_outside_lane) continue;
        const auto distance = x * x + y * y;
        if(best == layout.widgets.size() || outside_lane != best_outside_lane || distance < best_distance) {
          best = i;
          best_outside_lane = outside_lane;
          best_distance = distance;
        }
      }
      if(best != layout.widgets.size()) focus = best;
    }
  };

  UiScreen::UiScreen() = default;
  UiScreen::~UiScreen() = default;

  bool UiScreen::load(const std::string &path) {
    const auto fail = [&](const char *reason) {
      std::fprintf(stderr, "%s: %s\n", path.empty() ? "<empty UI layout path>" : path.c_str(), reason);
      return false;
    };

    ui::Layout layout;
    try {
      std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
      if(!input) return fail("Cannot open UI layout.");
      const auto json = nlohmann::json::parse(input, nullptr, false);
      if(input.bad()) return fail("Cannot read UI layout.");
      if(json.is_discarded()) return fail("Invalid UI layout JSON.");
      if(const auto error = ui::read_layout(json, layout)) return fail(error);
    } catch(const std::filesystem::filesystem_error &error) {
      return fail(error.what());
    } catch(const std::ios_base::failure &error) {
      return fail(error.what());
    }

    if(!state_) state_ = std::make_unique<State>();
    state_->initial = ui::initial_focus_index(layout);
    state_->paint_order = ui::paint_order(layout);
    state_->layout = std::move(layout);
    reset_focus();
    return true;
  }

  void UiScreen::reset_focus() {
    if(state_) state_->focus = state_->initial;
  }

  std::string_view UiScreen::focused_id() const {
    if(!state_ || state_->focus == state_->layout.widgets.size()) return {};
    return state_->layout.widgets[state_->focus].id;
  }

  std::string_view UiScreen::update(int dx, int dy, bool accept) {
    if(!state_ || state_->focus == state_->layout.widgets.size()) return {};
    if(dx || dy) state_->step(dx, dy);
    if(!accept) return {};
    return state_->layout.widgets[state_->focus].id;
  }

  void UiScreen::draw(blit::Surface &surface) const {
    if(!state_ || !surface.data || surface.bounds.empty()) return;
    const auto &layout = state_->layout;
    const blit::Rect canvas{(surface.bounds.w - layout.width) / 2, (surface.bounds.h - layout.height) / 2,
                            layout.width, layout.height};
    const auto clip = surface.clip;
    const auto pen_before = surface.pen;
    const auto visible = canvas.intersection({0, 0, surface.bounds.w, surface.bounds.h}).intersection(clip);
    if(visible.empty()) return;
    surface.clip = visible;
    for(const auto i : state_->paint_order) {
      const auto &widget = layout.widgets[i];
      const blit::Rect box{canvas.x + widget.x, canvas.y + widget.y, widget.w, widget.h};
      if(visible.intersection(box).empty()) continue;
      const auto style = ui::widget_style(widget, i == state_->focus);
      ui::draw_widget_box(box.w, box.h, style, [&](int x, int y, int w, int h, ui::Rgba color) {
        surface.pen = pen(color);
        surface.rectangle({box.x + x, box.y + y, w, h});
      });
      if(!style.text.a || widget.kind == ui::Kind::Panel || widget.text.empty()) continue;

      auto text_box = box;
      auto alignment = blit::TextAlign::top_left;
      if(widget.kind == ui::Kind::Button) {
        constexpr int inset = ui::button_text_inset;
        text_box = {box.x + inset, box.y + inset, box.w - 2 * inset, box.h - 2 * inset};
        alignment = blit::TextAlign::center_center;
        if(text_box.empty()) continue;
      }
      const auto text_clip = visible.intersection(text_box);
      if(text_clip.empty()) continue;
      surface.clip = text_clip;
      surface.pen = pen(style.text);
      surface.text(widget.text, blit::minimal_font, text_box, true, alignment);
      surface.clip = visible;
    }
    surface.clip = clip;
    surface.pen = pen_before;
  }
}
