/**
 * \file drm_select_test.cpp
 * \brief Unit tests for drm_select.hpp: connector, mode, CRTC and pixel-format choices, and frame pacing.
 */
#include <gtest/gtest.h>

#include "drm_select.hpp"

namespace {
  using namespace blit_linux;

  DrmMode mode(int w, int h, bool preferred = false) {
    DrmMode m;
    m.width = w;
    m.height = h;
    m.preferred = preferred;
    return m;
  }

  DrmConnector connector(bool connected, std::vector<DrmMode> modes) {
    DrmConnector c;
    c.connected = connected;
    c.modes = std::move(modes);
    return c;
  }

  TEST(DrmSelect, ConnectorSkipsDisconnectedAndModelessOutputs) {
    const std::vector<DrmConnector> connectors = {
      connector(false, {mode(1920, 1080)}),  // HDMI, nothing plugged in
      connector(true, {}),                   // connected but reports no modes
      connector(true, {mode(480, 272)}),     // the panel
    };
    EXPECT_EQ(choose_connector(connectors), 2);
  }

  TEST(DrmSelect, NoConnectorWhenNothingIsConnected) {
    EXPECT_EQ(choose_connector({connector(false, {mode(640, 480)})}), -1);
    EXPECT_EQ(choose_connector({}), -1);
  }

  TEST(DrmSelect, ModePrefersTheNativeModeWhereverItIsListed) {
    EXPECT_EQ(choose_mode({mode(1024, 768), mode(800, 600), mode(480, 272, true)}), 2);
  }

  TEST(DrmSelect, ModeFallsBackToTheFirstWhenNoneIsPreferred) {
    EXPECT_EQ(choose_mode({mode(800, 600), mode(640, 480)}), 0);
    EXPECT_EQ(choose_mode({}), -1);
  }

  TEST(DrmSelect, CrtcKeepsTheOneAlreadyDrivingTheConnector) {
    DrmConnector c = connector(true, {mode(640, 480)});
    c.current_crtc_id = 52;
    c.possible_crtcs = 0b11;
    EXPECT_EQ(choose_crtc(c, {41, 52}), 1);
  }

  TEST(DrmSelect, CrtcOtherwiseTakesTheFirstThatCanDriveTheConnector) {
    DrmConnector c = connector(true, {mode(640, 480)});
    c.possible_crtcs = 0b110;
    EXPECT_EQ(choose_crtc(c, {41, 52, 63}), 1);

    // a current CRTC that isn't in the list doesn't count
    c.current_crtc_id = 99;
    EXPECT_EQ(choose_crtc(c, {41, 52, 63}), 1);
  }

  TEST(DrmSelect, NoCrtcWhenNoneCanDriveTheConnector) {
    DrmConnector c = connector(true, {mode(640, 480)});
    c.possible_crtcs = 0b100;
    EXPECT_EQ(choose_crtc(c, {41, 52}), -1);
  }

  TEST(DrmSelect, FourccCodesMatchTheKernel) {
    // values of DRM_FORMAT_XRGB8888 and DRM_FORMAT_RGB565 in drm_fourcc.h
    EXPECT_EQ(drm_format_xrgb8888, 0x34325258u);
    EXPECT_EQ(drm_format_rgb565, 0x36314752u);
  }

  TEST(DrmSelect, Depth16GivesRgb565) {
    const DrmPixelFormat format = drm_format_for_depth(16);
    EXPECT_EQ(format.fourcc, drm_format_rgb565);
    EXPECT_EQ(format.bits_per_pixel, 16);
    EXPECT_EQ(format.panel.bytes_per_pixel, 2);
    EXPECT_EQ(format.panel.pack(255, 0, 0), 0xF800u);
    EXPECT_EQ(format.panel.pack(0, 255, 0), 0x07E0u);
    EXPECT_EQ(format.panel.pack(0, 0, 255), 0x001Fu);
  }

  TEST(DrmSelect, OtherDepthsGiveXrgb8888) {
    for(uint64_t depth : {0, 24, 30, 32}) {
      const DrmPixelFormat format = drm_format_for_depth(depth);
      EXPECT_EQ(format.fourcc, drm_format_xrgb8888) << "depth " << depth;
      EXPECT_EQ(format.bits_per_pixel, 32) << "depth " << depth;
      EXPECT_EQ(format.panel.pack(255, 0, 0), 0x00FF0000u) << "depth " << depth;
      EXPECT_EQ(format.panel.pack(0, 255, 0), 0x0000FF00u) << "depth " << depth;
      EXPECT_EQ(format.panel.pack(0, 0, 255), 0x000000FFu) << "depth " << depth;
    }
  }

  TEST(DrmSelect, FrameIntervalIsJustUnderTheRefreshPeriod) {
    // standard 640x480 at 60 Hz: 25.175 MHz, 800x525 total, 16683 µs per refresh
    DrmMode vga = mode(640, 480);
    vga.clock_khz = 25175;
    vga.htotal = 800;
    vga.vtotal = 525;
    EXPECT_EQ(drm_frame_interval_us(vga), 15014u);
  }

  TEST(DrmSelect, FrameIntervalDefaultsTo60HzWithoutTiming) {
    EXPECT_EQ(drm_frame_interval_us(mode(640, 480)), default_frame_interval_us);
  }
}
