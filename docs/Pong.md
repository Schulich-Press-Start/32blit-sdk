# Make a scene, then make Pong

Start with **File → New Project…**. The first success is two coins animating over a layered map without touching C++. Then use the same SDK runtime to write Pong's rules, one behavior at a time.

1. **[Author the scene](scene-workshop/Authoring.md):** import PNGs, define clips, drag sprites into the scene, paint two map layers, and place the paddles and ball. Use **Play Scene**; no compiler needed.
2. **[Build and share](scene-workshop/Building.md):** build the generated `game.cpp` with MSVC or MSYS2 UCRT64 and launch the output without the editor. No Python, `assets.yml`, or Pong-specific runner is needed to build a scene game.
3. **[Write the gameplay](scene-workshop/Gameplay.md):** seven cumulative C++ checkpoints, from clearing the window to scoring and resetting a match. Use **Build & Run** or launch your compiler's output. The last lesson adds an editable game-specific component.

[Source artwork](scene-workshop/Import%20These%20PNGs/) · [Checkpoint files](scene-workshop/Checkpoints/)
