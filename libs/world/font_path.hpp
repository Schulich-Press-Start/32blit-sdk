#pragma once

#include <string_view>

namespace sdk {
  /// Authored font paths are project-relative, use '/', and name a .font file.
  constexpr bool is_project_font_path(std::string_view path) {
    constexpr std::string_view extension = ".font";
    if(path.size() <= extension.size() || path.find_first_of("\\:<>\"|?*") != path.npos) return false;
    for(const unsigned char c : path) if(c < ' ' || c == 127) return false;
    for(std::size_t i = 0; i < extension.size(); ++i) {
      const char c = path[path.size() - extension.size() + i];
      if((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != extension[i]) return false;
    }
    for(auto slash = path.find('/'); slash != path.npos; slash = path.find('/')) {
      const auto part = path.substr(0, slash);
      if(part.empty() || part.back() == '.' || part.back() == ' ') return false;
      path.remove_prefix(slash + 1);
    }
    return path.size() > extension.size();
  }
}
