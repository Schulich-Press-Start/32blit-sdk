/**
 * \file display_fb.cpp
 * \brief fbdev implementation of the display functions.
 *
 * How it works:
 * 1. `FBIOGET_VSCREENINFO` / `FBIOGET_FSCREENINFO` tell us the resolution,
 *    bytes per row and where each colour channel sits inside a pixel.
 * 2. `mmap()` gives us a pointer to the screen memory.
 * 3. Every frame, each game pixel is unpacked to 8-bit R, G, B and packed
 *    again in the panel's layout, `scale` times horizontally; each output
 *    row is then copied `scale - 1` more times for vertical scaling.
 *
 * The conversion is plain C++. NEON comes later, once frame times are
 * measured on the board (Stage 1.2 step 10).
 */
#include "display_fb.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace blit_linux {

  namespace {
    /// Framebuffer device. Boards with several displays may need this configurable later.
    constexpr const char *device_path = "/dev/fb0";

    /** \brief Where one colour channel lives inside a packed panel pixel. */
    struct Channel {
      uint8_t drop_bits; ///< Low bits to drop from an 8-bit value (8 - channel length).
      uint8_t offset;    ///< Bit position of the channel inside the pixel.
    };

    int fb_fd = -1;
    uint8_t *fb_map = nullptr;     ///< Start of the mapped device memory.
    size_t fb_map_size = 0;
    uint8_t *fb_visible = nullptr; ///< Top-left pixel of the visible screen inside fb_map.

    int panel_width = 0, panel_height = 0;
    uint32_t line_length = 0;      ///< Bytes per row, can be more than width * bytes_per_pixel.
    int bytes_per_pixel = 0;       ///< 2 or 4.

    Channel red, green, blue;
    uint32_t opaque_bits = 0;      ///< Alpha bits set to all ones, for panels that read alpha.

    /// Size of the last presented frame on screen; when it changes, the borders are re-cleared.
    int last_out_width = 0, last_out_height = 0;

    /** \brief Pack an 8-bit-per-channel colour into the panel's pixel layout. */
    inline uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
      return (uint32_t(r >> red.drop_bits) << red.offset)
           | (uint32_t(g >> green.drop_bits) << green.offset)
           | (uint32_t(b >> blue.drop_bits) << blue.offset)
           | opaque_bits;
    }

    /**
     * \brief Describe a channel from the driver's bitfield, or fail if we can't pack it.
     * \return false for channels wider than 8 bits (e.g. 10-bit panels).
     */
    bool make_channel(const fb_bitfield &field, Channel &out) {
      if(field.length == 0 || field.length > 8)
        return false;

      out.drop_bits = uint8_t(8 - field.length);
      out.offset = uint8_t(field.offset);
      return true;
    }

    /** \brief Fill the whole visible screen with black. */
    void clear_screen() {
      const uint32_t black = pack(0, 0, 0);

      for(int y = 0; y < panel_height; y++) {
        auto row = fb_visible + size_t(y) * line_length;

        if(bytes_per_pixel == 2)
          std::fill_n(reinterpret_cast<uint16_t *>(row), panel_width, uint16_t(black));
        else
          std::fill_n(reinterpret_cast<uint32_t *>(row), panel_width, black);
      }
    }

    /**
     * \brief Convert one row of output pixels.
     *
     * \tparam Out       `uint16_t` or `uint32_t`, matching the panel's pixel size.
     * \tparam ReadPixel Callable taking a source column and returning a packed panel pixel.
     * \param dst     Start of the destination row (panel column 0).
     * \param dst_x   Panel column where output column 0 lands (can be negative when cropping).
     * \param first_x First visible output column.
     * \param last_x  One past the last visible output column.
     * \param scale   Output columns per source column.
     */
    template<typename Out, typename ReadPixel>
    void convert_row(uint8_t *dst, int dst_x, int first_x, int last_x, int scale, ReadPixel read_pixel) {
      auto out = reinterpret_cast<Out *>(dst) + dst_x;

      for(int x = first_x; x < last_x; x++)
        out[x] = Out(read_pixel(x / scale));
    }

    /** \brief Dispatch convert_row() on the panel's pixel size. */
    template<typename ReadPixel>
    void convert_row_any(uint8_t *dst, int dst_x, int first_x, int last_x, int scale, ReadPixel read_pixel) {
      if(bytes_per_pixel == 2)
        convert_row<uint16_t>(dst, dst_x, first_x, last_x, scale, read_pixel);
      else
        convert_row<uint32_t>(dst, dst_x, first_x, last_x, scale, read_pixel);
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
    || !make_channel(var.red, red) || !make_channel(var.green, green) || !make_channel(var.blue, blue)) {
      fprintf(stderr, "32blit-linux: unsupported framebuffer format (%u bpp, type %u, visual %u)\n",
              var.bits_per_pixel, fix.type, fix.visual);
      display_shutdown();
      return false;
    }

    panel_width = int(var.xres);
    panel_height = int(var.yres);
    line_length = fix.line_length;
    bytes_per_pixel = int(var.bits_per_pixel / 8);

    // some drivers read the alpha bits; make every pixel fully opaque
    opaque_bits = var.transp.length ? ((1u << var.transp.length) - 1) << var.transp.offset : 0;

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
    fb_visible = fb_map + size_t(var.yoffset) * line_length + size_t(var.xoffset) * bytes_per_pixel;

    // wake the panel if the console blanked it; not all drivers support this
    ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);

    clear_screen();

    printf("32blit-linux: %s %dx%d, %d bpp\n", device_path, panel_width, panel_height, var.bits_per_pixel);
    return true;
  }

  void display_present(const blit::SurfaceInfo &frame, int scale) {
    if(!fb_visible)
      return;

    const int out_width = frame.bounds.w * scale;
    const int out_height = frame.bounds.h * scale;

    // a mode change can leave old pixels outside the new frame
    if(out_width != last_out_width || out_height != last_out_height) {
      clear_screen();
      last_out_width = out_width;
      last_out_height = out_height;
    }

    // top-left of the frame on the panel; negative if the frame is bigger than the panel
    const int dst_x = (panel_width - out_width) / 2;
    const int dst_y = (panel_height - out_height) / 2;

    // visible part of the frame, in output coordinates
    const int first_x = std::max(0, -dst_x), last_x = std::min(out_width, panel_width - dst_x);
    const int first_y = std::max(0, -dst_y), last_y = std::min(out_height, panel_height - dst_y);

    const int src_stride = frame.bounds.w * blit::pixel_format_stride[int(frame.format)];
    const size_t row_bytes = size_t(last_x - first_x) * bytes_per_pixel;

    // palette entries packed once per frame instead of once per pixel
    uint32_t palette_lut[256];
    if(frame.format == blit::PixelFormat::P) {
      for(int i = 0; i < 256; i++)
        palette_lut[i] = pack(frame.palette[i].r, frame.palette[i].g, frame.palette[i].b);
    }

    for(int y = first_y; y < last_y; y++) {
      uint8_t *dst = fb_visible + size_t(dst_y + y) * line_length;

      // vertical scaling: repeat the previous output row instead of converting again
      if(y % scale != 0 && y != first_y) {
        auto first_pixel = size_t(dst_x + first_x) * bytes_per_pixel;
        memcpy(dst + first_pixel, dst - line_length + first_pixel, row_bytes);
        continue;
      }

      const uint8_t *src = frame.data + (y / scale) * src_stride;

      switch(frame.format) {
        case blit::PixelFormat::RGB:
          convert_row_any(dst, dst_x, first_x, last_x, scale, [src](int sx) {
            auto p = src + sx * 3;
            return pack(p[0], p[1], p[2]);
          });
          break;

        case blit::PixelFormat::RGB565:
          // 32blit's RGB565 keeps red in the LOW bits (r | g << 5 | b << 11),
          // the reverse of Linux's RGB565, so it can't be copied straight to a 16-bit panel
          convert_row_any(dst, dst_x, first_x, last_x, scale, [src](int sx) {
            uint16_t v = reinterpret_cast<const uint16_t *>(src)[sx];
            return pack(uint8_t((v & 0x1F) << 3), uint8_t(((v >> 5) & 0x3F) << 2), uint8_t(((v >> 11) & 0x1F) << 3));
          });
          break;

        case blit::PixelFormat::P:
          convert_row_any(dst, dst_x, first_x, last_x, scale, [src, &palette_lut](int sx) {
            return palette_lut[src[sx]];
          });
          break;

        default: // set_screen_mode_format() only accepts the three formats above
          return;
      }
    }
  }

  void display_shutdown() {
    if(fb_visible)
      clear_screen();

    if(fb_map)
      munmap(fb_map, fb_map_size);

    if(fb_fd >= 0)
      close(fb_fd);

    fb_map = nullptr;
    fb_visible = nullptr;
    fb_map_size = 0;
    fb_fd = -1;
  }
}
