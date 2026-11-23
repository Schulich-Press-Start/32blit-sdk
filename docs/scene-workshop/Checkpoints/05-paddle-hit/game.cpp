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

struct PaddleSettings {
  const char *name;
  float speed;
  bool computer;
};

constexpr PaddleSettings paddles[] = {
  {"player", 170.0f, false},
  {"opponent", 92.0f, true}
};

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

// Bounce the ball only if its leading edge crosses the face of a paddle.
bool return_hit(Transform2D &ball, const sdk::Box &before, const sdk::Box &next, const sdk::Box &paddle) {
  if (next.top >= paddle.bottom || next.bottom <= paddle.top) {
    return false;
  }

  // Did the ball's leading edge cross the paddle's face during this tick?
  if (vx > 0.0f && before.right <= paddle.left && next.right >= paddle.left) {
    ball.x += paddle.left - next.right;
  } else if (vx < 0.0f && before.left >= paddle.right && next.left <= paddle.right) {
    ball.x += paddle.right - next.left;
  } else {
    return false;
  }

  vx = -vx;
  // A centre hit flies straight; hitting near an end gives a steeper return.
  const float half_height = (paddle.bottom - paddle.top) / 2.0f;
  const float offset = (sdk::center_y(next) - sdk::center_y(paddle)) / half_height;
  vy = std::clamp(offset, -1.0f, 1.0f) * 150.0f;
  return true;
}

// Move the player and check both paddles for a return.
void update_paddles(Transform2D &ball, const sdk::Box &before, const sdk::Box &next) {
  const int direction = player_direction();
  bool hit = false;

  for (const PaddleSettings &paddle : paddles) {
    Transform2D *position = game.position(paddle.name);
    const BoxCollider2D *box = game.collider(paddle.name);
    if (!position || !box || !fits_court(*box)) {
      continue;
    }

    if (!paddle.computer) {
      move_paddle(*position, *box, direction * paddle.speed * tick_seconds);
    }

    // The first return wins; the other paddles still get to move.
    if (!waiting && !hit) {
      hit = return_hit(ball, before, next, sdk::bounds(*position, *box));
    }
  }
}

// Load the court and wait for a serve before the ball moves.
void init() {
  set_screen_mode(ScreenMode::hires, Size(320, 240));
  game.open(restart_rally);
}

// Track the ball before and after each step so paddles can return it.
void update(uint32_t) {
  Transform2D *ball = game.position("ball");
  const BoxCollider2D *box = game.collider("ball");
  if (!ball || !box || !fits_court(*box)) {
    return;
  }

  serve();
  const sdk::Box before = sdk::bounds(*ball, *box);
  sdk::Box next = before;
  if (!waiting) {
    advance_ball(*ball, *box);
    next = sdk::bounds(*ball, *box);
  }
  update_paddles(*ball, before, next);
}

// Clear the background, then draw the scene and its current animation frames.
void render(uint32_t time) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
  game.draw(time);
}
