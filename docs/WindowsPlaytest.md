# 32blit Windows playtest kit

Extract the entire ZIP to a writable folder. Run the `.cmd` launchers from the extracted kit, not from the ZIP viewer. The kit targets Windows x64 and does not need CMake or Python to edit projects or run the supplied games.

| Launcher | What to try |
|---|---|
| `Edit Pong.cmd` | Follow [the Pong tutorial](Pong.md): move scene entities, edit prefab defaults and a collider, preview a sprite clip, paint the court, and save a source file in Documents. |
| `Play Pong.cmd` | Run the included Pong build. |
| `Edit Platformer.cmd` | Explore a larger project. Edit solid tiles, the player’s box, or sprite clips. Use **File → Save All** before playtesting. |
| `Play Platformer.cmd` | Move with arrows or WASD; jump with Z. |

The game launchers run from the correct project folders so the executables can find the saved assets. Restart a game to see saved scene, map, and supported sprite descriptor edits. The prebuilt games do not compile C++ or repack PNGs. Source changes, new packed artwork, and new component types need the development tools and the pinned SDK source. See the tutorial for the source-build workflow; the in-editor **Play** and **Dev** commands are for that setup.

In the editor, **Documents** shows line numbers, the active line, and the caret’s line and column. **Ctrl+S** saves the focused asset; **Ctrl+Shift+S** saves all. Text edits do not reconfigure the build or refresh component metadata until you rebuild. Tile cells remain 8 × 8; sprite frames can span multiple 8 × 8 cells. Use only project copies for experiments you want to discard.

Report the project, reproduction steps, expected and actual behavior, and whether the issue is in the editor or the running game. Include a screenshot or relevant log and the revisions listed in the kit’s `revisions.txt`.
