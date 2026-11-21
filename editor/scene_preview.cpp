#include "scene_preview.hpp"
#include "builtin_defaults.hpp"
#include "project.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <deque>
#include <fstream>
#include <stdexcept>

namespace editor {
  namespace fs = std::filesystem;

  struct MapDocument {
    explicit MapDocument(fs::path path) : path(std::move(path)) {}

    struct TileEdit { size_t layer, cell; uint8_t tile, transform; uint64_t revision; };
    TilePreview tiles;
    fs::path path;
    std::string tileset, metadata_error, save_error;
    std::vector<TileEdit> undo, redo;
    uint64_t revision = 0, saved_revision = 0, next_revision = 0;
    size_t layer = 0;
    int brush = -1;
    bool loaded = false, paint = false, paint_initialized = false, writable = false;
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

    sdk::FieldType field_type(const std::string &name) {
      using sdk::FieldType;
      for(const auto type : {FieldType::Bool, FieldType::Int32, FieldType::UInt32,
                             FieldType::Float, FieldType::String, FieldType::Entity})
        if(name == sdk::field_type_name(type)) return type;
      throw std::runtime_error("Unsupported registry field type: " + name);
    }
  }

  void MapDocuments::finish_frame() {
    retired_atlases_.clear();
    for(auto it = documents_.begin(); it != documents_.end();) {
      if(it->second.use_count() == 1) it = documents_.erase(it);
      else ++it;
    }
  }

  void MapDocuments::retire_atlas(TilePreview &preview) {
    // Earlier panels can have queued ImGui commands using the old texture.
    auto atlas = preview.release_atlas();
    if(atlas.texture) retired_atlases_.push_back(std::move(atlas));
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
    maps_.retire_atlas(tiles());
    MapDocument fresh(map_->path);
    fresh.paint = map_->paint;
    fresh.paint_initialized = map_->paint_initialized;
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
    markers_dirty_ = true;
    sprite_previews_.clear();
    selected_ = dragging_ = sdk::scene_end;
    add_component_type_ = UINT16_MAX;
    try {
      const auto &path = project.registry();
      std::ifstream input(project.root() / fs::u8path(path));
      if(!input) throw std::runtime_error("Missing " + path + ". Build the project's component registry target and reload.");
      registry_ = nlohmann::json::parse(input);
      if(registry_.at("version") != 1) throw std::runtime_error("Unsupported component registry version.");
      const auto &components = registry_.at("components");
      if(!components.is_object() || components.size() > types_.size()) throw std::runtime_error("Invalid component registry.");
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
        error_ = std::string(error.file) + ':' + std::to_string(error.line) + ": " + error.message;
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

  bool ScenePreview::draw_add_component(uint32_t index, const sdk::ComponentInfo *chosen) {
    if(chosen) {
      add_component_type_ = chosen->id;
      const auto defaults = registry_.find("defaults");
      const nlohmann::json *exported = nullptr;
      if(defaults != registry_.end()) {
        const auto component = defaults->find(chosen->name);
        if(component != defaults->end()) exported = &*component;
      }
      const auto *builtin = sdk::find_component(chosen->name);
      const auto *bytes = builtin ? static_cast<const char *>(sdk::builtin_component_defaults(*builtin)) : nullptr;
      for(uint8_t i = 0; i < chosen->field_count; ++i) {
        const auto &field = chosen->fields[i];
        auto &draft = initial_fields_[i];
        draft = {};
        if(exported) {
          const auto value = exported->find(field.name);
          if(value != exported->end())
            if(const auto *text = value->get_ptr<const std::string *>()) draft.default_value = *text;
        }
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
    for(uint8_t i = 0; i < type->field_count; ++i) {
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

  void ScenePreview::draw_object(uint32_t index) {
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
      if(draw_add_component(index, chosen)) return;
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
      for(uint8_t i = 0; i < type->field_count; ++i) {
        const auto &field = type->fields[i];
        const auto *value = local[i] ? local[i] : values[i];
        ImGui::TextWrapped("%s: %s%s", field.name, value ? (*value ? value : "\"\"") : "Uses component default",
                          local[i] ? " (set here)" : value ? " (inherited)" : "");
        if(!editable) continue;
        ImGui::PushID(i);
        if(ImGui::SmallButton("Edit")) {
          draft_ = value ? value : "";
          notice_.clear();
          ImGui::OpenPopup("Edit scene field");
        }
        bool updated = false;
        if(ImGui::BeginPopup("Edit scene field")) {
          ImGui::Text("%s (%s)", field.name, sdk::field_type_name(field.type));
          if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
          const bool enter = ImGui::InputText("##value", &draft_, ImGuiInputTextFlags_EnterReturnsTrue);
          if(ImGui::Button("Set") || enter) {
            const std::string_view text = draft_;
            updated = edit([&](auto &error) { return scene_.edit_field(index, id, i, &text, error); });
          }
          if(local[i] && !updated) {
            ImGui::SameLine();
            if(ImGui::Button("Reset")) {
              updated = edit([&](auto &error) { return scene_.edit_field(index, id, i, nullptr, error); });
            }
          }
          if(updated) ImGui::CloseCurrentPopup();
          else if(!notice_.empty()) ImGui::TextWrapped("%s", notice_.c_str());
          ImGui::EndPopup();
        }
        ImGui::PopID();
        if(updated) { ImGui::PopID(); return; }
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

  const ScenePreview::SpritePreview &ScenePreview::sprite_preview(const fs::path &root, std::string_view asset) {
    if(const auto found = sprite_previews_.find(asset); found != sprite_previews_.end()) return found->second;
    auto &preview = sprite_previews_[std::string(asset)];
    std::string text;
    if(!read_text(scene_path(root, std::string(asset) + ".sprite.json"), text) ||
       sdk::read_sprite_clips(nlohmann::json::parse(text, nullptr, false), preview.descriptor)) return preview;
    std::ifstream input(scene_path(root, preview.descriptor.image), std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if(size <= 0 || size > 16 * 1024 * 1024) return preview;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if(input.seekg(0).read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
      preview.image.load_png(renderer_, bytes);
    return preview;
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
      if(!map_only_) ImGui::Checkbox("Snap", &snap_);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
      if(ImGui::InputInt("Grid px", &grid_size_, 0, 0)) grid_size_ = std::clamp(grid_size_, 1, 512);
      ImGui::SetItemTooltip("World pixel spacing for the grid and entity snapping. Tile cells stay 8 x 8.");
      if(!map_only_) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        ImGui::InputInt2("Game width / height", screen_draft_);
        if(ImGui::Button("Apply Game Size")) {
          for(int i = 0; i < 2; ++i) screen_size_[i] = std::clamp(screen_draft_[i], 96, 4096);
        }
      }
      ImGui::EndPopup();
    }
    prepare_map(project);
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

  void ScenePreview::draw_objects(const Project &project, bool prefabs) {
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
          draw_object(i);
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
      draw_object(selected_);
      ImGui::PopID();
    }
  }

  void ScenePreview::refresh_layers(const Project &project) {
    map_->loaded = true;
    map_->tileset.clear();
    map_->metadata_error.clear();
    const auto path = scene_path(project.root(), map_->path.generic_u8string());
    map_->writable = !path.empty() && path == project.root() / map_->path;
    if(path.empty() && !tiles().map()) {
      if(map_only_) tiles().load(renderer_, {}, {}, {});
      return;
    }

    fs::path atlas;
    std::string transparent;
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
      const auto tileset = nlohmann::json::parse(text);
      const auto image = tileset.at("image").get<std::string>();
      atlas = scene_path(project.root(), image);
      if(atlas.empty()) throw std::runtime_error("Cannot read tileset image: " + image);
      transparent = tileset.value("transparent", std::string{});
    } catch(const std::exception &error) {
      map_->metadata_error = "Map tileset: " + std::string(error.what());
      atlas.clear();
    }
    maps_.retire_atlas(tiles());
    tiles().load(renderer_, path, atlas, transparent);
    if(map_->brush > tiles().tile_count()) map_->brush = -1;
  }

  void ScenePreview::map_history(bool redo) {
    auto &from = redo ? map_->redo : map_->undo;
    if(from.empty()) return;
    auto change = from.back();
    from.pop_back();
    auto &map = *tiles().map();
    auto &layer = map.layers.at(map.layer_order[change.layer]);
    std::swap(layer.tiles[change.cell], change.tile);
    std::swap(layer.transforms[change.cell], change.transform);
    std::swap(map_->revision, change.revision);
    (redo ? map_->undo : map_->redo).push_back(change);
    map_->save_error.clear();
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
    if(!map || paint_problem() || x < 0 || y < 0 || x >= map->bounds.w * 8 || y >= map->bounds.h * 8) return;
    const auto cell = static_cast<size_t>(y / 8) * map->bounds.w + static_cast<size_t>(x / 8);
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
    else tiles().draw_layers(map_->layer);
  }
}
