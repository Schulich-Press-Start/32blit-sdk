#include <gtest/gtest.h>
#include "tile_props.hpp"

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
