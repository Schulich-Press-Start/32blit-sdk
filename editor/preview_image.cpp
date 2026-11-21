#include "preview_image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#include <stb_image.h>

#include <charconv>
#include <limits>

namespace editor {
  std::string PreviewImage::load_png(SDL_Renderer *renderer, const std::vector<uint8_t> &bytes, std::string_view transparent) {
    *this = {};
    if(bytes.empty() || bytes.size() > std::numeric_limits<int>::max()) return "Invalid PNG data.";
    int w, h;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
      stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, nullptr, 4), stbi_image_free);
    if(!pixels) return "Cannot decode PNG.";
    if(!transparent.empty()) {
      unsigned rgb = 0;
      const auto end = transparent.data() + transparent.size();
      const auto result = std::from_chars(transparent.data(), end, rgb, 16);
      if(transparent.size() != 6 || result.ec != std::errc{} || result.ptr != end)
        return "Invalid image transparency color.";
      for(auto *p = pixels.get(), *last = p + 4 * w * h; p != last; p += 4)
        if(((unsigned(p[0]) << 16) | (unsigned(p[1]) << 8) | p[2]) == rgb) p[3] = 0;
    }
    decltype(texture) output(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h));
    if(!output || SDL_UpdateTexture(output.get(), nullptr, pixels.get(), w * 4) != 0 ||
       SDL_SetTextureBlendMode(output.get(), SDL_BLENDMODE_BLEND) != 0 ||
       SDL_SetTextureScaleMode(output.get(), SDL_ScaleModeNearest) != 0) return SDL_GetError();
    texture = std::move(output);
    width = w;
    height = h;
    return {};
  }
}
