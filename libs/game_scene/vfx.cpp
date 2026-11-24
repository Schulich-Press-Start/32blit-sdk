#include "vfx.hpp"
#include "audio_cues.hpp"
#include "game_scene.hpp"
#include "vfx_descriptor.hpp"
#include "../world/audio_descriptor.hpp"
#include <limits>
#include <map>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#include <stb_image.h>

namespace sdk {
  namespace {
    namespace fs = std::filesystem;
    using namespace detail;

    fs::path asset_path(const GameScene &game, std::string_view name) {
      const auto resolved = game.project_asset_path(name);
      if(resolved.empty()) throw std::runtime_error("Cannot resolve this path inside the open project.");
      const auto path = fs::u8path(resolved);
      if(!fs::is_regular_file(path)) throw std::runtime_error("Expected a regular project file.");
      return path;
    }

    template<class Load>
    auto with_path(std::string_view name, Load load) {
      try { return load(); }
      catch(const std::exception &error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
    }

    nlohmann::json read_json(const GameScene &game, std::string_view name, int limit) {
      const auto bytes = read_vfx_file<std::string>(asset_path(game, name), limit);
      return nlohmann::json::parse(bytes);
    }

    struct FreePixels { void operator()(stbi_uc *data) const { stbi_image_free(data); } };
    using Pixels = std::unique_ptr<stbi_uc, FreePixels>;
    struct Sheet {
      Sheet(Pixels pixels, int width, int height)
        : pixels(std::move(pixels)), surface(this->pixels.get(), blit::PixelFormat::RGBA, {width, height}) {}
      Pixels pixels;
      blit::Surface surface;
    };

    struct SpriteArt {
      SpriteClips sprite;
      blit::Surface *sheet;
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
    std::map<fs::path, Sheet> sheets;
    std::map<std::string, SpriteArt, std::less<>> sprites;
    std::vector<LayerArt> layers;
    AudioCues audio;
    blit::Point position;

    blit::Surface &image(const GameScene &game, const std::string &name) {
      return *with_path(name, [&] {
        auto path = asset_path(game, name);
        const auto it = sheets.find(path);
        if(it != sheets.end()) return &it->second.surface;
        const auto bytes = read_vfx_file<std::vector<uint8_t>>(path, vfx_image_limit);
        int width, height;
        Pixels pixels(stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, nullptr, STBI_rgb_alpha));
        if(!pixels) throw std::runtime_error("Cannot decode PNG (maximum 2048 x 2048 pixels).");
        return &sheets.try_emplace(std::move(path), std::move(pixels), width, height).first->second.surface;
      });
    }

    SpriteArt &sprite(const GameScene &game, const std::string &stem) {
      const auto it = sprites.find(stem);
      if(it != sprites.end()) return it->second;
      const auto path = stem + ".sprite.json";
      return *with_path(path, [&] {
        SpriteClips descriptor;
        if(const char *error = read_sprite_clips(read_json(game, path, vfx_image_limit), descriptor)) throw std::runtime_error(error);
        auto &sheet = image(game, descriptor.image);
        const auto grid = make_vfx_grid(descriptor, sheet.bounds.w, sheet.bounds.h);
        return &sprites.emplace(stem, SpriteArt{std::move(descriptor), &sheet, grid, {}}).first->second;
      });
    }

    void load(std::string_view path, const GameScene &game) {
      effect = read_vfx_descriptor(read_json(game, path, vfx_text_limit));
      layers.reserve(effect.layers.size());
      for(const auto &layer : effect.layers) {
        try {
          auto &art = sprite(game, layer.sprite);
          layers.push_back({&art, vfx_clip(layer, art.sprite, art.grid, art.animations)});
        } catch(const std::exception &error) {
          throw std::runtime_error("Layer " + std::to_string(layers.size() + 1) + ": " + error.what());
        }
      }
      if(!effect.cues.empty()) {
        with_path(effect.audio, [&] {
          auto text = read_vfx_file<std::string>(asset_path(game, effect.audio), vfx_text_limit);
          const auto json = parse_audio_descriptor(text);
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
      target.blit(resolved.art->sheet, {int(cell % grid.columns * grid.width), int(cell / grid.columns * grid.height),
                                      int(grid.width), int(grid.height)}, {int(x), int(y)});
    }
  }

  void Vfx::stop() { if(player_) player_->timeline.stop(); }
  bool Vfx::playing() const { return player_ && player_->timeline.playing(); }
  std::string_view Vfx::name() const { return player_ ? std::string_view(player_->effect.name) : std::string_view{}; }
}
