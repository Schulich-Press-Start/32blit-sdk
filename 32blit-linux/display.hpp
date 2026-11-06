/**
 * \file display.hpp
 * \brief Screen output, through DRM/KMS or the framebuffer device.
 *
 * Two backends behind one set of functions:
 * - **DRM/KMS** (display_drm.cpp, the default): two buffers and page flips,
 *   so frames change only at the panel's vertical blank (no tearing) and
 *   presenting waits for the panel's refresh.
 * - **fbdev** (display_fb.cpp, the fallback): writes straight into
 *   `/dev/fb0`. No vsync, so frames can tear; paced by a 60 Hz timer.
 *
 * The game's screen is shown at the largest whole-number scale that fits
 * the panel, centred with black borders (screen_fit.hpp). Both backends use
 * the same conversion code (panel.cpp).
 *
 * Main loop use: render and present a frame only when display_ready() and
 * at least display_frame_interval_us() have passed since the last one; sleep
 * with display_wait(), which wakes early when a page flip completes.
 */
#pragma once

#include <cstdint>

#include "engine/api_private.hpp"

namespace blit_linux {

  /** \brief Which backend display_init() uses (`--display` option). */
  enum class DisplayBackend {
    /// DRM, or fbdev if DRM can't be used (the reason is printed).
    automatic,
    drm,
    fbdev,
  };

  /**
   * \brief Parse a `--display` value.
   * \param name `auto`, `drm` or `fbdev`.
   * \return false for anything else.
   */
  bool parse_display_backend(const char *name, DisplayBackend &backend);

  /**
   * \brief Open the display and clear it to black.
   * \return false if no backend could be used (reasons printed to stderr).
   */
  bool display_init(DisplayBackend backend);

  /** \brief Visible panel size in pixels; 0x0 before display_init() succeeds. */
  blit::Size display_size();

  /**
   * \brief Shortest time between presented frames.
   *
   * fbdev: 1/60 s, its only pacing. DRM: just under the panel's refresh
   * period; vblank does the pacing, this only caps the rate where vblank is
   * fake (virtual GPUs).
   */
  uint64_t display_frame_interval_us();

  /** \brief True when display_present() can take a frame without waiting (no page flip still pending). */
  bool display_ready();

  /**
   * \brief Convert one game frame to the panel's format and show it.
   *
   * DRM: draws into the hidden buffer and queues a page flip to it. If a flip
   * is still pending, waits for it first.
   *
   * \param frame The game's screen: data, size, pixel format (`RGB`,
   *              `RGB565` or `P`) and palette (for `P`). Its size must fit
   *              the panel (set_screen_mode_format() only accepts sizes that do).
   */
  void display_present(const blit::SurfaceInfo &frame);

  /** \brief Sleep up to `timeout_us`; returns early when a page flip completes or a signal arrives. */
  void display_wait(uint64_t timeout_us);

  /**
   * \brief Give the screen back: restore what was shown before (DRM) or clear it (fbdev), and close the device.
   *
   * Call before restoring the text console. Safe to call more than once.
   */
  void display_shutdown();
}
