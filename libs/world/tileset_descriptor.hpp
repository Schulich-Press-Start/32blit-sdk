#pragma once

#include "timed_frames.hpp"
#include <bitset>
#include <charconv>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "../../32blit/types/tile_geometry.hpp"

namespace sdk {
  struct TileFrame { uint8_t cell; uint16_t duration_ms; };
  struct TileAnimation {
    static constexpr unsigned max_frames = 1024, max_duration_ms = 60000;
    std::string name;
    uint8_t cell;
    std::vector<TileFrame> frames;
  };

  struct TilesetDescriptor {
    static constexpr unsigned tile_id_count = blit::atlas_tile_id_count;
    using Assignments = std::bitset<tile_id_count>;
    using Properties = std::map<std::string, Assignments, std::less<>>;

    std::string image, transparent;
    Properties properties; // One-based atlas IDs. Bit 0 is reserved for empty cells.
    std::vector<TileAnimation> animations;
    int tile_size = blit::default_tile_size;
  };

  /// Returns an error or nullptr. An invalid descriptor does not replace the output.
  inline const char *read_tileset_descriptor(const nlohmann::json &json, TilesetDescriptor &output) {
    if(!json.is_object()) return "Expected a tileset descriptor object.";
    const auto image = json.find("image");
    if(image == json.end() || !image->is_string() || image->get_ref<const std::string &>().empty())
      return "Tileset image must be a nonempty path.";

    TilesetDescriptor descriptor;
    descriptor.image = image->get_ref<const std::string &>();
    if(const auto size = json.find("tile_size"); size != json.end()) {
      if(!size->is_number_integer() || *size < 1 || *size > blit::max_tile_size)
        return "Tileset tile_size must be a positive integer no greater than 65535.";
      descriptor.tile_size = size->get<int>();
    }
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

    if(const auto animations = json.find("animations"); animations != json.end()) {
      if(!animations->is_array() || animations->size() >= TilesetDescriptor::tile_id_count)
        return "Tileset animations must be an array of up to 255 clips.";
      const auto number = [](const nlohmann::json &object, const char *key, unsigned max) {
        const auto field = object.find(key);
        if(field == object.end() || !field->is_number_integer()) return 0u;
        const auto value = field->get<uint64_t>();
        return value <= max ? static_cast<unsigned>(value) : 0u;
      };
      std::set<std::string_view> names;
      TilesetDescriptor::Assignments cells;
      descriptor.animations.reserve(animations->size());
      for(const auto &item : *animations) {
        if(!item.is_object()) return "Each tile animation must be an object.";
        const auto name = item.find("name"), frames = item.find("frames");
        const unsigned cell = number(item, "cell", TilesetDescriptor::tile_id_count - 1);
        if(name == item.end() || !name->is_string() || !valid_name(name->get_ref<const std::string &>()))
          return "Each tile animation needs a nonempty name.";
        const auto &label = name->get_ref<const std::string &>();
        if(!cell || cells[cell] || !names.emplace(label).second)
          return "Tile animations need unique names and distinct base cell IDs from 1 to 255.";
        cells.set(cell);
        if(frames == item.end() || !frames->is_array() || frames->empty() || frames->size() > TileAnimation::max_frames)
          return "Each tile animation needs 1 to 1024 frames.";
        auto &clip = descriptor.animations.emplace_back(TileAnimation{label, static_cast<uint8_t>(cell), {}});
        clip.frames.reserve(frames->size());
        for(const auto &frame : *frames) {
          const unsigned id = number(frame, "cell", TilesetDescriptor::tile_id_count - 1);
          const unsigned duration = number(frame, "duration_ms", TileAnimation::max_duration_ms);
          if(!id || !duration) return "A tile frame needs a cell ID from 1 to 255 and duration_ms from 1 to 60000.";
          clip.frames.push_back({static_cast<uint8_t>(id), static_cast<uint16_t>(duration)});
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
    if(descriptor.tile_size != blit::default_tile_size || json.contains("tile_size"))
      json["tile_size"] = descriptor.tile_size;

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

    if(descriptor.animations.empty()) json.erase("animations");
    else {
      auto animations = Json::array();
      for(const auto &clip : descriptor.animations) {
        auto frames = Json::array();
        for(const auto &frame : clip.frames) frames.push_back({{"cell", frame.cell}, {"duration_ms", frame.duration_ms}});
        animations.push_back({{"name", clip.name}, {"cell", clip.cell}, {"frames", std::move(frames)}});
      }
      json["animations"] = std::move(animations);
    }
  }

  inline bool tile_animations_fit_atlas(const std::vector<TileAnimation> &animations, int tile_count) {
    for(const auto &clip : animations) {
      if(clip.cell > tile_count) return false;
      for(const auto &frame : clip.frames)
        if(frame.cell > tile_count) return false;
    }
    return true;
  }

  inline uint8_t tile_animation_cell(const TileAnimation &clip, uint64_t elapsed_ms) {
    const auto index = detail::timed_frame_index(clip.frames, elapsed_ms, true);
    return index == clip.frames.size() ? clip.cell : clip.frames[index].cell;
  }

  // A frame refers to art directly. It does not trigger another animation or inherit its properties.
  inline blit::TileFrameMap tile_animation_frames(const std::vector<TileAnimation> &animations, uint64_t elapsed_ms) {
    blit::TileFrameMap frames;
    for(unsigned id = 0; id < frames.size(); ++id) frames[id] = static_cast<uint8_t>(id);
    for(const auto &clip : animations) frames[clip.cell] = tile_animation_cell(clip, elapsed_ms);
    return frames;
  }
}
