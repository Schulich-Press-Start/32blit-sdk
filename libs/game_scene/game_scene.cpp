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
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>


#include "game_scene.hpp"
#include "project_assets.hpp"
#include "map_solidity.hpp"
#include "scene_text.hpp"
#include "sprite_preloads.hpp"
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


    const std::string &path_field(const Json &json, const char *field, std::string_view file) {
      const auto value = json.find(field);
      if(value == json.end() || !value->is_string() || value->get_ref<const std::string &>().empty())
        throw FileError(file, std::string("Expected a nonempty path in '") + field + "'.");
      return value->get_ref<const std::string &>();
    }


    struct Art {
      explicit Art(std::shared_ptr<ProjectAssets> assets) : assets(std::move(assets)) {}
      std::shared_ptr<ProjectAssets> assets;
      std::vector<std::shared_ptr<ImageAsset>> images;
      std::map<std::string, std::shared_ptr<const SpriteAsset>, std::less<>> sprites;
      std::unique_ptr<blit::Map> map;
      std::vector<blit::MapLayer *> layers;
      TilesetDescriptor::Properties tile_properties;
      std::vector<bool> solid_cells;
      std::vector<TileAnimation> tile_animations;
      blit::Surface *atlas = nullptr;
      struct Neighbor {
        std::string scene;
        blit::Point origin;
        std::unique_ptr<Art> art;
      };
      std::array<Neighbor, 4> neighbors;

      bool tile_has_property(size_t index, const TilesetDescriptor::Assignments *flag) const {
        if(!flag) return false;
        for(const auto *layer : layers)
          if((*flag)[layer->tiles[index]]) return true;
        return false;
      }

      const TilesetDescriptor::Assignments *tile_property(std::string_view name) const {
        const auto found = tile_properties.find(name);
        return found == tile_properties.end() ? nullptr : &found->second;
      }

      bool tile_is_solid(size_t index) const { return !solid_cells.empty() && solid_cells[index]; }

      blit::Surface &image(const fs::path &, const std::string &name, int color_key = -1) {
        auto resource = assets->image(name, color_key);
        for(const auto &loaded : images) if(loaded == resource) return loaded->surface;
        images.push_back(std::move(resource));
        return images.back()->surface;
      }

      void load_map(const fs::path &root, const char *scene, Art *image_cache = nullptr) {
        const auto name = fs::u8path(scene).replace_extension(".map").generic_u8string();
        const auto &path = assets->path(name);
        if(!fs::exists(fs::symlink_status(path))) return;
        const auto bytes = assets->text(name);
        map = blit::load_tiled_map(reinterpret_cast<const uint8_t *>(bytes->data()), bytes->size());
        if(!map) throw FileError(name, "Invalid native tilemap; expected a BTMP v1 or v2 file.");
        const auto sidecar = name + ".json";
        const auto association_resource = assets->json(sidecar);
        const auto &association = *association_resource;
        MapSolidity overrides;
        if(const char *error = read_map_solidity(association, map->bounds.w, map->bounds.h, overrides))
          throw FileError(sidecar, error);
        const auto &descriptor = path_field(association, "tileset", sidecar);
        constexpr std::string_view suffix = ".tileset.json";
        if(descriptor.size() <= suffix.size() || descriptor.compare(descriptor.size() - suffix.size(), suffix.size(), suffix) != 0)
          throw FileError(sidecar, "The tileset path must name a .tileset.json descriptor.");
        TilesetDescriptor tileset;
        if(const char *error = read_tileset_descriptor(*assets->json(descriptor), tileset)) throw FileError(descriptor, error);
        int key = -1;
        if(!tileset.transparent.empty()) {
          const auto &text = tileset.transparent;
          const auto end = text.data() + text.size();
          const auto parsed = std::from_chars(text.data(), end, key, 16);
          if(text.size() != 6 || parsed.ec != std::errc{} || parsed.ptr != end || key < 0)
            throw FileError(descriptor, "Expected six hexadecimal digits for transparent RGB.");
        }
        atlas = &(image_cache ? *image_cache : *this).image(root, tileset.image, key);
        const int count = blit::atlas_tile_count(atlas->bounds.w, atlas->bounds.h, tileset.tile_size);
        if(!count || !map->set_tile_size(tileset.tile_size))
          throw FileError(descriptor, "Tile size must fit the map and divide the PNG into at most 256 square cells.");
        if(!tile_animations_fit_atlas(tileset.animations, count))
          throw FileError(descriptor, "A tile animation uses a cell outside the PNG atlas.");
        layers.reserve(map->layer_order.size());
        for(const auto &layer_name : map->layer_order) {
          auto &layer = map->layers.at(layer_name);
          if(std::any_of(layer.tiles.begin(), layer.tiles.end(), [count](uint8_t id) { return id > count; }))
            throw FileError(name, "Layer '" + layer_name + "' uses a tile outside its atlas.");
          layers.push_back(&layer);
        }
        tile_properties = std::move(tileset.properties);
        tile_animations = std::move(tileset.animations);
        const auto *solid = tile_property("solid");
        const bool inherits_solid = solid && solid->any();
        if(inherits_solid || std::find(overrides.begin(), overrides.end(), SolidOverride::Solid) != overrides.end()) {
          const size_t cells = size_t(map->bounds.w) * map->bounds.h;
          solid_cells.resize(cells);
          for(size_t i = 0; i < cells; ++i) {
            const auto value = solid_override_at(overrides, i);
            solid_cells[i] = value == SolidOverride::Solid ||
                             (value == SolidOverride::Inherit && inherits_solid && tile_has_property(i, solid));
          }
        }
        if(!image_cache) load_neighbors(root, scene, association, sidecar);
      }

      void load_neighbors(const fs::path &root, std::string_view scene, const nlohmann::json &association,
                          std::string_view sidecar) {
        const auto connections = association.find("connections");
        if(connections == association.end()) return;
        if(!connections->is_object()) throw FileError(sidecar, "Expected connections keyed by north, south, west or east.");
        const auto size = map->pixel_size();
        constexpr std::array<std::string_view, 4> sides{"west", "east", "north", "south"};
        for(auto link = connections->begin(); link != connections->end(); ++link) {
          const auto direction = std::find(sides.begin(), sides.end(), link.key());
          if(direction == sides.end() || !link->is_object()) throw FileError(sidecar, "Invalid map connection direction or value.");
          const auto index = size_t(direction - sides.begin());
          auto &neighbor = neighbors[index];
          neighbor.scene = path_field(*link, "scene", sidecar);
          constexpr std::string_view suffix = ".scene";
          if(neighbor.scene == scene || !valid_project_relative_path(neighbor.scene) ||
             neighbor.scene.size() <= suffix.size() ||
             std::string_view(neighbor.scene).substr(neighbor.scene.size() - suffix.size()) != suffix ||
             !fs::is_regular_file(assets->path(neighbor.scene)))
            throw FileError(sidecar, "A connection must name a different existing .scene file.");

          int offset = 0;
          if(const auto field = link->find("offset_px"); field != link->end()) {
            if(!field->is_number_integer() || *field < -65535 || *field > 65535)
              throw FileError(sidecar, "Connection offset_px must be an integer from -65535 to 65535.");
            offset = field->get<int>();
          }
          neighbor.art = std::make_unique<Art>(assets);
          neighbor.art->load_map(root, neighbor.scene.c_str(), this);
          if(!neighbor.art->map) throw FileError(sidecar, "Connected scene needs a tilemap.");
          const auto other = neighbor.art->map->pixel_size();
          const bool horizontal = index < 2;
          const int along = horizontal ? size.h : size.w, other_along = horizontal ? other.h : other.w;
          if(offset >= along || int64_t(offset) + other_along <= 0 ||
             int64_t(offset) + other_along > INT32_MAX ||
             int64_t(size.w) + other.w > INT32_MAX || int64_t(size.h) + other.h > INT32_MAX)
            throw FileError(sidecar, "Connected map edges must overlap and fit in signed pixel coordinates.");
          neighbor.origin = horizontal ? blit::Point(index == 0 ? -other.w : size.w, offset)
                                       : blit::Point(offset, index == 2 ? -other.h : size.h);
        }
      }

      const SpriteAsset &sprite(const fs::path &, std::string_view stem) {
        if(const auto it = sprites.find(stem); it != sprites.end()) return *it->second;
        auto resource = assets->sprite(stem);
        return *sprites.emplace(std::string(stem), std::move(resource)).first->second;
      }

      void load_sprite_preloads(const std::vector<std::string> &preloads) {
        for(const auto &stem : preloads) sprite(assets->root(), stem);
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

      size_t memory_size() const {
        size_t bytes = sizeof(*this) + images.capacity() * sizeof(images.front()) + layers.capacity() * sizeof(layers.front());
        for(const auto &image : images) bytes += image->byte_cost;
        for(const auto &[name, sprite] : sprites)
          bytes += name.capacity() + sizeof(decltype(sprites)::value_type) + 64 + sprite->byte_cost + sprite->image->byte_cost;
        if(map) {
          bytes += sizeof(blit::Map) + map->flags.capacity() + (solid_cells.capacity() + 7) / 8;
          bytes += map->layer_order.capacity() * sizeof(std::string);
          for(const auto &name : map->layer_order) bytes += name.capacity() + 1;
          for(const auto &[name, layer] : map->layers)
            bytes += sizeof(blit::MapLayer) + 96 + name.capacity() + 1 + layer.name.capacity() + 1 +
                     layer.tiles.capacity() + layer.transforms.capacity();
        }
        bytes += tile_animations.capacity() * sizeof(TileAnimation);
        for(const auto &clip : tile_animations) bytes += sizeof(clip) + clip.name.capacity() + clip.frames.capacity() * sizeof(clip.frames.front());
        for(const auto &[name, property] : tile_properties) bytes += sizeof(property) + 96 + name.capacity();
        for(const auto &neighbor : neighbors) if(neighbor.art) bytes += neighbor.scene.capacity() + neighbor.art->memory_size();
        return bytes;
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

    void show_error(blit::Surface &screen, std::string_view text, int &y) {
      // Slice the stored diagnostic rather than allocating wrapped text every frame.
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
      std::optional<blit::Point> position;

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
          if(!position && !marker && std::strcmp(object.name, arrival) == 0) marker = &object;
          if(mover && (position || marker)) break;
        }
        return require_transform(doc, mover, actor, "actor", error) &&
               (position || require_transform(doc, marker, arrival, "arrival", error));
      }

      void apply(World &world) const {
        // prepare() checked both placements before SceneRuntime committed the load.
        auto &mover = *world.get<Transform2D>(world.find(actor));
        if(position) {
          mover.x = float(position->x);
          mover.y = float(position->y);
        } else {
          const auto &marker = *world.get<Transform2D>(world.find(arrival));
          mover.x = marker.x;
          mover.y = marker.y;
        }
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
    std::shared_ptr<ProjectAssets> assets;
    std::shared_ptr<const Json> manifest_data;
    std::map<fs::path, std::vector<std::string>> preloads;
    std::vector<std::shared_ptr<const std::string>> sources;
    std::shared_ptr<Art> active, staged, cached_art;
    struct CachedScene {
      std::unique_ptr<SceneDocument> catalogue;
      std::shared_ptr<Art> art;
      size_t bytes;
    };
    std::list<CachedScene> cache, pending_cache;
    size_t cache_budget = 0, cache_bytes = 0;
    bool preloading = false, opening = false;
    void (*on_loaded)() = nullptr;
    const Travel *pending_travel = nullptr; // Scoped to a travel load; cleared by finish().
    SceneError diagnostic{};

    explicit Player(World &world) : names(world, world.capacity()), transforms(world, world.capacity()),
      sprites(world, world.capacity()), colliders(world, world.capacity()),
      cameras(world, world.capacity()),
      scenes(world, limits_for(world.capacity()), {this, read, finish, prepare}), placed_generation(world.capacity()) {}

    void reset_resources() {
      assets = std::make_shared<ProjectAssets>(root);
      manifest_data = assets->json(manifest);
      preloads.clear();
      detail::visit_sprite_preloads(*manifest_data, root, [&](const fs::path &scene, const std::string &stem) {
        preloads[scene].push_back(stem);
      });
      cache.clear();
      cache_bytes = 0;
    }

    std::shared_ptr<Art> prepare_art(const SceneDocument &doc) {
      auto art = std::make_shared<Art>(assets);
      const auto *scene = doc.sources().front().path;
      art->load_map(root, scene);
      art->load_sprites(root, doc);
      if(const auto found = preloads.find(assets->path(scene)); found != preloads.end())
        art->load_sprite_preloads(found->second);
      return art;
    }

    void retain_pending() noexcept {
      if(pending_cache.empty()) return;
      const auto bytes = pending_cache.front().bytes;
      if(bytes > cache_budget) { pending_cache.clear(); return; }
      while(cache_bytes > cache_budget - bytes && !cache.empty()) {
        cache_bytes -= cache.back().bytes;
        cache.pop_back();
      }
      cache_bytes += bytes;
      cache.splice(cache.begin(), pending_cache);
    }

    bool load_cached(std::string_view scene, SceneError &error) {
      for(auto it = cache.begin(); it != cache.end(); ++it) {
        if(scene != it->catalogue->sources().front().path) continue;
        cache.splice(cache.begin(), cache, it);
        cached_art = cache.front().art;
        return scenes.load(*cache.front().catalogue, error);
      }
      return scenes.load(scene, error);
    }

    static bool read(void *context, std::string_view path, std::string_view &text) {
      auto &p = *static_cast<Player *>(context);
      try {
        if(p.sources.empty() && p.active && !p.pending_travel && !p.preloading && !p.opening)
          p.reset_resources(); // Explicit bridge reload sees edits; normal travel reuses resources.
        p.sources.push_back(p.assets->text(path, 1024 * 1024));
        text = *p.sources.back();
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
        p.staged = p.cached_art ? p.cached_art : p.prepare_art(doc);
        if(!p.cached_art && p.cache_budget) {
          auto catalogue = doc.compact_copy();
          const auto bytes = catalogue->memory_size() + p.staged->memory_size() + sizeof(CachedScene) + 64;
          p.pending_cache.push_back({std::move(catalogue), p.staged, bytes});
        }
        // Size the queue before committing artwork so rendering never grows it.
        if(!p.preloading) p.draw_items.reserve(p.sprites.capacity() + p.staged->layers.size());
        return true;
      } catch(const ProjectAssetError &e) {
        detail::scene_error(error, e.path, 0, e.what());
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
      if(success) {
        p.active.swap(p.staged);
        p.retain_pending();
      } else p.pending_cache.clear();
      p.staged.reset();
      p.cached_art.reset();
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
    auto old_assets = p.assets;
    auto old_manifest = p.manifest_data;
    auto old_preloads = std::move(p.preloads);
    auto old_cache = std::move(p.cache);
    const auto old_bytes = p.cache_bytes;
    p.cache_budget = scene_cache_budget_;
    p.opening = true;
    const auto old_callback = p.on_loaded;
    p.on_loaded = on_loaded;
    p.diagnostic = {};
    try {
      p.root = project_root();
      p.reset_resources();
      const auto &scene = path_field(*p.manifest_data, "scene", manifest);
      SceneError error;
      const bool success = p.scenes.load(scene, error);
      p.opening = false;
      if(success) return true;
      if(!*p.diagnostic.message) p.diagnostic = error;
    } catch(const FileError &e) {
      detail::scene_error(p.diagnostic, e.path, e.line, e.what());
    } catch(const std::exception &e) {
      detail::scene_error(p.diagnostic, manifest, 0, e.what());
    }
    p.opening = false;
    if(p.active) {
      p.root = std::move(old_root);
      p.on_loaded = old_callback;
      p.assets = std::move(old_assets);
      p.manifest_data = std::move(old_manifest);
      p.preloads = std::move(old_preloads);
      p.cache = std::move(old_cache);
      p.cache_bytes = old_bytes;
    }
    std::fprintf(stderr, "%s:%u: %s\n", p.diagnostic.file, p.diagnostic.line, p.diagnostic.message);
    return false;
  }

  std::string GameScene::project_asset_path(std::string_view project_relative) const {
    if(!player_ || !player_->active) return {};
    try {
      return player_->active->assets->path(project_relative).u8string();
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

  std::shared_ptr<ProjectAssets> GameScene::project_assets() const {
    return player_ && player_->active ? player_->active->assets : nullptr;
  }

  void GameScene::set_scene_cache_budget(size_t bytes) {
    scene_cache_budget_ = bytes;
    if(!player_) return;
    player_->cache.clear();
    player_->cache_bytes = 0;
    player_->cache_budget = bytes;
  }

  size_t GameScene::cached_scene_bytes() const { return player_ ? player_->cache_bytes : 0; }

  bool GameScene::preload_scene(std::string_view scene) {
    if(!player_ || !player_->active || player_->pending_travel || player_->preloading || !scene_cache_budget_) return false;
    auto &p = *player_;
    for(auto it = p.cache.begin(); it != p.cache.end(); ++it) {
      if(scene != it->catalogue->sources().front().path) continue;
      p.cache.splice(p.cache.begin(), p.cache, it);
      return true;
    }
    p.preloading = true;
    bool success = false;
    SceneError error;
    try {
      SceneDocument catalogue(limits_for(world.capacity()));
      if(read_scene_file(catalogue, scene, {&p, Player::read, nullptr, nullptr}, error) &&
         !catalogue.snapshot() && Player::prepare(&p, catalogue, error)) {
        success = !p.pending_cache.empty() && p.pending_cache.front().bytes <= p.cache_budget;
        if(success) p.retain_pending();
        else detail::scene_error(error, scene, 0, "scene resources exceed the configured cache budget");
      }
    } catch(const std::exception &e) {
      detail::scene_error(error, scene, 0, e.what());
    }
    p.sources.clear();
    p.staged.reset();
    p.pending_cache.clear();
    p.preloading = false;
    if(!success && !*error.message) detail::scene_error(error, scene, 0, "expected an authored scene for preloading");
    if(!success) report_travel_error(error);
    return success;
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
    if(p.load_cached({path, scene.size()}, error)) return true;
    return report_travel_error(*p.diagnostic.message ? p.diagnostic : error);
  }

  namespace {
  const Art::Neighbor *crossing_at(const Art &art, blit::Point world_point, const BoxCollider2D *shape,
                                  blit::Point &point) {
    if(!art.map) return nullptr;
    const auto size = art.map->pixel_size();
    const int edge = world_point.y >= 0 && world_point.y < size.h
        ? world_point.x < 0 ? 0 : world_point.x >= size.w ? 1 : -1
        : world_point.x >= 0 && world_point.x < size.w ? world_point.y < 0 ? 2 : 3 : -1;
    if(edge < 0) return nullptr;
    const auto &neighbor = art.neighbors[edge];
    if(!neighbor.art) return nullptr;
    const auto &dest = *neighbor.art->map;
    const auto bounds = dest.pixel_size();
    const auto x = int64_t(world_point.x) - neighbor.origin.x;
    const auto y = int64_t(world_point.y) - neighbor.origin.y;
    if(x < 0 || x >= bounds.w || y < 0 || y >= bounds.h) return nullptr;
    point = blit::Point(int(x), int(y));
    if(shape && shape->width > 0 && shape->height > 0) {
      const double left = x + shape->offset_x, top = y + shape->offset_y;
      const double right = left + shape->width, bottom = top + shape->height;
      if(!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom) ||
         left < 0 || top < 0 || right > bounds.w || bottom > bounds.h) return nullptr;
      const int tile = dest.tile_size();
      for(int row = int(std::floor(top / tile)); row < int(std::ceil(bottom / tile)); ++row)
        for(int col = int(std::floor(left / tile)); col < int(std::ceil(right / tile)); ++col)
          if(neighbor.art->tile_is_solid(size_t(row) * dest.bounds.w + col)) return nullptr;
    } else {
      const auto cell = dest.cell_at(point);
      if(neighbor.art->tile_is_solid(size_t(cell.y) * dest.bounds.w + cell.x)) return nullptr;
    }
    return &neighbor;
  }
  }

  bool GameScene::can_cross_map(const char *actor, blit::Point world_point) {
    blit::Point destination;
    return player_ && player_->active && !player_->pending_travel && position(actor) &&
           crossing_at(*player_->active, world_point, collider(actor), destination);
  }

  bool GameScene::cross_map(std::string_view actor, blit::Point world_point) {
    if(!player_ || !player_->active || player_->pending_travel) return false;
    auto &p = *player_;
    Player::Travel request;
    if(!Player::Travel::name(request.actor, actor) || !position(request.actor)) return false;
    blit::Point point;
    const auto *neighbor = crossing_at(*p.active, world_point, collider(request.actor), point);
    if(!neighbor) return false;
    request.position = point;
    char scene[scene_path_size];
    if(!detail::scene_copy(scene, sizeof(scene), neighbor->scene)) return false;
    SceneError error;
    p.diagnostic = {};
    p.pending_travel = &request;
    if(p.load_cached(scene, error)) return true;
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
    return name == "solid" ? art.tile_is_solid(index) : art.tile_has_property(index, art.tile_property(name));
  }

  TileMoveResult GameScene::move_against_tiles(Transform2D &position, const BoxCollider2D &shape,
                                                blit::Vec2 delta, std::string_view property, bool block_edges) const {
    if(!player_ || !player_->active || !player_->active->map) return {};
    const auto &art = *player_->active;
    const auto &map = *art.map;
    const bool solid = property == "solid";
    const auto *flag = solid ? nullptr : art.tile_property(property);
    return sdk::move_against_tiles(position, shape, delta, map.bounds.w, map.bounds.h, map.tile_size(),
      [&](blit::Point cell) {
        const size_t index = size_t(cell.y) * map.bounds.w + cell.x;
        return solid ? art.tile_is_solid(index) : art.tile_has_property(index, flag);
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
    if(art.solid_cells.empty()) return best;

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
        if(!art.tile_is_solid(size_t(y) * map.bounds.w + x)) continue;

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
    return camera_origin(name, {blit::screen.bounds.w, blit::screen.bounds.h}, origin);
  }

  bool GameScene::camera_origin(const char *name, blit::Size view, blit::Point &origin) const {
    if(view.w <= 0 || view.h <= 0) return false;
    if(!player_ || !player_->active) return false;
    const auto entity = world.find(name);
    const auto *camera = world.get<Camera2D>(entity);
    if(!camera) return false;
    const auto *center = world.get<Transform2D>(camera->follow);
    if(!center) center = world.get<Transform2D>(entity);
    if(!center || !std::isfinite(center->x) || !std::isfinite(center->y)) return false;

    const auto map = camera->clamp_to_map ? map_size() : blit::Size{};
    int low_x = 0, low_y = 0, high_x = map.w, high_y = map.h;
    if(map.w > 0 && map.h > 0) {
      for(const auto &neighbor : player_->active->neighbors) {
        if(!neighbor.art) continue;
        const auto size = neighbor.art->map->pixel_size();
        low_x = std::min(low_x, neighbor.origin.x);
        low_y = std::min(low_y, neighbor.origin.y);
        high_x = std::max(high_x, neighbor.origin.x + size.w);
        high_y = std::max(high_y, neighbor.origin.y + size.h);
      }
    }
    const auto axis = [](float center, int view, int low, int high) {
      const double offset = std::floor(double(center) - view * .5);
      if(high <= low) return offset;
      return std::clamp(offset, double(low), double(std::max(low, high - view)));
    };
    const double x = axis(center->x, view.w, low_x, high_x);
    const double y = axis(center->y, view.h, low_y, high_y);
    if(x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max() - view.w ||
       y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max() - view.h) return false;
    origin = {int(x), int(y)};
    return true;
  }

  bool GameScene::draw_from_camera(uint32_t time, const char *name) {
    blit::Point origin;
    return camera_origin(name, origin) && draw(time, origin);
  }

  bool GameScene::draw(uint32_t time, blit::Point world_origin) {
    return draw_to(blit::screen, time, world_origin);
  }

  bool GameScene::draw_to(blit::Surface &screen, uint32_t time, blit::Point world_origin) {
    if(!player_ || !player_->active) {
      screen.pen = blit::Pen(27, 28, 35);
      screen.clear();
      screen.pen = blit::Pen(255, 229, 170);
      int y = 8;
      show_error(screen, "Cannot play this scene", y);
      if(player_) {
        const auto &error = player_->diagnostic;
        if(error.line) {
          char location[scene_path_size + 12];
          std::snprintf(location, sizeof(location), "%s:%u", error.file, error.line);
          show_error(screen, location, y);
        } else show_error(screen, error.file, y);
        show_error(screen, error.message, y);
      } else show_error(screen, "Call GameScene::open() in init().", y);
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
    for(const auto &neighbor : art.neighbors) {
      if(!neighbor.art) continue;
      const auto size = neighbor.art->map->pixel_size();
      if(!viewport.intersects({neighbor.origin.x, neighbor.origin.y, size.w, size.h})) continue;
      const blit::Rect local(viewport.x - neighbor.origin.x, viewport.y - neighbor.origin.y, viewport.w, viewport.h);
      blit::TileFrameMap neighbor_frames;
      const blit::TileFrameMap *ids = nullptr;
      if(!neighbor.art->tile_animations.empty()) {
        neighbor_frames = tile_animation_frames(neighbor.art->tile_animations, time);
        ids = &neighbor_frames;
      }
      for(auto *layer : neighbor.art->layers)
        layer->draw(screen, *neighbor.art->atlas, local, {clip.x, clip.y}, ids);
    }
    blit::TileFrameMap frames;
    const blit::TileFrameMap *frame_ids = nullptr;
    if(!art.tile_animations.empty()) {
      frames = tile_animation_frames(art.tile_animations, time);
      frame_ids = &frames;
    }
    for(const auto &item : items) {
      if(!item.sprite) {
        art.layers[item.sequence]->draw(screen, *art.atlas, viewport, {clip.x, clip.y}, frame_ids);
      } else {
        const auto &sprite = *p.sprites.get(item.sprite);
        const auto found = art.sprites.find(sprite.asset);
        if(found != art.sprites.end()) {
          const auto &position = *p.transforms.get(item.sprite);
          draw_sprite(screen, found->second->image->surface, found->second->clips, position, sprite, world_origin, time, sprite.flip_x);
        }
      }
    }
    items.clear();
    return true;
  }
}
