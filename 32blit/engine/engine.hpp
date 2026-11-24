#pragma once

#include <cstdint>
#include <string>

#include "../graphics/surface.hpp"

namespace blit {

  enum   ScreenMode  { lores, hires, hires_palette };
  extern Surface      screen;


  extern void     (*init)             ();
  extern void     (*update)           (uint32_t time);
  extern void     (*render)           (uint32_t time);

  void set_screen_mode(ScreenMode new_mode, Size bounds = {0, 0});
  bool set_screen_mode(ScreenMode new_mode, PixelFormat format, Size bounds = {0, 0});
  void set_screen_palette(const Pen *colours, int num_cols);

  uint32_t now();
  uint32_t now_us();
  uint32_t us_diff(uint32_t from, uint32_t to);

  /// Reads signed Unix seconds (UTC). Returns false and leaves the output unchanged if unavailable.
  /// This clock can change independently of game time; use now() for elapsed time.
  [[nodiscard]] bool get_wall_clock(int64_t &unix_seconds);

  uint32_t random();

  void debug(std::string message);
  int debugf(const char * psFormatString, ...);

  int tick(uint32_t time);

  const char *get_launch_path();
}
