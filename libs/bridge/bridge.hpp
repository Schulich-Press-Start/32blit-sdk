#pragma once

#include <cstddef>

namespace sdk {
  class World;
  class SceneRuntime;

  /// Attach the live World from game init, optionally with its SceneRuntime.
  /// Does nothing unless the native debug bridge was requested. The World and
  /// SceneRuntime must outlive the simulation. Without a SceneRuntime the
  /// snapshot workspace holds up to 4096 entities, 8192 components and 32768
  /// fields, reduced for smaller Worlds.
  void bridge_attach(World &world, SceneRuntime *scenes = nullptr);

  namespace dev_bridge {
    enum class Tick { Run, Pause, Step };

    struct Info {
      const char *title;
      int width, height;
      unsigned format, update_us, render_us;
      bool memory_capped = false;
      std::size_t live_bytes = 0, peak_bytes = 0;
    };

    /// Engine hooks. Start before simulation threads; stop after joining them.
    /// Address must be 127.0.0.1:<port>; restore without an address is rejected.
    /// Two concurrent clients; UTF-8 JSON Lines frames up to 1 MiB including LF.
    bool start(const char *address, const char *restore_path);
    bool enabled();
    /// Simulation-thread startup: restore (if requested) before listening.
    bool after_init();
    Tick poll(const Info &info);
    /// Simulation thread only. Logs are limited to 4096 bytes; clients with
    /// pending output (or a reply being written) may miss log events.
    void log(const char *message);
    void stop();
  }
}
