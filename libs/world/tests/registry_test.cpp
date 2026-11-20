// Component registry: registration, lookup, and the field data SDK_COMPONENT
// records. The registry fills before main(), so these tests read what the
// registrars below produced.
#include <gtest/gtest.h>

#include "registry.hpp"

namespace {

  struct Health {
    int32_t current, max;
    bool invulnerable;
  };
  SDK_COMPONENT(Health, current, max, invulnerable);

  // the most fields SDK_COMPONENT accepts, to cover the end of its expansion
  struct Wide {
    uint32_t f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16;
  };
  SDK_COMPONENT(Wide, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16);

}

TEST(Registry, FindsComponentByName) {
  auto transform = sdk::find_component("Transform");

  ASSERT_NE(transform, nullptr);
  EXPECT_STREQ(transform->name, "Transform");
  EXPECT_EQ(transform->size, sizeof(sdk::Transform));
  EXPECT_EQ(transform->field_count, 3);
}

TEST(Registry, UnknownNameIsNotFound) {
  EXPECT_EQ(sdk::find_component("Nonexistent"), nullptr);
  EXPECT_EQ(sdk::find_component(nullptr), nullptr);
  EXPECT_EQ(sdk::find_component("transform"), nullptr);  // lookup is case-sensitive
}

TEST(Registry, RecordsFieldNamesTypesAndOffsets) {
  auto transform = sdk::find_component("Transform");
  ASSERT_NE(transform, nullptr);

  auto y = sdk::find_field(*transform, "y");
  ASSERT_NE(y, nullptr);
  EXPECT_EQ(y->type, sdk::FieldType::Float);
  EXPECT_EQ(y->offset, offsetof(sdk::Transform, y));

  auto health = sdk::find_component("Health");
  ASSERT_NE(health, nullptr);
  auto current = sdk::find_field(*health, "current");
  auto invulnerable = sdk::find_field(*health, "invulnerable");
  ASSERT_NE(current, nullptr);
  ASSERT_NE(invulnerable, nullptr);
  EXPECT_EQ(current->type, sdk::FieldType::Int32);
  EXPECT_EQ(invulnerable->type, sdk::FieldType::Bool);
  EXPECT_EQ(invulnerable->offset, offsetof(Health, invulnerable));
}

TEST(Registry, OffsetsReachTheRightMember) {
  sdk::Transform t{1.0f, 2.0f, 3.0f};
  auto rotation = sdk::find_field(*sdk::component_info<sdk::Transform>(), "rotation");
  ASSERT_NE(rotation, nullptr);

  // what a tool does: walk to the field with only the registry to go on
  auto value = *reinterpret_cast<const float *>(reinterpret_cast<const char *>(&t) + rotation->offset);
  EXPECT_FLOAT_EQ(value, 3.0f);
}

TEST(Registry, HandlesStringFields) {
  auto name = sdk::find_component("Name");
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(name->field_count, 1);
  EXPECT_EQ(name->fields[0].type, sdk::FieldType::String);
  EXPECT_EQ(name->size, sdk::string_field_size);
}

TEST(Registry, HandlesTheMaximumNumberOfFields) {
  auto wide = sdk::find_component("Wide");
  ASSERT_NE(wide, nullptr);
  EXPECT_EQ(wide->field_count, sdk::max_component_fields);
  EXPECT_STREQ(wide->fields[0].name, "f1");
  EXPECT_STREQ(wide->fields[sdk::max_component_fields - 1].name, "f16");
  EXPECT_EQ(wide->fields[sdk::max_component_fields - 1].offset, offsetof(Wide, f16));
}

TEST(Registry, IdsAreUniqueAndLookUpTheSameComponent) {
  ASSERT_GE(sdk::component_count(), 4);

  for(uint16_t id = 0; id < sdk::component_count(); id++) {
    auto component = sdk::component_by_id(id);
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->id, id);
    EXPECT_EQ(sdk::find_component(component->name), component);
  }

  EXPECT_EQ(sdk::component_by_id(sdk::component_count()), nullptr);
}
