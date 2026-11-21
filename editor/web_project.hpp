#pragma once
#include <filesystem>
#include <string_view>

namespace editor {
  inline constexpr char web_import_zip[] = "/editor-import.zip";
  inline constexpr char web_export_zip[] = "/editor-export.zip";

  // Extract into a new staging directory and return the project root within it.
  std::filesystem::path unpack_web_project(const std::filesystem::path &staging);
  std::filesystem::path create_web_project(const std::filesystem::path &staging, std::string_view name);
  void pack_web_project(const std::filesystem::path &root);
}
