#pragma once

#include "preview_image.hpp"
#include "tileset_descriptor.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace editor {
  class TilesetAssets {
  public:
    TilesetAssets(SDL_Renderer *renderer, const std::filesystem::path &root);
    void open(const std::string &path);
    void draw(const std::string &map_tileset, int brush, const std::function<void(int)> &select_brush,
              const std::function<void()> &refresh_files, const std::function<std::vector<std::string>()> &list_pngs);
    const std::string &active_path() const { return active_tab_; }
    void revert(const std::string &path);
    bool active_dirty() const;
    void save(bool all);
    bool dirty() const;

  private:
    struct Tab {
      sdk::TilesetDescriptor descriptor;
      std::string original, error, delete_property;
      std::array<char, 7> transparent{};
      std::array<char, 128> property_name{};
      PreviewImage preview;
      int selected_tile = 1;
      bool loaded = false, dirty = false;
    };

    void load(const std::string &path, Tab &tab);
    void save(const std::string &path, Tab &tab);
    void draw_tiles(Tab &tab, bool matches_map, int brush, const std::function<void(int)> &select_brush);
    void draw_properties(Tab &tab);
    void draw_import();
    void choose_png(std::string path);
    void import_png();

    SDL_Renderer *renderer_;
    std::filesystem::path root_;
    std::map<std::string, Tab> tabs_;
    std::string active_tab_, pending_tab_, map_tileset_, map_tab_, error_, import_error_;
    bool refresh_pending_ = false;
    std::vector<std::string> project_pngs_;
    std::string import_source_, import_descriptor_, import_image_;
  };
}
