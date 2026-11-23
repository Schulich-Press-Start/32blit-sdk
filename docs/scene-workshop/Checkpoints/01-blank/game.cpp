#include "32blit.hpp"

using namespace blit;

// Set up the 320 by 240 game window.
void init() {
  set_screen_mode(ScreenMode::hires, Size(320, 240));
}

// Leave the game state unchanged while the window is blank.
void update(uint32_t) {}

// Clear the screen to navy for the first checkpoint.
void render(uint32_t) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
}
