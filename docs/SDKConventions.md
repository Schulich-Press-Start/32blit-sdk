# 32blit SDK coding conventions

Use this when adding something that the Pong tutorial does not cover. These are conventions for writing game code against the SDK, not a complete API listing. Use the [Editor Guide](../editor/README.md) for UI steps, [Tools](Tools.md) for asset packing and the development CLI, and Doxygen for signatures and overloads. [Team Pong](../examples/team-pong/pong.cpp) is a small working reference.

## Game loop, input, and units

| Callback | Put here | Keep out |
|---|---|---|
| `init()` | Screen mode, custom component pools, `game.open()` | Work that must repeat each tick |
| `update(uint32_t time)` | Input, movement, AI, collision, scoring, animation state changes | Drawing or reopening asset files every tick |
| `render(uint32_t time)` | Clear, draw map layers, entities, then HUD | Advancing physics or scoring based on how often the screen redraws |

The SDK schedules updates at **10 ms**. The callback's `time` is a timestamp in milliseconds, not a delta. If a speed is in pixels per second, multiply it by `0.010f` per update. Keep positions as floats so fractional movement accumulates; convert to pixels at drawing time. In the examples, screen origin is top-left, x increases to the right, and y increases downward. Name tile coordinates `cell` and use world pixels for positions, dimensions, and offsets.

```cpp
// From inside update(): separate held movement from a one-shot action.
const int direction = sdk::button_down(blit::Button::DPAD_DOWN)
                    - sdk::button_down(blit::Button::DPAD_UP);
paddle_y += direction * paddle_speed * 0.010f;
if (sdk::button_pressed(blit::Button::A)) serve();
```

`paddle_y`, `paddle_speed`, and `serve()` are game-owned state and behavior. Use held input for movement and `pressed` for actions such as serving or opening a menu.

## Entities, components, and game rules

- An entity is a handle, not a pointer. Check the result of `world.create()` or `world.find()` and don’t retain component pointers across removal or scene reload. `world.get<T>()` returns `nullptr` when the entity does not have that component.
- Register a custom data type once using `SDK_COMPONENT`. Attach a `ComponentPool<T>` to the `World` before loading a scene that uses it. Budget capacity for both the world and its pools; do not assume registration creates storage.
- Use `world.each<A, B>(...)` for systems that need both components. Update fields inside the callback, but create, destroy, add, and remove after the query completes. Don’t depend on query order.
- Author repeatable values on a prefab. Instance overrides beat prefab values, which beat C++ defaults. Keep transient state such as score or current velocity in gameplay code unless you deliberately need to serialize it.
- `Transform2D` gives an entity's position, `Sprite` selects artwork and a clip, and `BoxCollider2D` gives its axis-aligned shape. `GameScene` draws sprites; game code decides what happens when two shapes overlap.

For example, put the type in `paddle.hpp` and register it in one `paddle.cpp`:

```cpp
// paddle.hpp
struct PaddleDrive {
  float speed = 170.0f; // pixels per second
  bool computer = false;
};

// paddle.cpp
#include "paddle.hpp"
#include "registry.hpp"
SDK_COMPONENT(PaddleDrive, speed, computer);
```

Compile that registration into both the game and the registry exporter (`blit_component_metadata`). Rebuild and refresh `registry.json` after adding or changing reflected fields; otherwise the editor may not offer the new type. See [Pong’s CMake setup](../examples/team-pong/CMakeLists.txt) for the complete wiring.

## Playing a desktop scene

Include `game_scene.hpp` in a game built with `blit_scene_game`. This is a complete game that displays the startup scene; its empty `update()` is intentional because it has no gameplay rules:

```cpp
#include "game_scene.hpp"

sdk::GameScene game;

// Open the starting scene in a 320 by 240 window.
void init() {
  blit::set_screen_mode(blit::ScreenMode::hires, blit::Size(320, 240));
  game.open();
}
// Run game rules each tick; the starter scene has none yet.
void update(uint32_t) {}
// Clear the background, then draw the scene and its current animation frames.
void render(uint32_t time) {
  blit::screen.pen = blit::Pen(20, 22, 30);
  blit::screen.clear();
  game.draw(time);
}
```

- `open()` reads `scene` from `blit-project.json`. It attaches the built-in component pools, expands includes/prefabs and loads the artwork. `open(start_round)` also calls `void start_round()` after each successful authored load, including a bridge `load_scene`. Attach any custom pool to `game.world` **in `init()`, before `open()`**. The default entity capacity is 256; pass a capacity to the constructor to change it.
- `draw(time)` draws map layers and animated sprites in one world order. Larger `MapLayer::layer` and `Sprite::layer` values draw first; on ties, tiles draw before sprites and each type retains its prior order. New sprites default to `0`, and new tile layers to `1`. Set `Sprite::layer` directly in C++ to reorder a live sprite on the next draw. Draw the HUD afterward in screen space. A map is optional; an initial load failure shows a diagnostic and a failed reload preserves the previous world and artwork.
- `draw(time, camera)` uses the same world-space origin for map layers and sprites. `camera` is the world pixel at screen `(0, 0)`; clipping doesn't change that origin. Draw the HUD afterward in screen coordinates.
- `draw_from_camera(time, "camera")` explicitly selects a named `Camera2D`. It centers on the `follow` target’s transform, or the camera entity’s own transform if no target is available. It clamps to the map by default. `camera_origin(name, origin)` resolves the view without drawing. An unavailable camera returns false; ordinary `draw(time)` never selects a camera automatically.
- `map_size()` gives the map extent in pixels and `tile_size()` gives the cell width. `tile_has_property(cell, "solid")` takes **cell coordinates** and checks all layers. An empty cell, unknown property, or absent map returns false. The game still decides how a property affects movement.
- `GridNavigation` (`grid_navigation.hpp`) finds a shortest four-direction route in **cell coordinates**. Size its workspace with `resize(columns, rows)`, then call `find(start, goal, walkable)`. The path includes both endpoints or is empty if unreachable; a new search or resize replaces it. Searches reuse the buffers. The caller supplies walkability (including any clearance needed by larger actors), chooses when to search, and decides how to follow the route.
- `game.move_against_tiles(position, collider, blit::Vec2(dx, dy))` explicitly moves the box against the `solid` property across all map layers and blocks map edges. Pass a different property or `false` for `block_edges` when your game needs it. The result gives `moved`, `hit_x`, and `hit_y`; use the direction of the requested movement to distinguish a floor from a ceiling. The sweep stops at the first crossed solid face on each axis. It does not resolve initial overlaps or report actor contacts. For custom tile storage, include `tile_movement.hpp` and call `sdk::move_against_tiles` with a grid size and a cell predicate, as the platformer does.
- `game.position("hero")` and `game.collider("hero")` return pointers, or `nullptr` if the name or component is missing. Fetch them when needed rather than storing them across reloads. `sdk::bounds(*position, *collider)` yields `{left, top, right, bottom}` in world pixels. `sdk::overlaps(a, b)` requires positive overlapping area; `sdk::center_y(a)` gives the vertical midpoint. No helper changes speed, scores, or positions for you.
- Include `overlap_tracker.hpp` and construct `sdk::OverlapTracker` with your `World` once. Call `update(actor, filter)` after movement: it returns `Enter` on the first overlapping sample, `Stay` on later samples, and `Exit` when contact ends (including removal). The optional filter selects candidate entities. Handle events after the query; an exit can refer to a stale handle, and the returned events are only valid until the next `update()` or `reset()`. This is not swept detection. To ignore overlaps already present at scene load, `reset()` and sample once without handling those initial events, as Garden Walk does.
- `BoxCollider2D::category` is an index from 0–31, not an event or a render layer. Build a query mask with `collision_bit(index)` and combine bits with `|`. `OverlapTracker::update(actor, mask)` and `sweep_entities(world, box, delta, mask, ignore)` filter targets before geometry. The observer's category doesn't have to agree. A category outside 0–31 matches no query.
- `game.sweep(box, delta, mask, ignore)` returns `None`, `Entity`, or `Tile`, a hit fraction of the requested travel in `[0, 1]`, and an entry normal. It checks solid tiles separately from the entity mask; pass `false` for `solid_tiles` to exclude them. Tiles win exact ties, map edges do not block, and an initial overlap returns fraction `0` with a zero normal. Neither movement nor damage happens automatically. Targets are sampled at their current positions, not swept along their own paths.
- `game.travel("scenes/shed.scene", "player", "from_garden")` explicitly loads the destination and sets the destination player’s `x` and `y` from the named arrival. Both names must resolve to destination entities with `Transform2D`. A failed travel keeps the current world and artwork; successful travel replaces entity handles and calls the scene-loaded callback after positioning the actor. Call it outside world queries. Colliders and game-defined components never initiate travel on their own.

## Named input actions on desktop SDL

Include `input_actions.hpp`. Keep a `sdk::InputActions` map on the game thread, get a handle with `action("jump")` during setup, and call `update()` once at the start of each game update. Query `down(handle)` for held input or `pressed(handle)` / `released(handle)` for transitions. If an action has several bindings, it remains held until all of them are released. This doesn't change `blit::buttons`.

Call `load(path)` at startup or bind controls in code with `bind(handle, device, name)`. A successful load replaces the bindings without invalidating handles. A missing or invalid file logs an error, returns `false`, and leaves the map unchanged. After `GameScene::open()`, use `game.project_asset_path("assets/input.bindings")` to locate the file under the active project or packaged data root.

The format is `action keyboard|gamepad control`, one binding per line. Repeat the action to add an alternative. Use SDL scancode names such as `Left Shift` (physical key positions) and controller button names such as `a` or `dpleft`. Full-line comments start with `#`.

In Garden Walk, change `route keyboard Z` to `route keyboard K` in `assets/input.bindings`, then relaunch to trace the route with **K**. The platformer has the same setup for `jump`. No C++ changes are needed. The examples use raw buttons on non-SDL ports or if the binding file can't load.

## Authored screen-space UI

Include `ui_screen.hpp` in a scene game. Load a `.ui.json` with `UiScreen::load(game.project_asset_path("assets/menu.ui.json"))` after opening the project. The file defines the canvas size, paint order, widget bounds, text, and optional initial button. The canvas is centered at its authored pixel size. Draw it with `draw(screen)` after the world so it doesn't scroll with the camera.

Each widget has an optional integer `layer` (default `0`). Larger layers draw first, so lower layers appear in front. Within a layer, later widgets draw over earlier ones. Layer changes don't alter controller focus order.

The optional `style` object overrides `fill`, `border`, and `text` independently with `[red, green, blue, alpha]` arrays of integers from `0` to `255`. Omit a key to use the widget's default. Panels do not draw text, and the focused button outline takes precedence over its custom border.

When opening the menu, call `reset_focus()`. Pass directional and accept press edges to `update(dx, dy, accept)` while the menu is open. Only visible, enabled buttons can receive focus; an unavailable initial button falls back to the first eligible one in authored order. The SDK moves focus at most once, with horizontal input taking precedence, and returns the activated button ID. Your game decides whether to suspend gameplay, close the menu, or act on that ID. Garden Walk is a working example.

## Authored audio cues

A `.audio.json` file maps cue IDs to project-relative WAV paths. After `GameScene::open()`, call `AudioCues::load("assets/menu.audio.json", game)` from `audio_cues.hpp`. The SDL runtime preloads short WAVs and mixes `play("menu_focus")` into the existing audio output. A new cue replaces the previous cue, not the synth. Check `last_error()` if a call fails. Cue playback requires native SDL; game updates choose when to play sounds.

## Saving progress

Use `progress_store.hpp` for game-owned payloads. `write_progress(filename, payload, error)` writes a complete replacement under the HAL save directory. `read_progress(filename, max_bytes, payload, error)` bounds the read and distinguishes `loaded`, `missing`, and `error`. Supply a single portable filename, not a path. These calls are synchronous; use them at checkpoints or in menus, not per frame.

Version and validate your payload before applying it. Garden Walk saves a stable checkpoint ID and maps it back to an authored scene and arrival marker with `GameScene::travel`. It does not save entity handles or coordinates. Moving the marker changes where the checkpoint restores without invalidating the saved ID; a missing destination leaves the current scene intact.

## Assets and rendering

| File | Responsibility |
|---|---|
| `.scene` | Entities, prefabs, and authored component values |
| `.sprite.json` | PNG reference, rectangular frame grid, named clips, and frame times in milliseconds |
| `.tileset.json` | PNG reference, square cell size, optional tile properties |
| `.map` + `.map.json` | Cell placements and the link to the tileset |
| `blit-project.json` | Startup scene path, relative to the project root |

- Authoring paths are relative to the project root, **not** to the descriptor's folder. The player tries the editor project for project-local Play/Dev and finds a standalone game's manifest in `data/` beside the executable. Keep that directory with the executable when distributing it.
- `Sprite.asset` is the descriptor path without `.sprite.json` (for example, `assets/hero`, up to 23 UTF-8 bytes). `GameScene` resolves descriptors, validates their frame grids and caches their PNG sheets at load time. Frame cells are **zero-based**. A blank clip selects the first named clip, or cell zero if none exist. Don't derive collision bounds from the current frame.
- Map tile IDs are **one-based** (`0` is empty). Set the map cell size from its tileset before using `Map::cell_at`, `Map::cell_rect`, or `MapLayer::draw`. Prefer those helpers to hard-coding `/ 8`. Use the same geometry and layer data for drawing and tile collision; the game decides whether a tile property such as `solid` has any effect.
- Give the optional `.map` the same folder and basename as the `.scene`. Its `.map.json` sidecar selects the tileset, including its square tile size and optional six-digit transparent RGB key. PNG alpha is respected for both sprites and tiles. Save and restart Play (or reload the scene) to see changes to data and PNGs; only gameplay C++ or new reflected components require recompilation. Scene games need no `assets.yml` or per-image CMake entries.
- Lower-level games can still use packed assets or construct a `SceneRuntime` directly. `SceneReader::prepare` can validate and stage resources after the parser resolves all included sources; commit them in `SceneReader::finish` only on a successful load. Report `SceneError` with its file and line.

## Adding something beyond Pong

- **Animated enemy:** Author `Transform2D` + `Sprite` on a prefab. Add a game-defined `Patrol` component for speed and range. In `update`, move matching entities and change facing; in `render`, draw their sprite. Platformer is the larger reference for moving actors.
- **Pickup or trigger:** Author a `Transform2D` + `BoxCollider2D` and a custom marker component. In `update`, test overlap and award the pickup or fire an event. Queue the removal until after the world query. A trigger does not have to be solid or have a sprite.
- **Another level:** Author a `.scene` and matching map and tileset, then decide in game code when to load it. Creating a file in the editor does not switch the game's active scene automatically.
