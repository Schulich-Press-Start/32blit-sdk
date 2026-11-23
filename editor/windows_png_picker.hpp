#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>

namespace editor {
  // Return a project-relative path for an existing project PNG, an absolute
  // source path for an import, or an empty string for cancellation.
  inline std::string browse_png(const std::filesystem::path &root) {
    wchar_t name[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"PNG images (*.png)\0*.png\0";
    dialog.lpstrFile = name;
    dialog.nMaxFile = static_cast<DWORD>(std::size(name));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&dialog)) {
      if(CommDlgExtendedError()) throw std::runtime_error("Could not open the system file picker.");
      return {};
    }
    const std::filesystem::path source(name);
    const auto relative = source.lexically_relative(root);
    return !relative.empty() && *relative.begin() != ".." ? relative.generic_u8string() : source.generic_u8string();
  }
}
#endif
