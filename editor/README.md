# 32blit Editor Guide

The optional desktop editor lets you author scenes and maps without running the game and tune temporary values while playtesting. Game builds use the same source files whether or not you open the editor.

From the SDK root on Windows:

```powershell
cmake -S editor -B build-editor "-DSDL2_DIR=$PWD/vs/sdl/SDL2-2.28.5/cmake"
cmake --build build-editor --config Release
.\build-editor\Release\32blit-editor.exe --project ..\32blit-platformer
```

On Linux, use the same configure and build commands without `SDL2_DIR` if SDL2 development files are installed, then run `./build-editor/32blit-editor`. CMake downloads pinned ImGui, nlohmann/json, and stb sources for this tool only.

The desktop editor uses the operating system's standard title bar and window controls. On Windows it requests per-monitor DPI awareness and rebuilds its font atlas when the window's DPI changes. The **File** menu lives inside the editor, below the native title bar.

Use **View → Dark Mode** in the top menu bar to switch the workspace to dark colors. Uncheck it to return to the default light theme. The desktop editor remembers this choice across projects and restarts; on Windows it stores `dark` or `light` in `%APPDATA%\32blit\Editor\theme.txt`, not in your project. The browser editor keeps the choice for the current session.

## Run the browser editor

With the Emscripten SDK activated, build the same editor for WebAssembly from the SDK root:

```sh
emcmake cmake -S editor -B build-editor-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-editor-web --target 32blit-editor-web
python3 -m http.server 8080 --directory build-editor-web
```

Open `http://localhost:8080/32blit-editor-web.html`. **New Project** in the browser creates an authoring project with `blit-project.json`, a built-in component registry, and `scenes/main.scene`. Unlike the native Windows starter, it has no game source, player, or build configuration.

To open an existing project, use **Import Project ZIP**. Put `blit-project.json` at the archive root or in a single enclosing project folder. Include the scenes, maps and their sidecars, tileset descriptors, and PNGs. For scene authoring, include the exported component registry: the manifest’s `registry` path, root `registry.json`, or the registry in the manifest’s configured build directory.

Use **File → Save All**, then **Export Project ZIP**. The browser shows **Exporting project ZIP…** while it packages the saved files. The browser working copy is in memory; save does not update the original ZIP, and reloading the page loses the working copy. Extract the exported ZIP to open it with the native editor. Browser builds do not launch games, run CMake, or connect to Live; no local helper is required for authoring.

## Start a new game on Windows

1. Extract the **whole** editor kit. Start `Editor/32blit-editor.exe`, choose **File → New Project...**, enter a project name, and click **Create...**. Pick any writable parent folder. The editor creates and opens a new child folder; it won't overwrite a project.
2. The project starts empty: `assets/`, `scenes/main.scene`, a built-in `registry.json`, and `blit-project.json` pointing to that scene. `game.cpp`, `CMakeLists.txt`, and `CMakePresets.json` are the source of a runnable game, not files you need to write to see your artwork. There are no Pong actors, rules, or required filenames for your sprites.
3. Add a `Transform2D` and `Sprite` to an entity, or create `scenes/main.map` and paint layers (see below). **File → Save All**, then **Play Scene**. The kit's prebuilt player reads the project's saved data, animates sprites, and draws the map. An empty scene is a blank game window. After saving another edit, use **Restart Scene Preview** to see it in the running preview.

New Project copies `Editor/SceneStarter/{game.cpp,CMakeLists.txt,CMakePresets.json}` and `Editor/SceneStarter/Runtime/` (the generic `game.exe` and its DLLs). It does not copy a reference game. A developer build without `SceneStarter` uses the SDK's `templates/scene-game` for the three source files; without a prebuilt runtime it offers **Build & Run**, not a compiler-free preview. To provide **Play Scene** in that editor, place the complete kit `SceneStarter` next to its executable before creating a project.

## Playtest and build

Open a trusted project with **File → Open Project...** on Windows, or pass `--project <folder>` to the desktop editor. Choose the folder containing `blit-project.json`. Save or discard changes, stop the game, and disconnect Live before switching projects.

| Top-bar action | What runs |
| --- | --- |
| **Play Scene** | The generic Windows player at `windows.preview_exe`, with the project as its working directory. No compiler or SDK installation is needed. It knows the SDK's built-in components; it never compiles or runs your `game.cpp`, including custom components. |
| **Restart Scene Preview** | Replaces **Play Scene** while the generic preview is running. It stops that preview and launches it again with the saved scene. It is disabled for unsaved changes and does not restart a compiled game or Dev. |
| **Build & Run** | CMake configures the project's platform preset, builds its `target` in Debug, then runs the manifest's `exe` from the project folder. Choose this for rules written in C++ **even when Play Scene is available**. A failed build does not fall back to the preview. |
| **Play** | For an existing project with `windows.play_exe`, runs that project's packaged game without rebuilding it. |
| **Dev** | Configures the project, then starts the installed `blit dev` CLI with build-on-save. It watches the startup scene and its associated map for reload; optional `scene_watch` paths add dependencies. |
| **Stop** | Ends the editor-managed game or watcher. Close independent watchers before sharing a Dev build folder. |

**File → Save All** before **Play Scene**, **Restart Scene Preview**, or **Build & Run**. These actions do not save for you. For configuration, build, and game output, open **Tools → Build / Game Log**. **File → Reconfigure** configures without launching.

The log follows running command output while scrolled to the bottom. Scroll up to read earlier output without jumping; scroll to the bottom to resume following.

**Building outside the kit.** Install CMake and a desktop C++ toolchain ([Windows source build](../docs/SourceBuild.md)). The new project's `windows` preset uses the default CMake generator. The editor makes the kit's sibling `32blit-sdk` (or its development checkout) available to CMake unless you set `32BLIT_DIR` yourself. An extracted project's source contains no absolute SDK path. From its folder you can instead configure once with the SDK of your choice:

```powershell
cmake --preset windows "-D32BLIT_DIR=C:/path/to/32blit-sdk"
cmake --build build-windows --config Debug --target game
```

For MSYS2 GCC/Ninja, use the `windows-mingw` preset from a UCRT64 environment with the matching toolchain and SDL libraries. To use that toolchain in the editor, set `windows.preset` to `windows-mingw`, `windows.build_dir` to `build-mingw`, and `windows.exe` to `build-mingw/Debug/game.exe` in `blit-project.json`, then launch the editor from that environment and reopen the project. Use separate build folders for different generators. On Linux the starter uses the `linux` preset and `build-linux/game`.

**Sharing a game.** Build the `game` target and distribute the executable **together with** its runtime libraries and its sibling `data/` directory, preserving that layout. The scene-game build stages authored data automatically; importing a sprite or map does not require editing a CMake asset list. No editor, checkout, or compiler is needed to play a distributed build. To share an *editable* project instead, include its manifest, source, registry, `scenes/`, `assets/`, and `Runtime/` if provided.

## Arrange the editor

**Project Files** shows relevant files in a collapsible tree rooted at the project. Open a `.scene` in **2D Viewport**; other supported text files open in **Documents**. **File → Open File…** uses the same routing. Hidden directories, common build folders, and unsupported extensions are skipped. Other assets are listed without a preview.
  Right-click files or folders to **Copy**, **Paste** into a folder or the project root, or **Delete**. Paste never replaces an existing path. Deletion is permanent and requires confirmation; close affected assets before deleting them.
  Open `assets/input.bindings` in **Documents** to edit a game's keyboard and gamepad bindings. Save, then relaunch the game to apply changes.

**Documents** starts with `CMakeLists.txt`. Text tabs are editable and show line numbers, the active line, and the caret’s line and column. Press **Ctrl+S** or choose **File → Save Document**. Unsaved text is marked on the tab and included in **Save All**. If a file changed on disk after opening, save refuses to overwrite it. PNG previews are read-only. Opening a document or scene tab does not change the startup scene for Play.

The main dock starts with **2D Viewport**, **Sprites**, **Documents**, **UI**, and **Audio**. Opening a `.sprite.json`, `.ui.json`, or `.audio.json` from **Project Files** selects its dedicated tab. The right dock starts with **Entities**, **Prefabs**, and **Layers**; the desktop build also shows **Live** there and **Tools** in the bottom dock.

Each dock's **+** opens or moves a panel into that dock, even if it is empty. Drag a tab within a dock to reorder it or onto another dock to move it. Dropping outside the docks leaves it in place. Drag the dividers to resize the docks. Dock layout and open tabs reset when the editor closes.

## Author a UI layout

Choose **File → New UI Layout…** or open an existing `.ui.json` from **Project Files**. In **UI**, use **Add Widget** for a panel, label, or button. Left-drag a widget to move it, scroll to zoom at the cursor, middle-drag to pan, and click **Fit View** to reset the view. Select a widget on the canvas or in the picker to edit its exact bounds, text, and **Layer**. Lower layers draw in front; widgets on the same layer draw in the order they appear in the layout. A button can be disabled or marked **Initial focus**. **Show Text** opens the same asset as JSON; save and relaunch the game to see your changes.

Under **Style**, edit **Fill color**, **Border color**, and (for labels and buttons) **Text color**. Each has its own alpha (**A**): `0` is transparent and `255` is opaque. Use **Reset** to restore that color's default. A focused button keeps its focus outline so a transparent border doesn't hide controller focus.

Try `assets/pause.ui.json` in Garden Walk. Open the pause menu with **Esc** or controller **Start**, move focus with the arrow keys or D-pad, and activate with **Enter** or controller **A**. The layout owns the button IDs and appearance; game code decides what each ID does.

## Author audio cues

Put short WAVs and a `.audio.json` file in the project. Start a new descriptor with `{}` or open Garden Walk's `assets/menu.audio.json` from **Project Files**. In **Audio**, select a cue or choose **New Cue**, then pick a project WAV. **Play Preview** auditions the selection in the desktop editor before you commit it. Choose **Apply Changes** or **Add Cue**, save with **Ctrl+S**, and relaunch the game. The game plays cues by ID, so changing a cue's WAV needs no C++ changes.

Garden Walk plays sounds when pause-menu focus moves or an item is activated. Try **Save checkpoint** in the shed, then relaunch and choose **Restore checkpoint** to return to its authored arrival marker.

## Entities, components, and prefabs

- An **entity** is one object placed in a scene or spawned by game code. At runtime, the `World` identifies it by an entity handle. The scene key identifies the authored object; the name is what `world.find()` looks up. The editor uses the key as the name for new entities.
- A **component** holds data. Built-ins include `Name`, `Transform2D`, `Sprite`, `BoxCollider2D`, and `Camera2D`. Games can register their own types with `SDK_COMPONENT`. Adding a component makes its data available to game code; it does not attach behavior by itself.
- A **prefab** is an optional recipe for an entity's starting components and values. It has a key but no runtime handle. Each placement creates a separate entity. If you only need one instance, you can skip the prefab.

### Create a one-off entity

1. In **Entities → Add Entity**, enter the key `level_exit`, leave **Prefab** set to **None**, and click **Create**.
2. Select `level_exit`, choose **Add Component → Transform2D**, set `x` and `y`, and use **File → Save Scene**.
3. After loading the scene in C++, look up the entity and check for the component before using it:

   ```cpp
   if (const auto exit = world.find("level_exit")) {
       if (const auto *position = world.get<sdk::Transform2D>(exit)) {
           // Use position->x and position->y in your level-exit logic.
       }
   }
   ```

   The same setup can be written by hand in the scene:

   ```text
   entity level_exit
     Transform2D x=100 y=40
   ```

### Reuse a setup with a prefab

In **Prefabs → Add Prefab**, create `crate`, then choose **Add Component → Transform2D**. The dialog shows known runtime defaults. Check fields to set prefab values, such as `y=40`; leave other fields unchecked to use their runtime defaults. In **Entities → Add Entity**, select `crate` to create instances. An entity override wins over the prefab value, which wins over the C++ default. **Reset** removes a local field override. Use **Live** to try values without saving them back.

For `Sprite`, pick an imported descriptor in **asset** and then a named animation in **clip**. The clip list uses the entity's effective asset, including an asset inherited from its prefab. A blank clip uses the descriptor default; **Edit text → Reset** instead removes a local override so the prefab can supply the value. Missing values stay visible so you can repair them without losing the authored value. Use **Save All** to commit changes.

For a game-defined `Door` with string fields `scene` and `arrival`, choose the destination `.scene` and a named entity with `Transform2D` from that saved scene. Save destination edits before selecting a marker. The picker only writes data: **Build & Run** is required to try the game's door behavior, and a `BoxCollider2D` alone does not switch scenes.

## Author a scene without running the game

**2D Viewport** opens the configured scene at startup. Open `.scene` and `.map` files from **Project Files** as tabs in the same viewport, or use the viewport tab strip’s **+** to create either. A `.map` tab works without a scene or component registry. **Entities** and **Prefabs** follow the active scene; **Layers** follows the active map. Reopen **2D Viewport** from a dock’s **+** to resume editing if the panel was closed.

Select a marker in **2D Viewport** or an item in **Entities** to inspect the same object. Use **Entities → Add Entity** to create a standalone entity or an instance of a prefab. Drag an editable marker to set local `Transform2D.x` and `Transform2D.y` overrides. Included entities are selectable but read-only; open their source scene to edit them.

To place a sprite without creating components by hand, drag a saved `.sprite.json` from **Project Files** onto a writable scene in the **2D Viewport** with the entity overlay visible. Each drop creates one entity with `Transform2D` and `Sprite` at the drop position. It respects **Snap** and is a single scene undo step. Leave the clip blank to use the first clip or choose an explicit clip in **Entities**.

A scene displays the `.map` with the same name in the same folder (for example, `levels/level.scene` and `levels/level.map`). The scene tab and standalone map tab share tile edits, dirty state, and undo/redo. **Layers** toggles individual layers and the scene’s entity overlay. Each tilemap needs a `.map.json` sidecar pointing to its `.tileset.json`; there is no project-wide atlas fallback.

On a writable map, **Layers → Add Layer** creates an empty layer with **Order** `1` and selects it for painting. Select a tile layer and edit **Order**, then press **Enter** to apply. Edit a sprite's **layer** in **Entities → Sprite** or on its prefab. The editor and game draw larger numbers first: a canopy at `0` covers a player at `1`, with ground at `2`. At equal numbers, tiles draw before sprites, and each type keeps its usual order. New sprites default to `0`. Order edits are undoable; **Save All** writes them to the map or scene.

Turn **Paint** on in **2D Viewport**. Choose a tile or **Eraser** in **Tilesets**, then click a layer's name in **Layers** to set the target. Its checkbox only controls visibility. The viewport explains why painting is blocked if a selection is missing or the target is hidden. Hover **Paint** when ready to see the active brush and target. Painting updates the map in memory; selecting a brush or target does not edit the map. Without the editor, use the SDK's `Map::set_tile` and `write_map` to change the same native map.

The viewport, Tilesets palette, and PNG viewer all use `PreviewImage` for PNG decoding and nearest-neighbor texture upload. Map and tileset previews can also apply an RGB color key. Tile slicing and transforms stay separate from image loading, so sprite previews can reuse it without adopting map rules.

### Create a tileset and tilemap

1. Open **Tilesets → +** and choose a PNG. Set the square cell size in pixels; both PNG dimensions must divide evenly into that size. Existing tilesets without `tile_size` default to 8 px. There can be up to 255 usable cells (the last cell of a legacy 256-cell atlas is not addressable).
2. Use **2D Viewport → + → New Tilemap**. Choose the map path, width and height in cells, and a tileset. This creates a native `.map` with an empty **Ground** layer and a `.map.json` sidecar that references the tileset. To put the map behind a scene, use the scene’s folder and basename.
3. In **Tilesets**, click a tile to edit its flags and, if this is the active map’s tileset, select it as the brush. Flags such as `solid`, `ladder`, and `water` are saved in the `.tileset.json`, not copied into maps. Gameplay code decides how to use them; importing a tileset does not change existing game collision logic.
   Use **Settings → Tileset Properties** to define or delete flags for the entire tileset. Deleting a definition asks for confirmation and reports how many tiles will lose it. Use the selected tile’s checkboxes to assign or unassign flags without removing the definitions. Unchecked flags are omitted from the tile’s JSON. Changes reach the file on Save. **Settings** also contains the optional Transparent RGB color key.

**Project Files** opens the PNG as an image preview, the `.tileset.json` in **Tilesets**, and the `.map` in **2D Viewport**. Keep the PNG, descriptor, map, and sidecar together when sharing a project.

Drag empty viewport space to pan when painting is off. Middle-drag or hold **Shift** and left-drag to pan in either mode. Scroll to zoom around the cursor. **Frame** (**F** by default) centers the selected entity without changing zoom, and **Fit Screen** fits the game-size guide. Rebind framing and the drag modifier in **Shortcuts** for the current session.

**View** contains **Grid**, **Grid px**, **Snap**, and the game-size guide settings. Grid spacing and snap are measured in world pixels. In Paint mode the grid follows the tileset cell size.

In a scene or map tab, choose **View → Show Tile Property** and select a property from the map’s saved tileset. Amber highlights mark cells where at least one layer has the property, including layers hidden in the editor. Paint and undo update the overlay immediately; save tileset changes to update property assignments. Select **Off** to clear the overlay. This view setting does not change project files, layer visibility, or gameplay.

**View → Validate Route** traces a single lane from named `spawn` to `goal` in a scene with a saved `path` tile property. Markers must be centered on endpoint cells. Cyan arrows show travel direction; errors show the first offending cell when available. The check includes hidden layers and unsaved scene/map edits without writing files.

**Game Size** sets the guide width and height in game pixels for this open scene tab. It defaults to **320 × 240** (the standard hires size); use **160 × 120** for lores or enter the size requested by the game’s `set_screen_mode` call. The guide is anchored at `(0, 0)` and does not resize the running game or change its emulated panel.

Use **Prefabs → Add Prefab** to create a reusable setup. Entities and Prefabs share the component editor: add or remove a local component, set a field, or reset an override. A component inherited from a prefab can only be removed at the prefab; removing a local copy restores the inherited values. Fields not set by the entity or prefab use their C++ component defaults; the registry records types, not default values.

**BoxCollider2D** describes an axis-aligned rectangle in world pixels: `width` and `height` give the size, and `offset_x` and `offset_y` locate its top-left relative to the entity’s `Transform2D`. The selected entity shows an orange outline when the box has positive dimensions. The component can be inherited from a prefab or overridden on an entity. It describes a shape only: the scene-game starter supplies built-in component pools, but your game decides how to respond to a collision.

To label collision categories, add an indexed `collision_categories` array to `blit-project.json`, for example `["Default", "Door", "Enemy", "Pickup"]`, and reopen the project. In **BoxCollider2D → category**, pick a label or edit its numeric index. Changing the order of labels changes what those numbers mean. Render layers are separate. The game supplies a query mask; authoring a category alone does not make an object solid or trigger any behavior.

**Camera2D** is a view the game can select explicitly. Select a placed camera in **Entities** to see its frame in the viewport. Its `Transform2D` is the center of a fixed view; set `follow` to an entity reference such as `@player` to use that entity’s position instead. `clamp_to_map` defaults to true. Selecting or editing a camera does not change editor pan or zoom, or automatically activate it during gameplay.

**Sprite** displays the first frame of the selected clip on an entity with a `Transform2D`, using its `offset_x` and `offset_y` for the top-left corner in world pixels. Set `asset` to the project-relative descriptor path without `.sprite.json` (up to 23 bytes), and set `clip` to its name or leave it blank for the first clip. Previews inherit from prefabs and are independent of collider outlines. If the asset or clip cannot be resolved, the viewport keeps the entity marker. Reload the scene to refresh previews after editing a sprite file.

Use **File → Save** (**Ctrl+S**) on the active asset and **Edit → Undo / Redo** (**Ctrl+Z / Ctrl+Shift+Z**) for scene and map edits. **File → Revert** names the active dirty file and asks before discarding that file’s unsaved edits. Reverting a map also resets its other open view; it does not revert scene objects. **File → Save All** (**Ctrl+Shift+S**) writes dirty scenes, maps, tilesets, and sprites. Play saves nothing. A scene tab shows a dot if its scene or map is dirty. Select a locally authored entity to **Duplicate** or **Delete** it; deletion is refused if another object references it.

Saving writes the root `.scene` in canonical format (formatting and comments are not retained). You can make the same changes by hand without opening the editor. Use **Reload** to pick up external edits when the file is clean. Closing an unsaved tab asks before discarding.

**Live** is a separate panel for inspecting a running World. Live edits are temporary and do not write scene files. A fresh Play loads the saved scene and applies entity overrides over prefab values and C++ defaults.

Scene authoring uses the manifest's `registry` path, or the configured build directory's `registry.json` if no path is specified. New native projects explicitly use root `registry.json`, so built-ins work before a first build. When adding a reflected custom component, build your project's registry exporter, copy `<build-dir>/registry.json` over the project's root `registry.json`, then reopen the project. Use **Build & Run** for scenes with that component; the generic preview cannot load game-specific types. For Platformer, build `platformer_registry` if its registry is missing.

The SDK scene and map codecs do not require SDL or a running World. A browser port can use the same codecs in WebAssembly with project files loaded into browser storage. Game launch and live inspection are separate from scene and map authoring.

## Author sprite animations

For a new sprite, choose the PNG from the list and accept the suggested **Sprite file** (or give it a project-relative name such as `assets/player`). The editor adds `.sprite.json` to extensionless names and avoids suggesting an existing filename. **Create draft** opens the new asset; **File → Save Sprite** writes it.

In **Sprites → Sheet settings**, choose rectangular frame dimensions from 1 to 2048 px. The grid must divide the sheet exactly. Pick cells, name clips, and use the frame list to reorder frames and edit their durations. **Set all** applies one duration to the clip; **Ctrl+Z** and **Ctrl+Shift+Z** undo and redo outside text fields.

**File → Save Sprite** writes frame order, durations, and loop settings, not PNG pixels. The browser exports the same descriptor. Platformer uses `Transform2D` and `Sprite` to render its actors from packed artwork. Gameplay still chooses movement, facing, and state-based animation. In a native project manifest, optional `scene_watch` paths let **Dev** reload game-authored dependencies such as sprite descriptors.


## Attach to an existing game

Start a Debug game with `blit dev` or pass `--bridge 127.0.0.1:8765`, then choose **File → Attach to Running Game…** and enter its local bridge address. The address field is only in the attach dialog; **Build & Run** and **Dev** connect automatically to a bridge-enabled Debug game. Prebuilt preview and packaged launches do not request a bridge.

Omit `--project` and pass `--connect 127.0.0.1:6132` to attach immediately. With `--project`, `--connect` only selects the address for a managed Debug game; opening a project does not launch it.

Select a named entity in **Live**, pick a component, edit a supported field, and click **Apply**. Use **Game** to pause, resume, refresh entities (**F5**), or disconnect. The command box is in **Tools → Console**. Live edits do not rewrite project files. The game window remains separate.
