# 32blit live inspector

Optional Milestone B desktop tool. It connects to a native SDL Debug game over the same loopback bridge as `blit console`. Game builds do not depend on it. Tiled remains the map editor.

From the SDK root on Windows:

```powershell
cmake -S editor -B build-editor "-DSDL2_DIR=$PWD/vs/sdl/SDL2-2.28.5/cmake"
cmake --build build-editor --config Release
.\build-editor\Release\32blit-editor.exe --connect 127.0.0.1:6132
```

On Linux, use the same configure and build commands without `SDL2_DIR` if SDL2 development files are installed, then run `./build-editor/32blit-editor`. CMake downloads pinned ImGui and JSON sources for this tool only.

Start the game with `blit dev` or pass `--bridge 127.0.0.1:6132` to a Debug game. Select a named entity, pick a component, edit a supported field, and click **Apply**. The inspector also has a command box and pause/resume controls. Refresh with **F5**. Edits are live only; they do not rewrite `.scene`, `.tmx`, or C++ files. The game window remains separate.
