/**
 * \file display_drm.hpp
 * \brief DRM/KMS display backend: tear-free output with page flips.
 *
 * Uses the kernel's mode-setting API through libdrm: the panel scans out one
 * of two buffers while the game's next frame is drawn into the other, then
 * a page flip swaps them at the panel's vertical blank. The buffers are
 * "dumb" buffers (plain memory, no GPU), which every KMS driver supports.
 *
 * The game owns the display (is DRM master) while it runs; on exit it puts
 * back whatever was shown before (the text console, normally) and closes
 * the device, which hands the display to the next program.
 */
#pragma once

#include <cstdint>

#include "engine/api_private.hpp"

namespace blit_linux {

  /**
   * \brief Find a card with a connected output, set its preferred mode and show a black screen.
   *
   * Tries `/dev/dri/card0` to `card7`. Prints why each card can't be used.
   *
   * \return false if no card could be used.
   */
  bool drm_init();

  /** \brief Mode size in pixels; 0x0 before drm_init() succeeds. */
  blit::Size drm_size();

  /** \brief Shortest time between frames for the mode in use (see display_frame_interval_us()). */
  uint64_t drm_frame_interval_us();

  /** \brief True when no page flip is pending. */
  bool drm_ready();

  /** \brief Draw the frame into the hidden buffer and flip to it (see display_present()). */
  void drm_present(const blit::SurfaceInfo &frame);

  /** \brief Sleep up to `timeout_us`; returns early when a page flip completes or a signal arrives. */
  void drm_wait(uint64_t timeout_us);

  /** \brief Restore the previous display configuration, free the buffers and close the device. Safe to call more than once. */
  void drm_shutdown();
}
