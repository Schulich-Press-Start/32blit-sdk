/*! \file mat.cpp
*/
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include "map.hpp"

namespace blit {
  namespace {
    uint16_t read_le16(const uint8_t *data) {
      return uint16_t(data[0]) | (uint16_t(data[1]) << 8);
    }

    bool valid_utf8(const uint8_t *data, std::size_t size) {
      while (size != 0) {
        const uint8_t first = *data++;
        --size;
        if (first < 0x80)
          continue;
        if (first < 0xc2 || first > 0xf4)
          return false;

        const std::size_t count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
        if (size < count)
          return false;
        // Exclude overlong sequences, surrogates, and values above U+10FFFF.
        if ((first == 0xe0 && data[0] < 0xa0) ||
            (first == 0xed && data[0] >= 0xa0) ||
            (first == 0xf0 && data[0] < 0x90) ||
            (first == 0xf4 && data[0] >= 0x90))
          return false;
        for (std::size_t i = 0; i < count; ++i) {
          if ((data[i] & 0xc0) != 0x80)
            return false;
        }
        data += count;
        size -= count;
      }
      return true;
    }
  }

  std::unique_ptr<Map> load_tiled_map(const uint8_t *data, std::size_t size) {
    constexpr std::size_t header_size = 12;
    if (!data || size < header_size || std::memcmp(data, "BTMP", 4) != 0 ||
        read_le16(data + 4) != 1)
      return nullptr;

    const uint16_t width = read_le16(data + 6);
    const uint16_t height = read_le16(data + 8);
    const uint16_t layer_count = read_le16(data + 10);
    // Map multiplies the dimensions and computes tile indices in signed arithmetic.
    if (!width || !height || !layer_count ||
        width > std::numeric_limits<int32_t>::max() / height)
      return nullptr;

    const uint32_t area = uint32_t(width) * height;
    data += header_size;
    size -= header_size;
    // Each layer needs at least a name-length byte and two complete planes.
    const std::size_t layer_budget = size / layer_count;
    if (!layer_budget || area > (layer_budget - 1) / 2)
      return nullptr;

    const std::size_t cells = area;
    auto map = std::make_unique<Map>(Rect(0, 0, width, height));
    map->layer_order.reserve(layer_count);
    for (uint16_t i = 0; i < layer_count; ++i) {
      if (!size)
        return nullptr;
      const uint8_t name_size = *data++;
      --size;
      if (!name_size || name_size > size || (size - name_size) / 2 < cells ||
          std::memchr(data, 0, name_size) || !valid_utf8(data, name_size))
        return nullptr;

      std::string name(reinterpret_cast<const char *>(data), name_size);
      data += name_size;
      const uint8_t *transforms = data + cells;
      const uint8_t *end = transforms + cells;
      if (std::any_of(transforms, end, [](uint8_t value) { return value > 7; }))
        return nullptr;

      auto [it, inserted] = map->layers.try_emplace(name);
      if (!inserted)
        return nullptr;
      auto &layer = it->second;
      layer.map = map.get();
      layer.name = name;
      layer.tiles.assign(data, transforms);
      layer.transforms.assign(transforms, end);
      map->layer_order.push_back(std::move(name));
      data = end;
      size -= name_size + 2 * cells;
    }
    if (size != 0)
      return nullptr;
    return map;
  }

  std::vector<uint8_t> write_map(const Map &map) {
    const auto w = map.bounds.w, h = map.bounds.h;
    if(w <= 0 || h <= 0 || w > 65535 || h > 65535 || w > INT32_MAX / h ||
       map.layer_order.empty() || map.layer_order.size() > 65535 || map.layer_order.size() != map.layers.size()) return {};
    const size_t cells = size_t(w) * h;
    std::vector<uint8_t> bytes{'B','T','M','P',1,0};
    for(auto value : {unsigned(w), unsigned(h), unsigned(map.layer_order.size())}) {
      bytes.push_back(uint8_t(value));
      bytes.push_back(uint8_t(value >> 8));
    }
    for(size_t i = 0; i < map.layer_order.size(); ++i) {
      const auto &name = map.layer_order[i];
      const auto layer = map.layers.find(name);
      if(name.empty() || name.size() > 255 || name.find('\0') != name.npos ||
         !valid_utf8(reinterpret_cast<const uint8_t *>(name.data()), name.size()) ||
         std::find(map.layer_order.begin(), map.layer_order.begin() + i, name) != map.layer_order.begin() + i ||
         layer == map.layers.end() || layer->second.tiles.size() != cells || layer->second.transforms.size() != cells ||
         std::any_of(layer->second.transforms.begin(), layer->second.transforms.end(), [](uint8_t t) { return t > 7; })) return {};
      bytes.push_back(uint8_t(name.size()));
      bytes.insert(bytes.end(), name.begin(), name.end());
      bytes.insert(bytes.end(), layer->second.tiles.begin(), layer->second.tiles.end());
      bytes.insert(bytes.end(), layer->second.transforms.begin(), layer->second.transforms.end());
    }
    return bytes;
  }

  bool Map::set_tile(const std::string &layer, Point cell, uint8_t tile, uint8_t transform) {
    const auto index = tile_index(cell);
    if(index < 0 || transform > 7) return false;
    const auto found = layers.find(layer);
    if(found == layers.end() || size_t(index) >= found->second.tiles.size() ||
       size_t(index) >= found->second.transforms.size()) return false;
    found->second.tiles[index] = tile;
    found->second.transforms[index] = transform;
    return true;
  }

  std::string_view tiled_map_path(std::string_view generated_scene) {
    const auto first_break = generated_scene.find('\n');
    if (first_break == std::string_view::npos)
      return {};

    auto without_cr = [](std::string_view line) {
      if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
      return line;
    };
    if (without_cr(generated_scene.substr(0, first_break)) != "scene 1 Tiled")
      return {};

    generated_scene.remove_prefix(first_break + 1);
    const auto marker = without_cr(generated_scene.substr(0, generated_scene.find('\n')));
    constexpr std::string_view prefix = "# blit-map ";
    if (marker.substr(0, prefix.size()) != prefix)
      return {};
    return marker.substr(prefix.size());
  }


  void Map::add_layer(std::string name, std::vector<uint8_t> tiles) {
    MapLayer &layer = layers[name];
    layer.map = this;
    layer.name = name;
    layer.tiles = tiles;
  }


  Map::Map(Rect bounds) : bounds(bounds) {
    flags.resize(bounds.w * bounds.h);
  }

  bool Map::set_tile_size(int size) {
    if(!valid_tile_size(size) || bounds.w > std::numeric_limits<int32_t>::max() / size ||
       bounds.h > std::numeric_limits<int32_t>::max() / size) return false;
    tile_size_ = size;
    return true;
  }

  Point Map::cell_at(Point pixel) const {
    const auto tile = [size = tile_size_](int32_t p) { return p / size - (p % size < 0); };
    return {tile(pixel.x), tile(pixel.y)};
  }

  Rect Map::cell_rect(Point cell) const {
    return {cell.x * tile_size_, cell.y * tile_size_, tile_size_, tile_size_};
  }

  int32_t Map::tile_index(Point p) {
    return this->bounds.contains(p) ? p.x + p.y * this->bounds.w : -1;
  }

  void MapLayer::add_flags(uint8_t t, uint8_t f) {
    for (uint32_t i = 0; i < this->tiles.size(); i++) {
      if (this->tiles[i] == t)
        map->flags[i] |= f;
    }
  }

  void MapLayer::mipmap_texture_span(Surface *dest, Point s, uint16_t c, Surface *sprites, Vec2 swc, Vec2 ewc) {
    if(!c) return;
    dest->alpha = 255;
    if(sprites->mipmaps.empty() || (map->tile_size() % 2)) {
      texture_span(dest, s, c, sprites, swc, ewc);
      return;
    }
    // calculate the mipmap index to use for drawing
    float span_length = (ewc - swc).length();
    float mipmap = ((span_length / float(c)) / 2.0f);
    unsigned last = static_cast<unsigned>(std::min<std::size_t>(15, sprites->mipmaps.size() - 1));
    while(map->tile_size() % (1 << last)) --last;
    auto mipmap_index = static_cast<unsigned>(std::min(mipmap, float(last)));
    const uint8_t blend = (mipmap - floorf(mipmap)) * 255;
    texture_span(dest, s, c, sprites->mipmaps[mipmap_index], swc, ewc, mipmap_index);

    if (++mipmap_index <= last) {
      dest->alpha = blend;
      texture_span(dest, s, c, sprites->mipmaps[mipmap_index], swc, ewc, mipmap_index);
    }
  }

  void MapLayer::texture_span(Surface *dest, Point s, uint16_t c, Surface *sprites, Vec2 swc, Vec2 ewc, uint8_t mipmap_index) {
    BlitBlendFunc bbf = dest->bbf;

    const auto world = map->pixel_size();
    const int size = map->tile_size();
    if(!c || mipmap_index >= 16 || size % (1 << mipmap_index)) return;
    const int tile_size = size >> mipmap_index;
    const int count = atlas_tile_count(sprites->bounds.w, sprites->bounds.h, tile_size);
    if(!count) return;
    const int columns = sprites->bounds.w / tile_size;

    Vec2 wc = swc;
    Vec2 dwc = (ewc - swc) / float(c);
    for (int x = s.x; x < s.x + c; x++) {
      if (wc.x >= 0 && wc.x < world.w && wc.y >= 0 && wc.y < world.h) {
        const Point pixel(wc.x, wc.y);
        const Point pt(pixel.x / size, pixel.y / size);

        int16_t tile_id = tile_at(pt) - 1;

        if (tile_id >= 0 && tile_id < count) {
          const Point sp(tile_id % columns * tile_size, tile_id / columns * tile_size);

          Point uv((pixel.x % size) >> mipmap_index, (pixel.y % size) >> mipmap_index);

          // apply uv transform for tile
          uint8_t transform = transform_at(pt);
          if (transform & 0b010) { uv.y = (tile_size - 1) - uv.y; }
          if (transform & 0b100) { uv.x = (tile_size - 1) - uv.x; }
          if (transform & 0b001) std::swap(uv.x, uv.y);

          Point t = sp + uv;

          bbf(sprites, sprites->offset(t), dest, dest->offset(x, s.y), 1, 1);
        }
      }

      wc += dwc;
    }
  }

  void MapLayer::add_flags(std::vector<uint8_t> ts, uint8_t f) {
    for (auto t : ts) {
      this->add_flags(t, f);
    }
  }

  uint8_t MapLayer::tile_at(Point p) {
    int32_t ti = map->tile_index(p);
    return ti == -1 ? 0 : tiles[ti];
  }

  uint8_t MapLayer::transform_at(Point p) {
    int32_t ti = map->tile_index(p);
    return ti < 0 || static_cast<std::size_t>(ti) >= transforms.size() ? 0 : transforms[ti];
  }

  uint8_t Map::get_flags(Point p) {
    int32_t ti = tile_index(p);
    return ti == -1 ? 0 : this->flags[ti];
  }

  bool Map::has_flag(Point p, uint8_t f) {
    return this->get_flags(p) & f;
  }

  void Map::tiles_in_rect(Rect vp, std::function<void(Point)> f) {
    const auto first = cell_at({vp.x, vp.y});
    const auto last = cell_at({vp.x + vp.w, vp.y + vp.h});
    int minx = first.x, maxx = last.x;
    int miny = first.y, maxy = last.y;

    minx = minx < 0 ? 0 : minx;
    miny = miny < 0 ? 0 : miny;

    maxx = maxx > bounds.w - 1 ? bounds.w - 1 : maxx;
    maxy = maxy > bounds.h - 1 ? bounds.h - 1 : maxy;

    Point pt;
    for (pt.y = miny; pt.y <= maxy; pt.y++) {
      for (pt.x = minx; pt.x <= maxx; pt.x++) {
        f(pt);
      }
    }
  }
}
