#pragma once

#include "graphics/font.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sdk {
  /// Owns the bytes behind a runtime 32blit font.
  class FontAsset {
  public:
    FontAsset() = default;
    FontAsset(FontAsset &&) noexcept = default;
    FontAsset &operator=(FontAsset &&) noexcept = default;

    /// Load a packed FONT file at a trusted or already resolved filesystem path.
    bool load(const std::filesystem::path &path, std::string *error = nullptr);
    /// Resolve an authored .font path inside the supplied project directory before loading.
    /// Both load methods leave the previous font unchanged on failure.
    bool load_project(const std::filesystem::path &root, std::string_view relative, std::string *error = nullptr);

    /// Get a borrowed view, or minimal_font if empty. Keep this asset alive and unchanged
    /// while the view is in use.
    blit::Font font() const;

  private:
    std::vector<uint8_t> bytes_;
  };
}
