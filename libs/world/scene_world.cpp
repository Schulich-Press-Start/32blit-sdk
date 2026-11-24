#include "scene_text.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <limits>

namespace sdk {
  struct SceneAccess {
    static bool querying(const World &world) { return world.querying_; }
    static bool has_free_slot(const World &world) { return world.free_ != UINT16_MAX; }

    static uint32_t capacity_after_clear(const World &world) {
      uint32_t count = 0;
      for(auto i = world.free_; i != UINT16_MAX; i = world.slots_[i].next) ++count;
      for(uint16_t i = 0; i < world.capacity_; ++i) {
        const auto &slot = world.slots_[i];
        if(slot.alive && slot.generation != std::numeric_limits<uint32_t>::max()) ++count;
      }
      return count;
    }

    static bool room_for(const World &world, uint16_t type, uint32_t needed, bool replacing) {
      const auto *pool = world.pool_for(type);
      if(!pool || !pool->default_constructible()) return false;
      const uint32_t available = replacing ? pool->capacity() : pool->capacity() - pool->size();
      return needed <= available;
    }
  };

  namespace {
    using detail::scene_error;
    using detail::SceneOutput;

    bool object_error(const SceneDocument &doc, const SceneObject &object, uint32_t line,
                      SceneError &error, std::string_view message) {
      return scene_error(error, doc.sources()[object.source].path, line, message);
    }

    uint32_t find_object(const SceneDocument &doc, std::string_view key, bool prefab) {
      const auto &objects = doc.objects();
      for(size_t i = 0; i < objects.size(); ++i)
        if(objects[i].prefab == prefab && key == objects[i].key) return static_cast<uint32_t>(i);
      return scene_end;
    }

    const SceneObject *base_of(const SceneDocument &doc, const SceneObject &object) {
      if(doc.snapshot() || !*object.base) return nullptr;
      const uint32_t index = find_object(doc, object.base, true);
      return index == scene_end ? nullptr : &doc.objects()[index];
    }

    template<class F> void visit_components(const SceneDocument &doc, const SceneObject &object, F &&fn) {
      for(auto index = object.first_component; index != scene_end; index = doc.component(index).next)
        fn(doc.component(index));
    }

    template<class F> void visit_fields(const SceneDocument &doc, const SceneComponent &component, F &&fn) {
      for(auto index = component.first_field; index != scene_end; index = doc.field(index).next)
        fn(doc.field(index));
    }

    bool reference_is_live(const World &world, const SceneDocument &doc, const SceneObject &prefab,
                           const Entity *bindings, SceneError &error) {
      bool valid = true;
      visit_components(doc, prefab, [&](const SceneComponent &component) {
        const auto &type = *component_by_id(component.type);
        visit_fields(doc, component, [&](const SceneField &field) {
          if(valid && type.fields[field.field].type == FieldType::Entity && field.value[0] == '@') {
            const auto target = find_object(doc, field.value + 1, false);
            if(target == scene_end || !world.alive(bindings[target]))
              valid = object_error(doc, prefab, field.line, error, "prefab reference no longer has a live target");
          }
        });
      });
      return valid;
    }

    // Called only for validated records, on an entity with preflighted space in its pools.
    void apply_components(World &world, Entity entity, const SceneDocument &doc,
                          const SceneObject &object, const Entity *bindings) {
      visit_components(doc, object, [&](const SceneComponent &component) {
        auto *data = static_cast<char *>(world.add(entity, component.type));
        if(!data) std::terminate();
        const auto &type = *component_by_id(component.type);
        visit_fields(doc, component, [&](const SceneField &field) {
          const auto &info = type.fields[field.field];
          auto *dest = data + info.offset;
          if(info.type == FieldType::Entity) {
            Entity target;
            if(field.value[0] == '@') {
              const auto index = find_object(doc, field.value + 1, false);
              if(index == scene_end) std::terminate();
              target = bindings[index];
            }
            std::memcpy(dest, &target, sizeof(target));
          } else if(info.type == FieldType::String && !field.value[0]) {
            std::memset(dest, 0, string_field_size);
          } else if(!parse_value(info.type, field.value, dest)) {
            std::terminate();
          }
        });
      });
    }

    bool preflight_scene(World &world, const SceneDocument &doc, const Entity *bindings,
                         size_t binding_count, SceneError &error) {
      if(doc.sources().empty()) return scene_error(error, {}, 0, "no scene document");
      const std::string_view root = doc.sources()[0].path;
      if(doc.uses_external_metadata()) return scene_error(error, root, 0, "scene must be parsed with the runtime component registry");
      if(SceneAccess::querying(world)) return scene_error(error, root, 0, "cannot load during a query");
      if(binding_count < doc.objects().size() || (!bindings && !doc.objects().empty()))
        return scene_error(error, root, 0, "scene binding buffer too small");

      const uint32_t capacity = SceneAccess::capacity_after_clear(world);
      uint32_t entity_count = 0;
      uint32_t demand[max_components] = {};
      const auto *name_type = component_info<Name>();
      const auto &objects = doc.objects();
      for(size_t i = 0; i < objects.size(); ++i) {
        const auto &object = objects[i];
        if(object.prefab) continue;
        if(++entity_count > capacity) return object_error(doc, object, object.line, error, "entity pool is full");
        if(*object.name) {
          for(size_t prev = 0; prev < i; ++prev)
            if(!objects[prev].prefab && std::strcmp(objects[prev].name, object.name) == 0)
              return object_error(doc, object, object.line, error, "duplicate entity name");
          if(!name_type || !SceneAccess::room_for(world, name_type->id, ++demand[name_type->id], true))
            return object_error(doc, object, object.line, error, "missing or full Name pool");
        }
        std::bitset<max_components> seen;
        bool possible = true;
        auto require = [&](const SceneComponent &component) {
          if(!possible || seen.test(component.type)) return;
          seen.set(component.type);
          if(!SceneAccess::room_for(world, component.type, ++demand[component.type], true))
            possible = object_error(doc, object, object.line, error, "component pool unavailable or too small");
        };
        if(const auto *base = base_of(doc, object)) visit_components(doc, *base, require);
        visit_components(doc, object, require);
        if(!possible) return false;
      }
      return true;
    }

    bool write_field(SceneOutput &out, const World &world, const FieldInfo &field, const char *data) {
      out.append(" ");
      out.append(field.name);
      out.append("=");
      if(field.type == FieldType::Entity) {
        Entity reference;
        std::memcpy(&reference, data, sizeof(reference));
        if(world.alive(reference)) {
          out.append("@e");
          out.number(reference.index());
        } else {
          out.append("null");
        }
        return true;
      }
      if(field.type == FieldType::Float) {
        float value;
        std::memcpy(&value, data, sizeof(value));
        if(!std::isfinite(value)) return false;
      }
      char text[max_value_text];
      const int length = format_value(field.type, data, text, sizeof(text));
      const auto capacity = text_field_size(field.type);
      if(length < 0 || (capacity && length >= capacity)) return false;
      out.token({text, static_cast<size_t>(length)});
      return true;
    }
  }

  bool instantiate_scene(World &world, const SceneDocument &scene, Entity *bindings, size_t count, SceneError &error) {
    if(!preflight_scene(world, scene, bindings, count, error)) return false;

    // All field text and references were validated by the document reader/editor.
    // Capacity and type checks above make this commit allocation-free and infallible.
    for(uint16_t i = 0; i < world.capacity(); ++i)
      if(auto old = world.entity_at(i)) world.destroy(old);

    const auto &objects = scene.objects();
    for(size_t i = 0; i < objects.size(); ++i) {
      if(objects[i].prefab) { bindings[i] = {}; continue; }
      const auto entity = world.create();
      if(!entity) std::terminate();
      bindings[i] = entity;
      if(*objects[i].name) {
        auto *name = world.add<Name>(entity);
        if(!name) std::terminate();
        std::memcpy(name->value, objects[i].name, sizeof(name->value));
      }
    }
    for(size_t i = 0; i < objects.size(); ++i) {
      const auto &object = objects[i];
      if(object.prefab) continue;
      if(const auto *base = base_of(scene, object)) apply_components(world, bindings[i], scene, *base, bindings);
      apply_components(world, bindings[i], scene, object, bindings);
    }
    error = {};
    return true;
  }

  static int write_snapshot(const World &world, char *buf, int cap, const SceneLimits *limits,
                            std::string_view source, const char *const *keys) {
    SceneOutput out(buf, cap);
    if(limits && (limits->sources == 0 || world.size() > limits->objects)) return out.fail();
    out.append("snapshot 1 World");
    if(!source.empty()) {
      out.append(" source=");
      out.token(source);
    }
    out.append("\n");
    uint32_t components = 0, fields = 0;
    const uint16_t types = component_count();
    for(uint16_t i = 0; i < world.capacity(); ++i) {
      const auto entity = world.entity_at(i);
      if(!entity) continue;
      out.append("entity e");
      out.number(i);
      out.append(" name=\"\"");
      if(keys && keys[i]) {
        out.append(" bind=");
        out.append(keys[i]);
      }
      out.append("\n");
      for(uint16_t id = 0; id < types; ++id) {
        const auto *data = static_cast<const char *>(world.get(entity, id));
        if(!data) continue;
        const auto &info = *component_by_id(id);
        if(limits) {
          if(components == limits->components || info.field_count > limits->fields - fields) return out.fail();
          ++components;
          fields += info.field_count;
        }
        out.append("  ");
        out.append(info.name);
        for(uint8_t f = 0; f < info.field_count; ++f)
          if(!write_field(out, world, info.fields[f], data + info.fields[f].offset)) return out.fail();
        out.append("\n");
      }
    }
    return out.finish();
  }

  int save_world(const World &world, char *buf, int cap) {
    return write_snapshot(world, buf, cap, nullptr, {}, nullptr);
  }

  bool load_world(World &world, std::string_view text, SceneDocument &scratch,
                  Entity *bindings, size_t count, SceneError &error) {
    constexpr std::string_view path = "snapshot.scene";
    if(!read_scene(scratch, text, path, {}, error)) return false;
    if(!scratch.snapshot()) return scene_error(error, path, 1, "expected a runtime snapshot");
    return instantiate_scene(world, scratch, bindings, count, error);
  }

  SceneRuntime::SceneRuntime(World &world, SceneLimits limits, SceneReader reader)
    : world_(world), reader_(reader), active_(limits), staging_(limits), snapshot_(limits),
      bindings_(limits.objects), staging_bindings_(limits.objects), snapshot_keys_(world.capacity()) {}

  void SceneDocument::swap(SceneDocument &other) noexcept {
    std::swap(limits_, other.limits_);
    std::swap(title_, other.title_);
    std::swap(snapshot_source_, other.snapshot_source_);
    std::swap(snapshot_, other.snapshot_);
    std::swap(types_, other.types_);
    std::swap(type_count_, other.type_count_);
    sources_.swap(other.sources_);
    objects_.swap(other.objects_);
    components_.swap(other.components_);
    fields_.swap(other.fields_);
  }

  namespace {
    struct SceneReadAttempt {
      const SceneReader *reader;
      bool success = false;
      ~SceneReadAttempt() {
        if(reader && reader->finish) reader->finish(reader->context, success);
      }
    };
  }

  bool SceneRuntime::load(std::string_view path, SceneError &error) {
    SceneReadAttempt attempt{&reader_};
    if(!read_scene_file(staging_, path, reader_, error)) return false;
    if(staging_.snapshot()) return scene_error(error, path, 1, "expected an authored scene");
    if(reader_.prepare && !reader_.prepare(reader_.context, staging_, error)) return false;
    if(!instantiate_scene(world_, staging_, staging_bindings_.data(), staging_bindings_.size(), error)) return false;
    active_.swap(staging_);
    bindings_.swap(staging_bindings_);
    attempt.success = true;
    return true;
  }

  int SceneRuntime::save(char *buf, int cap) const {
    std::fill(snapshot_keys_.begin(), snapshot_keys_.end(), nullptr);
    for(size_t i = 0; i < active_.objects().size(); ++i)
      if(world_.alive(bindings_[i])) snapshot_keys_[bindings_[i].index()] = active_.objects()[i].key;
    const std::string_view source = active_.sources().empty() ? "" : active_.sources()[0].path;
    return write_snapshot(world_, buf, cap, &snapshot_.limits_, source, snapshot_keys_.data());
  }

  bool SceneRuntime::restore(std::string_view text, SceneError &error) {
    constexpr std::string_view path = "snapshot.scene";
    if(!read_scene(snapshot_, text, path, {}, error)) return false;
    if(!snapshot_.snapshot()) return scene_error(error, path, 1, "expected a runtime snapshot");

    const auto source = snapshot_.snapshot_source();
    const bool new_catalogue = !source.empty() && (active_.sources().empty() || source != active_.sources()[0].path);
    SceneReadAttempt attempt{new_catalogue ? &reader_ : nullptr};
    if(new_catalogue) {
      if(!read_scene_file(staging_, source, reader_, error)) return false;
      if(staging_.snapshot()) return scene_error(error, source, 1, "expected an authored scene");
      if(reader_.prepare && !reader_.prepare(reader_.context, staging_, error)) return false;
    }
    if(!instantiate_scene(world_, snapshot_, staging_bindings_.data(), staging_bindings_.size(), error)) return false;
    if(new_catalogue) active_.swap(staging_);

    std::fill(bindings_.begin(), bindings_.end(), Entity{});
    if(!source.empty()) {
      for(size_t i = 0; i < snapshot_.objects().size(); ++i) {
        const auto key = snapshot_.objects()[i].base;
        if(!*key) continue;
        const auto index = find_object(active_, key, false);
        if(index != scene_end) bindings_[index] = staging_bindings_[i];
      }
    }
    attempt.success = true;
    return true;
  }

  Entity SceneRuntime::spawn(std::string_view prefab, float x, float y, SceneError &error) {
    const auto index = find_object(active_, prefab, true);
    if(index == scene_end) {
      scene_error(error, active_.sources().empty() ? "" : active_.sources()[0].path, 0, "unknown prefab");
      return {};
    }
    const auto &object = active_.objects()[index];
    if(!std::isfinite(x) || !std::isfinite(y)) {
      object_error(active_, object, object.line, error, "spawn coordinates must be finite");
      return {};
    }
    if(SceneAccess::querying(world_) || !SceneAccess::has_free_slot(world_)) {
      object_error(active_, object, object.line, error, "cannot create entity");
      return {};
    }
    const auto *transform_type = component_info<Transform2D>();
    bool possible = transform_type && SceneAccess::room_for(world_, transform_type->id, 1, false);
    visit_components(active_, object, [&](const SceneComponent &component) {
      if(possible && component.type != transform_type->id)
        possible = SceneAccess::room_for(world_, component.type, 1, false);
    });
    if(!possible) {
      object_error(active_, object, object.line, error, "prefab component pool unavailable or full");
      return {};
    }
    if(!reference_is_live(world_, active_, object, bindings_.data(), error)) return {};

    const auto entity = world_.create();
    if(!entity) std::terminate();
    apply_components(world_, entity, active_, object, bindings_.data());
    auto *transform = world_.add<Transform2D>(entity);
    if(!transform) std::terminate();
    transform->x = x;
    transform->y = y;
    error = {};
    return entity;
  }
}
