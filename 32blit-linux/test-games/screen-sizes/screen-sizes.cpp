/**
 * \file screen-sizes.cpp
 * \brief Test game for screen sizes: where each size lands on the panel, and how sharply it is scaled.
 *
 * Steps through a list of sizes with A (next) and B (previous), asking for
 * each with the `bool` overload of set_screen_mode(). Draws:
 * - an 8-pixel checkerboard: every square must stay square and equally
 *   sized, with hard edges (soft edges mean the scaler smooths pixels);
 * - a 1-pixel white border on the screen's edge: all four sides must be
 *   visible, so nothing is cropped;
 * - a 1-pixel diagonal from corner to corner;
 * - the size asked for, whether it was accepted, and the size in use.
 *
 * A rejected size keeps the previous screen (and says so). On the dev
 * board's 480x272 panel, 640x480 and 3840x2160 are rejected; on a 640x480
 * VM screen only 3840x2160 is.
 */
#include <cstdio>

#include "32blit.hpp"

using namespace blit;

namespace {
  const Size sizes[] = {
    Size(160, 120),   // lores default
    Size(320, 240),   // hires default
    Size(480, 270),   // 16:9
    Size(640, 480),   // larger than the dev board panel
    Size(96, 96),     // smallest allowed
    Size(3840, 2160), // too big for any panel we have
  };
  constexpr int size_count = int(sizeof(sizes) / sizeof(sizes[0]));

  int current = 0;
  bool accepted = false;

  void ask_for(int index) {
    current = index;
    accepted = set_screen_mode(ScreenMode::hires, PixelFormat::RGB, sizes[index]);
  }
}

void init() {
  ask_for(0);
}

void update(uint32_t time) {
  if(buttons.pressed & Button::A)
    ask_for((current + 1) % size_count);
  else if(buttons.pressed & Button::B)
    ask_for((current + size_count - 1) % size_count);
}

void render(uint32_t time) {
  const Size bounds = screen.bounds;

  screen.pen = Pen(0, 0, 0);
  screen.clear();

  // checkerboard
  screen.pen = Pen(48, 48, 96);
  for(int y = 0; y < bounds.h; y += 8) {
    for(int x = (y / 8) % 2 * 8; x < bounds.w; x += 16)
      screen.rectangle(Rect(x, y, 8, 8));
  }

  // diagonal and 1-pixel border
  screen.pen = Pen(255, 64, 64);
  screen.line(Point(0, 0), Point(bounds.w - 1, bounds.h - 1));

  screen.pen = Pen(255, 255, 255);
  screen.rectangle(Rect(0, 0, bounds.w, 1));
  screen.rectangle(Rect(0, bounds.h - 1, bounds.w, 1));
  screen.rectangle(Rect(0, 0, 1, bounds.h));
  screen.rectangle(Rect(bounds.w - 1, 0, 1, bounds.h));

  // short lines, so they fit the 96x96 screen
  char line[32];
  snprintf(line, sizeof(line), "%dx%d", sizes[current].w, sizes[current].h);
  screen.text(line, minimal_font, Point(4, 4));
  screen.text(accepted ? "accepted" : "rejected", minimal_font, Point(4, 14));
  snprintf(line, sizeof(line), "now %dx%d", bounds.w, bounds.h);
  screen.text(line, minimal_font, Point(4, 24));
  screen.text("A next B back", minimal_font, Point(4, 34));
}
