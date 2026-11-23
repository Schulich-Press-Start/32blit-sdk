# Scene editor roadmap: from Pong to top-down games

Build small SDK modules that consume what the editor already saves. Do not put movement, scores, story progression, or enemy rules into the editor. Keep `Play Scene` an art-and-layout preview; use `Build & Run` to test C++ game rules.

Track general runtime capabilities and the final Godot/Unity reassessment in [EngineCapabilities.md](EngineCapabilities.md).

| Priority | SDK work | Editor workflow | Design references |
| --- | --- | --- | --- |
| 1. Camera and viewport | Draw tiles and sprites from one world origin and leave HUD coordinates fixed. `Camera2D` provides fixed and follow views with optional map clamping; game code chooses the camera. | Select a camera to see its frame without changing editor pan. | [RiverRaider](https://github.com/lummi01/32Blit-RiverRaider), [Super Square Bros.](https://github.com/ThePythonator/Super-Square-Bros) |
| 2. Tile properties and collision | Query saved tile flags and explicitly sweep bodies against solid tiles with `move_against_tiles`. Keep `one_way`, `path`, and `buildable` distinct from `solid`. | **View → Show Tile Property** highlights cells on any layer, including editor-hidden layers. | [TowerDefense](https://github.com/Blackhawk-TA/TowerDefense), [Miner Willy](https://github.com/mikerr/32blit-games/tree/master/willy), [Jetpac](https://github.com/mikerr/32blit-games/tree/master/jetpac) |
| 3. Scene links and named markers | Resolve a destination scene and a named arrival marker. Keep door conditions, interactions, and progression in game code. | Pick a destination and arrival marker in the inspector; flag broken links before running. Use named placements for starts and exits instead of hard-coded coordinates. | [GateKeeper](https://github.com/Blackhawk-TA/GateKeeper), [Pitfall Harry](https://github.com/lummi01/32Blit-Pitfall-Harry) |

## First milestone: Garden Walk

[Garden Walk](../examples/garden-walk/) is a top-down example with original art. The camera follows the player across the garden and the HUD stays fixed. Walls block movement because their tiles have the authored `solid` property.

Walk south from the starting position into `shed_door`, then walk south in the shed to return. In **Entities**, select `shed_door` and use **Door → scene / arrival** to change `from_garden` to `spare_arrival`. **Save Scene**, then relaunch the same executable from the project folder; it reads the new destination without recompiling. A standalone export uses its own saved `data/` copy.

`BoxCollider2D` only supplies the overlap area. The example's `Door` holds two strings, and `enter_door()` in `game.cpp` explicitly calls `game.travel(...)` on entry. Both scenes author their own `player` and a named arrival entity with `Transform2D`. The runtime moves the destination's player to the marker; it does not carry the source entity's state across scenes. The linked projects are design references; Garden Walk uses no upstream art or game code.

Garden Walk samples `OverlapTracker` after movement and travels only on `Enter`. On a successful load it primes the tracker after placing the player at the arrival marker. An arrival inside the return door therefore stays in the destination until the player leaves and re-enters the box.

On Windows, open `examples/garden-walk` with **File → Open Project…**, then choose **Build & Run**. With the compiler and SDL configured, you can also run `cmake --preset windows` and `cmake --build build-windows --config Debug --target game` from the example folder, then launch `build-windows/Debug/game.exe`. Move with arrows or WASD.

In the garden, press **Z** to plot a route from the player’s current cell to the fountain. Move and press **Z** again to search from the new position. In a test copy, change which tiles are marked `solid`, save, and relaunch: the same code routes around the edited obstacles.

**Tile overlay check:** In a copy of Garden Walk, choose **2D Viewport → View → Show Tile Property → solid**. Hide **Walls** in **Layers**: the wall art disappears but the amber tint stays. Show **Walls** again and select its name. In **Tilesets**, pick the gray brick tile (tile 3), enable **Paint**, and place two bricks immediately east of the player across the horizontal path. **Save All → Build & Run**: holding Right stops at the new wall. Stop the game, uncheck **solid** for tile 3, and save. The bricks remain but the tint disappears. On the next run, the player walks through the bricks with no C++ change.

**Camera check:** Select `camera` in Garden Walk’s **Entities** panel. The garden camera follows `@player`; the shed camera uses its own fixed center. The example calls `draw_from_camera(time, "camera")` explicitly in both scenes.

## Tower defense

Open [`examples/tower-defense`](../examples/tower-defense/) and choose **Build & Run**. Use the arrows or WASD to move the cursor, **Z** to build, and **X** to reset. Building the first tower starts a five-second countdown. Try the first three blue pads to repel the wave.

To edit a copy, use **View → Show Tile Property** to inspect `path` and `buildable`. The **Lane** layer needs one orthogonally connected, nonbranching route; center `spawn` and `goal` on its endpoints. On **Tower Pads**, erase one pad and paint another on empty ground. Save and relaunch the same executable from the project folder. It reads both changes from the saved scene data. A standalone build with its own `data/` copy needs that copy updated. A spare path tile connected to the middle of the lane produces a branch error rather than silently choosing a direction.

In `scenes/main.scene`, enable **2D Viewport → View → Validate Route**. Cyan arrows follow the exact chain the game will use. Erase an interior lane cell: the preview points to a broken end before you save. Undo to restore the arrows. Move a marker off the tile center to check endpoint diagnostics. The preview reads unsaved tile and entity edits and does not change wave or combat behavior.

Next: give another developer the [blank-project two-room handoff](TopDownHandoff.md). Use their first blocker to choose the next SDK or editor improvement.
