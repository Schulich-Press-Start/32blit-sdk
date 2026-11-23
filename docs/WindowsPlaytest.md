# Legacy 32blit Windows playtest kit

These launchers belong to the earlier multi-game playtest ZIP. For the **Scene Workshop** ZIP, open `START HERE.html` and use [the scene-first tutorial](Pong.md); no `.cmd` launcher is needed.

Extract the entire ZIP to a writable folder. Run the `.cmd` launchers from the extracted kit, not from the ZIP viewer. The kit targets Windows x64 and does not need CMake or Python to edit projects or run the supplied games.

| Launcher | What to try |
|---|---|
| `Build Pong in Editor.cmd` | Start the earlier kit's blank Pong scene. Use the guide bundled with that version. |
| `Edit Pong.cmd` | Inspect entities, prefab defaults, colliders, sprite clips, and the court in the legacy kit. |
| `Play Pong.cmd` | Run the included Pong build. |
| `Edit Platformer.cmd` | Explore a larger project. Edit solid tiles, the player’s box, or sprite clips. Use **File → Save All** before playtesting. |
| `Play Platformer.cmd` | Move with arrows or WASD; jump with Z. |

The [current tutorial](Pong.md) creates a generic buildable project, starting with animated coins before adding Pong rules.

Going beyond Pong? Read [SDK coding conventions](SDKConventions.md) for game code patterns and the [Editor Guide](../editor/README.md) for general authoring controls.

The launchers and editor **Play** run the bundled Windows games from the correct project folders. Restart the game to see saved scene, map, and sprite edits. The prebuilt games do not compile C++ or repack PNGs. To rebuild, use the source-build workflow in the tutorial.

## Optional: build and live-reload with Dev

For the tested Windows source build, install **Visual Studio or Build Tools with Desktop development with C++**, **CMake**, and **Python**. Use the SDK revision listed in `revisions.txt`. MSYS2 is not required to run the kit, and MSYS2-only source builds are not verified for this handoff.

From PowerShell in the extracted kit root, with your SDK checkout path substituted:

```powershell
$sdk = 'C:\path\to\32blit-sdk'
py -m pip install '32blit==0.7.4' 'bitstring<5' -e "$sdk\tools\blit-tools"
& '.\Configure Dev.cmd' team-pong $sdk
```

Make sure `cmake` and `blit` are on `PATH`. Use `32blit-platformer` instead of `team-pong` to configure Platformer. Once configuration succeeds, click **Dev** in that project's editor. The script passes `32BLIT_DIR` and uses the build folder expected by the project. If CMake reports an incompatible cache, use a fresh extraction instead of copying generated build files between machines. **Play** is unchanged and requires none of this.

## Editing and feedback

If you open `Editor/32blit-editor.exe` directly, choose **File → Open Project…** and select the folder containing `blit-project.json`, not the kit's outer folder. Save or discard edits and stop the running game before switching projects.

In **Documents**, **Ctrl+S** saves the focused asset. If you need to share an error, select and copy it from **Tools → Build / Game Log**. **Project Files** offers right-click copy, paste, and confirmed delete. Tilesets can use different square cell sizes; sprites can use rectangular frames that fit their sheet exactly.

Report the project, reproduction steps, expected and actual behavior, and whether the issue is in the editor or the running game. Include a screenshot or relevant log and the revisions listed in the kit’s `revisions.txt`.
