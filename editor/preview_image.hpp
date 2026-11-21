#pragma once

#include <SDL.h>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace editor {
  struct PreviewImage {
    // On failure returns an error and leaves no texture.
    std::string load_png(SDL_Renderer *renderer, const std::vector<uint8_t> &bytes, std::string_view transparent = {});

    struct TextureDeleter { void operator()(SDL_Texture *texture) const { SDL_DestroyTexture(texture); } };
    std::unique_ptr<SDL_Texture, TextureDeleter> texture;
    int width = 0, height = 0;
  };
}
