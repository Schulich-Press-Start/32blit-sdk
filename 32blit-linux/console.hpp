/**
 * \file console.hpp
 * \brief Keep the Linux text console out of the way while a game runs.
 *
 * When a game draws straight to the framebuffer from a text console (a
 * virtual terminal, e.g. Ctrl+Alt+F3), three things would otherwise go wrong:
 * - the kernel keeps drawing the console's text and blinking cursor on top of the game;
 * - every key pressed during play is echoed and ends up typed into the shell afterwards;
 * - a crash or Ctrl+C leaves the console in that state.
 *
 * console_init() switches the terminal to graphics mode and turns off echo;
 * console_restore() undoes both and is also run from signal handlers.
 */
#pragma once

namespace blit_linux {

  /**
   * \brief Switch the console to graphics mode, turn off echo, install signal handlers.
   *
   * Each step is optional: under SSH or without permission the game still
   * runs, the console just isn't hidden (a message says so).
   */
  void console_init();

  /**
   * \brief Put the console back to text mode and restore terminal settings.
   *
   * Async-signal-safe and safe to call more than once.
   */
  void console_restore();

  /** \brief True once SIGINT, SIGTERM or SIGHUP has been received. */
  bool quit_requested();
}
