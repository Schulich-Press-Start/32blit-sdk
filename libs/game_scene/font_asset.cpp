#include "font_asset.hpp"

#include "font_path.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <utility>

namespace sdk {
  namespace fs = std::filesystem;
  namespace {
    bool fail(std::string *error, const char *reason) {
      if(error) *error = reason;
      return false;
    }
  }

  bool FontAsset::load(const fs::path &path, std::string *error) {
    std::error_code ec;
    if(path.empty() || path.native().find(fs::path::value_type{}) != fs::path::string_type::npos ||
       !fs::is_regular_file(path, ec)) return fail(error, "Font is missing or is not a regular file.");

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if(!input) return fail(error, "Cannot open font file.");
    const auto size = static_cast<std::streamoff>(input.tellg());
    std::array<uint8_t, 8> header;
    if(!input.seekg(0).read(reinterpret_cast<char *>(header.data()), header.size()))
      return fail(error, "Cannot read the packed FONT header.");
    constexpr std::array<uint8_t, 4> magic{'F', 'O', 'N', 'T'};
    if(!std::equal(magic.begin(), magic.end(), header.begin())) return fail(error, "Expected packed FONT data.");

    const std::size_t count = header[4], width = header[5], height = header[6];
    // Native drawing indexes the full space-to-DEL range. Advances are independent
    // of bitmap width: ttblit permits zero advance and includes horizontal spacing.
    if(count < 96 || !width || !height) return fail(error, "FONT needs at least 96 glyphs and nonzero cell dimensions.");
    const auto expected = header.size() + count + count * width * ((height + 7) / 8);
    if(size != static_cast<std::streamoff>(expected)) return fail(error, "FONT file size does not match its header.");

    std::vector<uint8_t> bytes(expected);
    std::copy(header.begin(), header.end(), bytes.begin());
    if(!input.read(reinterpret_cast<char *>(bytes.data() + header.size()),
                   static_cast<std::streamsize>(expected - header.size())) ||
       input.peek() != std::char_traits<char>::eof() || !input.eof() || input.bad())
      return fail(error, "Cannot read the complete FONT file.");
    bytes_ = std::move(bytes);
    if(error) error->clear();
    return true;
  }

  bool FontAsset::load_project(const fs::path &root, std::string_view relative, std::string *error) {
    if(!is_project_font_path(relative)) return fail(error, "Expected a project-relative .font path using /.");
    if(root.empty() || root.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
      return fail(error, "A project root is required for a font path.");
    try {
      std::error_code ec;
      const auto base = fs::canonical(root, ec);
      if(ec || !fs::is_directory(base, ec)) return fail(error, "Cannot resolve the project directory.");
      const auto path = fs::canonical(base / fs::u8path(relative), ec);
      if(ec) return fail(error, "Cannot resolve the font file inside the project.");
      const auto local = path.lexically_relative(base);
      if(local.empty() || *local.begin() == "..") return fail(error, "Font path leaves the project directory.");
      return load(path, error);
    } catch(const fs::filesystem_error &) {
      return fail(error, "Cannot resolve the font file inside the project.");
    }
  }

  blit::Font FontAsset::font() const {
    return bytes_.empty() ? blit::minimal_font : blit::Font(bytes_.data());
  }
}
