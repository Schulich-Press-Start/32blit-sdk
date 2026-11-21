#pragma once

#include "scene.hpp"
#include "sprite_clips.hpp"
#include "tile_preview.hpp"
#include <array>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace editor {
  class Project;

  struct MapDocument;
  class MapDocuments {
  public:
    explicit MapDocuments(const std::filesystem::path &root) : root_(root) {}
    void finish_frame();

  private:
    friend class ScenePreview;
    void retire_atlas(TilePreview &preview);
    std::filesystem::path root_;
    std::map<std::filesystem::path, std::shared_ptr<MapDocument>> documents_;
    std::vector<PreviewImage> retired_atlases_;
  };

  class ScenePreview {
  public:
    ScenePreview(std::string path, SDL_Renderer *renderer, MapDocuments &maps);
    const std::string &path() const { return path_; }
    bool map_only() const { return map_only_; }
    const std::string &tileset_path() const;
    void select_brush(int tile);
    int selected_brush() const;
    bool other_map_view(const ScenePreview &other) const { return this != &other && map_ == other.map_; }
    bool dirty() const { return dirty_ || dirty(true); }
    bool paint_mode() const;
    bool dirty(bool map) const;
    bool can_history(bool map, bool redo) const;
    void history(bool map, bool redo) { if(map) map_history(redo); else undo_or_redo(redo); }
    void prepare_map(const Project &project);
    void discard_map(const Project &project);
    void save_file(const Project &project, bool map);
    std::string file_path(bool map) const;
    void revert_file(const Project &project, bool map);
    void draw_viewport(const Project &project, ImGuiKey frame_key, ImGuiKey pan_key);
    void draw_objects(const Project &project, bool prefabs);
    void draw_layers(const Project &project);
    void invalidate_maps();

  private:
    void reload(const Project &project);
    void save(const Project &project);
    bool ready(const Project &project);
    void refresh_layers(const Project &project);
    void draw_add_object(bool prefab);
    bool draw_add_component(uint32_t index, const sdk::ComponentInfo *chosen);
    void draw_object(uint32_t index);
    const sdk::SceneObject *prefab_for(const sdk::SceneObject &object) const;
    void draw_canvas(const std::filesystem::path &root, bool frame_selection, bool frame_screen, ImGuiKey frame_key, ImGuiKey pan_key);
    void refresh_markers();
    void move_marker(uint32_t object, double x, double y);
    void changed(bool ok, const sdk::SceneError &error);
    const char *paint_problem() const;
    void paint_cell(double x, double y);
    void map_history(bool redo);
    TilePreview &tiles();
    const TilePreview &tiles() const;
    bool map_only_;
    bool show_grid_ = true;

    struct Revision {
      std::unique_ptr<sdk::SceneDocument> document;
      uint32_t selection;
      uint64_t id;
    };
    Revision snapshot() const;
    void record(std::vector<Revision> &stack, Revision revision);
    void undo_or_redo(bool redo);
    template<class Edit> bool edit(Edit operation) {
      auto before = snapshot();
      sdk::SceneError error;
      const bool ok = operation(error);
      if(ok) {
        record(undo_, std::move(before));
        redo_.clear();
        revision_ = ++next_revision_;
      } else scene_.restore_from(*before.document);
      changed(ok, error);
      return ok;
    }
    std::vector<Revision> undo_, redo_;
    uint64_t revision_ = 0, next_revision_ = 0, saved_revision_ = 0;
    bool snap_ = false;
    int grid_size_ = 8;

    nlohmann::json registry_; // Owns the names borrowed by the reflection records.
    std::array<sdk::ComponentInfo, sdk::max_components> types_;
    std::array<std::array<sdk::FieldInfo, sdk::max_component_fields>, sdk::max_components> fields_;
    sdk::SceneDocument scene_{{}};
    std::string path_, error_, notice_, original_, object_key_, base_key_, draft_;
    struct InitialField {
      std::optional<std::string> default_value;
      std::string value;
      bool enabled = false;
    };
    std::array<InitialField, sdk::max_component_fields> initial_fields_;
    uint16_t add_component_type_ = UINT16_MAX;
    bool writable_ = false, dirty_ = false;
    bool loaded_ = false;
    SDL_Renderer *renderer_;
    MapDocuments &maps_;
    std::shared_ptr<MapDocument> map_;
    bool show_entities_ = true;

    struct SpritePreview { sdk::SpriteClips descriptor; PreviewImage image; };
    const SpritePreview &sprite_preview(const std::filesystem::path &root, std::string_view asset);
    std::map<std::string, SpritePreview, std::less<>> sprite_previews_;
    struct Marker { uint32_t object; float x, y; sdk::Sprite sprite{}; };
    std::vector<Marker> markers_;
    uint32_t selected_ = sdk::scene_end, dragging_ = sdk::scene_end;
    uint16_t transform_ = 0;
    uint8_t x_field_ = 0, y_field_ = 0;
    bool markers_dirty_ = true;
    double camera_x_ = 0, camera_y_ = 0, zoom_ = 0;
    int screen_size_[2] = {320, 240};
    int screen_draft_[2]{};
    int pan_button_ = -1;
  };
}
