#include "tile_preview.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace editor {
  namespace {
    std::vector<uint8_t> read_asset(const std::filesystem::path &path) {
      std::ifstream in(path, std::ios::binary | std::ios::ate);
      const auto size = in.tellg();
      if(size <= 0 || size > 16 * 1024 * 1024) return {};
      std::vector<uint8_t> bytes(static_cast<size_t>(size));
      if(!in.seekg(0).read(reinterpret_cast<char *>(bytes.data()), size)) return {};
      return bytes;
    }
  }

  void TilePreview::load(SDL_Renderer *renderer, const std::filesystem::path &map, const std::filesystem::path &atlas,
                         std::string_view transparent, int tile_size) {
    error_.clear();
    atlas_ = {};
    if(!map_) {
      if(map.empty()) { error_ = "Map file is missing or outside the project."; return; }
      original_ = read_asset(map);
      map_ = blit::load_tiled_map(original_.data(), original_.size());
      visible_.assign(map_ ? map_->layer_order.size() : 0, 1);
    }
    if(!map_) { error_ = "Cannot read native tilemap: " + map.filename().u8string(); return; }
    if(!map_->set_tile_size(tile_size)) { error_ = "Tile size exceeds the map's supported world dimensions."; return; }
    if(atlas.empty()) {
      error_ = "No atlas image available for the associated tileset.";
      return;
    }
    const auto bytes = read_asset(atlas);
    if(bytes.empty()) { error_ = "Cannot read atlas: " + atlas.filename().u8string(); return; }
    error_ = atlas_.load_png(renderer, bytes, transparent);
    if(!error_.empty()) { error_ += " Atlas: " + atlas.filename().u8string(); return; }
    if(!tile_count()) {
      error_ = "Atlas must contain at most 256 whole square cells of the selected size: " + atlas.filename().u8string();
      atlas_ = {};
    }
  }

  void TilePreview::draw_layers(size_t &selected) {
    if(!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
    if(!map_) return;
    for(size_t i = 0; i < visible_.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      bool visible = visible_[i] != 0;
      if(ImGui::Checkbox("##visible", &visible)) visible_[i] = visible;
      ImGui::SameLine();
      if(ImGui::Selectable(map_->layer_order[i].c_str(), selected == i)) selected = i;
      ImGui::PopID();
    }
  }

  bool TilePreview::save(const std::filesystem::path &path, std::string &error) {
    try {
      if(!map_ || read_asset(path) != original_) throw std::runtime_error("Map changed on disk; reload before saving.");
      auto output = blit::write_map(*map_);
      if(output.empty() || output.size() > 16 * 1024 * 1024) throw std::runtime_error("Cannot serialize native map.");
      auto directory = path;
      directory += ".editor-save";
      const auto staged = directory / path.filename();
      if(!std::filesystem::create_directory(directory)) throw std::runtime_error("Map save staging directory already exists.");
      struct Cleanup {
        const std::filesystem::path &file, &directory;
        ~Cleanup() { std::error_code error; std::filesystem::remove(file, error); std::filesystem::remove(directory, error); }
      } cleanup{staged, directory};
      std::ofstream out(staged, std::ios::binary);
      out.write(reinterpret_cast<const char *>(output.data()), static_cast<std::streamsize>(output.size()));
      out.close();
      if(!out) throw std::runtime_error("Could not write staged map.");
      if(read_asset(path) != original_) throw std::runtime_error("Map changed on disk; reload before saving.");
      std::filesystem::rename(staged, path);
      original_ = std::move(output);
      error.clear();
      return true;
    } catch(const std::exception &failure) { error = failure.what(); return false; }
  }

  void TilePreview::draw(ImDrawList &draw, ImVec2 top, ImVec2 bottom, double x, double y, double zoom) const {
    if(!atlas_.texture) return;
    const ImVec2 center((top.x + bottom.x) * .5f, (top.y + bottom.y) * .5f);
    const auto pos = [&](double px, double py) {
      return ImVec2(float(center.x + (px - x) * zoom), float(center.y + (py - y) * zoom));
    };
    const int tile_size = map_->tile_size();
    const auto cell = [zoom, tile_size](double offset, double camera, int limit, bool upper) {
      const double tile = (camera + offset / zoom) / tile_size;
      return static_cast<int>(std::clamp(upper ? std::ceil(tile) : std::floor(tile), 0.0, double(limit)));
    };
    const int left = cell(top.x - center.x, x, map_->bounds.w, false);
    const int right = cell(bottom.x - center.x, x, map_->bounds.w, true);
    const int first = cell(top.y - center.y, y, map_->bounds.h, false);
    const int last = cell(bottom.y - center.y, y, map_->bounds.h, true);
    if(left >= right || first >= last) return;
    const int columns = atlas_.width / tile_size;
    const int tile_count = this->tile_count();
    const auto texture = reinterpret_cast<ImTextureID>(atlas_.texture.get());
    const float rw = 1.0f / atlas_.width, rh = 1.0f / atlas_.height;
    for(size_t i = 0; i < visible_.size(); ++i) {
      if(!visible_[i]) continue;
      const auto &layer = map_->layers.at(map_->layer_order[i]);
      for(int row = first; row < last; ++row) {
        auto index = row * map_->bounds.w + left;
        for(int col = left; col < right; ++col, ++index) {
          const int tile = int(layer.tiles[index]) - 1;
          if(tile < 0 || tile >= tile_count) continue;
          const auto a = pos(col * tile_size, row * tile_size);
          const auto b = pos((col + 1) * tile_size, (row + 1) * tile_size);
          const int sx = (tile % columns) * tile_size, sy = (tile / columns) * tile_size;
          const auto transform = layer.transforms[index];
          // Same inverse sampling order as MapLayer::texture_span.
          const auto uv = [&](int u, int v) {
            if(transform & 2) v = tile_size - v;
            if(transform & 4) u = tile_size - u;
            if(transform & 1) std::swap(u, v);
            return ImVec2((sx + u) * rw, (sy + v) * rh);
          };
          draw.AddImageQuad(texture, a, {b.x, a.y}, b, {a.x, b.y},
                            uv(0, 0), uv(tile_size, 0), uv(tile_size, tile_size), uv(0, tile_size));
        }
      }
    }
  }
}
