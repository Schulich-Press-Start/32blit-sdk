/**
 * \file panel.cpp
 * \brief Frame conversion into a panel's pixel layout (see panel.hpp).
 *
 * Every frame, each game pixel is unpacked to 8-bit R, G, B and packed again
 * in the panel's layout, `scale` times horizontally; each output row is then
 * copied `scale - 1` more times for vertical scaling.
 *
 * The conversion is plain C++. NEON (ARM SIMD) could speed it up later if
 * profiling shows it's needed.
 */
#include "panel.hpp"

#include <algorithm>
#include <cstring>

namespace blit_linux {

  namespace {
    /**
     * \brief Convert one row of output pixels.
     *
     * \tparam Out       `uint16_t` or `uint32_t`, matching the panel's pixel size.
     * \tparam ReadPixel Callable taking a source column and returning a packed panel pixel.
     * \param dst        First output pixel of the row.
     * \param width      Output pixels in the row.
     * \param scale      Output pixels per source pixel.
     * \param read_pixel Returns the packed panel pixel for a source column.
     */
    template<typename Out, typename ReadPixel>
    void convert_row(uint8_t *dst, int width, int scale, ReadPixel read_pixel) {
      auto out = reinterpret_cast<Out *>(dst);

      for(int x = 0; x < width; x++)
        out[x] = Out(read_pixel(x / scale));
    }

    /** \brief Dispatch convert_row() on the panel's pixel size. */
    template<typename ReadPixel>
    void convert_row_any(int bytes_per_pixel, uint8_t *dst, int width, int scale, ReadPixel read_pixel) {
      if(bytes_per_pixel == 2)
        convert_row<uint16_t>(dst, width, scale, read_pixel);
      else
        convert_row<uint32_t>(dst, width, scale, read_pixel);
    }

    /**
     * \brief Widen a 5-bit channel to 8 bits so its maximum becomes 255.
     *
     * Copies the top bits into the empty low bits: 31 becomes 255, not 248.
     */
    inline uint8_t widen5(uint8_t v) { return uint8_t((v << 3) | (v >> 2)); }

    /** \brief Widen a 6-bit channel to 8 bits so its maximum becomes 255 (63 becomes 255, not 252). */
    inline uint8_t widen6(uint8_t v) { return uint8_t((v << 2) | (v >> 4)); }
  }

  void clear_panel(const PanelBuffer &panel, const PanelFormat &format) {
    const uint32_t black = format.pack(0, 0, 0);

    for(int y = 0; y < panel.size.h; y++) {
      auto row = panel.pixels + size_t(y) * panel.line_length;

      if(format.bytes_per_pixel == 2)
        std::fill_n(reinterpret_cast<uint16_t *>(row), panel.size.w, uint16_t(black));
      else
        std::fill_n(reinterpret_cast<uint32_t *>(row), panel.size.w, black);
    }
  }

  void present_frame(const PanelBuffer &panel, const PanelFormat &format, const blit::SurfaceInfo &frame, const ScreenFit &fit) {
    const int out_width = frame.bounds.w * fit.scale;
    const int out_height = frame.bounds.h * fit.scale;

    // never write outside the panel, whatever the caller passed
    if(fit.scale < 1 || fit.x < 0 || fit.y < 0 || fit.x + out_width > panel.size.w || fit.y + out_height > panel.size.h)
      return;

    const int src_stride = frame.bounds.w * blit::pixel_format_stride[int(frame.format)];
    const size_t row_bytes = size_t(out_width) * format.bytes_per_pixel;

    // palette entries packed once per frame instead of once per pixel
    uint32_t palette_lut[256];
    if(frame.format == blit::PixelFormat::P) {
      for(int i = 0; i < 256; i++)
        palette_lut[i] = format.pack(frame.palette[i].r, frame.palette[i].g, frame.palette[i].b);
    }

    for(int y = 0; y < out_height; y++) {
      uint8_t *dst = panel.pixels + size_t(fit.y + y) * panel.line_length + size_t(fit.x) * format.bytes_per_pixel;

      // vertical scaling: repeat the previous output row instead of converting again
      if(y % fit.scale != 0) {
        memcpy(dst, dst - panel.line_length, row_bytes);
        continue;
      }

      const uint8_t *src = frame.data + (y / fit.scale) * src_stride;

      switch(frame.format) {
        case blit::PixelFormat::RGB:
          convert_row_any(format.bytes_per_pixel, dst, out_width, fit.scale, [src, &format](int sx) {
            auto p = src + sx * 3;
            return format.pack(p[0], p[1], p[2]);
          });
          break;

        case blit::PixelFormat::RGB565:
          // 32blit's RGB565 keeps red in the LOW bits (r | g << 5 | b << 11),
          // the reverse of Linux's RGB565, so it can't be copied straight to a 16-bit panel
          convert_row_any(format.bytes_per_pixel, dst, out_width, fit.scale, [src, &format](int sx) {
            uint16_t v = reinterpret_cast<const uint16_t *>(src)[sx];
            return format.pack(widen5(v & 0x1F), widen6((v >> 5) & 0x3F), widen5((v >> 11) & 0x1F));
          });
          break;

        case blit::PixelFormat::P:
          convert_row_any(format.bytes_per_pixel, dst, out_width, fit.scale, [src, &palette_lut](int sx) {
            return palette_lut[src[sx]];
          });
          break;

        // set_screen_mode_format() only accepts the three formats above
        default:
          return;
      }
    }
  }
}
