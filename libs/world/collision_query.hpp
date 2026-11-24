#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

#include "../../32blit/types/point.hpp"
#include "../../32blit/types/vec2.hpp"
#include "box_collision.hpp"
#include "world.hpp"

namespace sdk {
  using CollisionMask = uint32_t;
  constexpr CollisionMask collision_bit(uint32_t category) {
    return category < 32 ? CollisionMask{1} << category : 0;
  }
  constexpr CollisionMask all_collision_categories = UINT32_MAX;

  struct BoxSweepHit {
    double fraction = 1;
    blit::Point normal{};
  };

  /// Sweep a positive-area box against a stationary positive-area box. Initial
  /// overlap has no entry normal. Head-on contact at either endpoint counts;
  /// touching without moving into overlap does not. X wins a corner-entry tie.
  inline bool sweep_box(Box moving, blit::Vec2 delta, Box target, BoxSweepHit &hit) {
    hit = {};
    if(!(moving.left < moving.right && moving.top < moving.bottom &&
         target.left < target.right && target.top < target.bottom) ||
       !std::isfinite(delta.x) || !std::isfinite(delta.y)) return false;
    if(overlaps(moving, target)) {
      hit.fraction = 0;
      return true;
    }

    double enter = -std::numeric_limits<double>::infinity();
    double exit = std::numeric_limits<double>::infinity();
    blit::Point normal{};
    const auto clip_axis = [&](double lo, double hi, double target_lo, double target_hi,
                               double velocity, blit::Point negative_normal) {
      if(velocity == 0) return lo < target_hi && target_lo < hi;
      const bool forward = velocity > 0;
      const double entry_time = forward ? (target_lo - hi) / velocity : (target_hi - lo) / velocity;
      const double exit_time = forward ? (target_hi - lo) / velocity : (target_lo - hi) / velocity;
      if(entry_time > enter) {
        enter = entry_time;
        normal = forward ? negative_normal : blit::Point{-negative_normal.x, -negative_normal.y};
      }
      if(exit_time < exit) exit = exit_time;
      return enter < exit;
    };
    if(!clip_axis(moving.left, moving.right, target.left, target.right, delta.x, {-1, 0}) ||
       !clip_axis(moving.top, moving.bottom, target.top, target.bottom, delta.y, {0, -1}) ||
       !(enter >= 0 && enter <= 1)) return false;
    hit = {enter == 0 ? 0 : enter, normal};
    return true;
  }

  struct EntitySweepHit {
    Entity entity{};
    BoxSweepHit contact{};
  };

  /// Sweep against current collider positions. The mask selects target categories
  /// only, and equal fractions prefer the lowest live entity slot.
  inline EntitySweepHit sweep_entities(World &world, Box moving, blit::Vec2 delta,
                                       CollisionMask mask = all_collision_categories, Entity ignore = {}) {
    EntitySweepHit best;
    if(!mask) return best;
    const auto ignored_index = world.alive(ignore) ? ignore.index() : UINT16_MAX;
    world.each<Transform2D, BoxCollider2D>([&](Entity entity, const Transform2D &pos, const BoxCollider2D &shape) {
      if(entity.index() == ignored_index || !(mask & collision_bit(shape.category))) return;
      BoxSweepHit contact;
      if(sweep_box(moving, delta, bounds(pos, shape), contact) &&
         (!best.entity || contact.fraction < best.contact.fraction ||
          (contact.fraction == best.contact.fraction && entity.index() < best.entity.index())))
        best = {entity, contact};
    });
    return best;
  }
}
