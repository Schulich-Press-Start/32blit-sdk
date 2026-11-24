#include "dialogue_box.hpp"
#include "text_lines.hpp"

#include <limits>
#include <utility>

#include "graphics/surface.hpp"

namespace sdk {

  bool DialogueBox::open(const blit::Rect &bounds, const blit::Font &font,
                         std::initializer_list<std::string_view> pages) {
    return open(bounds, font, pages.begin(), pages.size());
  }

  bool DialogueBox::open(const blit::Rect &bounds, const blit::Font &font,
                         const std::vector<std::string_view> &pages) {
    return open(bounds, font, pages.data(), pages.size());
  }

  bool DialogueBox::open(const blit::Rect &bounds, const blit::Font &font,
                         const std::string_view *pages, std::size_t count) {
    constexpr auto max_coordinate = std::numeric_limits<int32_t>::max();
    if(count == 0 || bounds.empty() || !font.data || !font.char_w_variable ||
       !font.char_w || !font.char_h || bounds.h < font.char_h ||
       bounds.x > max_coordinate - bounds.w || bounds.y > max_coordinate - bounds.h) return false;

    Layout layout;
    layout.page_ends.reserve(count);
    const int line_height = font.char_h + font.spacing_y;
    const int lines_per_page = 1 + (bounds.h - font.char_h) / line_height;
    int lines = 0;

    const auto finish_page = [&] {
      layout.page_ends.push_back(layout.line_ends.size());
      lines = 0;
    };
    const auto add_line = [&](std::string_view line) {
      if(lines == lines_per_page) finish_page();
      if(!line.empty()) layout.text.append(line);
      layout.line_ends.push_back(layout.text.size());
      ++lines;
    };

    for(std::size_t i = 0; i < count; ++i) {
      if(!detail::wrap_text_lines(pages[i], bounds.w, font, false, add_line)) return false;
      finish_page();
    }

    layout_ = std::move(layout);
    bounds_ = bounds;
    font_ = font;
    page_ = 0;
    return true;
  }

  bool DialogueBox::active() const {
    return page_ < layout_.page_ends.size();
  }

  DialogueBox::AdvanceResult DialogueBox::advance() {
    if(!active()) return AdvanceResult::inactive;
    if(++page_ < layout_.page_ends.size()) return AdvanceResult::next_page;
    close();
    return AdvanceResult::finished;
  }

  void DialogueBox::close() {
    layout_ = {};
    page_ = 0;
  }

  void DialogueBox::draw(blit::Surface &surface, const blit::Pen &pen) const {
    if(!active() || !surface.data || surface.bounds.empty() || surface.clip.empty() || !pen.a) return;
    const auto clipped_bounds = bounds_.intersection({0, 0, surface.bounds.w, surface.bounds.h});
    if(clipped_bounds.empty()) return;
    const auto visible = clipped_bounds.intersection(surface.clip);
    if(visible.empty()) return;

    const auto old_clip = surface.clip;
    const auto old_pen = surface.pen;
    surface.clip = visible;
    surface.pen = pen;

    const std::string_view text = layout_.text;
    const auto first = page_ == 0 ? 0 : layout_.page_ends[page_ - 1];
    const auto end = layout_.page_ends[page_];
    auto offset = first == 0 ? 0 : layout_.line_ends[first - 1];
    const int line_height = font_.char_h + font_.spacing_y;
    for(auto line = first; line < end; ++line) {
      const auto next = layout_.line_ends[line];
      if(next != offset) {
        const auto y = bounds_.y + static_cast<int32_t>(line - first) * line_height;
        surface.text(text.substr(offset, next - offset), font_, {bounds_.x, y, bounds_.w, font_.char_h});
      }
      offset = next;
    }

    surface.pen = old_pen;
    surface.clip = old_clip;
  }
}
