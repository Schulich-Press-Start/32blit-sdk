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
constexpr float ball_speed = 190.0f;

float vx = 0.0f;
float vy = 0.0f;
bool waiting = true;

// Wait for a new serve after loading the court.
void restart_rally() {
  waiting = true;
}

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

// Start a waiting rally with a practice shot toward the lower left.
void serve() {
  if (waiting && sdk::button_pressed(Button::A)) {
    vx = -ball_speed;
    vy = 140.0f; // This practice serve reaches the bottom wall before the left goal.
    waiting = false;
  }
}

// Move the ball one tick and bounce it off the top or bottom rail.
void advance_ball(Transform2D &ball, const BoxCollider2D &box) {
  ball.x += vx * tick_seconds;
  ball.y += vy * tick_seconds;

  const float top = court_top - box.offset_y;
  const float bottom = court_bottom - box.offset_y - box.height;
  if (ball.y < top && vy < 0.0f) {
    ball.y = top;
    vy = -vy;
  } else if (ball.y > bottom && vy > 0.0f) {
    ball.y = bottom;
    vy = -vy;
  }
}

// Load the court and wait for a serve before the ball moves.
void init() {
  set_screen_mode(ScreenMode::hires, Size(320, 240));
  game.open(restart_rally);
}

// Move the paddle, then serve or advance the ball if it is in play.
void update(uint32_t) {
  move_player();

  Transform2D *ball = game.position("ball");
  const BoxCollider2D *box = game.collider("ball");
  if (!ball || !box || !fits_court(*box)) {
    return;
  }

  serve();
  if (!waiting) {
    advance_ball(*ball, *box);
  }
}

// Clear the background, then draw the scene and its current animation frames.
void render(uint32_t time) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
  game.draw(time);
}
