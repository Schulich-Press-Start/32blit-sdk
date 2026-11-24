#include "overlap_tracker.hpp"

#include <gtest/gtest.h>

namespace {
  struct Fixture {
    sdk::World world{4};
    sdk::ComponentPool<sdk::Transform2D> positions{world, world.capacity()};
    sdk::ComponentPool<sdk::BoxCollider2D> boxes{world, world.capacity()};
    sdk::OverlapTracker tracker{world};

    sdk::Entity place(float x) {
      const auto entity = world.create();
      world.add<sdk::Transform2D>(entity)->x = x;
      world.add<sdk::BoxCollider2D>(entity);
      return entity;
    }
  };

  void expect_event(const sdk::OverlapEvent &event, sdk::OverlapPhase phase, sdk::Entity observer, sdk::Entity other) {
    EXPECT_EQ(event.phase, phase);
    EXPECT_EQ(event.observer.index(), observer.index());
    EXPECT_EQ(event.observer.generation(), observer.generation());
    EXPECT_EQ(event.other.index(), other.index());
    EXPECT_EQ(event.other.generation(), other.generation());
  }
}

TEST(OverlapTracker, ReportsEntryStayAndExitWithoutCountingEdgeTouches) {
  Fixture f;
  const auto actor = f.place(0), other = f.place(8);
  auto &tracked = f.tracker.update(actor);
  EXPECT_TRUE(tracked.empty());
  f.world.get<sdk::Transform2D>(other)->x = 7;
  ASSERT_EQ(f.tracker.update(actor).size(), 1u);
  expect_event(tracked[0], sdk::OverlapPhase::Enter, actor, other);
  ASSERT_EQ(f.tracker.update(actor).size(), 1u);
  expect_event(tracked[0], sdk::OverlapPhase::Stay, actor, other);
  f.world.get<sdk::BoxCollider2D>(other)->width = 0;
  ASSERT_EQ(f.tracker.update(actor).size(), 1u);
  expect_event(tracked[0], sdk::OverlapPhase::Exit, actor, other);
  EXPECT_TRUE(f.tracker.update(actor).empty());
}

TEST(OverlapTracker, RecycledSlotExitsOldEntityAndEntersReplacement) {
  Fixture f;
  const auto actor = f.place(0), other = f.place(3);
  f.tracker.update(actor);
  ASSERT_TRUE(f.world.destroy(other));
  const auto replacement = f.place(3);
  ASSERT_EQ(other.index(), replacement.index());
  const auto &events = f.tracker.update(actor);
  ASSERT_EQ(events.size(), 2u);
  expect_event(events[0], sdk::OverlapPhase::Exit, actor, other);
  expect_event(events[1], sdk::OverlapPhase::Enter, actor, replacement);
  EXPECT_FALSE(f.world.alive(events[0].other));
}

TEST(OverlapTracker, FilteringAndResetControlTheBaseline) {
  Fixture f;
  const auto actor = f.place(0), other = f.place(2), ignored = f.place(3);
  const auto filter = [&](sdk::Entity entity) { return entity.index() != ignored.index(); };
  const auto &events = f.tracker.update(actor, filter);
  ASSERT_EQ(events.size(), 1u);
  expect_event(events[0], sdk::OverlapPhase::Enter, actor, other);
  ASSERT_EQ(f.tracker.update(actor, [](sdk::Entity) { return false; }).size(), 1u);
  expect_event(events[0], sdk::OverlapPhase::Exit, actor, other);
  f.tracker.update(actor, filter);
  f.tracker.reset();
  EXPECT_TRUE(events.empty());
  ASSERT_EQ(f.tracker.update(actor, filter).size(), 1u);
  expect_event(events[0], sdk::OverlapPhase::Enter, actor, other);
}

TEST(OverlapTracker, ChangingObserverSeparatesOldAndNewContacts) {
  Fixture f;
  const auto actor = f.place(0), replacement = f.place(3), other = f.place(6);
  const auto filter = [&](sdk::Entity entity) { return entity.index() == other.index(); };
  f.tracker.update(actor, filter);
  const auto &events = f.tracker.update(replacement, filter);
  ASSERT_EQ(events.size(), 2u);
  expect_event(events[0], sdk::OverlapPhase::Exit, actor, other);
  expect_event(events[1], sdk::OverlapPhase::Enter, replacement, other);
  ASSERT_TRUE(f.world.destroy(replacement));
  ASSERT_EQ(f.tracker.update(replacement, filter).size(), 1u);
  expect_event(events[0], sdk::OverlapPhase::Exit, replacement, other);
}

TEST(OverlapTracker, MaskFiltersCandidatesBeforePredicateAndChangingMaskEmitsExit) {
  Fixture f;
  const auto actor = f.place(0), selected = f.place(2), excluded = f.place(3), distant = f.place(9);
  f.world.get<sdk::BoxCollider2D>(selected)->category = 31;
  f.world.get<sdk::BoxCollider2D>(distant)->category = 31;
  f.world.get<sdk::BoxCollider2D>(actor)->category = 32;
  const auto mask = sdk::collision_bit(31);
  int calls = 0;
  const auto &events = f.tracker.update(actor, mask, [&](sdk::Entity candidate) {
    ++calls;
    EXPECT_EQ(candidate.index(), selected.index());
    return true;
  });
  EXPECT_EQ(calls, 1);
  ASSERT_EQ(events.size(), 1u);
  expect_event(events[0], sdk::OverlapPhase::Enter, actor, selected);
  ASSERT_EQ(f.tracker.update(actor, 0).size(), 1u);
  expect_event(events[0], sdk::OverlapPhase::Exit, actor, selected);
  ASSERT_EQ(f.tracker.update(actor, sdk::collision_bit(0)).size(), 1u);
  expect_event(events[0], sdk::OverlapPhase::Enter, actor, excluded);
}
