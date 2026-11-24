#pragma once

#include "grid_route.hpp"

#include <cstddef>
#include <limits>
#include <vector>

namespace sdk {
  /// Reusable scratch for shortest, four-direction paths with uniform step costs.
  class GridNavigation {
  public:
    // Call when dimensions change, not per search. Zero disables navigation.
    void resize(int columns, int rows) {
      if(columns <= 0 || rows <= 0 ||
         std::size_t(columns) > std::size_t((std::numeric_limits<int>::max)()) / rows) {
        columns = rows = 0;
      }
      const auto count = std::size_t(columns) * rows;
      parent_.resize(count);
      queue_.reserve(count);
      path_.reserve(count);
      path_.clear();
      columns_ = columns;
      rows_ = rows;
    }

    const std::vector<RouteCell> &path() const { return path_; }

    /// Return a route including both endpoints, or an empty route if unreachable.
    /// The result belongs to this workspace, until the next find() or resize().
    /// The predicate receives only in-bounds cells. Searches do not allocate.
    template<class Walkable>
    const std::vector<RouteCell> &find(RouteCell start, RouteCell goal, Walkable walkable) {
      path_.clear();
      if(!contains(start) || !contains(goal) || !walkable(start) || !walkable(goal)) return path_;
      const int source = index(start), target = index(goal);
      queue_.clear();
      for(auto &parent : parent_) parent = -1;
      parent_[source] = source;
      queue_.push_back(source);
      constexpr RouteCell directions[]{{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
      for(std::size_t i = 0; i < queue_.size() && parent_[target] == -1; ++i) {
        const int current = queue_[i];
        const auto from = cell(current);
        for(const auto d : directions) {
          const RouteCell next{from.x + d.x, from.y + d.y};
          if(!contains(next)) continue;
          const int to = index(next);
          if(parent_[to] != -1 || !walkable(next)) continue;
          parent_[to] = current;
          if(to == target) break;
          queue_.push_back(to);
        }
      }
      if(parent_[target] == -1) return path_;
      for(int i = target; i != source; i = parent_[i]) path_.push_back(cell(i));
      path_.push_back(start);
      for(std::size_t i = 0, j = path_.size() - 1; i < j; ++i, --j) {
        const auto tmp = path_[i];
        path_[i] = path_[j];
        path_[j] = tmp;
      }
      return path_;
    }

  private:
    bool contains(RouteCell p) const { return p.x >= 0 && p.x < columns_ && p.y >= 0 && p.y < rows_; }
    int index(RouteCell p) const { return p.y * columns_ + p.x; }
    RouteCell cell(int i) const { return {i % columns_, i / columns_}; }
    int columns_ = 0, rows_ = 0;
    std::vector<int> parent_, queue_;
    std::vector<RouteCell> path_;
  };
}
