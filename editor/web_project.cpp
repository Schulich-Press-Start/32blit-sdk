#include "web_project.hpp"
#include "registry.hpp"
#include <nlohmann/json.hpp>
#include <miniz.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace editor {
  namespace {
    namespace fs = std::filesystem;

    constexpr std::uintmax_t mib = 1024 * 1024;
    constexpr auto max_archive_size = 128 * mib;
    constexpr auto max_project_size = 96 * mib;
    constexpr auto max_file_size = 24 * mib;
    constexpr std::size_t max_entries = 4096;
    constexpr std::size_t max_depth = 32;
    constexpr char project_file[] = "blit-project.json";

    void write_starter_file(const fs::path &path, std::string_view data) {
      std::ofstream file(path, std::ios::binary);
      file.write(data.data(), data.size());
      file.close();
      if(!file) throw std::runtime_error("Could not write starter file: " + path.filename().u8string());
    }

    struct Zip {
      mz_zip_archive handle{};
      ~Zip() { if(handle.m_pState) mz_zip_end(&handle); }

      void require(bool ok, std::string_view message) {
        if(!ok) throw std::runtime_error(std::string(message) + ": " + mz_zip_get_error_string(mz_zip_peek_last_error(&handle)));
      }
    };

    // MEMFS filenames cross the Wasm/JS boundary as UTF-8. Don't let invalid
    // byte sequences collapse to the same replacement characters on extraction.
    bool valid_utf8(std::string_view name) {
      unsigned remaining = 0, lower = 0x80, upper = 0xbf;
      for(unsigned char byte : name) {
        if(remaining) {
          if(byte < lower || byte > upper) return false;
          --remaining;
          lower = 0x80;
          upper = 0xbf;
        } else if(byte < 0x80) continue;
        else if(byte >= 0xc2 && byte <= 0xdf) remaining = 1;
        else if(byte >= 0xe0 && byte <= 0xef) {
          remaining = 2;
          if(byte == 0xe0) lower = 0xa0;
          if(byte == 0xed) upper = 0x9f;
        } else if(byte >= 0xf0 && byte <= 0xf4) {
          remaining = 3;
          if(byte == 0xf0) lower = 0x90;
          if(byte == 0xf4) upper = 0x8f;
        } else return false;
      }
      return remaining == 0;
    }

    void validate_path(std::string_view name) {
      const auto invalid = [] { throw std::runtime_error("ZIP contains an unsafe or overlong project path."); };
      if(name.empty() || name.size() > MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE - 2 || !valid_utf8(name) ||
         name.find_first_of("\\:\"<>|?*") != name.npos) invalid();
      for(unsigned char byte : name) if(byte < 32 || byte == 127) invalid();
      std::size_t depth = 0;
      for(;;) {
        const auto slash = name.find('/');
        const auto part = name.substr(0, slash);
        if(++depth > max_depth || part.empty() || part.size() > 255 || part.back() == '.' || part.back() == ' ' ||
           part == "__proto__") invalid();
        if(slash == name.npos) break;
        name.remove_prefix(slash + 1);
      }
    }

    // ASCII case collisions are ambiguous when a ZIP is unpacked on Windows.
    struct PathOrder {
      using is_transparent = void;

      bool operator()(std::string_view a, std::string_view b) const {
        const auto fold = [](unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                           [&](char lhs, char rhs) { return fold(lhs) < fold(rhs); });
      }
    };

    struct Entry {
      bool directory;
      std::optional<mz_uint> index;
      std::uintmax_t size;
    };

    struct ProjectEntries {
      std::map<std::string, Entry, PathOrder> paths;
      std::uintmax_t total = 0;

      void add(std::string_view path, bool directory, mz_uint index, std::uintmax_t size) {
        validate_path(path);
        if(size > max_file_size || size > max_project_size - total)
          throw std::runtime_error("Project exceeds the ZIP limits (24 MiB per file, 96 MiB total).");
        total += size;
        for(auto slash = path.find('/'); slash != path.npos; slash = path.find('/', slash + 1))
          add_path(path.substr(0, slash), true, std::nullopt, 0);
        add_path(path, directory, index, size);
      }

      bool has_file(std::string_view path) const {
        const auto it = paths.find(path);
        return it != paths.end() && it->first == path && !it->second.directory;
      }

    private:
      void add_path(std::string_view path, bool directory, std::optional<mz_uint> index, std::uintmax_t size) {
        const auto existing = paths.lower_bound(path);
        if(existing != paths.end() && !PathOrder{}(path, existing->first)) {
          auto &entry = existing->second;
          if(existing->first != path || !entry.directory || !directory || (entry.index && index))
            throw std::runtime_error("ZIP contains conflicting project paths: " + std::string(path));
          if(index) entry.index = index;
        } else {
          if(paths.size() == max_entries) throw std::runtime_error("Project ZIP has more than 4096 files and folders.");
          paths.emplace_hint(existing, std::string(path), Entry{directory, index, size});
        }
      }
    };

    std::string project_prefix(const ProjectEntries &entries) {
      if(entries.has_file(project_file)) return {};
      if(!entries.paths.empty()) {
        const auto prefix = entries.paths.begin()->first + '/';
        const std::string_view folder(prefix.data(), prefix.size() - 1);
        if(entries.has_file(prefix + project_file)) {
          for(const auto &item : entries.paths) {
            const std::string_view path = item.first;
            if(path != folder && path.substr(0, prefix.size()) != prefix)
              throw std::runtime_error("ZIP must contain just one wrapped project folder.");
          }
          return std::string(folder);
        }
      }
      throw std::runtime_error("ZIP must contain blit-project.json at its root or in a single project folder.");
    }

    struct ExtractFile {
      std::FILE *file;
      std::uintmax_t size;

      static std::size_t write(void *opaque, mz_uint64 offset, const void *data, std::size_t count) {
        auto &out = *static_cast<ExtractFile *>(opaque);
        if(offset > out.size || count > out.size - offset) return 0;
        return std::fwrite(data, 1, count, out.file);
      }
    };
  }

  fs::path unpack_web_project(const fs::path &staging) {
    if(fs::file_size(web_import_zip) > max_archive_size) throw std::runtime_error("Project ZIP is larger than 128 MiB.");
    Zip zip;
    zip.require(mz_zip_reader_init_file(&zip.handle, web_import_zip, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY),
                "Cannot open project ZIP");
    const auto count = mz_zip_reader_get_num_files(&zip.handle);
    if(count > max_entries) throw std::runtime_error("Project ZIP has more than 4096 files and folders.");
    ProjectEntries entries;
    for(mz_uint i = 0; i < count; ++i) {
      mz_zip_archive_file_stat stat{};
      zip.require(mz_zip_reader_file_stat(&zip.handle, i, &stat), "Cannot read ZIP entry");
      const auto len = mz_zip_reader_get_filename(&zip.handle, i, nullptr, 0);
      if(len < 2 || len > sizeof(stat.m_filename)) throw std::runtime_error("ZIP entry has an invalid filename length.");
      std::string_view filename(stat.m_filename, len - 1);
      const bool directory = stat.m_is_directory;
      // The upper mode bits are used by Unix ZIP writers for links and special files.
      const auto type = (stat.m_external_attr >> 16) & 0170000;
      if(!stat.m_is_supported || (type != 0 && type != (directory ? 0040000u : 0100000u)))
        throw std::runtime_error("ZIP contains a link, special file, or unsupported compression.");
      if(stat.m_local_header_ofs >= zip.handle.m_central_directory_file_ofs ||
         stat.m_comp_size > zip.handle.m_central_directory_file_ofs - stat.m_local_header_ofs ||
         (!stat.m_comp_size && stat.m_uncomp_size) || (!stat.m_uncomp_size && stat.m_crc32) ||
         (directory && stat.m_uncomp_size)) throw std::runtime_error("ZIP contains invalid entry sizes or offsets.");
      if(directory && filename.back() == '/') filename.remove_suffix(1);
      entries.add(filename, directory, i, stat.m_uncomp_size);
      zip.require(mz_zip_validate_file(&zip.handle, i, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY), "Invalid ZIP entry header");
    }
    const auto prefix = project_prefix(entries);
    if(!fs::create_directories(staging)) throw std::runtime_error("Could not create project staging directory.");
    for(const auto &[name, entry] : entries.paths) {
      const auto path = staging / fs::u8path(name);
      if(entry.directory) {
        if(!fs::create_directory(path)) throw std::runtime_error("Cannot create ZIP project folder: " + name);
        continue;
      }
      ExtractFile out{std::fopen(path.c_str(), "wbx"), entry.size};
      if(!out.file) throw std::runtime_error("Cannot create ZIP project file: " + name);
      const bool ok = mz_zip_reader_extract_to_callback(&zip.handle, *entry.index, ExtractFile::write, &out, 0);
      const bool closed = std::fclose(out.file) == 0;
      if(!closed) throw std::runtime_error("Could not write ZIP project file: " + name);
      if(!ok) zip.require(false, "Cannot extract " + name);
    }
    return prefix.empty() ? staging : staging / fs::u8path(prefix);
  }

  fs::path create_web_project(const fs::path &staging, std::string_view name) {
    if(name.empty() || name.size() > 80 || name.find_first_of("/\\:\"<>|?*") != name.npos ||
       name.front() == ' ' || name.back() == ' ' || name.back() == '.' || name == "__proto__" ||
       !valid_utf8(name) || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
      throw std::runtime_error("Enter a project name of up to 80 UTF-8 bytes without filename punctuation or control characters.");

    constexpr char scene_file[] = "scenes/main.scene";
    constexpr char registry_file[] = "registry.json";
    const auto root = staging / fs::u8path(name);
    if(!fs::create_directories(root / "scenes") || !fs::create_directory(root / "assets"))
      throw std::runtime_error("Could not create starter project folders.");

    using Json = nlohmann::json;
    const Json manifest = {{"name", name}, {"scene", scene_file}, {"registry", registry_file}};
    auto components = Json::object();
    for(uint16_t i = 0; i < sdk::component_count(); ++i) {
      const auto &info = *sdk::component_by_id(i);
      auto fields = Json::object();
      for(uint8_t j = 0; j < info.field_count; ++j) {
        const auto &field = info.fields[j];
        fields[field.name] = sdk::field_type_name(field.type);
      }
      components[info.name] = std::move(fields);
    }
    const Json registry = {{"version", 1}, {"components", std::move(components)}};
    write_starter_file(root / project_file, manifest.dump(2) + '\n');
    write_starter_file(root / registry_file, registry.dump(2) + '\n');
    write_starter_file(root / scene_file, "scene 1 main\nentity origin\n  Transform2D\n");
    return root;
  }

  void pack_web_project(const fs::path &root) {
    ProjectEntries entries;
    for(const auto &file : fs::recursive_directory_iterator(root)) {
      const auto type = file.symlink_status();
      const bool directory = fs::is_directory(type);
      if(!directory && !fs::is_regular_file(type)) throw std::runtime_error("Project contains a link or special file.");
      const auto name = file.path().lexically_relative(root).generic_u8string();
      entries.add(name, directory, 0, directory ? 0 : file.file_size());
    }
    try {
      Zip zip;
      zip.require(mz_zip_writer_init_file(&zip.handle, web_export_zip, 0), "Cannot create project ZIP");
      for(const auto &[name, entry] : entries.paths) {
        if(entry.directory) {
          const auto folder = name + '/';
          zip.require(mz_zip_writer_add_mem(&zip.handle, folder.c_str(), nullptr, 0, 0), "Cannot add project folder");
        } else {
          const auto path = root / fs::u8path(name);
          const bool ok = mz_zip_writer_add_file(&zip.handle, name.c_str(), path.c_str(), nullptr, 0,
                                                 MZ_DEFAULT_LEVEL | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE);
          if(!ok) zip.require(false, "Cannot add " + name);
        }
      }
      zip.require(mz_zip_writer_finalize_archive(&zip.handle), "Cannot finalize project ZIP");
      zip.require(mz_zip_writer_end(&zip.handle), "Cannot close project ZIP");
      if(fs::file_size(web_export_zip) > max_archive_size) throw std::runtime_error("Project ZIP is larger than 128 MiB.");
    } catch(...) {
      std::error_code error;
      fs::remove(web_export_zip, error);
      throw;
    }
  }
}
