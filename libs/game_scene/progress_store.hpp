#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sdk {
  enum class ProgressReadResult { loaded, missing, error };

  // Native scene games store opaque bytes in the HAL's save directory. The filename
  // must be a single Windows-compatible UTF-8 leaf, not a path or device name.
  // On missing or error, payload is cleared; error is set only on failure.
  ProgressReadResult read_progress(std::string_view filename, std::size_t max_bytes,
                                  std::string &payload, std::string &error);

  // Stage and close the complete payload before replacing the previous file.
  // This preserves the previous save on write failure, but does not guarantee
  // persistence across power loss. Error is cleared on success.
  bool write_progress(std::string_view filename, std::string_view payload, std::string &error);
}
