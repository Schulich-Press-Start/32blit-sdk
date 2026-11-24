#pragma once

#include "types/point.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace blit { struct Surface; }

namespace sdk {
  class GameScene;

  /// One loaded one-shot effect, independent of scene reloads. Use on the game thread.
  class Vfx {
  public:
    Vfx();
    ~Vfx();

    /// Preload the art and any authored audio cues after GameScene::open().
    /// A failed load leaves the previous effect and its playback intact.
    bool load(std::string_view path, const GameScene &game);

    /// Start (or restart) at a world pixel, firing zero-offset cues immediately.
    /// Pass the engine's monotonic millisecond timestamp, not a wall clock.
    /// Returns false if no effect is loaded.
    bool start(uint32_t now, blit::Point position = {});
    void update(uint32_t now);

    /// Draw at the most recent update time. view_origin is the world point at (0, 0).
    /// Call after drawing the world for a foreground effect.
    void draw(blit::Surface &target, blit::Point view_origin = {}) const;

    /// Cancel remaining events and drawing. A one-shot cue already fired can finish.
    void stop();
    bool playing() const;
    std::string_view name() const;
    const std::string &last_error() const { return last_error_; }

  private:
    struct Player;
    std::unique_ptr<Player> player_;
    std::string last_error_;
  };
}
