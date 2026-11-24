#include "grid_navigation.hpp"

#include <gtest/gtest.h>

TEST(GridNavigation, TakesShortestDetourAndReusesWorkspaceAfterMapEdits) {
  sdk::GridNavigation nav;
  nav.resize(5, 3);
  bool wall = true;
  const auto walkable = [&](sdk::RouteCell cell) {
    return cell.x != 2 || (cell.y == 2 && !wall);
  };
  const auto &path = nav.find({0, 1}, {4, 1}, walkable);
  EXPECT_TRUE(path.empty());
  wall = false;
  const std::vector<sdk::RouteCell> expected{{0, 1}, {1, 1}, {1, 2}, {2, 2}, {3, 2}, {4, 2}, {4, 1}};
  EXPECT_EQ(nav.find({0, 1}, {4, 1}, walkable), expected);
  EXPECT_TRUE(nav.find({0, 1}, {2, 1}, walkable).empty());
  EXPECT_TRUE(nav.find({-1, 1}, {4, 1}, walkable).empty());
}

TEST(GridNavigation, StartAtGoalAndResizeInvalidateThePreviousPath) {
  sdk::GridNavigation nav;
  nav.resize(2, 2);
  const auto open = [](sdk::RouteCell) { return true; };
  const std::vector<sdk::RouteCell> same{{1, 1}};
  EXPECT_EQ(nav.find({1, 1}, {1, 1}, open), same);
  nav.resize(0, 0);
  EXPECT_TRUE(nav.find({0, 0}, {0, 0}, open).empty());
  nav.resize(1, 3);
  const std::vector<sdk::RouteCell> up{{0, 2}, {0, 1}, {0, 0}};
  EXPECT_EQ(nav.find({0, 2}, {0, 0}, open), up);
}
