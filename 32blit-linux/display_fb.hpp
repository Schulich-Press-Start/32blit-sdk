/**
 * \file display_fb.hpp
 * \brief fbdev display backend: screen output through `/dev/fb0`.
 *
 * fbdev is the simplest way to get pixels on a Linux display: the kernel
 * exposes the screen as a block of memory that we map and write into. It
 * has no vsync or page flipping, so frames can tear. It is the fallback
 * when DRM/KMS (display_drm.hpp) can't be used; display.cpp chooses.
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
  bool fb_init();

  /** \brief Visible panel size in pixels; 0x0 before fb_init() succeeds. */
  blit::Size fb_size();

  /** \brief Convert one game frame to the panel's format and copy it on screen (see display_present()). */
  void fb_present(const blit::SurfaceInfo &frame);

  /** \brief Clear the screen, unmap and close the device. Safe to call more than once. */
  void fb_shutdown();
}
