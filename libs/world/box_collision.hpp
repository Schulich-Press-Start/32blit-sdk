#pragma once

#include "registry.hpp"

namespace sdk {
  struct Box { float left, top, right, bottom; };

  inline Box bounds(const Transform2D &position, const BoxCollider2D &collider) {
    const float left = position.x + collider.offset_x, top = position.y + collider.offset_y;
    return {left, top, left + collider.width, top + collider.height};
  }

  inline float center_y(Box box) { return (box.top + box.bottom) * 0.5f; }

  /// Disabled (empty) boxes and boxes that merely touch do not overlap.
  inline bool overlaps(Box a, Box b) {
    return a.left < a.right && a.top < a.bottom && b.left < b.right && b.top < b.bottom &&
           a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
  }
}
