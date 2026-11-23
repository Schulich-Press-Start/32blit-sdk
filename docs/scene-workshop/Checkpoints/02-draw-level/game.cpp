#include "32blit.hpp"
#include "game_scene.hpp"

using namespace blit;

sdk::GameScene game;

// Open the starting scene in a 320 by 240 window.
void init() {
  set_screen_mode(ScreenMode::hires, Size(320, 240));
  game.open();
}

// Leave the actors in place; drawing handles their animation.
void update(uint32_t) {}

// Clear the background, then draw the scene and its current animation frames.
void render(uint32_t time) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
  game.draw(time);
}
