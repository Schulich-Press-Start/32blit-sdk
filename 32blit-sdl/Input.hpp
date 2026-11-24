#include "SDL.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

class System;

class Input {
	public:
		static std::map<int, int> keys;
		static std::map<int, int> buttons;

    static int find_key(int key);
		static int find_button(int button);

		explicit Input(System *system);
    ~Input();
    Input(const Input &) = delete;
    Input &operator=(const Input &) = delete;

		// Input handlers return true if they handled the event
		bool handle_mouse(int button, bool state, int x, int y);
		bool handle_keyboard(const SDL_Keysym &key, bool state);
		void handle_controller_button(SDL_JoystickID instance_id, int button, bool state);
		bool handle_controller_motion(int axis, int value);
    bool handle_controller_accel(float data[3]);
    void handle_focus_lost();

    // controller specific functions
    void handle_controller_added(int joystick_index);
    void handle_controller_removed(SDL_JoystickID instance_id);

    void rumble_controllers(const float& volume);

private:
		System *target;

    struct MutexDeleter {
      void operator()(SDL_mutex *mutex) const { SDL_DestroyMutex(mutex); }
    };
    struct ControllerDeleter {
      void operator()(SDL_GameController *controller) const { SDL_GameControllerClose(controller); }
    };
    using ControllerHandle = std::unique_ptr<SDL_GameController, ControllerDeleter>;

    struct GameController {
      ControllerHandle handle;
      SDL_JoystickID instance_id;
      bool can_rumble;
      uint32_t held_buttons = 0;
    };

    // Hotplug runs on the event thread, but rumble runs on the game thread.
    // Construct before opening controllers, and destroy after closing them.
    std::unique_ptr<SDL_mutex, MutexDeleter> controllers_mutex;
    std::vector<GameController> game_controllers;

    void _virtual_analog(int, int, int, int);
		void _virtual_tilt(int, int, int, int);

    std::vector<GameController>::iterator find_controller(SDL_JoystickID instance_id);
    uint32_t held_controller_buttons() const;

		bool left_ctrl = false;
};
