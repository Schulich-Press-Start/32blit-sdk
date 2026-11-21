#pragma once
#include "preview_image.hpp"
#include <filesystem>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace editor {
  class Documents {
  public:
    Documents(const std::filesystem::path &root, SDL_Renderer *renderer);
    void draw();
    bool dirty() const;
    bool active_dirty() const;
    void save(bool all);
    void request_open() { open_requested_ = true; }
    // Return true for any absolute path that must not be deleted while an asset is open.
    bool draw_files(const std::function<bool(const std::filesystem::path &)> &is_asset_open);
    std::string take_asset_request();
    void refresh_files();
    std::vector<std::string> tilesets() const;
    std::vector<std::string> pngs() const;
  private:
    struct Tab {
      std::string path, label;
      PreviewImage image;
      std::string content, original, save_error;
      std::string_view line_ending = "\n";
      std::vector<int> line_starts;
      int cursor = 0, line = 1, column = 1;
      bool bom = false, modified = false, open = true;

      void index_lines();
      void update_cursor(int offset);
    };
    bool save(Tab &tab);
    void draw_text(Tab &tab);
    void draw_close_prompt();
    bool open(const std::string &path);
    bool draw_file_tree(std::size_t first, std::size_t last, std::size_t offset);
    void draw_file_actions(std::string_view path, bool directory);
    void draw_file_operation(const std::function<bool(const std::filesystem::path &)> &is_asset_open);
    void paste_file();
    void delete_file(const std::function<bool(const std::filesystem::path &)> &is_asset_open);
    std::filesystem::path root_;
    SDL_Renderer *renderer_;
    struct ProjectFile { std::string path; bool previewable; };
    std::vector<ProjectFile> files_;
    std::vector<Tab> tabs_;
    std::string selected_, active_, pending_close_, path_, error_, browser_error_;
    std::string asset_request_;
    enum class FileAction { None, Paste, ConfirmDelete };
    FileAction file_action_ = FileAction::None;
    std::string copied_path_, operation_path_;
    bool delete_directory_ = false;
    bool open_requested_ = false;
  };

  void workspace_style();
}
