#pragma once

#include <bitset>
#include <cstdint>
#include "SDL_scancode.h"

namespace blit {
  struct ActionInput {
    std::bitset<SDL_NUM_SCANCODES> keys;
    uint32_t pad = 0;
  };

  // Updated before render and update in each SDL system loop. Read on the game thread.
  const ActionInput &action_input();
}
