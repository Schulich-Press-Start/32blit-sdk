# 32blit SDK coding conventions

Use this when adding something that the Pong tutorial does not cover. These are conventions for writing game code against the SDK, not a complete API listing. Use [Tools](Tools.md) for asset packing and the development CLI, and Doxygen for signatures and overloads. [Team Pong](../examples/team-pong/pong.cpp) is a small working reference.

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

### Wall-clock time

For calendar time, include `32blit.hpp` and call `blit::get_wall_clock(int64_t &unix_seconds)`. On success it returns `true` and writes whole Unix seconds since 1970-01-01 00:00:00 UTC, without applying a local time zone. On failure it returns `false` and leaves the output unchanged. Always check the return value and choose a fallback appropriate to the game; don't substitute uptime for a date or time of day.

The SDL port reads the host system clock, which can jump when adjusted and is not affected by editor pause or stepping. The bundled STM32, Pico, and standalone Linux ports do not provide a wall clock through this API. Use `blit::now()` and the update timestamp for game timing instead.

This is an optional HAL capability added in firmware API 0.3. Existing apps remain compatible with updated firmware; new device builds require API 0.3 under the normal firmware version checks, even on devices where the clock is unavailable.

## Entities, components, and game rules

- An entity is a handle, not a pointer. Check the result of `world.create()` or `world.find()` and don’t retain component pointers across removal or scene reload. `world.get<T>()` returns `nullptr` when the entity does not have that component.
- Register a custom data type once using `SDK_COMPONENT`. Attach a `ComponentPool<T>` to the `World` before loading a scene that uses it. Budget capacity for both the world and its pools; do not assume registration creates storage.
- Invalid pool setup (duplicate pools, a pool larger than its world, or attachment during a query) throws on exception-enabled builds and terminates on exception-free builds such as STM32. Normal runtime budget exhaustion still returns an invalid handle or `nullptr`.
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

Use `char[sdk::path_field_size]` (`path` in the registry) for project-relative paths such as `Door.scene`. It holds up to 63 UTF-8 bytes, plus the terminator. Use `/` between nonempty components; absolute paths, `.` and `..` components, backslashes, colons and control characters are not allowed. Set an authored field to `""` to clear its path, including an inherited value. Use `char[sdk::string_field_size]` (`string`) for short text such as entity and clip names, which still hold up to 23 UTF-8 bytes.

To clear a live path through the Debug console, use `set player.Sprite.asset ""`. An empty Set action value has the same effect for a Path field. Numeric fields still reject an empty value, and omitting the console value remains an error.

Reflected numeric text is locale-independent. Floats round-trip without heap allocation using the header-only `fast_float` parser and the SDK's existing JSON number converter; this also supports embedded C++17 libraries without floating-point `charconv`. CMake fetches the pinned headers unless they are supplied under `3rd-party`.

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
- Include `grid_step.hpp` for tile-locked movement. Keep a `GridStepState` per actor and call `grid_place()` after loading, travelling, or otherwise moving the actor outside the stepper. Pass a requested direction, grid dimensions in cells, a walkability predicate, and elapsed milliseconds to `grid_step()` each update. A request starts at most one step, and a held direction can start another after landing. The step interpolates to an exact cell-centred position; a reversal cannot interrupt it. Blocked requests update `facing` without moving. Garden Walk requires directional input to return to neutral after a door warp before accepting another step.
- `grid_cell(position, collider, tile_size, cell)` returns false for invalid or out-of-range geometry and leaves the output cell unchanged. `grid_place()` likewise returns false without changing the transform or stepping state. Check these results when accepting externally supplied positions.
- `game.position("hero")` and `game.collider("hero")` return pointers, or `nullptr` if the name or component is missing. Fetch them when needed rather than storing them across reloads. `sdk::bounds(*position, *collider)` yields `{left, top, right, bottom}` in world pixels. `sdk::overlaps(a, b)` requires positive overlapping area; `sdk::center_y(a)` gives the vertical midpoint. No helper changes speed, scores, or positions for you.
- Include `overlap_tracker.hpp` and construct `sdk::OverlapTracker` with your `World` once. Call `update(actor, filter)` after movement: it returns `Enter` on the first overlapping sample, `Stay` on later samples, and `Exit` when contact ends (including removal). The optional filter selects candidate entities. Handle events after the query; an exit can refer to a stale handle, and the returned events are only valid until the next `update()` or `reset()`. This is not swept detection. To ignore overlaps already present at scene load, `reset()` and sample once without handling those initial events, as Garden Walk does.
- `BoxCollider2D::category` is an index from 0–31, not an event or a render layer. Build a query mask with `collision_bit(index)` and combine bits with `|`. `OverlapTracker::update(actor, mask)` and `sweep_entities(world, box, delta, mask, ignore)` filter targets before geometry. The observer's category doesn't have to agree. A category outside 0–31 matches no query.
- `game.sweep(box, delta, mask, ignore)` returns `None`, `Entity`, or `Tile`, a hit fraction of the requested travel in `[0, 1]`, and an entry normal. It checks solid tiles separately from the entity mask; pass `false` for `solid_tiles` to exclude them. Tiles win exact ties, map edges do not block, and an initial overlap returns fraction `0` with a zero normal. Neither movement nor damage happens automatically. Targets are sampled at their current positions, not swept along their own paths.
- `game.travel("scenes/shed.scene", "player", "from_garden")` explicitly loads the destination and sets the destination player’s `x` and `y` from the named arrival. Both names must resolve to destination entities with `Transform2D`. A failed travel keeps the current world and artwork; successful travel replaces entity handles and calls the scene-loaded callback after positioning the actor. Call it outside world queries. Colliders and game-defined components never initiate travel on their own.

### Preparing fast scene transitions

`GameScene` retains recently loaded, validated scene catalogues and resources within a configurable byte budget. Cached travel re-instantiates entities without reading files, parsing scene text, or decoding PNGs. `preload_scene(path)` does the cold work before a door or seam is reached and does not create entities or call the scene-loaded callback. Check its result. Garden Walk preloads its three areas during startup.

```cpp
game.set_scene_cache_budget(256 * 1024);
if (!game.open(on_loaded)) return;
if (!game.preload_scene("scenes/shed.scene")) {
  // Report the failed preload before starting gameplay.
}
```

The default optional-retention budget is 2 MiB, not a preallocated 2 MiB buffer. `cached_scene_bytes()` reports a conservative retained-resource estimate, including compact catalogues, maps, and referenced image buffers. Shared images can be counted more than once, so retention stays below the estimate. The active scene, externally held VFX resources, and load-time scratch space still require memory outside this optional budget. Reducing the budget clears retained scenes; zero disables retention. Least recently used scenes are evicted when new cached resources would exceed the budget. Choose a budget and preload working set that fit the target hardware instead of loading every area of a large game.

Assets are immutable within a project generation. Normal travel uses the prepared versions. Call `open()` again, or use an explicit Debug bridge `load_scene`, to reload edited data. A failed load keeps the active world and its artwork. Loaded VFX retain their prior generation until reloaded explicitly. `ProjectAssets` in `project_assets.hpp` shares descriptors, text, and decoded images between scenes and effects; it does not keep unused pixel buffers alive. Its `stats()` counters report actual file reads and JSON/PNG decode attempts. Path metadata is bounded separately to 256 entries.

Cold scene loads remain synchronous. To avoid a hitch during play, prepare the required areas before play or during an explicit loading screen. This is not asynchronous streaming.

## Named input actions on desktop SDL

Include `input_actions.hpp`. Keep a `sdk::InputActions` map on the game thread, get a handle with `action("jump")` during setup, and call `update()` once at the start of each game update. Query `down(handle)` for held input or `pressed(handle)` / `released(handle)` for transitions. If an action has several bindings, it remains held until all of them are released. This doesn't change `blit::buttons`.

Call `load(path)` at startup or bind controls in code with `bind(handle, device, name)`. A successful load replaces the bindings without invalidating handles. A missing or invalid file logs an error, returns `false`, and leaves the map unchanged. After `GameScene::open()`, use `game.project_asset_path("assets/input.bindings")` to locate the file under the active project or packaged data root.

The format is `action keyboard|gamepad control`, one binding per line. Repeat the action to add an alternative. Use SDL scancode names such as `Left Shift` (physical key positions) and controller button names such as `a` or `dpleft`. Full-line comments start with `#`.

In Garden Walk, change `route keyboard Z` to `route keyboard K` in `assets/input.bindings`, then relaunch to trace the route with **K**. The platformer has the same setup for `jump`. No C++ changes are needed. The examples use raw buttons on non-SDL ports or if the binding file can't load.

## Authored screen-space UI

Include `ui_screen.hpp` in a scene game. Load a `.ui.json` with `UiScreen::load(game.project_asset_path("assets/menu.ui.json"))` after opening the project. The file defines the canvas size, paint order, widget bounds, text, and optional initial button. The canvas is centered at its authored pixel size. Draw it with `draw(screen)` after the world so it doesn't scroll with the camera.

Omit `font` to use `blit::minimal_font`, or set `"font": "assets/menu.font"` to use a packed 32blit font for all labels and buttons. Pass the project directory as the second argument to `load`, e.g. `std::filesystem::u8path(game.project_asset_path("."))`. The font path is relative to that root, not to the layout; an invalid or missing font rejects the load and keeps the previous screen.

Set `"wrap": true` on a label or button to wrap to its inner width. Authored line breaks are preserved, and words wider than a line split at glyph boundaries. The runtime prepares line breaks when the layout loads. Text still clips to the widget rectangle; wrapping does not resize the widget or change focus navigation. Omit the option to keep authored lines as-is.

For custom drawing, include `font_asset.hpp` and keep a `sdk::FontAsset` alongside the code using it. Call `load_project(root, "assets/menu.font")` for authored paths, or `load(path)` for an already resolved file, then pass `asset.font()` to `screen.text`. Check the load result (an optional `std::string*` receives the error), and keep the asset alive and unchanged while using the returned font view. The loader reads raw `FONT` blobs from the 32blit toolchain, with nonzero cell dimensions and glyph slots covering space (`0x20`) through DEL (`0x7f`), not PNG or TTF source files.

Each widget has an optional integer `layer` (default `0`). Larger layers draw first, so lower layers appear in front. Within a layer, later widgets draw over earlier ones. Layer changes don't alter controller focus order.

The optional `style` object overrides `fill`, `border`, and `text` independently with `[red, green, blue, alpha]` arrays of integers from `0` to `255`. Omit a key to use the widget's default. Panels do not draw text, and the focused button outline takes precedence over its custom border.

When opening the menu, call `reset_focus()`. Pass directional and accept press edges to `update(dx, dy, accept)` while the menu is open. Only visible, enabled buttons can receive focus; an unavailable initial button falls back to the first eligible one in authored order. The SDK moves focus at most once, with horizontal input taking precedence, and returns the activated button ID. Your game decides whether to suspend gameplay, close the menu, or act on that ID. Garden Walk is a working example.

## Dialogue boxes

Include `dialogue_box.hpp` in a scene game and keep a `sdk::DialogueBox` for the conversation. Open it with a screen-space text rectangle, a 32blit font, and pages in speaking order:

```cpp
#include "32blit.hpp"
#include "dialogue_box.hpp"

sdk::DialogueBox dialogue;

bool start_conversation() {
  return dialogue.open({16, 176, 288, 48}, blit::minimal_font,
                       {"The garden is open.\nPlease close the gate behind you.",
                        "The path to the fountain starts just beyond it."});
}

bool update_dialogue(bool advance_pressed) {
  return advance_pressed && dialogue.advance() == sdk::DialogueBox::AdvanceResult::finished;
}

void draw_dialogue() {
  dialogue.draw(blit::screen, blit::Pen(255, 255, 255));
}
```

`open()` copies the text, preserves explicit line breaks, wraps at spaces when possible, and splits overlong words when necessary. Overflow goes onto additional display pages without scaling the font. Each authored page starts on a fresh display page. An invalid setup (such as no pages or a box too small for its text) leaves any current dialogue unchanged. The font's glyph data must stay valid while the box is open.

Use a fresh press edge to advance, not held input or the same press that opened the dialogue. Advancing past the last page returns `finished` and makes `active()` false. Call `close()` to dismiss it without advancing. Draw it after the world and after any panel artwork. Drawing only paints text, is clipped to the text rectangle and surface clip, and doesn't change the current page.

## Authored audio cues

A `.audio.json` file maps cue IDs to project-relative WAV paths. After `GameScene::open()`, call `AudioCues::load("assets/menu.audio.json", game)` from `audio_cues.hpp`. The SDL runtime preloads short WAVs and mixes `play("menu_focus")` into the existing audio output. A new cue replaces the previous cue, not the synth. Check `last_error()` if a call fails. Cue playback requires native SDL; game updates choose when to play sounds.

## Authored visual effects

Each `.vfx.json` defines one named, one-shot effect. Save this example as `assets/fountain-pop.vfx.json` in Garden Walk, which already provides both sprite sheets and the audio descriptor:

```json
{
  "name": "Fountain Pop",
  "duration_ms": 700,
  "layers": [
    {"sprite": "assets/gardener", "cell": 0, "x": -6, "y": -7},
    {"sprite": "assets/fountain", "clip": "flow", "x": 0, "y": -12, "start_ms": 120, "clip_offset_ms": 180}
  ],
  "audio": "assets/menu.audio.json",
  "cues": [{"at_ms": 0, "cue": "menu_focus"}, {"at_ms": 350, "cue": "menu_confirm"}]
}
```

- `name` is a nonempty single-line label of at most 256 UTF-8 bytes. `duration_ms` is the effect's lifetime (1–3,600,000 ms).
- `layers` is optional and lists up to 64 sprites in paint order: the last layer draws on top. `sprite` is a project-relative `.sprite.json` stem, just like `Sprite.asset`. Each layer picks either a zero-based atlas `cell` or a named `clip`. A clip honors its existing frame durations and loop setting; a non-looping clip holds its last frame.
- `x` and `y` default to zero and locate the frame's top-left in whole pixels relative to the effect origin (−32,768 to 32,768). `start_ms` defaults to zero and must precede `duration_ms`. Once visible, the layer draws until the effect ends. For a `clip`, `clip_offset_ms` starts sampling partway through that animation; it defaults to zero (0–3,600,000 ms).
- `audio` is a project-relative `.audio.json` path; it is required only when `cues` has entries. Each of up to 256 timeline entries binds a `cue` ID to `at_ms` (0 through `duration_ms`, inclusive). Entries fire in time order, breaking ties by authored order. The native SDL player uses the existing single cue channel, so a new cue replaces the one still sounding.

Keep a `sdk::Vfx` from `vfx.hpp` as game state. Call `effect.load("assets/fountain-pop.vfx.json", game)` after `GameScene::open()` and check the return value and `last_error()`. Loading preloads and validates the referenced art and audio; a failed replacement leaves the previous effect loaded. Builds without native SDL cue output can load sprite-only effects.

VFX preparation uses `GameScene::project_assets()` to share sprite descriptors and decoded images with the scene. A loaded effect retains shared sprite handles and its project-generation service, so it can keep drawing after scene travel or another project is opened; it does not automatically adopt edited art or the new project's assets. Replacing or destroying the effect releases those handles. Animation lookup caches remain per effect. Audio descriptor text is shared during preparation, while WAV buffers remain owned by each audio player and use the existing SDL cue channel.

Call `effect.start(time, {world_x, world_y})` on a gameplay event and `effect.update(time)` on every update, using the engine's monotonic millisecond timestamp. Start fires zero-offset cues; subsequent updates fire each crossed cue once even if a frame was skipped. Call `effect.draw(blit::screen, camera_origin)` after drawing the scene with that same camera origin. Drawing and updating do not load files. Restart starts a new run at zero; `stop()` stops drawing and cancels pending cues without cutting off a cue that has already fired.

For a runtime smoke check, trigger the example at a visible world position on a press edge. The gardener appears immediately with the first cue, the fountain appears 120 ms later already sampling the second clip frame, and the second cue fires once at 350 ms. Both layers disappear at 700 ms. Trigger it again to check restart; call `stop()` before 350 ms to check that the second cue is canceled.


## Saving progress

Use `progress_store.hpp` for game-owned payloads. `write_progress(filename, payload, error)` writes a complete replacement under the HAL save directory. `read_progress(filename, max_bytes, payload, error)` bounds the read and distinguishes `loaded`, `missing`, and `error`. Supply a single portable filename, not a path. These calls are synchronous; use them at checkpoints or in menus, not per frame.

Version and validate your payload before applying it. Garden Walk saves a stable checkpoint ID and maps it back to an authored scene and arrival marker with `GameScene::travel`. It does not save entity handles or coordinates. Moving the marker changes where the checkpoint restores without invalidating the saved ID; a missing destination leaves the current scene intact.

## Assets and rendering

| File | Responsibility |
|---|---|
| `.scene` | Entities, prefabs, and authored component values |
| `.sprite.json` | PNG reference, rectangular frame grid, named clips, and frame times in milliseconds |
| `.tileset.json` | PNG reference, square cell size, optional tile properties and animated tiles |
| `.map` + `.map.json` | Cell placements, the link to the tileset, optional solidity overrides, and connections |
| `blit-project.json` | Startup scene path and optional per-scene sprite preloads, relative to the project root |

- Authoring paths are relative to the project root, **not** to the descriptor's folder. The player tries the editor project for project-local Play/Dev and finds a standalone game's manifest in `data/` beside the executable. Keep that directory with the executable when distributing it.
- `Sprite.asset` is the project-relative descriptor path without `.sprite.json` (for example, `assets/hero`, up to 63 UTF-8 bytes). `GameScene` resolves descriptors, validates their frame grids and caches their PNG sheets at load time. Frame cells are **zero-based**. `Sprite.clip` remains a short name (up to 23 UTF-8 bytes). A blank clip selects the first named clip, or cell zero if none exist. Don't derive collision bounds from the current frame.
- For sprites assigned by gameplay code rather than by a scene or prefab, add `"sprite_preloads": {"scenes/main.scene": ["assets/hero", "assets/hero_run"]}` to `blit-project.json` in the editor's **Documents**. Save and reload the scene. Use the exact stems you assign to `Sprite.asset`. The player loads only the destination scene's list alongside its authored sprites, reusing duplicate entries without creating entities. A missing or invalid preload resource aborts that scene load and preserves the previous scene.
- Map tile IDs are **one-based** (`0` is empty). Set the map cell size from its tileset before using `Map::cell_at`, `Map::cell_rect`, or `MapLayer::draw`. Prefer those helpers to hard-coding `/ 8`. Use the same geometry and layer data for drawing and tile collision; the game decides whether a tile property such as `solid` has any effect.
- Give the optional `.map` the same folder and basename as the `.scene`. Its `.map.json` sidecar selects the tileset, including its square tile size and optional six-digit transparent RGB key. PNG alpha is respected for both sprites and tiles. Save and restart Play (or reload the scene) to see changes to data and PNGs; only gameplay C++ or new reflected components require recompilation. Scene games need no `assets.yml` or per-image CMake entries.
- Lower-level games can still use packed assets or construct a `SceneRuntime` directly. `SceneReader::prepare` can validate and stage resources after the parser resolves all included sources; commit them in `SceneReader::finish` only on a successful load. Report `SceneError` with its file and line.
- To preserve a native pixel-art scale, resolve `camera_origin(name, view_size, origin)` for a smaller viewport and call `draw_to(surface, time, origin)` on a reusable buffer. Integer-scale that buffer into the display; draw text or HUD at display resolution. Scene positions and collision remain in map pixels.

### Connected maps

Add a `connections` object to the `.map.json` sidecar to attach a neighboring map to an edge:

```json
{
  "tileset": "assets/terrain.tileset.json",
  "connections": {"west": {"scene": "scenes/route.scene", "offset_px": 0}}
}
```

Edges are `west`, `east`, `north`, and `south`. A link names a different scene with a native map. `offset_px` shifts the neighbor down for east/west links or right for north/south links, relative to the active map. Edges must overlap. Add the opposite link with the negated offset on the neighbor for travel in both directions and a consistent view across the boundary.

`GameScene` stages connected tilemaps with the active one and draws their visible portions, including animated tiles. Only the active scene's entities run and draw. A clamped scene camera can scroll into the neighboring map. Before stepping outside, call `can_cross_map(actor, proposed_world_point)` to check the destination against the actor's collider; after movement reaches that point, call `cross_map(actor, proposed_world_point)`. That loads the destination scene and translates the actor's position before the load callback. This does not change movement or collision-query boundaries automatically. Use ordinary marker-based `travel()` for doors and other teleports.

### Tile solidity and animation

An optional `.map.json` field overrides solidity by position, independently of art or render layer:

```json
{
  "tileset": "assets/terrain.tileset.json",
  "solid_overrides": [{"x": 3, "y": 4, "solid": false}, {"x": 6, "y": 2, "solid": true}]
}
```

Coordinates are zero-based cells. Each coordinate must be in bounds and appear at most once. An omitted coordinate inherits `solid` from any layer; `false` clears that result, and `true` can block even an empty cell. `GameScene::tile_has_property(cell, "solid")`, the default `move_against_tiles`, and tile-inclusive `sweep` all respect the override. Other properties are unchanged. Existing BTMP v1/v2 maps can use this field without changing the binary.

An optional `.tileset.json` field animates all placements of a base tile ID:

```json
{
  "image": "assets/terrain.png",
  "animations": [
    {"name": "water", "cell": 7, "frames": [{"cell": 7, "duration_ms": 120}, {"cell": 8, "duration_ms": 160}]}
  ]
}
```

Names and base cell IDs must be unique within the tileset. Base and frame IDs are one-based and must exist in the PNG. Each clip loops through 1–1024 frames of 1–60000 ms each. All placements share the same phase (`GameScene::draw` uses its time argument; the editor uses its preview clock). The drawn frame keeps the placement's transform; a frame never triggers another tile animation. Collision and other tile properties still read the original base ID, not the displayed frame.

## Adding something beyond Pong

- **Animated enemy:** Author `Transform2D` + `Sprite` on a prefab. Add a game-defined `Patrol` component for speed and range. In `update`, move matching entities and change facing; in `render`, draw their sprite. Platformer is the larger reference for moving actors.
- **Pickup or trigger:** Author a `Transform2D` + `BoxCollider2D` and a custom marker component. In `update`, test overlap and award the pickup or fire an event. Queue the removal until after the world query. A trigger does not have to be solid or have a sprite.
- **Another level:** Author a `.scene` and matching map and tileset, then decide in game code when to load it. Creating a file in the editor does not switch the game's active scene automatically.
