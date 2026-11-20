#include <algorithm>
#include <cstdio>
#include <cmath>
#include <iostream>
#include "SDL.h"

#include "graphics/surface.hpp"
#ifndef __EMSCRIPTEN__
#include "graphics/screen_fit.hpp"
#endif

#include "Renderer.hpp"
#include "System.hpp"

Renderer::Renderer(SDL_Window *window, int width, int height) : sys_width(width), sys_height(height) {
#ifndef __EMSCRIPTEN__
	SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "nearest", SDL_HINT_OVERRIDE);
#endif
	//SDL_SetHint(SDL_HINT_RENDER_DRIVER, "openGL");
	renderer = SDL_CreateRenderer(window, -1, 0);
#ifdef __EMSCRIPTEN__
	if (renderer == nullptr) {
		std::cerr << "could not create renderer: " << SDL_GetError() << std::endl;
	}
#endif

#ifdef __EMSCRIPTEN__
  current = fb_texture;

	int w, h;
	SDL_GetWindowSize(window, &w, &h);
	resize(w, h);
  set_mode(mode);
#else
	if(!renderer || SDL_RenderSetLogicalSize(renderer, width, height) != 0 ||
	   SDL_RenderSetIntegerScale(renderer, SDL_TRUE) != 0)
		return;
#endif

	// Clear the window.
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);
	SDL_RenderPresent(renderer);

  fb_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, sys_width, sys_height);
#ifndef __EMSCRIPTEN__
  if(!fb_texture)
    return;
#endif
  fb_565_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGR565, SDL_TEXTUREACCESS_STREAMING, sys_width, sys_height);
}

Renderer::~Renderer() {
	SDL_DestroyTexture(fb_texture);
	SDL_DestroyTexture(fb_565_texture);
	SDL_DestroyRenderer(renderer);
}

#ifdef __EMSCRIPTEN__
void Renderer::set_mode(Mode new_mode) {
	mode = new_mode;

  int w = is_lores ? sys_width / 2 : sys_width;
  int h = is_lores ? sys_height / 2 : sys_height;

  SDL_RenderSetLogicalSize(renderer, w, h);
  SDL_RenderSetIntegerScale(renderer, (mode == KeepPixels) ? SDL_TRUE : SDL_FALSE);

  if (mode == Stretch) {
    // override the automatic scaling to not preserve the ratio
    SDL_RenderSetViewport(renderer, nullptr);

    SDL_RenderSetScale(renderer, (float)win_width / w, (float)win_height / h);
	}
}

void Renderer::resize(int width, int height) {
	win_width = width;
	win_height = height;

  if (mode == Stretch)
	  set_mode(mode);
}
#endif

void Renderer::update(System *sys) {
  auto format = blit::PixelFormat(sys->format());

#ifdef __EMSCRIPTEN__
	current = format == blit::PixelFormat::RGB565 ? fb_565_texture : fb_texture;

  if(is_lores != (sys->mode() == 0)) {
    is_lores = sys->mode() == 0;
    set_mode(mode);
  }

	sys->update_texture(current);
#else
  auto texture = format == blit::PixelFormat::RGB565 ? fb_565_texture : fb_texture;
  if(!sys->update_texture(texture))
    return;

  current = texture;
  const auto source = sys->frame_rect();
  if(active_rect.w != source.w || active_rect.h != source.h) {
    active_rect = source;
    const auto fit = blit::fit_screen({sys_width, sys_height}, {source.w, source.h});
    panel_rect = {fit.x, fit.y, source.w * fit.scale, source.h * fit.scale};
  }
#endif
}

bool Renderer::_render(SDL_Texture *target, SDL_Rect *destination) {
#ifdef __EMSCRIPTEN__
	SDL_SetRenderTarget(renderer, target);
#else
	if(SDL_SetRenderTarget(renderer, target) != 0)
		return false;
#endif
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
	SDL_RenderClear(renderer);
#ifdef __EMSCRIPTEN__
	SDL_RenderCopy(renderer, current, nullptr, destination);
#else
	if(current)
		SDL_RenderCopy(renderer, current, &active_rect, destination);
#endif
	return true;
}

void Renderer::present() {
#ifdef __EMSCRIPTEN__
  SDL_Rect dest;
  dest.x = 0;
  dest.y = 0;
  dest.w = sys_width;
  dest.h = sys_height;

	_render(nullptr, &dest);
#else
	_render(nullptr, &panel_rect);
#endif
	SDL_RenderPresent(renderer);
}

void Renderer::read_pixels(int width, int height, Uint32 format, Uint8 *buffer) {
	SDL_Texture *record_target = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_TARGET, width, height);
#ifdef __EMSCRIPTEN__
	_render(record_target, nullptr);
#else
	if(!record_target)
		return;
	if(!_render(record_target, &panel_rect)) {
		SDL_DestroyTexture(record_target);
		return;
	}
#endif
	SDL_RenderReadPixels(renderer, nullptr, format, buffer, width*SDL_BYTESPERPIXEL(format));
#ifndef __EMSCRIPTEN__
	SDL_SetRenderTarget(renderer, nullptr);
#endif
	SDL_DestroyTexture(record_target);
}

