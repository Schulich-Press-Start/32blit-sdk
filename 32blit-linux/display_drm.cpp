/**
 * \file display_drm.cpp
 * \brief DRM/KMS display backend.
 *
 * Setup (drm_init):
 * 1. Open a card and check it supports dumb buffers.
 * 2. Copy its connectors, modes and CRTCs into the drm_select.hpp structs and
 *    let those functions choose.
 * 3. Save the CRTC's current configuration (normally the text console's), to
 *    put back on exit.
 * 4. Create two dumb buffers in the driver's preferred format, map them,
 *    clear them, and show the first with drmModeSetCrtc().
 *
 * Per frame (drm_present): convert the game frame into the hidden buffer
 * with present_frame(), then drmModePageFlip() with an event. The flip
 * completes at the next vertical blank and is reported on the device's file
 * descriptor, which drm_wait() and the flip wait poll.
 */
#include "display_drm.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_select.hpp"
#include "panel.hpp"
#include "screen_fit.hpp"

namespace blit_linux {

  namespace {
    /// Cards tried, `/dev/dri/card0` up to this number minus one.
    constexpr int max_cards = 8;

    /// Longest wait for a page flip while running; if it never completes, the driver's flips are treated as broken.
    constexpr int flip_timeout_ms = 1000;
    /// Longest wait for a pending flip at exit.
    constexpr int exit_flip_timeout_ms = 100;

    /** \brief One of the two scan-out buffers. */
    struct Buffer {
      /// Dumb buffer handle.
      uint32_t handle = 0;
      /// Framebuffer object the CRTC scans out.
      uint32_t fb_id = 0;
      uint8_t *map = nullptr;
      size_t map_size = 0;
      PanelBuffer panel;
      /// Game screen size last drawn here; when it changes, the borders are cleared.
      blit::Size last_screen_size;
    };

    int drm_fd = -1;
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    drmModeModeInfo mode;
    blit::Size mode_size;
    uint64_t frame_interval_us = default_frame_interval_us;
    DrmPixelFormat pixel_format;

    /// What the CRTC showed before (normally the console); put back on exit.
    drmModeCrtc *saved_crtc = nullptr;
    /// True once our mode is set, so exit knows there is something to undo.
    bool mode_set = false;

    Buffer buffers[2];
    /// The buffer not on screen; the next frame is drawn there.
    int back = 1;
    bool flip_pending = false;
    /// Cleared if the driver rejects or never completes a flip; frames are then shown with drmModeSetCrtc(), without vsync.
    bool use_flips = true;

    void on_page_flip(int, unsigned, unsigned, unsigned, void *) {
      flip_pending = false;
    }

    /** \brief Read pending events from the device (page-flip completion). */
    void handle_events() {
      drmEventContext context = {};
      context.version = 2;
      context.page_flip_handler = on_page_flip;
      drmHandleEvent(drm_fd, &context);
    }

    /** \brief Milliseconds since `start` on the monotonic clock. */
    int64_t ms_since(const timespec &start) {
      timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      return int64_t(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    }

    /** \brief Wait until no flip is pending, at most `timeout_ms`. \return false on timeout or error. */
    bool wait_for_flip(int timeout_ms) {
      timespec start;
      clock_gettime(CLOCK_MONOTONIC, &start);

      while(flip_pending) {
        const int64_t left_ms = timeout_ms - ms_since(start);
        if(left_ms <= 0)
          return false;

        pollfd pfd = {drm_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, int(left_ms));
        if(ret > 0)
          handle_events();
        else if(ret < 0 && errno != EINTR)
          return false;
      }

      return true;
    }

    /** \brief Print why `path` can't be used. */
    void report(const char *path, const char *reason) {
      fprintf(stderr, "32blit-linux: DRM %s: %s\n", path, reason);
    }

    /** \brief Create, register and map one scan-out buffer of the mode's size, and clear it. */
    bool create_buffer(const char *path, Buffer &buffer) {
      uint32_t pitch = 0;
      uint64_t size = 0;
      if(drmModeCreateDumbBuffer(drm_fd, uint32_t(mode_size.w), uint32_t(mode_size.h), uint32_t(pixel_format.bits_per_pixel), 0,
                                 &buffer.handle, &pitch, &size) != 0) {
        report(path, "cannot create a buffer");
        return false;
      }

      const uint32_t handles[4] = {buffer.handle};
      const uint32_t pitches[4] = {pitch};
      const uint32_t offsets[4] = {};
      if(drmModeAddFB2(drm_fd, uint32_t(mode_size.w), uint32_t(mode_size.h), pixel_format.fourcc, handles, pitches, offsets, &buffer.fb_id, 0) != 0) {
        report(path, "cannot register a buffer in this pixel format");
        return false;
      }

      uint64_t offset = 0;
      if(drmModeMapDumbBuffer(drm_fd, buffer.handle, &offset) != 0) {
        report(path, "cannot map a buffer");
        return false;
      }

      void *map = mmap(nullptr, size_t(size), PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, off_t(offset));
      if(map == MAP_FAILED) {
        report(path, "cannot map a buffer");
        return false;
      }

      buffer.map = static_cast<uint8_t *>(map);
      buffer.map_size = size_t(size);
      buffer.panel.pixels = buffer.map;
      buffer.panel.size = mode_size;
      buffer.panel.line_length = pitch;

      clear_panel(buffer.panel, pixel_format.panel);
      return true;
    }

    void destroy_buffer(Buffer &buffer) {
      if(buffer.map)
        munmap(buffer.map, buffer.map_size);
      if(buffer.fb_id)
        drmModeRmFB(drm_fd, buffer.fb_id);
      if(buffer.handle)
        drmModeDestroyDumbBuffer(drm_fd, buffer.handle);

      buffer = Buffer();
    }

    /** \brief Copy one connector's state into a DrmConnector. */
    DrmConnector describe_connector(const drmModeConnector &connector) {
      DrmConnector info;
      info.connected = connector.connection == DRM_MODE_CONNECTED;

      for(int i = 0; i < connector.count_modes; i++) {
        const drmModeModeInfo &m = connector.modes[i];
        info.modes.push_back({m.hdisplay, m.vdisplay, m.clock, m.htotal, m.vtotal, (m.type & DRM_MODE_TYPE_PREFERRED) != 0});
      }

      if(connector.encoder_id) {
        if(drmModeEncoder *encoder = drmModeGetEncoder(drm_fd, connector.encoder_id)) {
          info.current_crtc_id = encoder->crtc_id;
          drmModeFreeEncoder(encoder);
        }
      }

      for(int i = 0; i < connector.count_encoders; i++) {
        if(drmModeEncoder *encoder = drmModeGetEncoder(drm_fd, connector.encoders[i])) {
          info.possible_crtcs |= encoder->possible_crtcs;
          drmModeFreeEncoder(encoder);
        }
      }

      return info;
    }

    /**
     * \brief Choose the connector, mode and CRTC on the open card.
     * \return false if the card has no connected output it can drive.
     */
    bool choose_output(const char *path) {
      drmModeRes *resources = drmModeGetResources(drm_fd);
      if(!resources) {
        report(path, "not a display device (no mode-setting resources)");
        return false;
      }

      std::vector<drmModeConnector *> connectors;
      std::vector<DrmConnector> infos;
      for(int i = 0; i < resources->count_connectors; i++) {
        if(drmModeConnector *connector = drmModeGetConnector(drm_fd, resources->connectors[i])) {
          connectors.push_back(connector);
          infos.push_back(describe_connector(*connector));
        }
      }

      const std::vector<uint32_t> crtc_ids(resources->crtcs, resources->crtcs + resources->count_crtcs);

      bool ok = false;
      const int c = choose_connector(infos);
      if(c < 0) {
        report(path, "no connected display");
      } else {
        const int m = choose_mode(infos[c].modes);
        const int k = choose_crtc(infos[c], crtc_ids);
        if(k < 0) {
          report(path, "no display controller (CRTC) can drive the connected display");
        } else {
          connector_id = connectors[c]->connector_id;
          crtc_id = crtc_ids[k];
          mode = connectors[c]->modes[m];
          mode_size = blit::Size(mode.hdisplay, mode.vdisplay);
          frame_interval_us = drm_frame_interval_us(infos[c].modes[m]);
          ok = true;
        }
      }

      for(auto connector : connectors)
        drmModeFreeConnector(connector);
      drmModeFreeResources(resources);

      return ok;
    }

    /** \brief Set up `path` completely. On failure the caller cleans up with drm_shutdown(). */
    bool try_card(const char *path) {
      drm_fd = open(path, O_RDWR | O_CLOEXEC);
      if(drm_fd < 0) {
        report(path, strerror(errno));
        return false;
      }

      uint64_t has_dumb = 0;
      if(drmGetCap(drm_fd, DRM_CAP_DUMB_BUFFER, &has_dumb) != 0 || !has_dumb) {
        report(path, "no dumb-buffer support");
        return false;
      }

      if(!choose_output(path))
        return false;

      uint64_t depth = 0;
      drmGetCap(drm_fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &depth);
      pixel_format = drm_format_for_depth(depth);

      saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);

      for(auto &buffer : buffers) {
        if(!create_buffer(path, buffer))
          return false;
      }

      if(drmModeSetCrtc(drm_fd, crtc_id, buffers[0].fb_id, 0, 0, &connector_id, 1, &mode) != 0) {
        const bool denied = errno == EACCES || errno == EPERM;
        report(path, denied ? "cannot set the mode: another program controls the display"
                            : "cannot set the mode");
        return false;
      }

      mode_set = true;
      back = 1;
      flip_pending = false;
      use_flips = true;

      const char *driver = "unknown";
      drmVersion *version = drmGetVersion(drm_fd);
      if(version && version->name)
        driver = version->name;

      printf("32blit-linux: DRM %s (%s) %dx%d @ %.1f Hz, %d bpp\n", path, driver, mode_size.w, mode_size.h,
             double(mode.clock) * 1000.0 / (double(mode.htotal) * double(mode.vtotal)), pixel_format.bits_per_pixel);

      if(version)
        drmFreeVersion(version);

      return true;
    }
  }

  bool drm_init() {
    bool any_card = false;

    for(int i = 0; i < max_cards; i++) {
      char path[32];
      snprintf(path, sizeof(path), "/dev/dri/card%d", i);
      if(access(path, F_OK) != 0)
        continue;

      any_card = true;
      if(try_card(path))
        return true;

      drm_shutdown();
    }

    if(!any_card)
      fprintf(stderr, "32blit-linux: DRM: no /dev/dri/card* devices\n");

    return false;
  }

  blit::Size drm_size() {
    return mode_set ? mode_size : blit::Size();
  }

  uint64_t drm_frame_interval_us() {
    return frame_interval_us;
  }

  bool drm_ready() {
    return !flip_pending;
  }

  void drm_present(const blit::SurfaceInfo &frame) {
    if(!mode_set)
      return;

    if(flip_pending && !wait_for_flip(flip_timeout_ms)) {
      fprintf(stderr, "32blit-linux: DRM page flip never completed; showing frames without vsync\n");
      flip_pending = false;
      use_flips = false;
    }

    Buffer &buffer = buffers[back];

    // a size change can leave old pixels outside the new screen
    if(frame.bounds != buffer.last_screen_size) {
      clear_panel(buffer.panel, pixel_format.panel);
      buffer.last_screen_size = frame.bounds;
    }

    present_frame(buffer.panel, pixel_format.panel, frame, fit_screen(mode_size, frame.bounds));

    if(use_flips) {
      if(drmModePageFlip(drm_fd, crtc_id, buffer.fb_id, DRM_MODE_PAGE_FLIP_EVENT, nullptr) == 0) {
        flip_pending = true;
      } else {
        fprintf(stderr, "32blit-linux: DRM page flip failed (%s); showing frames without vsync\n", strerror(errno));
        use_flips = false;
      }
    }

    if(!use_flips)
      drmModeSetCrtc(drm_fd, crtc_id, buffer.fb_id, 0, 0, &connector_id, 1, &mode);

    back ^= 1;
  }

  void drm_wait(uint64_t timeout_us) {
    timespec timeout;
    timeout.tv_sec = time_t(timeout_us / 1000000);
    timeout.tv_nsec = long(timeout_us % 1000000) * 1000;

    if(!flip_pending) {
      nanosleep(&timeout, nullptr);
      return;
    }

    pollfd pfd = {drm_fd, POLLIN, 0};
    if(ppoll(&pfd, 1, &timeout, nullptr) > 0)
      handle_events();
  }

  void drm_shutdown() {
    if(drm_fd < 0)
      return;

    // a buffer the display may still flip to can't be removed
    if(flip_pending)
      wait_for_flip(exit_flip_timeout_ms);
    flip_pending = false;

    // put back what was shown before (normally the console). If nothing was,
    // removing our buffers below switches the display off.
    if(mode_set && saved_crtc && saved_crtc->mode_valid)
      drmModeSetCrtc(drm_fd, saved_crtc->crtc_id, saved_crtc->buffer_id, saved_crtc->x, saved_crtc->y, &connector_id, 1, &saved_crtc->mode);

    if(saved_crtc)
      drmModeFreeCrtc(saved_crtc);
    saved_crtc = nullptr;

    for(auto &buffer : buffers)
      destroy_buffer(buffer);

    // closing the device also gives up control of the display (DRM master)
    close(drm_fd);
    drm_fd = -1;
    mode_set = false;
    mode_size = blit::Size();
  }
}
