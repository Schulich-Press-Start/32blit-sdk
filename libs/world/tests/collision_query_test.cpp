#include "collision_query.hpp"

#include <gtest/gtest.h>

namespace {
  constexpr sdk::Box origin{0, 0, 2, 2};

  void expect_contact(const sdk::BoxSweepHit &hit, double fraction, int x, int y) {
    EXPECT_DOUBLE_EQ(hit.fraction, fraction);
    EXPECT_EQ(hit.normal.x, x);
    EXPECT_EQ(hit.normal.y, y);
  }

  struct Fixture {
    sdk::World world{8};
    sdk::ComponentPool<sdk::Transform2D> positions{world, world.capacity()};
    sdk::ComponentPool<sdk::BoxCollider2D> shapes{world, world.capacity()};

    sdk::Entity place(float x, uint32_t category = 0) {
      const auto entity = world.create();
      world.add<sdk::Transform2D>(entity)->x = x;
      world.add<sdk::BoxCollider2D>(entity)->category = category;
      return entity;
    }
  };
}

TEST(BoxSweep, FastMovementFindsEntryAndOpposingNormal) {
  sdk::BoxSweepHit hit;
  ASSERT_TRUE(sdk::sweep_box(origin, blit::Vec2(100, 3), {40, 0, 41, 4}, hit));
  expect_contact(hit, 0.38, -1, 0);
  ASSERT_TRUE(sdk::sweep_box(origin, blit::Vec2(0, -4), {0, -4, 2, -2}, hit));
  expect_contact(hit, 0.5, 0, 1);
  ASSERT_TRUE(sdk::sweep_box(origin, blit::Vec2(4, 4), {3, 3, 6, 6}, hit));
  expect_contact(hit, 0.25, -1, 0);
}

TEST(BoxSweep, OverlapAndInwardTouchCountAtStartAndEntryCountsAtEnd) {
  sdk::BoxSweepHit hit;
  ASSERT_TRUE(sdk::sweep_box(origin, blit::Vec2(), {1, 1, 3, 3}, hit));
  expect_contact(hit, 0, 0, 0);
  ASSERT_TRUE(sdk::sweep_box(origin, blit::Vec2(4, 0), {2, 0, 4, 2}, hit));
  expect_contact(hit, 0, -1, 0);
  ASSERT_TRUE(sdk::sweep_box(origin, blit::Vec2(4, 0), {6, 0, 8, 2}, hit));
  expect_contact(hit, 1, -1, 0);
  ASSERT_TRUE(sdk::sweep_box(origin, blit::Vec2(4, 4), {6, 6, 8, 8}, hit));
  expect_contact(hit, 1, -1, 0);
}

TEST(BoxSweep, RejectsMovingAwayEdgeSkimsCornerGrazesAndEmptyBoxes) {
  sdk::BoxSweepHit hit;
  EXPECT_FALSE(sdk::sweep_box(origin, blit::Vec2(-4, 0), {2, 0, 4, 2}, hit));
  EXPECT_FALSE(sdk::sweep_box(origin, blit::Vec2(4, 0), {3, 2, 5, 4}, hit));
  EXPECT_FALSE(sdk::sweep_box(origin, blit::Vec2(4, 4), {0, 4, 2, 6}, hit));
  EXPECT_FALSE(sdk::sweep_box(origin, blit::Vec2(), {2, 0, 4, 2}, hit));
  EXPECT_FALSE(sdk::sweep_box(origin, blit::Vec2(4, 0), {3, 0, 3, 2}, hit));
  EXPECT_FALSE(sdk::sweep_box({0, 0, -2, 2}, blit::Vec2(4, 0), {3, 0, 5, 2}, hit));
  expect_contact(hit, 1, 0, 0);
}

TEST(EntitySweep, MaskAndLiveIgnoreSelectTargetsBeforeClosestHit) {
  static_assert(sdk::collision_bit(0) == 1u && sdk::collision_bit(31) == 0x80000000u);
  static_assert(sdk::collision_bit(32) == 0 && sdk::collision_bit(UINT32_MAX) == 0);
  Fixture f;
  const auto shooter = f.place(0, 31);
  const auto nearest = f.place(3);
  f.place(4, 32);
  const auto far = f.place(8, 31);
  const auto only_high = sdk::collision_bit(31);
  const blit::Vec2 delta(16, 0);
  EXPECT_EQ(sdk::sweep_entities(f.world, origin, delta, only_high).entity.index(), shooter.index());
  EXPECT_EQ(sdk::sweep_entities(f.world, origin, delta, only_high, shooter).entity.index(), far.index());
  EXPECT_EQ(sdk::sweep_entities(f.world, origin, delta, sdk::all_collision_categories, shooter).entity.index(),
            nearest.index());
  EXPECT_FALSE(sdk::sweep_entities(f.world, origin, delta, 0).entity);
  ASSERT_TRUE(f.world.destroy(nearest));
  EXPECT_EQ(sdk::sweep_entities(f.world, origin, delta, sdk::all_collision_categories, shooter).entity.index(),
            far.index());
}

TEST(EntitySweep, EqualHitsPreferLowestSlotDespiteDenseOrderAndIgnoreRequiresIdentity) {
  Fixture f, foreign;
  const auto stale = f.place(8);
  const auto other = f.place(8);
  ASSERT_TRUE(f.world.destroy(stale));
  const auto replacement = f.place(8);
  ASSERT_EQ(replacement.index(), stale.index());
  ASSERT_TRUE(foreign.world.destroy(foreign.place(8)));
  const auto foreign_handle = foreign.place(8);
  ASSERT_EQ(replacement.index(), foreign_handle.index());
  ASSERT_EQ(replacement.generation(), foreign_handle.generation());

  const auto mask = sdk::collision_bit(0);
  const blit::Vec2 delta(6, 0);
  for(const auto ignore : {stale, foreign_handle}) {
    const auto hit = sdk::sweep_entities(f.world, origin, delta, mask, ignore);
    ASSERT_TRUE(hit.entity);
    EXPECT_EQ(hit.entity.index(), replacement.index());
    EXPECT_EQ(hit.entity.generation(), replacement.generation());
    expect_contact(hit.contact, 1, -1, 0);
  }
  EXPECT_EQ(sdk::sweep_entities(f.world, origin, delta, mask, replacement).entity.index(), other.index());
}
