#include <gtest/gtest.h>
#include "../../../32blit/types/map.hpp"

namespace {
  constexpr uint8_t legacy[] = {
    'B', 'T', 'M', 'P', 1, 0, 2, 0, 1, 0, 2, 0,
    1, 'z', 1, 2, 0, 7,
    1, 'a', 3, 4, 2, 5,
  };
  const std::vector<uint8_t> version_two{
    'B', 'T', 'M', 'P', 2, 0, 2, 0, 1, 0, 2, 0,
    1, 'z', 0, 0, 0, 0x80, 1, 2, 0, 7,
    1, 'a', 0xff, 0xff, 0xff, 0x7f, 3, 4, 2, 5,
  };
}

TEST(NativeMap, ReadsLegacyLayersInDocumentOrder) {
  const auto map = blit::load_tiled_map(legacy, sizeof(legacy));
  ASSERT_TRUE(map);
  EXPECT_EQ(map->layer_order, (std::vector<std::string>{"z", "a"}));
  EXPECT_EQ(map->layers.at("z").layer, 2);
  EXPECT_EQ(map->layers.at("a").layer, 1);
  EXPECT_EQ(map->layers.at("z").tiles, (std::vector<uint8_t>{1, 2}));
  EXPECT_EQ(map->layers.at("a").transforms, (std::vector<uint8_t>{2, 5}));
}

TEST(NativeMap, WritesAndReadsSignedLittleEndianOrdersWithoutReorderingTheLayers) {
  const auto map = blit::load_tiled_map(legacy, sizeof(legacy));
  ASSERT_TRUE(map);
  map->layers.at("z").layer = INT32_MIN;
  map->layers.at("a").layer = INT32_MAX;
  EXPECT_EQ(blit::write_map(*map), version_two);

  const auto reopened = blit::load_tiled_map(version_two.data(), version_two.size());
  ASSERT_TRUE(reopened);
  EXPECT_EQ(reopened->layer_order, map->layer_order);
  EXPECT_EQ(reopened->layers.at("z").layer, INT32_MIN);
  EXPECT_EQ(reopened->layers.at("a").layer, INT32_MAX);
  EXPECT_EQ(blit::write_map(*reopened), version_two);
}

TEST(NativeMap, RejectsIncompleteOrUnsupportedAssets) {
  for(std::size_t size = 0; size < version_two.size(); ++size) {
    SCOPED_TRACE(size);
    EXPECT_FALSE(blit::load_tiled_map(version_two.data(), size));
  }
  auto invalid = version_two;
  invalid.push_back(0);
  EXPECT_FALSE(blit::load_tiled_map(invalid.data(), invalid.size()));
  invalid.pop_back();
  invalid[4] = 3;
  EXPECT_FALSE(blit::load_tiled_map(invalid.data(), invalid.size()));
}
