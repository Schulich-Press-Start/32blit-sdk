#pragma once

#include "text_lines.hpp"
#include "ui_layout.hpp"

namespace sdk::ui {
  struct WrappedText {
    std::string text;
    bool overflows = false;
  };

  inline WrappedText prepare_wrapped_text(const Widget &widget, const blit::Font &font) {
    WrappedText result;
    if(!widget.wrap || widget.kind == Kind::Panel || widget.text.empty()) return result;
    const int inset = widget.kind == Kind::Button ? button_text_inset : 0;
    const int width = widget.w - inset * 2, height = widget.h - inset * 2;
    if(width <= 0 || height <= 0) {
      result.overflows = true;
      return result;
    }
    const int line_height = font.char_h + font.spacing_y;
    std::size_t lines = 0;
    detail::wrap_text_lines(widget.text, width, font, true, [&](std::string_view line) {
      if(lines++) result.text.push_back('\n');
      result.text.append(line);
      if(line.size() == 1 && font.char_w_variable[detail::glyph_index(line.front())] > width)
        result.overflows = true;
    });
    const auto block_height = static_cast<int64_t>(lines) * line_height;
    const auto top = widget.kind == Kind::Button ? (height - block_height) / 2 : 0;
    result.overflows |= top < 0 || top + block_height - font.spacing_y > height;
    return result;
  }
}
