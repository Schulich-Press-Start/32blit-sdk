/**
 * \file file_test.cpp
 * \brief Unit tests for the POSIX file API in file.cpp.
 *
 * file_init() puts the save folder next to the running executable, so each
 * test gets its own folder under `<build dir>/32blit-linux/tests/saves/` and
 * deletes it afterwards. No hardware or special permissions needed.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "file.hpp"

namespace {
  using namespace blit_linux;

  /** \brief Gives each test a clean save folder and removes it afterwards. */
  class FileTest : public ::testing::Test {
  protected:
    void TearDown() override {
      // Delete only this test's own folder, and only if it really is inside saves/,
      // so a broken title check can't make a test delete the build folder.
      const std::string saves = (std::filesystem::read_symlink("/proc/self/exe").parent_path() / "saves/").string();
      const std::string folder = get_save_path();

      if(folder.compare(0, saves.size(), saves) == 0 && folder.find("/..") == std::string::npos)
        std::filesystem::remove_all(folder);
    }

    /** \brief Absolute path of a file inside the save folder. */
    static std::string save_file(const char *name) {
      return std::string(get_save_path()) + name;
    }

    static bool ends_with(const std::string &text, const std::string &end) {
      return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
    }
  };

  TEST_F(FileTest, SlashesInTitleStayInsideSavesFolder) {
    file_init("sps/slash-test");

    EXPECT_TRUE(ends_with(get_save_path(), "/saves/sps_slash-test/")) << get_save_path();
    EXPECT_TRUE(std::filesystem::is_directory(get_save_path()));
  }

  TEST_F(FileTest, DotDotTitleCannotEscapeSavesFolder) {
    file_init("..");

    EXPECT_TRUE(ends_with(get_save_path(), "/saves/game/")) << get_save_path();
  }

  TEST_F(FileTest, WritesAtOffsetsAndReadsBack) {
    file_init("sps-file-roundtrip");
    const auto path = save_file("data.bin");

    void *out = open_file(path, blit::OpenMode::write);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(write_file(out, 0, 4, "abcd"), 4);
    EXPECT_EQ(close_file(out), 0);

    // read | write must not truncate: overwrite the middle only
    void *edit = open_file(path, blit::OpenMode::read | blit::OpenMode::write);
    ASSERT_NE(edit, nullptr);
    EXPECT_EQ(write_file(edit, 1, 2, "XY"), 2);
    EXPECT_EQ(close_file(edit), 0);

    void *in = open_file(path, blit::OpenMode::read);
    ASSERT_NE(in, nullptr);
    char buffer[4] = {};
    EXPECT_EQ(get_file_length(in), 4u);
    EXPECT_EQ(read_file(in, 0, 4, buffer), 4);
    EXPECT_EQ(std::string(buffer, 4), "aXYd");
    EXPECT_EQ(close_file(in), 0);
  }

  TEST_F(FileTest, ReadStopsAtEndOfFile) {
    file_init("sps-file-short-read");
    const auto path = save_file("data.bin");

    void *out = open_file(path, blit::OpenMode::write);
    ASSERT_NE(out, nullptr);
    write_file(out, 0, 4, "abcd");
    close_file(out);

    void *in = open_file(path, blit::OpenMode::read);
    ASSERT_NE(in, nullptr);
    char buffer[10] = {};
    EXPECT_EQ(read_file(in, 2, sizeof(buffer), buffer), 2);
    EXPECT_EQ(std::string(buffer, 2), "cd");
    close_file(in);
  }

  TEST_F(FileTest, OpeningADirectoryFails) {
    file_init("sps-file-dir");

    EXPECT_EQ(open_file(get_save_path(), blit::OpenMode::read), nullptr);
  }
}
