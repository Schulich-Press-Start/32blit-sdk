#include "ActionInput.hpp"

class System {
	public:
		static const Uint32 timer_event;
		static const Uint32 loop_event;

    static constexpr int default_width = 320;
    static constexpr int default_height = 240;

#ifndef __EMSCRIPTEN__
    static constexpr int lores_width = default_width / 2;
    static constexpr int lores_height = default_height / 2;
    static constexpr int max_panel_dimension = 4096;
#endif

    // Panel dimensions on native SDL; legacy game dimensions on the web.
		static int width;
		static int height;

		System();
		~System();

		void run();
		bool stop(); // false if the game thread was detached

		int update_thread();
		int timer_thread();

		bool loop();

#ifdef __EMSCRIPTEN__
		Uint32 mode();
#else
    SDL_Rect frame_rect() const;
#endif
    Uint32 format();

		bool update_texture(SDL_Texture *);
		void notify_redraw();

		void set_joystick(int axis, float value);
		void set_tilt(int axis, float value);
		void set_keyboard_button(SDL_Scancode key, int button, bool state);
		void clear_action_keys();
		void set_controller_buttons(uint32_t pad, int button = 0, bool state = false);

	private:
		void set_button(int button, bool state); // caller holds m_input

		SDL_Thread *t_system_timer = nullptr;
		SDL_Thread *t_system_loop = nullptr;

		SDL_mutex *m_input = nullptr;

		SDL_sem *s_timer_stop = nullptr;
		SDL_sem *s_loop_update = nullptr;
		SDL_sem *s_loop_redraw = nullptr;
		SDL_sem *s_loop_ended = nullptr;

		bool running = false;

		Uint32 last_render_time = 0;
#ifndef __EMSCRIPTEN__
    uint64_t frame_work_us = 0;
    Uint32 last_budget_warning_time = 0;
    bool warned_frame_budget = false;
#endif

		// shadow input
		Uint32 shadow_buttons = 0;
		blit::ActionInput shadow_action_input;
		float shadow_joystick[2] = {0, 0};
		float shadow_tilt[3] = {0, 0, 0};
};
