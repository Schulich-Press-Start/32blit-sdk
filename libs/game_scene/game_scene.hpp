#pragma once

#include "32blit.hpp"
#include "collision_query.hpp"
#include "tile_movement.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace sdk {
  enum class SceneHitKind { None, Entity, Tile };

  struct SceneSweepHit {
    SceneHitKind kind = SceneHitKind::None;
    Entity entity{};
    blit::Point tile{};
    BoxSweepHit contact{};
  };

  /// Desktop player for an editor-authored project. Keep it alive for the game loop.
  class GameScene {
  public:
    explicit GameScene(uint16_t capacity = 256);
    ~GameScene();

    /// Attach any game-defined component pools to world before calling open() in init().
    /// Loads the manifest's startup scene. on_loaded runs after each successful scene load.
    /// A failed reload leaves the last playable scene and its assets intact.
    bool open(void (*on_loaded)() = nullptr);

    /// Resolve a project-relative asset after open() (also valid in on_loaded).
    /// Returns an absolute UTF-8 path, or an empty string if the path cannot be resolved.
    /// A missing file can still resolve; the caller decides whether it is required.
    std::string project_asset_path(std::string_view project_relative) const;

    /// Load a project-relative .scene after open(). Both actor and arrival must be
    /// destination entities with a matching runtime Name and a Transform2D.
    /// Sets the destination actor's x and y to the arrival's before on_loaded;
    /// other state is authored by the destination. Failure leaves the current scene intact.
    bool travel(std::string_view scene, std::string_view actor, std::string_view arrival);

    /// Draw map layers and sprites together, larger layer numbers first.
    /// At equal numbers, tiles draw first and both types keep their usual order.
    /// Sprite::layer edits take effect on the next draw. Pass the engine timestamp in milliseconds.
    /// world_origin is the world pixel shown at screen (0, 0), even when clipped.
    /// Select the screen mode and clear the background in game code. On initial load
    /// failure this displays the diagnostic instead and returns false.
    bool draw(uint32_t time, blit::Point world_origin = {});

    /// Find a named Camera2D and resolve its view's top-left using the current screen size.
    /// Uses the followed entity's Transform2D, or the camera's own if that is unavailable.
    /// With clamp_to_map, each axis is limited to [0, max(0, map size - screen size)]
    /// when a map is present. Returns false without changing origin if no view is available.
    bool camera_origin(const char *name, blit::Point &origin) const;
    /// Draw through the selected camera. Returns false without drawing if no view is available.
    bool draw_from_camera(uint32_t time, const char *name);

    /// Return zero size (or zero cell size) if the current scene has no map.
    blit::Size map_size() const;
    int tile_size() const;
    /// True if any layer at this cell has the authored property. Empty and out-of-map cells are false.
    bool tile_has_property(blit::Point cell, std::string_view name) const;

    /// Sweep x then y against the named tile property on any layer. Map edges block by default.
    /// A scene without a tilemap does not move the box; use direct transform edits for free motion.
    TileMoveResult move_against_tiles(Transform2D &position, const BoxCollider2D &shape,
                                     blit::Vec2 delta, std::string_view property = "solid",
                                     bool block_edges = true) const;

    /// Return the first contact against current entity boxes or authored solid tiles.
    /// The mask selects entity categories only, and ignore excludes a live entity in this world.
    /// A tile wins an exact tie with an entity. Map edges are not solid.
    SceneSweepHit sweep(Box moving, blit::Vec2 delta, CollisionMask mask = all_collision_categories,
                        Entity ignore = {}, bool solid_tiles = true);

    /// Named components, or nullptr. Reacquire after reload or structural world changes.
    Transform2D *position(const char *name) { return world.get<Transform2D>(world.find(name)); }
    const BoxCollider2D *collider(const char *name) const { return world.get<BoxCollider2D>(world.find(name)); }

    World world;

  private:
    struct Player;
    std::unique_ptr<Player> player_;
  };

  inline bool button_down(blit::Button button) { return (blit::buttons.state & button) != 0; }
  inline bool button_pressed(blit::Button button) { return (blit::buttons.pressed & button) != 0; }
}
