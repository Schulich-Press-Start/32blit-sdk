#pragma once

#ifndef __EMSCRIPTEN__
#include <filesystem>
#endif

namespace editor {
  class Appearance {
  public:
    Appearance();
    void apply_scale(float scale) const;
    void draw_menu();

  private:
    void apply_colors() const;
    bool dark_ = false;
#ifndef __EMSCRIPTEN__
    std::filesystem::path theme_file_;
#endif
  };
}
