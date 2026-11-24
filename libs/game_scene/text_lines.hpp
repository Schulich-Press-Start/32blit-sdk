#pragma once

#include "graphics/font.hpp"
#include <string_view>

namespace sdk::detail {
  inline int glyph_index(unsigned char c) {
    const int code = c & 0x7f;
    return code < ' ' ? 0 : code - ' ';
  }

  inline bool wrap_space(char c) { return c == ' ' || c == '\t'; }

  // Emits slices of the original text, including empty authored lines. Reject a
  // too-wide glyph unless the caller can safely clip it within a widget.
  template<class Emit>
  bool wrap_text_lines(std::string_view text, int width, const blit::Font &font, bool clip_glyph, Emit emit) {
    for(;;) {
      const auto newline = text.find_first_of("\r\n");
      auto line = text.substr(0, newline);
      do {
        int remaining = width;
        std::size_t end = 0, word_break = std::string_view::npos;
        for(; end < line.size(); ++end) {
          if(end && wrap_space(line[end]) && !wrap_space(line[end - 1])) word_break = end;
          const int advance = font.char_w_variable[glyph_index(line[end])];
          if(advance > remaining) break;
          remaining -= advance;
        }
        if(end == line.size()) {
          emit(line);
          break;
        }
        if(!end && !clip_glyph) return false;
        if(word_break != std::string_view::npos) {
          emit(line.substr(0, word_break));
          line.remove_prefix(word_break);
          while(!line.empty() && wrap_space(line.front())) line.remove_prefix(1);
        } else {
          if(!end) end = 1;
          emit(line.substr(0, end));
          line.remove_prefix(end);
        }
      } while(!line.empty());
      if(newline == std::string_view::npos) return true;
      const bool crlf = text[newline] == '\r' && newline + 1 < text.size() && text[newline + 1] == '\n';
      text.remove_prefix(newline + (crlf ? 2 : 1));
    }
  }
}
