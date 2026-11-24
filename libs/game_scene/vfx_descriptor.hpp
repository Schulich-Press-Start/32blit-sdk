#pragma once

#include "../world/sprite_clips.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace sdk::detail {
  inline constexpr int vfx_duration_limit = 3600000;
  inline constexpr int vfx_position_limit = 32768;
  inline constexpr unsigned vfx_cell_limit = 2048u * 2048u - 1;
  inline constexpr unsigned vfx_layer_limit = 64, vfx_cue_limit = 256;
  inline constexpr int vfx_text_limit = 256 * 1024;

  struct VfxLayer {
    std::string sprite, clip;
    unsigned cell = 0, start_ms = 0, clip_offset_ms = 0;
    int x = 0, y = 0;
  };

  struct VfxCue { unsigned at_ms; std::string cue; };

  struct VfxDescriptor {
    std::string name, audio;
    unsigned duration_ms = 600;
    std::vector<VfxLayer> layers;
    std::vector<VfxCue> cues;
  };

  inline bool vfx_has_suffix(std::string_view path, std::string_view suffix) {
    return path.size() > suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
  }

  inline bool vfx_label(std::string_view text) {
    return !text.empty() && text.size() <= 256 && text.find_first_not_of(' ') != text.npos &&
      std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
  }

  inline bool vfx_relative_path(std::string_view path) {
    if(path.empty() || path.size() > 1024 || path.find_first_of(":\\") != path.npos ||
       std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    for(;;) {
      const auto slash = path.find('/');
      const auto part = path.substr(0, slash);
      if(part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
      if(slash == path.npos) return true;
      path.remove_prefix(slash + 1);
    }
  }

  inline VfxDescriptor read_vfx_descriptor(const nlohmann::json &json) {
    using Json = nlohmann::json;
    const auto text = [](const Json &object, const char *key) -> const std::string & {
      const auto it = object.find(key);
      if(it == object.end() || !it->is_string()) throw std::runtime_error(std::string("Expected text for '") + key + "'.");
      return it->get_ref<const std::string &>();
    };
    const auto number = [](const Json &object, const char *key, int min, int max, bool optional = false) {
      const auto it = object.find(key);
      if(it == object.end() && optional) return 0;
      if(it != object.end() && it->is_number_integer() &&
         (!it->is_number_unsigned() || it->get<uint64_t>() <= static_cast<uint64_t>(max))) {
        const auto value = it->get<int64_t>();
        if(value >= min && value <= max) return static_cast<int>(value);
      }
      throw std::runtime_error(std::string("Expected '") + key + "' to be an integer from " +
                               std::to_string(min) + " to " + std::to_string(max) + ".");
    };
    if(!json.is_object()) throw std::runtime_error("Expected a VFX descriptor object.");
    VfxDescriptor effect;
    effect.name = text(json, "name");
    if(!vfx_label(effect.name)) throw std::runtime_error("Enter an effect name (1–256 bytes, no control characters).");
    effect.duration_ms = number(json, "duration_ms", 1, vfx_duration_limit);
    if(const auto layers = json.find("layers"); layers != json.end()) {
      if(!layers->is_array() || layers->size() > vfx_layer_limit) throw std::runtime_error("Expected up to 64 sprite layers.");
      effect.layers.reserve(layers->size());
      for(const auto &entry : *layers) {
        try {
          if(!entry.is_object()) throw std::runtime_error("Expected a sprite layer object.");
          VfxLayer layer;
          layer.sprite = text(entry, "sprite");
          if(!vfx_relative_path(layer.sprite) || vfx_has_suffix(layer.sprite, ".sprite.json"))
            throw std::runtime_error("Choose a project-relative sprite stem without .sprite.json.");
          if(entry.contains("clip") == entry.contains("cell")) throw std::runtime_error("Choose either 'clip' or 'cell'.");
          if(entry.contains("clip")) {
            layer.clip = text(entry, "clip");
            if(layer.clip.empty() || layer.clip.find('\0') != layer.clip.npos) throw std::runtime_error("Choose a named sprite clip.");
            layer.clip_offset_ms = number(entry, "clip_offset_ms", 0, vfx_duration_limit, true);
          } else {
            layer.cell = number(entry, "cell", 0, vfx_cell_limit);
            if(entry.contains("clip_offset_ms")) throw std::runtime_error("Only a clip can have a clip offset.");
          }
          layer.start_ms = number(entry, "start_ms", 0, static_cast<int>(effect.duration_ms) - 1, true);
          layer.x = number(entry, "x", -vfx_position_limit, vfx_position_limit, true);
          layer.y = number(entry, "y", -vfx_position_limit, vfx_position_limit, true);
          effect.layers.push_back(std::move(layer));
        } catch(const std::exception &error) {
          throw std::runtime_error("Layer " + std::to_string(effect.layers.size() + 1) + ": " + error.what());
        }
      }
    }
    if(json.contains("audio")) {
      effect.audio = text(json, "audio");
      if(!vfx_relative_path(effect.audio) || !vfx_has_suffix(effect.audio, ".audio.json"))
        throw std::runtime_error("Choose a project-relative .audio.json cue descriptor.");
    }
    if(const auto cues = json.find("cues"); cues != json.end()) {
      if(!cues->is_array() || cues->size() > vfx_cue_limit) throw std::runtime_error("Expected up to 256 audio cue bindings.");
      if(!cues->empty() && effect.audio.empty()) throw std::runtime_error("Choose an audio descriptor before binding cues.");
      effect.cues.reserve(cues->size());
      for(const auto &entry : *cues) {
        try {
          if(!entry.is_object()) throw std::runtime_error("Expected an audio cue binding object.");
          VfxCue cue{static_cast<unsigned>(number(entry, "at_ms", 0, effect.duration_ms)), text(entry, "cue")};
          if(cue.cue.empty() || cue.cue.find('\0') != cue.cue.npos) throw std::runtime_error("Choose a cue ID.");
          effect.cues.push_back(std::move(cue));
        } catch(const std::exception &error) {
          throw std::runtime_error("Cue " + std::to_string(effect.cues.size() + 1) + ": " + error.what());
        }
      }
    }
    return effect;
  }

  inline nlohmann::json write_vfx_descriptor(const VfxDescriptor &effect) {
    using Json = nlohmann::json;
    Json layers = Json::array(), cues = Json::array();
    for(const auto &layer : effect.layers) {
      Json value = {{"sprite", layer.sprite}, {"x", layer.x}, {"y", layer.y}, {"start_ms", layer.start_ms}};
      if(layer.clip.empty()) value["cell"] = layer.cell;
      else {
        value["clip"] = layer.clip;
        if(layer.clip_offset_ms) value["clip_offset_ms"] = layer.clip_offset_ms;
      }
      layers.push_back(std::move(value));
    }
    for(const auto &[at_ms, cue] : effect.cues) cues.push_back({{"at_ms", at_ms}, {"cue", cue}});
    Json value = {{"name", effect.name}, {"duration_ms", effect.duration_ms},
                  {"layers", std::move(layers)}, {"cues", std::move(cues)}};
    if(!effect.audio.empty()) value["audio"] = effect.audio;
    return value;
  }

  struct VfxGrid {
    unsigned width, height, columns, rows;
    unsigned count() const { return columns * rows; }
  };

  inline VfxGrid make_vfx_grid(const SpriteClips &sprite, unsigned width, unsigned height) {
    if(!width || !height || width % sprite.frame_width || height % sprite.frame_height)
      throw std::runtime_error("Frame dimensions must divide the PNG into complete cells.");
    const VfxGrid grid{sprite.frame_width, sprite.frame_height, width / sprite.frame_width, height / sprite.frame_height};
    for(const auto &[name, clip] : sprite.clips)
      for(const auto &frame : clip.frames)
        if(frame.cell >= grid.count()) throw std::runtime_error("Clip '" + name + "' has a cell outside the PNG.");
    return grid;
  }

  // Share one set of frame boundaries per referenced clip, not one copy per layer.
  class VfxAnimation {
  public:
    explicit VfxAnimation(const SpriteClip &clip) : clip_(clip), times_(clip.frames) {}

    unsigned cell_at(unsigned elapsed) const { return sprite_clip_cell_at(clip_, elapsed, &times_); }

  private:
    const SpriteClip &clip_; // Validated, nonempty clip owned by the cached SpriteClips.
    PreparedFrameTimes times_;
  };
  using VfxAnimations = std::map<std::string_view, VfxAnimation, std::less<>>;

  inline const VfxAnimation *vfx_clip(const VfxLayer &layer, const SpriteClips &sprite, const VfxGrid &grid,
                                       VfxAnimations &animations) {
    if(layer.clip.empty()) {
      if(layer.cell >= grid.count()) throw std::runtime_error("Cell is outside the PNG.");
      return nullptr;
    }
    const auto clip = sprite.clips.find(layer.clip);
    if(clip == sprite.clips.end()) throw std::runtime_error("No sprite clip named '" + layer.clip + "'.");
    return &animations.try_emplace(std::string_view(clip->first), clip->second).first->second;
  }

  inline unsigned vfx_layer_cell(const VfxLayer &layer, const VfxAnimation *clip, unsigned elapsed) {
    return clip ? clip->cell_at(elapsed - layer.start_ms + layer.clip_offset_ms) : layer.cell;
  }

  // Prepared outside playback. Sorting indices preserves authored order for simultaneous cues.
  class VfxTimeline {
  public:
    void prepare(const VfxDescriptor &effect) {
      duration_ = effect.duration_ms;
      order_.resize(effect.cues.size());
      std::iota(order_.begin(), order_.end(), size_t(0));
      std::stable_sort(order_.begin(), order_.end(), [&](size_t a, size_t b) { return effect.cues[a].at_ms < effect.cues[b].at_ms; });
      stop();
    }

    void start(uint32_t now) {
      previous_ = now;
      elapsed_ = 0;
      next_ = 0;
      playing_ = true;
    }

    template<class Play>
    void update(uint32_t now, const VfxDescriptor &effect, Play play) {
      if(!playing_) return;
      const uint32_t delta = now - previous_; // Also correct across the engine timestamp's uint32_t wrap.
      if(delta > UINT32_MAX / 2) return;      // Ignore a stale timestamp rather than jumping to the end.
      previous_ = now;
      elapsed_ += std::min<unsigned>(delta, duration_ - elapsed_);
      while(next_ < order_.size() && effect.cues[order_[next_]].at_ms <= elapsed_)
        play(effect.cues[order_[next_++]].cue);
      if(elapsed_ == duration_) stop();
    }

    void stop() { playing_ = false; }
    bool playing() const { return playing_; }
    unsigned elapsed() const { return elapsed_; }

  private:
    std::vector<size_t> order_;
    size_t next_ = 0;
    unsigned elapsed_ = 0, duration_ = 0;
    uint32_t previous_ = 0;
    bool playing_ = false;
  };
}
