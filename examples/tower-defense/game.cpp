#include "game_scene.hpp"
#include "grid_route.hpp"

#include <array>
#include <cstdio>

using namespace blit;

namespace {
  constexpr int columns = 20;
  constexpr int rows = 15;
  constexpr int tile = 16;
  constexpr int width = columns * tile;
  constexpr int height = rows * tile;
  constexpr int cell_count = columns * rows;
  constexpr Point start_cursor(3, 4);

  constexpr int ticks_per_second = 100; // 32blit calls update every 10 ms.
  constexpr int setup_ticks = 5 * ticks_per_second;
  constexpr int spawn_gap = 90;
  constexpr int shot_gap = 40;
  constexpr int tower_range = 24;
  constexpr int wave_size = 6;
  constexpr int enemy_health = 3;
  constexpr int start_lives = 3;

  enum class Cell { ground, path, buildable };
  enum class Round { invalid, planning, running, won, lost };

  struct Tower {
    Point position;
    int cooldown = 0;
  };

  struct Enemy {
    Point position;
    int distance = 0;
    int health = 0;
  };

  sdk::GameScene game;
  std::array<Cell, cell_count> cells{};
  std::array<Point, cell_count> route{};
  int route_size = 0;
  sdk::RouteIssue route_issue;

  std::array<Tower, cell_count> towers{};
  std::array<Enemy, wave_size> enemies{};
  Round round_state = Round::invalid;
  Point cursor = start_cursor;
  const char *placement_error = nullptr;
  int tower_count = 0;
  int lives = start_lives;
  int spawned = 0;
  int defeated = 0;
  int spawn_in = 0;

  // Keep cursor moves inside the authored map.
  bool in_map(Point cell) {
    return cell.x >= 0 && cell.x < columns && cell.y >= 0 && cell.y < rows;
  }

  // Store the cell grid in row-major order.
  int cell_index(Point cell) {
    return cell.y * columns + cell.x;
  }

  // Translate a grid cell into its center in world pixels.
  Point cell_center(Point cell) {
    return Point(cell.x * tile + tile / 2, cell.y * tile + tile / 2);
  }

  // Classify tiles for placement, then trace the authored lane for the wave.
  sdk::RouteIssue build_route() {
    route_size = 0;
    if (game.map_size() != Size(width, height) || game.tile_size() != tile)
      return {"Use a 20 x 15 map with 16 px tiles."};

    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < columns; ++x) {
        const Point cell(x, y);
        // GameScene checks all layers; a path tile always takes priority over a pad.
        cells[cell_index(cell)] = game.tile_has_property(cell, "path") ? Cell::path :
          game.tile_has_property(cell, "buildable") ? Cell::buildable : Cell::ground;
      }
    }

    sdk::RouteCell spawn, goal;
    const auto *spawn_marker = game.position("spawn");
    if (!spawn_marker) return {"Spawn needs a Transform2D."};
    if (!sdk::centered_route_cell(columns, rows, tile, spawn_marker->x, spawn_marker->y, spawn))
      return {spawn.x < 0 ? "Spawn is outside the map." : "Center spawn on a tile.", spawn};
    const auto *goal_marker = game.position("goal");
    if (!goal_marker) return {"Goal needs a Transform2D."};
    if (!sdk::centered_route_cell(columns, rows, tile, goal_marker->x, goal_marker->y, goal))
      return {goal.x < 0 ? "Goal is outside the map." : "Center goal on a tile.", goal};

    const auto issue = sdk::trace_route(columns, rows, spawn, goal,
      [](sdk::RouteCell cell) { return cells[cell_index(Point(cell.x, cell.y))] == Cell::path; },
      [](sdk::RouteCell cell) { route[route_size++] = Point(cell.x, cell.y); });
    if (issue.message) route_size = 0;
    return issue;
  }

  // Reset game-owned state without reloading the authored scene.
  void restart_round() {
    round_state = Round::planning;
    cursor = start_cursor;
    placement_error = nullptr;
    tower_count = 0;
    lives = start_lives;
    spawned = 0;
    defeated = 0;
    spawn_in = setup_ticks;
  }

  // A tap moves one cell and dismisses the last placement message.
  void move_cursor() {
    Point next = cursor;
    if (sdk::button_pressed(Button::DPAD_LEFT)) --next.x;
    else if (sdk::button_pressed(Button::DPAD_RIGHT)) ++next.x;
    else if (sdk::button_pressed(Button::DPAD_UP)) --next.y;
    else if (sdk::button_pressed(Button::DPAD_DOWN)) ++next.y;
    if (!in_map(next) || next == cursor) return;
    cursor = next;
    placement_error = nullptr;
  }

  // Report why the current tile cannot accept a tower, or nullptr if it can.
  const char *placement_problem() {
    const Cell kind = cells[cell_index(cursor)];
    if (kind == Cell::path) return "Cannot build on the path.";
    if (kind != Cell::buildable) return "This tile is not buildable.";
    const Point center = cell_center(cursor);
    for (int i = 0; i < tower_count; ++i) {
      if (towers[i].position == center) return "This tile already has a tower.";
    }
    return nullptr;
  }

  // Build one tower per press; the first valid build starts the wave countdown.
  void place_tower() {
    placement_error = placement_problem();
    if (placement_error) return;
    towers[tower_count++] = {cell_center(cursor), 0};
    if (round_state == Round::planning) round_state = Round::running;
  }

  // Move along each segment center-to-center at one pixel per update.
  Point enemy_position(int distance) {
    const int segment = distance / tile;
    const int offset = distance % tile;
    const Point direction = route[segment + 1] - route[segment];
    return cell_center(route[segment]) + Point(direction.x * offset, direction.y * offset);
  }

  // Hit the first living enemy in range, then wait before firing again.
  void fire_towers() {
    for (int i = 0; i < tower_count; ++i) {
      Tower &tower = towers[i];
      if (tower.cooldown > 0) --tower.cooldown;
      if (tower.cooldown > 0) continue;
      for (int j = 0; j < spawned; ++j) {
        Enemy &enemy = enemies[j];
        if (enemy.health == 0) continue;
        const Point offset = enemy.position - tower.position;
        if (offset.x * offset.x + offset.y * offset.y > tower_range * tower_range) continue;
        --enemy.health;
        if (enemy.health == 0) ++defeated;
        tower.cooldown = shot_gap;
        break;
      }
    }
  }

  // Advance one tick, losing a life on arrival or winning when the wave ends.
  void update_wave() {
    if (spawned < wave_size && --spawn_in == 0) {
      enemies[spawned++] = {cell_center(route[0]), 0, enemy_health};
      spawn_in = spawn_gap;
    }

    const int goal_distance = (route_size - 1) * tile;
    for (int i = 0; i < spawned; ++i) {
      Enemy &enemy = enemies[i];
      if (enemy.health == 0) continue;
      ++enemy.distance;
      if (enemy.distance == goal_distance) {
        enemy.health = 0;
        --lives;
        if (lives == 0) {
          round_state = Round::lost;
          return;
        }
      } else {
        enemy.position = enemy_position(enemy.distance);
      }
    }

    fire_towers();
    if (defeated + start_lives - lives == wave_size) round_state = Round::won;
  }

  // Draw towers, living enemies and their remaining health over the scene.
  void draw_units() {
    for (int i = 0; i < tower_count; ++i) {
      const Point center = towers[i].position;
      screen.pen = Pen(22, 40, 48);
      screen.circle(center, 6);
      screen.pen = Pen(107, 226, 241);
      screen.circle(center, 4);
    }
    for (int i = 0; i < spawned; ++i) {
      const Enemy &enemy = enemies[i];
      if (enemy.health == 0) continue;
      const Point center = enemy.position;
      screen.pen = Pen(117, 42, 38);
      screen.circle(center, 6);
      screen.pen = Pen(243, 99, 85);
      screen.circle(center, 4);
      screen.pen = Pen(255, 228, 163);
      screen.rectangle(Rect(center.x - 4, center.y - 8, enemy.health * 3, 2));
    }
  }

  // Outline the selected cell without covering the terrain or tower in it.
  void draw_cursor() {
    const Point top_left(cursor.x * tile, cursor.y * tile);
    const Point top_right = top_left + Point(tile - 1, 0);
    const Point bottom_left = top_left + Point(0, tile - 1);
    const Point bottom_right = top_left + Point(tile - 1, tile - 1);
    screen.pen = Pen(222, 250, 255);
    if (placement_problem()) screen.pen = Pen(255, 188, 90);
    screen.line(top_left, top_right);
    screen.line(top_right, bottom_right);
    screen.line(bottom_right, bottom_left);
    screen.line(bottom_left, top_left);
  }

  // Keep controls, round status and diagnostics in the top two tile rows.
  void draw_hud() {
    screen.pen = Pen(22, 32, 40, 210);
    screen.rectangle(Rect(0, 0, width, 2 * tile));
    screen.pen = Pen(226, 244, 248);
    screen.text("TOWER DEFENSE | ARROWS MOVE   Z BUILD   X RESTART", minimal_font, Point(5, 2));
    if (route_issue.message) {
      screen.pen = Pen(255, 188, 90);
      const auto cell = route_issue.cell;
      char title[48] = "Route error:";
      if (in_map(Point(cell.x, cell.y)))
        std::snprintf(title, sizeof(title), "Route error at cell (%d, %d):", cell.x, cell.y);
      screen.text(title, minimal_font, Point(5, 12));
      screen.text(route_issue.message, minimal_font, Point(5, 22));
      return;
    }

    char wave_status[64];
    const char *status = "Build a tower to start the wave.";
    if (round_state == Round::won) status = "Wave repelled! Press X to restart.";
    else if (round_state == Round::lost) status = "Goal lost! Press X to restart.";
    else if (round_state == Round::running) {
      if (spawned == 0) {
        const int seconds = (spawn_in + ticks_per_second - 1) / ticks_per_second;
        std::snprintf(wave_status, sizeof(wave_status), "Wave starts in %d s   Lives: %d", seconds, lives);
      } else {
        std::snprintf(wave_status, sizeof(wave_status), "Lives: %d   Enemies stopped: %d / %d", lives, defeated, wave_size);
      }
      status = wave_status;
    }
    screen.text(status, minimal_font, Point(5, 12));
    if (placement_error) {
      screen.pen = Pen(255, 188, 90);
      screen.text(placement_error, minimal_font, Point(5, 22));
    }
  }
}

// Load the startup scene and validate its flags and markers once.
void init() {
  set_screen_mode(ScreenMode::hires, Size(width, height));
  if (!game.open()) return;
  route_issue = build_route();
  if (!route_issue.message) restart_round();
}

// Restart on X, or handle the cursor and advance the active wave.
void update(uint32_t) {
  if (round_state == Round::invalid) return;
  if (sdk::button_pressed(Button::B)) {
    restart_round();
    return;
  }
  if (round_state == Round::won || round_state == Round::lost) return;
  move_cursor();
  if (sdk::button_pressed(Button::A)) place_tower();
  if (round_state == Round::running) update_wave();
}

// Draw the authored scene first; GameScene shows its own load errors.
void render(uint32_t time) {
  screen.pen = Pen(18, 26, 31);
  screen.clear();
  if (!game.draw(time)) return;
  if (!route_issue.message) draw_units();
  draw_hud();
  if (!route_issue.message) draw_cursor();
}
