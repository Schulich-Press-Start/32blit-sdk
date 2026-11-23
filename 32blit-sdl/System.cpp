#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include "SDL.h"

#ifndef __EMSCRIPTEN__
#include <vector>
#include "graphics/screen_fit.hpp"
#endif

#include "File.hpp"
#include "System.hpp"
#include "Input.hpp"
#include "32blit.hpp"
#include "UserCode.hpp"
#include "JPEG.hpp"
#include "Multiplayer.hpp"

#include "engine/api_private.hpp"

#ifdef BLIT_DEV_BRIDGE
#include "bridge.hpp"
#ifdef BLIT_MEMORY_CAP_MB
#include "MemoryBudget.hpp"
#endif

namespace {
  struct BridgeClock {
    bool active = false;
    uint32_t wall = 0, game = 0;
    unsigned update_us = 0, render_us = 0;
  };

  thread_local BridgeClock bridge_clock;
}
#endif

#if !defined(__EMSCRIPTEN__) || defined(BLIT_DEV_BRIDGE)
namespace {
  unsigned duration_us(std::chrono::steady_clock::time_point begin) {
    using namespace std::chrono;
    return static_cast<unsigned>(duration_cast<microseconds>(steady_clock::now() - begin).count());
  }
}
#endif

extern Input *blit_input;

// blit audio channels
static blit::AudioChannel channels[CHANNEL_COUNT];

#ifdef __EMSCRIPTEN__
int System::width = System::default_width;
int System::height = System::default_height;
static uint8_t framebuffer[System::default_width * System::default_height * 3];
#else
int System::width = 480;
int System::height = 272;
// Allocate once for the largest accepted screen, regardless of the initial mode.
static std::vector<uint8_t> framebuffer;
#endif
static blit::Pen palette[256];

// blit debug callback
void blit_debug(const char *message) {
	std::cout << message;
#ifdef BLIT_DEV_BRIDGE
	if(bridge_clock.active) sdk::dev_bridge::log(message);
#endif
}

// blit screenmode callback
#ifdef __EMSCRIPTEN__
blit::ScreenMode _mode = blit::ScreenMode::lores;
static blit::ScreenMode requested_mode = blit::ScreenMode::lores;
#else
static blit::Size requested_bounds, rendered_bounds;
#endif
static blit::PixelFormat cur_format = blit::PixelFormat::RGB;
static blit::PixelFormat requested_format = blit::PixelFormat::RGB;

blit::SurfaceInfo cur_surf_info;

static void set_screen_palette(const blit::Pen *colours, int num_cols) {
	memcpy(palette, colours, num_cols * sizeof(blit::Pen));
}

static bool set_screen_mode_format(blit::ScreenMode new_mode, blit::SurfaceTemplate &new_surf_template) {
#ifdef __EMSCRIPTEN__
  new_surf_template.data = framebuffer;
#else
  new_surf_template.data = framebuffer.data();
#endif

  if(new_surf_template.format == (blit::PixelFormat)-1)
    new_surf_template.format = blit::PixelFormat::RGB;

#ifdef __EMSCRIPTEN__
  blit::Size default_bounds(System::width, System::height);

  if(new_surf_template.bounds.empty())
    new_surf_template.bounds = default_bounds;

  switch(new_mode) {
    case blit::ScreenMode::lores:
      new_surf_template.bounds /= 2;
      break;
    case blit::ScreenMode::hires:
    case blit::ScreenMode::hires_palette:
      break;
  }

  if(new_surf_template.bounds != default_bounds && new_surf_template.bounds != default_bounds / 2)
    return false;
#else
  if(new_surf_template.bounds.empty()) {
    new_surf_template.bounds = new_mode == blit::ScreenMode::lores
      ? blit::Size(System::lores_width, System::lores_height)
      : blit::Size(System::default_width, System::default_height);
  }
  if(!blit::screen_size_allowed({System::width, System::height}, new_surf_template.bounds))
    return false;
#endif

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

#ifdef __EMSCRIPTEN__
  requested_mode = new_mode;
#else
  requested_bounds = new_surf_template.bounds;
#endif
  requested_format = new_surf_template.format;
  cur_surf_info = blit::SurfaceInfo(new_surf_template);

  return true;
}

blit::SurfaceInfo &set_screen_mode(blit::ScreenMode new_mode) {
  blit::SurfaceTemplate temp{nullptr, {0, 0}, new_mode == blit::ScreenMode::hires_palette ? blit::PixelFormat::P : blit::PixelFormat::RGB};

  // The panel may be too small for a default mode; keep the prior surface on rejection.
  set_screen_mode_format(new_mode, temp);

  return cur_surf_info;
}

// blit timer callback
std::chrono::steady_clock::time_point start;
static uint32_t wall_now() {
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
	return (uint32_t)elapsed.count();
}

uint32_t now() {
#ifdef BLIT_DEV_BRIDGE
  if(bridge_clock.active) return bridge_clock.game;
#endif
  return wall_now();
}

// blit random callback
#ifdef __MINGW32__
// Windows/MinGW doesn't support a non-deterministic source of randomness, so we fall back upon the age-old time seed once more
// Without this, random_device() will always return the same number and thus our mersenne twister will always produce the same sequence.
std::mt19937 random_generator(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#else
std::random_device random_device;
std::mt19937 random_generator(random_device());
#endif
std::uniform_int_distribution<uint32_t> random_distribution;
uint32_t blit_random() {
	return random_distribution(random_generator);
}


// us timer used by profiler

void enable_us_timer()
{
	// Enable/initialise timer
}

uint32_t get_us_timer()
{
	// get current time in us
	uint64_t ticksPerUs = SDL_GetPerformanceFrequency() / 1000000;
	return SDL_GetPerformanceCounter() / ticksPerUs;
}

uint32_t get_max_us_timer()
{
	// largest us value timer can produce for wrapping
	return UINT32_MAX;
}

/* Added a command line ability to specify a launch_path parameter. */
extern const char *launch_path;
static const char *get_launch_path() {
  return launch_path;
}

static blit::GameMetadata get_metadata() {
  blit::GameMetadata ret;

  ret.title = metadata_title;
  ret.author = metadata_author;
  ret.description = metadata_description;
  ret.version = metadata_version;
  ret.url = metadata_url;
  ret.category = metadata_category;

  return ret;
}

extern Multiplayer *blit_multiplayer;
bool blit_is_multiplayer_connected() {
	return blit_multiplayer->is_connected();
}

void blit_set_multiplayer_enabled(bool enabled) {
	blit_multiplayer->set_enabled(enabled);
}

void blit_send_message(const uint8_t *data, uint16_t length) {
	blit_multiplayer->send_message(data, length);
}

// blit API
static const blit::APIConst blit_api_const {
  blit::api_version_major, blit::api_version_minor,
  channels,

  ::set_screen_mode,
  ::set_screen_palette,

  ::now,
  ::blit_random,
  nullptr, // exit
  ::blit_debug,

  ::open_file,
  ::read_file,
  ::write_file,
  ::close_file,
  ::get_file_length,
  ::list_files,
  ::file_exists,
  ::directory_exists,
  ::create_directory,
  ::rename_file,
  ::remove_file,
  ::get_save_path,
  ::is_storage_available,

  ::enable_us_timer,
  ::get_us_timer,
  ::get_max_us_timer,

  blit_decode_jpeg_buffer,
  blit_decode_jpeg_file,

  nullptr, // launch
  nullptr, // erase_game
  nullptr, // get_type_handler_metadata

  ::get_launch_path,

  blit_is_multiplayer_connected,
  blit_set_multiplayer_enabled,
  blit_send_message,

  nullptr, // flash_to_tmp
  nullptr, // tmp_file_closed

  ::get_metadata,

  ::set_screen_mode_format,

  nullptr, // i2c_send
  nullptr, // i2c_recieve

  nullptr, // set_raw_cdc_enabled
  nullptr, // cdc_write
  nullptr, // cdc_read

  nullptr, // list_installed_games
  nullptr, // can_launch

  nullptr, // get_screen_data
  nullptr, // set_framebuffer
};

static blit::APIData blit_api_data;
static blit::ActionInput current_action_input;

namespace blit {
  const APIConst &api = blit_api_const;
  APIData &api_data = blit_api_data;
  const ActionInput &action_input() { return current_action_input; }
}

// SDL events
const Uint32 System::timer_event = SDL_RegisterEvents(2);
const Uint32 System::loop_event = System::timer_event + 1;

#ifndef __EMSCRIPTEN__
// Thread bouncers
static int system_timer_thread(void *ptr) {
	// Bounce back in to the class.
	System *sys = (System *)ptr;
	return sys->timer_thread();
}

static int system_loop_thread(void *ptr) {
	// Bounce back in to the class.
	System *sys = (System *)ptr;
	return sys->update_thread();
}
#endif

System::System() {
	m_input = SDL_CreateMutex();
	s_timer_stop = SDL_CreateSemaphore(0);
	s_loop_update = SDL_CreateSemaphore(0);
	s_loop_redraw = SDL_CreateSemaphore(0);
	s_loop_ended = SDL_CreateSemaphore(0);
}

System::~System() {
	SDL_DestroyMutex(m_input);
	SDL_DestroySemaphore(s_timer_stop);
	SDL_DestroySemaphore(s_loop_update);
	SDL_DestroySemaphore(s_loop_redraw);
	SDL_DestroySemaphore(s_loop_ended);
}

void System::run() {
	running = true;

	start = std::chrono::steady_clock::now();

#ifndef __EMSCRIPTEN__
	framebuffer.resize(size_t(width) * height * 3);
#endif

	blit::update = ::update;
	blit::render = ::render;

	setup_base_path();

	blit::set_screen_mode(blit::lores);

#ifdef __EMSCRIPTEN__
	::init();
#else
	t_system_loop = SDL_CreateThread(system_loop_thread, "Loop", (void *)this);
	t_system_timer = SDL_CreateThread(system_timer_thread, "Timer", (void *)this);
#endif
}

int System::timer_thread() {
	// Signal the system loop every 10 msec.
	int dropped = 0;
	SDL_Event event = {};
	event.type = timer_event;

	while (SDL_SemWaitTimeout(s_timer_stop, 10)) {
		if (SDL_SemValue(s_loop_update)) {
			dropped++;
			if(dropped > 100) {
				dropped = 100;
				event.user.code = 2;
				SDL_PushEvent(&event);
			} else {
				event.user.code = 1;
				SDL_PushEvent(&event);
			}
		} else {
			SDL_SemPost(s_loop_update);
			dropped = 0;
			event.user.code = 0;
			SDL_PushEvent(&event);
		}
	}
	return 0;
}

int System::update_thread() {
  // Run the blit user code once every time we are signalled.
  SDL_Event event = {};
  event.type = loop_event;

  ::init(); // Run init here because the user can make it hang.

#ifdef BLIT_DEV_BRIDGE
  if(sdk::dev_bridge::enabled()) {
    if(!sdk::dev_bridge::after_init()) {
      event.user.code = -1;
      SDL_PushEvent(&event);
      SDL_SemPost(s_loop_ended);
      return 1;
    }
    bridge_clock.wall = wall_now();
    bridge_clock.game = bridge_clock.wall ? bridge_clock.wall : 1;
    bridge_clock.active = true;
    // Prime the fixed-step clock without an update so the first Step runs once.
    blit::tick(bridge_clock.game);
  }
#endif

  while (true) {
    SDL_SemWait(s_loop_update);
    if(!running) break;
    bool rendered = loop();
    if(!running) break;
    if(rendered) {
      // present on main thread if we need to
      SDL_PushEvent(&event);
      SDL_SemWait(s_loop_redraw);
    }
  }
  SDL_SemPost(s_loop_ended);
  return 0;
}

bool System::loop() {
  SDL_LockMutex(m_input);
  blit::buttons = shadow_buttons;
  current_action_input = shadow_action_input;
  blit::tilt.x = shadow_tilt[0];
  blit::tilt.y = shadow_tilt[1];
  blit::tilt.z = shadow_tilt[2];
  blit::tilt.normalize();

  blit::joystick.x = shadow_joystick[0];
  blit::joystick.y = shadow_joystick[1];
  SDL_UnlockMutex(m_input);

  bool rendered = false;

  // only render at 50Hz (main loop runs at 100Hz)
  // however, the emscripten loop (usually) runs at the display refresh rate
  auto time_now = wall_now();
#ifdef BLIT_DEV_BRIDGE
  using sdk::dev_bridge::Tick;
  auto tick = Tick::Run;
  if(bridge_clock.active) {
    const uint32_t elapsed = time_now - bridge_clock.wall;
    bridge_clock.wall = time_now;
    sdk::dev_bridge::Info info{metadata_title, blit::screen.bounds.w, blit::screen.bounds.h,
                               static_cast<unsigned>(blit::screen.format),
                               bridge_clock.update_us, bridge_clock.render_us};
#ifdef BLIT_MEMORY_CAP_MB
    info.memory_capped = true;
    info.live_bytes = blit::simulator::live_bytes();
    info.peak_bytes = blit::simulator::peak_bytes();
#endif
    tick = sdk::dev_bridge::poll(info);
    if(tick != Tick::Pause) bridge_clock.game += tick == Tick::Step ? 10 : elapsed;
  }
#endif
#ifndef __EMSCRIPTEN__
  if(time_now - last_render_time >= 20)
#endif
  {
#ifndef __EMSCRIPTEN__
    const auto render_begin = std::chrono::steady_clock::now();
#endif
#ifdef BLIT_DEV_BRIDGE
    blit::render(bridge_clock.active ? bridge_clock.game : time_now);
#else
    blit::render(time_now);
#endif
#ifndef __EMSCRIPTEN__
    const unsigned render_us = duration_us(render_begin);
    frame_work_us += render_us;
#ifdef BLIT_DEV_BRIDGE
    if(bridge_clock.active) bridge_clock.render_us = render_us;
#endif
#endif
    last_render_time = time_now;

#ifdef __EMSCRIPTEN__
    _mode = requested_mode;
#else
    rendered_bounds = requested_bounds;
#endif
    cur_format = requested_format;

    rendered = true;
  }

#ifdef BLIT_DEV_BRIDGE
  if(!bridge_clock.active || tick != Tick::Pause)
#endif
  {
#ifndef __EMSCRIPTEN__
    const auto update_begin = std::chrono::steady_clock::now();
#endif
    blit::tick(::now());
#ifndef __EMSCRIPTEN__
    const unsigned update_us = duration_us(update_begin);
    frame_work_us += update_us;
#ifdef BLIT_DEV_BRIDGE
    if(bridge_clock.active) bridge_clock.update_us = update_us;
#endif
#endif
  }

#ifndef __EMSCRIPTEN__
  // Include update-only ticks since the last frame, not time waiting for SDL or the bridge.
  if(rendered) {
    if(frame_work_us > 16600) {
      const auto finished = wall_now();
      if(!warned_frame_budget || finished - last_budget_warning_time >= 1000) {
        std::cerr << "Warning: update + render took " << frame_work_us
                  << " us (over the 16.6 ms frame budget)\n";
        last_budget_warning_time = finished;
        warned_frame_budget = true;
      }
    }
    frame_work_us = 0;
  }
#endif
  blit_input->rumble_controllers(blit::vibration);

  blit_multiplayer->update();

  return rendered;
}

#ifdef __EMSCRIPTEN__
Uint32 System::mode() {
	return _mode;
}
#else
SDL_Rect System::frame_rect() const {
  return {0, 0, rendered_bounds.w, rendered_bounds.h};
}
#endif

Uint32 System::format() {
	return Uint32(cur_format);
}

bool System::update_texture(SDL_Texture *texture) {
#ifdef __EMSCRIPTEN__
  bool is_lores = _mode == blit::ScreenMode::lores;

  SDL_Rect dest_rect{0, 0, is_lores ? width / 2 : width, is_lores ? height / 2 : height};
  auto stride = dest_rect.w * blit::pixel_format_stride[int(cur_format)];

  if(cur_format == blit::PixelFormat::P) {
    uint8_t col_fb[default_width * default_height * 3];

    auto in = framebuffer, out = col_fb;
    auto size = dest_rect.w * dest_rect.h;

    for(int i = 0; i < size; i++) {
      uint8_t index = *(in++);
      (*out++) = palette[index].r;
      (*out++) = palette[index].g;
      (*out++) = palette[index].b;
    }

    return SDL_UpdateTexture(texture, &dest_rect, col_fb, stride * 3) == 0;
  }
  return SDL_UpdateTexture(texture, &dest_rect, framebuffer, stride) == 0;
#else
  const auto area = frame_rect();
  void *pixels;
  int pitch;
  if(SDL_LockTexture(texture, &area, &pixels, &pitch) != 0)
    return false;

  auto out = static_cast<uint8_t *>(pixels);
  const auto *in = framebuffer.data();
  if(cur_format == blit::PixelFormat::P) {
    for(int y = 0; y < area.h; ++y) {
      auto *row = out;
      for(int x = 0; x < area.w; ++x) {
        const auto &colour = palette[*in++];
        *row++ = colour.r;
        *row++ = colour.g;
        *row++ = colour.b;
      }
      out += pitch;
    }
  } else {
    const int stride = area.w * blit::pixel_format_stride[int(cur_format)];
    for(int y = 0; y < area.h; ++y) {
      std::memcpy(out, in, stride);
      in += stride;
      out += pitch;
    }
  }
  SDL_UnlockTexture(texture);
  return true;
#endif
}

void System::notify_redraw() {
	SDL_SemPost(s_loop_redraw);
}

void System::set_joystick(int axis, float value) {
	if (axis < 2) {
		SDL_LockMutex(m_input);
		shadow_joystick[axis] = value;
		SDL_UnlockMutex(m_input);
	}
}

void System::set_tilt(int axis, float value) {
	if (axis < 3) {
		SDL_LockMutex(m_input);
		shadow_tilt[axis] = value;
		SDL_UnlockMutex(m_input);
	}
}

void System::set_keyboard_button(SDL_Scancode key, int button, bool state) {
	SDL_LockMutex(m_input);
	if (key > SDL_SCANCODE_UNKNOWN && key < SDL_NUM_SCANCODES) {
		shadow_action_input.keys.set(key, state);
	}
	set_button(button, state);
	SDL_UnlockMutex(m_input);
}

void System::clear_action_keys() {
	SDL_LockMutex(m_input);
	shadow_action_input.keys.reset();
	SDL_UnlockMutex(m_input);
}

void System::set_controller_buttons(uint32_t pad, int button, bool state) {
	SDL_LockMutex(m_input);
	shadow_action_input.pad = pad;
	set_button(button, state);
	SDL_UnlockMutex(m_input);
}

void System::set_button(int button, bool state) {
	if (state) {
		shadow_buttons |= button;
	} else {
		shadow_buttons &= ~button;
	}
}

bool System::stop() {
  int returnValue;
  running = false;

  // make sure the update thread is not waiting for a render to complete
  SDL_SemPost(s_loop_redraw);

  const bool stopped = SDL_SemWaitTimeout(s_loop_ended, 500) == 0;
  if(!stopped) {
    std::cerr << "User code appears to have frozen. Detaching thread." << std::endl;
    SDL_DetachThread(t_system_loop);
  } else {
    SDL_WaitThread(t_system_loop, &returnValue);
  }

  SDL_SemPost(s_timer_stop);
  SDL_WaitThread(t_system_timer, &returnValue);
  return stopped;
}
