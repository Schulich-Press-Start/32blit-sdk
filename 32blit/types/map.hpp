#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <string>
#include <map>
#include <vector>
#include <functional>

#include "../types/rect.hpp"
#include "../graphics/surface.hpp"
#include "tile_geometry.hpp"

namespace blit {
  struct Map;

  struct MapLayer {
    Map *map;

    std::string name;
    std::vector<uint8_t> tiles;
    std::vector<uint8_t> transforms;

    void add_flags(uint8_t t, uint8_t f);
    void add_flags(std::vector<uint8_t> t, uint8_t f);

    uint8_t tile_at(blit::Point p);
    uint8_t transform_at(blit::Point p);

    // Draw tiles intersecting the world-space viewport, mapping its origin to destination.
    void draw(Surface &dest, Surface &atlas, Rect viewport, Point destination = {});
    void mipmap_texture_span(blit::Surface *dest, blit::Point s, uint16_t c, blit::Surface *sprites, Vec2 swc, Vec2 ewc);
    void texture_span(blit::Surface *dest, blit::Point s, uint16_t c, blit::Surface *sprites, Vec2 swc, Vec2 ewc, uint8_t mipmap_index = 0);

    // Shared world draw order; smaller values draw nearer the front.
    int32_t layer = 1;
  };

  struct Map {
    blit::Rect bounds;

    std::map<std::string, MapLayer> layers;
    // Document order for packed maps (the tie-break for equal orders); empty for ad hoc maps.
    std::vector<std::string> layer_order;
    std::vector<uint8_t> flags;

    Map(blit::Rect bounds);

    // BTMP doesn't store tile size. Configure world geometry from the associated tileset.
    bool set_tile_size(int size);
    int tile_size() const { return tile_size_; }
    Size pixel_size() const { return {bounds.w * tile_size_, bounds.h * tile_size_}; }
    Point cell_at(Point pixel) const;
    Rect cell_rect(Point cell) const;

    void add_layer(std::string name, std::vector<uint8_t> tiles);
    int32_t tile_index(blit::Point p);

    uint8_t get_flags(blit::Point p);
    bool has_flag(blit::Point p, uint8_t f);

    void tiles_in_rect(blit::Rect r, std::function<void(blit::Point)> f);
    bool set_tile(const std::string &layer, Point cell, uint8_t tile, uint8_t transform = 0);

  private:
    int tile_size_ = default_tile_size;
  };

  // Decodes BTMP v1 or v2; returns nullptr for malformed data. V1 layers get N through 1 in document order.
  std::unique_ptr<Map> load_tiled_map(const uint8_t *data, std::size_t size);
  // Serialize BTMP v2 in document order, with signed LE32 order after each name. Empty on invalid input.
  std::vector<uint8_t> write_map(const Map &map);

  // Extracts the second-line marker from a generated Tiled scene. The view
  // borrows the scene text; the caller must validate and resolve the path.
  std::string_view tiled_map_path(std::string_view generated_scene);
}

inline void blit::MapLayer::draw(Surface &dest, Surface &atlas, Rect viewport, Point destination) {
  const int size = map->tile_size();
  const int count = atlas_tile_count(atlas.bounds.w, atlas.bounds.h, size);
  if(!count || viewport.empty()) return;
  const int columns = atlas.bounds.w / size;
  const auto clip = dest.clip;
  dest.clip = clip.intersection({destination.x, destination.y, viewport.w, viewport.h});
  const auto world = map->pixel_size();
  const auto area = viewport.intersection({0, 0, world.w, world.h});
  if(!dest.clip.empty() && !area.empty()) {
    const auto first = map->cell_at({area.x, area.y});
    const auto last = map->cell_at({area.x + area.w - 1, area.y + area.h - 1});
    for(int y = first.y; y <= last.y; ++y) {
      for(int x = first.x; x <= last.x; ++x) {
        const Point cell(x, y);
        const int id = tile_at(cell) - 1;
        if(id < 0 || id >= count) continue;
        // BTMP stores horizontal/vertical/swap in bits 2/1/0; Surface uses 0/1/2.
        const auto bits = transform_at(cell);
        const int transform = ((bits & 4) >> 2) | (bits & 2) | ((bits & 1) << 2);
        dest.blit(&atlas, {id % columns * size, id / columns * size, size, size},
                  {destination.x + x * size - viewport.x, destination.y + y * size - viewport.y}, transform);
      }
    }
  }
  dest.clip = clip;
}
