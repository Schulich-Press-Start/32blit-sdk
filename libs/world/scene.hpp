#pragma once

#include "world.hpp"

#include <string_view>
#include <vector>

namespace sdk {
  constexpr size_t scene_key_size = 64;
  constexpr size_t scene_path_size = 96;
  constexpr uint32_t scene_end = UINT32_MAX;

  struct SceneLimits {
    uint32_t objects = 256, components = 512, fields = 1024, sources = 16;
  };

  struct SceneError {
    char file[scene_path_size] = {};
    uint32_t line = 0;
    char message[96] = {};
  };

  /// Return text for a project-relative path. The parser consumes it before
  /// returning; every returned view must remain valid until that call ends.
  struct SceneReader {
    void *context = nullptr;
    bool (*read)(void *context, std::string_view path, std::string_view &text) = nullptr;
    /// SceneRuntime calls this after loading: publish staged assets only on success.
    void (*finish)(void *context, bool success) noexcept = nullptr;
  };

  struct SceneSource { char path[scene_path_size]; uint32_t parent; };
  struct SceneObject {
    // `base` is a prefab key in scenes and a binding key in snapshots.
    char key[scene_key_size], base[scene_key_size], name[string_field_size];
    uint32_t source, line, first_component = scene_end;
    bool prefab;
  };
  struct SceneComponent { uint16_t type; uint32_t line, first_field = scene_end, next = scene_end; };
  struct SceneField { uint8_t field; char value[scene_key_size + 1]; uint32_t line, next = scene_end; };

  /// Reserves its budgets at construction. Reading and editing use only that space.
  class SceneDocument {
  public:
    explicit SceneDocument(SceneLimits limits);
    SceneDocument(const SceneDocument &) = delete;
    SceneDocument &operator=(const SceneDocument &) = delete;

    std::string_view title() const { return title_; }
    bool snapshot() const { return snapshot_; }
    std::string_view snapshot_source() const { return snapshot_source_; }
    const std::vector<SceneSource> &sources() const { return sources_; }
    const std::vector<SceneObject> &objects() const { return objects_; }
    const SceneComponent &component(uint32_t index) const { return components_[index]; }
    const SceneField &field(uint32_t index) const { return fields_[index]; }

  private:
    friend struct SceneParser;
    friend class SceneRuntime;
    friend bool set_scene_field(SceneDocument &, std::string_view, std::string_view, std::string_view,
                                std::string_view, SceneError &);
    friend bool rename_scene_entity(SceneDocument &, std::string_view, std::string_view, SceneError &);
    void clear();
    void swap(SceneDocument &other) noexcept;
    SceneLimits limits_;
    char title_[scene_key_size] = {};
    char snapshot_source_[scene_path_size] = {};
    bool snapshot_ = false;
    std::vector<SceneSource> sources_;
    std::vector<SceneObject> objects_;
    std::vector<SceneComponent> components_;
    std::vector<SceneField> fields_;
  };

  /// Read a version-1 scene or snapshot. Failure leaves `out` empty.
  bool read_scene(SceneDocument &out, std::string_view text, std::string_view path,
                  SceneReader reader, SceneError &error);
  /// Validate a project-relative path before loading the root through reader.
  bool read_scene_file(SceneDocument &out, std::string_view path, SceneReader reader, SceneError &error);
  /// Write the authored root only (keeps include directives, not included data).
  int write_scene(const SceneDocument &scene, char *buf, int cap);
  /// Edit a root entity or prefab by stable key (includes are read-only).
  bool set_scene_field(SceneDocument &scene, std::string_view key, std::string_view component,
                       std::string_view field, std::string_view value, SceneError &error);
  bool rename_scene_entity(SceneDocument &scene, std::string_view key, std::string_view name, SceneError &error);

  /// Atomically replace entities using the supplied document. `bindings` has
  /// at least one entry per SceneObject; prefab entries receive invalid handles.
  bool instantiate_scene(World &world, const SceneDocument &scene, Entity *bindings, size_t count, SceneError &error);

  /// Runtime snapshots carry a distinct header. They contain registered state,
  /// never includes/prefabs, and remap references instead of saving raw handles.
  int save_world(const World &world, char *buf, int cap);
  bool load_world(World &world, std::string_view text, SceneDocument &scratch,
                  Entity *bindings, size_t count, SceneError &error);

  /// Owns the preallocated loading workspace and the active prefab catalogue.
  /// The bound World must outlive this object.
  class SceneRuntime {
  public:
    SceneRuntime(World &world, SceneLimits limits, SceneReader reader);
    World &world() const { return world_; }
    bool load(std::string_view path, SceneError &error);
    Entity spawn(std::string_view prefab, float x, float y, SceneError &error);
    int save(char *buf, int cap) const;
    bool restore(std::string_view text, SceneError &error);

  private:
    World &world_;
    SceneReader reader_;
    SceneDocument active_, staging_, snapshot_;
    std::vector<Entity> bindings_, staging_bindings_;
    mutable std::vector<const char *> snapshot_keys_;
  };
}
