#pragma once

#include "preview_image.hpp"
#include "sprite_clips.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace editor {
  class SpriteAssets {
  public:
    SpriteAssets(SDL_Renderer *renderer, const std::filesystem::path &root);
    void open(const std::string &path);
    void draw(const std::function<std::vector<std::string>()> &list_pngs,
              const std::function<void()> &refresh_files);
    const std::string &active_path() const { return active_tab_; }
    bool active_dirty() const;
    bool playing() const;
    bool dirty() const;
    bool can_history(bool redo) const;
    void history(bool redo);
    void save(bool all);
    void revert(const std::string &path);

  private:
    struct Revision {
      sdk::SpriteClips descriptor;
      std::string clip;
      size_t frame;
      uint64_t id;
    };
    struct Tab {
      sdk::SpriteClips descriptor;
      std::optional<std::string> original;
      PreviewImage preview;
      std::string error, clip;
      std::array<char, 128> clip_name{};
      int width = 8, height = 8;
      unsigned cell = 0;
      size_t frame = 0;
      uint64_t started = 0, elapsed = 0;
      std::vector<Revision> undo, redo;
      uint64_t revision = 0, saved_revision = 0, next_revision = 0;
      bool editing_duration = false;
      bool loaded = false, dirty = false, playing = false;
    };

    void load(const std::string &path, Tab &tab);
    void suggest_draft(std::string image);
    void create();
    void draw_tab(Tab &tab, const std::function<std::vector<std::string>()> &list_pngs);
    void draw_atlas(Tab &tab);
    void draw_clip(Tab &tab);
    static void record_edit(Tab &tab);
    static void changed(Tab &tab);

    SDL_Renderer *renderer_;
    std::filesystem::path root_;
    std::map<std::string, Tab> tabs_;
    std::string active_tab_, pending_tab_, error_, new_path_, new_image_, new_error_;
    std::vector<std::string> pngs_;
    bool refresh_pending_ = false;
  };
}
