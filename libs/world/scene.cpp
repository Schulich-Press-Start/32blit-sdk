#include "scene_text.hpp"

#include <cstdio>

namespace sdk {
  namespace {
    using detail::scene_copy;
    using detail::scene_error;

    bool space(char c) { return c == ' ' || c == '\t'; }

    bool valid_key(std::string_view key) {
      return key.size() < scene_key_size && detail::scene_bare_token(key);
    }

    bool valid_path(std::string_view path) {
      return path.size() < scene_path_size && valid_project_relative_path(path);
    }

    class SceneLine {
    public:
      explicit SceneLine(std::string_view text) : text_(text) {}

      bool boundary() const { return text_.empty() || space(text_.front()) || text_.front() == '#'; }

      bool done() {
        skip_space();
        return text_.empty() || text_.front() == '#';
      }

      bool take(char punctuation) {
        skip_space();
        if(text_.empty() || text_.front() != punctuation) return false;
        text_.remove_prefix(1);
        return true;
      }

      std::string_view word() {
        skip_space();
        size_t size = 0;
        while(size < text_.size() && detail::scene_bare_character(text_[size])) ++size;
        const auto word = text_.substr(0, size);
        text_.remove_prefix(size);
        return word;
      }

      template<size_t N> bool token(char (&out)[N], bool *overflow = nullptr) {
        if(!take('"')) {
          const auto bare = word();
          if(bare.empty() || !boundary()) return false;
          if(overflow && bare.size() >= N) *overflow = true;
          return scene_copy(out, N, bare);
        }
        size_t size = 0;
        bool too_long = false;
        while(!text_.empty()) {
          unsigned char c = text_.front();
          text_.remove_prefix(1);
          if(c == '"') {
            out[size] = '\0';
            const bool ended = boundary();
            if(overflow) *overflow = ended && too_long;
            return ended && !too_long;
          }
          if(c == '\\') {
            if(text_.empty()) return false;
            c = text_.front();
            text_.remove_prefix(1);
            if(c != '"' && c != '\\') {
              if(c < '0' || c > '3' || text_.size() < 2
                 || text_[0] < '0' || text_[0] > '7' || text_[1] < '0' || text_[1] > '7') return false;
              c = static_cast<unsigned char>(((c - '0') << 6) | ((text_[0] - '0') << 3) | (text_[1] - '0'));
              text_.remove_prefix(2);
            }
          }
          if(c == 0) return false;
          if(size == N - 1) {
            if(!overflow) return false;
            too_long = true;
          } else out[size++] = static_cast<char>(c);
        }
        return false;
      }

    private:
      void skip_space() {
        while(!text_.empty() && space(text_.front())) text_.remove_prefix(1);
      }

      std::string_view text_;
    };

    const ComponentInfo *scene_component(std::string_view name) {
      for(uint16_t i = 0; i < component_count(); ++i) {
        const auto *info = component_by_id(i);
        if(name == info->name) return info;
      }
      return nullptr;
    }

    uint8_t scene_field(const ComponentInfo &component, std::string_view name) {
      uint8_t field = 0;
      for(; field < component.field_count; ++field)
        if(name == component.fields[field].name) break;
      return field;
    }

    bool valid_value(FieldType type, const char *value) {
      if(type == FieldType::Entity)
        return std::string_view(value) == "null" || (*value == '@' && valid_key(value + 1));
      if(const auto capacity = text_field_size(type)) {
        const std::string_view text(value);
        return text.size() < static_cast<size_t>(capacity) &&
               (type != FieldType::Path || text.empty() || valid_project_relative_path(text));
      }
      unsigned char parsed[string_field_size];
      return parse_value(type, value, parsed);
    }

    void path_field_diagnostic(char *message, size_t capacity, const ComponentInfo &type,
                               const FieldInfo &field, bool too_long) {
      if(too_long)
        std::snprintf(message, capacity, "%s.%s path too long (max %d UTF-8 bytes)",
                      type.name, field.name, path_field_size - 1);
      else
        std::snprintf(message, capacity, "%s.%s: expected a project-relative path using /",
                      type.name, field.name);
    }

    template<class T> bool full(const std::vector<T> &records, uint32_t limit) {
      return records.size() == limit || records.size() == records.capacity();
    }

    template<class T, class Key>
    uint32_t &record_link(std::vector<T> &records, uint32_t &first, Key T::*member, Key key) {
      auto *link = &first;
      while(*link != scene_end && records[*link].*member != key) link = &records[*link].next;
      return *link;
    }

    // Scene keys identify entities and prefabs in separate namespaces.
    uint32_t object_index(const SceneDocument &doc, std::string_view key, bool prefab) {
      const auto &objects = doc.objects();
      for(uint32_t i = 0; i < objects.size(); ++i)
        if(objects[i].prefab == prefab && key == objects[i].key) return i;
      return scene_end;
    }

    bool resolved_value(const SceneDocument &doc, FieldType type, const char *value) {
      return type != FieldType::Entity || *value != '@' || object_index(doc, value + 1, false) != scene_end;
    }
  }

  SceneDocument::SceneDocument(SceneLimits limits) : limits_(limits) {
    sources_.reserve(limits.sources);
    objects_.reserve(limits.objects);
    components_.reserve(limits.components);
    fields_.reserve(limits.fields);
  }

  void SceneDocument::clear() {
    title_[0] = '\0';
    snapshot_source_[0] = '\0';
    snapshot_ = false;
    types_ = nullptr;
    type_count_ = 0;
    sources_.clear();
    objects_.clear();
    components_.clear();
    fields_.clear();
  }

  struct SceneParser {
    SceneDocument &doc;
    SceneReader reader;
    SceneError &error;
    const ComponentInfo *types = nullptr;
    uint16_t type_count = 0;

    bool read(std::string_view text, std::string_view path, bool from_reader) {
      error = {};
      doc.clear();
      if(!valid_path(path)) return scene_error(error, path, 0, "invalid project-relative scene path");
      if(types && !valid_types()) return scene_error(error, path, 0, "invalid scene component metadata");
      SceneSource root{{}, scene_end};
      scene_copy(root.path, sizeof(root.path), path);
      if(full(doc.sources_, doc.limits_.sources)) return scene_error(error, root.path, 0, "scene source limit exceeded");
      doc.sources_.push_back(root);
      if((!from_reader || fetch(0, text, 0, 0)) && parse_source(text, 0) && validate_references()) {
        if(!types || !doc.snapshot_) {
          doc.types_ = types;
          doc.type_count_ = type_count;
          return true;
        }
        fail(0, 1, "expected an authored scene");
      }
      doc.clear();
      return false;
    }

  private:
    bool valid_types() const {
      if(type_count > max_components || (!types && type_count)) return false;
      for(uint16_t i = 0; i < type_count; ++i) {
        const auto &type = types[i];
        if(type.id != i || !type.name || type.field_count > max_component_fields ||
           (!type.fields && type.field_count)) return false;
        for(uint8_t j = 0; j < type.field_count; ++j) if(!type.fields[j].name) return false;
      }
      return true;
    }

    bool fail(uint32_t source, uint32_t line, std::string_view message) {
      return scene_error(error, doc.sources_[source].path, line, message);
    }

    bool fetch(uint32_t source, std::string_view &text, uint32_t from, uint32_t line) {
      if(reader.read && reader.read(reader.context, doc.sources_[source].path, text)) return true;
      return fail(from, line, line == 0 ? "cannot read scene" : "cannot read scene include");
    }

    bool parse_source(std::string_view text, uint32_t source) {
      uint32_t line = 0, object = scene_end;
      bool header = false;
      while(!text.empty()) {
        if(line == UINT32_MAX) return fail(source, line, "too many scene lines");
        ++line;
        const auto newline = text.find('\n');
        auto raw = text.substr(0, newline);
        text.remove_prefix(newline == text.npos ? text.size() : newline + 1);
        if(!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        const bool indented = !raw.empty() && space(raw.front());
        SceneLine input(raw);
        if(input.done()) continue;
        if(!header) {
          if(indented) return fail(source, line, "expected an unindented scene header");
          if(!parse_header(input, source, line)) return false;
          header = true;
        } else if(indented) {
          if(object == scene_end) return fail(source, line, "component outside an entity or prefab");
          if(!parse_component(input, doc.objects_[object], line)) return false;
        } else {
          const auto kind = input.word();
          if(kind == "include") {
            uint32_t child;
            if(!parse_include(input, source, line, child)) return false;
            std::string_view contents;
            if(!fetch(child, contents, source, line) || !parse_source(contents, child)) return false;
            object = scene_end;
          } else if(kind == "entity" || kind == "prefab") {
            if(!parse_object(input, source, line, kind == "prefab", object)) return false;
          } else {
            return fail(source, line, "expected include, prefab or entity");
          }
        }
      }
      return header || fail(source, line ? line : 1, "missing scene header");
    }

    bool parse_header(SceneLine &input, uint32_t source, uint32_t line) {
      const auto kind = input.word();
      const bool snapshot = source == 0 && kind == "snapshot";
      if(kind != "scene" && !snapshot) return fail(source, line, "missing scene header");
      if(input.word() != "1") return fail(source, line, "unsupported scene version (expected 1)");
      char title[scene_key_size];
      if(!input.boundary() || !input.token(title) || !*title)
        return fail(source, line, "invalid scene title");
      if(snapshot && !input.done()) {
        if(input.word() != "source" || !input.take('=') || !input.token(doc.snapshot_source_) ||
           !valid_path(doc.snapshot_source_)) return fail(source, line, "invalid snapshot source");
      }
      if(!input.done()) return fail(source, line, "unexpected scene header token");
      if(source == 0) {
        scene_copy(doc.title_, sizeof(doc.title_), title);
        doc.snapshot_ = snapshot;
      }
      return true;
    }

    bool parse_include(SceneLine &input, uint32_t source, uint32_t line, uint32_t &child) {
      if(doc.snapshot_) return fail(source, line, "snapshots cannot include scenes");
      SceneSource included{{}, source};
      if(!input.boundary() || !input.token(included.path) || !input.done() || !valid_path(included.path))
        return fail(source, line, "invalid project-relative include path");
      const std::string_view path(included.path);
      for(uint32_t i = 0; i < doc.sources_.size(); ++i) {
        if(path != doc.sources_[i].path) continue;
        for(auto parent = source; parent != scene_end; parent = doc.sources_[parent].parent)
          if(parent == i) return fail(source, line, "scene include cycle");
        return fail(source, line, "scene already included");
      }
      if(full(doc.sources_, doc.limits_.sources)) return fail(source, line, "scene source limit exceeded");
      child = static_cast<uint32_t>(doc.sources_.size());
      doc.sources_.push_back(included);
      return true;
    }

    bool parse_object(SceneLine &input, uint32_t source, uint32_t line, bool prefab, uint32_t &index) {
      if(prefab && doc.snapshot_) return fail(source, line, "snapshots cannot contain prefabs");
      const auto key = input.word();
      if(!valid_key(key)) return fail(source, line, "invalid scene object key");
      if(object_index(doc, key, prefab) != scene_end)
        return fail(source, line, prefab ? "duplicate prefab key" : "duplicate entity key");
      SceneObject object{};
      scene_copy(object.key, sizeof(object.key), key);
      if(!prefab) {
        if(input.take(':')) {
          if(doc.snapshot_) return fail(source, line, "snapshot entities cannot inherit from prefabs");
          const auto base = input.word();
          if(!valid_key(base)) return fail(source, line, "invalid prefab key");
          scene_copy(object.base, sizeof(object.base), base);
        }
        if(input.done()) {
          if(doc.snapshot_) return fail(source, line, "snapshot entity headers require name=\"\"");
          if(!scene_copy(object.name, sizeof(object.name), key))
            return fail(source, line, "entity key does not fit the default name; use name=");
        } else if(input.word() != "name" || !input.take('=') || !input.token(object.name)) {
          return fail(source, line, "invalid entity name (expected name=token)");
        }
        if(doc.snapshot_ && *object.name) return fail(source, line, "snapshot entity header names must be empty");
        if(doc.snapshot_ && !input.done()) {
          const auto directive = input.word();
          if(directive != "bind" || !input.take('=')) return fail(source, line, "expected snapshot bind=key");
          const auto binding = input.word();
          if(!valid_key(binding)) return fail(source, line, "invalid snapshot binding key");
          for(const auto &prior : doc.objects_)
            if(binding == prior.base) return fail(source, line, "duplicate snapshot binding key");
          scene_copy(object.base, sizeof(object.base), binding);
        }
      }
      if(!input.done()) return fail(source, line, prefab ? "prefab headers accept only a key" : "unexpected entity header token");
      if(full(doc.objects_, doc.limits_.objects)) return fail(source, line, "scene object limit exceeded");
      object.source = source;
      object.line = line;
      object.prefab = prefab;
      index = static_cast<uint32_t>(doc.objects_.size());
      doc.objects_.push_back(object);
      return true;
    }

    bool parse_component(SceneLine &input, SceneObject &object, uint32_t line) {
      const auto name = input.word();
      if(name == "Name" && !doc.snapshot_) return fail(object.source, line, "use name= on the entity header instead of Name");
      const auto *info = types ? nullptr : scene_component(name);
      if(types) for(uint16_t i = 0; i < type_count; ++i) {
        if(name == types[i].name) { info = types + i; break; }
      }
      if(!info) return fail(object.source, line, "unknown scene component");
      auto &component = record_link(doc.components_, object.first_component, &SceneComponent::type, info->id);
      if(component != scene_end) return fail(object.source, line, "duplicate scene component");
      if(full(doc.components_, doc.limits_.components)) return fail(object.source, line, "scene component limit exceeded");
      component = static_cast<uint32_t>(doc.components_.size());
      doc.components_.push_back({info->id, line});
      auto *next = &doc.components_.back().first_field;
      uint32_t seen = 0;
      while(!input.done()) {
        SceneField field{};
        field.field = scene_field(*info, input.word());
        if(field.field == info->field_count) return fail(object.source, line, "unknown scene field");
        const uint32_t bit = uint32_t{1} << field.field;
        if(seen & bit) return fail(object.source, line, "duplicate scene field");
        seen |= bit;
        const auto &metadata = info->fields[field.field];
        bool overflow = false;
        const bool token = input.take('=') && input.token(field.value, &overflow);
        if(!token || !valid_value(metadata.type, field.value)) {
          if(metadata.type != FieldType::Path) return fail(object.source, line, "invalid scene field value");
          char message[sizeof(SceneError::message)];
          path_field_diagnostic(message, sizeof(message), *info, metadata,
                                overflow || (token && std::strlen(field.value) >= path_field_size));
          return fail(object.source, line, message);
        }
        if(full(doc.fields_, doc.limits_.fields)) return fail(object.source, line, "scene field limit exceeded");
        field.line = line;
        *next = static_cast<uint32_t>(doc.fields_.size());
        doc.fields_.push_back(field);
        next = &doc.fields_.back().next;
      }
      return true;
    }

    bool validate_references() {
      for(const auto &object : doc.objects_) {
        if(!doc.snapshot_ && *object.base && object_index(doc, object.base, true) == scene_end)
          return fail(object.source, object.line, "unknown base prefab");
        for(auto c = object.first_component; c != scene_end; c = doc.components_[c].next) {
          const auto &component = doc.components_[c];
          const auto &info = types ? types[component.type] : *component_by_id(component.type);
          for(auto f = component.first_field; f != scene_end; f = doc.fields_[f].next) {
            const auto &field = doc.fields_[f];
            if(!resolved_value(doc, info.fields[field.field].type, field.value))
              return fail(object.source, field.line, "unknown entity reference target");
          }
        }
      }
      return true;
    }
  };

  bool read_scene(SceneDocument &out, std::string_view text, std::string_view path,
                  SceneReader reader, SceneError &error) {
    return SceneParser{out, reader, error}.read(text, path, false);
  }

  bool read_scene_file(SceneDocument &out, std::string_view path, SceneReader reader, SceneError &error) {
    return SceneParser{out, reader, error}.read({}, path, true);
  }

  bool read_scene_authoring(SceneDocument &out, std::string_view path, SceneReader reader, SceneError &error,
                            const ComponentInfo *types, uint16_t count) {
    if(!types) return SceneParser{out, reader, error}.read({}, {}, true);
    return SceneParser{out, reader, error, types, count}.read({}, path, true);
  }

  int write_scene(const SceneDocument &scene, char *buf, int cap) {
    detail::SceneOutput out(buf, cap);
    if(scene.sources().empty()) return out.fail();
    out.append(scene.snapshot() ? "snapshot 1 " : "scene 1 ");
    out.token(scene.title());
    if(!scene.snapshot_source().empty()) {
      out.append(" source=");
      out.token(scene.snapshot_source());
    }
    out.append("\n");
    for(const auto &source : scene.sources()) {
      if(source.parent != 0) continue;
      out.append("include ");
      out.token(source.path);
      out.append("\n");
    }
    for(const auto &object : scene.objects()) {
      if(object.source != 0) continue;
      out.append(object.prefab ? "prefab " : "entity ");
      out.append(object.key);
      if(!scene.snapshot() && *object.base) {
        out.append(" : ");
        out.append(object.base);
      }
      if(!object.prefab && std::string_view(object.name) != object.key) {
        out.append(" name=");
        out.token(object.name);
      }
      if(scene.snapshot() && *object.base) {
        out.append(" bind=");
        out.append(object.base);
      }
      out.append("\n");
      for(auto c = object.first_component; c != scene_end; c = scene.component(c).next) {
        const auto &component = scene.component(c);
        const auto *info = scene.type(component.type);
        if(!info) return out.fail();
        out.append("  ");
        out.append(info->name);
        for(auto f = component.first_field; f != scene_end; f = scene.field(f).next) {
          const auto &field = scene.field(f);
          out.append(" ");
          out.append(info->fields[field.field].name);
          out.append("=");
          out.token(field.value);
        }
        out.append("\n");
      }
    }
    return out.finish();
  }

  const ComponentInfo *SceneDocument::type(uint16_t id) const {
    return types_ ? (id < type_count_ ? types_ + id : nullptr) : component_by_id(id);
  }

  SceneObject *SceneDocument::editable_object(uint32_t index, SceneError &error) {
    error = {};
    if(index >= objects_.size()) {
      scene_error(error, sources_.empty() ? "" : sources_[0].path, 0, "unknown entity or prefab");
      return nullptr;
    }
    auto &object = objects_[index];
    if(object.source == 0) return &object;
    scene_error(error, sources_[object.source].path, object.line, "included scene objects are read-only");
    return nullptr;
  }

  bool SceneDocument::add_object(std::string_view key, std::string_view base, bool prefab, SceneError &error) {
    error = {};
    const std::string_view path = sources_.empty() ? "" : sources_[0].path;
    if(sources_.empty() || snapshot_) return scene_error(error, path, 0, "expected an authored scene");
    if(!valid_key(key) || (!prefab && key.size() >= string_field_size))
      return scene_error(error, path, 0, "invalid key (entity keys must fit the default name)");
    if(object_index(*this, key, prefab) != scene_end) return scene_error(error, path, 0, "duplicate scene object key");
    if(!base.empty() && (prefab || object_index(*this, base, true) == scene_end))
      return scene_error(error, path, 0, "invalid base prefab");
    if(!prefab) for(const auto &other : objects_)
      if(!other.prefab && key == other.name) return scene_error(error, path, 0, "duplicate entity name");
    if(full(objects_, limits_.objects)) return scene_error(error, path, 0, "scene object limit exceeded");
    SceneObject object{};
    object.prefab = prefab;
    scene_copy(object.key, sizeof(object.key), key);
    scene_copy(object.base, sizeof(object.base), base);
    if(!prefab) scene_copy(object.name, sizeof(object.name), key);
    objects_.push_back(object);
    return true;
  }

  void SceneDocument::restore_from(const SceneDocument &source) {
    sources_.reserve(source.limits_.sources);
    objects_.reserve(source.limits_.objects);
    components_.reserve(source.limits_.components);
    fields_.reserve(source.limits_.fields);
    limits_ = source.limits_;
    sources_ = source.sources_;
    objects_ = source.objects_;
    components_ = source.components_;
    fields_ = source.fields_;
    std::memcpy(title_, source.title_, sizeof(title_));
    std::memcpy(snapshot_source_, source.snapshot_source_, sizeof(snapshot_source_));
    snapshot_ = source.snapshot_;
    types_ = source.types_;
    type_count_ = source.type_count_;
  }

  bool SceneDocument::duplicate_entity(uint32_t index, std::string_view key, SceneError &error) {
    const auto *source = editable_object(index, error);
    if(!source) return false;
    if(source->prefab || snapshot_) return scene_error(error, sources_[0].path, source->line, "expected a placed entity");
    auto components = components_.size(), fields = fields_.size();
    for(auto c = source->first_component; c != scene_end; c = components_[c].next) {
      ++components;
      for(auto f = components_[c].first_field; f != scene_end; f = fields_[f].next) ++fields;
    }
    if(components > limits_.components || fields > limits_.fields)
      return scene_error(error, sources_[0].path, source->line, "duplicate exceeds scene budget");
    if(!add_object(key, source->base, false, error)) return false;
    const auto target = static_cast<uint32_t>(objects_.size() - 1);
    for(auto c = source->first_component; c != scene_end; c = components_[c].next) {
      const auto &component = components_[c];
      edit_component(target, component.type, true, error);
      for(auto f = component.first_field; f != scene_end; f = fields_[f].next) {
        const auto &field = fields_[f];
        const bool self = type(component.type)->fields[field.field].type == FieldType::Entity &&
                          field.value[0] == '@' && std::string_view(field.value + 1) == source->key;
        char reference[string_field_size + 1] = {'@'};
        if(self) scene_copy(reference + 1, string_field_size, key);
        const std::string_view value = self ? reference : field.value;
        edit_field(target, component.type, field.field, &value, error);
      }
    }
    return true;
  }

  bool SceneDocument::delete_entity(uint32_t index, SceneError &error) {
    auto *object = editable_object(index, error);
    if(!object) return false;
    if(object->prefab || snapshot_) return scene_error(error, sources_[0].path, object->line, "expected a placed entity");
    for(const auto &other : objects_) {
      if(&other == object) continue;
      for(auto c = other.first_component; c != scene_end; c = components_[c].next) {
        const auto &component = components_[c];
        const auto *info = type(component.type);
        for(auto f = component.first_field; f != scene_end; f = fields_[f].next) {
          const auto &field = fields_[f];
          if(info->fields[field.field].type == FieldType::Entity && field.value[0] == '@' &&
             std::string_view(field.value + 1) == object->key)
            return scene_error(error, sources_[other.source].path, field.line, "entity is still referenced");
        }
      }
    }
    while(object->first_component != scene_end)
      edit_component(index, components_[object->first_component].type, false, error);
    objects_.erase(objects_.begin() + index);
    return true;
  }

  void SceneDocument::erase_field(uint32_t &link) {
    const auto index = link;
    link = fields_[index].next;
    fields_.erase(fields_.begin() + index);
    for(auto &component : components_) if(component.first_field != scene_end && component.first_field > index) --component.first_field;
    for(auto &field : fields_) if(field.next != scene_end && field.next > index) --field.next;
  }

  bool SceneDocument::edit_component(uint32_t index, uint16_t id, bool add, SceneError &error) {
    auto *object = editable_object(index, error);
    if(!object) return false;
    const auto fail = [&](const char *message) { return scene_error(error, sources_[0].path, object->line, message); };
    const auto *info = type(id);
    if(!info || (!snapshot_ && std::string_view(info->name) == "Name")) return fail("component cannot be added or removed here");
    auto &link = record_link(components_, object->first_component, &SceneComponent::type, id);
    if(add) {
      if(link != scene_end) return fail("component already present");
      if(full(components_, limits_.components)) return fail("scene component limit exceeded");
      link = static_cast<uint32_t>(components_.size());
      components_.push_back({id, object->line});
    } else {
      if(link == scene_end) return fail("component is not set on this object");
      const auto removed = link;
      auto &component = components_[removed];
      while(component.first_field != scene_end) erase_field(component.first_field);
      link = component.next;
      components_.erase(components_.begin() + removed);
      for(auto &entry : objects_) if(entry.first_component != scene_end && entry.first_component > removed) --entry.first_component;
      for(auto &entry : components_) if(entry.next != scene_end && entry.next > removed) --entry.next;
    }
    return true;
  }

  bool SceneDocument::edit_field(uint32_t index, uint16_t id, uint8_t field, const std::string_view *value, SceneError &error) {
    auto *object = editable_object(index, error);
    if(!object) return false;
    const auto fail = [&](const char *message) { return scene_error(error, sources_[0].path, object->line, message); };
    const auto *info = type(id);
    if(!info || field >= info->field_count) return fail("unknown scene field");
    if(!snapshot_ && std::string_view(info->name) == "Name") return fail("use name= on the entity header");
    auto &c = record_link(components_, object->first_component, &SceneComponent::type, id);
    auto *f = c == scene_end ? nullptr : &record_link(fields_, components_[c].first_field, &SceneField::field, field);
    if(!value) {
      if(!f || *f == scene_end) return fail("field has no local override");
      erase_field(*f);
      return true;
    }
    SceneField entry{};
    const auto &metadata = info->fields[field];
    const auto field_type = metadata.type;
    if(!scene_copy(entry.value, sizeof(entry.value), *value) || value->find('\0') != value->npos ||
       !valid_value(field_type, entry.value)) {
      if(field_type != FieldType::Path) return fail("invalid scene field value");
      char message[sizeof(SceneError::message)];
      path_field_diagnostic(message, sizeof(message), *info, metadata, value->size() >= path_field_size);
      return fail(message);
    }
    if(!resolved_value(*this, field_type, entry.value)) return fail("unknown entity reference target");
    if(f && *f != scene_end) {
      std::memcpy(fields_[*f].value, entry.value, value->size() + 1);
      return true;
    }
    if(full(fields_, limits_.fields)) return fail("scene field limit exceeded");
    if(!f) {
      if(full(components_, limits_.components)) return fail("scene component limit exceeded");
      c = static_cast<uint32_t>(components_.size());
      components_.push_back({id, object->line});
      f = &components_.back().first_field;
    }
    entry.field = field;
    entry.line = object->line;
    *f = static_cast<uint32_t>(fields_.size());
    fields_.push_back(entry);
    return true;
  }

  bool set_scene_field(SceneDocument &doc, std::string_view key, std::string_view component,
                       std::string_view field, std::string_view value, SceneError &error) {
    auto index = object_index(doc, key, false);
    if(index == scene_end) index = object_index(doc, key, true);
    if(!doc.editable_object(index, error)) return false;
    const auto count = doc.types_ ? doc.type_count_ : component_count();
    for(uint16_t id = 0; id < count; ++id) {
      const auto &info = *doc.type(id);
      if(component == info.name) return doc.edit_field(index, id, scene_field(info, field), &value, error);
    }
    return scene_error(error, doc.sources_[0].path, doc.objects_[index].line, "unknown scene component");
  }

  bool rename_scene_entity(SceneDocument &doc, std::string_view key, std::string_view name, SceneError &error) {
    error = {};
    const auto index = object_index(doc, key, false);
    if(index == scene_end) {
      const std::string_view path = doc.sources_.empty() ? std::string_view{} : doc.sources_[0].path;
      return scene_error(error, path, 0, "unknown entity key");
    }
    auto &entity = doc.objects_[index];
    const auto fail = [&](std::string_view message) {
      return scene_error(error, doc.sources_[entity.source].path, entity.line, message);
    };
    if(entity.source != 0) return fail("included scene objects are read-only");
    char replacement[string_field_size];
    if(!scene_copy(replacement, sizeof(replacement), name) || name.find('\0') != name.npos) return fail("invalid entity name");
    if(doc.snapshot_ && !name.empty()) return fail("snapshot entity header names must be empty");
    if(!name.empty())
      for(const auto &other : doc.objects_)
        if(!other.prefab && &other != &entity && name == other.name) return fail("duplicate entity name");
    std::memcpy(entity.name, replacement, name.size() + 1);
    return true;
  }
}
