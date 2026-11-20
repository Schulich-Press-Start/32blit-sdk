/**
 * \file display.cpp
 * \brief Chooses the display backend and forwards to it.
 */
#include "display.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "display_drm.hpp"
#include "display_fb.hpp"

namespace blit_linux {

  namespace {
    /// fbdev frame interval: about 60 Hz. fbdev has no vsync, so this timer is its only pacing.
    constexpr uint64_t fb_frame_interval_us = 16667;

    /// True when DRM is in use, false for fbdev.
    bool using_drm = false;
  }

  bool parse_display_backend(const char *name, DisplayBackend &backend) {
    if(strcmp(name, "auto") == 0)
      backend = DisplayBackend::automatic;
    else if(strcmp(name, "drm") == 0)
      backend = DisplayBackend::drm;
    else if(strcmp(name, "fbdev") == 0)
      backend = DisplayBackend::fbdev;
    else
      return false;

    return true;
  }

  bool display_init(DisplayBackend backend) {
    if(backend != DisplayBackend::fbdev) {
      using_drm = drm_init();
      if(using_drm)
        return true;
      if(backend == DisplayBackend::drm)
        return false;

      fprintf(stderr, "32blit-linux: DRM unavailable, using the framebuffer device (no vsync)\n");
    }

    return fb_init();
  }

  blit::Size display_size() {
    return using_drm ? drm_size() : fb_size();
  }

  uint64_t display_frame_interval_us() {
    return using_drm ? drm_frame_interval_us() : fb_frame_interval_us;
  }

  bool display_ready() {
    return using_drm ? drm_ready() : true;
  }

  void display_present(const blit::SurfaceInfo &frame) {
    if(using_drm)
      drm_present(frame);
    else
      fb_present(frame);
  }

  void display_wait(uint64_t timeout_us) {
    if(using_drm) {
      drm_wait(timeout_us);
      return;
    }

    timespec timeout;
    timeout.tv_sec = time_t(timeout_us / 1000000);
    timeout.tv_nsec = long(timeout_us % 1000000) * 1000;
    nanosleep(&timeout, nullptr);
  }

  void display_shutdown() {
    if(using_drm)
      drm_shutdown();
    else
      fb_shutdown();
  }
}
