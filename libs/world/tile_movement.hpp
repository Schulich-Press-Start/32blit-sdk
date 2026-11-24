#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

#include "../../32blit/types/point.hpp"
#include "registry.hpp"

namespace sdk {
  struct TileMoveResult {
    blit::Vec2 moved{};
    bool hit_x = false, hit_y = false;
  };

  /// Move the positive-area box along X, then Y, testing only newly entered tile strips.
  /// Exact contact is not a hit unless the requested travel crosses the contact plane.
  /// is_solid receives in-bounds cells only. Outside is empty unless block_edges is set.
  /// Tiles already overlapped on an axis are not resolved; motion may leave them.
  /// Closed map edges allow inward movement from outside but never push the box inward.
  /// A box too large to fit on an axis cannot move on that axis with block_edges set.
  /// Invalid grids, nonpositive box dimensions, and nonfinite geometry or unrepresentable
  /// requested positions leave the transform unchanged and report no hits.
  template<class IsSolid>
  TileMoveResult move_against_tiles(Transform2D &position, const BoxCollider2D &shape,
                                   blit::Vec2 delta, int columns, int rows, int tile_size,
                                   IsSolid is_solid, bool block_edges = false) {
    const double requested_x = double(position.x) + double(delta.x);
    const double requested_y = double(position.y) + double(delta.y);
    constexpr double max_position = double((std::numeric_limits<float>::max)());
    if(columns <= 0 || rows <= 0 || tile_size <= 0 ||
       !(std::abs(requested_x) <= max_position && std::abs(requested_y) <= max_position) ||
       !(shape.width > 0 && shape.height > 0) || !std::isfinite(shape.width) || !std::isfinite(shape.height) ||
       !std::isfinite(shape.offset_x) || !std::isfinite(shape.offset_y)) return {};

    const double tile = tile_size;
    // Clip before converting to int, even for travel starting far outside the map.
    const auto cell_floor = [tile](double edge, int count) {
      return int(std::floor(std::clamp(edge / tile, 0.0, double(count))));
    };
    const auto cell_ceil = [tile](double edge, int count) {
      return int(std::ceil(std::clamp(edge / tile, 0.0, double(count))));
    };
    const auto sweep = [&](bool x_axis, float &coord, double requested, float travel,
                           float box_offset, float box_size, float cross_coord, float cross_offset, float cross_size,
                           int strips, int cross_cells) {
      if(travel == 0) return false;
      const bool forward = travel > 0;
      const double offset = box_offset, front_offset = offset + double(box_size);
      const auto beyond = [forward](double a, double b) { return forward ? a > b : a < b; };
      double target = requested;
      float destination = static_cast<float>(target);
      bool hit = false;
      const auto stop_at = [&](double contact) {
        if(beyond(target, contact)) {
          target = contact;
          hit = true;
        }
        if(beyond(double(destination), contact)) {
          destination = static_cast<float>(contact);
          // An offset box's contact need not be representable by a float transform.
          if(beyond(double(destination), contact)) destination = std::nextafter(destination, coord);
        }
      };

      if(block_edges) {
        const double extent = strips * tile;
        if(double(box_size) > extent) return true;
        const double edge = forward ? extent - front_offset : -offset;
        stop_at(beyond(double(coord), edge) ? double(coord) : edge);
      }

      const double leading_offset = forward ? front_offset : offset;
      const double start_edge = double(coord) + leading_offset;
      // Detect crossings from the requested travel and from rounding the stored position.
      const double end_edge = (beyond(double(destination), target) ? double(destination) : target) + leading_offset;
      // A positive sweep tests each strip's near (left/top) face; a negative one
      // tests its far (right/bottom) face. A face at the endpoint is not crossed.
      const int first = forward ? cell_ceil(start_edge, strips) : cell_floor(start_edge, strips);
      const int end = forward ? cell_ceil(end_edge, strips) : cell_floor(end_edge, strips);
      if(first != end) {
        const int cross_begin = cell_floor(double(cross_coord) + double(cross_offset), cross_cells);
        const int cross_end = cell_ceil(double(cross_coord) + (double(cross_offset) + double(cross_size)), cross_cells);
        if(cross_begin < cross_end) {
          const int step = forward ? 1 : -1;
          for(int face = first; face != end; face += step) {
            const int strip = forward ? face : face - 1;
            for(int cell = cross_begin; cell < cross_end; ++cell) {
              if(is_solid(x_axis ? blit::Point{strip, cell} : blit::Point{cell, strip})) {
                const double contact = face * tile - leading_offset;
                stop_at(beyond(double(coord), contact) ? double(coord) : contact);
                coord = destination;
                return hit;
              }
            }
          }
        }
      }
      coord = destination;
      return hit;
    };

    const float start_x = position.x, start_y = position.y;
    const bool hit_x = sweep(true, position.x, requested_x, delta.x, shape.offset_x, shape.width,
                             start_y, shape.offset_y, shape.height, columns, rows);
    const bool hit_y = sweep(false, position.y, requested_y, delta.y, shape.offset_y, shape.height,
                             position.x, shape.offset_x, shape.width, rows, columns);
    return {blit::Vec2(position.x - start_x, position.y - start_y), hit_x, hit_y};
  }
}
