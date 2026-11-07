/**
 * \file drm_select_test.cpp
 * \brief Unit tests for drm_select.hpp: connector, mode, CRTC and pixel-format choices, frame pacing, and plane scaling.
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

  TEST(DrmSelect, ScaledPlaneReadsTheScreenAt1xAndShowsItAtTheFit) {
    // 160x120 on a 480x272 panel: 2x at (80, 16)
    const PlaneRects rects = scaled_plane_rects(blit::Size(160, 120), fit_screen(blit::Size(480, 272), blit::Size(160, 120)));
    EXPECT_EQ(rects.src.x, 0u);
    EXPECT_EQ(rects.src.y, 0u);
    EXPECT_EQ(rects.src.w, 160u << 16);  // 16.16 fixed point
    EXPECT_EQ(rects.src.h, 120u << 16);
    EXPECT_EQ(rects.dst.x, 80u);
    EXPECT_EQ(rects.dst.y, 16u);
    EXPECT_EQ(rects.dst.w, 320u);
    EXPECT_EQ(rects.dst.h, 240u);
  }

  TEST(DrmSelect, FullPlaneShowsTheWholeBufferUnscaled) {
    const PlaneRects rects = full_plane_rects(blit::Size(640, 480));
    EXPECT_EQ(rects.src.w, 640u << 16);
    EXPECT_EQ(rects.src.h, 480u << 16);
    EXPECT_EQ(rects.dst.x, 0u);
    EXPECT_EQ(rects.dst.y, 0u);
    EXPECT_EQ(rects.dst.w, 640u);
    EXPECT_EQ(rects.dst.h, 480u);
  }

  TEST(DrmSelect, AutomaticScalerNeedsScalingToDoAndSharpPixels) {
    const ScreenFit at2x = {2, 0, 0};
    const ScreenFit at1x = {1, 80, 16};
    EXPECT_TRUE(want_plane_scaling(ScalerMode::automatic, at2x, true));
    EXPECT_FALSE(want_plane_scaling(ScalerMode::automatic, at2x, false));  // would blur pixel art
    EXPECT_FALSE(want_plane_scaling(ScalerMode::automatic, at1x, true));   // nothing to scale
  }

  TEST(DrmSelect, ScalerOnAndOffIgnoreScaleAndFilter) {
    for(const ScreenFit fit : {ScreenFit{1, 0, 0}, ScreenFit{3, 4, 4}}) {
      for(bool nearest : {false, true}) {
        EXPECT_TRUE(want_plane_scaling(ScalerMode::on, fit, nearest));
        EXPECT_FALSE(want_plane_scaling(ScalerMode::off, fit, nearest));
      }
    }
  }
}
