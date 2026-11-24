#include "SDL.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "Input.hpp"
#include "engine/input.hpp"
#include "System.hpp"

namespace {
  class ControllerLock {
  public:
    explicit ControllerLock(SDL_mutex *mutex) : mutex(mutex) {
      if (SDL_LockMutex(mutex) != 0) {
        SDL_LogCritical(SDL_LOG_CATEGORY_INPUT, "Could not lock controller mutex: %s", SDL_GetError());
        std::abort();
      }
    }

    ~ControllerLock() {
      SDL_UnlockMutex(mutex);
    }

    ControllerLock(const ControllerLock &) = delete;
    ControllerLock &operator=(const ControllerLock &) = delete;

  private:
    SDL_mutex *mutex;
  };
}

std::map<int, int> Input::keys = {
	// arrow keys
	{SDLK_DOWN,   blit::Button::DPAD_DOWN},
	{SDLK_UP,     blit::Button::DPAD_UP},
	{SDLK_LEFT,   blit::Button::DPAD_LEFT},
	{SDLK_RIGHT,  blit::Button::DPAD_RIGHT},

	// wasd
	{SDLK_w,       blit::Button::DPAD_UP},
	{SDLK_a,       blit::Button::DPAD_LEFT},
	{SDLK_s,       blit::Button::DPAD_DOWN},
	{SDLK_d,       blit::Button::DPAD_RIGHT},

	// action buttons
	{SDLK_z,       blit::Button::A},
	{SDLK_x,       blit::Button::B},
	{SDLK_c,       blit::Button::X},
	{SDLK_v,       blit::Button::Y},

	{SDLK_u,       blit::Button::A},
	{SDLK_i,       blit::Button::B},
	{SDLK_o,       blit::Button::X},
	{SDLK_p,       blit::Button::Y},

	// system buttons
	{SDLK_1,       blit::Button::HOME},
	{SDLK_2,       blit::Button::MENU},
	{SDLK_3,       blit::Button::JOYSTICK},

  {SDLK_ESCAPE,  blit::Button::MENU},
};

std::map<int, int> Input::buttons = {
	// dpad
	{SDL_CONTROLLER_BUTTON_DPAD_DOWN,   blit::Button::DPAD_DOWN},
	{SDL_CONTROLLER_BUTTON_DPAD_UP,     blit::Button::DPAD_UP},
	{SDL_CONTROLLER_BUTTON_DPAD_LEFT,   blit::Button::DPAD_LEFT},
	{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,  blit::Button::DPAD_RIGHT},

	// action buttons
	{SDL_CONTROLLER_BUTTON_A,           blit::Button::A},
	{SDL_CONTROLLER_BUTTON_B,           blit::Button::B},
	{SDL_CONTROLLER_BUTTON_X,           blit::Button::X},
	{SDL_CONTROLLER_BUTTON_Y,           blit::Button::Y},

	// system buttons
	{SDL_CONTROLLER_BUTTON_BACK,        blit::Button::HOME},
	{SDL_CONTROLLER_BUTTON_START,       blit::Button::MENU},
	{SDL_CONTROLLER_BUTTON_LEFTSTICK,   blit::Button::JOYSTICK},
};

int Input::find_key(int key) {
	auto iter = keys.find(key);
	if (iter == keys.end()) return 0;
	else return iter->second;
}

int Input::find_button(int button) {
	auto iter = buttons.find(button);
	if (iter == buttons.end()) return 0;
	else return iter->second;
}

Input::Input(System *target) : target(target), controllers_mutex(SDL_CreateMutex()) {
  if (!controllers_mutex) {
    SDL_LogCritical(SDL_LOG_CATEGORY_INPUT, "Could not create controller mutex: %s", SDL_GetError());
    std::abort();
  }
  // Open all joysticks as game controllers.
  const int count = SDL_NumJoysticks();
  for (int n = 0; n < count; ++n) {
    if (SDL_IsGameController(n)) {
      handle_controller_added(n);
    }
  }
}

// System::stop() joins the game thread before Input is destroyed. Controller
// handles close before the mutex; no access to the already-deleted System is needed.
Input::~Input() = default;

bool Input::handle_mouse(int button, bool state, int x, int y) {

#ifdef __EMSCRIPTEN__
  int half_w = target->mode() ? target->width / 2 : target->width / 4;
  int half_h = target->mode() ? target->height / 2 : target->height / 4;
#else
  // SDL delivers mouse events in the renderer's logical panel coordinates.
  int half_w = target->width / 2;
  int half_h = target->height / 2;
#endif

	if (button == SDL_BUTTON_LEFT) {
		if (state) {
			if(left_ctrl){
				_virtual_tilt(x, y, half_w, half_h);
			} else {
				x = x - half_w;
				y = y - half_h;
				_virtual_analog(x, y, half_w, half_h);
			}
		} else {
			_virtual_analog(0, 0, half_w, half_h);
		}
		return true;
	}
	return false;
}

bool Input::handle_keyboard(const SDL_Keysym &key, bool state) {
	const int blit_button = find_key(key.sym);
	target->set_keyboard_button(key.scancode, blit_button, state);
	if (blit_button) {
		return true;
	} else if (key.sym == SDLK_LCTRL) {
		left_ctrl = state;
		return true;
	}
	return false;
}

void Input::handle_controller_button(SDL_JoystickID instance_id, int button, bool state) {
  static_assert(SDL_CONTROLLER_BUTTON_MAX <= 32, "Action input pad mask must hold all SDL controller buttons");
  const int blit_button = find_button(button);

  ControllerLock lock(controllers_mutex.get());
  const auto controller = find_controller(instance_id);
  if (controller != game_controllers.end() && button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX) {
    const uint32_t bit = uint32_t{1} << button;
    if (state) {
      controller->held_buttons |= bit;
    } else {
      controller->held_buttons &= ~bit;
    }
  } else if (!blit_button) {
    return;
  }

  target->set_controller_buttons(held_controller_buttons(), blit_button, state);
}

bool Input::handle_controller_motion(int axis, int value) {
	float fvalue = value / 32768.0f;
	switch(axis) {
		case SDL_CONTROLLER_AXIS_LEFTX:
			target->set_joystick(0, fvalue);
			return true;
		case SDL_CONTROLLER_AXIS_LEFTY:
			target->set_joystick(1, fvalue);
			return true;
		case SDL_CONTROLLER_AXIS_RIGHTX:
			target->set_tilt(0, fvalue);
			return true;
		case SDL_CONTROLLER_AXIS_RIGHTY:
			target->set_tilt(1, fvalue);
			return true;
		default:
			break;
	}
	return false;
}

bool Input::handle_controller_accel(float data[3]) {
  target->set_tilt(0, -data[0]);
  target->set_tilt(1, -data[2]);
  target->set_tilt(2,  data[1]);
  return true;
}

void Input::handle_focus_lost() {
  left_ctrl = false;
  target->clear_action_keys();
}

void Input::handle_controller_added(int joystick_index) {
  ControllerLock lock(controllers_mutex.get());
  ControllerHandle handle(SDL_GameControllerOpen(joystick_index));
  if (!handle) {
    return;
  }

  const auto instance_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(handle.get()));
  if (instance_id < 0 || find_controller(instance_id) != game_controllers.end()) {
    // SDL can also enqueue an add event for a controller opened at startup.
    return;
  }

  // Welcome rumble to test if it can rumble.
  const bool can_rumble = SDL_GameControllerRumble(handle.get(), 0xFFFF, 0xFFFF, 200) == 0;

#if SDL_VERSION_ATLEAST(2, 0, 14)
  // Enable accelerometer if present.
  SDL_GameControllerSetSensorEnabled(handle.get(), SDL_SENSOR_ACCEL, SDL_TRUE);
#endif

  game_controllers.push_back({std::move(handle), instance_id, can_rumble});
}

void Input::handle_controller_removed(SDL_JoystickID instance_id) {
  ControllerLock lock(controllers_mutex.get());
  const auto controller = find_controller(instance_id);
  if (controller == game_controllers.end()) {
    return;
  }

  game_controllers.erase(controller);
  target->set_controller_buttons(held_controller_buttons());
}

void Input::rumble_controllers(const float& volume) {
  if (volume > 0) {
    const auto frequency = volume * 0xFFFF;
    ControllerLock lock(controllers_mutex.get());
    for (const auto &controller : game_controllers) {
      if (controller.can_rumble) {
        SDL_GameControllerRumble(controller.handle.get(), frequency, frequency, 50);
      }
    }
  }
}

void Input::_virtual_tilt(int x, int y, int half_w, int half_h) {
	float z = 80.0f;
	x = x - half_w;
	y = y - half_h;
	blit::Vec3 shadow_tilt(x, y, z);
	shadow_tilt.normalize();
	target->set_tilt(0, shadow_tilt.x);
	target->set_tilt(1, shadow_tilt.y);
	target->set_tilt(2, shadow_tilt.z);
}

void Input::_virtual_analog(int x, int y, int half_w, int half_h) {
	float jx = (float)x / half_w;
	float jy = (float)y / half_h;
	target->set_joystick(0, jx);
	target->set_joystick(1, jy);
}

std::vector<Input::GameController>::iterator Input::find_controller(SDL_JoystickID instance_id) {
  return std::find_if(game_controllers.begin(), game_controllers.end(), [instance_id](const GameController &controller) {
    return controller.instance_id == instance_id;
  });
}

uint32_t Input::held_controller_buttons() const {
  uint32_t held = 0;
  for (const auto &controller : game_controllers) {
    held |= controller.held_buttons;
  }
  return held;
}
