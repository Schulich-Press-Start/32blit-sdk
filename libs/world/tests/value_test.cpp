// Reading and writing field values as text. The property that matters is the
// round trip: anything format_value() writes, parse_value() must read back as
// the same value, bit for bit, because scene files and the console both rely
// on it.
#include <cstring>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "registry.hpp"

namespace {

  std::string format(sdk::FieldType type, const void *src) {
    char buf[sdk::max_value_text];
    const int length = sdk::format_value(type, src, buf, sizeof(buf));
    EXPECT_GE(length, 0);
    EXPECT_EQ(length, static_cast<int>(strlen(buf)));
    return buf;
  }

  /// Writes a value out, reads it back, and reports whether the bytes match.
  template<class T>
  bool round_trips(sdk::FieldType type, T value) {
    char text[sdk::max_value_text];
    if(sdk::format_value(type, &value, text, sizeof(text)) < 0)
      return false;

    T read_back{};
    if(!sdk::parse_value(type, text, &read_back))
      return false;

    return memcmp(&value, &read_back, sizeof(T)) == 0;
  }
}

TEST(Value, FormatsEachType) {
  const bool yes = true, no = false;
  EXPECT_EQ(format(sdk::FieldType::Bool, &yes), "true");
  EXPECT_EQ(format(sdk::FieldType::Bool, &no), "false");

  const int32_t negative = -42;
  EXPECT_EQ(format(sdk::FieldType::Int32, &negative), "-42");

  const uint32_t big = 4294967295u;
  EXPECT_EQ(format(sdk::FieldType::UInt32, &big), "4294967295");

  const float half = 0.5f;
  EXPECT_EQ(format(sdk::FieldType::Float, &half), "0.5");

  char name[sdk::string_field_size] = "player";
  EXPECT_EQ(format(sdk::FieldType::String, name), "player");
}

TEST(Value, ParsesEachType) {
  bool flag = false;
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::Bool, "true", &flag));
  EXPECT_TRUE(flag);
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::Bool, "false", &flag));
  EXPECT_FALSE(flag);

  // 1 and 0 as well, because people type them into the console
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::Bool, "1", &flag));
  EXPECT_TRUE(flag);
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::Bool, "0", &flag));
  EXPECT_FALSE(flag);

  int32_t whole = 0;
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::Int32, "-200", &whole));
  EXPECT_EQ(whole, -200);

  uint32_t unsigned_whole = 0;
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::UInt32, "7", &unsigned_whole));
  EXPECT_EQ(unsigned_whole, 7u);

  float number = 0.0f;
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::Float, "1.5", &number));
  EXPECT_FLOAT_EQ(number, 1.5f);
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::Float, "-1.5e3", &number));
  EXPECT_FLOAT_EQ(number, -1500.0f);

  char name[sdk::string_field_size] = {};
  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::String, "coin_1", &name));
  EXPECT_STREQ(name, "coin_1");
}

TEST(Value, FloatsRoundTripExactly) {
  // the awkward ones: values with no short decimal form, the extremes, and -0
  const float values[] = {
    0.0f, -0.0f, 1.0f, 0.1f, 1.0f / 3.0f, 3.14159265f, -2.7182818f,
    1e-30f, 1e30f, 16777216.0f, 16777217.0f,
    std::numeric_limits<float>::min(),
    std::numeric_limits<float>::max(),
    std::numeric_limits<float>::denorm_min(),
  };

  for(float value : values)
    EXPECT_TRUE(round_trips(sdk::FieldType::Float, value)) << "value " << value;
}

TEST(Value, IntegersAndBoolsRoundTrip) {
  const int32_t whole[] = {0, 1, -1, 2147483647, -2147483647 - 1};
  for(int32_t value : whole)
    EXPECT_TRUE(round_trips(sdk::FieldType::Int32, value)) << "value " << value;

  const uint32_t unsigned_whole[] = {0u, 1u, 4294967295u};
  for(uint32_t value : unsigned_whole)
    EXPECT_TRUE(round_trips(sdk::FieldType::UInt32, value)) << "value " << value;

  EXPECT_TRUE(round_trips(sdk::FieldType::Bool, true));
  EXPECT_TRUE(round_trips(sdk::FieldType::Bool, false));
}

TEST(Value, StringsRoundTripIncludingTheLongestThatFits) {
  char name[sdk::string_field_size] = {};
  const std::string longest(sdk::string_field_size - 1, 'x');

  ASSERT_TRUE(sdk::parse_value(sdk::FieldType::String, longest.c_str(), name));
  EXPECT_EQ(format(sdk::FieldType::String, name), longest);

  // empty text is not a value; clearing a string is a separate operation
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::String, "", name));
}

TEST(Value, RejectsTextThatIsNotAValue) {
  int32_t whole = 99;
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, "abc", &whole));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, "1.5", &whole));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, "12x", &whole));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, " 12", &whole));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, "12 ", &whole));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, "", &whole));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, nullptr, &whole));
  EXPECT_EQ(whole, 99) << "a rejected value must leave the field alone";

  // out of range for the type
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Int32, "2147483648", &whole));
  uint32_t unsigned_whole = 0;
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::UInt32, "-1", &unsigned_whole));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::UInt32, "4294967296", &unsigned_whole));

  bool flag = false;
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Bool, "True", &flag));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Bool, "yes", &flag));

  float number = 0.0f;
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Float, "1.0.0", &number));
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Float, "x", &number));

  char name[sdk::string_field_size] = "kept";
  const std::string too_long(sdk::string_field_size, 'x');
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::String, too_long.c_str(), name));
  EXPECT_STREQ(name, "kept") << "too long must fail, not truncate";
}

TEST(Value, RefusesInfinityAndNotANumber) {
  // std::from_chars reads both; a game has no use for either, and a nan
  // spreads through every sum it touches, so they are refused where a scene
  // file or console line can still be pointed at.
  float number = 1.0f;

  for(const char *text : {"inf", "-inf", "infinity", "nan", "NaN", "-nan"})
    EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Float, text, &number)) << "text " << text;

  EXPECT_FLOAT_EQ(number, 1.0f) << "a refused value must leave the field alone";

  // writing them still works, so a field that has already gone wrong shows up
  // in a dump, and the scene file it lands in is then refused on reload
  const float infinity = std::numeric_limits<float>::infinity();
  char buf[sdk::max_value_text];
  EXPECT_GT(sdk::format_value(sdk::FieldType::Float, &infinity, buf, sizeof(buf)), 0);
  EXPECT_FALSE(sdk::parse_value(sdk::FieldType::Float, buf, &number));
}

TEST(Value, ReportsWhenTheBufferIsTooSmall) {
  const int32_t value = -12345;
  char buf[4] = {'#', '#', '#', '#'};

  EXPECT_EQ(sdk::format_value(sdk::FieldType::Int32, &value, buf, 4), -1);
  EXPECT_EQ(sdk::format_value(sdk::FieldType::Int32, &value, buf, 0), -1);

  // exactly enough: six characters plus the terminator
  char exact[7];
  EXPECT_EQ(sdk::format_value(sdk::FieldType::Int32, &value, exact, sizeof(exact)), 6);
  EXPECT_STREQ(exact, "-12345");
}

TEST(Value, FormatsStringsWithNoTerminatorOfTheirOwn) {
  char full[sdk::string_field_size];
  memset(full, 'a', sizeof(full));  // every byte used, nothing left for a terminator

  char buf[sdk::max_value_text];
  const int length = sdk::format_value(sdk::FieldType::String, full, buf, sizeof(buf));

  EXPECT_EQ(length, sdk::string_field_size);
  EXPECT_EQ(strlen(buf), static_cast<size_t>(sdk::string_field_size));
}

TEST(Value, PathsRoundTripAtTheByteLimitAndRejectMalformedText) {
  const auto type = sdk::FieldType::Path;
  const auto longest = "dir/" + std::string(sdk::path_field_size - 7, 'a') + "\xc3\xa9";
  ASSERT_EQ(longest.size(), sdk::path_field_size - 1);
  char path[sdk::path_field_size] = {};
  ASSERT_TRUE(sdk::parse_value(type, longest.c_str(), path));
  EXPECT_EQ(format(type, path), longest);

  EXPECT_FALSE(sdk::parse_value(type, (longest + "a").c_str(), path));
  for(const char *invalid : {"/root/file", "a//b", "a/./b", "a/../b", "a/", "a\\b", "C:/a", "a/\tb"})
    EXPECT_FALSE(sdk::parse_value(type, invalid, path)) << invalid;
  EXPECT_STREQ(path, longest.c_str());

  ASSERT_TRUE(sdk::parse_value(type, "", path));
  EXPECT_EQ(format(type, path), "");
  for(char byte : path) EXPECT_EQ(byte, '\0');
  std::memset(path, 'a', sizeof(path));
  char text[sdk::max_value_text];
  EXPECT_EQ(sdk::format_value(type, path, text, sizeof(text)), -1);
}

TEST(Value, NamesEachType) {
  EXPECT_STREQ(sdk::field_type_name(sdk::FieldType::Bool), "bool");
  EXPECT_STREQ(sdk::field_type_name(sdk::FieldType::Float), "float");
  EXPECT_STREQ(sdk::field_type_name(sdk::FieldType::String), "string");
  EXPECT_STREQ(sdk::field_type_name(sdk::FieldType::Path), "path");
}
