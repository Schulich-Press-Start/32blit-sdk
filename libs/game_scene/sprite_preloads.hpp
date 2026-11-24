#pragma once

#include "../world/registry.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace sdk::detail {
  // Validate every entry, but let the caller decide which scene's assets to load.
  template<class Visit>
  void visit_sprite_preloads(const nlohmann::json &manifest, const std::filesystem::path &root, Visit visit) {
    namespace fs = std::filesystem;
    const auto preloads = manifest.find("sprite_preloads");
    if(preloads == manifest.end()) return;
    if(!preloads->is_object()) throw std::runtime_error("Expected sprite_preloads to map .scene paths to arrays of sprite stems.");

    const auto resolve = [&](const std::string &name) {
      const auto path = fs::weakly_canonical(root / fs::u8path(name));
      if(std::mismatch(root.begin(), root.end(), path.begin(), path.end()).first != root.end())
        throw std::runtime_error("sprite_preloads path must stay inside the project: " + name);
      return path;
    };

    constexpr std::string_view suffix = ".sprite.json";
    for(auto entry = preloads->begin(); entry != preloads->end(); ++entry) {
      const auto &scene = entry.key();
      if(!valid_project_relative_path(scene) || fs::u8path(scene).extension() != ".scene" || !entry->is_array())
        throw std::runtime_error("Expected a .scene path and an array of sprite stems in sprite_preloads: " + scene);
      const auto scene_path = resolve(scene);
      for(const auto &value : *entry) {
        const auto *stem = value.get_ptr<const std::string *>();
        if(!stem || stem->size() >= path_field_size || !valid_project_relative_path(*stem) ||
           (stem->size() >= suffix.size() && stem->compare(stem->size() - suffix.size(), suffix.size(), suffix) == 0))
          throw std::runtime_error("Expected a Sprite.asset stem without .sprite.json in sprite_preloads for " + scene);
        resolve(*stem + std::string(suffix));
        visit(scene_path, *stem);
      }
    }
  }
}
