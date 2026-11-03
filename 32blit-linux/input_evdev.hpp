/**
 * \file input_evdev.hpp
 * \brief Buttons from Linux input devices (evdev, `/dev/input/event*`).
 *
 * Every key-like device (keyboards, gamepads, and GPIO buttons described as
 * `gpio-keys` in a devicetree) shows up as an `event` device. We read key
 * presses from all of them and turn them into 32blit button bits using one
 * table (see input_evdev.cpp).
 *
 * Reading evdev needs read access to `/dev/input/event*`: be in the
 * `input` group or run as root.
 */
#pragma once

#include <cstdint>

namespace blit_linux {

  /**
   * \brief Open every input device that has at least one mapped key.
   * \return Number of devices opened (0 is allowed; the game just gets no input).
   */
  int input_init();

  /**
   * \brief Read all pending key events without blocking.
   * \return Current button state as `blit::Button` bits.
   */
  uint32_t input_poll();

  /** \brief Close all input devices. Safe to call more than once. */
  void input_shutdown();
}
