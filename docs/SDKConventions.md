# 32blit SDK coding conventions

Use this when adding something that the Pong tutorial does not cover. These are conventions for writing game code against the SDK, not a complete API listing. Use the [Editor Guide](../editor/README.md) for UI steps, [Tools](Tools.md) for asset packing and the development CLI, and Doxygen for signatures and overloads. [Team Pong](../examples/team-pong/pong.cpp) is a small working reference.

## Game loop, input, and units

| Callback | Put here | Keep out |
|---|---|---|
| `init()` | Screen mode, component pools, packed assets, initial scene | Work that must repeat each tick |
| `update(uint32_t time)` | Input, movement, AI, collision, scoring, animation state changes | Drawing or reopening asset files every tick |
| `render(uint32_t time)` | Clear, draw map layers, entities, then HUD | Advancing physics or scoring based on how often the screen redraws |

The SDK schedules updates at **10 ms**. The callback's `time` is a timestamp in milliseconds, not a delta. If a speed is in pixels per second, multiply it by `0.010f` per update. Keep positions as floats so fractional movement accumulates; convert to pixels at drawing time. In the examples, screen origin is top-left, x increases to the right, and y increases downward. Name tile coordinates `cell` and use world pixels for positions, dimensions, and offsets.

```cpp
// From inside update(): separate held movement from a one-shot action.
const int direction = bool(blit::buttons & blit::Button::DPAD_DOWN)
                    - bool(blit::buttons & blit::Button::DPAD_UP);
paddle_y += direction * paddle_speed * 0.010f;
if (blit::buttons.pressed & blit::Button::A) serve();
```

`paddle_y`, `paddle_speed`, and `serve()` are game-owned state and behavior. Use held input for movement and `pressed` for actions such as serving or opening a menu.

## Entities, components, and game rules

- An entity is a handle, not a pointer. Check the result of `world.create()` or `world.find()` and don’t retain component pointers across removal or scene reload. `world.get<T>()` returns `nullptr` when the entity does not have that component.
- Register a custom data type once using `SDK_COMPONENT`. Attach a `ComponentPool<T>` to the `World` before loading a scene that uses it. Budget capacity for both the world and its pools; do not assume registration creates storage.
- Use `world.each<A, B>(...)` for systems that need both components. Update fields inside the callback, but create, destroy, add, and remove after the query completes. Don’t depend on query order.
- Author repeatable values on a prefab. Instance overrides beat prefab values, which beat C++ defaults. Keep transient state such as score or current velocity in gameplay code unless you deliberately need to serialize it.
- `Transform2D` is a position (with a rotation field), `Sprite` describes artwork and a clip, and `BoxCollider2D` describes an axis-aligned shape. Adding them does not install an AI, physics response, or renderer. Game code chooses what a collision means and when the actor changes animation.

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

## Assets and rendering

| File | Responsibility |
|---|---|
| `.scene` | Entities, prefabs, and authored component values |
| `.sprite.json` | PNG reference, rectangular frame grid, named clips, and frame times in milliseconds |
| `.tileset.json` | PNG reference, square cell size, optional tile properties |
| `.map` + `.map.json` | Cell placements and the link to the tileset |
| `assets.yml` | PNGs and other files packed into the game at build time |

- Resolve project-relative paths from the project root. Run the sample executable from that folder. Load and validate assets during initialization or reload, not inside the draw loop. When using `SceneRuntime` with staged assets, publish them in `SceneReader::finish` only on success; log `SceneError` with its file and line.
- A `Sprite.asset` value is the descriptor path without `.sprite.json` (for example, `assets/ball`). Resolve the descriptor and packed sheet, then pass them to `sdk::draw_sprite` with the transform, offsets in the `Sprite`, camera origin, and elapsed clip time. Sprite frame cells are **zero-based** and the grid must divide the sheet exactly. Don’t derive collision bounds from the current frame.
- Map tile IDs are **one-based** (`0` is empty). Set the map cell size from its tileset before using `Map::cell_at`, `Map::cell_rect`, or `MapLayer::draw`. Prefer those helpers to hard-coding `/ 8`. Use the same geometry and layer data for drawing and tile collision; the game decides whether a tile property such as `solid` has any effect.
- Saving a scene, clip, or map updates its data file. A build is needed for C++ changes, new reflected component types, or PNG changes that are packed into the executable. A prebuilt playtest executable cannot pick up new packed art just because you imported it into the editor.

## Adding something beyond Pong

- **Animated enemy:** Author `Transform2D` + `Sprite` on a prefab. Add a game-defined `Patrol` component for speed and range. In `update`, move matching entities and change facing; in `render`, draw their sprite. Platformer is the larger reference for moving actors.
- **Pickup or trigger:** Author a `Transform2D` + `BoxCollider2D` and a custom marker component. In `update`, test overlap and award the pickup or fire an event. Queue the removal until after the world query. A trigger does not have to be solid or have a sprite.
- **Another level:** Author a `.scene` and matching map and tileset, then decide in game code when to load it. Creating a file in the editor does not switch the game's active scene automatically.
