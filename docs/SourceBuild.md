# Building a scene game

See the [scene-workshop build instructions](scene-workshop/Building.md) for both Visual Studio Build Tools and MSYS2 UCRT64. The Windows kit includes the SDK; no private repository access is required. Use **Play Scene** for built-in components without compiling and use your compiled executable for custom components and game rules.

The editor generates the project from `templates/scene-game/`. Build the `game` target while developing. For distribution, install the Release build and share its executable, matching DLLs and `data` folder together.
