/**
 * \file panel.hpp
 * \brief Converting a game frame into a panel's pixel layout.
 *
 * Panels (and the Linux drivers in front of them) store pixels in different
 * layouts: 16 or 32 bits per pixel, the colour channels in any order, 5 to 8
 * bits per channel (RGB565, XRGB8888, BGR, RGB666, ...). PanelFormat
 * describes one layout and present_frame() converts a 32blit frame into it.
 *
 * The display backends only find the panel's memory and layout; the
 * conversion here is shared by all of them (fbdev now, DRM in Stage 1.2) and
 * is unit-tested without hardware (tests/panel_test.cpp).
 */
#pragma once

#include <cstdint>

#include "engine/api_private.hpp"

#include "screen_fit.hpp"

namespace blit_linux {

  /** \brief Where one colour channel lives inside a packed panel pixel. */
  struct PanelChannel {
    uint8_t offset = 0; ///< Bit position of the channel's lowest bit.
    uint8_t length = 8; ///< Bits in the channel, 1 to 8.
  };

  /** \brief Pixel layout of a panel. */
  struct PanelFormat {
    int bytes_per_pixel = 4; ///< 2 or 4.
    PanelChannel red, green, blue;
    uint32_t opaque_bits = 0; ///< Set in every pixel: all-ones alpha, for panels that read alpha.

    /** \brief Pack an 8-bit-per-channel colour into this layout, dropping each channel's low bits. */
    uint32_t pack(uint8_t r, uint8_t g, uint8_t b) const {
      return (uint32_t(r >> (8 - red.length)) << red.offset)
           | (uint32_t(g >> (8 - green.length)) << green.offset)
           | (uint32_t(b >> (8 - blue.length)) << blue.offset)
           | opaque_bits;
    }
  };

  /** \brief A panel's visible pixel memory. */
  struct PanelBuffer {
    uint8_t *pixels = nullptr; ///< Top-left visible pixel.
    blit::Size size;           ///< Visible width and height in pixels.
    uint32_t line_length = 0;  ///< Bytes from one row to the next; can be more than width x bytes per pixel.
  };

  /** \brief Fill every visible pixel with black. Row padding is left alone. */
  void clear_panel(const PanelBuffer &panel, const PanelFormat &format);

  /**
   * \brief Convert a game frame into the panel's layout, scaled and placed by `fit`.
   *
   * Writes only the area the scaled frame covers; the borders keep whatever
   * they had, so call clear_panel() once whenever the fit changes. Does
   * nothing if the scaled frame would not lie completely inside the panel.
   *
   * \param panel  Destination memory.
   * \param format Destination pixel layout.
   * \param frame  The game's screen: data, bounds, format (`RGB`, `RGB565` or `P`) and palette (for `P`).
   * \param fit    Scale and position, normally from fit_screen().
   */
  void present_frame(const PanelBuffer &panel, const PanelFormat &format, const blit::SurfaceInfo &frame, const ScreenFit &fit);
}
