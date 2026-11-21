#include "project.hpp"
#ifndef __EMSCRIPTEN__
#include <imgui.h>
#include <cfloat>
#endif
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>


namespace editor {
  namespace fs = std::filesystem;
  using Json = nlohmann::json;

  static std::string get_text(const Json &object, const char *key) {
    auto value = object.at(key).get<std::string>();
    if(value.empty() || value.find('\0') != value.npos) throw std::runtime_error(std::string("Invalid project setting: ") + key);
    return value;
  }

  static std::string get_path(const Json &object, const char *key) {
    auto value = get_text(object, key);
    auto path = fs::u8path(value);
    if(path.has_root_path()) throw std::runtime_error(std::string("Expected project-relative path: ") + key);
    for(const auto &part : path) if(part == "..") throw std::runtime_error(std::string("Invalid project path: ") + key);
    return value;
  }

  Project::Project(const char *directory) {
    if(!directory) return;
    auto root = fs::canonical(fs::u8path(directory));
    std::ifstream input(root / "blit-project.json");
    if(!input) throw std::runtime_error("Cannot open " + (root / "blit-project.json").u8string());
    const auto config = Json::parse(input);
    scene_ = get_path(config, "scene");
#ifdef __EMSCRIPTEN__
    if(!fs::is_regular_file(root / fs::u8path(scene_)))
      throw std::runtime_error("Missing startup scene: " + scene_);
    if(config.contains("registry")) registry_ = get_path(config, "registry");
    else {
      registry_ = "registry.json";
      if(!fs::is_regular_file(root / registry_)) {
        for(const auto *key : {"windows", "linux"}) {
          const auto platform = config.find(key);
          if(platform == config.end() || !platform->contains("build_dir")) continue;
          const auto candidate = fs::u8path(get_path(*platform, "build_dir")) / "registry.json";
          if(!fs::is_regular_file(root / candidate)) continue;
          registry_ = candidate.generic_u8string();
          break;
        }
      }
    }
#else
#ifdef _WIN32
    const auto platform = config.find("windows");
#else
    const auto platform = config.find("linux");
#endif
    if(platform != config.end() && !platform->is_object())
      throw std::runtime_error("Expected a platform settings object.");
#ifdef _WIN32
    if(platform != config.end() && platform->contains("play_exe"))
      play_exe_ = get_path(*platform, "play_exe");
#endif
    if(platform != config.end() && (play_exe_.empty() || platform->contains("preset") ||
                                    platform->contains("build_dir") || platform->contains("exe"))) {
      preset_ = get_text(*platform, "preset");
      build_ = get_path(*platform, "build_dir");
      exe_ = get_path(*platform, "exe");
      target_ = get_text(config, "target");
      if(const auto watch = config.find("scene_watch"); watch != config.end()) {
        if(!watch->is_array()) throw std::runtime_error("Expected a scene_watch path array.");
        for(const auto &entry : *watch) scene_watch_.push_back(get_path(Json{{"scene_watch", entry}}, "scene_watch"));
      }
    }
    registry_ = config.contains("registry") ? get_path(config, "registry")
      : (fs::u8path(build_) / "registry.json").generic_u8string();
#endif
    name_ = root.filename().u8string();
    root_ = std::move(root);
    status_ = "Ready";
  }

#ifndef __EMSCRIPTEN__
  bool Project::running() const { return phase_ == Phase::Play || (phase_ == Phase::Dev && ready_); }

  void Project::start(bool dev, const std::string &address) {
    if(active() || !(dev ? build_configured() : play_configured())) return;
    dev_ = dev;
    configure_only_ = false;
    ready_ = false;
    address_ = address;
    log_.clear();
    try {
      if(!dev && !play_exe_.empty()) {
        launch_play(play_exe_, "Playing (packaged game)");
      } else if(dev && fs::is_regular_file(root_ / fs::u8path(build_) / "CMakeCache.txt")) {
        build_or_watch();
      } else {
        configure();
      }
    } catch(const std::exception &error) { status_ = error.what(); }
  }

  void Project::configure() {
    process_.start(root_, {"cmake", "--preset", preset_, "-B", build_});
    phase_ = Phase::Configure;
    status_ = "Configuring...";
  }

  void Project::reconfigure() {
    if(!build_configured() || active()) return;
    configure_only_ = true;
    log_.clear();
    try { configure(); }
    catch(const std::exception &error) { status_ = error.what(); }
  }

  void Project::build_or_watch() {
    if(dev_) {
      std::vector<std::string> args{"blit", "dev", "--build-dir", build_, "--target", target_, "--exe", exe_,
                                    "--connect", address_, "--build-on-save", "--scene", scene_};
      auto map = fs::u8path(scene_);
      map.replace_extension(".map");
      if(fs::is_regular_file(root_ / map)) {
        args.insert(args.end(), {"--scene-watch", map.generic_u8string()});
        map += ".json";
        if(std::ifstream association(root_ / map); association) {
          args.insert(args.end(), {"--scene-watch", map.generic_u8string()});
          const auto metadata = Json::parse(association, nullptr, false);
          if(metadata.is_object() && metadata.contains("tileset") && metadata["tileset"].is_string()) {
            const auto tileset = get_path(metadata, "tileset");
            args.insert(args.end(), {"--scene-watch", tileset});
          }
        }
      }
      for(const auto &path : scene_watch_) args.insert(args.end(), {"--scene-watch", path});
      process_.start(root_, args);
      phase_ = Phase::Dev;
      status_ = "Starting Dev...";
    } else {
      process_.start(root_, {"cmake", "--build", build_, "--config", "Debug", "--target", target_});
      phase_ = Phase::Build;
      status_ = "Building...";
    }
  }

  void Project::launch_play(const std::string &exe, const char *status) {
    std::vector<std::string> args{(root_ / fs::u8path(exe)).u8string()};
    if(play_exe_.empty()) args.insert(args.end(), {"--bridge", address_});
    process_.start(root_, args);
    phase_ = Phase::Play;
    status_ = status;
  }

  void Project::tick() {
    if(!active()) return;
    auto text = process_.read_log();
    log_ += text;
    if(log_.size() > 32768) log_.erase(0, log_.size() - 32768);
    if(phase_ == Phase::Dev && !ready_ && log_.find("Connected:") != log_.npos) {
      ready_ = true;
      status_ = "Dev running";
    }
    // Drain queued output before releasing the process and its pipe.
    if(text.size() == 8192) return;
    const auto result = process_.poll();
    if(!result) return;
    const auto previous = phase_;
    phase_ = Phase::Idle;
    if(*result != 0) { status_ = "Process exited (" + std::to_string(*result) + "); see build / game log."; return; }
    try {
      if(previous == Phase::Configure) {
        if(configure_only_) status_ = "Configured";
        else build_or_watch();
      } else if(previous == Phase::Build) {
        launch_play(exe_, "Playing (fresh game)");
      } else status_ = "Game exited";
    } catch(const std::exception &error) { status_ = error.what(); }
  }

  void Project::stop() {
    process_.stop();
    phase_ = Phase::Idle;
    status_ = "Stopped";
  }

  void Project::draw_log() {
    ImGui::InputTextMultiline("##project log", log_.data(), log_.size() + 1,
                              ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
  }
#endif
}
