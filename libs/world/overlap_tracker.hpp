#pragma once

#include "collision_query.hpp"

#include <type_traits>
#include <vector>

namespace sdk {
  enum class OverlapPhase { Enter, Stay, Exit };
  struct OverlapEvent { OverlapPhase phase; Entity observer, other; };

  /// Tracks one observer against the matching boxes in a World. Allocate once;
  /// update and reset do not allocate. The World must outlive the tracker.
  class OverlapTracker {
  public:
    explicit OverlapTracker(World &world) : world_(world), slots_(world.capacity()) {
      active_.reserve(world.capacity());
      events_.reserve(size_t(world.capacity()) * 2);
    }
    OverlapTracker(const OverlapTracker &) = delete;
    OverlapTracker &operator=(const OverlapTracker &) = delete;

    /// Drop the baseline without emitting exits, e.g. when a scene is replaced.
    void reset() {
      clear_contacts();
      events_.clear();
    }

    /// Sample after movement. Filter is called for overlapping candidates inside a
    /// World query: it must not mutate the World or re-enter this tracker. The result
    /// is valid until the next update/reset. Process events outside the query, and
    /// recheck handles before use: Exit can reference a destroyed/recycled entity.
    /// This samples positions, not swept motion. Event order is unspecified.
    /// The mask selects target categories before geometry and the optional filter.
    template<class Filter>
    const std::vector<OverlapEvent> &update(Entity observer, CollisionMask mask, Filter &&filter) {
      events_.clear();
      if(!world_.alive(observer)) observer = {};
      if(!same(observer_, observer)) {
        for(const auto index : active_) emit(OverlapPhase::Exit, slots_[index].other);
        clear_contacts();
        observer_ = observer;
      }

      const auto *position = world_.get<Transform2D>(observer_);
      const auto *shape = world_.get<BoxCollider2D>(observer_);
      if(mask && position && shape && shape->width > 0 && shape->height > 0) {
        const auto box = bounds(*position, *shape);
        world_.each<Transform2D, BoxCollider2D>([&](Entity other, const Transform2D &pos, const BoxCollider2D &collider) {
          if(other.index() == observer_.index() || !(mask & collision_bit(collider.category)) ||
             !overlaps(box, bounds(pos, collider)) || !filter(other)) return;
          auto &slot = slots_[other.index()];
          if(same(slot.other, other)) {
            emit(OverlapPhase::Stay, other);
          } else {
            if(slot.other) emit(OverlapPhase::Exit, slot.other);
            else active_.push_back(other.index());
            emit(OverlapPhase::Enter, other);
          }
          slot = {other, true};
        });
      }

      for(size_t i = 0; i < active_.size();) {
        auto &slot = slots_[active_[i]];
        if(slot.seen) {
          slot.seen = false;
          ++i;
        } else {
          emit(OverlapPhase::Exit, slot.other);
          slot = {};
          active_[i] = active_.back();
          active_.pop_back();
        }
      }
      return events_;
    }

    template<class Filter, std::enable_if_t<std::is_invocable_v<Filter &, Entity &>, int> = 0>
    const std::vector<OverlapEvent> &update(Entity observer, Filter filter) {
      return update(observer, all_collision_categories, filter);
    }

    const std::vector<OverlapEvent> &update(Entity observer, CollisionMask mask = all_collision_categories) {
      return update(observer, mask, [](Entity) { return true; });
    }

  private:
    struct Slot { Entity other; bool seen = false; };

    // Inputs are validated against the bound World (or empty), so its identity is implicit.
    static bool same(Entity a, Entity b) { return a.index() == b.index() && a.generation() == b.generation(); }
    void emit(OverlapPhase phase, Entity other) { events_.push_back({phase, observer_, other}); }
    void clear_contacts() {
      for(const auto index : active_) slots_[index] = {};
      active_.clear();
      observer_ = {};
    }

    World &world_;
    Entity observer_;
    std::vector<Slot> slots_;
    std::vector<uint16_t> active_;
    std::vector<OverlapEvent> events_;
  };
}
