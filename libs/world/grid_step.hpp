#pragma once

#include <cmath>

#include "../../32blit/types/point.hpp"
#include "registry.hpp"

namespace sdk {
  enum class StepDirection : unsigned char {
    none, left, right, up, down
  };

  // Per-actor stepping state. Call grid_place() after open(), travel(), or any other
  // change that may leave the actor off-grid or the state pointing at a stale cell.
  struct GridStepState {
    blit::Point cell{};         // cell the actor occupies, or is leaving while moving
    blit::Point destination{};  // cell being entered while moving
    StepDirection facing = StepDirection::none;   // direction the walk clip should show
    StepDirection moving = StepDirection::none;   // StepDirection::none while idle
    int elapsed_ms = 0;
  };

  struct GridStepResult {
    bool started = false;   // a step began during this call
    bool finished = false;  // a step landed during this call
    bool blocked = false;   // a requested step was refused (edge or unwalkable cell)
  };

  namespace step_detail {
    inline blit::Point neighbour(blit::Point cell, StepDirection direction) {
      switch(direction) {
        case StepDirection::left:  return {cell.x - 1, cell.y};
        case StepDirection::right: return {cell.x + 1, cell.y};
        case StepDirection::up:    return {cell.x, cell.y - 1};
        case StepDirection::down:  return {cell.x, cell.y + 1};
        default:                   return cell;
      }
    }
  }

  /// Box top-left that centres `shape` inside `cell`. Offsets are honoured, so a collider
  /// anchored at the actor's feet still lands in the right cell.
  inline blit::Vec2 grid_box_origin(blit::Point cell, int tile_size, const BoxCollider2D &shape) {
    const float slack_x = (float(tile_size) - shape.width) * 0.5f;
    const float slack_y = (float(tile_size) - shape.height) * 0.5f;
    return blit::Vec2(float(cell.x) * float(tile_size) + slack_x - shape.offset_x,
                      float(cell.y) * float(tile_size) + slack_y - shape.offset_y);
  }

  /// Cell containing the centre of the actor's box. Works for any positive box size.
  inline blit::Point grid_cell(const Transform2D &position, const BoxCollider2D &shape, int tile_size) {
    if(tile_size <= 0) return {};
    const double centre_x = double(position.x + shape.offset_x) + double(shape.width) * 0.5;
    const double centre_y = double(position.y + shape.offset_y) + double(shape.height) * 0.5;
    return {int(std::floor(centre_x / tile_size)), int(std::floor(centre_y / tile_size))};
  }

  /// Snap an actor onto the cell containing its box centre and centre it there.
  /// Facing is preserved so the sprite keeps its last direction; movement is cleared.
  inline void grid_place(GridStepState &state, Transform2D &position, const BoxCollider2D &shape,
                         int tile_size) {
    state.cell = grid_cell(position, shape, tile_size);
    state.destination = state.cell;
    state.moving = StepDirection::none;
    state.elapsed_ms = 0;
    if(tile_size <= 0 || !(shape.width > 0) || !(shape.height > 0)) return;
    const auto origin = grid_box_origin(state.cell, tile_size, shape);
    position.x = origin.x;
    position.y = origin.y;
  }

  /**
   * Advance one tile-locked actor by `elapsed_ms`.
   *
   * While idle, a requested direction starts a step into the neighbouring cell when that
   * cell is in bounds and is_walkable() accepts it, and the step advances by `elapsed_ms`
   * in the same call, so a held direction never loses a frame. `facing` updates as soon as
   * a direction is requested, including when the step is refused, which lets games turn the
   * sprite on the spot. While a step is running, `requested` is ignored until the step
   * lands, so a reversal cannot cut a step short and a held direction resumes on the next
   * call. At most one step starts and one step lands per call.
   *
   * A completed step leaves the transform exactly centred in its cell; while stepping, the
   * transform is interpolated. The stepper therefore owns the actor's position: move the
   * actor some other way with grid_place().
   *
   * is_walkable() receives in-bounds cells only. Invalid grid, box, or timing input leaves
   * the state and transform untouched and reports nothing.
   */
  template<class IsWalkable>
  GridStepResult grid_step(GridStepState &state, Transform2D &position, const BoxCollider2D &shape,
                           StepDirection requested, int tile_size, int step_ms, int columns, int rows,
                           IsWalkable is_walkable, int elapsed_ms) {
    GridStepResult result;
    if(tile_size <= 0 || step_ms <= 0 || columns <= 0 || rows <= 0 || elapsed_ms < 0 ||
       !(shape.width > 0 && shape.height > 0) || !std::isfinite(shape.width) || !std::isfinite(shape.height) ||
       !std::isfinite(shape.offset_x) || !std::isfinite(shape.offset_y)) return result;

    if(state.moving == StepDirection::none && requested != StepDirection::none) {
      state.facing = requested;
      const auto target = step_detail::neighbour(state.cell, requested);
      if(target.x >= 0 && target.y >= 0 && target.x < columns && target.y < rows && is_walkable(target)) {
        state.cell = grid_cell(position, shape, tile_size);
        state.destination = target;
        state.moving = requested;
        state.elapsed_ms = 0;
        const auto origin = grid_box_origin(state.cell, tile_size, shape);
        position.x = origin.x;
        position.y = origin.y;
        result.started = true;
      } else result.blocked = true;
    }

    if(state.moving == StepDirection::none) return result;

    state.elapsed_ms += elapsed_ms;
    if(state.elapsed_ms >= step_ms) {
      state.cell = state.destination;
      state.moving = StepDirection::none;
      state.elapsed_ms = 0;
      const auto origin = grid_box_origin(state.cell, tile_size, shape);
      position.x = origin.x;
      position.y = origin.y;
      result.finished = true;
      return result;
    }

    const auto from = grid_box_origin(state.cell, tile_size, shape);
    const auto to = grid_box_origin(state.destination, tile_size, shape);
    const float progress = float(state.elapsed_ms) / float(step_ms);
    position.x = from.x + (to.x - from.x) * progress;
    position.y = from.y + (to.y - from.y) * progress;
    return result;
  }
}
