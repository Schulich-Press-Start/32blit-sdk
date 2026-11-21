#pragma once

#include "../32blit/types/map.hpp"
#include "preview_image.hpp"
#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <SDL.h>

namespace editor {
  class TilePreview {
  public:
    void load(SDL_Renderer *renderer, const std::filesystem::path &map, const std::filesystem::path &atlas,
              std::string_view transparent);
    void draw(ImDrawList &draw, ImVec2 top, ImVec2 bottom, double x, double y, double zoom) const;
    void draw_layers(size_t &selected);
    const std::string &error() const { return error_; }
    blit::Map *map() const { return map_.get(); }
    bool visible(size_t layer) const { return layer < visible_.size() && visible_[layer]; }
    int tile_count() const { return std::min(255, (atlas_.width / 8) * (atlas_.height / 8)); }
    bool save(const std::filesystem::path &path, std::string &error);
    PreviewImage release_atlas() { return std::move(atlas_); }

  private:
    std::unique_ptr<blit::Map> map_;
    PreviewImage atlas_;
    std::vector<uint8_t> visible_;
    std::vector<uint8_t> original_;
    std::string error_;
  };
}
