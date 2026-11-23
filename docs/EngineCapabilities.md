# General 2D engine capability checklist

Build reusable primitives, not a module for each reference game. The engine supplies mechanisms; game code supplies rules and chooses when to use them. A checked item means the basic capability exists, not that it matches every feature in a larger engine. See [the editor roadmap](EditorRoadmap.md) and [the reference-game audit](TopDownHandoff.md#what-the-current-modules-cover).

## Available foundations

- [x] Scene authoring and loading: named entities, prefabs, overrides, reflected fields, runtime spawning, and named arrivals on scene travel.
- [x] 2D rendering: tilemaps, sprite clips, shared tile/sprite render order, and fixed or following cameras. HUD drawing is screen-space.
- [x] Spatial primitives: transforms, axis-aligned collider boxes, overlap queries, and authored tile properties.
- [x] Grid primitives: tile queries and single-lane validation. A single lane is not general navigation.
- [x] Code-level input, text and basic menus, timers, audio playback, and save slots.

## Track the gaps

- [x] **2D movement against solid tiles.** `move_against_tiles` sweeps a `BoxCollider2D` along X then Y, slides along the unblocked axis, and returns actual displacement and blocked axes. Garden Walk and the existing platformer use the same solver. The games still own input, acceleration, gravity, ladders, water, and jump policy.
- [x] **Contact lifecycle.** `OverlapTracker` samples one actor’s overlaps and returns enter, stay, and exit events without allocating per tick. Garden Walk uses it to detect door entry. A collider remains data; doors, pickups, and damage remain game rules.
- [x] **Collision categories and first-hit sweeps.** Named categories label entity boxes; each query supplies its own mask. `GameScene::sweep` finds the first eligible entity or solid tile across the requested travel. No automatic tick or all-pairs pass. Garden Walk opts into a door mask; actor sweeps use current target positions.
- [ ] **Broader collision shapes and surfaces, as needed.** Use overlapping gameplay examples to drive one-way surfaces or moving-solid support; don't promise a rigid-body solver when kinematic movement suffices.
- [x] **Grid navigation.** `GridNavigation` finds shortest four-direction paths on a uniform-cost grid. The caller chooses the goal and walkability; Garden Walk can trace a path to the fountain around authored `solid` tiles. Separate from the single-lane validator.
- [x] **Input actions and bindings.** Native SDL games can bind named actions to keyboard scancodes and gamepad buttons. Garden Walk and the platformer read project bindings at launch; games still decide what each action does. Raw `blit::buttons` remains available.
- [x] **Reusable UI and focus.** `.ui.json` layouts provide editable screen-space panels, labels, and buttons. `UiScreen` draws them and manages directional focus; Garden Walk uses it for its keyboard and controller pause menu. The game owns screen transitions and button behavior.
- [x] **Authored audio cues.** Assign and preview WAVs in the editor. Garden Walk plays named focus and confirm cues through native SDL; the game decides when to play them.
- [x] **Save integration.** Bounded reads and staged replacements use the HAL save directory. Garden Walk saves stable checkpoint IDs and restores authored arrivals across scene loads and restarts.

**Suggested next:** Reassess the reference games before adding more systems. Keep additional collision shapes and surfaces demand-driven. STM32 is an efficiency reference, not a support or benchmarking requirement.

## Reassessment gate

Ask the original question again: **If we wanted to reimplement the reference games in Godot or Unity, what would the engine already provide, what would we still write in game code, and does our SDK/editor now provide those same general building blocks?** Review at least two genres with actual projects; separate missing mechanisms from game rules and editor convenience.
