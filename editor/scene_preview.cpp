#include "scene_preview.hpp"
#include "builtin_defaults.hpp"
#include "documents.hpp"
#include "project.hpp"
#include "../libs/world/tileset_descriptor.hpp"

#include <imgui.h>
#include <imgui_internal.h> // Custom combo previews render literal names.
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <deque>
#include <fstream>
#include <stdexcept>

namespace editor {
  namespace fs = std::filesystem;

  struct MapDocument {
    explicit MapDocument(fs::path path) : path(std::move(path)) {}

    // For an appended layer, layer is the previous paint target and added_layer is its name.
    // A populated order stores the previous value instead of a cell or append.
    struct TileEdit {
      size_t layer, cell;
      uint8_t tile, transform;
      uint64_t revision;
      std::string added_layer{};
      std::optional<int32_t> order{};
    };
    TilePreview tiles;
    fs::path path;
    std::string tileset, metadata_error, save_error;
    sdk::TilesetDescriptor::Properties tile_properties;
    std::string new_layer, layer_error;
    std::vector<TileEdit> undo, redo;
    uint64_t revision = 0, saved_revision = 0, next_revision = 0;
    uint64_t load_revision = 0;
    size_t layer = 0;
    int brush = -1;
    bool loaded = false, paint = false, paint_initialized = false, writable = false;
    bool order_error = false;
  };

  namespace {
    constexpr int scene_file_limit = 1024 * 1024;

    fs::path scene_path(const fs::path &root, std::string_view path) {
      const auto requested = fs::u8path(path);
      if(requested.has_root_path()) return {};
      std::error_code error;
      const auto absolute = fs::canonical(root / requested, error);
      if(error) return {};
      const auto relative = absolute.lexically_relative(root);
      if(relative.empty() || *relative.begin() == ".." || !fs::is_regular_file(absolute, error)) return {};
      return absolute;
    }

    bool read_text(const fs::path &path, std::string &text) {
      std::ifstream input(path, std::ios::binary | std::ios::ate);
      const auto size = input.tellg();
      if(size < 0 || size > scene_file_limit) return false;
      text.resize(static_cast<size_t>(size));
      input.seekg(0);
      return static_cast<bool>(input.read(text.data(), static_cast<std::streamsize>(size)));
    }

    struct SceneFiles {
      const fs::path &root;
      std::deque<std::string> contents; // The parser borrows each file until parsing completes.

      static bool read(void *context, std::string_view path, std::string_view &text) {
        auto &files = *static_cast<SceneFiles *>(context);
        auto &buffer = files.contents.emplace_back();
        if(!read_text(scene_path(files.root, path), buffer)) return false;
        text = buffer;
        return true;
      }
    };
    std::string scene_diagnostic(const sdk::SceneError &error) {
      return std::string(error.file) + ':' + std::to_string(error.line) + ": " + error.message;
    }

    const std::string *exported_default(const nlohmann::json &registry, const char *type, const char *field) {
      const auto defaults = registry.find("defaults");
      if(defaults == registry.end()) return nullptr;
      const auto component = defaults->find(type);
      if(component == defaults->end()) return nullptr;
      const auto value = component->find(field);
      return value == component->end() ? nullptr : value->get_ptr<const std::string *>();
    }

    const sdk::FieldInfo *string_field(const sdk::ComponentInfo &type, const char *name) {
      const auto *field = sdk::find_field(type, name);
      return field && field->type == sdk::FieldType::String ? field : nullptr;
    }

    struct DoorFields {
      explicit DoorFields(const sdk::ComponentInfo &type) {
        if(std::string_view(type.name) != "Door") return;
        scene = string_field(type, "scene");
        arrival = string_field(type, "arrival");
      }
      bool has(const sdk::FieldInfo &field) const { return scene == &field || arrival == &field; }
      uint8_t display_index(const sdk::ComponentInfo &type, uint8_t index) const {
        if(!scene || !arrival || scene < arrival) return index;
        const auto *field = type.fields + index;
        if(field == scene) return static_cast<uint8_t>(arrival - type.fields);
        if(field == arrival) return static_cast<uint8_t>(scene - type.fields);
        return index;
      }
      const sdk::FieldInfo *scene = nullptr, *arrival = nullptr;
    };


    sdk::FieldType field_type(const std::string &name) {
      using sdk::FieldType;
      for(const auto type : {FieldType::Bool, FieldType::Int32, FieldType::UInt32,
                             FieldType::Float, FieldType::String, FieldType::Entity})
        if(name == sdk::field_type_name(type)) return type;
      throw std::runtime_error("Unsupported registry field type: " + name);
    }

    bool is_collision_category(const sdk::ComponentInfo &type, const sdk::FieldInfo &field) {
      return std::string_view(type.name) == "BoxCollider2D" &&
             std::string_view(field.name) == "category" && field.type == sdk::FieldType::UInt32;
    }

    const char *collision_category_label(const std::vector<std::string> &labels, const char *value) {
      uint32_t index;
      if(!sdk::parse_value(sdk::FieldType::UInt32, value, &index) || index >= labels.size() || labels[index].empty())
        return nullptr;
      return labels[index].c_str();
    }

    std::optional<uint32_t> pick_collision_category(const std::vector<std::string> &labels, const char *value) {
      if(std::none_of(labels.begin(), labels.end(), [](const auto &label) { return !label.empty(); })) return {};
      const auto *label = collision_category_label(labels, value);
      std::optional<uint32_t> selected;
      if(ImGui::BeginCombo("##category", label ? label : "Choose named category")) {
        for(uint32_t i = 0; i < labels.size(); ++i) {
          if(labels[i].empty()) continue;
          ImGui::PushID(static_cast<int>(i));
          if(ImGui::Selectable(labels[i].c_str(), label == labels[i].c_str())) selected = i;
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      return selected;
    }

    enum class SpriteField { Other, Asset, Clip };

    SpriteField sprite_field(const sdk::ComponentInfo &type, const sdk::FieldInfo &field) {
      if(std::string_view(type.name) != "Sprite" || field.type != sdk::FieldType::String) return SpriteField::Other;
      if(std::string_view(field.name) == "asset") return SpriteField::Asset;
      if(std::string_view(field.name) == "clip") return SpriteField::Clip;
      return SpriteField::Other;
    }

    template<class Options> std::optional<std::string_view> pick_string(const char *blank, const char *value,
                                                                       Options options, bool limit_name = true) {
      std::optional<std::string_view> selection;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
      if(ImGui::BeginCombo("##reference value", nullptr, ImGuiComboFlags_CustomPreview)) {
        if(ImGui::Selectable(blank, !*value)) selection = "";
        if(!*value) ImGui::SetItemDefaultFocus();
        bool too_long = false;
        const auto option = [&](const std::string &name) {
          if(limit_name && name.size() >= sdk::string_field_size) { too_long = true; return; }
          ImGui::PushID(&name);
          const auto pos = ImGui::GetCursorScreenPos();
          const bool selected = name == value;
          if(ImGui::Selectable("##option", selected, 0, {0, ImGui::CalcTextSize(name.c_str()).y})) selection = name;
          ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
          ImGui::SetItemTooltip("%s", name.c_str());
          if(selected) ImGui::SetItemDefaultFocus();
          ImGui::PopID();
        };
        options(option);
        if(too_long) ImGui::TextDisabled("Names over %d UTF-8 bytes are omitted.", sdk::string_field_size - 1);
        ImGui::EndCombo();
      }
      if(ImGui::BeginComboPreview()) {
        ImGui::TextUnformatted(*value ? value : blank);
        ImGui::EndComboPreview();
      }
      return selection;
    }

    std::optional<std::string_view> pick_sprite(Documents &documents, SpriteField field, const char *value,
                                                const sdk::SpriteClips *descriptor) {
      const bool asset = field == SpriteField::Asset;
      const char *blank = asset ? "No sprite asset (blank value)" : "Use descriptor default (blank)";
      return pick_string(blank, value, [&](const auto &option) {
        if(asset) {
          if(ImGui::IsWindowAppearing()) documents.refresh_files();
          for(const auto &name : documents.sprite_stems()) option(name);
          if(documents.sprite_stems().empty()) ImGui::TextDisabled("No .sprite.json descriptors found.");
        } else if(descriptor) {
          for(const auto &entry : descriptor->clips) option(entry.first);
        } else ImGui::TextDisabled("Choose a valid sprite descriptor to list clips.");
      });
    }

    void describe_clip(const char *value, const sdk::SpriteClips *descriptor) {
      if(!descriptor) {
        if(*value) ImGui::TextWrapped("Cannot resolve this clip without a valid sprite descriptor.");
      } else if(*value) {
        if(descriptor->clips.find(value) == descriptor->clips.end())
          ImGui::TextWrapped("Clip not found in the effective sprite descriptor.");
      } else if(descriptor->clips.empty()) ImGui::TextDisabled("Blank uses cell 0.");
      else {
        const auto &name = descriptor->clips.begin()->first;
        if(name.size() < sdk::string_field_size) ImGui::TextDisabled("Blank uses the first clip: %s", name.c_str());
        else ImGui::TextDisabled("Blank uses the first clip in the descriptor.");
      }
    }
  }

  void MapDocuments::finish_frame() {
    retired_images_.clear();
    for(auto it = documents_.begin(); it != documents_.end();) {
      if(it->second.use_count() == 1) it = documents_.erase(it);
      else ++it;
    }
  }

  void MapDocuments::retire_image(PreviewImage image) {
    // Earlier panels can have queued ImGui commands using the old texture.
    if(image.texture) retired_images_.push_back(std::move(image));
  }

  ScenePreview::ScenePreview(std::string path, SDL_Renderer *renderer, MapDocuments &maps)
    : map_only_(fs::u8path(path).extension() == ".map"), path_(std::move(path)), renderer_(renderer), maps_(maps) {
    auto map_path = fs::u8path(path_).lexically_normal();
    map_path.replace_extension(".map");
    if(!maps_.root_.empty() && !map_path.has_root_path()) {
      std::error_code error;
      const auto canonical = fs::weakly_canonical(maps_.root_ / map_path, error);
      const auto relative = canonical.lexically_relative(maps_.root_);
      if(!error && !relative.empty() && *relative.begin() != "..") map_path = relative;
    }
    auto &entry = maps_.documents_[map_path];
    if(!entry) entry = std::make_shared<MapDocument>(std::move(map_path));
    map_ = entry;
    if(map_only_ && !map_->paint_initialized) {
      map_->paint_initialized = true;
      map_->paint = true;
    }
  }

  const std::string &ScenePreview::tileset_path() const { return map_->tileset; }
  TilePreview &ScenePreview::tiles() { return map_->tiles; }
  const TilePreview &ScenePreview::tiles() const { return map_->tiles; }
  bool ScenePreview::paint_mode() const { return tiles().map() && map_->paint; }
  bool ScenePreview::dirty(bool map) const { return map ? map_->revision != map_->saved_revision : dirty_; }
  bool ScenePreview::can_history(bool map, bool redo) const {
    return map ? !(redo ? map_->redo : map_->undo).empty() : !(redo ? redo_ : undo_).empty();
  }

  void ScenePreview::select_brush(int tile) {
    if(tiles().map() && tile >= 0 && tile <= tiles().tile_count()) map_->brush = tile;
  }

  int ScenePreview::selected_brush() const { return tiles().map() ? map_->brush : -1; }

  void ScenePreview::invalidate_maps() { map_->loaded = false; }

  void ScenePreview::prepare_map(const Project &project) {
    if(!map_->loaded) refresh_layers(project);
  }

  void ScenePreview::discard_map(const Project &project) {
    maps_.retire_image(tiles().release_atlas());
    MapDocument fresh(map_->path);
    fresh.paint = map_->paint;
    fresh.paint_initialized = map_->paint_initialized;
    fresh.load_revision = map_->load_revision;
    *map_ = std::move(fresh);
    prepare_map(project);
  }

  void ScenePreview::reload(const Project &project) {
    loaded_ = true;
    error_.clear();
    notice_.clear();
    dirty_ = writable_ = false;
    undo_.clear();
    redo_.clear();
    revision_ = saved_revision_ = next_revision_ = 0;
    route_revision_.reset();
    markers_dirty_ = true;
    for(auto &entry : sprite_previews_) maps_.retire_image(std::move(entry.second.image));
    sprite_previews_.clear();
    door_destinations_.clear();
    selected_ = dragging_ = sdk::scene_end;
    add_component_type_ = UINT16_MAX;
    try {
      collision_categories_ = project.collision_categories();
      const auto &path = project.registry();
      std::ifstream input(project.root() / fs::u8path(path));
      if(!input) throw std::runtime_error("Missing " + path + ". Build the project's component registry target and reload.");
      registry_ = nlohmann::json::parse(input);
      if(registry_.at("version") != 1) throw std::runtime_error("Unsupported component registry version.");
      auto &components = registry_.at("components");
      if(!components.is_object() || components.size() > types_.size()) throw std::runtime_error("Invalid component registry.");
      // Older projects can use the built-in camera without re-exporting custom types.
      if(!components.contains("Camera2D") && components.size() < types_.size()) {
        if(const auto *camera = sdk::find_component("Camera2D")) {
          auto fields = nlohmann::json::object();
          for(uint8_t i = 0; i < camera->field_count; ++i)
            fields[camera->fields[i].name] = sdk::field_type_name(camera->fields[i].type);
          components[camera->name] = std::move(fields);
        }
      }
      // Older exports already describe Sprite, but may not include its new field.
      if(const auto sprite = components.find("Sprite"); sprite != components.end() &&
         sprite->is_object() && !sprite->contains("layer")) {
        (*sprite)["layer"] = sdk::field_type_name(sdk::FieldType::Int32);
      }
      if(const auto collider = components.find("BoxCollider2D"); collider != components.end() &&
         collider->is_object() && !collider->contains("category")) {
        (*collider)["category"] = sdk::field_type_name(sdk::FieldType::UInt32);
      }
      uint16_t count = 0;
      for(auto component = components.cbegin(); component != components.cend(); ++component, ++count) {
        auto &fields = fields_[count];
        if(!component->is_object() || component->size() > fields.size()) throw std::runtime_error("Invalid fields for " + component.key());
        uint8_t index = 0;
        for(auto field = component->cbegin(); field != component->cend(); ++field)
          fields[index++] = {field.key().c_str(), field_type(field->get_ref<const std::string &>()), 0};
        types_[count] = {component.key().c_str(), count, 0, fields.data(), index};
      }
      SceneFiles files{project.root(), {}};
      sdk::SceneError error;
      if(!sdk::read_scene_authoring(scene_, path_, {&files, SceneFiles::read}, error, types_.data(), count)) {
        error_ = scene_diagnostic(error);
        if(std::string_view(error.message) == "cannot read scene include") error_ += " (Check the included project file.)";
        return;
      }
      original_ = std::move(files.contents.front());
      const auto relative = fs::u8path(path_);
      // Edit only a real, authored root file; never follow aliases for writes.
      writable_ = *relative.begin() != "generated" && scene_path(project.root(), path_) == project.root() / relative;
    } catch(const std::exception &error) { error_ = error.what(); }
  }

  void ScenePreview::save(const Project &project) {
    try {
      const auto path = project.root() / fs::u8path(path_);
      const auto unchanged = [&] {
        std::string current;
        return scene_path(project.root(), path_) == path && read_text(path, current) && current == original_;
      };
      if(!unchanged()) throw std::runtime_error("Scene changed on disk. Discard and reload before saving again.");
      std::string output(scene_file_limit, '\0');
      const int size = sdk::write_scene(scene_, output.data(), scene_file_limit);
      if(size < 0) throw std::runtime_error("Could not serialize scene within the file size limit.");
      auto directory = path;
      directory += ".editor-save";
      const auto staged = directory / path.filename();
      if(!fs::create_directory(directory)) throw std::runtime_error("Scene save staging directory already exists.");
      struct Cleanup {
        const fs::path &file, &directory;
        ~Cleanup() { std::error_code error; fs::remove(file, error); fs::remove(directory, error); }
      } cleanup{staged, directory};
      std::ofstream out(staged, std::ios::binary);
      out.write(output.data(), size);
      out.close();
      if(!out) throw std::runtime_error("Could not write staged scene.");
      if(!unchanged()) throw std::runtime_error("Scene changed on disk. Discard and reload before saving again.");
      fs::rename(staged, path);
      output.resize(static_cast<size_t>(size));
      original_ = std::move(output);
      saved_revision_ = revision_;
      dirty_ = false;
      notice_ = "Scene saved.";
    } catch(const std::exception &error) { notice_ = error.what(); }
  }

  ScenePreview::Revision ScenePreview::snapshot() const {
    auto copy = std::make_unique<sdk::SceneDocument>(sdk::SceneLimits{0, 0, 0, 0});
    copy->restore_from(scene_);
    return {std::move(copy), selected_, revision_};
  }

  void ScenePreview::record(std::vector<Revision> &stack, Revision revision) {
    if(stack.size() == 20) stack.erase(stack.begin());
    stack.push_back(std::move(revision));
  }

  void ScenePreview::undo_or_redo(bool redo) {
    auto &from = redo ? redo_ : undo_;
    if(from.empty()) return;
    record(redo ? undo_ : redo_, snapshot());
    const auto &state = from.back();
    scene_.restore_from(*state.document);
    selected_ = state.selection;
    revision_ = state.id;
    from.pop_back();
    dragging_ = sdk::scene_end;
    changed(true, {});
  }

  void ScenePreview::changed(bool ok, const sdk::SceneError &error) {
    if(ok) {
      dirty_ = revision_ != saved_revision_;
      markers_dirty_ = true;
      notice_.clear();
    } else notice_ = error.message;
  }

  void ScenePreview::draw_add_object(bool prefab) {
    if(ImGui::SmallButton(prefab ? "Add Prefab" : "Add Entity")) {
      object_key_.clear();
      base_key_.clear();
      ImGui::OpenPopup("New scene object");
    }
    if(!ImGui::BeginPopup("New scene object")) return;
    ImGui::InputText("Key", &object_key_);
    if(!prefab && ImGui::BeginCombo("Prefab", base_key_.empty() ? "None" : base_key_.c_str())) {
      if(ImGui::Selectable("None", base_key_.empty())) base_key_.clear();
      for(const auto &object : scene_.objects()) {
        if(!object.prefab) continue;
        ImGui::PushID(object.key);
        if(ImGui::Selectable(object.key, base_key_ == object.key)) base_key_ = object.key;
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if(ImGui::Button("Create")) {
      const bool ok = edit([&](auto &error) { return scene_.add_object(object_key_, base_key_, prefab, error); });
      if(ok && !prefab) selected_ = static_cast<uint32_t>(scene_.objects().size() - 1);
      if(ok) ImGui::CloseCurrentPopup();
    }
    if(!notice_.empty()) ImGui::TextWrapped("%s", notice_.c_str());
    ImGui::EndPopup();
  }

  const sdk::SceneObject *ScenePreview::prefab_for(const sdk::SceneObject &object) const {
    if(!*object.base) return nullptr;
    const auto &objects = scene_.objects();
    const auto base = std::find_if(objects.begin(), objects.end(), [&](const auto &candidate) {
      return candidate.prefab && std::string_view(candidate.key) == object.base;
    });
    return base != objects.end() ? &*base : nullptr;
  }

  bool ScenePreview::draw_add_component(const fs::path &root, Documents &documents, uint32_t index,
                                        const sdk::ComponentInfo *chosen) {
    if(chosen) {
      add_component_type_ = chosen->id;
      const auto *builtin = sdk::find_component(chosen->name);
      const auto *bytes = builtin ? static_cast<const char *>(sdk::builtin_component_defaults(*builtin)) : nullptr;
      for(uint8_t i = 0; i < chosen->field_count; ++i) {
        const auto &field = chosen->fields[i];
        auto &draft = initial_fields_[i];
        draft = {};
        if(const auto *text = exported_default(registry_, chosen->name, field.name)) draft.default_value = *text;
        if(!draft.default_value && bytes) {
          const auto *native_field = sdk::find_field(*builtin, field.name);
          char value[sdk::max_value_text];
          if(native_field && native_field->type == field.type &&
             sdk::format_value(field.type, bytes + native_field->offset, value, sizeof(value)) >= 0)
            draft.default_value = value;
        }
        if(draft.default_value) draft.value = *draft.default_value;
      }
      notice_.clear();
      ImGui::OpenPopup("Add prefab component");
    }
    if(!ImGui::BeginPopup("Add prefab component")) return false;
    const auto *type = scene_.type(add_component_type_);
    if(!type) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return false;
    }
    ImGui::Text("Add %s", type->name);
    if(type->field_count) ImGui::TextDisabled("Check a field to set an initial override.");
    const auto *asset_field = std::string_view(type->name) == "Sprite" ? string_field(*type, "asset") : nullptr;
    const DoorFields door(*type);
    for(uint8_t position = 0; position < type->field_count; ++position) {
      const auto i = door.display_index(*type, position);
      const auto &field = type->fields[i];
      auto &draft = initial_fields_[i];
      ImGui::PushID(i);
      ImGui::Checkbox(field.name, &draft.enabled);
      ImGui::SameLine();
      if(draft.default_value) {
        const auto &value = *draft.default_value;
        ImGui::TextDisabled("(%s, default: %s)", sdk::field_type_name(field.type), value.empty() ? "\"\"" : value.c_str());
      } else ImGui::TextDisabled("(%s, default unavailable)", sdk::field_type_name(field.type));
      if(draft.enabled) {
        ImGui::Indent();
        const auto picker = sprite_field(*type, field);
        if(picker != SpriteField::Other) {
          const SpritePreview *sprite = nullptr;
          if(asset_field) {
            const auto &asset = initial_fields_[asset_field - type->fields];
            const auto *stem = asset.enabled ? &asset.value : asset.default_value ? &*asset.default_value : nullptr;
            if(stem && !stem->empty() && stem->size() < sdk::string_field_size)
              sprite = &sprite_descriptor(root, *stem, true);
          }
          const auto *descriptor = sprite && !sprite->descriptor_error ? &sprite->descriptor : nullptr;
          if(picker == SpriteField::Clip) describe_clip(draft.value.c_str(), descriptor);
          else if(sprite && sprite->descriptor_error) ImGui::TextWrapped("%s", sprite->descriptor_error);
          if(const auto value = pick_sprite(documents, picker, draft.value.c_str(), descriptor)) draft.value.assign(*value);
        } else if(door.has(field)) {
          const char *scene = "";
          if(door.scene) {
            const auto &target = initial_fields_[door.scene - type->fields];
            if(target.enabled) scene = target.value.c_str();
            else if(target.default_value) scene = target.default_value->c_str();
          }
          if(const auto value = draw_door_field(root, documents, &field == door.scene, draft.value.c_str(), scene, true))
            draft.value.assign(*value);
        } else if(is_collision_category(*type, field)) {
          if(const auto choice = pick_collision_category(collision_categories_, draft.value.c_str()))
            draft.value = std::to_string(*choice);
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15);
        ImGui::InputText("##initial value", &draft.value);
        ImGui::Unindent();
      }
      ImGui::PopID();
    }
    const bool attempted = ImGui::Button("Add");
    if(attempted) {
      const char *failed_field = nullptr;
      const bool ok = edit([&](auto &error) {
        if(!scene_.edit_component(index, type->id, true, error)) return false;
        for(uint8_t i = 0; i < type->field_count; ++i) {
          const auto &draft = initial_fields_[i];
          if(!draft.enabled) continue;
          const std::string_view text = draft.value;
          if(!scene_.edit_field(index, type->id, i, &text, error)) {
            failed_field = type->fields[i].name;
            return false;
          }
        }
        return true;
      });
      if(ok) ImGui::CloseCurrentPopup();
      else if(failed_field) notice_ = std::string(failed_field) + ": " + notice_;
    }
    ImGui::SameLine();
    if(ImGui::Button("Cancel")) {
      notice_.clear();
      ImGui::CloseCurrentPopup();
    }
    if(!notice_.empty()) ImGui::TextWrapped("%s", notice_.c_str());
    ImGui::EndPopup();
    return attempted; // A failed transaction can also invalidate the inspector's scene references.
  }

  void ScenePreview::draw_object(const fs::path &root, Documents &documents, uint32_t index) {
    const auto &entity = scene_.objects()[index];
    const bool editable = writable_ && entity.source == 0;
    std::array<const sdk::SceneComponent *, sdk::max_components> inherited{}, authored{};
    const auto collect = [&](const sdk::SceneObject &object, auto &components) {
      for(auto i = object.first_component; i != sdk::scene_end; i = scene_.component(i).next) {
        const auto &component = scene_.component(i);
        components[component.type] = &component;
      }
    };
    if(const auto *base = prefab_for(entity)) {
      collect(*base, inherited);
      ImGui::TextDisabled("Prefab: %s", entity.base);
    }
    collect(entity, authored);
    const auto &source = scene_.sources()[entity.source];
    if(entity.line) ImGui::TextWrapped("%s:%u", source.path, entity.line);
    if(!editable) ImGui::TextDisabled("Read-only source");
    if(*entity.name) ImGui::TextWrapped("Name.value: %s", entity.name);
    const sdk::ComponentInfo *chosen = nullptr;
    if(editable && ImGui::BeginCombo("##add component", "Add Component")) {
      for(uint16_t id = 0; id < types_.size(); ++id) {
        const auto *type = scene_.type(id);
        if(!type) break;
        if(inherited[id] || authored[id] || std::string_view(type->name) == "Name") continue;
        if(ImGui::Selectable(type->name)) chosen = type;
      }
      ImGui::EndCombo();
    }
    if(editable && entity.prefab) {
      if(draw_add_component(root, documents, index, chosen)) return;
    } else if(chosen) {
      edit([&](auto &error) { return scene_.edit_component(index, chosen->id, true, error); });
      return;
    }
    for(uint16_t id = 0; id < types_.size(); ++id) {
      const auto *type = scene_.type(id);
      if(!type) break;
      if(!inherited[id] && !authored[id]) continue;
      ImGui::PushID(id);
      ImGui::SeparatorText(type->name);
      if(editable && authored[id] && ImGui::SmallButton(inherited[id] ? "Remove local component overrides" : "Remove Component")) {
        edit([&](auto &error) { return scene_.edit_component(index, id, false, error); });
        ImGui::PopID();
        return;
      }
      std::array<const char *, sdk::max_component_fields> values{};
      const auto apply = [&](const sdk::SceneComponent *component) {
        if(!component) return;
        for(auto i = component->first_field; i != sdk::scene_end; i = scene_.field(i).next) {
          const auto &field = scene_.field(i);
          values[field.field] = field.value;
        }
      };
      apply(authored[id]);
      const auto local = values;
      apply(inherited[id]);
      const SpritePreview *sprite = nullptr;
      if(std::string_view(type->name) == "Sprite") {
        if(const auto *asset = string_field(*type, "asset")) {
          const auto i = asset - type->fields;
          const auto *stem = local[i] ? local[i] : values[i];
          if(stem && *stem) sprite = &sprite_descriptor(root, stem, true);
        }
      }
      const auto *descriptor = sprite && !sprite->descriptor_error ? &sprite->descriptor : nullptr;
      const DoorFields door(*type);
      const auto effective_door_value = [&](const sdk::FieldInfo *field) -> const char * {
        if(!field) return nullptr;
        const auto i = field - type->fields;
        if(local[i] || values[i]) return local[i] ? local[i] : values[i];
        const auto *value = exported_default(registry_, type->name, field->name);
        return value ? value->c_str() : nullptr;
      };
      const auto *target = effective_door_value(door.scene);
      for(uint8_t position = 0; position < type->field_count; ++position) {
        const auto i = door.display_index(*type, position);
        const auto &field = type->fields[i];
        const bool door_picker = door.has(field);
        const auto *value = door_picker ? effective_door_value(&field) : local[i] ? local[i] : values[i];
        const bool category = is_collision_category(*type, field);
        const char *text = value ? value : category ? "0" : "";
        ImGui::TextWrapped("%s: %s%s", field.name, value ? (*value ? value : "\"\"") : "Uses component default",
                          local[i] ? " (set here)" : values[i] ? " (inherited)" : value ? " (component default)" : "");
        if(category)
          if(const auto *label = collision_category_label(collision_categories_, text)) ImGui::TextDisabled("%s", label);
        const auto picker = sprite_field(*type, field);
        if(picker == SpriteField::Clip) describe_clip(text, descriptor);
        else if(picker == SpriteField::Asset && sprite && sprite->descriptor_error)
          ImGui::TextWrapped("%s", sprite->descriptor_error);
        ImGui::PushID(i);
        std::optional<std::string_view> selection;
        if(door_picker) selection = draw_door_field(root, documents, &field == door.scene, text, target ? target : "", editable);
        else if(editable && picker != SpriteField::Other) selection = pick_sprite(documents, picker, text, descriptor);
        std::string category_value;
        if(editable && category) {
          if(const auto choice = pick_collision_category(collision_categories_, text)) {
            category_value = std::to_string(*choice);
            selection = category_value;
          }
        }
        if(!editable) { ImGui::PopID(); continue; }
        if(selection && (!local[i] || *selection != local[i])) {
          edit([&](auto &error) { return scene_.edit_field(index, id, i, &*selection, error); });
          ImGui::PopID();
          ImGui::PopID();
          return;
        }
        if(ImGui::SmallButton(door_picker || picker != SpriteField::Other || category ? "Edit text" : "Edit")) {
          draft_ = text;
          notice_.clear();
          ImGui::OpenPopup("Edit scene field");
        }
        bool attempted = false;
        if(ImGui::BeginPopup("Edit scene field")) {
          bool updated = false;
          ImGui::Text("%s (%s)", field.name, sdk::field_type_name(field.type));
          if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
          const bool enter = ImGui::InputText("##value", &draft_, ImGuiInputTextFlags_EnterReturnsTrue);
          if(door_picker && draft_.size() >= sdk::string_field_size)
            ImGui::TextWrapped("Door fields can hold at most %d UTF-8 bytes.", sdk::string_field_size - 1);
          if(ImGui::Button("Set") || enter) {
            attempted = true;
            const std::string_view text = draft_;
            updated = edit([&](auto &error) { return scene_.edit_field(index, id, i, &text, error); });
          }
          if(!attempted && local[i]) {
            ImGui::SameLine();
            if(ImGui::Button("Reset")) {
              attempted = true;
              updated = edit([&](auto &error) { return scene_.edit_field(index, id, i, nullptr, error); });
            }
          }
          if(updated) ImGui::CloseCurrentPopup();
          else if(!notice_.empty()) ImGui::TextWrapped("%s", notice_.c_str());
          ImGui::EndPopup();
        }
        ImGui::PopID();
        if(attempted) { ImGui::PopID(); return; }
      }
      ImGui::PopID();
    }
  }

  bool ScenePreview::ready(const Project &project) {
    if(map_only_) return true;
    if(!loaded_) reload(project);
    if(error_.empty()) return true;
    ImGui::TextWrapped("%s", error_.c_str());
    return false;
  }

  void ScenePreview::save_file(const Project &project, bool map) {
    if(!dirty(map)) return;
    if(!map) { save(project); return; }
    const auto path = project.root() / map_->path;
    if(scene_path(project.root(), map_->path.generic_u8string()) != path) {
      map_->save_error = "Map path changed on disk; reload before saving.";
    } else if(tiles().save(path, map_->save_error)) map_->saved_revision = map_->revision;
  }

  std::string ScenePreview::file_path(bool map) const { return map ? map_->path.generic_u8string() : path_; }

  void ScenePreview::revert_file(const Project &project, bool map) {
    if(map) discard_map(project);
    else reload(project);
  }

  ScenePreview::SpritePreview &ScenePreview::sprite_descriptor(const fs::path &root, std::string_view asset, bool inspect) {
    // The canvas uses cached data; visible inspector fields check each descriptor once per frame.
    const auto frame = ImGui::GetFrameCount();
    const auto found = sprite_previews_.find(asset);
    if(found != sprite_previews_.end() && (!inspect || found->second.checked_frame == frame)) return found->second;
    auto &preview = found == sprite_previews_.end() ? sprite_previews_[std::string(asset)] : found->second;
    preview.checked_frame = frame;
    fs::path path;
    try { path = scene_path(root, std::string(asset) + ".sprite.json"); }
    catch(const std::system_error &) {} // An authored byte string need not be a valid filesystem path.
    catch(const std::range_error &) {}
    std::error_code error;
    const auto modified = path.empty() ? fs::file_time_type::min() : fs::last_write_time(path, error);
    const auto size = path.empty() || error ? uintmax_t(-1) : fs::file_size(path, error);
    if(found != sprite_previews_.end() && preview.modified == modified && preview.descriptor_size == size) return preview;
    preview.modified = modified;
    preview.descriptor_size = size;
    preview.descriptor = {};
    maps_.retire_image(std::move(preview.image));
    preview.image_attempted = false;
    std::string text;
    preview.descriptor_error = !read_text(path, text) ? "Sprite descriptor is missing or unreadable." :
      sdk::read_sprite_clips(nlohmann::json::parse(text, nullptr, false), preview.descriptor);
    return preview;
  }

  const ScenePreview::SpritePreview &ScenePreview::sprite_preview(const fs::path &root, std::string_view asset) {
    auto &preview = sprite_descriptor(root, asset, false);
    if(preview.image_attempted || preview.descriptor_error) return preview;
    preview.image_attempted = true;
    std::ifstream input(scene_path(root, preview.descriptor.image), std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if(size <= 0 || size > 16 * 1024 * 1024) return preview;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if(input.seekg(0).read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
      preview.image.load_png(renderer_, bytes);
    return preview;
  }

  const ScenePreview::DoorDestination &ScenePreview::door_destination(const fs::path &root, std::string_view path, bool refresh) {
    const auto now = ImGui::GetTime();
    const auto found = door_destinations_.find(path);
    const bool first = found == door_destinations_.end();
    auto &destination = first ? door_destinations_[std::string(path)] : found->second;
    if(!first && !refresh && now < destination.next_check) return destination;
    destination.next_check = now + 1.0;

    const auto stamp = [&](std::string_view name) {
      DoorSourceStamp source;
      try { source.file = scene_path(root, name); }
      catch(const std::system_error &) {}
      catch(const std::range_error &) {}
      if(!source.file.empty()) {
        std::error_code error;
        source.modified = fs::last_write_time(source.file, error);
        if(!error) source.size = fs::file_size(source.file, error);
      }
      return source;
    };
    // Include attempted reads so repairing a missing include invalidates the cached error, too.
    if(!first && std::all_of(destination.sources.begin(), destination.sources.end(), [&](const auto &source) {
      return stamp(source.first) == source.second;
    })) return destination;
    destination.sources.clear();
    destination.arrivals.clear();
    destination.error.clear();
    constexpr std::string_view suffix = ".scene";
    if(path.size() <= suffix.size() || path.substr(path.size() - suffix.size()) != suffix) {
      destination.error = "Expected a project-relative .scene path.";
      return destination;
    }

    struct DestinationReader {
      decltype(stamp) &stamp_file;
      DoorDestination &destination;
      std::deque<std::string> contents;
    } files{stamp, destination, {}};
    const auto read = [](void *context, std::string_view name, std::string_view &text) {
      auto &files = *static_cast<DestinationReader *>(context);
      const auto &source = files.destination.sources.emplace_back(std::string(name), files.stamp_file(name));
      auto &buffer = files.contents.emplace_back();
      if(!read_text(source.second.file, buffer)) return false;
      text = buffer;
      return true;
    };
    sdk::SceneDocument target(sdk::SceneLimits{});
    sdk::SceneError error;
    const auto count = static_cast<uint16_t>(registry_.at("components").size());
    if(!sdk::read_scene_authoring(target, path, {&files, read}, error, types_.data(), count)) {
      destination.error = scene_diagnostic(error);
      return destination;
    }

    uint16_t transform = 0;
    while(const auto *type = target.type(transform)) {
      if(std::string_view(type->name) == "Transform2D") break;
      ++transform;
    }
    const auto has_transform = [&](const sdk::SceneObject &object) {
      for(auto i = object.first_component; i != sdk::scene_end; i = target.component(i).next)
        if(target.component(i).type == transform) return true;
      return false;
    };
    std::vector<std::string_view> transformed_prefabs;
    for(const auto &object : target.objects())
      if(object.prefab && has_transform(object)) transformed_prefabs.emplace_back(object.key);
    std::sort(transformed_prefabs.begin(), transformed_prefabs.end());
    std::map<std::string_view, bool> named;
    for(const auto &object : target.objects()) {
      if(object.prefab || !*object.name) continue;
      const bool positioned = has_transform(object) ||
        std::binary_search(transformed_prefabs.begin(), transformed_prefabs.end(), std::string_view(object.base));
      if(!named.emplace(object.name, positioned).second) {
        destination.error = std::string(target.sources()[object.source].path) + ':' + std::to_string(object.line) +
                            ": duplicate entity name: " + object.name;
        return destination;
      }
    }
    for(const auto &[name, positioned] : named)
      if(positioned) destination.arrivals.emplace_back(name);
    return destination;
  }

  std::optional<std::string_view> ScenePreview::draw_door_field(const fs::path &root, Documents &documents,
                                                               bool scene_field, const char *value, const char *scene,
                                                               bool editable) {
    const auto *destination = *scene && std::string_view(scene).size() < sdk::string_field_size
      ? &door_destination(root, scene) : nullptr;
    const bool valid_scene = destination && destination->error.empty();
    if(std::string_view(value).size() >= sdk::string_field_size)
      ImGui::TextWrapped("Door fields can hold at most %d UTF-8 bytes.", sdk::string_field_size - 1);
    else if(scene_field && destination && !valid_scene)
      ImGui::TextWrapped("Destination scene: %s", destination->error.c_str());
    else if(!scene_field && *value) {
      if(!valid_scene) ImGui::TextWrapped("Cannot resolve this arrival without a valid destination scene.");
      else if(!std::binary_search(destination->arrivals.begin(), destination->arrivals.end(), value))
        ImGui::TextWrapped("No entity with this name and Transform2D in the saved destination scene.");
    }
    if(!editable) return std::nullopt;
    const char *blank = scene_field ? "No destination scene (blank)" : "No arrival name (blank)";
    return pick_string(blank, value, [&](const auto &option) {
      if(scene_field) {
        if(ImGui::IsWindowAppearing()) documents.refresh_files();
        for(const auto &path : documents.scene_paths()) option(path);
        if(documents.scene_paths().empty()) ImGui::TextDisabled("No .scene files found.");
      } else {
        if(destination && ImGui::IsWindowAppearing()) door_destination(root, scene, true);
        if(!destination || !destination->error.empty()) {
          ImGui::TextDisabled("Choose a valid destination scene to list arrivals.");
        } else {
          for(const auto &name : destination->arrivals) option(name);
          if(destination->arrivals.empty()) ImGui::TextDisabled("No named entities with Transform2D in the saved scene.");
        }
      }
    });
  }

  const std::bitset<256> *ScenePreview::shown_tile_property() const {
    if(shown_tile_property_.empty()) return nullptr;
    const auto found = map_->tile_properties.find(shown_tile_property_);
    return found == map_->tile_properties.end() ? nullptr : &found->second;
  }

  bool ScenePreview::can_validate_route() const {
    return !map_only_ && tiles().map() && map_->tile_properties.find("path") != map_->tile_properties.end();
  }

  void ScenePreview::refresh_route() {
    const std::array<uint64_t, 3> revisions{revision_, map_->revision, map_->load_revision};
    if(route_revision_ == revisions) return;
    route_revision_ = revisions;
    route_cells_.clear();
    if(markers_dirty_) refresh_markers();

    const auto &map = *tiles().map();
    const auto &objects = scene_.objects();
    const auto endpoint = [&](std::string_view name, sdk::RouteCell &cell) -> const char * {
      const bool spawn = name == "spawn";
      const auto marker = std::find_if(markers_.begin(), markers_.end(), [&](const auto &marker) {
        return objects[marker.object].name == name;
      });
      if(marker == markers_.end())
        return spawn ? "No entity named 'spawn' with Transform2D." : "No entity named 'goal' with Transform2D.";
      if(sdk::centered_route_cell(map.bounds.w, map.bounds.h, map.tile_size(), marker->x, marker->y, cell))
        return nullptr;
      if(cell.x < 0)
        return spawn ? "'spawn' is outside the map." : "'goal' is outside the map.";
      return spawn ? "Center 'spawn' on its tile." : "Center 'goal' on its tile.";
    };
    sdk::RouteCell spawn{-1, -1}, goal{-1, -1};
    if(const auto *error = endpoint("spawn", spawn)) {
      route_issue_ = {error, spawn};
      return;
    }
    if(const auto *error = endpoint("goal", goal)) {
      route_issue_ = {error, goal};
      return;
    }
    const auto &property = map_->tile_properties.find("path")->second;
    route_issue_ = sdk::trace_route(map.bounds.w, map.bounds.h, spawn, goal, [&](sdk::RouteCell cell) {
      const auto index = size_t(cell.y) * map.bounds.w + cell.x;
      for(const auto &entry : map.layers) {
        const auto id = entry.second.tiles[index];
        if(id && property.test(id)) return true;
      }
      return false;
    }, [&](sdk::RouteCell cell) { route_cells_.push_back(cell); });
    if(route_issue_.message) route_cells_.clear();
  }

  void ScenePreview::draw_viewport(const Project &project, ImGuiKey frame_key, ImGuiKey pan_key) {
    const auto reload_view = [&] {
      if(map_only_ || !dirty(true)) discard_map(project);
      if(!map_only_) reload(project);
    };
    ImGui::BeginDisabled(dirty(map_only_));
    if(ImGui::SmallButton("Reload")) reload_view();
    ImGui::EndDisabled();
    if(dirty(map_only_)) ImGui::SetItemTooltip("Save or use File > Revert before reloading.");
    if(!ready(project)) return;
    prepare_map(project);
    ImGui::SameLine();
    const bool frame = !map_only_ && ImGui::SmallButton("Frame");
    if(!map_only_) {
      ImGui::SetItemTooltip("Frame Selection (%s)", ImGui::GetKeyName(frame_key));
      ImGui::SameLine();
    }
    const bool fit = ImGui::SmallButton(map_only_ ? "Fit Map" : "Fit Screen");
    ImGui::SameLine();
    if(ImGui::SmallButton("View")) {
      std::copy_n(screen_size_, 2, screen_draft_);
      ImGui::OpenPopup("Viewport options");
    }
    if(ImGui::BeginPopup("Viewport options")) {
      ImGui::Checkbox("Grid", &show_grid_);
      if(!map_only_) {
        const bool route_available = can_validate_route();
        ImGui::BeginDisabled(!route_available && !show_route_);
        ImGui::Checkbox("Validate Route", &show_route_);
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Trace from 'spawn' to 'goal' using the saved 'path' tile property on all layers.\nUses the current scene and tile edits without saving.");
        ImGui::Checkbox("Snap", &snap_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        if(ImGui::InputInt("Grid px", &grid_size_, 0, 0)) grid_size_ = std::clamp(grid_size_, 1, 512);
        ImGui::SetItemTooltip("Entity grid and snapping in world pixels. In Paint mode the grid follows the tileset.");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputInt2("Game width / height", screen_draft_);
        if(ImGui::Button("Apply Game Size")) {
          for(int i = 0; i < 2; ++i) screen_size_[i] = std::clamp(screen_draft_[i], 96, 4096);
        }
      }
      ImGui::SeparatorText("Show Tile Property");
      if(tiles().map() && !map_->tile_properties.empty()) {
        const auto *shown = shown_tile_property() ? shown_tile_property_.c_str() : "";
        if(const auto selection = pick_string("Off", shown, [&](const auto &option) {
          for(const auto &entry : map_->tile_properties) option(entry.first);
        }, false)) shown_tile_property_.assign(*selection);
        ImGui::TextDisabled("Checks all layers, including hidden ones.");
      } else ImGui::TextDisabled("No tile properties on this map.");
      ImGui::EndPopup();
    }
    const char *paint_hint = nullptr;
    if(tiles().map()) {
      ImGui::SameLine();
      if(ImGui::SmallButton(map_->paint ? "Paint: on###paint" : "Paint: off###paint")) {
        map_->paint = !map_->paint;
        map_->paint_initialized = true;
        dragging_ = sdk::scene_end;
      }
      if(!map_->paint) {
        ImGui::SetItemTooltip("%s", map_only_ ? "Turn on to paint tiles instead of panning." :
                                               "Turn on to paint tiles instead of moving entities.");
      } else if(!(paint_hint = paint_problem())) {
        const auto &layer = tiles().map()->layer_order[map_->layer];
        if(map_->brush == 0) ImGui::SetItemTooltip("Eraser -> %s", layer.c_str());
        else ImGui::SetItemTooltip("Tile %d -> %s", map_->brush, layer.c_str());
      }
    }
    ImGui::SameLine();
    const bool map_status = map_only_ || paint_mode();
    ImGui::TextDisabled("%s", map_status ? (tiles().map() ? (dirty(true) ? "Map unsaved" : "Map saved") : "Map unavailable") :
                                          (dirty(false) ? "Scene unsaved" : "Scene saved"));
    if(paint_hint) ImGui::TextWrapped("%s", paint_hint);
    if(!notice_.empty()) ImGui::TextWrapped("%s", notice_.c_str());
    if(!map_->metadata_error.empty()) ImGui::TextWrapped("%s", map_->metadata_error.c_str());
    if(!map_->save_error.empty()) ImGui::TextWrapped("%s", map_->save_error.c_str());
    draw_canvas(project.root(), frame, fit, frame_key, pan_key);
  }

  void ScenePreview::draw_objects(const Project &project, bool prefabs, Documents &documents) {
    if(map_only_) {
      ImGui::TextDisabled("Entities and prefabs belong to scene tabs.");
      return;
    }
    if(!ready(project)) return;
    if(writable_) draw_add_object(prefabs);
    if(!notice_.empty()) ImGui::TextWrapped("%s", notice_.c_str());
    bool empty = true;
    for(uint32_t i = 0; i < scene_.objects().size(); ++i) {
      const auto &entity = scene_.objects()[i];
      if(entity.prefab != prefabs) continue;
      empty = false;
      ImGui::PushID(entity.key);
      const char *label = *entity.name ? entity.name : entity.key;
      if(prefabs) {
        if(ImGui::TreeNodeEx("object", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label)) {
          draw_object(project.root(), documents, i);
          ImGui::TreePop();
        }
      } else if(ImGui::Selectable(label, selected_ == i)) selected_ = i;
      ImGui::PopID();
    }
    if(empty) ImGui::TextDisabled("%s", prefabs ? "No prefabs." : "No placed entities.");
    if(!prefabs && selected_ != sdk::scene_end) {
      ImGui::PushID(scene_.objects()[selected_].key);
      const uint32_t index = selected_;
      if(writable_ && scene_.objects()[index].source == 0) {
        if(ImGui::SmallButton("Duplicate")) { object_key_.clear(); ImGui::OpenPopup("Duplicate entity"); }
        if(ImGui::BeginPopup("Duplicate entity")) {
          ImGui::InputText("New key", &object_key_);
          if(ImGui::Button("Create copy") && edit([&](auto &error) { return scene_.duplicate_entity(index, object_key_, error); })) {
            selected_ = static_cast<uint32_t>(scene_.objects().size() - 1);
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndPopup();
        }
        ImGui::SameLine();
        if(ImGui::SmallButton("Delete") && edit([&](auto &error) { return scene_.delete_entity(index, error); })) {
          selected_ = sdk::scene_end;
          ImGui::PopID();
          return;
        }
      }
      if(selected_ != index) { ImGui::PopID(); return; }
      ImGui::SeparatorText(*scene_.objects()[selected_].name ? scene_.objects()[selected_].name : scene_.objects()[selected_].key);
      draw_object(project.root(), documents, selected_);
      ImGui::PopID();
    }
  }

  void ScenePreview::refresh_layers(const Project &project) {
    map_->loaded = true;
    ++map_->load_revision;
    map_->tileset.clear();
    map_->metadata_error.clear();
    map_->tile_properties.clear();
    const auto path = scene_path(project.root(), map_->path.generic_u8string());
    map_->writable = !path.empty() && path == project.root() / map_->path;
    if(path.empty() && !tiles().map()) {
      if(map_only_) tiles().load(renderer_, {}, {}, {});
      return;
    }

    fs::path atlas;
    std::string transparent;
    int tile_size = blit::default_tile_size;
    try {
      auto sidecar = map_->path;
      sidecar += ".json";
      std::string text;
      if(!read_text(scene_path(project.root(), sidecar.generic_u8string()), text))
        throw std::runtime_error("Missing or unreadable " + sidecar.generic_u8string());
      const auto relative = nlohmann::json::parse(text).at("tileset").get<std::string>();
      constexpr std::string_view suffix = ".tileset.json";
      if(relative.size() <= suffix.size() || relative.compare(relative.size() - suffix.size(), suffix.size(), suffix) != 0 ||
         !read_text(scene_path(project.root(), relative), text))
        throw std::runtime_error("Cannot read tileset descriptor: " + relative);
      map_->tileset = fs::u8path(relative).lexically_normal().generic_u8string();
      sdk::TilesetDescriptor tileset;
      if(const auto error = sdk::read_tileset_descriptor(nlohmann::json::parse(text), tileset))
        throw std::runtime_error(error);
      tile_size = tileset.tile_size;
      atlas = scene_path(project.root(), tileset.image);
      if(atlas.empty()) throw std::runtime_error("Cannot read tileset image: " + tileset.image);
      transparent = std::move(tileset.transparent);
      map_->tile_properties = std::move(tileset.properties);
    } catch(const std::exception &error) {
      map_->metadata_error = "Map tileset: " + std::string(error.what());
      atlas.clear();
    }
    maps_.retire_image(tiles().release_atlas());
    tiles().load(renderer_, path, atlas, transparent, tile_size);
    if(map_->brush > tiles().tile_count()) map_->brush = -1;
  }

  void ScenePreview::map_history(bool redo) {
    auto &from = redo ? map_->redo : map_->undo;
    if(from.empty()) return;
    auto change = std::move(from.back());
    from.pop_back();
    auto &map = *tiles().map();
    if(change.order) {
      std::swap(map.layers.at(map.layer_order[change.layer]).layer, *change.order);
    } else if(change.added_layer.empty()) {
      auto &layer = map.layers.at(map.layer_order[change.layer]);
      std::swap(layer.tiles[change.cell], change.tile);
      std::swap(layer.transforms[change.cell], change.transform);
    } else if(redo) {
      tiles().append_layer(change.added_layer);
      map_->layer = map.layer_order.size() - 1;
    } else {
      tiles().pop_layer();
      map_->layer = change.layer;
    }
    std::swap(map_->revision, change.revision);
    (redo ? map_->undo : map_->redo).push_back(std::move(change));
    map_->save_error.clear();
    map_->order_error = false;
  }

  const char *ScenePreview::paint_problem() const {
    if(!map_->writable) return "This map is read-only.";
    const int count = tiles().tile_count();
    const auto *map = tiles().map();
    if(map->layer_order.empty()) return "This map has no layers.";
    const bool no_tile = map_->brush < 0 || map_->brush > count;
    if(map_->layer >= map->layer_order.size())
      return no_tile ? "Pick a tile in Tilesets and a layer in Layers." : "Pick a paint target in Layers.";
    if(!tiles().visible(map_->layer))
      return no_tile ? "Show the target layer and pick a tile in Tilesets." : "Show the target layer in Layers to paint.";
    return no_tile ? "Pick a tile or Eraser in Tilesets." : nullptr;
  }

  void ScenePreview::paint_cell(double x, double y) {
    auto *map = tiles().map();
    if(!map || paint_problem()) return;
    const auto world = map->pixel_size();
    if(x < 0 || y < 0 || x >= world.w || y >= world.h) return;
    const auto cell = static_cast<size_t>(map->tile_index(map->cell_at({static_cast<int>(x), static_cast<int>(y)})));
    auto &layer = map->layers.at(map->layer_order[map_->layer]);
    const auto tile = static_cast<uint8_t>(map_->brush);
    if(layer.tiles[cell] == tile && !layer.transforms[cell]) return;
    if(map_->undo.size() == 20) map_->undo.erase(map_->undo.begin());
    map_->undo.push_back({map_->layer, cell, layer.tiles[cell], layer.transforms[cell], map_->revision});
    map_->redo.clear();
    layer.tiles[cell] = tile;
    layer.transforms[cell] = 0;
    map_->revision = ++map_->next_revision;
  }

  void ScenePreview::draw_layers(const Project &project) {
    if(!map_only_ && ImGui::Checkbox("Entities", &show_entities_)) dragging_ = sdk::scene_end;
    prepare_map(project);
    if(!tiles().map() && tiles().error().empty()) ImGui::TextDisabled("No native map for this scene.");
    else {
      const auto previous_target = map_->layer;
      if(tiles().map() && map_->writable && ImGui::SmallButton("Add Layer")) {
        map_->new_layer.clear();
        map_->layer_error.clear();
        ImGui::OpenPopup("New map layer");
      }
      if(ImGui::BeginPopup("New map layer")) {
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("Layer name", &map_->new_layer, ImGuiInputTextFlags_EnterReturnsTrue);
        if(ImGui::Button("Create") || enter) {
          try {
            tiles().append_layer(map_->new_layer);
            if(map_->undo.size() == 20) map_->undo.erase(map_->undo.begin());
            map_->undo.push_back({map_->layer, 0, 0, 0, map_->revision, map_->new_layer});
            map_->redo.clear();
            map_->revision = ++map_->next_revision;
            map_->layer = tiles().map()->layer_order.size() - 1;
            ImGui::CloseCurrentPopup();
          } catch(const std::exception &error) { map_->layer_error = error.what(); }
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        if(!map_->layer_error.empty()) ImGui::TextWrapped("%s", map_->layer_error.c_str());
        ImGui::EndPopup();
      }
      tiles().draw_layers(map_->layer);
      auto *map = tiles().map();
      if(map_->layer != previous_target) map_->order_error = false;
      if(map && map_->layer < map->layer_order.size()) {
        ImGui::PushID(static_cast<int>(map_->layer));
        auto &order = map->layers.at(map->layer_order[map_->layer]).layer;
        char value[sdk::max_value_text];
        sdk::format_value(sdk::FieldType::Int32, &order, value, sizeof(value));
        const ImGuiInputTextFlags flags = map_->writable ? ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
                                                        : ImGuiInputTextFlags_ReadOnly;
        if(ImGui::InputText("Order", value, sizeof(value), flags)) {
          int32_t updated;
          map_->order_error = !sdk::parse_value(sdk::FieldType::Int32, value, &updated);
          if(!map_->order_error && updated != order) {
            if(map_->undo.size() == 20) map_->undo.erase(map_->undo.begin());
            map_->undo.push_back({map_->layer, 0, 0, 0, map_->revision, {}, order});
            map_->redo.clear();
            order = updated;
            map_->revision = ++map_->next_revision;
            map_->save_error.clear();
          }
        }
        ImGui::SetItemTooltip("Lower values draw in front. Press Enter to apply.");
        if(map_->order_error) ImGui::TextWrapped("Enter a whole number from -2147483648 to 2147483647.");
        ImGui::PopID();
      }
    }
  }
}
