#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sdk {
  /// Named keyboard and gamepad actions for the native SDL port. Use on the game thread.
  class InputActions {
  public:
    enum class Device : uint8_t { Keyboard, Gamepad };

    class Handle {
    public:
      Handle() = default;
      friend bool operator==(Handle a, Handle b) { return a.index_ == b.index_; }
      friend bool operator!=(Handle a, Handle b) { return !(a == b); }

    private:
      friend class InputActions;
      explicit Handle(std::size_t index) : index_(index) {}
      std::size_t index_ = static_cast<std::size_t>(-1);
    };

    /// Find or register a case-sensitive name. Keep this map's handles for frame-time reads.
    Handle action(std::string_view name);
    /// Add an SDL scancode or controller button by name. Invalid names and handles return false.
    bool bind(Handle action, Device device, const char *physical);
    /// Replace all bindings from a UTF-8 path without invalidating handles. Rows are
    /// `action keyboard Key Name` or `action gamepad button`; blank and # comment lines
    /// are ignored. Missing, unreadable, or invalid files log to stderr and leave the map intact.
    bool load(const std::string &path);

    /// Call once at the start of each game update. Edges follow the OR of all bindings.
    void update();
    bool down(Handle action) const { return (state(action) & Down) != 0; }
    bool pressed(Handle action) const { return (state(action) & Pressed) != 0; }
    bool released(Handle action) const { return (state(action) & Released) != 0; }

  private:
    enum State : uint8_t { Down = 1, Pressed = 2, Released = 4 };
    struct Binding { uint16_t physical; Device device; };
    struct Entry {
      std::string name;
      std::vector<Binding> bindings;
      uint8_t state = 0;
    };

    Handle find(std::string_view name) const;
    uint8_t state(Handle action) const {
      return action.index_ < entries_.size() ? entries_[action.index_].state : 0;
    }

    std::vector<Entry> entries_;
  };
}
