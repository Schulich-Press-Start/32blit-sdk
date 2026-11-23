#include "32blit.hpp"
#include "game_scene.hpp"

sdk::GameScene game;

// Open the starting scene in a 320 by 240 window.
void init() {
  blit::set_screen_mode(blit::ScreenMode::hires, blit::Size(320, 240));
  game.open();
}

// Run game rules each tick; the starter scene has none yet.
void update(uint32_t) {
}

// Clear the background, then draw the scene and its current animation frames.
void render(uint32_t time) {
  blit::screen.pen = blit::Pen(16, 25, 45);
  blit::screen.clear();
  game.draw(time);
}
