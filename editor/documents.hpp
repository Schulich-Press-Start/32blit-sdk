#pragma once
#include "preview_image.hpp"
#include "ui_preview.hpp"
#include <array>
#include <filesystem>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace editor {
  class Documents {
  public:
    Documents(const std::filesystem::path &root, SDL_Renderer *renderer);
    ~Documents();
    Documents(const Documents &) = delete;
    Documents &operator=(const Documents &) = delete;
    enum Panel { Text, UI, Audio, PanelCount };
    void draw(Panel panel);
    void draw_dialogs();
    bool dirty() const;
    bool active_dirty(Panel panel) const;
    void save_active(Panel panel);
    void save_all();
    void request_open() { open_requested_ = true; }
    bool open(const std::string &path);
    std::optional<Panel> take_panel_request();
    // Return true for any absolute path that must not be deleted while an asset is open.
    void draw_files(const std::function<bool(const std::filesystem::path &)> &is_asset_open);
    std::string take_asset_request();
    void refresh_files();
    std::vector<std::string> tilesets() const;
    std::vector<std::string> pngs() const;
    const std::vector<std::string> &sprite_stems() const { return sprite_stems_; }
    const std::vector<std::string> &scene_paths() const { return scene_paths_; }
  private:
    struct AudioEditor {
      std::vector<std::pair<std::string, std::string>> cues;
      std::size_t selected = std::string::npos;
      std::string id, wav, parse_error, playback_error;
      bool needs_parse = true;

      void parse(const std::string &source);
      void select(std::size_t index);
      void write(std::string &source) const;
    };
    struct Tab {
      std::string path, label;
      PreviewImage image;
      std::string content, original, save_error;
      std::string_view line_ending = "\n";
      std::optional<UiPreview> ui;
      std::optional<AudioEditor> audio;
      std::vector<int> line_starts;
      int cursor = 0, line = 1, column = 1;
      bool bom = false, modified = false, open = true;
      bool show_source = false;

      Panel panel() const { return ui ? UI : audio ? Audio : Text; }
      void index_lines();
      void update_cursor(int offset);
      void content_changed();
    };
    bool save(Tab &tab);
    void draw_document(Tab &tab);
    void draw_text(Tab &tab);
    void draw_audio(Tab &tab);
#ifndef __EMSCRIPTEN__
    std::string play_audio(const std::string &tab_path, const std::string &wav);
    void stop_audio();

    SDL_AudioDeviceID audio_device_ = 0;
    bool audio_initialized_ = false;
    std::string playing_tab_;
#endif
    void draw_close_prompt();
    void erase_closed_tabs();
    void draw_file_tree(std::size_t first, std::size_t last, std::size_t offset);
    void draw_file_actions(std::string_view path, bool directory);
    void draw_file_operation(const std::function<bool(const std::filesystem::path &)> &is_asset_open);
    void paste_file();
    void delete_file(const std::function<bool(const std::filesystem::path &)> &is_asset_open);
    std::filesystem::path root_;
    SDL_Renderer *renderer_;
    struct ProjectFile { std::string path; bool previewable; };
    std::vector<ProjectFile> files_;
    std::vector<std::string> sprite_stems_, scene_paths_, wav_paths_;
    std::vector<Tab> tabs_;
    struct PanelState { std::string active; bool select = false; };
    std::array<PanelState, PanelCount> panels_;
    std::optional<Panel> panel_request_;
    std::string pending_close_, path_, error_, browser_error_;
    std::string asset_request_;
    enum class FileAction { None, Paste, ConfirmDelete };
    FileAction file_action_ = FileAction::None;
    std::string copied_path_, operation_path_;
    bool delete_directory_ = false;
    bool open_requested_ = false;
  };
}
