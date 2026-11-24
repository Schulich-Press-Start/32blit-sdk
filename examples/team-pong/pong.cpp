#include "32blit.hpp"
#include "game_scene.hpp"
#include "paddle.hpp"

#include <algorithm>
#include <cstdio>

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
Transform2D serve_position;
int serve_direction = -1;
unsigned int player_score = 0;
unsigned int cpu_score = 0;

// Clear both scores and wait for the first serve of a new match.
void reset_match() {
  player_score = 0;
  cpu_score = 0;
  serve_direction = -1;
  waiting = true;
}

// Remember the ball's starting position and reset the match on scene load.
void start_match() {
  const Transform2D *ball = game.position("ball");
  if (ball) {
    serve_position = *ball;
  }
  reset_match();
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

// Launch the next serve using the last goal and total points to choose its direction.
void serve() {
  if (waiting && sdk::button_pressed(Button::A)) {
    vx = serve_direction * ball_speed;
    const unsigned int points_played = player_score + cpu_score;
    if (points_played % 2 == 0) {
      vy = 74.0f;
    } else {
      vy = -74.0f;
    }
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

// Chase the ball without moving farther than the CPU can travel in one tick.
void move_cpu(Transform2D &paddle, const BoxCollider2D &box, const sdk::Box &ball, float step) {
  const sdk::Box paddle_bounds = sdk::bounds(paddle, box);
  const float gap = sdk::center_y(ball) - sdk::center_y(paddle_bounds);
  // Stop at the ball's height if it is closer than one step.
  move_paddle(paddle, box, std::clamp(gap, -step, step));
}

// Use each paddle's settings to move it and check for a return.
void update_paddles(Transform2D &ball, const sdk::Box &before, const sdk::Box &next) {
  const int direction = player_direction();
  bool hit = false;

  // Read the current components each tick: role, speed and even the actors can change live.
  for (uint16_t slot = 0; slot < game.world.capacity(); ++slot) {
    const sdk::Entity actor = game.world.entity_at(slot);
    const PaddleDrive *drive = game.world.get<PaddleDrive>(actor);
    if (!drive) {
      continue;
    }
    Transform2D *position = game.world.get<Transform2D>(actor);
    const BoxCollider2D *box = game.world.get<BoxCollider2D>(actor);
    if (!position || !box || !fits_court(*box)) {
      continue;
    }

    const float step = std::max(0.0f, drive->speed) * tick_seconds;
    if (drive->computer) {
      move_cpu(*position, *box, next, step);
    } else {
      move_paddle(*position, *box, direction * step);
    }

    // The first return wins; the other paddles still get to move.
    if (!waiting && !hit) {
      hit = return_hit(ball, before, next, sdk::bounds(*position, *box));
    }
  }
}

// Award a point when the whole ball leaves the court, then set up the next serve.
void check_goal(Transform2D &ball, const sdk::Box &box) {
  // A point counts only when the whole ball has left the court.
  if (box.right < 0.0f) {
    cpu_score += 1;
    serve_direction = -1;
  } else if (box.left > 320.0f) {
    player_score += 1;
    serve_direction = 1;
  } else {
    return;
  }
  ball = serve_position;
  waiting = true;
}

// Set up paddle controls and load the court for a new match.
void init() {
  set_screen_mode(ScreenMode::hires, Size(320, 240));
  static sdk::ComponentPool<PaddleDrive> drives(game.world, game.world.capacity());
  game.open(start_match);
}

// Run one match tick, including input, paddle hits, goals and reset.
void update(uint32_t) {
  Transform2D *ball = game.position("ball");
  const BoxCollider2D *box = game.collider("ball");
  if (!ball || !box || !fits_court(*box)) {
    return;
  }

  if (sdk::button_pressed(Button::B)) {
    *ball = serve_position;
    reset_match();
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

  if (!waiting) {
    // A paddle hit may have moved the ball back onto the court.
    check_goal(*ball, sdk::bounds(*ball, *box));
  }
}

// Draw the court and actors, then add the score and serve prompt.
void render(uint32_t time) {
  screen.pen = Pen(16, 25, 45);
  screen.clear();
  if (!game.draw(time)) {
    return; // Leave the SDK's scene error visible instead of drawing a scoreboard on it.
  }

  char hud[64];
  std::snprintf(hud, sizeof(hud), "YOU %u   TEAM PONG   %u CPU", player_score, cpu_score);
  screen.pen = Pen(208, 248, 248);
  screen.text(hud, fat_font, Point(160, 3), true, TextAlign::top_center);
  screen.text("UP / DOWN: MOVE   Z: SERVE   X: RESET", minimal_font,
              Point(160, 229), true, TextAlign::top_center);
  if (waiting) {
    screen.text("PRESS Z TO SERVE", fat_font, Point(160, 95), true, TextAlign::top_center);
  }
}
