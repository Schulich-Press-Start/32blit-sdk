/**
 * \file screen_fit.hpp
 * \brief Which game screen sizes the port accepts, and where they go on the panel.
 *
 * Games pick their own screen size (the `bounds` argument of
 * `blit::set_screen_mode()`), so the size also sets the aspect ratio. The
 * port shows the screen at the largest whole-number scale that fits the
 * panel, centred, with black borders. Whole numbers keep pixel art sharp:
 * every game pixel becomes the same number of panel pixels.
 *
 * Examples on a 480x272 panel: 160x120 shows at 2x, 320x240 and 480x270 at
 * 1x, 96x96 at 2x. On a 640x480 panel, 320x240 shows at 2x and fills it.
 *
 * Pure maths with no device access, so it is unit-tested (tests/screen_fit_test.cpp).
 */
#pragma once

#include <algorithm>

#include "types/size.hpp"

namespace blit_linux {

  /// Smallest width or height a game screen may have (the same limit as the Pico DPI port).
  constexpr int min_screen_size = 96;

  /** \brief Scale and position of a game screen on the panel. */
  struct ScreenFit {
    int scale; ///< Panel pixels per game pixel, in each direction (1 or more).
    int x;     ///< Panel column of the game screen's left edge.
    int y;     ///< Panel row of the game screen's top edge.
  };

  /**
   * \brief Whether a game may use a screen of this size on this panel.
   * \return true if both sides are at least min_screen_size and no larger than the panel.
   */
  inline bool screen_size_allowed(blit::Size panel, blit::Size screen) {
    return screen.w >= min_screen_size && screen.h >= min_screen_size
        && screen.w <= panel.w && screen.h <= panel.h;
  }

  /**
   * \brief Scale `screen` by the largest whole number that fits `panel`, and centre it.
   *
   * Only meaningful for sizes screen_size_allowed() accepts. When the leftover
   * space is odd, the extra pixel goes to the right or bottom border.
   */
  inline ScreenFit fit_screen(blit::Size panel, blit::Size screen) {
    const int scale = std::max(1, std::min(panel.w / screen.w, panel.h / screen.h));

    return {scale, (panel.w - screen.w * scale) / 2, (panel.h - screen.h * scale) / 2};
  }
}
