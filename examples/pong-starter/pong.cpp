#include "32blit.hpp"

using namespace blit;

void init() { set_screen_mode(ScreenMode::hires, Size(320, 240)); }
void update(uint32_t) {}
void render(uint32_t) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
}
