#include "game_scene.hpp"
#include "door.hpp"
#include "overlap_tracker.hpp"
#include "grid_navigation.hpp"
#include "ui_screen.hpp"
#include "audio_cues.hpp"
#include "progress_store.hpp"

#include <cstdio>
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
  bool audio_ready = false;
  const char *progress_notice = nullptr;
  constexpr char progress_file[] = "garden-walk-progress.txt";
  struct Checkpoint { const char *id, *scene, *arrival; };
  constexpr Checkpoint checkpoints[]{
    {"garden", "scenes/main.scene", "from_shed"},
    {"shed", "scenes/shed.scene", "from_garden"},
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
  constexpr float step = 1.2f; // 120 pixels per second at 100 updates per second.
  constexpr auto door_mask = sdk::collision_bit(1); // Door in blit-project.json.

  // Only configured doors count as targets for this game's contact rule.
  bool is_door(sdk::Entity entity) {
    const Door *door = game.world.get<Door>(entity);
    return door && door->scene[0] && door->arrival[0];
  }

  // Sample arrivals as a baseline so spawning inside a door does not send the player back.
  void prime_doors() {
    contacts.reset();
    const int tile = game.tile_size();
    const Size map = game.map_size();
    navigation.resize(tile ? map.w / tile : 0, tile ? map.h / tile : 0);
    requested_route = false;
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
    if (door) game.travel(door->scene, "player", door->arrival);
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
}

// Open the authored garden and select the game's screen size.
void init() {
  set_screen_mode(ScreenMode::hires, Size(width, height));
  static sdk::ComponentPool<Door> doors(game.world, game.world.capacity());
  ready = game.open(prime_doors);
  if (ready) {
    pause_available = pause_menu.load(game.project_asset_path("assets/pause.ui.json"));
    audio_ready = menu_audio.load("assets/menu.audio.json", game);
    if (!audio_ready) std::fprintf(stderr, "Garden Walk audio: %s\n", menu_audio.last_error().c_str());
  }
#ifdef GARDEN_ACTION_BINDINGS
  if (ready) {
    const char *names[]{"move_left", "move_right", "move_up", "move_down", "route", "menu", "ui_accept", "ui_back"};
    for (int i = 0; i < ActionCount; ++i) actions[i] = input.action(names[i]);
    custom_input = input.load(game.project_asset_path("assets/input.bindings"));
  }
#endif
}

// Read four-way input and slide along walls by checking one axis at a time.
void update(uint32_t) {
#ifdef GARDEN_ACTION_BINDINGS
  if (custom_input) input.update();
#endif
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

  float dx = 0;
  float dy = 0;
  if (action_down(Left)) dx -= step;
  if (action_down(Right)) dx += step;
  if (action_down(Up)) dy -= step;
  if (action_down(Down)) dy += step;
  if (dx != 0 && dy != 0) {
    dx *= 0.70710678f;
    dy *= 0.70710678f;
  }
  if (dx != 0 || dy != 0) game.move_against_tiles(*player, *shape, Vec2(dx, dy));
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
  screen.pen = Pen(18, 30, 37);
  screen.rectangle(Rect(0, 0, width, 17));
  screen.pen = Pen(239, 242, 211);
  const char *hint = pause_available ? "GARDEN WALK / MOVE / ROUTE / MENU: PAUSE" : "GARDEN WALK / MOVE / ROUTE TO FOUNTAIN";
  if (requested_route && navigation.path().empty()) hint = "NO ROUTE / MOVE AND TRY AGAIN";
  if (progress_notice) hint = progress_notice;
  screen.text(hint, minimal_font, Point(7, 5));
  if (paused) pause_menu.draw(screen);
}
