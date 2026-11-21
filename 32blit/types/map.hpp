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
  };

  struct Map {
    blit::Rect bounds;

    std::map<std::string, MapLayer> layers;
    // TMX document order for packed maps; empty for ad hoc maps.
    std::vector<std::string> layer_order;
    std::vector<uint8_t> flags;

    Map(blit::Rect bounds);

    // BTMP stores cells only. Configure world geometry from the associated tileset.
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

  // Decodes a complete BTMP v1 asset; returns nullptr for malformed data.
  std::unique_ptr<Map> load_tiled_map(const uint8_t *data, std::size_t size);
  // Serialize native BTMP layers in draw order. Empty on invalid input.
  std::vector<uint8_t> write_map(const Map &map);

  // Extracts the second-line marker from a generated Tiled scene. The view
  // borrows the scene text; the caller must validate and resolve the path.
  std::string_view tiled_map_path(std::string_view generated_scene);
}
