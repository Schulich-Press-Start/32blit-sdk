#include <gtest/gtest.h>

#include <cstddef>
#include <limits>

#include "tile_movement.hpp"

namespace {
  template<std::size_t Rows, std::size_t Stride>
  sdk::TileMoveResult move(sdk::Transform2D &position, const sdk::BoxCollider2D &shape, blit::Vec2 delta,
                           const char (&map)[Rows][Stride], bool block_edges = false) {
    constexpr int columns = int(Stride) - 1, rows = int(Rows);
    return sdk::move_against_tiles(position, shape, delta, columns, rows, 16, [&](blit::Point cell) {
      if(cell.x < 0 || cell.x >= columns || cell.y < 0 || cell.y >= rows) {
        ADD_FAILURE() << "Predicate called for an out-of-bounds cell";
        return false;
      }
      return map[cell.y][cell.x] == '#';
    }, block_edges);
  }
}

TEST(TileMovement, SweepsWholeTilesAndStopsAtTheFirstSolidStrip) {
  sdk::Transform2D position{1, 3, 27};
  int calls = 0;
  const auto result = sdk::move_against_tiles(position, {}, blit::Vec2(95, 0), 8, 4, 16, [&](blit::Point cell) {
    EXPECT_EQ(cell, blit::Point(++calls, 0));
    return cell.x == 3 || cell.x == 4;
  });
  EXPECT_EQ(calls, 3);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FALSE(result.hit_y);
  EXPECT_FLOAT_EQ(result.moved.x, 39);
  EXPECT_FLOAT_EQ(result.moved.y, 0);
  EXPECT_FLOAT_EQ(position.x, 40);
  EXPECT_FLOAT_EQ(position.y, 3);
  EXPECT_FLOAT_EQ(position.rotation, 27);
}

TEST(TileMovement, SweepsNegativeXThenNegativeY) {
  constexpr char map[][8] = {".......", "....#..", ".......", ".......", "...#...", "......."};
  sdk::Transform2D position{84, 72};
  const auto result = move(position, {}, blit::Vec2(-72, -68), map);
  EXPECT_TRUE(result.hit_x);
  EXPECT_TRUE(result.hit_y);
  EXPECT_FLOAT_EQ(result.moved.x, -20);
  EXPECT_FLOAT_EQ(result.moved.y, -40);
  EXPECT_FLOAT_EQ(position.x, 64);
  EXPECT_FLOAT_EQ(position.y, 32);
}

TEST(TileMovement, SlidesAlongAFlushWallAfterBlockingX) {
  constexpr char map[][4] = {".#.", ".#.", ".#.", "..."};
  sdk::Transform2D position{4, 4};
  const auto result = move(position, {}, blit::Vec2(26, 31), map);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FALSE(result.hit_y);
  EXPECT_FLOAT_EQ(result.moved.x, 4);
  EXPECT_FLOAT_EQ(result.moved.y, 31);
  EXPECT_FLOAT_EQ(position.x, 8);
  EXPECT_FLOAT_EQ(position.y, 35);
}

TEST(TileMovement, LandingFlushIsNotAHitButTryingToCrossIs) {
  constexpr char map[][4] = {".#."};
  for(const int direction : {1, -1}) {
    SCOPED_TRACE(direction);
    sdk::Transform2D position{direction > 0 ? 0.0f : 40.0f, 0};
    auto result = move(position, {}, blit::Vec2(float(8 * direction), 0), map);
    EXPECT_FALSE(result.hit_x);
    EXPECT_FLOAT_EQ(result.moved.x, float(8 * direction));

    result = move(position, {}, blit::Vec2(float(direction), 0), map);
    EXPECT_TRUE(result.hit_x);
    EXPECT_FLOAT_EQ(result.moved.x, 0);
    EXPECT_FLOAT_EQ(position.x, direction > 0 ? 8.0f : 32.0f);
  }
}

TEST(TileMovement, ColliderOffsetsAffectTheLeadingEdgeAndThePerpendicularSpan) {
  constexpr char map[][4] = {"...", ".#."};
  sdk::Transform2D position{2, 12};
  const auto result = move(position, {3, 3, 5, 3}, blit::Vec2(27, 0), map);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FLOAT_EQ(result.moved.x, 6);
  EXPECT_FLOAT_EQ(position.x, 8);
  EXPECT_FLOAT_EQ(position.y, 12);
}

TEST(TileMovement, SweepsPositiveYWithAnOddTileSize) {
  sdk::Transform2D position{8.5f, 0};
  const auto result = sdk::move_against_tiles(position, {0, 0, 3, 2.5f}, blit::Vec2(0, 35), 3, 6, 7,
    [](blit::Point cell) { return cell == blit::Point(1, 3); });
  EXPECT_FALSE(result.hit_x);
  EXPECT_TRUE(result.hit_y);
  EXPECT_FLOAT_EQ(result.moved.x, 0);
  EXPECT_FLOAT_EQ(result.moved.y, 18.5f);
  EXPECT_FLOAT_EQ(position.y, 18.5f);
}

TEST(TileMovement, FractionalContactRoundsToTheNonOverlappingSide) {
  constexpr char map[][4] = {".#."};
  constexpr sdk::BoxCollider2D shape{0.2f, 0, 4, 8};
  sdk::Transform2D position{4, 4};
  auto result = move(position, shape, blit::Vec2(80, 0), map);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FLOAT_EQ(result.moved.x, 7.8f);
  EXPECT_LE(double(position.x) + double(shape.offset_x) + double(shape.width), 16.0);

  result = move(position, shape, blit::Vec2(80, 0), map);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FLOAT_EQ(result.moved.x, 0);

  // This request ends exactly flush in real coordinates, though the rounded
  // transform would cross the wall. Store the safe position without a hit.
  position = {11.75f, 4};
  result = move(position, shape, blit::Vec2(0.25f - shape.offset_x, 0), map);
  EXPECT_FALSE(result.hit_x);
  EXPECT_LE(double(position.x) + double(shape.offset_x) + double(shape.width), 16.0);
}

TEST(TileMovement, OpenEdgesAreEmptyAndClosedEdgesClampTheBox) {
  constexpr char map[][3] = {"..", ".."};
  constexpr sdk::BoxCollider2D shape{2, -3, 8, 8};
  sdk::Transform2D position{5, 7};
  auto result = move(position, shape, blit::Vec2(-40, 40), map);
  EXPECT_FALSE(result.hit_x);
  EXPECT_FALSE(result.hit_y);
  EXPECT_FLOAT_EQ(position.x, -35);
  EXPECT_FLOAT_EQ(position.y, 47);
  EXPECT_FLOAT_EQ(result.moved.x, -40);
  EXPECT_FLOAT_EQ(result.moved.y, 40);

  position = {5, 7};
  result = move(position, shape, blit::Vec2(-40, 40), map, true);
  EXPECT_TRUE(result.hit_x);
  EXPECT_TRUE(result.hit_y);
  EXPECT_FLOAT_EQ(position.x, -2);
  EXPECT_FLOAT_EQ(position.y, 27);
  EXPECT_FLOAT_EQ(result.moved.x, -7);
  EXPECT_FLOAT_EQ(result.moved.y, 20);

  result = move(position, shape, blit::Vec2(24, -24), map, true);
  EXPECT_FALSE(result.hit_x);
  EXPECT_FALSE(result.hit_y);
  EXPECT_FLOAT_EQ(position.x, 22);
  EXPECT_FLOAT_EQ(position.y, 3);
}

TEST(TileMovement, AnOpenEdgeStillTestsTheFirstInBoundsTileWhenEntering) {
  constexpr char map[][4] = {"#.."};
  sdk::Transform2D position{-40, 4};
  const auto result = move(position, {}, blit::Vec2(55, 0), map);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FLOAT_EQ(result.moved.x, 32);
  EXPECT_FLOAT_EQ(position.x, -8);
}

TEST(TileMovement, AClosedEdgeDoesNotPushAnAlreadyOutsideBox) {
  constexpr char map[][3] = {"..", ".."};
  sdk::Transform2D position{-10, 30};
  auto result = move(position, {}, blit::Vec2(-6, 4), map, true);
  EXPECT_TRUE(result.hit_x);
  EXPECT_TRUE(result.hit_y);
  EXPECT_FLOAT_EQ(result.moved.x, 0);
  EXPECT_FLOAT_EQ(result.moved.y, 0);

  result = move(position, {}, blit::Vec2(3, -2), map, true);
  EXPECT_FALSE(result.hit_x);
  EXPECT_FALSE(result.hit_y);
  EXPECT_FLOAT_EQ(position.x, -7);
  EXPECT_FLOAT_EQ(position.y, 28);

  result = move(position, {}, blit::Vec2(40, -40), map, true);
  EXPECT_TRUE(result.hit_x);
  EXPECT_TRUE(result.hit_y);
  EXPECT_FLOAT_EQ(position.x, 24);
  EXPECT_FLOAT_EQ(position.y, 0);
}

TEST(TileMovement, AnOversizeBoxOnlyBlocksTheAxisOnWhichItCannotFit) {
  constexpr char map[][3] = {"..", ".."};
  sdk::Transform2D position{2, 4};
  const auto result = move(position, {0, 0, 40, 8}, blit::Vec2(6, 3), map, true);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FALSE(result.hit_y);
  EXPECT_FLOAT_EQ(result.moved.x, 0);
  EXPECT_FLOAT_EQ(result.moved.y, 3);
}

TEST(TileMovement, StartingInsideASolidDoesNotResolveItOrDisableNewContacts) {
  constexpr char map[][5] = {".##."};
  sdk::Transform2D position{12, 4};
  auto result = move(position, {}, blit::Vec2(), map);
  EXPECT_FALSE(result.hit_x);
  EXPECT_FALSE(result.hit_y);
  EXPECT_FLOAT_EQ(position.x, 12);
  EXPECT_FLOAT_EQ(position.y, 4);

  result = move(position, {}, blit::Vec2(40, 0), map);
  EXPECT_TRUE(result.hit_x);
  EXPECT_FLOAT_EQ(position.x, 24);

  result = move(position, {}, blit::Vec2(-20, 0), map);
  EXPECT_FALSE(result.hit_x);
  EXPECT_FLOAT_EQ(result.moved.x, -20);
  EXPECT_FLOAT_EQ(position.x, 4);
}

TEST(TileMovement, InvalidInputsDoNotMoveOrQueryThePredicate) {
  sdk::Transform2D position{4, 6, 31};
  const auto check = [&](const sdk::BoxCollider2D &shape, blit::Vec2 delta, int columns, int rows, int tile_size) {
    const auto result = sdk::move_against_tiles(position, shape, delta, columns, rows, tile_size, [](blit::Point) {
      ADD_FAILURE() << "Predicate called for invalid geometry";
      return true;
    });
    EXPECT_FALSE(result.hit_x);
    EXPECT_FALSE(result.hit_y);
    EXPECT_FLOAT_EQ(result.moved.x, 0);
    EXPECT_FLOAT_EQ(result.moved.y, 0);
    EXPECT_FLOAT_EQ(position.x, 4);
    EXPECT_FLOAT_EQ(position.y, 6);
    EXPECT_FLOAT_EQ(position.rotation, 31);
  };

  check({}, blit::Vec2(20, 20), 0, 2, 16);
  check({}, blit::Vec2(20, 20), 2, -1, 16);
  check({}, blit::Vec2(20, 20), 2, 2, 0);
  check({0, 0, 0, 8}, blit::Vec2(20, 20), 2, 2, 16);
  check({0, 0, 8, -1}, blit::Vec2(20, 20), 2, 2, 16);
  check({0, std::numeric_limits<float>::infinity(), 8, 8}, blit::Vec2(20, 20), 2, 2, 16);
  check({}, blit::Vec2(20, std::numeric_limits<float>::quiet_NaN()), 2, 2, 16);
}
