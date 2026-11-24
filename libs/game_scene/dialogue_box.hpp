#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "graphics/font.hpp"
#include "types/rect.hpp"

namespace blit { struct Pen; struct Surface; }

namespace sdk {
  /// A screen-space text box. The game owns the panel, input, and any dialogue rules.
  class DialogueBox {
  public:
    enum class AdvanceResult { inactive, next_page, finished };

    /// Copy and lay out the pages, starting at the first one. Bounds describe the text area.
    /// Each supplied page starts a new display page, and long pages may span several.
    /// Font glyph tables must remain valid while open. Invalid input leaves any open dialogue intact.
    bool open(const blit::Rect &bounds, const blit::Font &font, std::initializer_list<std::string_view> pages);
    bool open(const blit::Rect &bounds, const blit::Font &font, const std::vector<std::string_view> &pages);

    bool active() const;
    /// Call once per advance press. Advancing past the last page closes the box and returns finished.
    AdvanceResult advance();
    void close();

    /// Draw the current page within the text bounds, surface bounds, and existing clip.
    void draw(blit::Surface &surface, const blit::Pen &pen) const;

  private:
    bool open(const blit::Rect &bounds, const blit::Font &font, const std::string_view *pages, std::size_t count);

    struct Layout {
      std::string text;
      std::vector<std::size_t> line_ends; // Byte offset after each line in text.
      std::vector<std::size_t> page_ends; // Line index after each page.
    };

    Layout layout_;
    blit::Rect bounds_;
    blit::Font font_{nullptr, nullptr, 0, 0};
    std::size_t page_ = 0;
  };
}
