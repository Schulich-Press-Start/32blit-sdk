/**
 * \file display_fb.cpp
 * \brief fbdev implementation of the display functions.
 *
 * How it works:
 * 1. `FBIOGET_VSCREENINFO` / `FBIOGET_FSCREENINFO` tell us the resolution,
 *    bytes per row and where each colour channel sits inside a pixel.
 * 2. `mmap()` gives us a pointer to the screen memory.
 * 3. Every frame, present_frame() (panel.cpp) converts the game's screen
 *    into that memory at the scale and position from fit_screen().
 */
#include "display_fb.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "panel.hpp"

namespace blit_linux {

  namespace {
    /// Framebuffer device. Boards with several displays may need this configurable later.
    constexpr const char *device_path = "/dev/fb0";

    int fb_fd = -1;
    uint8_t *fb_map = nullptr; ///< Start of the mapped device memory.
    size_t fb_map_size = 0;

    PanelBuffer panel;         ///< Visible screen inside fb_map; `pixels` is null until display_init() succeeds.
    PanelFormat panel_format;

    /// Size of the last presented game screen; when it changes, the scale or position may too, so the borders are re-cleared.
    blit::Size last_screen_size;

    /**
     * \brief Describe a channel from the driver's bitfield, or fail if we can't pack it.
     * \return false for channels wider than 8 bits (e.g. 10-bit panels).
     */
    bool make_channel(const fb_bitfield &field, PanelChannel &out) {
      if(field.length == 0 || field.length > 8)
        return false;

      out.offset = uint8_t(field.offset);
      out.length = uint8_t(field.length);
      return true;
    }
  }

  bool display_init() {
    fb_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if(fb_fd < 0) {
      fprintf(stderr, "32blit-linux: cannot open %s: %s\n", device_path, strerror(errno));
      return false;
    }

    fb_var_screeninfo var;
    fb_fix_screeninfo fix;
    if(ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0) {
      fprintf(stderr, "32blit-linux: cannot read %s mode: %s\n", device_path, strerror(errno));
      display_shutdown();
      return false;
    }

    // only "one pixel = one packed RGB value" framebuffers; no palettes or bit planes
    if(fix.type != FB_TYPE_PACKED_PIXELS || fix.visual != FB_VISUAL_TRUECOLOR
    || (var.bits_per_pixel != 16 && var.bits_per_pixel != 32)
    || !make_channel(var.red, panel_format.red) || !make_channel(var.green, panel_format.green) || !make_channel(var.blue, panel_format.blue)) {
      fprintf(stderr, "32blit-linux: unsupported framebuffer format (%u bpp, type %u, visual %u)\n",
              var.bits_per_pixel, fix.type, fix.visual);
      display_shutdown();
      return false;
    }

    panel_format.bytes_per_pixel = int(var.bits_per_pixel / 8);

    // some drivers read the alpha bits; make every pixel fully opaque
    panel_format.opaque_bits = var.transp.length ? ((1u << var.transp.length) - 1) << var.transp.offset : 0;

    // smem_len is the whole buffer (may hold several screens); fall back for drivers that leave it 0
    fb_map_size = fix.smem_len ? fix.smem_len : size_t(fix.line_length) * var.yres_virtual;
    void *map = mmap(nullptr, fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if(map == MAP_FAILED) {
      fprintf(stderr, "32blit-linux: cannot map %s: %s\n", device_path, strerror(errno));
      fb_map_size = 0;
      display_shutdown();
      return false;
    }

    fb_map = static_cast<uint8_t *>(map);

    // the visible screen can start partway into the buffer if something panned it
    panel.pixels = fb_map + size_t(var.yoffset) * fix.line_length + size_t(var.xoffset) * panel_format.bytes_per_pixel;
    panel.size = blit::Size(int(var.xres), int(var.yres));
    panel.line_length = fix.line_length;

    // wake the panel if the console blanked it; not all drivers support this
    ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);

    clear_panel(panel, panel_format);

    printf("32blit-linux: %s %dx%d, %d bpp\n", device_path, panel.size.w, panel.size.h, var.bits_per_pixel);
    return true;
  }

  blit::Size display_size() {
    return panel.pixels ? panel.size : blit::Size();
  }

  void display_present(const blit::SurfaceInfo &frame) {
    if(!panel.pixels)
      return;

    // a size change can leave old pixels outside the new screen
    if(frame.bounds != last_screen_size) {
      clear_panel(panel, panel_format);
      last_screen_size = frame.bounds;
    }

    present_frame(panel, panel_format, frame, fit_screen(panel.size, frame.bounds));
  }

  void display_shutdown() {
    if(panel.pixels)
      clear_panel(panel, panel_format);

    if(fb_map)
      munmap(fb_map, fb_map_size);

    if(fb_fd >= 0)
      close(fb_fd);

    fb_map = nullptr;
    fb_map_size = 0;
    fb_fd = -1;
    panel = PanelBuffer();
    last_screen_size = blit::Size();
  }
}
