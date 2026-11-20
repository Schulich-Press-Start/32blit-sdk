#pragma once

#include "scene.hpp"

namespace sdk {
  enum class ActionType : uint8_t { Set, Destroy, AddComponent, RemoveComponent, Spawn, LoadScene };

  struct Action {
    ActionType type = ActionType::Set;
    char target[scene_path_size] = {};  ///< Entity name, prefab key, or project-relative scene path.
    char path[48] = {};  ///< Component.Field for set; Component for add/remove; x for spawn.
    char value[48] = {};  ///< Value for set; y for spawn.
  };

  struct Result {
    bool ok;
    char message[64];
    char file[scene_path_size] = {};
    uint32_t line = 0;
  };

  /// Parse set, destroy, add, remove, spawn or load_scene. No heap or World access.
  /// On failure, err describes the problem and out is unchanged.
  bool parse_console(const char *line, Action &out, Result &err);

  /// Apply a command to the live World. Failures leave it unchanged.
  /// Spawn and load_scene require a SceneRuntime bound to this World.
  Result apply(World &world, const Action &action, SceneRuntime *scenes = nullptr);

  /// Write a canonical command or a C++ statement using the variable `world`.
  /// C++ for spawn/load_scene also references a SceneRuntime variable named `scenes`.
  /// Returns length (excluding NUL), or -1 on invalid input/insufficient space.
  /// A writable buffer is set to an empty string on failure.
  int to_console(const Action &action, char *buf, int cap);
  int to_cpp(const Action &action, char *buf, int cap);

  /// Write an entity-body fragment for set/add, quoting values as needed.
  /// For spawn, write an entity using the matching prefab and its key as a candidate
  /// stable key; callers must change that key if it collides in the destination.
  /// Same buffer contract; returns -1 for header-only Name edits, destroy, remove and load_scene.
  int to_scene(const Action &action, char *buf, int cap);
}
