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
    bool active() const { return process_.active(); }
    bool running() const;
    void start(bool dev, const std::string &address);
    void reconfigure();
    void stop();
    void tick();
    void draw_log() const;
#endif
    const std::string &status() const { return status_; }
    const std::string &name() const { return name_; }
    const std::filesystem::path &root() const { return root_; }
    const std::string &scene() const { return scene_; }
    const std::string &registry() const { return registry_; }
  private:
    std::filesystem::path root_;
    std::string name_, scene_, registry_, status_;
#ifndef __EMSCRIPTEN__
    enum class Phase { Idle, Configure, Build, Play, Dev };
    void configure();
    void build_or_watch();
    Process process_;
    Phase phase_ = Phase::Idle;
    std::string preset_, build_, exe_, target_, address_, log_;
    std::vector<std::string> scene_watch_;
    bool dev_ = false, ready_ = false;
    bool configure_only_ = false;
#endif
  };
}
