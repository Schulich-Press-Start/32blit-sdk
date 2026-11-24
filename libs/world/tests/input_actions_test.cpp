#include "input_actions.hpp"
#include "../../../32blit-sdl/ActionInput.hpp"

#include <SDL_gamecontroller.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace {
  blit::ActionInput snapshot;

  class NamedInput : public testing::Test {
  protected:
    void SetUp() override {
      snapshot = {};
      std::filesystem::remove(path);
    }
    void TearDown() override { std::filesystem::remove(path); }

    void write(const char *text) {
      std::ofstream file(path, std::ios::binary);
      file << text;
      file.close();
      ASSERT_TRUE(file);
    }

    const std::string path = "blit-input-actions-test.bindings";
    sdk::InputActions input;
  };
}

// This test links the mapping but does not start the SDL game loop.
namespace blit {
  const ActionInput &action_input() { return snapshot; }
}

TEST_F(NamedInput, CombinesKeyboardAndGamepadStateBeforeComputingEdges) {
  using Device = sdk::InputActions::Device;
  const auto jump = input.action("jump");
  const auto unmapped = input.action("do-nothing");
  EXPECT_EQ(input.action("jump"), jump);
  EXPECT_NE(input.action("Jump"), jump);
  ASSERT_TRUE(input.bind(jump, Device::Keyboard, "Space"));
  ASSERT_TRUE(input.bind(jump, Device::Keyboard, "Left Shift"));
  ASSERT_TRUE(input.bind(jump, Device::Gamepad, "a"));
  EXPECT_FALSE(input.bind(jump, Device::Keyboard, "not a scancode"));
  EXPECT_FALSE(input.bind(jump, Device::Gamepad, "not a button"));
  EXPECT_FALSE(input.bind({}, Device::Keyboard, "Space"));

  snapshot.keys[SDL_SCANCODE_SPACE] = true;
  input.update();
  EXPECT_TRUE(input.down(jump));
  EXPECT_TRUE(input.pressed(jump));
  EXPECT_FALSE(input.released(jump));
  EXPECT_FALSE(input.down(unmapped));
  EXPECT_FALSE(input.down({}));

  snapshot.keys[SDL_SCANCODE_LSHIFT] = true;
  snapshot.pad = uint32_t{1} << SDL_CONTROLLER_BUTTON_A;
  input.update();
  EXPECT_FALSE(input.pressed(jump));
  snapshot.keys.reset();
  input.update();
  EXPECT_TRUE(input.down(jump));
  EXPECT_FALSE(input.pressed(jump));
  EXPECT_FALSE(input.released(jump));

  snapshot.pad = 0;
  snapshot.keys[SDL_SCANCODE_LSHIFT] = true;
  input.update();
  EXPECT_TRUE(input.down(jump));
  EXPECT_FALSE(input.pressed(jump));
  EXPECT_FALSE(input.released(jump));

  snapshot.keys.reset();
  input.update();
  EXPECT_FALSE(input.down(jump));
  EXPECT_FALSE(input.pressed(jump));
  EXPECT_TRUE(input.released(jump));
  input.update();
  EXPECT_FALSE(input.released(jump));
}

TEST_F(NamedInput, ReplacesBindingsOnlyAfterReadingAWholeValidFile) {
  const auto jump = input.action("jump");
  ASSERT_TRUE(input.bind(jump, sdk::InputActions::Device::Keyboard, "Space"));
  EXPECT_FALSE(input.load(path));
  ASSERT_NO_FATAL_FAILURE(write("jump keyboard B\njump gamepad unknown_button\n"));
  EXPECT_FALSE(input.load(path));
  snapshot.keys[SDL_SCANCODE_B] = true;
  input.update();
  EXPECT_FALSE(input.down(jump));
  snapshot.keys[SDL_SCANCODE_SPACE] = true;
  input.update();
  EXPECT_TRUE(input.down(jump));

  ASSERT_NO_FATAL_FAILURE(write("\xef\xbb\xbf  # default bindings\r\n\r\nroll\tkeyboard Left Shift  \r\n"
                                "jump gamepad b\r\n"));
  ASSERT_TRUE(input.load(path));
  EXPECT_EQ(input.action("jump"), jump);
  const auto roll = input.action("roll");
  snapshot.keys[SDL_SCANCODE_LSHIFT] = true;
  input.update();
  EXPECT_TRUE(input.released(jump));
  EXPECT_TRUE(input.pressed(roll));
  snapshot.pad = uint32_t{1} << SDL_CONTROLLER_BUTTON_B;
  input.update();
  EXPECT_TRUE(input.pressed(jump));

  ASSERT_NO_FATAL_FAILURE(write("# no bindings\n"));
  ASSERT_TRUE(input.load(path));
  input.update();
  EXPECT_TRUE(input.released(jump));
  EXPECT_TRUE(input.released(roll));
}
