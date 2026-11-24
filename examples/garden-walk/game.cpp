#include "game_scene.hpp"
#include "door.hpp"
#include "overlap_tracker.hpp"
#include "grid_navigation.hpp"
#include "grid_step.hpp"
#include "ui_screen.hpp"
#include "audio_cues.hpp"
#include "progress_store.hpp"
#include "dialogue_box.hpp"
#include "vfx.hpp"

#include <cstdio>
#include <cmath>
#include <filesystem>
#ifdef GARDEN_ACTION_BINDINGS
#include "input_actions.hpp"
#endif

using namespace blit;

namespace {
  sdk::GameScene game;
  sdk::OverlapTracker contacts(game.world);
  sdk::GridNavigation navigation;
  sdk::UiScreen pause_menu;
  sdk::AudioCues menu_audio;
  sdk::DialogueBox guide_dialogue;
  sdk::Vfx fountain_pop;
  bool evening_coat = false;
  char clock_page[64] = "This device has no wall clock.";
  constexpr char day_sprite[] = "assets/gardener";
  constexpr char evening_sprite[] = "assets/sprite/gardener_evening";

  void apply_coat() {
    if (auto *sprite = game.world.get<sdk::Sprite>(game.world.find("player")))
      std::snprintf(sprite->asset, sizeof(sprite->asset), "%s", evening_coat ? evening_sprite : day_sprite);
  }
  bool audio_ready = false;
  const char *progress_notice = nullptr;
  constexpr char progress_file[] = "garden-walk-progress.txt";
  struct Checkpoint { const char *id, *scene, *arrival; };
  constexpr Checkpoint checkpoints[]{
    {"garden", "scenes/main.scene", "from_shed"},
    {"shed", "scenes/shed.scene", "from_garden"},
    {"meadow", "scenes/connected/meadow.scene", "from_main"},
  };

  bool restore_checkpoint() {
    std::string payload, error;
    switch (sdk::read_progress(progress_file, 64, payload, error)) {
      case sdk::ProgressReadResult::missing:
        progress_notice = "NO CHECKPOINT SAVED";
        return false;
      case sdk::ProgressReadResult::error:
        std::fprintf(stderr, "Garden Walk progress: %s\n", error.c_str());
        progress_notice = "COULD NOT READ CHECKPOINT";
        return false;
      case sdk::ProgressReadResult::loaded: break;
    }
    constexpr std::string_view version = "garden-walk 1\n";
    for (const auto &checkpoint : checkpoints) {
      if (payload.compare(0, version.size(), version) != 0 ||
          std::string_view(payload).substr(version.size()) != checkpoint.id) continue;
      if (game.travel(checkpoint.scene, "player", checkpoint.arrival)) {
        progress_notice = "CHECKPOINT RESTORED";
        return true;
      }
      progress_notice = "CHECKPOINT DESTINATION UNAVAILABLE";
      return false;
    }
    progress_notice = "INVALID CHECKPOINT";
    return false;
  }

  void save_checkpoint() {
    for (const auto &checkpoint : checkpoints) {
      if (!game.position(checkpoint.arrival)) continue;
      std::string error;
      if (sdk::write_progress(progress_file, std::string("garden-walk 1\n") + checkpoint.id, error))
        progress_notice = "CHECKPOINT SAVED";
      else {
        std::fprintf(stderr, "Garden Walk progress: %s\n", error.c_str());
        progress_notice = "COULD NOT SAVE CHECKPOINT";
      }
      return;
    }
    progress_notice = "NO CHECKPOINT IN THIS SCENE";
  }
  bool pause_available = false;
  bool paused = false;
  bool requested_route = false;
  bool ready = false;
  constexpr int width = 320;
  constexpr int height = 240;
  // One tile per step, like an overworld: 16 pixels in 200ms.
  constexpr int step_ms = 200;
  constexpr int update_ms = 10;
  sdk::GridStepState stepping;
  // A warp lands the player next to the return door, so a direction held through the
  // warp must be released before it can step back onto that door.
  bool door_latch = false;
  constexpr auto door_mask = sdk::collision_bit(1); // Door in blit-project.json.

  // Only configured doors count as targets for this game's contact rule.
  bool is_door(sdk::Entity entity) {
    const Door *door = game.world.get<Door>(entity);
    return door && door->scene[0] && door->arrival[0];
  }

  // Sample arrivals as a baseline so spawning inside a door does not send the player back.
  void prime_doors() {
    contacts.reset();
    guide_dialogue.close();
    fountain_pop.stop();
    apply_coat();
    const int tile = game.tile_size();
    const Size map = game.map_size();
    navigation.resize(tile ? map.w / tile : 0, tile ? map.h / tile : 0);
    requested_route = false;
    auto *player = game.position("player");
    const auto *collider = game.collider("player");
    if (player && collider) sdk::grid_place(stepping, *player, *collider, tile);
    contacts.update(game.world.find("player"), door_mask, is_door);
  }

  // Garden Walk chooses to travel on entry, after the SDK has finished querying.
  void enter_door(sdk::Entity player) {
    sdk::Entity entered;
    for (const auto &event : contacts.update(player, door_mask, is_door)) {
      if (event.phase == sdk::OverlapPhase::Enter) {
        entered = event.other;
        break;
      }
    }
    const Door *door = game.world.get<Door>(entered);
    if (door && game.travel(door->scene, "player", door->arrival)) door_latch = true;
  }
  // Search only on request. The tilemap tells us which cells are walkable.
  void find_fountain_route(const sdk::Transform2D &player) {
    const auto *fountain = game.position("fountain");
    const int tile = game.tile_size();
    const Size map = game.map_size();
    if (!fountain || !tile) return;
    const auto cell = [&](const sdk::Transform2D &position) -> sdk::RouteCell {
      if (!(position.x >= 0 && position.y >= 0 && position.x < map.w && position.y < map.h)) return {-1, -1};
      return {int(position.x / tile), int(position.y / tile)};
    };
    navigation.find(cell(player), cell(*fountain), [](sdk::RouteCell p) {
      return !game.tile_has_property(Point(p.x, p.y), "solid");
    });
    requested_route = true;
  }

  const sdk::Transform2D *nearby(const sdk::Transform2D &player, const char *name) {
    const auto *target = game.position(name);
    const int range = game.tile_size();
    return target && std::abs(player.x - target->x) <= range && std::abs(player.y - target->y) <= range
      ? target : nullptr;
  }

  void open_guide() {
    int64_t seconds;
    if (get_wall_clock(seconds)) {
      const int minutes = int((seconds % 86400 + 86400) % 86400) / 60;
      std::snprintf(clock_page, sizeof(clock_page), "The garden clock reads %02d:%02d UTC.", minutes / 60, minutes % 60);
    }
    evening_coat = !evening_coat;
    apply_coat();
    guide_dialogue.open({16, 180, 288, 40}, minimal_font, {
      "Welcome to the meadow! The ford is walkable, but the low hedge blocks your path.",
      clock_page,
      evening_coat ? "Try this blue coat. Your trail continues west to the garden." : "Back to your green coat. Follow the path west to the garden."
    });
  }

  Point adjacent_cell(Point cell, sdk::StepDirection direction) {
    switch (direction) {
      case sdk::StepDirection::left: return {cell.x - 1, cell.y};
      case sdk::StepDirection::right: return {cell.x + 1, cell.y};
      case sdk::StepDirection::up: return {cell.x, cell.y - 1};
      case sdk::StepDirection::down: return {cell.x, cell.y + 1};
      default: return cell;
    }
  }

  // The route is world-space debug art; the instruction bar remains screen-space.
  void draw_route(Point origin) {
    const int tile = game.tile_size();
    const auto &path = navigation.path();
    screen.pen = Pen(255, 218, 107);
    for (const auto p : path)
      screen.circle(Point(p.x * tile + tile / 2, p.y * tile + tile / 2) - origin, 2);
  }
}

// Raw logical controls are the fallback when the desktop bindings cannot load.
namespace {
  enum Action { Left, Right, Up, Down, Route, Pause, Accept, Back, ActionCount };
  constexpr Button default_buttons[]{Button::DPAD_LEFT, Button::DPAD_RIGHT, Button::DPAD_UP, Button::DPAD_DOWN,
                                     Button::A, Button::MENU, Button::A, Button::B};
#ifdef GARDEN_ACTION_BINDINGS
  sdk::InputActions input;
  sdk::InputActions::Handle actions[ActionCount];
  bool custom_input = false;
#endif

  bool action_down(Action action) {
#ifdef GARDEN_ACTION_BINDINGS
    if (custom_input) return input.down(actions[action]);
#endif
    return sdk::button_down(default_buttons[action]);
  }

  bool action_pressed(Action action) {
#ifdef GARDEN_ACTION_BINDINGS
    if (custom_input) return input.pressed(actions[action]);
#endif
    return sdk::button_pressed(default_buttons[action]);
  }

  // One direction per step. Left/right wins a diagonal press, matching the menu.
  sdk::StepDirection requested_direction() {
    if (action_down(Left)) return sdk::StepDirection::left;
    if (action_down(Right)) return sdk::StepDirection::right;
    if (action_down(Up)) return sdk::StepDirection::up;
    if (action_down(Down)) return sdk::StepDirection::down;
    return sdk::StepDirection::none;
  }
}

// Open the authored garden and select the game's screen size.
void init() {
  set_screen_mode(ScreenMode::hires, Size(width, height));
  static sdk::ComponentPool<Door> doors(game.world, game.world.capacity());
  ready = game.open(prime_doors);
  if (ready) {
    pause_available = pause_menu.load(game.project_asset_path("assets/pause.ui.json"),
                                     std::filesystem::u8path(game.project_asset_path(".")));
    audio_ready = menu_audio.load("assets/menu.audio.json", game);
    if (!audio_ready) std::fprintf(stderr, "Garden Walk audio: %s\n", menu_audio.last_error().c_str());
    if (!fountain_pop.load("assets/vfx/fountain-pop.vfx.json", game))
      std::fprintf(stderr, "Garden Walk fountain: %s\n", fountain_pop.last_error().c_str());
  }
#ifdef GARDEN_ACTION_BINDINGS
  if (ready) {
    const char *names[]{"move_left", "move_right", "move_up", "move_down", "route", "menu", "ui_accept", "ui_back"};
    for (int i = 0; i < ActionCount; ++i) actions[i] = input.action(names[i]);
    custom_input = input.load(game.project_asset_path("assets/input.bindings"));
  }
#endif
}

// Walk one tile at a time; a held direction keeps going, a blocked direction turns in place.
void update(uint32_t time) {
#ifdef GARDEN_ACTION_BINDINGS
  if (custom_input) input.update();
#endif
  fountain_pop.update(time);
  if (guide_dialogue.active()) {
    if (action_pressed(Accept)) guide_dialogue.advance();
    else if (action_pressed(Back)) guide_dialogue.close();
    return;
  }
  if (pause_available && action_pressed(Pause)) {
    paused = !paused;
    progress_notice = nullptr;
    if (paused) pause_menu.reset_focus();
    return;
  }
  if (paused) {
    if (action_pressed(Back)) {
      paused = false;
      return;
    }
    const int dx = int(action_pressed(Right)) - int(action_pressed(Left));
    const int dy = int(action_pressed(Down)) - int(action_pressed(Up));
    const auto previous_focus = pause_menu.focused_id();
    const auto command = pause_menu.update(dx, dy, action_pressed(Accept));
    if (audio_ready) {
      if (!command.empty()) menu_audio.play("menu_confirm");
      else if (previous_focus != pause_menu.focused_id()) menu_audio.play("menu_focus");
    }
    if (command == "resume") paused = false;
    else if (command == "show_route") {
      if (const auto *player = game.position("player")) find_fountain_route(*player);
      paused = false;
    } else if (command == "clear_route") {
      requested_route = false;
      paused = false;
    } else if (command == "save_checkpoint") {
      save_checkpoint();
    } else if (command == "restore_checkpoint") {
      if (restore_checkpoint()) paused = false;
    }
    return;
  }

  const sdk::Entity entity = game.world.find("player");
  sdk::Transform2D *player = game.world.get<sdk::Transform2D>(entity);
  const sdk::BoxCollider2D *shape = game.world.get<sdk::BoxCollider2D>(entity);
  if (!player || !shape) {
    contacts.update(entity, door_mask, is_door);
    return;
  }

  const int tile = game.tile_size();
  const Size map = game.map_size();
  sdk::StepDirection requested = requested_direction();
  if (door_latch) {
    if (requested == sdk::StepDirection::none) door_latch = false;
    else requested = sdk::StepDirection::none;
  }
  const auto outside = [&](Point cell) { return tile && (cell.x < 0 || cell.y < 0 || cell.x >= map.w / tile || cell.y >= map.h / tile); };
  const auto previous_cell = stepping.cell;
  if (tile && stepping.moving == sdk::StepDirection::none && requested != sdk::StepDirection::none) {
    const auto next = adjacent_cell(previous_cell, requested);
    if (outside(next)) {
      const auto target = sdk::grid_box_origin(next, tile, *shape);
      if (game.can_cross_map("player", {int(target.x), int(target.y)})) {
        stepping.destination = next;
        stepping.facing = stepping.moving = requested;
        stepping.elapsed_ms = 0;
      }
    }
  }
  const auto step = grid_step(stepping, *player, *shape, requested, tile, step_ms,
            tile ? map.w / tile : 0, tile ? map.h / tile : 0,
            [](Point cell) { return !game.tile_has_property(cell, "solid"); }, update_ms);
  if (step.finished && outside(stepping.cell)) {
    if (game.cross_map("player", {int(player->x), int(player->y)})) return;
    const auto origin = sdk::grid_box_origin(previous_cell, tile, *shape);
    player->x = origin.x;
    player->y = origin.y;
    sdk::grid_place(stepping, *player, *shape, tile);
  }
  if (auto *sprite = game.world.get<sdk::Sprite>(entity)) {
    if (stepping.facing == sdk::StepDirection::left) sprite->flip_x = true;
    if (stepping.facing == sdk::StepDirection::right) sprite->flip_x = false;
  }
  if (stepping.moving == sdk::StepDirection::none && (action_pressed(Accept) || action_pressed(Route))) {
    if (nearby(*player, "guide")) { open_guide(); return; }
    if (const auto *fountain = nearby(*player, "fountain"))
      fountain_pop.start(time, {int(fountain->x), int(fountain->y)});
  }
  if (action_pressed(Route)) find_fountain_route(*player);
  enter_door(entity);
}

// Scroll the whole scene, then draw a HUD in fixed screen coordinates.
void render(uint32_t time) {
  screen.pen = Pen(18, 30, 37);
  screen.clear();
  if (!ready) {
    game.draw(time); // Preserve the startup diagnostic on failed scene loads.
    return;
  }
  Point origin;
  if (!game.camera_origin("camera", origin) || !game.draw(time, origin)) return;
  if (requested_route) draw_route(origin);
  fountain_pop.draw(screen, origin);
  screen.pen = Pen(18, 30, 37);
  screen.rectangle(Rect(0, 0, width, 17));
  screen.pen = Pen(239, 242, 211);
  const char *hint = "GARDEN WALK / Z: ROUTE / ESC: PAUSE / EAST: MEADOW";
  if (const auto *player = game.position("player")) {
    if (nearby(*player, "guide")) hint = "Z / A: TALK TO THE MEADOW GUIDE";
    else if (nearby(*player, "fountain")) hint = "Z / A: PLAY THE FOUNTAIN EFFECT";
    else if (game.position("guide")) hint = "MEADOW / FOLLOW THE PATH TO THE GUIDE";
  }
  if (requested_route && navigation.path().empty()) hint = "NO ROUTE / MOVE AND TRY AGAIN";
  if (progress_notice) hint = progress_notice;
  screen.text(hint, minimal_font, Point(7, 5));
  if (paused) pause_menu.draw(screen);
  if (guide_dialogue.active()) {
    screen.pen = Pen(34, 51, 66);
    screen.rectangle({8, 172, 304, 60});
    guide_dialogue.draw(screen, Pen(239, 242, 211));
    screen.pen = Pen(255, 218, 107);
    screen.text("Z / A: NEXT", minimal_font, Point(16, 221));
  }
}
