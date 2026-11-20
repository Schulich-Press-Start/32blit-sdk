# Tools <!-- omit in toc -->

The individual asset tools have been deprecated in favour of https://github.com/32blit/32blit-tools which installs `32blit` or `32blit.exe` command on Linux/Mac or Windows respectively.

You can install these tools with `pip`:

```
pip3 install 32blit construct "bitstring<5"
```

Head on over to https://github.com/32blit/32blit-tools for further documentation covering the installation of the new tools.

- [Desktop development tools](#desktop-development-tools)
  - [Tiled maps and objects](#tiled-maps-and-objects)
  - [Tile properties](#tile-properties)
  - [Tiled project classes](#tiled-project-classes)
- [Asset Pipeline](#asset-pipeline)
  - [Additional Options](#additional-options)
- [Visual Studio](#visual-studio)

# Desktop development tools

This SDK also provides `blit` for the native SDL development workflow. It is separate from the upstream `32blit` asset packer and requires Python 3.9 or newer.

```sh
python -m pip install -e tools/blit-tools
```

Configure your game as **Debug** against this SDK. The bridge is enabled by default for native SDL Debug builds (`BLIT_DEV_BRIDGE=OFF` disables it); the code is not linked in Release, handheld, or browser builds. For an optimized Debug build with GCC or Clang, configure with `-DCMAKE_CXX_FLAGS_DEBUG="-O2 -g"`.

If the game uses a `sdk::World`, link `BlitWorld` and call `sdk::bridge_attach(world)` from `init()` after setting up the World and its pools. Guard the `#include "bridge.hpp"` and the call with `#ifdef BLIT_DEV_BRIDGE`. If you use `SceneRuntime`, pass it too: `sdk::bridge_attach(world, &scenes)` enables scene actions and retains the prefab catalogue and stable bindings across restart. Without a World, the bridge still provides connection info, pause/step/resume, timing, and game-loop logs.

Run the game yourself and connect a terminal, or let `blit dev` build and launch it:

```sh
./build/my-game --bridge 127.0.0.1:6132
blit console --exec "set player.Transform.x 200" --exec "get player.Transform" --json
```

```sh
blit dev --build-dir build --target my-game --exe build/my-game --build-on-save --scene levels/level1.scene
```

Use the actual executable path (with `.exe` and, for multi-config generators, the configuration folder where applicable). Run `blit dev` from the game project or pass `--project-dir`. Omit `--build-on-save` to rebuild from your IDE while `blit dev` watches the executable instead. A failed `blit dev` build does not stop the running game. A successful build requests a snapshot and restores it after `init()` in the new process. Only registered World state is restored; game globals, timers, audio, and other process state are initialized again.

`--scene` watches an SDK scene that your game reads through the attached `SceneRuntime`. The game still loads its initial scene in `init()`; the watcher sends `load_scene` after a save settles. It works with or without `--build-on-save`. Use `--scene-watch levels/props.scene` for each included file you also want to watch. These paths are relative to the project, use `/`, and cannot contain spaces. A valid save replaces the live scene without rebuilding or restarting; an invalid save reports `file:line` and leaves the current scene active. This does not convert Tiled files; it watches SDK scene files.

`blit console` with no arguments opens a prompt. It also accepts `--script FILE` and `--watch player.Transform.x`. In addition to SDK actions, it supports `hello`, `list`, `components`, `get`, `snapshot`, `pause`, `step`, `resume`, and `stats`. `step` requires a paused game. `stats` reports the last update and render times in microseconds. With `-DBLIT_MEMORY_CAP_MB=64` (example budget), it also reports `live_bytes` and `peak_bytes` for the game's C++ allocations. This opt-in simulator cap does not measure total process RAM or allocations through C APIs. Native SDL logs combined update/render work over 16.6 ms.

The protocol is UTF-8 JSON Lines on IPv4 loopback only. The game accepts two clients and processes at most one command per simulation poll. Frames are capped at 1 MiB, and log notifications are best-effort. The native tools do not start an HTTP server or implement browser reload.

The native simulator defaults to a 480×272 panel; launch the game with `--panel 640x480` to test a different panel. It uses the same integer-fit policy as `32blit-linux`. The game still chooses its own render resolution.

## Tiled maps and objects

With CMake 3.21+ and Python 3.9+, register the import on an existing game target. Keep the `SDK_COMPONENT` registrations in translation units linked by both the game and the host exporter:

```cmake
blit_component_metadata(game_registry registry.json components.cpp)
blit_tiled_assets(my-game maps/level.tmx authoring.schema.json registry.json)
```

CMake exports `registry.json` under the build directory and generates `generated/maps/level.generated.scene` in the project. For a cross-build, pass a registry file from a separate native build. No `blit dev` process or Tiled installation is needed for CMake to import checked-in inputs.

The schema explicitly maps object classes to registered fields. For example, a Tiled point `PlayerSpawn` can use a `pilot` prefab from the hand-authored scene:

```json
{"version":1,"classes":{"PlayerSpawn":{"shape":"point","prefab":"pilot",
  "fields":{"Transform.x":"$x","Transform.y":"$y"}}}}
```

Add `include generated/maps/level.generated.scene` to the authored level. Read the generated scene through `SceneReader`. When `blit::tiled_map_path(text)` returns a path, validate it under the project and read the complete packed file into `blit::load_tiled_map(bytes, size)`. Stage the resulting `Map`; use `SceneReader::finish` to publish it only on a successful scene load. Use `Map::layer_order` when drawing in Tiled order. Ship the generated scene and the `.map` file it names alongside the authored data, and ignore `generated/` in version control. To clean up old content-addressed map files, stop the game, remove the generated directory, then rebuild.

```sh
blit import maps/level.tmx --schema authoring.schema.json --registry build/registry.json
blit dev --build-dir build --target my-game --exe build/my-game --build-on-save \
  --scene levels/level1.scene --tiled maps/level.tmx \
  --tiled-schema authoring.schema.json --tiled-registry build/registry.json
```

`--tiled` is repeatable. It watches the TMX and referenced TSX files without sending those saves through the C++ build watcher. Supported inputs: finite orthogonal maps, 8×8 tiles, one atlas, CSV or base64 (optionally zlib), and explicitly mapped points/rectangles. Used tile IDs must fit the engine's one-byte index with `0` reserved for empty. Object references use the map path and Tiled object IDs.

## Tile properties

In `authoring.schema.json`, add `"tile_flags": {"solid": 0, "ladder": 1, "water": 2}` alongside `version` and `classes`. Each lowercase identifier gets an explicit, unique bit from 0 to 31. In Tiled, set a tile's matching custom property to **bool**; missing or false properties leave that bit clear. Unrelated properties are not imported as flags.

`blit_tiled_assets` generates `tile_flags.hpp` for the game target. The map named by the generated scene has a companion file at the same path plus `.props`; ship it alongside the map. Load it into a staged `sdk::TileProps` (`tile_props.hpp`) with `props.load(bytes, size, sdk::tile_flag_schema)`. A `false` return rejects the scene load. Publish the table and map together in `SceneReader::finish` on success.

```cpp
bool is_solid(const sdk::TileProps &props, const blit::Point &cell, blit::MapLayer &ground) {
  return props.has(ground.tile_at(cell), sdk::TileFlag::is_solid);
}
```

`Map`'s imported IDs are one-based, so `tile_at` can go straight into `has`. For a zero-based `TileMap`, first handle its empty sentinel and then add 1 to the atlas index (without narrowing). Editing existing bool values in Tiled reloads without a C++ build; moving a bit or renaming a flag changes the schema and requires rebuilding. Reordering declarations does not change the schema.

## Tiled project classes

After exporting `registry.json`, generate or refresh the workspace and reopen it in Tiled:

```sh
blit tiled-project my-game.tiled-project --schema authoring.schema.json --registry build/registry.json
```

Select a generated class such as `PlayerSpawn` on a map object, or `TileFlags` on an atlas tile. Object properties are typed from their registered SDK fields; geometry mappings like `$x` and `$y` use the object's placement instead. Optional per-class `defaults` are keyed by Tiled property name. Explicit values in TMX win over defaults. A mapped property without a schema default must be set on the object.

Refresh after schema or component changes. It preserves unrelated classes, IDs, folders and extension settings. Keep the project's `32blit.sdk.generatedClasses` property: it identifies the generated entries that the tool may replace. The workspace is not an input to the SDK runtime.

# Asset Pipeline

The new `32blit` tool has been integrated into CMake so that you can turn your raw images and assets into packed data at compile time.

To use this new setup your project must have an `assets.yml` and you should reference it in your `CMakeLists.txt` right after `blit_executable` like so:

```cmake
blit_assets_yaml (project-name assets.yml)
```

Substituting `project-name` for the first argument given to `blit_executable`.

Assuming you assets are in a directory called `assets/`, a simple `assets.yml` might look like this:

```yml
assets.cpp:
    assets/sprites.png:
        name: sprites_data
    assets/level.tmx:
        name: level_data
```

When you configure and compile your project, this will add an `assets.cpp` to your project's sources and output an `assets.hpp` file which you can include into your code to reference your assets:

```c++
#include "assets.hpp"
```

This file will include `sprites_data`, `sprites_data_length`, `level_data`, and `level_data_length`.

You can load sprites like so:

```c++
screen.sprites = Surface::load(sprites_data);
```

For your level data you might want to create an in-memory copy of it that you can edit to change your level in game:

```c++
// Store your level width/height in tiles
constexpr uint16_t level_width = 64;
constexpr uint16_t level_height = 64;

// Allocate memory for the writeable copy of the level
auto local_level_data = new uint8_t[level_width * level_height]

// Copy read-only level data into writeable copy
memcpy(local_level_data, level_data, level_width * level_height);

// Load our level data into the TileMap
level = new TileMap(local_level_data, nullptr, Size(level_width, level_height), screen.sprites);
```

## Additional Options
```yaml
assets.cpp:                         # Output filename, can also be a .hpp
    assets/sprites.png:
        name: sprites_data          # Name of variable generated for this asset, if omitted is generated from the
                                    # file name

        type: image/image           # Type of this asset, usually auto-detected from the extension

        palette: assets/palette.png # Optional palette for the image. Supports .act, .pal, .gpl and any image file

        transparent: 255,0,255      # Optional colour to map to transparency

        packed: yes                 # Pack the image into the minimum number of bits, for example an image with
                                    # two colours would use one bit per pixel. Set to false to always use eight
                                    # bits per pixel

        strict: true                # Allow only the colours defined in the palette, if false (the default)
                                    # colours are automatically added to the palette

    # If you do not need to specify any options other than the name, you can use
    assets/sprites.png: sprites_data

    # Parses a Tiled map
    assets/level.tmx:
        name: level_data
        type: map/tiled
    
    # Embeds a file without any processing
    assets/raw.bin:
        name: raw_bin
        type: raw/binary
    
    # Parses a CSV file into an array of bytes
    assets/raw.csv:
        name: csv_data
        type: raw/csv
     
    # Generates a font from an image
    # It can be used as `const Font font(asset_font)`
    assets/font.png:
        name: asset_font

        type: font/image            # This is required here as it would be auto-detected as an image asset

        height: 8                   # The height of the font, should match the image file if specified

        horizontal_spacing: 1       # Additional space between characters for variable-width mode (defaults to 1)

        vertical_spacing: 1         # Space between lines (defaults to 1)

        space_width: 4              # Width of the space character (defaults to 1)

    # Generates a font from a font file, can use any format supported by freetype
    assets/Comic_Sans_MS.ttf:
        name: asset_comic_font

        type: font/font

        height: 16                  # The height of the font to request, the resulting font's height may be
                                    # slightly different

        vertical_spacing: 1         # Space between lines (defaults to 1)

# It is also possible to define multiple outputs
more-assets.hpp:
    assets/extra_sprites.png: extra_sprites

```

# Visual Studio
To use `assets.yml` with a Visual Studio project, you need to run the packer as a pre-build step. Make sure that the output path is in the project's include path.
```
python -m ttblit pack --force --config $(ProjectDir)assets.yml --output $(ProjectDir)
```
To add the build step right click the project and click properties. From there go to build events and paste the command into the command line text box.

If the step is failing to complete then make sure that both python and the 32blit pip package is installed. 

After the build completes you will want to add the two files it generates to the project. (assets.cpp and assets.hpp).
Right click the project go to add then existing item and choose the two files from the project directory.

Failing to do this may cause errors when trying to debug similar to the ones below.

`LNK2001	unresolved external symbol "unsigned char const * const sprites_data" (?sprites_data@@3QBEB)`

`LNK1120	1 unresolved externals`
