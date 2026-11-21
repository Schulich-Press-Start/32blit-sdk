#include <gtest/gtest.h>
#include "tile_props.hpp"
#include "../../../32blit/types/tile_geometry.hpp"

TEST(TileProps, RejectsIncompatibleTablesWithoutReplacingLiveFlags) {
  sdk::TileProps props;
  std::array<uint8_t, 12 + 256 * 4> bytes{'B', 'T', 'P', '1', 42};
  bytes[12 + 255 * 4 + 3] = 128;
  ASSERT_TRUE(props.load(bytes.data(), bytes.size(), 42));
  EXPECT_TRUE(props.has(255, uint32_t{1} << 31));
  EXPECT_FALSE(props.has(256, uint32_t{1} << 31));
  EXPECT_FALSE(props.has(0, uint32_t{1} << 31));
  EXPECT_FALSE(props.has(255, 0));
  EXPECT_FALSE(props.has(255, (uint32_t{1} << 31) | 1));

  bytes.back() = 0;
  EXPECT_FALSE(props.load(bytes.data(), bytes.size(), 43));
  EXPECT_FALSE(props.load(bytes.data(), bytes.size() - 1, 42));
  bytes[12] = 1;
  EXPECT_FALSE(props.load(bytes.data(), bytes.size(), 42));
  EXPECT_EQ(props.mask(255), uint32_t{1} << 31);
}

TEST(TileGeometry, CountsWholeCellsInAnyNumberOfColumns) {
  EXPECT_EQ(blit::atlas_tile_count(45, 30, 15), 6);
  EXPECT_EQ(blit::atlas_tile_count(255, 1, 1), 255);
  EXPECT_EQ(blit::atlas_tile_count(128, 128, blit::default_tile_size), 255);
  EXPECT_EQ(blit::atlas_tile_count(257, 1, 1), 0);
  EXPECT_EQ(blit::atlas_tile_count(46, 30, 15), 0);
  EXPECT_EQ(blit::atlas_tile_count(45, 31, 15), 0);
  EXPECT_EQ(blit::atlas_tile_count(45, 30, 0), 0);
  EXPECT_EQ(blit::atlas_tile_count(0, 30, 15), 0);
}
