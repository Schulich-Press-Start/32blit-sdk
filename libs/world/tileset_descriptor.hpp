#pragma once

#include <bitset>
#include <charconv>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace sdk {
  struct TilesetDescriptor {
    static constexpr unsigned tile_id_count = 256;
    using Assignments = std::bitset<tile_id_count>;
    using Properties = std::map<std::string, Assignments, std::less<>>;

    std::string image, transparent;
    Properties properties; // One-based atlas IDs. Bit 0 is reserved for empty cells.
  };

  /// Returns an error or nullptr. An invalid descriptor does not replace the output.
  inline const char *read_tileset_descriptor(const nlohmann::json &json, TilesetDescriptor &output) {
    if(!json.is_object()) return "Expected a tileset descriptor object.";
    const auto image = json.find("image");
    if(image == json.end() || !image->is_string() || image->get_ref<const std::string &>().empty())
      return "Tileset image must be a nonempty path.";

    TilesetDescriptor descriptor;
    descriptor.image = image->get_ref<const std::string &>();
    if(const auto transparent = json.find("transparent"); transparent != json.end()) {
      if(!transparent->is_string()) return "Tileset transparency must be a string.";
      descriptor.transparent = transparent->get_ref<const std::string &>();
    }

    const auto definitions = json.find("properties");
    const bool legacy = definitions == json.end();
    const auto valid_name = [](const std::string &name) {
      return !name.empty() && name.find('\0') == name.npos;
    };
    if(!legacy) {
      if(!definitions->is_array()) return "Tileset properties must be an array of names.";
      for(const auto &definition : *definitions) {
        if(!definition.is_string() || !valid_name(definition.get_ref<const std::string &>()))
          return "Tileset properties must have nonempty names.";
        if(!descriptor.properties.try_emplace(definition.get_ref<const std::string &>()).second)
          return "Tileset property names must be unique.";
      }
    }

    if(const auto tiles = json.find("tiles"); tiles != json.end()) {
      if(!tiles->is_object()) return "Tiles must be an object keyed by tile ID.";
      for(auto tile = tiles->begin(); tile != tiles->end(); ++tile) {
        const auto &key = tile.key();
        unsigned id = 0;
        const auto end = key.data() + key.size();
        const auto parsed = std::from_chars(key.data(), end, id);
        if(parsed.ec != std::errc{} || parsed.ptr != end || key.front() == '0' || id >= TilesetDescriptor::tile_id_count)
          return "Tileset tile IDs must be integers from 1 to 255.";
        if(!tile->is_object()) return "Each tile's properties must be an object.";
        for(auto flag = tile->begin(); flag != tile->end(); ++flag) {
          if(!valid_name(flag.key()) || !flag->is_boolean())
            return "Tile properties must have nonempty names and boolean values.";
          // Old assets have no definitions. Even a false key can supply an unchecked definition.
          const auto property = legacy ? descriptor.properties.try_emplace(flag.key()).first
                                       : descriptor.properties.find(flag.key());
          if(property == descriptor.properties.end()) return "A tile uses an undefined tileset property.";
          if(flag->get<bool>()) property->second.set(id);
          else if(!legacy) return "Unchecked tile properties must be omitted.";
        }
      }
    }

    output = std::move(descriptor);
    return nullptr;
  }

  /// Write the current schema without discarding unrelated descriptor fields.
  inline void write_tileset_descriptor(nlohmann::json &json, const TilesetDescriptor &descriptor) {
    using Json = nlohmann::json;
    json["image"] = descriptor.image;
    if(descriptor.transparent.empty()) json.erase("transparent");
    else json["transparent"] = descriptor.transparent;

    auto definitions = Json::array();
    for(const auto &property : descriptor.properties) definitions.push_back(property.first);
    json["properties"] = std::move(definitions);

    auto tiles = Json::object();
    for(unsigned id = 1; id < TilesetDescriptor::tile_id_count; ++id) {
      Json flags;
      for(const auto &[name, assignments] : descriptor.properties)
        if(assignments.test(id)) flags[name] = true;
      if(!flags.is_null()) tiles[std::to_string(id)] = std::move(flags);
    }
    if(!tiles.empty() || json.contains("tiles")) json["tiles"] = std::move(tiles);
  }
}
