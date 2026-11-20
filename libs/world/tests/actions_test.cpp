#include <gtest/gtest.h>
#include "actions.hpp"
#include "world.hpp"

#include <cstring>

namespace {
  sdk::Result run_command(sdk::World &world, const char *line) {
    sdk::Action action;
    sdk::Result result;
    return sdk::parse_console(line, action, result) ? sdk::apply(world, action) : result;
  }
}

TEST(SetAction, ChangesOnlyTheRequestedLiveField) {
  sdk::World world(2);
  sdk::ComponentPool<sdk::Name> names(world, 2);
  sdk::ComponentPool<sdk::Transform> transforms(world, 2);
  const auto player = world.create("player"), door = world.create("door");
  auto *position = world.add<sdk::Transform>(player);
  auto *door_position = world.add<sdk::Transform>(door);
  ASSERT_NE(position, nullptr);
  ASSERT_NE(door_position, nullptr);
  *position = {15, 81, 3};
  *door_position = {27, 12, 0};

  const auto result = run_command(world, "  set\tplayer.Transform.x  200  ");
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_FLOAT_EQ(position->x, 200);
  EXPECT_FLOAT_EQ(position->y, 81);
  EXPECT_FLOAT_EQ(door_position->x, 27);
}

TEST(SetAction, RejectsMalformedOrOversizedCommandsWithoutReplacingAction) {
  sdk::World world(1);
  sdk::ComponentPool<sdk::Name> names(world, 1);
  sdk::ComponentPool<sdk::Transform> transforms(world, 1);
  auto *position = world.add<sdk::Transform>(world.create("player"));
  ASSERT_NE(position, nullptr);

  sdk::Action action;
  sdk::Result result;
  ASSERT_TRUE(sdk::parse_console("set player.Transform.x 200", action, result));
  for(const char *line : {
      "reset player.Transform.x 1", "set .Transform.x 1", "set player..x 1",
      "set player.Transform. 1", "set player.Transform.x.y 1", "set player.Transform.x",
      "set player.Transform.x 1\nset player.Transform.x 2",
      "set 123456789012345678901234.Transform.x 1",
      "set player.123456789012345678901234567890123456789012345678.x 1",
      "set player.Transform.x 123456789012345678901234567890123456789012345678"}) {
    SCOPED_TRACE(line);
    EXPECT_FALSE(sdk::parse_console(line, action, result));
    EXPECT_FALSE(result.ok);
  }
  ASSERT_TRUE(sdk::apply(world, action).ok);
  EXPECT_FLOAT_EQ(position->x, 200);
}

TEST(SetAction, ReportsLookupAndValueFailuresWithoutChangingState) {
  sdk::World world(2);
  sdk::ComponentPool<sdk::Name> names(world, 2);
  sdk::ComponentPool<sdk::Transform> transforms(world, 1);
  ASSERT_TRUE(world.create("door"));
  auto *position = world.add<sdk::Transform>(world.create("player"));
  ASSERT_NE(position, nullptr);
  position->x = 15;

  struct Failure { const char *line; const char *diagnostic; };
  for(const auto &test : {
      Failure{"set playr.Transform.x 200", "no entity"},
      Failure{"set player.Unknown.x 200", "unknown component"},
      Failure{"set door.Transform.x 200", "has no component"},
      Failure{"set player.Transform.xx 200", "unknown field"},
      Failure{"set player.Transform.x nan", "float"},
      Failure{"set player.Transform.x 200 extra", "float"}}) {
    SCOPED_TRACE(test.line);
    const auto result = run_command(world, test.line);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(std::strstr(result.message, test.diagnostic), nullptr);
    EXPECT_FLOAT_EQ(position->x, 15);
  }
  EXPECT_EQ(world.get<sdk::Transform>(world.find("door")), nullptr);
}

TEST(SetAction, RenameDoesNotStealAnotherEntityName) {
  sdk::World world(2);
  sdk::ComponentPool<sdk::Name> names(world, 2);
  auto player = world.create("player"), door = world.create("door");
  ASSERT_TRUE(player);
  ASSERT_TRUE(door);
  EXPECT_FALSE(run_command(world, "set player.Name.value door").ok);
  EXPECT_EQ(world.find("door").index(), door.index());
  EXPECT_EQ(world.find("player").index(), player.index());

  const auto result = run_command(world, "set player.Name.value hero");
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_FALSE(world.find("player"));
  EXPECT_EQ(world.find("hero").index(), player.index());
}

TEST(WorldAction, StructuralCommandsHonorPoolCapacityAndEntityLifetime) {
  sdk::World world(2);
  sdk::ComponentPool<sdk::Name> names(world, 2);
  sdk::ComponentPool<sdk::Transform> transforms(world, 1);
  const auto player = world.create("player"), door = world.create("door");
  ASSERT_TRUE(run_command(world, "add player.Transform").ok);
  ASSERT_TRUE(run_command(world, "set player.Transform.x 200").ok);
  EXPECT_FALSE(run_command(world, "add door.Transform").ok);
  EXPECT_FALSE(run_command(world, "add door.Transform extra").ok);
  EXPECT_FALSE(run_command(world, "remove door.Transform").ok);
  EXPECT_FALSE(run_command(world, "destroy player extra").ok);
  ASSERT_TRUE(run_command(world, "add player.Transform").ok);
  ASSERT_NE(world.get<sdk::Transform>(player), nullptr);
  EXPECT_FLOAT_EQ(world.get<sdk::Transform>(player)->x, 200);

  ASSERT_TRUE(run_command(world, "remove player.Transform").ok);
  EXPECT_TRUE(world.alive(player));
  EXPECT_EQ(world.get<sdk::Transform>(player), nullptr);
  ASSERT_TRUE(run_command(world, "add door.Transform").ok);
  ASSERT_TRUE(run_command(world, "destroy door").ok);
  EXPECT_FALSE(world.alive(door));
  EXPECT_FALSE(world.find("door"));
  EXPECT_TRUE(run_command(world, "add player.Transform").ok);
}

TEST(WorldAction, CanonicalConsoleOutputReplaysEverySupportedVerb) {
  sdk::World world(1);
  sdk::ComponentPool<sdk::Name> names(world, 1);
  sdk::ComponentPool<sdk::Transform> transforms(world, 1);
  const auto player = world.create("player");
  ASSERT_TRUE(player);

  for(const char *line : {"add player.Transform", "set player.Transform.x 200",
                          "remove player.Transform", "destroy player"}) {
    SCOPED_TRACE(line);
    sdk::Action action;
    sdk::Result result;
    ASSERT_TRUE(sdk::parse_console(line, action, result)) << result.message;
    char text[64];
    const int size = sdk::to_console(action, text, sizeof(text));
    ASSERT_EQ(size, static_cast<int>(std::strlen(line)));
    EXPECT_STREQ(text, line);
    ASSERT_TRUE(run_command(world, text).ok);
    if(action.type == sdk::ActionType::Set) {
      const auto *position = world.get<sdk::Transform>(player);
      ASSERT_NE(position, nullptr);
      EXPECT_FLOAT_EQ(position->x, 200);
    }
  }
  EXPECT_FALSE(world.alive(player));
}

TEST(WorldAction, PrintersHandleBufferLimitsAndSceneBoundaries) {
  sdk::Action action;
  sdk::Result result;
  ASSERT_TRUE(sdk::parse_console("set player.Transform.x 200", action, result));
  char scene[64];
  EXPECT_EQ(sdk::to_scene(action, scene, sizeof(scene)), 15);
  EXPECT_STREQ(scene, "Transform x=200");
  EXPECT_EQ(sdk::to_scene(action, scene, sizeof("Transform x=200") - 1), -1);
  EXPECT_STREQ(scene, "");
  char small[8] = "old";
  EXPECT_EQ(sdk::to_console(action, small, sizeof(small)), -1);
  EXPECT_STREQ(small, "");
  EXPECT_EQ(sdk::to_cpp(action, small, sizeof(small)), -1);
  EXPECT_STREQ(small, "");

  ASSERT_TRUE(sdk::parse_console("remove player.Transform", action, result));
  EXPECT_EQ(sdk::to_scene(action, scene, sizeof(scene)), -1);
  ASSERT_TRUE(sdk::parse_console("set player.Caption.text two \"words\"\t\\#", action, result));
  ASSERT_GT(sdk::to_scene(action, scene, sizeof(scene)), 0);
  EXPECT_STREQ(scene, "Caption text=\"two \\\"words\\\"\\011\\\\#\"");
  ASSERT_TRUE(sdk::parse_console("set player.Name.value other", action, result));
  EXPECT_EQ(sdk::to_scene(action, scene, sizeof(scene)), -1);
}

TEST(WorldAction, SceneCommandsPrintReplayableSyntax) {
  sdk::Action action;
  sdk::Result result;
  char text[160];
  ASSERT_TRUE(sdk::parse_console("spawn enemy 300 100", action, result)) << result.message;
  EXPECT_EQ(action.type, sdk::ActionType::Spawn);
  ASSERT_GT(sdk::to_console(action, text, sizeof(text)), 0);
  EXPECT_STREQ(text, "spawn enemy 300 100");
  ASSERT_GT(sdk::to_cpp(action, text, sizeof(text)), 0);
  EXPECT_STREQ(text, "sdk::apply(world, sdk::Action{sdk::ActionType::Spawn, \"enemy\", \"300\", \"100\"}, &scenes);");
  ASSERT_GT(sdk::to_scene(action, text, sizeof(text)), 0);
  EXPECT_STREQ(text, "entity enemy : enemy name=\"\"\n  Transform x=300 y=100");

  ASSERT_TRUE(sdk::parse_console("load_scene project/level.scene", action, result)) << result.message;
  EXPECT_EQ(action.type, sdk::ActionType::LoadScene);
  ASSERT_GT(sdk::to_console(action, text, sizeof(text)), 0);
  EXPECT_STREQ(text, "load_scene project/level.scene");
  ASSERT_GT(sdk::to_cpp(action, text, sizeof(text)), 0);
  EXPECT_STREQ(text, "sdk::apply(world, sdk::Action{sdk::ActionType::LoadScene, \"project/level.scene\", \"\", \"\"}, &scenes);");
  EXPECT_EQ(sdk::to_scene(action, text, sizeof(text)), -1);
  EXPECT_STREQ(text, "");

  for(const char *line : {"spawn enemy 300", "spawn enemy 300 100 extra", "spawn enemy inf 100",
                          "spawn enemy 300 nan", "load_scene", "load_scene project/level.scene extra"}) {
    SCOPED_TRACE(line);
    EXPECT_FALSE(sdk::parse_console(line, action, result));
    EXPECT_EQ(action.type, sdk::ActionType::LoadScene);
    EXPECT_STREQ(action.target, "project/level.scene");
  }
}
