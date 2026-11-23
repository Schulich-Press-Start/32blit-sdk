#pragma once
#ifndef __EMSCRIPTEN__
#include "process.hpp"
#endif
#include <filesystem>
#include <string>
#include <vector>

namespace editor {
  class Project {
  public:
    explicit Project(const char *directory);
    bool loaded() const { return !root_.empty(); }
#ifndef __EMSCRIPTEN__
    bool build_configured() const { return !target_.empty(); }
    bool has_scene_preview() const { return !preview_exe_.empty(); }
    bool has_packaged_game() const { return !play_exe_.empty(); }
    bool active() const { return process_.active(); }
    bool running() const;
    bool scene_preview_running() const;
    enum class Launch { ScenePreview, PackagedGame, Build, Dev };
    void start(Launch mode, const std::string &address);
    void restart_scene_preview();
    void reconfigure();
    void stop();
    void tick();
    void draw_log();
#endif
    const std::string &status() const { return status_; }
    const std::string &name() const { return name_; }
    const std::filesystem::path &root() const { return root_; }
    const std::string &scene() const { return scene_; }
    const std::string &registry() const { return registry_; }
    const std::vector<std::string> &collision_categories() const { return collision_categories_; }
  private:
    std::filesystem::path root_;
    std::string name_, scene_, registry_, status_;
    std::vector<std::string> collision_categories_;
#ifndef __EMSCRIPTEN__
    enum class Phase { Idle, Configure, Build, Play, Dev };
    void configure();
    void build_or_watch();
    void launch_play(const std::string &exe, const char *status);
    Process process_;
    Phase phase_ = Phase::Idle;
    Launch mode_ = Launch::Build;
    std::string preset_, build_, exe_, play_exe_, preview_exe_, target_, address_, log_;
    std::vector<std::string> scene_watch_;
    bool ready_ = false;
    bool configure_only_ = false;
#endif
  };
}
