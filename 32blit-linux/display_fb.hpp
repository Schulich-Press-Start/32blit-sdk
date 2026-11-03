/**
 * \file display_fb.hpp
 * \brief Screen output through the Linux framebuffer device (`/dev/fb0`).
 *
 * fbdev is the simplest way to get pixels on a Linux display: the kernel
 * exposes the screen as a block of memory that we map and write into.
 * It has no vsync or page flipping, so frames can tear. Stage 1.2 adds a
 * DRM/KMS backend behind the same functions and keeps this one as the
 * fallback.
 *
 * The game's screen is shown at the largest whole-number scale that fits
 * the panel, centred with black borders (screen_fit.hpp). The pixel
 * conversion itself lives in panel.cpp.
 */
#pragma once

#include "engine/api_private.hpp"

namespace blit_linux {

  /**
   * \brief Open and map `/dev/fb0`, and clear it to black.
   *
   * Supports 16- and 32-bit-per-pixel true-colour framebuffers in any
   * channel order (read from the driver). Prints the reason on failure.
   *
   * \return false if the framebuffer can't be used.
   */
  bool display_init();

  /** \brief Visible panel size in pixels; 0x0 before display_init() succeeds. */
  blit::Size display_size();

  /**
   * \brief Convert one game frame to the panel's format and copy it on screen.
   *
   * \param frame The game's screen: data, size, pixel format (`RGB`,
   *              `RGB565` or `P`) and palette (for `P`). Its size must fit
   *              the panel (set_screen_mode_format() only accepts sizes that do).
   */
  void display_present(const blit::SurfaceInfo &frame);

  /** \brief Clear the screen, unmap and close the device. Safe to call more than once. */
  void display_shutdown();
}
