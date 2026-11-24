#pragma once

#include <cstddef>

namespace blit::simulator {
  // Bytes requested through the simulator's global C++ allocation functions.
  std::size_t live_bytes() noexcept;
  std::size_t peak_bytes() noexcept;
}
