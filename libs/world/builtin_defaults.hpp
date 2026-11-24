#pragma once

#include "registry.hpp"

namespace sdk {
  // Only built-ins are safe to instantiate here; reflection does not expose
  // a constructor for arbitrary game components.
  inline const void *builtin_component_defaults(const ComponentInfo &info) {
    static constexpr Name name{};
    static constexpr Transform2D transform{};
    static constexpr BoxCollider2D box{};
    static constexpr Sprite sprite{};
    static constexpr Camera2D camera{};
    if(&info == component_info<Name>()) return &name;
    if(&info == component_info<Transform2D>()) return &transform;
    if(&info == component_info<BoxCollider2D>()) return &box;
    if(&info == component_info<Sprite>()) return &sprite;
    if(&info == component_info<Camera2D>()) return &camera;
    return nullptr;
  }
}
