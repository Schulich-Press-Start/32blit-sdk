# 32blit Windows playtest kit

Extract the entire ZIP to a writable folder. Run the `.cmd` launchers from the extracted kit, not from the ZIP viewer. The kit targets Windows x64 and does not need CMake or Python to edit projects or run the supplied games.

| Launcher | What to try |
|---|---|
| `Edit Pong.cmd` | Open the completed Pong reference: explore entities, prefab defaults, colliders, sprite clips, the court, and source files. |
| `Play Pong.cmd` | Run the included Pong build. |
| `Edit Platformer.cmd` | Explore a larger project. Edit solid tiles, the player’s box, or sprite clips. Use **File → Save All** before playtesting. |
| `Play Platformer.cmd` | Move with arrows or WASD; jump with Z. |

Follow the illustrated [Pong tutorial](Pong.md) to build a game from the blank `pong-starter` project. The scratch tutorial needs the SDK checkout and Windows build tools; the completed reference runs immediately from this kit.

The launchers and editor **Play** run the bundled Windows games from the correct project folders. Restart the game to see saved scene, map, and sprite edits. The prebuilt games do not compile C++ or repack PNGs. To rebuild, use the source-build workflow in the tutorial.

In **Documents**, **Ctrl+S** saves the focused asset. If you need to share an error, select and copy it from **Tools → Build / Game Log**. **Project Files** offers right-click copy, paste, and confirmed delete. Tilesets can use different square cell sizes; sprites can use rectangular frames that fit their sheet exactly.

Report the project, reproduction steps, expected and actual behavior, and whether the issue is in the editor or the running game. Include a screenshot or relevant log and the revisions listed in the kit’s `revisions.txt`.
