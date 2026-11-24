#pragma once

#include "graphics/surface.hpp"
#include "registry.hpp"
#include "sprite_clips.hpp"

#include <cmath>
#include <string_view>

namespace sdk {
  /// Draw an entity's sprite using a descriptor/sheet already resolved from Sprite::asset.
  /// view_origin is the world point at destination (0, 0). Offset applies before conversion
  /// to whole pixels and does not flip with the artwork. A blank clip uses the first clip,
  /// or cell zero if none exist. A missing override leaves the authored selection in effect.
  /// Returns false for an unknown authored clip or invalid frame.
  inline bool draw_sprite(blit::Surface &target, blit::Surface &sheet, const SpriteClips &asset,
                          const Transform2D &transform, const Sprite &sprite, blit::Point view_origin,
                          uint32_t elapsed_ms, bool flip_x = false, std::string_view clip_override = {}) {
    auto clip = asset.clips.begin();
    if (sprite.clip[0]) {
      clip = asset.clips.find(sprite.clip);
      if (clip == asset.clips.end()) return false;
    }
    if (!clip_override.empty()) {
      const auto alternate = asset.clips.find(clip_override);
      if (alternate != asset.clips.end()) clip = alternate;
    }
    const unsigned cell = clip == asset.clips.end() ? 0 : sprite_cell_at(clip->second, elapsed_ms);
    if (!asset.frame_width || !asset.frame_height || sheet.bounds.w <= 0 || sheet.bounds.h <= 0) return false;
    if(sheet.bounds.w % asset.frame_width || sheet.bounds.h % asset.frame_height) return false;
    const unsigned columns = static_cast<unsigned>(sheet.bounds.w) / asset.frame_width;
    const unsigned rows = static_cast<unsigned>(sheet.bounds.h) / asset.frame_height;
    if (!columns || cell / columns >= rows) return false;
    const double x = double(transform.x) + sprite.offset_x, y = double(transform.y) + sprite.offset_y;
    // Cull in floating point before converting or constructing a screen rectangle.
    const double left = std::trunc(x) - view_origin.x, top = std::trunc(y) - view_origin.y;
    if (!(left > double(target.clip.x) - asset.frame_width && left < double(target.clip.x) + target.clip.w &&
          top > double(target.clip.y) - asset.frame_height && top < double(target.clip.y) + target.clip.h)) return true;
    const blit::Rect frame(cell % columns * asset.frame_width, cell / columns * asset.frame_height,
                           asset.frame_width, asset.frame_height);
    target.blit(&sheet, frame, blit::Point(static_cast<int32_t>(left), static_cast<int32_t>(top)),
                flip_x ? blit::SpriteTransform::HORIZONTAL : blit::SpriteTransform::NONE);
    return true;
  }
}
