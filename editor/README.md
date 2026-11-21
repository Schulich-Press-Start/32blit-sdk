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

## Run the browser editor

With the Emscripten SDK activated, build the same editor for WebAssembly from the SDK root:

```sh
emcmake cmake -S editor -B build-editor-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-editor-web --target 32blit-editor-web
python3 -m http.server 8080 --directory build-editor-web
```

Open `http://localhost:8080/32blit-editor-web.html`. **New Project** creates a project in the browser with `blit-project.json`, a built-in component registry, and `scenes/main.scene`. The starter is editable in both editors, but has no game source or build configuration.

To open an existing project, use **Import Project ZIP**. Put `blit-project.json` at the archive root or in a single enclosing project folder. Include the scenes, maps and their sidecars, tileset descriptors, and PNGs. For scene authoring, include the exported component registry: the manifest’s `registry` path, root `registry.json`, or the registry in the manifest’s configured build directory.

Use **File → Save All**, then **Export Project ZIP**. The browser shows **Exporting project ZIP…** while it packages the saved files. The browser working copy is in memory; save does not update the original ZIP, and reloading the page loses the working copy. Extract the exported ZIP to open it with the native editor. Browser builds do not launch games, run CMake, or connect to Live; no local helper is required for authoring.

## Playtest from the editor

Pass the directory of a trusted project containing `blit-project.json`. Platformer ships an example with its desktop build and launch settings and startup scene. Have CMake on `PATH`; **Dev** also needs the installed `blit` CLI.

- **Play** launches `windows.play_exe` when supplied in a Windows kit; otherwise it configures and incrementally builds a fresh Debug game. No files are watched.
- **Dev** starts `blit dev` with build-on-save and watches the startup scene and its native map for reload. Select **Build / Game Log** from the **Tools** dropdown for build and reload messages.
  Select and copy text from the Console or Build / Game Log. Drag the dock divider to give the output more room.
- **File → Reconfigure** runs the configured CMake preset without launching a game. Use a separate build folder when switching generators.
- **Stop** ends the editor-managed session. Close any independently launched watcher before using the same build folder with **Dev**.

## Arrange the editor

**Project Files** shows relevant files in a collapsible tree rooted at the project. Open a `.scene` in **2D Viewport**; other supported text files open in **Documents**. **File → Open File…** uses the same routing. Hidden directories, common build folders, and unsupported extensions are skipped. Other assets are listed without a preview.
  Right-click files or folders to **Copy**, **Paste** into a folder or the project root, or **Delete**. Paste never replaces an existing path. Deletion is permanent and requires confirmation; close affected assets before deleting them.

**Documents** starts with `CMakeLists.txt`. Text tabs are editable and show line numbers, the active line, and the caret’s line and column. Press **Ctrl+S** or choose **File → Save Document**. Unsaved text is marked on the tab and included in **Save All**. If a file changed on disk after opening, save refuses to overwrite it. PNG previews are read-only. Opening a document or scene tab does not change the startup scene for Play.

The main dock starts with **2D Viewport**, **Sprites**, and **Documents**. Opening a `.sprite.json` from **Project Files** selects **Sprites**. The right dock starts with **Entities**, **Prefabs**, and **Layers**; the desktop build also shows **Live** there and **Tools** in the bottom dock.

Each dock's **+** opens or moves a panel into that dock, even if it is empty. Drag a tab within a dock to reorder it or onto another dock to move it. Dropping outside the docks leaves it in place. Drag the dividers to resize the docks. Dock layout and open tabs reset when the editor closes.

## Entities, components, and prefabs

- An **entity** is one object placed in a scene or spawned by game code. At runtime, the `World` identifies it by an entity handle. The scene key identifies the authored object; the name is what `world.find()` looks up. The editor uses the key as the name for new entities.
- A **component** holds data. The SDK registers `Name`, `Transform2D`, `Sprite`, and `BoxCollider2D`; games can register their own types with `SDK_COMPONENT` (Platformer’s `Movement` is one example). Components are optional. Adding one makes its data available to game systems; it does not attach behavior by itself.
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

## Author a scene without running the game

**2D Viewport** opens the configured scene at startup. Open `.scene` and `.map` files from **Project Files** as tabs in the same viewport, or use the viewport tab strip’s **+** to create either. A `.map` tab works without a scene or component registry. **Entities** and **Prefabs** follow the active scene; **Layers** follows the active map. Reopen **2D Viewport** from a dock’s **+** to resume editing if the panel was closed.

Select a marker in **2D Viewport** or an item in **Entities** to inspect the same object. Use **Entities → Add Entity** to create a standalone entity or an instance of a prefab. Drag an editable marker to set local `Transform2D.x` and `Transform2D.y` overrides. Included entities are selectable but read-only; open their source scene to edit them.

A scene displays the `.map` with the same name in the same folder (for example, `levels/level.scene` and `levels/level.map`). The scene tab and standalone map tab share tile edits, dirty state, and undo/redo. **Layers** toggles individual layers and the scene’s entity overlay. Each tilemap needs a `.map.json` sidecar pointing to its `.tileset.json`; there is no project-wide atlas fallback.

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

**Game Size** sets the guide width and height in game pixels for this open scene tab. It defaults to **320 × 240** (the standard hires size); use **160 × 120** for lores or enter the size requested by the game’s `set_screen_mode` call. The guide is anchored at `(0, 0)` and does not resize the running game or change its emulated panel.

Use **Prefabs → Add Prefab** to create a reusable setup. Entities and Prefabs share the component editor: add or remove a local component, set a field, or reset an override. A component inherited from a prefab can only be removed at the prefab; removing a local copy restores the inherited values. Fields not set by the entity or prefab use their C++ component defaults; the registry records types, not default values.

**BoxCollider2D** describes an axis-aligned rectangle in world pixels: `width` and `height` give the size, and `offset_x` and `offset_y` locate its top-left relative to the entity’s `Transform2D`. The selected entity shows an orange outline when the box has positive dimensions. The component can be inherited from a prefab or overridden on an entity. It describes a shape only; the game must allocate a component pool and implement how the box collides.

**Sprite** displays the first frame of the selected clip on an entity with a `Transform2D`, using its `offset_x` and `offset_y` for the top-left corner in world pixels. Set `asset` to the project-relative descriptor path without `.sprite.json` (up to 23 bytes), and set `clip` to its name or leave it blank for the first clip. Previews inherit from prefabs and are independent of collider outlines. If the asset or clip cannot be resolved, the viewport keeps the entity marker. Reload the scene to refresh previews after editing a sprite file.

Use **File → Save** (**Ctrl+S**) on the active asset and **Edit → Undo / Redo** (**Ctrl+Z / Ctrl+Shift+Z**) for scene and map edits. **File → Revert** names the active dirty file and asks before discarding that file’s unsaved edits. Reverting a map also resets its other open view; it does not revert scene objects. **File → Save All** (**Ctrl+Shift+S**) writes dirty scenes, maps, tilesets, and sprites. Play saves nothing. A scene tab shows a dot if its scene or map is dirty. Select a locally authored entity to **Duplicate** or **Delete** it; deletion is refused if another object references it.

Saving writes the root `.scene` in canonical format (formatting and comments are not retained). You can make the same changes by hand without opening the editor. Use **Reload** to pick up external edits when the file is clean. Closing an unsaved tab asks before discarding.

**Live** is a separate panel for inspecting a running World. Live edits are temporary and do not write scene files. A fresh Play loads the saved scene and applies entity overrides over prefab values and C++ defaults.

Scene authoring needs `registry.json` in the configured build directory. For Platformer, build the `platformer_registry` target if it is missing. Click **Reload** after external scene, map, or registry edits.

The SDK scene and map codecs do not require SDL or a running World. A browser port can use the same codecs in WebAssembly with project files loaded into browser storage. Game launch and live inspection are separate from scene and map authoring.

## Author sprite animations

For a new sprite, choose the PNG from the list and accept the suggested **Sprite file** (or give it a project-relative name such as `assets/player`). The editor adds `.sprite.json` to extensionless names and avoids suggesting an existing filename. **Create draft** opens the new asset; **File → Save Sprite** writes it.

In **Sprites → Sheet settings**, choose rectangular frame dimensions from 1 to 2048 px. The grid must divide the sheet exactly. Pick cells, name clips, and use the frame list to reorder frames and edit their durations. **Set all** applies one duration to the clip; **Ctrl+Z** and **Ctrl+Shift+Z** undo and redo outside text fields.

**File → Save Sprite** writes frame order, durations, and loop settings, not PNG pixels. The browser exports the same descriptor. Platformer uses `Transform2D` and `Sprite` to render its actors from packed artwork. Gameplay still chooses movement, facing, and state-based animation. In a native project manifest, optional `scene_watch` paths let **Dev** reload game-authored dependencies such as sprite descriptors.


## Attach to an existing game

Start a Debug game with `blit dev` or pass `--bridge 127.0.0.1:8765`, then choose **File → Attach to Running Game…** and enter its local bridge address. The address field is only in the attach dialog; Play and Dev connect automatically.

Omit `--project` and pass `--connect 127.0.0.1:6132` to attach immediately. With `--project`, `--connect` only selects the address for the managed game; no game is launched until you click Play or Dev.

Select a named entity in **Live**, pick a component, edit a supported field, and click **Apply**. Use **Game** to pause, resume, refresh entities (**F5**), or disconnect. The command box is in **Tools → Console**. Live edits do not rewrite project files. The game window remains separate.
