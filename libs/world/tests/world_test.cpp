#include <gtest/gtest.h>
#include "world.hpp"

namespace {
  struct Position { int x = 7; };
  struct Velocity { float dx = 0; };
  SDK_COMPONENT(Velocity, dx);
  struct alignas(64) Tracked {
    static int living;
    int value;
    explicit Tracked(int v) noexcept : value(v) { ++living; }
    Tracked(Tracked &&other) noexcept : value(other.value) { ++living; }
    ~Tracked() noexcept { --living; }
  };
  int Tracked::living = 0;
}

TEST(World, ExhaustionAndRecyclingNeverReviveOldHandles) {
  sdk::World world(2);
  auto first = world.create(), second = world.create();
  EXPECT_FALSE(world.create());
  EXPECT_EQ(world.size(), 2);
  EXPECT_TRUE(world.destroy(first));
  auto replacement = world.create();
  EXPECT_EQ(replacement.index(), first.index());
  EXPECT_NE(replacement.generation(), first.generation());
  EXPECT_FALSE(world.alive(first));
  EXPECT_FALSE(world.destroy(first));
  EXPECT_TRUE(world.alive(second));
  EXPECT_TRUE(world.alive(replacement));
  EXPECT_EQ(world.size(), 2);
  EXPECT_FALSE(world.destroy({}));
}

TEST(World, ForeignHandleCannotAccessOrDestroyMatchingLocalEntity) {
  sdk::World level(1), menu(1);
  sdk::ComponentPool<Position> positions(menu, 1);
  const auto player = level.create(), button = menu.create();
  ASSERT_EQ(player.index(), button.index());
  ASSERT_EQ(player.generation(), button.generation());

  EXPECT_EQ(positions.emplace(player), nullptr);
  auto *position = positions.emplace(button);
  ASSERT_NE(position, nullptr);
  position->x = 42;

  EXPECT_FALSE(menu.alive(player));
  EXPECT_EQ(positions.get(player), nullptr);
  EXPECT_EQ(std::as_const(positions).get(player), nullptr);
  EXPECT_FALSE(positions.erase(player));
  EXPECT_FALSE(menu.destroy(player));
  ASSERT_TRUE(menu.alive(button));
  ASSERT_NE(positions.get(button), nullptr);
  EXPECT_EQ(positions.get(button)->x, 42);
}

TEST(World, ReusingWorldAddressDoesNotReviveOldHandle) {
  alignas(sdk::World) unsigned char storage[sizeof(sdk::World)];
  auto *world = ::new (storage) sdk::World(1);
  const auto old = world->create();
  world->~World();

  world = ::new (storage) sdk::World(1);
  const auto current = world->create();
  EXPECT_EQ(old.index(), current.index());
  EXPECT_EQ(old.generation(), current.generation());
  EXPECT_FALSE(world->alive(old));
  EXPECT_FALSE(world->destroy(old));
  EXPECT_TRUE(world->alive(current));
  world->~World();
}

TEST(World, ZeroBudgetRejectsCreation) {
  sdk::World world(0);
  sdk::ComponentPool<Position> pool(world, 0);
  EXPECT_FALSE(world.create());
  EXPECT_EQ(pool.emplace({}), nullptr);
}

TEST(Pool, CapacityDuplicateAndStaleAccess) {
  sdk::World world(3);
  sdk::ComponentPool<Position> pool(world, 1);
  auto a = world.create(), b = world.create();
  auto *original = pool.emplace(a);
  ASSERT_NE(original, nullptr);
  original->x = 42;
  EXPECT_EQ(pool.emplace(a), original);
  EXPECT_EQ(pool.emplace(b), nullptr);
  EXPECT_EQ(pool.get(a)->x, 42);
  EXPECT_EQ(pool.size(), 1);
  EXPECT_TRUE(world.destroy(a));
  auto c = world.create();
  EXPECT_EQ(pool.get(a), nullptr);
  EXPECT_EQ(pool.emplace(a), nullptr);
  EXPECT_FALSE(pool.erase(a));
  EXPECT_EQ(pool.size(), 0);
  ASSERT_NE(pool.emplace(c), nullptr);
  EXPECT_EQ(pool.get(c)->x, 7);
}

TEST(Pool, RemovalRelocatesAndDestroysAlignedObjectsCorrectly) {
  sdk::World world(3);
  {
    sdk::ComponentPool<Tracked> pool(world, 3);
    auto a = world.create(), b = world.create(), c = world.create();
    pool.emplace(a, 10); pool.emplace(b, 20); pool.emplace(c, 30);
    EXPECT_EQ(Tracked::living, 3);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(pool.get(a)) % alignof(Tracked), 0u);
    EXPECT_TRUE(pool.erase(b));
    EXPECT_EQ(Tracked::living, 2);
    ASSERT_NE(pool.get(c), nullptr);
    EXPECT_EQ(pool.get(c)->value, 30);
    EXPECT_TRUE(world.destroy(a));
    EXPECT_EQ(Tracked::living, 1);
    EXPECT_EQ(pool.get(c)->value, 30);
  }
  EXPECT_EQ(Tracked::living, 0);
}

TEST(Pool, EntityDestructionRemovesComponentsFromEveryAttachedPool) {
  sdk::World world(2);
  sdk::ComponentPool<Position> a(world, 2), b(world, 2);
  auto entity = world.create();
  a.emplace(entity); b.emplace(entity);
  world.destroy(entity);
  EXPECT_EQ(a.size(), 0);
  EXPECT_EQ(b.size(), 0);
  // Unlink a temporary pool without disrupting the remaining chain.
  { sdk::ComponentPool<Position> temporary(world, 1); }
  auto next = world.create(); a.emplace(next); b.emplace(next);
  world.destroy(next);
  EXPECT_EQ(a.size(), 0);
  EXPECT_EQ(b.size(), 0);
}

TEST(Pool, OwnerCanBeDestroyedBeforePool) {
  auto world = std::make_unique<sdk::World>(1);
  sdk::ComponentPool<Tracked> pool(*world, 1);
  auto entity = world->create(); pool.emplace(entity, 5);
  world.reset();
  EXPECT_EQ(pool.get(entity), nullptr);
  EXPECT_EQ(pool.emplace(entity, 9), nullptr);
  EXPECT_FALSE(pool.erase(entity));
}

TEST(Pool, InvalidSetupUnlinksTheFailedPool) {
  sdk::World world(1);
  EXPECT_THROW((sdk::ComponentPool<Position>(world, 2)), std::invalid_argument);
  auto entity = world.create();
  EXPECT_TRUE(world.destroy(entity));
}

TEST(World, NamedCreationRejectsDuplicatesAndExhaustionWithoutLeakingSlots) {
  sdk::World world(3);
  EXPECT_FALSE(world.create("no-pool"));
  sdk::ComponentPool<sdk::Name> names(world, 2);
  EXPECT_FALSE(world.create(""));
  EXPECT_FALSE(world.create("123456789012345678901234"));
  auto player = world.create("player");
  ASSERT_TRUE(player);
  EXPECT_FALSE(world.create("player"));
  EXPECT_FALSE(world.find("play"));
  EXPECT_FALSE(world.find("Player"));
  auto door = world.create("12345678901234567890123");
  ASSERT_TRUE(door);
  EXPECT_FALSE(world.create("full"));
  EXPECT_EQ(world.size(), 2);
  EXPECT_TRUE(world.create());

  EXPECT_TRUE(world.destroy(player));
  EXPECT_FALSE(world.find("player"));
  EXPECT_TRUE(world.alive(world.find("12345678901234567890123")));
  auto replacement = world.create("player");
  EXPECT_TRUE(world.alive(replacement));
  EXPECT_FALSE(world.alive(player));
}

TEST(World, RegistryFieldEditIsVisibleToTypedGameCode) {
  sdk::World world(2);
  sdk::ComponentPool<sdk::Name> names(world, 2);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 2);
  auto player = world.create("player"), door = world.create("door");
  auto *transform = world.add<sdk::Transform2D>(player);
  ASSERT_NE(transform, nullptr);
  transform->y = 81;
  ASSERT_NE(world.add<sdk::Transform2D>(door), nullptr);

  const auto *info = sdk::find_component("Transform2D");
  ASSERT_NE(info, nullptr);
  const auto *field = sdk::find_field(*info, "x");
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(sdk::find_field(*info, "missing"), nullptr);
  auto *data = static_cast<char *>(world.get(world.find("player"), info->id));
  ASSERT_NE(data, nullptr);
  EXPECT_TRUE(sdk::parse_value(field->type, "200", data + field->offset));
  auto *read_back = std::as_const(world).get<sdk::Transform2D>(player);
  ASSERT_NE(read_back, nullptr);
  EXPECT_FLOAT_EQ(read_back->x, 200);
  EXPECT_FLOAT_EQ(read_back->y, 81);
  EXPECT_FLOAT_EQ(world.get<sdk::Transform2D>(door)->x, 0);
}

TEST(World, RegistryDispatchRespectsCapacityOwnershipAndPoolLifetime) {
  sdk::World world(2), other(1);
  auto first = world.create(), second = world.create(), foreign = other.create();
  const auto *info = sdk::find_component("Velocity");
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(world.add(first, info->id), nullptr);
  EXPECT_EQ(world.add(first, UINT16_MAX), nullptr);
  {
    EXPECT_THROW((sdk::ComponentPool<Velocity>(world, 3)), std::invalid_argument);
    sdk::ComponentPool<Velocity> velocities(world, 1);
    EXPECT_THROW((sdk::ComponentPool<Velocity>(world, 1)), std::invalid_argument);
    auto *velocity = static_cast<Velocity *>(world.add(first, info->id));
    ASSERT_NE(velocity, nullptr);
    velocity->dx = 6;
    EXPECT_EQ(world.add(second, info->id), nullptr);
    EXPECT_EQ(world.get(foreign, info->id), nullptr);
    EXPECT_FALSE(world.remove(foreign, info->id));
    ASSERT_NE(world.get<Velocity>(first), nullptr);
    EXPECT_FLOAT_EQ(world.get<Velocity>(first)->dx, 6);
    EXPECT_TRUE(world.remove(first, info->id));
    EXPECT_EQ(world.get<Velocity>(first), nullptr);
    ASSERT_NE(world.add<Velocity>(second), nullptr);
  }
  EXPECT_EQ(world.get<Velocity>(second), nullptr);
  sdk::ComponentPool<Velocity> replacement(world, 1);
  auto *value = world.add<Velocity>(second);
  ASSERT_NE(value, nullptr);
  EXPECT_FLOAT_EQ(value->dx, 0);
}

TEST(WorldQuery, VisitsOnlyMatchesAfterSwapRemovalAndRecycling) {
  sdk::World world(4);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 4);
  sdk::ComponentPool<Velocity> velocities(world, 3);
  auto removed = world.create(), moving = world.create(), still = world.create();
  ASSERT_NE(world.add<sdk::Transform2D>(removed), nullptr);
  ASSERT_NE(world.add<sdk::Transform2D>(moving), nullptr);
  ASSERT_NE(world.add<sdk::Transform2D>(still), nullptr);
  ASSERT_NE(world.add<sdk::Transform2D>(world.create()), nullptr);
  ASSERT_NE(world.add<Velocity>(removed), nullptr);
  auto *velocity = world.add<Velocity>(moving);
  ASSERT_NE(velocity, nullptr);
  velocity->dx = 6;
  ASSERT_TRUE(world.destroy(removed));
  const auto recycled = world.create();
  ASSERT_NE(world.add<Velocity>(recycled), nullptr); // no Transform2D

  int visits = 0;
  world.each<sdk::Transform2D, Velocity>([&](sdk::Entity entity, sdk::Transform2D &t, Velocity &v) {
    EXPECT_EQ(entity.index(), moving.index());
    EXPECT_EQ(entity.generation(), moving.generation());
    EXPECT_TRUE(world.alive(entity));
    t.x += v.dx;
    ++visits;
  });
  world.each<Velocity, sdk::Transform2D>([](sdk::Entity, const Velocity &v, sdk::Transform2D &t) {
    t.x += v.dx;
  });
  EXPECT_EQ(visits, 1);
  EXPECT_FLOAT_EQ(world.get<sdk::Transform2D>(moving)->x, 12);
  EXPECT_FLOAT_EQ(world.get<sdk::Transform2D>(still)->x, 0);
}

TEST(WorldQuery, MissingOrEmptyPoolProducesNoMatches) {
  sdk::World world(1);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 1);
  ASSERT_NE(world.add<sdk::Transform2D>(world.create()), nullptr);
  auto unexpected = [](sdk::Entity, sdk::Transform2D &, Velocity &) { ADD_FAILURE(); };
  world.each<sdk::Transform2D, Velocity>(unexpected);
  sdk::ComponentPool<Velocity> velocities(world, 1);
  world.each<sdk::Transform2D, Velocity>(unexpected);
}

TEST(WorldQuery, NestedAndThrowingCallbacksKeepMutationGuardScoped) {
  sdk::World world(2);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 2);
  auto entity = world.create();
  ASSERT_NE(world.add<sdk::Transform2D>(entity), nullptr);

  auto query = [&](sdk::Entity current, sdk::Transform2D &t) {
    world.each<sdk::Transform2D>([&](sdk::Entity nested, sdk::Transform2D &inner) {
      EXPECT_FALSE(world.destroy(nested));
      inner.x = 5;
    });
    EXPECT_FALSE(world.create());
    EXPECT_FALSE(world.destroy(current));
    EXPECT_FALSE(world.remove<sdk::Transform2D>(current));
    EXPECT_EQ(world.add<sdk::Transform2D>(current), nullptr);
    EXPECT_FALSE(transforms.erase(current));
    EXPECT_EQ(transforms.emplace(current), nullptr);
    EXPECT_THROW((sdk::ComponentPool<Velocity>(world, 1)), std::logic_error);
    EXPECT_FLOAT_EQ(t.x, 5);
    throw 1;
  };
  EXPECT_THROW(world.each<sdk::Transform2D>(query), int);
  EXPECT_TRUE(world.remove<sdk::Transform2D>(entity));
  EXPECT_TRUE(world.create());
}
