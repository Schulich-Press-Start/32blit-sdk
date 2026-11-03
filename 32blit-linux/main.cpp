/**
 * \file main.cpp
 * \brief Entry point and `blit::api` implementation for the Linux port.
 *
 * The 32blit engine never talks to hardware itself. It calls through two
 * tables that each port fills in (declared in `32blit/engine/api_private.hpp`):
 * - `blit::api` (`APIConst`): functions — screen mode, time, files, ...
 * - `blit::api_data` (`APIData`): live state — buttons, joystick, tilt, ...
 *
 * This file fills in both tables and runs the game. The whole port is one
 * thread:
 *
 * \code
 * init()
 * loop:
 *   read input        -> api_data.buttons
 *   blit::tick(now)   -> runs update() for every 10 ms that has passed
 *   if a frame is due: render(), present to the display
 *   sleep until the next update or frame
 * \endcode
 *
 * Holding HOME for one second exits the game (the handheld has no keyboard
 * for Ctrl+C). The exit code is what the launcher hand-off will use later.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "engine/api_private.hpp"
#include "engine/engine.hpp"

#include "console.hpp"
#include "display_fb.hpp"
#include "file.hpp"
#include "input_evdev.hpp"
#include "screen_fit.hpp"
#include "user_code.hpp"

namespace blit_linux {

  namespace {
    /** \name Screen
     *  The game draws into `framebuffer`; display_present() converts and copies it to the panel.
     *  Games may use any screen size from 96x96 up to the panel's size (screen_fit.hpp).
     */
    ///@{
    constexpr blit::Size lores_size(160, 120); ///< `lores` size when the game doesn't ask for one
    constexpr blit::Size hires_size(320, 240); ///< `hires` / `hires_palette` size when the game doesn't ask for one

    /// The game's screen. Sized in main() for the largest allowed screen: the whole panel at 3 bytes per pixel (RGB).
    std::vector<uint8_t> framebuffer;

    /// Colours for `PixelFormat::P` (palette) modes, set by the game.
    blit::Pen palette[256];

    blit::PixelFormat cur_format = blit::PixelFormat::RGB;
    blit::Size cur_bounds = lores_size;

    /// Returned by set_screen_mode(); must outlive the call.
    blit::SurfaceInfo cur_surf_info;
    ///@}

    /** \name Main loop timing */
    ///@{
    /// Time between rendered frames. ~60 Hz until DRM vsync paces frames (Stage 1.2).
    constexpr uint64_t frame_interval_us = 16667;

    /// Holding HOME this long exits the game.
    constexpr uint32_t home_hold_exit_ms = 1000;
    ///@}

    /// Audio channels the engine mixes. Nothing plays them until the ALSA thread (Stage 1.2).
    blit::AudioChannel channels[CHANNEL_COUNT];

    /// `--launch_path` argument: a file the game was asked to open (as on the 32blit console).
    const char *launch_path = nullptr;

    /// Port start time; now() and the µs timer count from here.
    timespec start_time;

    std::mt19937 random_generator{std::random_device{}()};

    /** \brief Microseconds since the port started, from the monotonic clock. */
    uint64_t elapsed_us() {
      timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return uint64_t(ts.tv_sec - start_time.tv_sec) * 1000000 + (ts.tv_nsec - start_time.tv_nsec) / 1000;
    }

    /** \brief Sleep until `elapsed_us()` reaches `target_us`. Returns early on a signal. */
    void sleep_until_us(uint64_t target_us) {
      uint64_t now = elapsed_us();
      if(target_us <= now)
        return;

      timespec ts;
      uint64_t wait_us = target_us - now;
      ts.tv_sec = time_t(wait_us / 1000000);
      ts.tv_nsec = long(wait_us % 1000000) * 1000;
      nanosleep(&ts, nullptr);
    }

    /** \brief Undo everything main() set up. Runs at exit, however the game exits. */
    void shutdown_port() {
      console_restore();
      input_shutdown();
      display_shutdown();
    }

    // ---- blit::api functions -------------------------------------------------
    // The engine calls these through the tables at the bottom of this namespace.

    /**
     * \brief Validate a screen mode and point the engine at our framebuffer.
     *
     * Called by `blit::set_screen_mode()`. Fills in `data`, `bounds`,
     * `format` and `palette` of `new_surf_template`.
     *
     * \param new_mode          lores, hires or hires_palette. Only picks the size
     *                          when the game doesn't ask for one (160x120 or 320x240).
     * \param new_surf_template Requested format/bounds in; final surface out.
     *                          `format == -1` means "port default" (RGB).
     *                          Empty bounds mean "the mode's normal size".
     *                          Any size from 96x96 up to the panel is accepted.
     * \return false for formats this port doesn't support and for sizes
     *         smaller than 96x96 or larger than the panel. The current
     *         screen mode stays as it was.
     */
    bool set_screen_mode_format(blit::ScreenMode new_mode, blit::SurfaceTemplate &new_surf_template) {
      new_surf_template.data = framebuffer.data();

      if(new_surf_template.format == (blit::PixelFormat)-1)
        new_surf_template.format = blit::PixelFormat::RGB;

      if(new_surf_template.bounds.empty())
        new_surf_template.bounds = new_mode == blit::ScreenMode::lores ? lores_size : hires_size;

      if(!screen_size_allowed(display_size(), new_surf_template.bounds))
        return false;

      switch(new_surf_template.format) {
        case blit::PixelFormat::RGB:
        case blit::PixelFormat::RGB565:
          new_surf_template.palette = nullptr;
          break;
        case blit::PixelFormat::P:
          new_surf_template.palette = palette;
          break;
        default:
          return false;
      }

      // takes effect from the next present, i.e. for the frame the game is drawing now
      cur_format = new_surf_template.format;
      cur_bounds = new_surf_template.bounds;
      return true;
    }

    /**
     * \brief Older API entry: set a mode at its normal size with its default format.
     * \return Description of the screen surface: the new one, or the
     *         unchanged current one if the panel is too small for the mode.
     */
    blit::SurfaceInfo &set_screen_mode(blit::ScreenMode new_mode) {
      blit::SurfaceTemplate temp{nullptr, {0, 0},
        new_mode == blit::ScreenMode::hires_palette ? blit::PixelFormat::P : blit::PixelFormat::RGB};

      // only fails on panels smaller than the mode's normal size
      if(set_screen_mode_format(new_mode, temp))
        cur_surf_info = blit::SurfaceInfo(temp);

      return cur_surf_info;
    }

    /** \brief Copy the game's palette for `PixelFormat::P` modes. */
    void set_screen_palette(const blit::Pen *colours, int num_cols) {
      if(num_cols > 256)
        num_cols = 256;
      memcpy(palette, colours, size_t(num_cols) * sizeof(blit::Pen));
    }

    /** \brief Milliseconds since the port started. */
    uint32_t now() {
      return uint32_t(elapsed_us() / 1000);
    }

    /** \brief 32 random bits. */
    uint32_t get_random() {
      return random_generator();
    }

    /** \brief Exit the game. Cleanup runs through atexit(shutdown_port). */
    void exit_game(bool is_error) {
      std::exit(is_error ? EXIT_FAILURE : EXIT_SUCCESS);
    }

    /** \brief Debug text from `blit::debug()` / `debugf()`, to stdout. */
    void debug(const char *message) {
      fputs(message, stdout);
      fflush(stdout);
    }

    /** \brief Microsecond timer for the profiler; wraps at 2^32 µs (~71 minutes). */
    uint32_t get_us_timer() {
      return uint32_t(elapsed_us());
    }

    /** \brief Largest value get_us_timer() returns before wrapping. */
    uint32_t get_max_us_timer() {
      return UINT32_MAX;
    }

    /** \brief File the game was launched with (`--launch_path`), or nullptr. */
    const char *get_launch_path() {
      return launch_path;
    }

    /** \brief Game metadata from metadata.yml (or the weak defaults). */
    blit::GameMetadata get_metadata() {
      blit::GameMetadata ret;
      ret.title = metadata_title;
      ret.author = metadata_author;
      ret.description = metadata_description;
      ret.version = metadata_version;
      ret.url = metadata_url;
      ret.category = metadata_category;
      return ret;
    }

    /**
     * \name Multiplayer
     * There is no link hardware yet (proposal open question 5), so the link
     * is never connected and messages go nowhere. The engine calls these
     * without null checks, so they must exist.
     */
    ///@{
    bool is_multiplayer_connected() { return false; }
    void set_multiplayer_enabled(bool) {}
    void send_message(const uint8_t *, uint16_t) {}
    ///@}

    /**
     * \brief The function table the engine calls.
     *
     * Order must match `blit::APIConst` exactly. `nullptr` entries are
     * features this port doesn't have; the engine checks for null before
     * calling the optional ones (launcher, flash, i2c, CDC). JPEG decoding
     * is not optional in the engine: `blit::decode_jpeg_*` will crash on
     * this port until it is implemented.
     */
    const blit::APIConst api_const {
      blit::api_version_major, blit::api_version_minor,

      channels,

      set_screen_mode,
      set_screen_palette,

      now,
      get_random,
      exit_game,
      debug,

      // files
      open_file,
      read_file,
      write_file,
      close_file,
      get_file_length,
      list_files,
      file_exists,
      directory_exists,
      create_directory,
      rename_file,
      remove_file,
      get_save_path,
      is_storage_available,

      // profiler
      nullptr, // enable_us_timer: the monotonic clock is always running
      get_us_timer,
      get_max_us_timer,

      // jpeg
      nullptr, // decode_jpeg_buffer
      nullptr, // decode_jpeg_file

      // launcher (the LVGL launcher is a separate program; see the proposal §4.1)
      nullptr, // launch
      nullptr, // erase_game
      nullptr, // get_type_handler_metadata

      get_launch_path,

      // multiplayer
      is_multiplayer_connected,
      set_multiplayer_enabled,
      send_message,

      nullptr, // flash_to_tmp
      nullptr, // tmp_file_closed

      get_metadata,

      set_screen_mode_format,

      nullptr, // i2c_send
      nullptr, // i2c_receive

      nullptr, // set_raw_cdc_enabled
      nullptr, // cdc_write
      nullptr, // cdc_read

      nullptr, // list_installed_games
      nullptr, // can_launch

      nullptr, // get_screen_data: single buffer, the engine's screen pointer never changes
      nullptr, // set_framebuffer
    };

    /// Live state the engine reads (buttons, joystick, tilt...) and the game can set (LED, vibration).
    blit::APIData api_data_storage;

    /**
     * \brief Run the game until it exits, HOME is held, or a quit signal arrives.
     * \return Process exit code.
     */
    int run_loop() {
      uint64_t next_frame_us = elapsed_us();
      bool home_down = false;
      uint32_t home_down_since = 0;

      while(!quit_requested()) {
        blit::api_data.buttons = input_poll();

        uint32_t time = now();

        // hold HOME to quit
        if(blit::api_data.buttons & blit::Button::HOME) {
          if(!home_down) {
            home_down = true;
            home_down_since = time;
          } else if(time - home_down_since >= home_hold_exit_ms) {
            break;
          }
        } else {
          home_down = false;
        }

        // runs update() as many times as needed; returns ms until the next update is due
        int ms_to_update = blit::tick(time);

        uint64_t now_us = elapsed_us();
        if(now_us >= next_frame_us) {
          blit::render(now());

          blit::SurfaceInfo frame;
          frame.data = framebuffer.data();
          frame.bounds = cur_bounds;
          frame.format = cur_format;
          frame.palette = palette;
          display_present(frame);

          next_frame_us += frame_interval_us;

          // if we fell more than a frame behind, don't render a burst of frames to catch up
          if(now_us > next_frame_us)
            next_frame_us = now_us + frame_interval_us;
        }

        uint64_t next_update_us = elapsed_us() + uint64_t(ms_to_update > 0 ? ms_to_update : 0) * 1000;
        sleep_until_us(next_frame_us < next_update_us ? next_frame_us : next_update_us);
      }

      return EXIT_SUCCESS;
    }

    /** \brief Print command-line help. */
    void print_usage(const char *argv0) {
      printf("Usage: %s <options>\n\n", argv0);
      printf(" --launch_path <file> -- File for the game to open (as the 32blit launcher would pass).\n");
      printf(" --info               -- Print metadata info and exit.\n");
      printf(" --help               -- Show this help.\n\n");
      printf("Controls: arrows/WASD = D-pad, Z X C V = A B X Y, 1 = HOME (hold to quit), 2/Esc = MENU\n");
    }
  }
}

/// \cond
// The engine looks these up by name (declared extern in api_private.hpp).
namespace blit {
  const APIConst &api = blit_linux::api_const;
  APIData &api_data = blit_linux::api_data_storage;
}
/// \endcond

/**
 * \brief Program entry: parse arguments, set up devices, run the game.
 *
 * Fails (exit code 1) only if the display can't be opened. Missing input
 * devices or an unhideable console are reported and the game still runs.
 */
int main(int argc, char *argv[]) {
  using namespace blit_linux;

  for(int i = 1; i < argc; i++) {
    std::string arg(argv[i]);

    if(arg == "--launch_path" && i + 1 < argc) {
      launch_path = argv[++i];
    } else if(arg == "--info") {
      printf("%s %s\n\n%s\n\n", metadata_title, metadata_version, metadata_description);
      printf(" Category: %s\n Author:   %s\n URL:      %s\n", metadata_category, metadata_author, metadata_url);
      return EXIT_SUCCESS;
    } else if(arg == "--help") {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    } else {
      fprintf(stderr, "Unknown option %s\n\n", argv[i]);
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  printf("%s %s\n", metadata_title, metadata_version);
  printf("Powered by 32Blit Linux runtime - github.com/Schulich-Press-Start/32blit-sdk\n");

  clock_gettime(CLOCK_MONOTONIC, &start_time);

  file_init(metadata_title);

  if(!display_init())
    return EXIT_FAILURE;

  // big enough for the largest screen a game may ask for: the whole panel, 3 bytes per pixel
  const blit::Size panel = display_size();
  framebuffer.resize(size_t(panel.w) * size_t(panel.h) * 3);

  input_init();
  console_init();

  // from here on, every way out (return, api.exit, std::exit) restores the console and devices
  std::atexit(shutdown_port);

  blit::update = ::update;
  blit::render = ::render;

  // same default as the other ports; the game usually picks its own mode in init()
  blit::set_screen_mode(blit::ScreenMode::lores);

  ::init();

  return run_loop();
}
