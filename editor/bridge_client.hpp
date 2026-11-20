#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace editor {
  using Json = nlohmann::json;

  // Set is a UI action; both Set and Console use the bridge's console operation.
  enum class Operation { Connect, Disconnect, Entities, Components, Get, Set, Console, Pause, Resume };

  struct Request {
    Operation operation;
    std::string text = {};
    uint16_t port = 0;
  };

  struct Reply {
    Operation operation;
    Json data = {};
    std::string transport_error = {};
  };

  std::optional<uint16_t> bridge_port(std::string_view address);

  class BridgeClient {
  public:
    BridgeClient();
    ~BridgeClient();

    // One request at a time. Socket IO and reply parsing happen on the worker.
    bool submit(Request request);
    std::optional<Reply> take_reply();

  private:
    void run();

    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Request> request_;
    std::optional<Reply> reply_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
  };
}
