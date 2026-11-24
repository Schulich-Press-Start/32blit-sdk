#pragma once

#include <cstdint>

namespace sdk {
  struct RouteCell { int x, y; };

  constexpr bool operator==(RouteCell a, RouteCell b) { return a.x == b.x && a.y == b.y; }

  struct RouteIssue {
    const char *message = nullptr;
    RouteCell cell{-1, -1};
  };

  /// Resolve world pixels to a grid cell and check the integer tile center.
  /// An off-center coordinate still reports its containing cell; an outside coordinate does not.
  inline bool centered_route_cell(int width, int height, int tile_size, double x, double y, RouteCell &cell) {
    cell = {-1, -1};
    if(tile_size <= 0) return false;
    const double column = x / tile_size, row = y / tile_size;
    if(!(x >= 0 && column < width && y >= 0 && row < height)) return false;
    cell = {int(column), int(row)};
    return x == double(cell.x) * tile_size + tile_size / 2 && y == double(cell.y) * tile_size + tile_size / 2;
  }

  /// Validate one orthogonal chain containing every path cell. The predicate is
  /// called only for in-bounds cells and must remain stable during the trace.
  /// Emit receives cells in spawn-to-goal order; discard that output on error.
  template<class IsPath, class Emit>
  RouteIssue trace_route(int width, int height, RouteCell spawn, RouteCell goal, IsPath is_path, Emit emit) {
    if(width <= 0 || height <= 0) return {"Route needs a non-empty grid."};
    const auto contains = [&](RouteCell cell) {
      return cell.x >= 0 && cell.x < width && cell.y >= 0 && cell.y < height;
    };
    const auto path_at = [&](RouteCell cell) { return contains(cell) && is_path(cell); };
    if(!contains(spawn)) return {"Spawn is outside the map.", spawn};
    if(!contains(goal)) return {"Goal is outside the map.", goal};
    if(spawn == goal) return {"Spawn and goal must use different tiles.", spawn};
    if(!is_path(spawn)) return {"Spawn must be on a path tile.", spawn};
    if(!is_path(goal)) return {"Goal must be on a path tile.", goal};

    constexpr RouteCell directions[]{{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    std::uint64_t path_count = 0;
    RouteIssue bad_endpoint, stray_end;
    for(int y = 0; y < height; ++y) {
      for(int x = 0; x < width; ++x) {
        const RouteCell cell{x, y};
        if(!is_path(cell)) continue;
        ++path_count;
        int degree = 0;
        for(const auto dir : directions)
          if(path_at({x + dir.x, y + dir.y})) ++degree;
        if(degree > 2) return {"Path branches; keep a single lane.", cell};
        if(cell == spawn || cell == goal) {
          if(degree != 1 && !bad_endpoint.message)
            bad_endpoint = {cell == spawn ? "Spawn must be a path endpoint." : "Goal must be a path endpoint.", cell};
        } else if(degree != 2 && !stray_end.message) {
          stray_end = {"Path has a gap or an extra end.", cell};
        }
      }
    }
    if(bad_endpoint.message) return bad_endpoint;
    if(stray_end.message) return stray_end;

    // In a finite undirected grid, a chain starting at a degree-one cell can
    // neither loop nor stop before the other degree-one cell.
    const auto walk_chain = [&](auto &&visit) {
      RouteCell previous{-1, -1}, current = spawn;
      for(;;) {
        visit(current);
        if(current == goal) return;
        for(const auto dir : directions) {
          const RouteCell next{current.x + dir.x, current.y + dir.y};
          if(next == previous || !path_at(next)) continue;
          previous = current;
          current = next;
          break;
        }
      }
    };

    std::uint64_t route_count = 0;
    walk_chain([&](RouteCell cell) { ++route_count; emit(cell); });
    if(route_count == path_count) return {};

    // The remaining cells form disconnected loops. Find the first one by
    // comparing prefix counts, only on failure, without a grid-sized buffer.
    const auto index = [&](RouteCell cell) { return std::uint64_t(cell.y) * width + cell.x; };
    const auto cell_at = [&](std::uint64_t i) { return RouteCell{int(i % width), int(i / width)}; };
    std::uint64_t first = 0, last = std::uint64_t(width) * height - 1;
    while(first < last) {
      const auto middle = first + (last - first) / 2;
      std::uint64_t unmatched = 0;
      for(auto i = first; i <= middle; ++i)
        if(is_path(cell_at(i))) ++unmatched;
      walk_chain([&](RouteCell cell) {
        const auto i = index(cell);
        if(i >= first && i <= middle) --unmatched;
      });
      if(unmatched) last = middle;
      else first = middle + 1;
    }
    return {"Path contains a disconnected loop.", cell_at(first)};
  }
}
