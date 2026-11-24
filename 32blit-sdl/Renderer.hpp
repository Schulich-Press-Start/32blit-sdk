class System;

class Renderer {
	public:
		Renderer(SDL_Window *window, int width, int height);
		~Renderer();

#ifdef __EMSCRIPTEN__
		enum Mode {Stretch, KeepAspect, KeepPixels};

		void resize(int width, int height);
		void set_mode(Mode mode);
#else
		bool ready() const { return fb_texture && fb_565_texture; }
#endif
		void update(System *sys);
		void present();
		void read_pixels(int width, int height, Uint32 format, Uint8 *buffer);

	private:
		bool _render(SDL_Texture *target, SDL_Rect *destination);

		int sys_width, sys_height;
#ifdef __EMSCRIPTEN__
		int win_width, win_height;

    bool is_lores = false;
		Mode mode = KeepPixels;
#else
		SDL_Rect active_rect{};
		SDL_Rect panel_rect{};
#endif
		SDL_Renderer *renderer = nullptr;

		SDL_Texture *fb_texture = nullptr;
		SDL_Texture *fb_565_texture = nullptr;
		SDL_Texture *current = nullptr;
};
