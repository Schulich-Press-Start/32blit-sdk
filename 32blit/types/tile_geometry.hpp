#pragma once

#include <array>
#include <cstdint>

namespace blit {
  inline constexpr int default_tile_size = 8;
  inline constexpr int max_tile_size = 65535;
  inline constexpr int atlas_tile_id_count = 256;
  using TileFrameMap = std::array<uint8_t, atlas_tile_id_count>;

  constexpr bool valid_tile_size(int size) { return size > 0 && size <= max_tile_size; }

  // BTMP uses IDs 1–255; tolerate the unused last cell of a legacy 256-cell atlas.
  constexpr int atlas_tile_count(int width, int height, int size) {
    if(!valid_tile_size(size) || width <= 0 || height <= 0 || width % size || height % size)
      return 0;
    const int columns = width / size, rows = height / size;
    if(columns > atlas_tile_id_count / rows) return 0;
    const int count = columns * rows;
    return count == atlas_tile_id_count ? atlas_tile_id_count - 1 : count;
  }
}
