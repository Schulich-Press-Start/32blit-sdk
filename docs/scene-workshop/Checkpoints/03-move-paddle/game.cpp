#include "32blit.hpp"
#include "game_scene.hpp"

#include <algorithm>

using namespace blit;
using sdk::BoxCollider2D;
using sdk::Transform2D;

sdk::GameScene game;

constexpr float court_top = 24.0f;
constexpr float court_bottom = 216.0f;
constexpr float tick_seconds = 0.010f; // update() runs every 10 ms.

// Return -1 for up, +1 for down, or 0 when the inputs cancel.
int player_direction() {
  int direction = 0;
  if (sdk::button_down(Button::DPAD_UP)) {
    direction -= 1;
  }
  if (sdk::button_down(Button::DPAD_DOWN)) {
    direction += 1;
  }
  return direction;
}

// Check that the collision box has a usable size and fits between the rails.
bool fits_court(const BoxCollider2D &box) {
  return box.width > 0 && box.height > 0 && box.height <= court_bottom - court_top;
}

// Move a paddle vertically without letting its collision box cross a rail.
void move_paddle(Transform2D &paddle, const BoxCollider2D &box, float distance) {
  // Stop at the edge of the collider, even when the sprite has a different size.
  const float top = court_top - box.offset_y;
  const float bottom = court_bottom - box.offset_y - box.height;
  paddle.y = std::clamp(paddle.y + distance, top, bottom);
}

// Move the player paddle one step in the direction of the held keys.
void move_player() {
  Transform2D *player = game.position("player");
  const BoxCollider2D *box = game.collider("player");
  if (player && box && fits_court(*box)) {
    move_paddle(*player, *box, player_direction() * 170.0f * tick_seconds);
  }
}

// Open the starting scene in a 320 by 240 window.
void init() {
  set_screen_mode(ScreenMode::hires, Size(320, 240));
  game.open();
}

// Read player input and move the paddle once per tick.
void update(uint32_t) {
  move_player();
}

// Clear the background, then draw the scene and its current animation frames.
void render(uint32_t time) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
  game.draw(time);
}
