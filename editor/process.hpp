#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace editor {
  // Owns one process group and a bounded-per-frame log pipe.
  class Process {
  public:
    Process();
    ~Process();
    bool active() const;
    void start(const std::filesystem::path &directory, const std::vector<std::string> &args);
    std::string read_log();
    std::optional<int> poll();
    void stop();
  private:
    struct State;
    std::unique_ptr<State> state_;
  };
}
