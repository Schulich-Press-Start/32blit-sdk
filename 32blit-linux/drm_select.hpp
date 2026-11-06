/**
 * \file drm_select.hpp
 * \brief Choices the DRM backend makes from what the driver reports.
 *
 * Which connector (output) to use, which of its modes, which CRTC (the
 * display-engine block that scans a buffer out to it), which pixel format,
 * and how often to present frames. display_drm.cpp copies the driver's
 * answers into the small structs below and asks these functions; keeping
 * them free of libdrm types means they are unit-tested without hardware
 * (tests/drm_select_test.cpp).
 */
#pragma once

#include <cstdint>
#include <vector>

#include "panel.hpp"

namespace blit_linux {

  /** \brief One display mode (resolution and timing) a connector offers. */
  struct DrmMode {
    int width = 0;
    int height = 0;
    /// Pixel clock in kHz.
    uint32_t clock_khz = 0;
    /// Pixels per line and lines per frame, including blanking.
    int htotal = 0;
    int vtotal = 0;
    /// The driver marks the panel's native mode as preferred.
    bool preferred = false;
  };

  /** \brief One connector (an output: panel, HDMI, virtual screen). */
  struct DrmConnector {
    bool connected = false;
    std::vector<DrmMode> modes;
    /// CRTC currently driving this connector (e.g. the console's), or 0.
    uint32_t current_crtc_id = 0;
    /// Bit i set: the CRTC at index i of the driver's CRTC list can drive this connector.
    uint32_t possible_crtcs = 0;
  };

  /** \brief A DRM pixel format and the matching panel layout for present_frame(). */
  struct DrmPixelFormat {
    /// DRM fourcc code (`DRM_FORMAT_*`).
    uint32_t fourcc;
    int bits_per_pixel;
    PanelFormat panel;
  };

  /// DRM fourcc code from its four characters (as `fourcc_code()` in drm_fourcc.h).
  constexpr uint32_t drm_fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
  }

  /// `DRM_FORMAT_XRGB8888`: 32 bits, x:R:G:B 8:8:8:8, little-endian.
  constexpr uint32_t drm_format_xrgb8888 = drm_fourcc('X', 'R', '2', '4');
  /// `DRM_FORMAT_RGB565`: 16 bits, R:G:B 5:6:5, little-endian.
  constexpr uint32_t drm_format_rgb565 = drm_fourcc('R', 'G', '1', '6');

  /** \return Index of the first connected connector that has at least one mode, or -1. */
  inline int choose_connector(const std::vector<DrmConnector> &connectors) {
    for(size_t i = 0; i < connectors.size(); i++) {
      if(connectors[i].connected && !connectors[i].modes.empty())
        return int(i);
    }
    return -1;
  }

  /** \return Index of the preferred (native) mode, else the first one (drivers list the best first), or -1 if there are none. */
  inline int choose_mode(const std::vector<DrmMode> &modes) {
    for(size_t i = 0; i < modes.size(); i++) {
      if(modes[i].preferred)
        return int(i);
    }
    return modes.empty() ? -1 : 0;
  }

  /**
   * \brief Pick the CRTC to drive `connector`.
   *
   * The one already driving it if there is one (no reconfiguration needed),
   * otherwise the first CRTC it can use.
   *
   * \param crtc_ids The driver's CRTC list, in order (indices match `possible_crtcs` bits).
   * \return Index into `crtc_ids`, or -1.
   */
  inline int choose_crtc(const DrmConnector &connector, const std::vector<uint32_t> &crtc_ids) {
    for(size_t i = 0; i < crtc_ids.size() && i < 32; i++) {
      if(connector.current_crtc_id && crtc_ids[i] == connector.current_crtc_id)
        return int(i);
    }
    for(size_t i = 0; i < crtc_ids.size() && i < 32; i++) {
      if(connector.possible_crtcs & (1u << i))
        return int(i);
    }
    return -1;
  }

  /**
   * \brief Pixel format for the driver's preferred depth (`DRM_CAP_DUMB_PREFERRED_DEPTH`).
   *
   * 16 gives RGB565 (half the memory traffic, for panels wired for 16 bits);
   * anything else, including 0 (driver doesn't say), gives XRGB8888, which every
   * KMS driver supports.
   */
  inline DrmPixelFormat drm_format_for_depth(uint64_t preferred_depth) {
    DrmPixelFormat format;
    if(preferred_depth == 16) {
      format.fourcc = drm_format_rgb565;
      format.bits_per_pixel = 16;
      format.panel.bytes_per_pixel = 2;
      format.panel.red = {11, 5};
      format.panel.green = {5, 6};
      format.panel.blue = {0, 5};
    } else {
      format.fourcc = drm_format_xrgb8888;
      format.bits_per_pixel = 32;
      format.panel.bytes_per_pixel = 4;
      format.panel.red = {16, 8};
      format.panel.green = {8, 8};
      format.panel.blue = {0, 8};
    }
    return format;
  }

  /// Frame interval when a mode doesn't give its timing: 60 Hz.
  constexpr uint64_t default_frame_interval_us = 16667;

  /**
   * \brief Shortest time between presented frames for a mode.
   *
   * 90% of the refresh period. On real hardware a page flip completes only at
   * the next vertical blank, so vblank paces frames and this never holds one
   * back, even on a panel slightly faster than its nominal rate. Virtual GPUs
   * complete flips at once; there this caps the frame rate near the refresh rate.
   */
  inline uint64_t drm_frame_interval_us(const DrmMode &mode) {
    if(mode.clock_khz == 0 || mode.htotal <= 0 || mode.vtotal <= 0)
      return default_frame_interval_us;

    // period in µs = htotal * vtotal / (clock_khz * 1000) * 1e6
    const uint64_t period_us = uint64_t(mode.htotal) * uint64_t(mode.vtotal) * 1000 / mode.clock_khz;
    return period_us * 9 / 10;
  }
}
