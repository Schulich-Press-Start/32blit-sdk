#pragma once

#include "scene.hpp"

#include <charconv>
#include <cstring>

namespace sdk::detail {
  inline bool scene_copy(char *buf, size_t cap, std::string_view text) {
    if(text.size() >= cap) return false;
    if(!text.empty()) std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    return true;
  }

  inline bool scene_error(SceneError &out, std::string_view file, uint32_t line, std::string_view message) {
    scene_copy(out.file, sizeof(out.file), file.substr(0, sizeof(out.file) - 1));
    out.line = line;
    scene_copy(out.message, sizeof(out.message), message.substr(0, sizeof(out.message) - 1));
    return false;
  }

  inline bool scene_bare_character(unsigned char c) {
    return c > ' ' && c < 127 && c != '"' && c != '\\' && c != '=' && c != '#' && c != ':';
  }

  inline bool scene_bare_token(std::string_view text) {
    if(text.empty()) return false;
    for(unsigned char c : text)
      if(!scene_bare_character(c)) return false;
    return true;
  }

  class SceneOutput {
  public:
    SceneOutput(char *buf, int cap) : buf_(buf), remaining_(buf && cap > 0 ? cap : 0) {
      if(remaining_) buf_[0] = '\0';
    }

    void append(std::string_view text) {
      if(!remaining_) return;
      if(text.size() >= remaining_) { remaining_ = 0; return; }
      if(!text.empty()) std::memcpy(buf_ + size_, text.data(), text.size());
      size_ += text.size();
      remaining_ -= text.size();
    }

    /// Bare safe token where possible, otherwise quotes with reversible byte escapes.
    void token(std::string_view text) {
      if(scene_bare_token(text)) { append(text); return; }
      quoted(text);
    }

    void quoted(std::string_view text) {
      append("\"");
      for(unsigned char c : text) {
        if(c == '"') append("\\\"");
        else if(c == '\\') append("\\\\");
        else if(c < ' ' || c >= 127) {
          const char octal[] = {'\\', char('0' + (c >> 6)), char('0' + ((c >> 3) & 7)), char('0' + (c & 7))};
          append({octal, sizeof(octal)});
        } else {
          const char ch = static_cast<char>(c);
          append({&ch, 1});
        }
      }
      append("\"");
    }

    void number(uint32_t value) {
      char buf[10];
      const auto end = std::to_chars(buf, buf + sizeof(buf), value).ptr;
      append({buf, static_cast<size_t>(end - buf)});
    }

    int finish() {
      if(!remaining_) return fail();
      buf_[size_] = '\0';
      return static_cast<int>(size_);
    }

    int fail() {
      if(size_) buf_[0] = '\0';
      return -1;
    }

  private:
    char *buf_;
    size_t remaining_, size_ = 0;
  };
}
