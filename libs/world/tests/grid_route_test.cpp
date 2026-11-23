#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <vector>

#include "grid_route.hpp"

namespace {
  using sdk::RouteCell;
  using sdk::RouteIssue;

  template<std::size_t Height, std::size_t Stride>
  RouteIssue trace(const char (&map)[Height][Stride], RouteCell spawn, RouteCell goal,
                   std::vector<RouteCell> *ordered = nullptr) {
    constexpr int width = Stride - 1, height = Height;
    return sdk::trace_route(width, height, spawn, goal, [&](RouteCell cell) {
      if(cell.x < 0 || cell.x >= width || cell.y < 0 || cell.y >= height) {
        ADD_FAILURE() << "Predicate called for an out-of-bounds cell";
        return false;
      }
      return map[cell.y][cell.x] == '#';
    }, [&](RouteCell cell) {
      if(ordered) ordered->push_back(cell);
    });
  }
}

TEST(GridRoute, EmitsAnOrthogonalChainInSpawnToGoalOrder) {
  constexpr char map[][5] = {".###", "##..", "#..."};
  std::vector<RouteCell> ordered;
  const auto issue = trace(map, {0, 2}, {3, 0}, &ordered);
  EXPECT_EQ(issue.message, nullptr);
  EXPECT_EQ(issue.cell, (RouteCell{-1, -1}));
  EXPECT_EQ(ordered, (std::vector<RouteCell>{{0, 2}, {0, 1}, {1, 1}, {1, 0}, {2, 0}, {3, 0}}));
}

TEST(GridRoute, PointsAtInvalidEndpoints) {
  constexpr char map[][6] = {"####."};
  auto issue = trace(map, {-1, 0}, {3, 0});
  EXPECT_STREQ(issue.message, "Spawn is outside the map.");
  EXPECT_EQ(issue.cell, (RouteCell{-1, 0}));

  issue = trace(map, {0, 0}, {0, 0});
  EXPECT_STREQ(issue.message, "Spawn and goal must use different tiles.");
  EXPECT_EQ(issue.cell, (RouteCell{0, 0}));

  issue = trace(map, {0, 0}, {4, 0});
  EXPECT_STREQ(issue.message, "Goal must be on a path tile.");
  EXPECT_EQ(issue.cell, (RouteCell{4, 0}));

  issue = trace(map, {0, 0}, {2, 0});
  EXPECT_STREQ(issue.message, "Goal must be a path endpoint.");
  EXPECT_EQ(issue.cell, (RouteCell{2, 0}));
}

TEST(GridRoute, PointsAtTheBranchRatherThanItsExtraEnd) {
  constexpr char map[][4] = {".#.", "###"};
  const auto issue = trace(map, {0, 1}, {2, 1});
  EXPECT_STREQ(issue.message, "Path branches; keep a single lane.");
  EXPECT_EQ(issue.cell, (RouteCell{1, 1}));
}

TEST(GridRoute, RejectsGapsAndDoesNotConnectDiagonals) {
  constexpr char map[][4] = {"##.", "..#", "..#"};
  const auto issue = trace(map, {0, 0}, {2, 2});
  EXPECT_STREQ(issue.message, "Path has a gap or an extra end.");
  EXPECT_EQ(issue.cell, (RouteCell{1, 0}));
}

TEST(GridRoute, FindsDetachedLoopsEvenWhenTheyHaveNoExtraEnds) {
  constexpr char map[][7] = {"######", ".....#", ".##..#", ".##..."};
  const auto issue = trace(map, {0, 0}, {5, 2});
  EXPECT_STREQ(issue.message, "Path contains a disconnected loop.");
  EXPECT_EQ(issue.cell, (RouteCell{1, 2}));
}

TEST(GridRoute, ResolvesOnlyTheAuthoredTileCenter) {
  RouteCell cell;
  EXPECT_TRUE(sdk::centered_route_cell(3, 2, 16, 24, 8, cell));
  EXPECT_EQ(cell, (RouteCell{1, 0}));
  EXPECT_TRUE(sdk::centered_route_cell(3, 2, 5, 7, 2, cell));
  EXPECT_EQ(cell, (RouteCell{1, 0}));
  EXPECT_FALSE(sdk::centered_route_cell(3, 2, 16, 24.5, 8, cell));
  EXPECT_EQ(cell, (RouteCell{1, 0}));
  EXPECT_FALSE(sdk::centered_route_cell(3, 2, 16, 48, 8, cell));
  EXPECT_EQ(cell, (RouteCell{-1, -1}));
  EXPECT_FALSE(sdk::centered_route_cell(3, 2, 16, 8, std::numeric_limits<double>::quiet_NaN(), cell));
  EXPECT_EQ(cell, (RouteCell{-1, -1}));
}
