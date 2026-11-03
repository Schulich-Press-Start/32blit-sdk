/**
 * \file panel_test.cpp
 * \brief Unit tests for panel.cpp: converting game frames into panel pixel layouts.
 *
 * Expected pixels are built from each layout's channel masks, not with
 * PanelFormat::pack(), so the tests don't reuse the code they check.
 * Colours are pure primaries, black and white, whose expected value in
 * any layout is "every bit of the channel set" or "none".
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "panel.hpp"

namespace {
  using blit::Pen;
  using blit::PixelFormat;
  using blit::Size;
  using namespace blit_linux;

  /** \brief A panel pixel layout with a name for test output. */
  struct Layout {
    const char *name;
    PanelFormat format;
  };

  /** \brief Show the layout's name instead of its bytes in gtest output. */
  void PrintTo(const Layout &layout, std::ostream *os) {
    *os << layout.name;
  }

  PanelFormat make_format(int bytes_per_pixel, PanelChannel red, PanelChannel green, PanelChannel blue, uint32_t opaque_bits = 0) {
    PanelFormat format;
    format.bytes_per_pixel = bytes_per_pixel;
    format.red = red;
    format.green = green;
    format.blue = blue;
    format.opaque_bits = opaque_bits;
    return format;
  }

  // channel = {offset, length}
  const Layout layouts[] = {
    {"RGB565", make_format(2, {11, 5}, {5, 6}, {0, 5})},
    {"XRGB8888", make_format(4, {16, 8}, {8, 8}, {0, 8}, 0xFF000000)},
    {"XBGR8888", make_format(4, {0, 8}, {8, 8}, {16, 8})},
    {"RGB666", make_format(4, {12, 6}, {6, 6}, {0, 6})},
  };

  const Pen red(255, 0, 0), green(0, 255, 0), blue(0, 0, 255), white(255, 255, 255), black(0, 0, 0);

  /** \brief Every bit of one channel set. */
  uint32_t mask(const PanelChannel &channel) {
    return ((1u << channel.length) - 1) << channel.offset;
  }

  /** \brief Panel pixel for a colour whose channels are each 0 or 255. */
  uint32_t expected_pixel(const PanelFormat &format, const Pen &colour) {
    return (colour.r ? mask(format.red) : 0)
         | (colour.g ? mask(format.green) : 0)
         | (colour.b ? mask(format.blue) : 0)
         | format.opaque_bits;
  }

  /** \brief A game frame in one of the three screen formats, built from a list of colours (row by row). */
  struct TestFrame {
    std::vector<uint8_t> data;
    std::vector<Pen> palette = std::vector<Pen>(256);
    blit::SurfaceInfo info;

    TestFrame(PixelFormat format, Size size, const std::vector<Pen> &colours) {
      for(size_t i = 0; i < colours.size(); i++) {
        const Pen &c = colours[i];

        if(format == PixelFormat::RGB) {
          data.insert(data.end(), {c.r, c.g, c.b});
        } else if(format == PixelFormat::RGB565) {
          // 32blit's layout: red in the low bits (see pack_rgb565 in 32blit/graphics/blend.cpp)
          uint16_t v = uint16_t((c.r >> 3) | ((c.g >> 2) << 5) | ((c.b >> 3) << 11));
          uint8_t bytes[2];
          memcpy(bytes, &v, 2);
          data.insert(data.end(), {bytes[0], bytes[1]});
        } else {
          // P: one palette entry per pixel
          palette[i] = c;
          data.push_back(uint8_t(i));
        }
      }

      info.data = data.data();
      info.bounds = size;
      info.format = format;
      info.palette = palette.data();
    }
  };

  /** \brief Panel memory with padding after every row, pre-filled with a marker so untouched bytes show up. */
  struct TestPanel {
    static constexpr uint8_t marker = 0xAA;
    /// Bytes after each row that must never be written.
    static constexpr uint32_t padding = 8;

    PanelFormat format;
    std::vector<uint8_t> memory;
    PanelBuffer buffer;

    TestPanel(const PanelFormat &format, Size size) : format(format) {
      buffer.size = size;
      buffer.line_length = uint32_t(size.w * format.bytes_per_pixel) + padding;
      memory.assign(size_t(buffer.line_length) * size.h, marker);
      buffer.pixels = memory.data();
    }

    uint32_t pixel(int x, int y) const {
      uint32_t value = 0;
      memcpy(&value, memory.data() + size_t(y) * buffer.line_length + size_t(x) * format.bytes_per_pixel, format.bytes_per_pixel);
      return value;
    }

    bool padding_untouched() const {
      for(int y = 0; y < buffer.size.h; y++) {
        for(uint32_t i = buffer.line_length - padding; i < buffer.line_length; i++) {
          if(memory[size_t(y) * buffer.line_length + i] != marker)
            return false;
        }
      }
      return true;
    }

    bool untouched() const {
      for(auto byte : memory) {
        if(byte != marker)
          return false;
      }
      return true;
    }
  };

  const char *format_name(PixelFormat format) {
    switch(format) {
      case PixelFormat::RGB: return "RGB";
      case PixelFormat::RGB565: return "RGB565";
      case PixelFormat::P: return "Palette";
      default: return "Other";
    }
  }

  // ---- colours: every source format into every panel layout ------------------

  class PanelColours : public testing::TestWithParam<std::tuple<Layout, PixelFormat>> {
  protected:
    const PanelFormat &format() const { return std::get<0>(GetParam()).format; }
    PixelFormat source() const { return std::get<1>(GetParam()); }

    /** \brief Present a one-row frame of `colours` at 1x into a panel of the same size. */
    TestPanel present_row(const std::vector<Pen> &colours) {
      const Size size(int(colours.size()), 1);
      TestFrame frame(source(), size, colours);
      TestPanel panel(format(), size);

      clear_panel(panel.buffer, panel.format);
      present_frame(panel.buffer, panel.format, frame.info, {1, 0, 0});
      return panel;
    }
  };

  TEST_P(PanelColours, WhiteIsFullWhite) {
    const auto panel = present_row({white});

    EXPECT_EQ(panel.pixel(0, 0), expected_pixel(format(), white));
  }

  TEST_P(PanelColours, PrimariesKeepTheirOwnChannel) {
    const auto panel = present_row({red, green, blue, black});

    EXPECT_EQ(panel.pixel(0, 0), expected_pixel(format(), red));
    EXPECT_EQ(panel.pixel(1, 0), expected_pixel(format(), green));
    EXPECT_EQ(panel.pixel(2, 0), expected_pixel(format(), blue));
    EXPECT_EQ(panel.pixel(3, 0), expected_pixel(format(), black));
  }

  INSTANTIATE_TEST_SUITE_P(AllLayouts, PanelColours,
    testing::Combine(testing::ValuesIn(layouts), testing::Values(PixelFormat::RGB, PixelFormat::RGB565, PixelFormat::P)),
    [](const testing::TestParamInfo<PanelColours::ParamType> &info) {
      return std::string(format_name(std::get<1>(info.param))) + "_to_" + std::get<0>(info.param).name;
    });

  // ---- placement: scale, position, borders and row padding -------------------

  class PanelPlacement : public testing::TestWithParam<Layout> {
  protected:
    const PanelFormat &format() const { return GetParam().format; }
  };

  TEST_P(PanelPlacement, ScaledFrameLandsAtItsPositionAndBordersStayBlack) {
    // 3x2 frame at 2x, placed at (2, 1) on a 10x7 panel: covers columns 2-7 and rows 1-4
    const std::vector<Pen> colours = {red, green, blue, white, black, red};
    TestFrame frame(PixelFormat::RGB, Size(3, 2), colours);
    TestPanel panel(format(), Size(10, 7));
    const ScreenFit fit{2, 2, 1};

    clear_panel(panel.buffer, panel.format);
    present_frame(panel.buffer, panel.format, frame.info, fit);

    for(int y = 0; y < 7; y++) {
      for(int x = 0; x < 10; x++) {
        const bool inside = x >= 2 && x < 8 && y >= 1 && y < 5;
        const Pen &colour = inside ? colours[size_t((y - 1) / 2 * 3 + (x - 2) / 2)] : black;

        EXPECT_EQ(panel.pixel(x, y), expected_pixel(format(), colour)) << "at (" << x << ", " << y << ")";
      }
    }

    EXPECT_TRUE(panel.padding_untouched());
  }

  TEST_P(PanelPlacement, FrameThatWouldNotFitWritesNothing) {
    TestFrame frame(PixelFormat::RGB, Size(3, 2), {red, red, red, red, red, red});
    TestPanel panel(format(), Size(10, 7));

    // 6 wide from column 5 runs past column 9
    present_frame(panel.buffer, panel.format, frame.info, {2, 5, 1});
    // 4 tall from row 4 runs past row 6
    present_frame(panel.buffer, panel.format, frame.info, {2, 2, 4});
    present_frame(panel.buffer, panel.format, frame.info, {1, -1, 0});

    EXPECT_TRUE(panel.untouched());
  }

  INSTANTIATE_TEST_SUITE_P(AllLayouts, PanelPlacement, testing::ValuesIn(layouts),
    [](const testing::TestParamInfo<Layout> &info) {
      return std::string(info.param.name);
    });
}
