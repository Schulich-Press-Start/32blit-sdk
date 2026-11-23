#include "grid_step.hpp"

#include <gtest/gtest.h>

namespace {
  constexpr int tile = 16, step_ms = 200;
  // Column 1 is solid except in the bottom row, so a route down then right must turn
  // through the opening rather than through the wall.
  constexpr char map[][4] = {".#.", ".#.", "..."};
  constexpr int columns = 3, rows = 3;
  bool walkable(blit::Point cell) { return map[cell.y][cell.x] != '#'; }

  struct Fixture {
    sdk::Transform2D position{};
    sdk::BoxCollider2D shape{0, 0, 16, 16};
    sdk::GridStepState state{};

    Fixture() { sdk::grid_place(state, position, shape, tile); }

    sdk::GridStepResult step(sdk::StepDirection direction, int elapsed = step_ms) {
      return sdk::grid_step(state, position, shape, direction, tile, step_ms, columns, rows, walkable, elapsed);
    }
    blit::Point cell() const { return sdk::grid_cell(position, shape, tile); }
  };
}

TEST(GridStep, PlacesActorCentredInItsCell) {
  sdk::Transform2D position{37, 41};
  const sdk::BoxCollider2D shape{0, 0, 8, 8};
  sdk::GridStepState state{};
  sdk::grid_place(state, position, shape, tile);
  EXPECT_EQ(state.cell.x, 2);
  EXPECT_EQ(state.cell.y, 2);
  EXPECT_FLOAT_EQ(position.x, 2 * tile + 4);
  EXPECT_FLOAT_EQ(position.y, 2 * tile + 4);
  EXPECT_EQ(state.moving, sdk::StepDirection::none);
}

TEST(GridStep, PlacesFeetAnchoredColliderInItsCell) {
  // A collider hanging above the origin must still resolve to the cell under the actor.
  sdk::Transform2D position{20, 32};
  const sdk::BoxCollider2D shape{4, -12, 8, 12};
  sdk::GridStepState state{};
  sdk::grid_place(state, position, shape, tile);
  EXPECT_EQ(state.cell.x, 1);
  EXPECT_EQ(state.cell.y, 1);
  EXPECT_EQ(sdk::grid_cell(position, shape, tile).x, 1);
  EXPECT_EQ(sdk::grid_cell(position, shape, tile).y, 1);
}

TEST(GridStep, RefusedStepTurnsTheActorWithoutMoving) {
  Fixture f;
  const auto result = f.step(sdk::StepDirection::right, 0);
  EXPECT_TRUE(result.blocked);
  EXPECT_FALSE(result.started);
  EXPECT_EQ(f.state.facing, sdk::StepDirection::right);
  EXPECT_EQ(f.state.moving, sdk::StepDirection::none);
  EXPECT_FLOAT_EQ(f.position.x, 0);
  EXPECT_FLOAT_EQ(f.position.y, 0);
}

TEST(GridStep, StepInterpolatesThenLandsCentred) {
  Fixture f;
  const auto started = f.step(sdk::StepDirection::down, 0);
  EXPECT_TRUE(started.started);
  EXPECT_FALSE(started.finished);
  EXPECT_EQ(f.state.moving, sdk::StepDirection::down);
  EXPECT_FLOAT_EQ(f.position.y, 0);

  f.step(sdk::StepDirection::none, step_ms / 2);
  EXPECT_FLOAT_EQ(f.position.y, float(tile) / 2);

  const auto finished = f.step(sdk::StepDirection::none, step_ms / 2);
  EXPECT_TRUE(finished.finished);
  EXPECT_EQ(f.state.moving, sdk::StepDirection::none);
  EXPECT_EQ(f.cell().y, 1);
  EXPECT_FLOAT_EQ(f.position.y, float(tile));
}

TEST(GridStep, HeldDirectionResumesWithoutAGap) {
  Fixture f;
  const auto walked = f.step(sdk::StepDirection::down, step_ms);
  EXPECT_TRUE(walked.started);
  EXPECT_TRUE(walked.finished);
  EXPECT_EQ(f.state.moving, sdk::StepDirection::none);
  const auto resumed = f.step(sdk::StepDirection::down, 0);
  EXPECT_TRUE(resumed.started);
  EXPECT_EQ(f.state.destination.y, 2);
}

TEST(GridStep, ReversalCannotCutAStepShort) {
  Fixture f;
  f.step(sdk::StepDirection::down, step_ms / 2);
  const auto ignored = f.step(sdk::StepDirection::up, 0);
  EXPECT_FALSE(ignored.started);
  EXPECT_FALSE(ignored.blocked);
  EXPECT_EQ(f.state.moving, sdk::StepDirection::down);
  EXPECT_FLOAT_EQ(f.position.y, float(tile) / 2);
}

TEST(GridStep, SolidAndOffMapCellsAreRefused) {
  Fixture f;
  const auto into_wall = f.step(sdk::StepDirection::right, 0);
  EXPECT_TRUE(into_wall.blocked);
  EXPECT_EQ(f.state.moving, sdk::StepDirection::none);
  EXPECT_FLOAT_EQ(f.position.x, 0);

  const auto off_map = f.step(sdk::StepDirection::up, 0);
  EXPECT_TRUE(off_map.blocked);
  EXPECT_EQ(f.state.moving, sdk::StepDirection::none);
}

TEST(GridStep, TurningRoutesThroughAnOpening) {
  Fixture f;
  f.step(sdk::StepDirection::down, step_ms);
  f.step(sdk::StepDirection::down, step_ms);
  const auto turned = f.step(sdk::StepDirection::right, 0);
  EXPECT_TRUE(turned.started);
  EXPECT_EQ(f.state.destination.x, 1);
  EXPECT_EQ(f.state.destination.y, 2);
}

TEST(GridStep, InvalidInputLeavesStateAndTransformUntouched) {
  Fixture f;
  f.step(sdk::StepDirection::down, step_ms / 2);
  const auto position = f.position;
  const auto state = f.state;
  const int invalid[][4] = {{0, step_ms, columns, rows}, {tile, 0, columns, rows},
                            {tile, step_ms, 0, rows}, {tile, step_ms, columns, 0}};
  for(const auto &values : invalid) {
    const auto result = sdk::grid_step(f.state, f.position, f.shape, sdk::StepDirection::down,
                                       values[0], values[1], values[2], values[3], walkable, step_ms);
    EXPECT_FALSE(result.started || result.finished || result.blocked);
  }
  const auto negative = sdk::grid_step(f.state, f.position, f.shape, sdk::StepDirection::down,
                                       tile, step_ms, columns, rows, walkable, -1);
  EXPECT_FALSE(negative.started || negative.finished || negative.blocked);
  EXPECT_EQ(f.state.moving, state.moving);
  EXPECT_EQ(f.state.elapsed_ms, state.elapsed_ms);
  EXPECT_FLOAT_EQ(f.position.x, position.x);
  EXPECT_FLOAT_EQ(f.position.y, position.y);
}

TEST(GridStep, OversizedBoxStillCentresInItsCell) {
  sdk::Transform2D position{0, 0};
  const sdk::BoxCollider2D shape{0, 0, 40, 40};
  sdk::GridStepState state{};
  sdk::grid_place(state, position, shape, tile);
  EXPECT_EQ(state.cell.x, 1);
  EXPECT_EQ(state.cell.y, 1);
  EXPECT_FLOAT_EQ(position.x, 4);
  EXPECT_FLOAT_EQ(position.y, 4);
}
