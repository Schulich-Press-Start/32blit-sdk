#include <gtest/gtest.h>
#include "actions.hpp"
#include "scene.hpp"

#include <array>
#include <cstring>
#include <string>

namespace {
  struct Link { sdk::Entity target; };
  SDK_COMPONENT(Link, target);
  struct Caption { char text[sdk::string_field_size] = {}; };
  SDK_COMPONENT(Caption, text);

  struct File { std::string_view path, content; };
  struct Files { const File *data; size_t count; int opens = 0; };

  bool read_file(void *ctx, std::string_view path, std::string_view &text) {
    auto &files = *static_cast<Files *>(ctx);
    ++files.opens;
    for(size_t i = 0; i < files.count; ++i)
      if(files.data[i].path == path) {
        text = files.data[i].content;
        return true;
      }
    return false;
  }

  sdk::Result run(sdk::World &world, sdk::SceneRuntime &scenes, const char *line) {
    sdk::Action action;
    sdk::Result result;
    return sdk::parse_console(line, action, result) ? sdk::apply(world, action, &scenes) : result;
  }
}

TEST(Scene, AuthoredEditsPreserveStableReferencesAndIncludeOwnership) {
  constexpr std::string_view root =
    "scene 1 Level\ninclude maps/props.scene\nprefab enemy\n  Transform2D x=30 y=40 rotation=5\n"
    "entity p001 : enemy name=player\n  Transform2D x=100\n";
  const File inputs[] = {{"maps/props.scene", "scene 1 Props\nentity pickup\n  Link target=@p001\n"}};
  Files files{inputs, 1};
  const sdk::SceneReader reader{&files, read_file};
  const sdk::SceneLimits limits{8, 8, 16, 3};
  sdk::SceneDocument doc(limits), replay(limits);
  sdk::SceneError error;
  ASSERT_TRUE(sdk::read_scene(doc, root, "level.scene", reader, error)) << error.file << ':' << error.line << ' ' << error.message;
  EXPECT_FALSE(sdk::rename_scene_entity(doc, "p001", "pickup", error));
  EXPECT_FALSE(sdk::set_scene_field(doc, "pickup", "Link", "target", "null", error));
  EXPECT_FALSE(sdk::set_scene_field(doc, "p001", "Transform2D", "x", "nan", error));
  ASSERT_TRUE(sdk::rename_scene_entity(doc, "p001", "hero", error));
  ASSERT_TRUE(sdk::set_scene_field(doc, "p001", "Transform2D", "x", "121", error));
  ASSERT_TRUE(sdk::set_scene_field(doc, "p001", "Caption", "text", "a \"quote\"", error));

  char text[1024], second[1024];
  const int count = sdk::write_scene(doc, text, sizeof(text));
  ASSERT_GT(count, 0);
  EXPECT_NE(std::strstr(text, "include maps/props.scene"), nullptr);
  EXPECT_EQ(std::strstr(text, "entity pickup"), nullptr);
  ASSERT_TRUE(sdk::read_scene(replay, {text, static_cast<size_t>(count)}, "level.scene", reader, error)) << error.message;
  ASSERT_EQ(sdk::write_scene(replay, second, sizeof(second)), count);
  EXPECT_STREQ(text, second);

  sdk::World world(4);
  sdk::ComponentPool<sdk::Name> names(world, 4);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 4);
  sdk::ComponentPool<Link> links(world, 4);
  sdk::ComponentPool<Caption> captions(world, 4);
  std::array<sdk::Entity, 8> bindings;
  ASSERT_TRUE(sdk::instantiate_scene(world, replay, bindings.data(), bindings.size(), error)) << error.message;
  const auto hero = world.find("hero");
  const auto *transform = world.get<sdk::Transform2D>(hero);
  const auto *caption = world.get<Caption>(hero);
  const auto *link = world.get<Link>(world.find("pickup"));
  ASSERT_NE(transform, nullptr);
  ASSERT_NE(link, nullptr);
  ASSERT_NE(caption, nullptr);
  EXPECT_TRUE(world.alive(link->target));
  EXPECT_EQ(link->target.index(), hero.index());
  EXPECT_STREQ(caption->text, "a \"quote\"");
  EXPECT_FLOAT_EQ(transform->x, 121);
  EXPECT_FLOAT_EQ(transform->y, 40);
  EXPECT_FLOAT_EQ(transform->rotation, 5);
}

TEST(Scene, CachedCatalogueRemapsReferencesAndPreservesLargerLoadBudgets) {
  const sdk::SceneLimits limits{8, 16, 32, 2};
  const File inputs[] = {
    {"large.scene", "scene 1 Large\nentity a\n Transform2D x=1\nentity b\n Transform2D x=2\nentity c\n Transform2D x=3\n"},
    {"bad.scene", "scene 1 Bad\nentity missing_pool\n Caption text=hello\n"}
  };
  Files files{inputs, 2};
  sdk::SceneDocument original(limits);
  sdk::SceneError error;
  ASSERT_TRUE(sdk::read_scene(original, "scene 1 Small\nentity hero\n Transform2D x=42\nentity follower\n Link target=@hero\n",
                              "small.scene", {}, error));
  auto cached = original.compact_copy();
  sdk::World world(8);
  sdk::ComponentPool<sdk::Name> names(world, 8);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 8);
  sdk::ComponentPool<Link> links(world, 8);
  sdk::SceneRuntime runtime(world, limits, {&files, read_file});
  ASSERT_TRUE(runtime.load(*cached, error)) << error.message;
  const auto old_hero = world.find("hero");
  ASSERT_TRUE(runtime.load(*cached, error)) << error.message;
  EXPECT_FALSE(world.alive(old_hero));
  const auto hero = world.find("hero");
  const auto target = world.get<Link>(world.find("follower"))->target;
  EXPECT_TRUE(world.alive(target));
  EXPECT_EQ(target.index(), hero.index());
  EXPECT_EQ(target.generation(), hero.generation());
  EXPECT_FLOAT_EQ(world.get<sdk::Transform2D>(hero)->x, 42);
  EXPECT_FALSE(runtime.load("bad.scene", error));
  EXPECT_TRUE(world.alive(hero));
  EXPECT_EQ(world.get<Link>(world.find("follower"))->target.index(), hero.index());
  EXPECT_EQ(world.get<Link>(world.find("follower"))->target.generation(), hero.generation());
  ASSERT_TRUE(runtime.load("large.scene", error)) << error.message;
  EXPECT_FLOAT_EQ(world.get<sdk::Transform2D>(world.find("c"))->x, 3);
  EXPECT_EQ(world.size(), 3);
}

TEST(Scene, LongPathEditsAndSnapshotsRoundTrip) {
  const std::string longest = "assets/folder with space/" + std::string(sdk::path_field_size - 26, 'a');
  ASSERT_EQ(longest.size(), sdk::path_field_size - 1);
  sdk::SceneDocument doc(sdk::SceneLimits{});
  sdk::SceneError error;
  ASSERT_TRUE(sdk::read_scene(doc, "scene 1 Level\nentity hero\n  Sprite\n", "level.scene", {}, error));
  ASSERT_TRUE(sdk::set_scene_field(doc, "hero", "Sprite", "asset", longest, error)) << error.message;
  EXPECT_FALSE(sdk::set_scene_field(doc, "hero", "Sprite", "asset", longest + "a", error));
  EXPECT_NE(std::strstr(error.message, "Sprite.asset path too long (max 63 UTF-8 bytes)"), nullptr);
  EXPECT_FALSE(sdk::set_scene_field(doc, "hero", "Sprite", "asset", "nested/../escape", error));
  EXPECT_NE(std::strstr(error.message, "Sprite.asset: expected a project-relative path"), nullptr);

  char text[1024];
  const int scene_size = sdk::write_scene(doc, text, sizeof(text));
  ASSERT_GT(scene_size, 0);
  ASSERT_TRUE(sdk::read_scene(doc, {text, static_cast<size_t>(scene_size)}, "level.scene", {}, error)) << error.message;
  sdk::World world(2);
  sdk::ComponentPool<sdk::Name> names(world, 2);
  sdk::ComponentPool<sdk::Sprite> sprites(world, 2);
  sdk::Entity bindings[2];
  ASSERT_TRUE(sdk::instantiate_scene(world, doc, bindings, 2, error)) << error.message;
  const auto *sprite = world.get<sdk::Sprite>(world.find("hero"));
  ASSERT_NE(sprite, nullptr);
  EXPECT_STREQ(sprite->asset, longest.c_str());
  ASSERT_NE(world.add<sdk::Sprite>(world.create("blank")), nullptr);

  const auto snapshot_size = sdk::save_world(world, text, sizeof(text));
  ASSERT_GT(snapshot_size, 0);
  ASSERT_TRUE(sdk::load_world(world, {text, static_cast<size_t>(snapshot_size)}, doc, bindings, 2, error)) << error.message;
  sprite = world.get<sdk::Sprite>(world.find("hero"));
  ASSERT_NE(sprite, nullptr);
  EXPECT_STREQ(sprite->asset, longest.c_str());
  sprite = world.get<sdk::Sprite>(world.find("blank"));
  ASSERT_NE(sprite, nullptr);
  EXPECT_STREQ(sprite->asset, "");
}

TEST(Scene, DecodedPathLengthErrorsNameTheFieldAndLimit) {
  sdk::SceneDocument doc(sdk::SceneLimits{});
  sdk::SceneError error;
  const std::string root = "scene 1 Level\nentity hero\n  Sprite asset=";
  const auto value = std::string(sdk::path_field_size + 1, 'a');
  for(const auto &token : {value, '"' + value + '"'}) {
    EXPECT_FALSE(sdk::read_scene(doc, root + token + '\n', "level.scene", {}, error));
    EXPECT_EQ(error.line, 3u);
    EXPECT_NE(std::strstr(error.message, "Sprite.asset path too long (max 63 UTF-8 bytes)"), nullptr);
  }
  EXPECT_FALSE(sdk::read_scene(doc, root + "\"a/\\000b\"\n", "level.scene", {}, error));
  EXPECT_NE(std::strstr(error.message, "Sprite.asset: expected a project-relative path"), nullptr);
}

TEST(Scene, MetadataEditsRoundTripToFreshPlay) {
  const sdk::FieldInfo caption_fields[] = {{"text", sdk::FieldType::String, 0}};
  const sdk::FieldInfo transform_fields[] = {{"x", sdk::FieldType::Float, 0}, {"y", sdk::FieldType::Float, 0}};
  const sdk::ComponentInfo types[] = {{"Caption", 0, 0, caption_fields, 1}, {"Transform2D", 1, 0, transform_fields, 2}};
  File inputs[] = {
    {"root.scene", "scene 1 Level\ninclude map.scene\nprefab actor\n  Transform2D x=5 y=2\n"
                   "entity player : actor\n  Transform2D x=8 y=3\n  Caption text=old\n"},
    {"map.scene", "scene 1 Map\nentity bat : actor\n  Transform2D y=9\n"}
  };
  Files files{inputs, 2};
  const sdk::SceneReader reader{&files, read_file};
  const sdk::SceneLimits limits{8, 8, 16, 2};
  sdk::SceneDocument authored(limits);
  sdk::SceneError error;
  ASSERT_TRUE(sdk::read_scene_authoring(authored, "root.scene", reader, error, types, 2)) << error.message;
  const auto index = [&](std::string_view key) {
    uint32_t i = 0;
    for(; i < authored.objects().size(); ++i) if(authored.objects()[i].key == key) break;
    return i;
  };
  const auto player = index("player");
  const std::string_view text = "a \"quote\"", invalid = "nan";
  EXPECT_FALSE(authored.edit_field(player, 1, 0, &invalid, error));
  EXPECT_FALSE(authored.edit_component(index("bat"), 0, true, error));
  ASSERT_TRUE(authored.edit_field(player, 1, 0, nullptr, error));
  ASSERT_TRUE(authored.edit_component(player, 0, false, error));
  ASSERT_TRUE(authored.add_object("npc", "actor", false, error));
  ASSERT_TRUE(authored.edit_component(index("npc"), 0, true, error));
  ASSERT_TRUE(authored.edit_field(index("npc"), 0, 0, &text, error));

  sdk::World world(8);
  sdk::ComponentPool<sdk::Name> names(world, 8);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 8);
  sdk::ComponentPool<Caption> captions(world, 8);
  std::array<sdk::Entity, 8> bindings;
  EXPECT_FALSE(sdk::instantiate_scene(world, authored, bindings.data(), bindings.size(), error));
  char output[1024];
  const int size = sdk::write_scene(authored, output, sizeof(output));
  ASSERT_GT(size, 0);
  inputs[0].content = {output, static_cast<size_t>(size)};
  sdk::SceneRuntime runtime(world, limits, reader);
  ASSERT_TRUE(runtime.load("root.scene", error)) << error.message;
  const auto *transform = world.get<sdk::Transform2D>(world.find("player"));
  ASSERT_NE(transform, nullptr);
  EXPECT_FLOAT_EQ(transform->x, 5);
  EXPECT_FLOAT_EQ(transform->y, 3);
  EXPECT_EQ(world.get<Caption>(world.find("player")), nullptr);
  const auto *bat = world.get<sdk::Transform2D>(world.find("bat"));
  ASSERT_NE(bat, nullptr);
  EXPECT_FLOAT_EQ(bat->y, 9);
  const auto *npc = world.get<Caption>(world.find("npc"));
  ASSERT_NE(npc, nullptr);
  EXPECT_EQ(std::string_view(npc->text), text);

  ASSERT_TRUE(run(world, runtime, "set player.Transform2D.x 99").ok);
  EXPECT_FLOAT_EQ(transform->x, 99);
  ASSERT_TRUE(runtime.load("root.scene", error)) << error.message;
  transform = world.get<sdk::Transform2D>(world.find("player"));
  ASSERT_NE(transform, nullptr);
  EXPECT_FLOAT_EQ(transform->x, 5);
}

TEST(Scene, InvalidLoadKeepsLiveEntitiesAndActivePrefabs) {
  const File inputs[] = {
    {"good.scene", "scene 1 Good\nprefab enemy\n  Transform2D rotation=5\nentity player\n  Transform2D x=9\n"},
    {"bad.scene", "scene 1 Bad\ninclude extra/bad.scene\n"},
    {"extra/bad.scene", "scene 1 Bad\nentity error\n  Transform2D bad=1\n"},
    {"full.scene", "scene 1 Full\nentity one\n  Transform2D x=1\nentity two\n  Transform2D x=2\nentity three\n  Transform2D x=3\n"},
  };
  Files files{inputs, 4};
  sdk::World world(4);
  sdk::ComponentPool<sdk::Name> names(world, 4);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 2);
  sdk::SceneRuntime scenes(world, {8, 8, 16, 3}, {&files, read_file});
  ASSERT_TRUE(run(world, scenes, "load_scene good.scene").ok);
  const auto player = world.find("player");
  ASSERT_TRUE(player);
  const auto bad = run(world, scenes, "load_scene bad.scene");
  EXPECT_FALSE(bad.ok);
  EXPECT_STREQ(bad.file, "extra/bad.scene");
  EXPECT_EQ(bad.line, 3u);
  const auto full = run(world, scenes, "load_scene full.scene");
  EXPECT_FALSE(full.ok);
  EXPECT_STREQ(full.file, "full.scene");
  EXPECT_TRUE(world.alive(player));
  ASSERT_NE(world.get<sdk::Transform2D>(player), nullptr);
  EXPECT_FLOAT_EQ(world.get<sdk::Transform2D>(player)->x, 9);
  EXPECT_EQ(world.size(), 1);
  const auto spawned = run(world, scenes, "spawn enemy 300 100");
  ASSERT_TRUE(spawned.ok) << spawned.message;
  EXPECT_EQ(world.size(), 2);
  world.each<sdk::Transform2D>([&](sdk::Entity entity, sdk::Transform2D &t) {
    if(entity.index() != player.index()) {
      EXPECT_FLOAT_EQ(t.x, 300);
      EXPECT_FLOAT_EQ(t.y, 100);
      EXPECT_FLOAT_EQ(t.rotation, 5);
    }
  });
  EXPECT_FALSE(run(world, scenes, "spawn enemy 0 0").ok);
  EXPECT_EQ(world.size(), 2);
}

TEST(Scene, IncludeCyclesAndDuplicateKeysFailBeforeWorldAccess) {
  const File inputs[] = {
    {"cycle.scene", "scene 1 Loop\ninclude root.scene\n"},
    {"root.scene", "scene 1 Root\ninclude cycle.scene\n"},
    {"dup.scene", "scene 1 Dupe\nentity same\n"},
  };
  Files files{inputs, 3};
  const sdk::SceneReader reader{&files, read_file};
  sdk::SceneDocument doc({8, 8, 8, 4});
  sdk::SceneError error;
  EXPECT_FALSE(sdk::read_scene_file(doc, "../root.scene", reader, error));
  EXPECT_EQ(files.opens, 0);
  EXPECT_FALSE(sdk::read_scene_file(doc, "root.scene", reader, error));
  EXPECT_STREQ(error.file, "cycle.scene");
  EXPECT_EQ(error.line, 2u);
  EXPECT_TRUE(doc.objects().empty());
  EXPECT_FALSE(sdk::read_scene(doc, "scene 1 Root\ninclude dup.scene\nentity same\n", "root.scene", reader, error));
  EXPECT_TRUE(doc.objects().empty());
  EXPECT_FALSE(sdk::read_scene(doc, "scene 2 Root\n", "root.scene", reader, error));
  EXPECT_FALSE(sdk::read_scene(doc, "scene 1 Root\nentity hero\n  Link target=@missing\n", "root.scene", reader, error));
}

TEST(Scene, SnapshotRestoresUnnamedStateAndRemapsReferences) {
  sdk::World world(4);
  sdk::ComponentPool<sdk::Name> names(world, 3);
  sdk::ComponentPool<sdk::Transform2D> transforms(world, 2);
  sdk::ComponentPool<Link> links(world, 2);
  sdk::ComponentPool<Caption> captions(world, 2);
  const auto hero = world.create("hero"), unnamed = world.create(), stale = world.create();
  auto *hero_link = world.add<Link>(hero);
  auto *unnamed_link = world.add<Link>(unnamed);
  auto *transform = world.add<sdk::Transform2D>(unnamed);
  ASSERT_NE(hero_link, nullptr);
  ASSERT_NE(unnamed_link, nullptr);
  ASSERT_NE(transform, nullptr);
  ASSERT_NE(world.add<sdk::Name>(unnamed), nullptr);
  ASSERT_NE(world.add<Caption>(unnamed), nullptr);
  hero_link->target = unnamed;
  unnamed_link->target = stale;
  ASSERT_TRUE(world.destroy(stale));
  transform->x = 30;

  char text[1024];
  const int length = sdk::save_world(world, text, sizeof(text));
  ASSERT_GT(length, 0);
  EXPECT_EQ(std::string_view(text, 16), "snapshot 1 World");
  transform->x = 70;
  sdk::SceneDocument scratch({8, 8, 16, 2});
  sdk::SceneError error;
  std::array<sdk::Entity, 8> bindings;
  EXPECT_FALSE(sdk::load_world(world, "scene 1 No\n", scratch, bindings.data(), bindings.size(), error));
  EXPECT_TRUE(world.alive(hero));
  ASSERT_TRUE(sdk::load_world(world, {text, static_cast<size_t>(length)}, scratch,
                              bindings.data(), bindings.size(), error)) << error.message;
  EXPECT_FALSE(world.alive(hero));
  EXPECT_FALSE(world.alive(unnamed));
  EXPECT_EQ(world.size(), 2);
  const auto *restored_link = world.get<Link>(world.find("hero"));
  ASSERT_NE(restored_link, nullptr);
  EXPECT_TRUE(world.alive(restored_link->target));
  const auto *restored_transform = world.get<sdk::Transform2D>(restored_link->target);
  const auto *restored_name = world.get<sdk::Name>(restored_link->target);
  const auto *restored_caption = world.get<Caption>(restored_link->target);
  const auto *restored_inner_link = world.get<Link>(restored_link->target);
  ASSERT_NE(restored_transform, nullptr);
  ASSERT_NE(restored_name, nullptr);
  ASSERT_NE(restored_caption, nullptr);
  ASSERT_NE(restored_inner_link, nullptr);
  EXPECT_FLOAT_EQ(restored_transform->x, 30);
  EXPECT_STREQ(restored_name->value, "");
  EXPECT_STREQ(restored_caption->text, "");
  EXPECT_FALSE(restored_inner_link->target);
}

TEST(Scene, RuntimeSnapshotReopensCatalogueAndRebindsPrefabReferences) {
  const File inputs[] = {
    {"level.scene", "scene 1 Level\nprefab follower\n  Link target=@p1\nentity p1 name=player\n  Transform2D x=9\n"},
  };
  Files files{inputs, 1};
  sdk::SceneError error;
  sdk::World first(4), second(4);
  sdk::ComponentPool<sdk::Name> first_names(first, 4), second_names(second, 4);
  sdk::ComponentPool<sdk::Transform2D> first_transforms(first, 4), second_transforms(second, 4);
  sdk::ComponentPool<Link> first_links(first, 4), second_links(second, 4);
  const sdk::SceneLimits limits{8, 8, 16, 2};
  sdk::SceneRuntime original(first, limits, {&files, read_file}), replacement(second, limits, {&files, read_file});
  ASSERT_TRUE(original.load("level.scene", error)) << error.message;
  ASSERT_TRUE(run(first, original, "set player.Transform2D.x 200").ok);
  char text[1024];
  const int count = original.save(text, sizeof(text));
  ASSERT_GT(count, 0);
  ASSERT_TRUE(replacement.restore({text, static_cast<size_t>(count)}, error)) << error.message;
  const auto player = second.find("player");
  const auto *player_transform = second.get<sdk::Transform2D>(player);
  ASSERT_NE(player_transform, nullptr);
  EXPECT_FLOAT_EQ(player_transform->x, 200);
  const auto follower = replacement.spawn("follower", 4, 5, error);
  ASSERT_TRUE(follower) << error.message;
  const auto *link = second.get<Link>(follower);
  ASSERT_NE(link, nullptr);
  EXPECT_TRUE(second.alive(link->target));
  EXPECT_EQ(link->target.index(), player.index());
}
