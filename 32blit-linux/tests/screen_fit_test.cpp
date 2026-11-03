/**
 * \file screen_fit_test.cpp
 * \brief Unit tests for screen_fit.hpp: which screen sizes are allowed, and their scale and position.
 */
#include <gtest/gtest.h>

#include "screen_fit.hpp"

namespace {
  using blit::Size;
  using namespace blit_linux;

  /** \brief Compare a fit against the expected scale and position, printing all three on failure. */
  testing::AssertionResult fit_is(const ScreenFit &fit, int scale, int x, int y) {
    if(fit.scale == scale && fit.x == x && fit.y == y)
      return testing::AssertionSuccess();

    return testing::AssertionFailure()
        << "got scale " << fit.scale << " at (" << fit.x << ", " << fit.y << "), "
        << "expected scale " << scale << " at (" << x << ", " << y << ")";
  }

  const Size dev_board_panel(480, 272);
  const Size panel_640x480(640, 480);

  TEST(ScreenFit, DefaultModesKeepTheirPlaceOnTheDevBoardPanel) {
    EXPECT_TRUE(fit_is(fit_screen(dev_board_panel, Size(160, 120)), 2, 80, 16));
    EXPECT_TRUE(fit_is(fit_screen(dev_board_panel, Size(320, 240)), 1, 80, 16));
  }

  TEST(ScreenFit, DefaultModesFillA640x480Panel) {
    EXPECT_TRUE(fit_is(fit_screen(panel_640x480, Size(160, 120)), 4, 0, 0));
    EXPECT_TRUE(fit_is(fit_screen(panel_640x480, Size(320, 240)), 2, 0, 0));
  }

  TEST(ScreenFit, ShorterPanelSideLimitsTheScale) {
    // 480 / 96 = 5 but 272 / 96 = 2
    EXPECT_TRUE(fit_is(fit_screen(dev_board_panel, Size(96, 96)), 2, 144, 40));
  }

  TEST(ScreenFit, PanelSizedScreenFillsThePanel) {
    EXPECT_TRUE(fit_is(fit_screen(dev_board_panel, dev_board_panel), 1, 0, 0));
  }

  TEST(ScreenFit, OddLeftoverPixelGoesToTheBottomBorder) {
    // 16:9 at 480x270 leaves 2 rows: one above, one below
    EXPECT_TRUE(fit_is(fit_screen(dev_board_panel, Size(480, 270)), 1, 0, 1));
    // 3 rows left over: 1 above, 2 below
    EXPECT_TRUE(fit_is(fit_screen(dev_board_panel, Size(480, 269)), 1, 0, 1));
  }

  TEST(ScreenFit, AllowsSizesFromTheMinimumUpToThePanel) {
    EXPECT_TRUE(screen_size_allowed(dev_board_panel, Size(min_screen_size, min_screen_size)));
    EXPECT_TRUE(screen_size_allowed(dev_board_panel, Size(480, 270)));
    EXPECT_TRUE(screen_size_allowed(dev_board_panel, dev_board_panel));
  }

  TEST(ScreenFit, RejectsSizesLargerThanThePanel) {
    EXPECT_FALSE(screen_size_allowed(dev_board_panel, Size(481, 272)));
    EXPECT_FALSE(screen_size_allowed(dev_board_panel, Size(480, 273)));
    EXPECT_FALSE(screen_size_allowed(dev_board_panel, Size(3840, 2160)));
  }

  TEST(ScreenFit, RejectsSizesBelowTheMinimum) {
    EXPECT_FALSE(screen_size_allowed(dev_board_panel, Size(min_screen_size - 1, 120)));
    EXPECT_FALSE(screen_size_allowed(dev_board_panel, Size(160, min_screen_size - 1)));
    EXPECT_FALSE(screen_size_allowed(dev_board_panel, Size(0, 0)));
  }

  TEST(ScreenFit, RejectsEverythingBeforeTheDisplayIsOpen) {
    // display_size() is 0x0 until display_init() succeeds
    EXPECT_FALSE(screen_size_allowed(Size(), Size(160, 120)));
  }
}
