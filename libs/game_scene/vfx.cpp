#include "vfx.hpp"
#include "audio_cues.hpp"
#include "game_scene.hpp"
#include "project_assets.hpp"
#include "vfx_descriptor.hpp"
#include "../world/audio_descriptor.hpp"
#include <limits>
#include <map>


namespace sdk {
  namespace {
    using namespace detail;


    template<class Load>
    auto with_path(std::string_view name, Load load) {
      try { return load(); }
      catch(const std::exception &error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
    }

    struct SpriteArt {
      std::shared_ptr<const SpriteAsset> sprite;
      VfxGrid grid;
      VfxAnimations animations;
    };
    struct LayerArt {
      const SpriteArt *art;
      const VfxAnimation *clip;
    };
  }

  struct Vfx::Player {
    VfxDescriptor effect;
    VfxTimeline timeline;
    std::shared_ptr<ProjectAssets> assets;
    std::map<std::string, SpriteArt, std::less<>> sprites;
    std::vector<LayerArt> layers;
    AudioCues audio;
    blit::Point position;

    SpriteArt &sprite(const std::string &stem) {
      const auto it = sprites.find(stem);
      if(it != sprites.end()) return it->second;
      const auto path = stem + ".sprite.json";
      return *with_path(path, [&] {
        auto descriptor = assets->sprite(stem);
        const auto &sheet = descriptor->image->surface;
        const auto grid = make_vfx_grid(descriptor->clips, sheet.bounds.w, sheet.bounds.h);
        return &sprites.emplace(stem, SpriteArt{std::move(descriptor), grid, {}}).first->second;
      });
    }

    void load(std::string_view path, const GameScene &game) {
      assets = game.project_assets();
      if(!assets) throw std::runtime_error("No project is open.");
      effect = read_vfx_descriptor(*assets->json(path, vfx_text_limit));
      layers.reserve(effect.layers.size());
      for(const auto &layer : effect.layers) {
        try {
          auto &art = sprite(layer.sprite);
          layers.push_back({&art, vfx_clip(layer, art.sprite->clips, art.grid, art.animations)});
        } catch(const std::exception &error) {
          throw std::runtime_error("Layer " + std::to_string(layers.size() + 1) + ": " + error.what());
        }
      }
      if(!effect.cues.empty()) {
        const auto text = assets->text(effect.audio, vfx_text_limit);
        with_path(effect.audio, [&] {
          const auto json = parse_audio_descriptor(*text);
          for(const auto &cue : effect.cues)
            if(!json.contains(cue.cue)) throw std::runtime_error("Unknown audio cue ID: " + cue.cue);
          return true;
        });
        if(!audio.load(effect.audio, game)) throw std::runtime_error(audio.last_error());
      }
      timeline.prepare(effect);
    }
  };

  Vfx::Vfx() = default;
  Vfx::~Vfx() = default;

  bool Vfx::load(std::string_view path, const GameScene &game) {
    try {
      if(!vfx_relative_path(path) || !vfx_has_suffix(path, ".vfx.json"))
        throw std::runtime_error("Expected a project-relative .vfx.json path.");
      auto next = std::make_unique<Player>();
      next->load(path, game);
      player_ = std::move(next);
      last_error_.clear();
      return true;
    } catch(const std::exception &error) {
      last_error_ = std::string(path) + ": " + error.what();
      return false;
    }
  }

  bool Vfx::start(uint32_t now, blit::Point position) {
    if(!player_) {
      last_error_ = "No VFX descriptor is loaded.";
      return false;
    }
    last_error_.clear();
    player_->position = position;
    player_->timeline.start(now);
    update(now);
    return true;
  }

  void Vfx::update(uint32_t now) {
    if(!player_) return;
    player_->timeline.update(now, player_->effect, [&](const std::string &cue) {
      if(!player_->audio.play(cue)) last_error_ = "Cue '" + cue + "': " + player_->audio.last_error();
    });
  }

  void Vfx::draw(blit::Surface &target, blit::Point view_origin) const {
    if(!playing() || target.clip.w <= 0 || target.clip.h <= 0) return;
    const int64_t origin_x = int64_t(player_->position.x) - view_origin.x;
    const int64_t origin_y = int64_t(player_->position.y) - view_origin.y;
    const int64_t right = int64_t(target.clip.x) + target.clip.w, bottom = int64_t(target.clip.y) + target.clip.h;
    const auto elapsed = player_->timeline.elapsed();
    for(size_t i = 0; i < player_->layers.size(); ++i) {
      const auto &layer = player_->effect.layers[i];
      if(elapsed < layer.start_ms) continue;
      const auto &resolved = player_->layers[i];
      const auto &grid = resolved.art->grid;
      const int64_t x = origin_x + layer.x, y = origin_y + layer.y;
      if(x + grid.width <= target.clip.x || y + grid.height <= target.clip.y || x >= right || y >= bottom ||
         x > std::numeric_limits<int32_t>::max() || y > std::numeric_limits<int32_t>::max() ||
         x < std::numeric_limits<int32_t>::min() || y < std::numeric_limits<int32_t>::min()) continue;
      const auto cell = vfx_layer_cell(layer, resolved.clip, elapsed);
      target.blit(&resolved.art->sprite->image->surface, {int(cell % grid.columns * grid.width), int(cell / grid.columns * grid.height),
                                      int(grid.width), int(grid.height)}, {int(x), int(y)});
    }
  }

  void Vfx::stop() { if(player_) player_->timeline.stop(); }
  bool Vfx::playing() const { return player_ && player_->timeline.playing(); }
  std::string_view Vfx::name() const { return player_ ? std::string_view(player_->effect.name) : std::string_view{}; }
}
