#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#include <stb_image.h>

#include "game_scene.hpp"
#include "scene_text.hpp"
#include "sprite_render.hpp"
#include "tileset_descriptor.hpp"
#ifdef BLIT_DEV_BRIDGE
#include "bridge.hpp"
#endif

namespace sdk {
  namespace {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    constexpr char manifest[] = "blit-project.json";
    constexpr size_t asset_limit = 16 * 1024 * 1024; // Same file and PNG limits as the desktop editor.

    struct FileError : std::runtime_error {
      FileError(std::string_view path, const std::string &reason, uint32_t line = 0)
        : std::runtime_error(reason), path(path), line(line) {}
      std::string path;
      uint32_t line;
    };

    fs::path executable_directory() {
#ifdef _WIN32
      std::wstring buffer(260, L'\0');
      for(;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if(!length) return {};
        if(length < buffer.size()) {
          buffer.resize(length);
          return fs::canonical(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
      }
#elif defined(__APPLE__)
      uint32_t size = 0;
      _NSGetExecutablePath(nullptr, &size);
      std::vector<char> path(size);
      if(size && _NSGetExecutablePath(path.data(), &size) == 0) return fs::canonical(path.data()).parent_path();
      return {};
#elif defined(__linux__)
      std::error_code error;
      const auto path = fs::canonical("/proc/self/exe", error);
      return error ? fs::path{} : path.parent_path();
#else
      return {};
#endif
    }

    bool inside(const fs::path &root, const fs::path &absolute) {
      const auto relative = absolute.lexically_relative(root);
      return !relative.empty() && *relative.begin() != "..";
    }

    fs::path project_root() {
      const auto cwd = fs::canonical(fs::current_path());
      const auto exe = executable_directory();
      const bool working_project = fs::exists(cwd / manifest);
      // Editor Play/Dev runs a project-local executable with the authored files as cwd.
      if(working_project && !exe.empty() && inside(cwd, exe)) return cwd;
      if(!exe.empty() && fs::exists(exe / "data" / manifest)) return fs::canonical(exe / "data");
      if(working_project) return cwd;
      throw FileError(manifest, "Open a project folder or keep data/ beside the game executable.");
    }

    // Resolve existing symlinks as well as lexical segments before reading anything.
    // A missing file can still be resolved, so only an absent companion map is optional.
    fs::path project_file(const fs::path &root, std::string_view name) {
      if(name.empty() || name.find_first_of(":\\") != name.npos ||
         std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw FileError(name, "Expected a project-relative path using /.");
      const auto requested = fs::u8path(name);
      if(requested.has_root_path() || std::find(requested.begin(), requested.end(), "..") != requested.end())
        throw FileError(name, "Path must stay inside the project.");
      std::error_code error;
      const auto path = fs::weakly_canonical(root / requested, error);
      if(error || !inside(root, path)) throw FileError(name, "Cannot resolve this path inside the project.");
      return path;
    }

    std::string read_file(const fs::path &path, std::string_view name, size_t limit = asset_limit) {
      if(!fs::is_regular_file(path)) throw FileError(name, "File is missing or is not a regular file.");
      std::ifstream input(path, std::ios::binary | std::ios::ate);
      const auto size = static_cast<std::streamoff>(input.tellg());
      if(size < 0) throw FileError(name, "Cannot open file.");
      if(static_cast<uintmax_t>(size) > limit) throw FileError(name, "File exceeds the supported size limit.");
      std::string bytes(static_cast<size_t>(size), '\0');
      if(!input.seekg(0).read(bytes.data(), static_cast<std::streamsize>(size))) throw FileError(name, "Cannot read file.");
      return bytes;
    }

    Json read_json(const fs::path &root, std::string_view name) {
      const auto text = read_file(project_file(root, name), name);
      auto json = Json::parse(text, nullptr, false);
      if(json.is_discarded()) throw FileError(name, "Invalid JSON; check this file's syntax.");
      return json;
    }

    const std::string &path_field(const Json &json, const char *field, std::string_view file) {
      const auto value = json.find(field);
      if(value == json.end() || !value->is_string() || value->get_ref<const std::string &>().empty())
        throw FileError(file, std::string("Expected a nonempty path in '") + field + "'.");
      return value->get_ref<const std::string &>();
    }

    struct FreePixels { void operator()(stbi_uc *p) const { stbi_image_free(p); } };
    using Pixels = std::unique_ptr<stbi_uc, FreePixels>;
    struct Sheet {
      Sheet(Pixels pixels, int width, int height)
        : pixels(std::move(pixels)), surface(this->pixels.get(), blit::PixelFormat::RGBA, {width, height}) {}
      Pixels pixels;
      blit::Surface surface; // borrows pixels; neither the surface nor its data is copied
    };
    struct SpriteArt { SpriteClips clips; blit::Surface *sheet = nullptr; };

    struct Art {
      std::map<std::pair<fs::path, int>, Sheet> images;
      std::map<std::string, SpriteArt, std::less<>> sprites;
      std::unique_ptr<blit::Map> map;
      std::vector<blit::MapLayer *> layers;
      TilesetDescriptor::Properties tile_properties;
      blit::Surface *atlas = nullptr;

      bool tile_has_property(size_t index, const TilesetDescriptor::Assignments &flag) const {
        for(const auto *layer : layers)
          if(flag[layer->tiles[index]]) return true;
        return false;
      }

      blit::Surface &image(const fs::path &root, const std::string &name, int color_key = -1) {
        auto key = std::make_pair(project_file(root, name), color_key);
        if(const auto it = images.find(key); it != images.end()) return it->second.surface;
        const auto bytes = read_file(key.first, name);
        int w, h;
        Pixels pixels(stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
            static_cast<int>(bytes.size()), &w, &h, nullptr, STBI_rgb_alpha));
        if(!pixels) throw FileError(name, "Cannot decode PNG (maximum 2048 x 2048 pixels).");
        if(color_key >= 0) {
          for(auto *p = pixels.get(), *end = p + 4 * w * h; p != end; p += 4)
            if(((p[0] << 16) | (p[1] << 8) | p[2]) == color_key) p[3] = 0;
        }
        return images.try_emplace(std::move(key), std::move(pixels), w, h).first->second.surface;
      }

      void load_map(const fs::path &root, const char *scene) {
        const auto name = fs::u8path(scene).replace_extension(".map").generic_u8string();
        const auto path = project_file(root, name);
        if(!fs::exists(fs::symlink_status(path))) return;
        const auto bytes = read_file(path, name);
        map = blit::load_tiled_map(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
        if(!map) throw FileError(name, "Invalid native tilemap; expected a BTMP v1 or v2 file.");
        const auto sidecar = name + ".json";
        const auto association = read_json(root, sidecar);
        const auto &descriptor = path_field(association, "tileset", sidecar);
        constexpr std::string_view suffix = ".tileset.json";
        if(descriptor.size() <= suffix.size() || descriptor.compare(descriptor.size() - suffix.size(), suffix.size(), suffix) != 0)
          throw FileError(sidecar, "The tileset path must name a .tileset.json descriptor.");
        TilesetDescriptor tileset;
        if(const char *error = read_tileset_descriptor(read_json(root, descriptor), tileset)) throw FileError(descriptor, error);
        int key = -1;
        if(!tileset.transparent.empty()) {
          const auto &text = tileset.transparent;
          const auto end = text.data() + text.size();
          const auto parsed = std::from_chars(text.data(), end, key, 16);
          if(text.size() != 6 || parsed.ec != std::errc{} || parsed.ptr != end || key < 0)
            throw FileError(descriptor, "Expected six hexadecimal digits for transparent RGB.");
        }
        atlas = &image(root, tileset.image, key);
        const int count = blit::atlas_tile_count(atlas->bounds.w, atlas->bounds.h, tileset.tile_size);
        if(!count || !map->set_tile_size(tileset.tile_size))
          throw FileError(descriptor, "Tile size must fit the map and divide the PNG into at most 256 square cells.");
        layers.reserve(map->layer_order.size());
        for(const auto &layer_name : map->layer_order) {
          auto &layer = map->layers.at(layer_name);
          if(std::any_of(layer.tiles.begin(), layer.tiles.end(), [count](uint8_t id) { return id > count; }))
            throw FileError(name, "Layer '" + layer_name + "' uses a tile outside its atlas.");
          layers.push_back(&layer);
        }
        tile_properties = std::move(tileset.properties);
      }

      const SpriteArt &sprite(const fs::path &root, std::string_view stem) {
        if(const auto it = sprites.find(stem); it != sprites.end()) return it->second;
        const auto name = std::string(stem) + ".sprite.json";
        auto &art = sprites.try_emplace(std::string(stem)).first->second;
        if(const char *error = read_sprite_clips(read_json(root, name), art.clips)) throw FileError(name, error);
        art.sheet = &image(root, art.clips.image);
        const auto w = art.sheet->bounds.w, h = art.sheet->bounds.h;
        if(w % art.clips.frame_width || h % art.clips.frame_height)
          throw FileError(name, "Frame dimensions must divide the PNG into complete cells.");
        const auto cells = (w / art.clips.frame_width) * (h / art.clips.frame_height);
        for(const auto &[clip_name, clip] : art.clips.clips)
          for(const auto &frame : clip.frames)
            if(frame.cell >= cells) throw FileError(name, "Clip '" + clip_name + "' has a cell outside the PNG.");
        return art;
      }

      void load_sprites(const fs::path &root, const SceneDocument &doc) {
        // Stage actual selections, not isolated lines: an instance can inherit the
        // asset but override its clip. Unplaced prefabs must also be ready to spawn.
        struct Selection { std::string_view asset, clip; };
        const auto &type = *component_info<Sprite>();
        auto resolve = [&](const SceneObject &object, Selection selected) {
          for(auto c = object.first_component; c != scene_end; c = doc.component(c).next) {
            const auto &component = doc.component(c);
            if(component.type != type.id) continue;
            for(auto f = component.first_field; f != scene_end; f = doc.field(f).next) {
              const auto &field = doc.field(f);
              switch(type.fields[field.field].offset) {
                case offsetof(Sprite, asset): selected.asset = field.value; break;
                case offsetof(Sprite, clip): selected.clip = field.value; break;
              }
            }
          }
          if(!selected.asset.empty()) {
            const auto &clips = sprite(root, selected.asset).clips.clips;
            if(!selected.clip.empty() && clips.find(selected.clip) == clips.end())
              throw FileError(doc.sources()[object.source].path, "Unknown clip '" + std::string(selected.clip) +
                              "' for sprite '" + std::string(selected.asset) + "'.", object.line);
          }
          return selected;
        };
        std::map<std::string_view, Selection> prefabs;
        for(const auto &object : doc.objects())
          if(object.prefab) prefabs.emplace(object.key, resolve(object, {}));
        for(const auto &object : doc.objects())
          if(!object.prefab) resolve(object, *object.base ? prefabs.at(object.base) : Selection{});
      }
    };

    // Visit tile indices in sweep order so both slab times are monotone.
    // Use the same float boundaries as the tile Box passed to sweep_box.
    struct TileSweepAxis {
      double lo, hi, velocity;
      int size, count;

      int cell(int index) const { return velocity < 0 ? count - 1 - index : index; }
      float edge(int index) const { return float(index * size); }

      double entry(int index) const {
        if(velocity > 0) return (edge(index) - hi) / velocity;
        if(velocity < 0) return (edge(cell(index) + 1) - lo) / velocity;
        return edge(index) < hi ? -infinity : infinity;
      }

      double exit(int index) const {
        if(velocity > 0) return (edge(index + 1) - lo) / velocity;
        if(velocity < 0) return (edge(cell(index)) - hi) / velocity;
        return lo < edge(index + 1) ? infinity : -infinity;
      }

      int first_at(double fraction) const {
        int first = 0, last = count;
        while(first < last) {
          const int middle = first + (last - first) / 2;
          if(exit(middle) < fraction) first = middle + 1;
          else last = middle;
        }
        return first;
      }

      static constexpr double infinity = std::numeric_limits<double>::infinity();
    };

    SceneLimits limits_for(uint16_t capacity) {
      SceneLimits limits;
      limits.objects = std::max<uint32_t>(limits.objects, capacity);
      limits.components = std::max(limits.components, capacity * 5u);
      limits.fields = std::max(limits.fields, capacity * 14u);
      return limits;
    }

    bool report_travel_error(const SceneError &error) {
      std::fputs("GameScene::travel: ", stderr);
      if(*error.file) {
        std::fputs(error.file, stderr);
        if(error.line) std::fprintf(stderr, ":%u", error.line);
        std::fputs(": ", stderr);
      }
      std::fprintf(stderr, "%s\n", error.message);
      return false;
    }

    void show_error(std::string_view text, int &y) {
      // Slice the stored diagnostic rather than allocating wrapped text every frame.
      auto &screen = blit::screen;
      const auto &font = blit::minimal_font;
      const auto columns = static_cast<size_t>(std::max(1, (screen.bounds.w - 16) / (font.char_w + 1)));
      while(!text.empty() && y < screen.bounds.h) {
        const auto part = text.substr(0, columns);
        screen.text(part, font, blit::Point(8, y));
        y += font.char_h + font.spacing_y + 1;
        text.remove_prefix(part.size());
      }
    }
  }

  struct GameScene::Player {
    struct Travel {
      char actor[string_field_size], arrival[string_field_size];

      static bool name(char (&out)[string_field_size], std::string_view value) {
        return !value.empty() && value.size() < sizeof(out) && value.find('\0') == value.npos &&
               detail::scene_copy(out, sizeof(out), value);
      }

      static bool require_transform(const SceneDocument &doc, const SceneObject *object, const char *name,
                                    const char *role, SceneError &error) {
        char message[sizeof(error.message)];
        if(!object) {
          std::snprintf(message, sizeof(message), "No %s entity named '%s' (match the destination Name).", role, name);
          return detail::scene_error(error, doc.sources().front().path, 0, message);
        }
        const auto id = component_info<Transform2D>()->id;
        const auto has_transform = [&](const SceneObject &placement) {
          for(auto c = placement.first_component; c != scene_end; c = doc.component(c).next)
            if(doc.component(c).type == id) return true;
          return false;
        };
        if(has_transform(*object)) return true;
        if(*object->base) {
          for(const auto &base : doc.objects())
            if(base.prefab && std::strcmp(base.key, object->base) == 0 && has_transform(base)) return true;
        }
        std::snprintf(message, sizeof(message), "The %s entity '%s' needs Transform2D.", role, name);
        return detail::scene_error(error, doc.sources()[object->source].path, object->line, message);
      }

      bool prepare(const SceneDocument &doc, SceneError &error) const {
        const SceneObject *mover = nullptr, *marker = nullptr;
        for(const auto &object : doc.objects()) {
          if(object.prefab) continue;
          if(!mover && std::strcmp(object.name, actor) == 0) mover = &object;
          if(!marker && std::strcmp(object.name, arrival) == 0) marker = &object;
          if(mover && marker) break;
        }
        return require_transform(doc, mover, actor, "actor", error) &&
               require_transform(doc, marker, arrival, "arrival", error);
      }

      void apply(World &world) const {
        // prepare() checked both placements before SceneRuntime committed the load.
        auto &mover = *world.get<Transform2D>(world.find(actor));
        const auto &marker = *world.get<Transform2D>(world.find(arrival));
        mover.x = marker.x;
        mover.y = marker.y;
      }
    };

    struct DrawItem {
      int32_t layer;
      uint32_t sequence;
      Entity sprite; // Invalid for tiles; their sequence is the map layer index.

      bool operator<(const DrawItem &other) const {
        return layer != other.layer ? layer > other.layer : sequence < other.sequence;
      }
    };

    // Constructed in open(), after all C++ component registrars have run.
    ComponentPool<Name> names;
    ComponentPool<Transform2D> transforms;
    ComponentPool<Sprite> sprites;
    ComponentPool<BoxCollider2D> colliders;
    ComponentPool<Camera2D> cameras;
    SceneRuntime scenes;
    std::vector<uint32_t> placed_generation; // distinguish placed entities from runtime additions
    std::vector<DrawItem> draw_items;
    fs::path root;
    std::deque<std::string> sources; // SceneReader borrows each source until finish().
    std::unique_ptr<Art> active, staged;
    void (*on_loaded)() = nullptr;
    const Travel *pending_travel = nullptr; // Scoped to a travel load; cleared by finish().
    SceneError diagnostic{};

    explicit Player(World &world) : names(world, world.capacity()), transforms(world, world.capacity()),
      sprites(world, world.capacity()), colliders(world, world.capacity()),
      cameras(world, world.capacity()),
      scenes(world, limits_for(world.capacity()), {this, read, finish, prepare}), placed_generation(world.capacity()) {}

    static bool read(void *context, std::string_view path, std::string_view &text) {
      auto &p = *static_cast<Player *>(context);
      try {
        text = p.sources.emplace_back(read_file(project_file(p.root, path), path, 1024 * 1024));
        return true;
      } catch(const std::exception &e) {
        detail::scene_error(p.diagnostic, path, 0, e.what());
        if(!p.pending_travel) std::fprintf(stderr, "%s: %s\n", p.diagnostic.file, p.diagnostic.message);
        return false;
      }
    }

    static bool prepare(void *context, const SceneDocument &doc, SceneError &error) {
      auto &p = *static_cast<Player *>(context);
      if(p.pending_travel && !p.pending_travel->prepare(doc, error)) return false;
      try {
        p.staged = std::make_unique<Art>();
        p.staged->load_map(p.root, doc.sources().front().path);
        p.staged->load_sprites(p.root, doc);
        // Size the queue before committing artwork so rendering never grows it.
        p.draw_items.reserve(p.sprites.capacity() + p.staged->layers.size());
        return true;
      } catch(const FileError &e) {
        detail::scene_error(error, e.path, e.line, e.what());
      } catch(const std::exception &e) {
        detail::scene_error(error, doc.sources().front().path, 0, e.what());
      }
      p.diagnostic = error;
      return false;
    }

    static void finish(void *context, bool success) noexcept {
      auto &p = *static_cast<Player *>(context);
      const auto *travel = p.pending_travel;
      p.pending_travel = nullptr;
      p.sources.clear();
      if(success) p.active.swap(p.staged);
      p.staged.reset();
      if(success && travel) travel->apply(p.scenes.world());
      if(success && p.on_loaded) p.on_loaded();
    }
  };

  GameScene::GameScene(uint16_t capacity) : world(capacity) {}
  GameScene::~GameScene() = default;

  bool GameScene::open(void (*on_loaded)()) {
    if(!player_) {
      player_ = std::make_unique<Player>(world);
#ifdef BLIT_DEV_BRIDGE
      bridge_attach(world, &player_->scenes);
#endif
    }
    auto &p = *player_;
    auto old_root = std::move(p.root);
    const auto old_callback = p.on_loaded;
    p.on_loaded = on_loaded;
    p.diagnostic = {};
    try {
      p.root = project_root();
      const auto config = read_json(p.root, manifest);
      const auto &scene = path_field(config, "scene", manifest);
      SceneError error;
      if(p.scenes.load(scene, error)) return true;
      if(!*p.diagnostic.message) p.diagnostic = error;
    } catch(const FileError &e) {
      detail::scene_error(p.diagnostic, e.path, e.line, e.what());
    } catch(const std::exception &e) {
      detail::scene_error(p.diagnostic, manifest, 0, e.what());
    }
    if(p.active) {
      p.root = std::move(old_root);
      p.on_loaded = old_callback;
    }
    std::fprintf(stderr, "%s:%u: %s\n", p.diagnostic.file, p.diagnostic.line, p.diagnostic.message);
    return false;
  }

  std::string GameScene::project_asset_path(std::string_view project_relative) const {
    if(!player_ || !player_->active) return {};
    try {
      return project_file(player_->root, project_relative).u8string();
    } catch(const std::exception &e) {
      std::fputs("GameScene::project_asset_path: ", stderr);
      if(!project_relative.empty()) {
        std::fwrite(project_relative.data(), 1, project_relative.size(), stderr);
        std::fputs(": ", stderr);
      }
      std::fprintf(stderr, "%s\n", e.what());
      return {};
    }
  }

  bool GameScene::travel(std::string_view scene, std::string_view actor, std::string_view arrival) {
    SceneError error;
    const auto fail = [&](std::string_view message) {
      detail::scene_error(error, scene, 0, message);
      return report_travel_error(error);
    };
    if(!player_ || !player_->active) return fail("Call GameScene::open() successfully before travelling.");
    auto &p = *player_;
    if(p.pending_travel) return fail("Cannot travel during another travel load.");

    // The views can point into a Door or Name in the world being replaced.
    constexpr std::string_view suffix = ".scene";
    char path[scene_path_size];
    if(scene.size() <= suffix.size() || scene.substr(scene.size() - suffix.size()) != suffix ||
       !detail::scene_copy(path, sizeof(path), scene))
      return fail("Expected a project-relative .scene path.");
    Player::Travel request;
    if(!Player::Travel::name(request.actor, actor)) return fail("Expected a nonempty actor Name that fits a string field.");
    if(!Player::Travel::name(request.arrival, arrival)) return fail("Expected a nonempty arrival Name that fits a string field.");

    p.diagnostic = {};
    p.pending_travel = &request;
    if(p.scenes.load({path, scene.size()}, error)) return true;
    return report_travel_error(*p.diagnostic.message ? p.diagnostic : error);
  }

  blit::Size GameScene::map_size() const {
    if(!player_ || !player_->active || !player_->active->map) return {};
    return player_->active->map->pixel_size();
  }

  int GameScene::tile_size() const {
    if(!player_ || !player_->active || !player_->active->map) return 0;
    return player_->active->map->tile_size();
  }

  bool GameScene::tile_has_property(blit::Point cell, std::string_view name) const {
    if(!player_ || !player_->active || !player_->active->map) return false;
    const auto &art = *player_->active;
    const auto index = art.map->tile_index(cell);
    if(index < 0) return false;
    const auto property = art.tile_properties.find(name);
    return property != art.tile_properties.end() && art.tile_has_property(index, property->second);
  }

  TileMoveResult GameScene::move_against_tiles(Transform2D &position, const BoxCollider2D &shape,
                                                blit::Vec2 delta, std::string_view property, bool block_edges) const {
    if(!player_ || !player_->active || !player_->active->map) return {};
    const auto &art = *player_->active;
    const auto &map = *art.map;
    const auto flag = art.tile_properties.find(property);
    return sdk::move_against_tiles(position, shape, delta, map.bounds.w, map.bounds.h, map.tile_size(),
      [&](blit::Point cell) {
        return flag != art.tile_properties.end() &&
               art.tile_has_property(size_t(cell.y) * map.bounds.w + cell.x, flag->second);
      }, block_edges);
  }

  SceneSweepHit GameScene::sweep(Box moving, blit::Vec2 delta, CollisionMask mask, Entity ignore, bool solid_tiles) {
    SceneSweepHit best;
    if(!(moving.left < moving.right && moving.top < moving.bottom) ||
       !std::isfinite(delta.x) || !std::isfinite(delta.y)) return best;

    if(const auto actor = sweep_entities(world, moving, delta, mask, ignore); actor.entity)
      best = {SceneHitKind::Entity, actor.entity, {}, actor.contact};
    if(!solid_tiles || !player_ || !player_->active || !player_->active->map) return best;

    const auto &art = *player_->active;
    const auto solid = art.tile_properties.find("solid");
    if(solid == art.tile_properties.end() || solid->second.none()) return best;

    const auto &map = *art.map;
    const TileSweepAxis xs{moving.left, moving.right, delta.x, map.tile_size(), map.bounds.w};
    const TileSweepAxis ys{moving.top, moving.bottom, delta.y, map.tile_size(), map.bounds.h};
    // Only visit columns reachable while the moving box is in this row and
    // no later than the best contact. Include exact endpoint and tie times.
    for(int row = ys.first_at(0); row < ys.count; ++row) {
      const double from = std::max(0.0, ys.entry(row));
      if(from > best.contact.fraction) break;
      const double until = ys.exit(row);
      const int y = ys.cell(row);
      for(int column = xs.first_at(from); column < xs.count; ++column) {
        if(xs.entry(column) > std::min(until, best.contact.fraction)) break;
        const int x = xs.cell(column);
        if(!art.tile_has_property(size_t(y) * map.bounds.w + x, solid->second)) continue;

        BoxSweepHit contact;
        const Box tile{xs.edge(x), ys.edge(y), xs.edge(x + 1), ys.edge(y + 1)};
        if(sweep_box(moving, delta, tile, contact) && contact.fraction <= best.contact.fraction &&
           (best.kind != SceneHitKind::Tile || contact.fraction < best.contact.fraction)) {
          best = {SceneHitKind::Tile, {}, {x, y}, contact};
          if(contact.fraction == 0) return best;
        }
      }
    }
    return best;
  }

  bool GameScene::camera_origin(const char *name, blit::Point &origin) const {
    if(!player_ || !player_->active) return false;
    const auto entity = world.find(name);
    const auto *camera = world.get<Camera2D>(entity);
    if(!camera) return false;
    const auto *center = world.get<Transform2D>(camera->follow);
    if(!center) center = world.get<Transform2D>(entity);
    if(!center || !std::isfinite(center->x) || !std::isfinite(center->y)) return false;

    const auto screen = blit::screen.bounds;
    const auto map = camera->clamp_to_map ? map_size() : blit::Size{};
    const auto axis = [](float center, int view_extent, int map_extent) {
      const double offset = std::floor(double(center) - view_extent * .5);
      if(map_extent > 0) return std::clamp(offset, 0.0, double(std::max(0, map_extent - view_extent)));
      return offset;
    };
    const double x = axis(center->x, screen.w, map.w), y = axis(center->y, screen.h, map.h);
    if(x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max() - screen.w ||
       y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max() - screen.h) return false;
    origin = {int(x), int(y)};
    return true;
  }

  bool GameScene::draw_from_camera(uint32_t time, const char *name) {
    blit::Point origin;
    return camera_origin(name, origin) && draw(time, origin);
  }

  bool GameScene::draw(uint32_t time, blit::Point world_origin) {
    auto &screen = blit::screen;
    if(!player_ || !player_->active) {
      screen.pen = blit::Pen(27, 28, 35);
      screen.clear();
      screen.pen = blit::Pen(255, 229, 170);
      int y = 8;
      show_error("Cannot play this scene", y);
      if(player_) {
        const auto &error = player_->diagnostic;
        if(error.line) {
          char location[scene_path_size + 12];
          std::snprintf(location, sizeof(location), "%s:%u", error.file, error.line);
          show_error(location, y);
        } else show_error(error.file, y);
        show_error(player_->diagnostic.message, y);
      } else show_error("Call GameScene::open() in init().", y);
      return false;
    }
    auto &p = *player_;
    const auto &art = *p.active;
    auto &items = p.draw_items;
    const auto enqueue = [&](int32_t layer, Entity sprite) {
      items.push_back({layer, static_cast<uint32_t>(items.size()), sprite});
    };
    // This insertion order breaks ties: tile document order, authored entities,
    // then the existing World query order for sprites added at runtime.
    for(const auto *layer : art.layers) enqueue(layer->layer, {});
    uint16_t placed = 0;
    p.scenes.each_placement([&](Entity e) {
      ++placed;
      p.placed_generation[e.index()] = e.generation();
      if(const auto *sprite = p.sprites.get(e))
        if(p.transforms.get(e)) enqueue(sprite->layer, e);
    });
    if(placed < world.size()) world.each<Sprite, Transform2D>([&](Entity e, const Sprite &sprite, const Transform2D &) {
      if(p.placed_generation[e.index()] != e.generation()) enqueue(sprite.layer, e);
    });
    if(!std::is_sorted(items.begin(), items.end())) std::sort(items.begin(), items.end());

    const auto clip = screen.clip;
    const blit::Rect viewport(world_origin.x + clip.x, world_origin.y + clip.y, clip.w, clip.h);
    for(const auto &item : items) {
      if(!item.sprite) {
        art.layers[item.sequence]->draw(screen, *art.atlas, viewport, {clip.x, clip.y});
      } else {
        const auto &sprite = *p.sprites.get(item.sprite);
        const auto found = art.sprites.find(sprite.asset);
        if(found != art.sprites.end()) {
          const auto &position = *p.transforms.get(item.sprite);
          draw_sprite(screen, *found->second.sheet, found->second.clips, position, sprite, world_origin, time);
        }
      }
    }
    items.clear();
    return true;
  }
}
