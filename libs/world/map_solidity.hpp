#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace sdk {
  enum class SolidOverride : uint8_t { Inherit, Walkable, Solid };
  using MapSolidity = std::vector<SolidOverride>; // Empty, or indexed in BTMP row-major order.

  // Leave the output alone on a malformed sidecar. Missing entries inherit atlas solidity.
  inline const char *read_map_solidity(const nlohmann::json &sidecar, int width, int height, MapSolidity &out) {
    if(!sidecar.is_object()) return "Expected a map sidecar object.";
    MapSolidity cells;
    const auto overrides = sidecar.find("solid_overrides");
    if(overrides != sidecar.end()) {
      if(!overrides->is_array()) return "solid_overrides must be an array.";
      if(!overrides->empty()) cells.resize(size_t(width) * height);
      for(const auto &entry : *overrides) {
        if(!entry.is_object()) return "Each solid override needs x, y, and a Boolean solid value.";
        const auto x = entry.find("x"), y = entry.find("y"), solid = entry.find("solid");
        if(x == entry.end() || !x->is_number_integer() || y == entry.end() || !y->is_number_integer() ||
           solid == entry.end() || !solid->is_boolean())
          return "Each solid override needs integer x and y coordinates and a Boolean solid value.";
        const auto col = x->get<uint64_t>(), row = y->get<uint64_t>();
        if(col >= uint64_t(width) || row >= uint64_t(height)) return "A solid override is outside the map.";
        auto &cell = cells[size_t(row) * width + size_t(col)];
        if(cell != SolidOverride::Inherit) return "A map cell has duplicate solid overrides.";
        cell = solid->get<bool>() ? SolidOverride::Solid : SolidOverride::Walkable;
      }
    }
    out = std::move(cells);
    return nullptr;
  }

  inline void write_map_solidity(nlohmann::json &sidecar, int width, const MapSolidity &cells) {
    auto overrides = nlohmann::json::array();
    for(size_t i = 0; i < cells.size(); ++i) {
      if(cells[i] != SolidOverride::Inherit)
        overrides.push_back({{"x", i % width}, {"y", i / width}, {"solid", cells[i] == SolidOverride::Solid}});
    }
    if(overrides.empty()) sidecar.erase("solid_overrides");
    else sidecar["solid_overrides"] = std::move(overrides);
  }

  inline SolidOverride solid_override_at(const MapSolidity &cells, size_t index) {
    return cells.empty() ? SolidOverride::Inherit : cells[index];
  }
}
