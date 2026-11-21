# Make Pong with the 32blit SDK

**Team Pong** is a small, playable desktop game: you control the left paddle, the computer controls the right, and a missed ball gives the other side a point. The sample separates the court, actors, and animations you author from the gameplay you write in C++. No handheld or Tiled installation is needed. In the SDK checkout, the project is under `examples/team-pong`.

If you have the [Windows playtest kit](WindowsPlaytest.md), use **Play Pong.cmd** and **Edit Pong.cmd** to start immediately. Skip the source build below until you want to rebuild after changing C++.

## Build and play on Windows

Install Visual Studio or Build Tools with **Desktop development with C++**, CMake 3.21 or newer, and Python. From a Developer PowerShell with the SDK checkout as the current directory:

```powershell
py -m pip install '32blit==0.7.4' 'bitstring<5'
Set-Location .\examples\team-pong
cmake --preset windows
cmake --build build-windows --config Debug --target team_pong
.\build-windows\Debug\team_pong.exe
```

The first configure can download pinned dependencies. The SDK supplies the SDL packages for the Visual Studio build. If you copy `team-pong` out of the SDK, CMake also finds a sibling directory named `32blit-sdk`. For any other layout, configure with `cmake --preset windows "-D32BLIT_DIR=C:/src/32blit-sdk"` using the directory containing `32blit-config.cmake`. Run the executable **from the `team-pong` project directory** so it can read `scenes/` and the sprite descriptors in `assets/`.

| Keyboard | Gamepad | Action |
| --- | --- | --- |
| Up/Down arrows or W/S | D-pad Up/Down | Move the left paddle |
| Z or U | A | Serve when the game is waiting |
| X or I | B | Clear the score and return the ball to its serve position |

Press **Z** to start. The first serve comes toward you. Meet the ball near the end of your paddle to send it back at a steeper angle. After either player scores, press **Z** to serve again.

## Open the project in the editor

Build the game first: it exports `build-windows/registry.json`, so the editor knows about `PaddleDrive` as well as the built-in components. You can also export just the registry with `cmake --build build-windows --config Debug --target team_pong_registry`.

Build the [desktop editor](../editor/README.md) and open the sample, or use **Edit Pong.cmd** in the Windows kit. From the **SDK root**, using the editor build in that guide:

```powershell
.\build-editor\Release\32blit-editor.exe --project .\examples\team-pong
```

`blit-project.json` tells the editor to open `scenes/pong.scene` and use the sample's Windows build settings. Use **File → Save All** to write your authoring changes. **Play** builds and starts a fresh Debug game; use **Stop** before starting it again to see changes made during a session. If you run the game from PowerShell instead, relaunch it after changing the scene, map, or sprite descriptors. Only changes to C++ or packed PNG artwork need a rebuild.

## 1. Place actors and reuse a paddle prefab

Open `scenes/pong.scene` in **2D Viewport**. It contains three entities: `player`, `opponent`, and `ball`. The first two are instances of `paddle`, a prefab in the same scene. Select it in **Prefabs** to see the shared components:

```text
prefab paddle
  Transform2D
  Sprite asset=assets/paddle clip=pulse offset_x=-4 offset_y=-16
  BoxCollider2D width=8 height=32 offset_x=-4 offset_y=-16
  PaddleDrive
```

- `Transform2D` places an actor in the 320 × 240 court, with x increasing to the right and y increasing down. The entity supplies its own starting `x` and `y`.
- `Sprite` names the art and clip. The offsets put the sprite's top left 4 px left and 16 px above the transform, making the transform the paddle's center.
- `BoxCollider2D` uses the same offsets and an 8 × 32 box, so the drawn paddle and its hitbox line up. Select a paddle entity to see its orange collider outline. The box is data; `pong.cpp`, not the SDK, decides what happens when the ball hits it.
- `PaddleDrive` holds a `speed` in pixels per second and a `computer` flag. The player uses the C++ defaults; the opponent sets `computer=true` and its own slower speed. The opponent also overrides `Sprite.clip` to `steady`. An entity override wins over the prefab, which wins over the C++ default.

Try selecting `player` in **Entities** and setting `Transform2D.y` to `160`. Save and play: only the left paddle starts lower. Then try setting `PaddleDrive.speed` on the opponent to `65`. The computer should have more trouble returning angled shots. You can also edit the same `.scene` file in an external text editor and use **Reload** from its clean scene tab to pick up the changes.

## 2. Change a sprite clip

Open `assets/paddle.sprite.json` from **Project Files**. In **Sprites**, select `pulse`, select its second frame, and change **Duration (ms)** from `220` to `600`. Preview the clip, save the sprite, and play. The player's brighter frame lasts longer; the opponent's single-frame `steady` clip does not change.

The `.sprite.json` descriptor names the source PNG, frame dimensions, and each clip's cells, durations, and loop option. `Sprite.asset` is the project-relative descriptor path **without** `.sprite.json`. Both Pong descriptors use `assets/actors.png`: the paddles read 8 × 32 frames and the ball reads 8 × 8 frames. CMake packs the PNG via `assets.yml`; `sdk::draw_sprite` chooses and draws a frame using the elapsed milliseconds. The editor saves clip data, not pixels.

## 3. Paint the court

Open `scenes/pong.map` in **2D Viewport** to edit it on its own, or paint the same map behind `pong.scene`. The shared basename links the map to the scene in the editor. `scenes/pong.map.json` points to `assets/court.tileset.json`, which references `assets/court.png`.

1. In **Layers**, click the name **Court** to make it the paint target. Leave the visibility checkbox checked.
2. In **Tilesets**, choose the gold swatch (hover to see **Tile 4**). Turn **Paint** on in **2D Viewport** and click an empty cell inside the court. Try **Eraser** to clear it.
3. With the map tab active, use **File → Save Map**, or use **File → Save All**. Restart the game and look for the new tile.

The `.map` file stores native 8 × 8 cells, with tile ID 0 reserved for empty. The runtime reads it with `blit::load_tiled_map`, draws its layers with the packed court atlas, and then draws the actors. Use the existing court atlas for this exercise: the tileset association is for the editor, and the game uses `assets/court.png` from `assets.yml`. The court painting is decorative. The simulation bounces the ball at y=24 and y=216 regardless of which tiles you paint or erase.

## 4. Edit C++ from the editor

In **Project Files**, open `paddle.hpp` in **Documents**, replace `170.0f` with `230.0f` on the `speed` line, and choose **File → Save Document** (Ctrl+S with **Documents** focused). Save any other edits and click **Play**, or rebuild and launch from PowerShell. The player inherits the new default unless its entity or prefab sets `speed`; the opponent keeps its authored override. Saving source changes the project file but does not compile it. If a file has changed externally since you opened it, the editor refuses to overwrite the newer version; copy out any draft changes you want to keep, then discard and reopen the tab before merging them.

To follow the code, start in `pong.cpp`:

1. `init()` attaches component pools to a `sdk::World` and loads the authored scene through `sdk::SceneRuntime`. `SDK_COMPONENT(PaddleDrive, speed, computer)` in `paddle.cpp` registers the fields for scene loading and the editor's registry exporter. Registering a component does not add behavior.
2. `update()` runs every 10 ms. It moves the ball, moves paddles using their `PaddleDrive`, and reads the transforms plus collider offsets and sizes as rectangles. A crossing of a paddle face with vertical overlap reverses horizontal direction; the vertical bounce speed depends on where the ball hit the paddle. Once the ball leaves a side, the other side scores and the ball returns to its authored serve position.
3. `render()` draws the map, then every entity with both a `Transform2D` and a `Sprite`, then the score and controls.

You can also author this project in the browser editor: import a project ZIP containing `blit-project.json`, the source and assets, and the exported `registry.json` (for example, copy `build-windows/registry.json` to the archive root). **Save All** and **Export Project ZIP** to keep your edits. Build or run the game from an extracted copy on the desktop; the browser editor does not launch or compile it.
