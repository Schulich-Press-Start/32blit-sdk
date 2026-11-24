#pragma once

#include "timed_frames.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace sdk {
  struct SpriteFrame { unsigned cell, duration_ms; };
  struct SpriteClip { bool loop = true; std::vector<SpriteFrame> frames; };
  struct SpriteClips {
    std::string image;
    unsigned frame_width = 8, frame_height = 8;
    std::map<std::string, SpriteClip, std::less<>> clips;
  };

  inline const char *read_sprite_clips(const nlohmann::json &json, SpriteClips &out) {
    const auto number = [](const auto &object, const char *key, unsigned min, unsigned max, unsigned &value) {
      const auto it = object.find(key);
      if(it == object.end() || !it->is_number_integer()) return false;
      const auto count = it->template get<uint64_t>();
      if(count < min || count > max) return false;
      value = static_cast<unsigned>(count);
      return true;
    };
    if(!json.is_object()) return "Expected a sprite asset object.";
    const auto image = json.find("image");
    if(image == json.end() || !image->is_string() || image->get_ref<const std::string &>().empty() ||
       image->get_ref<const std::string &>().find('\0') != std::string::npos) return "Expected a PNG image path.";
    SpriteClips asset;
    asset.image = image->get_ref<const std::string &>();
    if(!number(json, "frame_width", 1, 2048, asset.frame_width) || !number(json, "frame_height", 1, 2048, asset.frame_height))
      return "Frame dimensions must be between 1 and 2048 pixels.";
    const auto clips = json.find("clips");
    if(clips == json.end() || !clips->is_object()) return "Expected clips keyed by name.";
    for(auto item = clips->begin(); item != clips->end(); ++item) {
      if(item.key().empty() || item.key().find('\0') != std::string::npos || !item->is_object()) return "Invalid clip name or body.";
      const auto loop = item->find("loop");
      const auto frames = item->find("frames");
      if(loop == item->end() || !loop->is_boolean()) return "Expected a loop setting for each clip.";
      if(frames == item->end() || !frames->is_array() || frames->empty() || frames->size() > 1024)
        return "Each clip needs 1 to 1024 frames.";
      auto &clip = asset.clips[item.key()];
      clip.loop = loop->get<bool>();
      clip.frames.reserve(frames->size());
      for(const auto &frame : *frames) {
        SpriteFrame entry{};
        if(!frame.is_object() || !number(frame, "cell", 0, 2048u * 2048u - 1, entry.cell) ||
           !number(frame, "duration_ms", 1, 60000, entry.duration_ms)) return "Invalid frame cell or duration (1–60000 ms).";
        clip.frames.push_back(entry);
      }
    }
    out = std::move(asset);
    return nullptr;
  }

  inline nlohmann::json write_sprite_clips(const SpriteClips &asset) {
    using Json = nlohmann::json;
    auto clips = Json::object();
    for(const auto &[name, clip] : asset.clips) {
      auto frames = Json::array();
      for(const auto &frame : clip.frames) frames.push_back({{"cell", frame.cell}, {"duration_ms", frame.duration_ms}});
      clips[name] = {{"loop", clip.loop}, {"frames", std::move(frames)}};
    }
    return {{"image", asset.image}, {"frame_width", asset.frame_width}, {"frame_height", asset.frame_height}, {"clips", std::move(clips)}};
  }

  namespace detail {
    inline unsigned sprite_clip_cell_at(const SpriteClip &clip, uint64_t elapsed_ms,
                                        const PreparedFrameTimes *prepared = nullptr) {
      if(clip.frames.empty()) return 0;
      const auto index = timed_frame_index(clip.frames, elapsed_ms, clip.loop, prepared);
      return index == clip.frames.size() ? clip.frames.back().cell : clip.frames[index].cell;
    }
  }

  // Elapsed time is relative to the start of the clip. Empty clips draw cell 0.
  inline unsigned sprite_cell_at(const SpriteClip &clip, uint64_t elapsed_ms) {
    return detail::sprite_clip_cell_at(clip, elapsed_ms);
  }
}
