#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "engine/api_private.hpp"
#include "progress_store.hpp"

namespace sdk {
  namespace {
    namespace fs = std::filesystem;
    constexpr auto stream_limit = std::numeric_limits<std::streamsize>::max();

    bool equals_upper(std::string_view name, std::string_view upper) {
      return std::equal(name.begin(), name.end(), upper.begin(), upper.end(), [](char c, char expected) {
        return (c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c) == expected;
      });
    }

    bool device_name(std::string_view filename) {
      auto stem = filename.substr(0, filename.find('.'));
      while(!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
      for(auto device : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}) {
        if(equals_upper(stem, device)) return true;
      }
      if(stem.size() < 4 || !(equals_upper(stem.substr(0, 3), "COM") ||
                             equals_upper(stem.substr(0, 3), "LPT"))) return false;
      const auto digit = stem.substr(3);
      // Win32 also recognizes the superscripts 1, 2 and 3 as device digits.
      return (digit.size() == 1 && digit.front() >= '1' && digit.front() <= '9') ||
             digit == "\xC2\xB9" || digit == "\xC2\xB2" || digit == "\xC2\xB3";
    }

    fs::path progress_path(std::string_view filename) {
      if(filename.empty() || filename.back() == '.' || filename.back() == ' ' ||
         filename.find_first_of("/\\:\"<>|?*") != filename.npos ||
         std::any_of(filename.begin(), filename.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
         device_name(filename)) throw std::runtime_error("Expected a single portable progress filename.");

      const auto root_string = blit::api.get_save_path ? blit::api.get_save_path() : nullptr;
      if(!root_string || !*root_string) throw std::runtime_error("The HAL save directory is unavailable.");
      // SDL_GetPrefPath returns UTF-8 even on Windows; keep paths wide there.
      const auto root = fs::u8path(root_string);
      if(!fs::is_directory(root)) throw std::runtime_error("The HAL save directory is missing or is not a directory.");
      return root / fs::u8path(filename);
    }

    // Do not follow a link or try to read a directory or special file as progress.
    bool progress_exists(const fs::path &path) {
      const auto status = fs::symlink_status(path);
      if(status.type() == fs::file_type::not_found) return false;
      if(!fs::is_regular_file(status)) throw std::runtime_error("The progress file is not a regular file.");
      return true;
    }

    fs::path staging_directory(const fs::path &destination) {
      const auto prefix = ".progress-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-";
      const auto parent = destination.parent_path();
      for(unsigned i = 0; i < 16; ++i) {
        const auto directory = parent / (prefix + std::to_string(i));
        if(directory == destination) continue;
        std::error_code error;
        if(fs::create_directory(directory, error)) return directory;
        if(error && error != std::errc::file_exists)
          throw fs::filesystem_error("Cannot create progress staging directory", directory, error);
      }
      throw std::runtime_error("Cannot reserve a progress staging directory.");
    }

    void replace_file(const fs::path &source, const fs::path &destination) {
#ifdef _WIN32
      // Replace in one same-volume move; do not fall back to copying or deleting the old file.
      if(!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const std::error_code error(static_cast<int>(GetLastError()), std::system_category());
        throw fs::filesystem_error("Cannot replace the progress file", source, destination, error);
      }
#else
      fs::rename(source, destination);
#endif
    }
  }

  ProgressReadResult read_progress(std::string_view filename, std::size_t max_bytes,
                                  std::string &payload, std::string &error) {
    error.clear();
    try {
      const auto path = progress_path(filename);
      payload.clear();
      if(!progress_exists(path)) return ProgressReadResult::missing;

      std::ifstream in(path, std::ios::binary | std::ios::ate);
      const auto size = static_cast<std::streamoff>(in.tellg());
      if(size < 0) throw std::runtime_error("Cannot open or measure the progress file.");
      const auto bytes = static_cast<std::uintmax_t>(size);
      if(bytes > max_bytes || bytes > payload.max_size() || bytes > static_cast<std::uintmax_t>(stream_limit))
        throw std::runtime_error("The progress file exceeds the read limit.");
      payload.resize(static_cast<std::size_t>(size));
      if(!in.seekg(0).read(payload.data(), static_cast<std::streamsize>(size)) ||
         in.peek() != std::char_traits<char>::eof() || !in.eof() || in.bad())
        throw std::runtime_error("Cannot read the complete progress file.");
      return ProgressReadResult::loaded;
    } catch(const std::exception &failure) {
      payload.clear();
      error = failure.what();
      return ProgressReadResult::error;
    }
  }

  bool write_progress(std::string_view filename, std::string_view payload, std::string &error) {
    error.clear();
    try {
      const auto path = progress_path(filename);
      progress_exists(path);
      if(payload.size() > static_cast<std::uintmax_t>(stream_limit))
        throw std::runtime_error("The progress payload is too large to write.");

      // A uniquely reserved directory keeps incomplete writes separate, even
      // if an earlier game process exited before cleaning up its staged file.
      const auto directory = staging_directory(path);
      const auto staged = directory / "payload";
      struct Cleanup {
        const fs::path &file, &directory;
        ~Cleanup() { std::error_code ignored; fs::remove(file, ignored); fs::remove(directory, ignored); }
      } cleanup{staged, directory};
      std::ofstream out(staged, std::ios::binary);
      if(!payload.empty()) out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
      out.close();
      if(!out) throw std::runtime_error("Cannot write the staged progress file.");
      replace_file(staged, path);
      return true;
    } catch(const std::exception &failure) {
      error = failure.what();
      return false;
    }
  }
}
