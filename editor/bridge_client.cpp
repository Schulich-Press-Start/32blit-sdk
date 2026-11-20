#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "bridge_client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace editor {
  namespace {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    constexpr size_t frame_limit = 1024 * 1024; // Includes LF, as on the bridge.
    constexpr char address_prefix[] = "127.0.0.1:";

#ifdef _WIN32
    using Socket = SOCKET;
    using SocketLength = int;
    constexpr Socket invalid_socket = INVALID_SOCKET;
    int last_socket_error() { return WSAGetLastError(); }
    bool interrupted(int error) { return error == WSAEINTR; }
    bool would_block(int error) { return error == WSAEWOULDBLOCK; }
    bool connecting(int error) { return would_block(error) || error == WSAEINPROGRESS || interrupted(error); }
    bool nonblocking(Socket socket) {
      u_long mode = 1;
      return ioctlsocket(socket, FIONBIO, &mode) == 0;
    }
    void close_socket(Socket socket) { closesocket(socket); }

    struct SocketRuntime {
      WSADATA data{};
      int error = WSAStartup(MAKEWORD(2, 2), &data);
      ~SocketRuntime() { if(!error) WSACleanup(); }
    };
#else
    using Socket = int;
    using SocketLength = socklen_t;
    constexpr Socket invalid_socket = -1;
    int last_socket_error() { return errno; }
    bool interrupted(int error) { return error == EINTR; }
    bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
    bool connecting(int error) { return error == EINPROGRESS || interrupted(error); }
    bool nonblocking(Socket socket) {
      const int status = fcntl(socket, F_GETFL);
      return status != -1 && fcntl(socket, F_SETFL, status | O_NONBLOCK) != -1 &&
             fcntl(socket, F_SETFD, FD_CLOEXEC) != -1;
    }
    void close_socket(Socket socket) { close(socket); }
    struct SocketRuntime { int error = 0; };
#endif

    [[noreturn]] void fail_socket(const char *action, int error = last_socket_error()) {
      throw std::runtime_error(std::string(action) + " (socket error " + std::to_string(error) + ")");
    }

    enum class Interest { Read, Write, Connect };

    int ready(Socket socket, Interest interest, std::chrono::milliseconds timeout) {
      fd_set sockets, failed;
      FD_ZERO(&sockets);
      FD_ZERO(&failed);
      FD_SET(socket, &sockets);
      if(interest == Interest::Connect) FD_SET(socket, &failed);
      timeval wait{static_cast<long>(timeout.count() / 1000),
                   static_cast<long>(timeout.count() % 1000 * 1000)};
#ifdef _WIN32
      const int count = 0; // Winsock ignores nfds.
#else
      const int count = socket + 1;
#endif
      return select(count, interest == Interest::Read ? &sockets : nullptr,
                    interest != Interest::Read ? &sockets : nullptr,
                    interest == Interest::Connect ? &failed : nullptr, &wait);
    }

    const char *wire_operation(Operation operation) {
      switch(operation) {
        case Operation::Connect: return "hello";
        case Operation::Disconnect: break; // Local only.
        case Operation::Entities: return "entities";
        case Operation::Components: return "components";
        case Operation::Get: return "get";
        case Operation::Set:
        case Operation::Console: return "console";
        case Operation::Pause: return "pause";
        case Operation::Resume: return "resume";
      }
      throw std::logic_error("Not a bridge operation");
    }

    class Connection {
    public:
      explicit Connection(const std::atomic<bool> &stopping) : stopping_(stopping) {}
      ~Connection() { close(); }
      bool open() const { return socket_ != invalid_socket; }

      void close() {
        if(open()) close_socket(socket_);
        socket_ = invalid_socket;
      }

      void connect(uint16_t port, Clock::time_point deadline) {
        close();
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(!open()) fail_socket("Could not create socket");
#ifndef _WIN32
        if(socket_ >= FD_SETSIZE) throw std::runtime_error("Socket exceeds select descriptor limit");
#endif
        if(!nonblocking(socket_)) fail_socket("Could not set socket nonblocking");
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if(setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
          fail_socket("Could not suppress SIGPIPE");
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if(::connect(socket_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) return;
        const auto error = last_socket_error();
        if(!connecting(error)) fail_socket("Could not connect", error);
        wait(Interest::Connect, deadline);
        int result = 0;
        SocketLength length = sizeof(result);
        if(getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&result), &length) != 0)
          fail_socket("Could not check connection");
        if(result) fail_socket("Could not connect", result);
      }

      Json exchange(const Request &request, Clock::time_point deadline) {
        if(!open()) throw std::runtime_error("Bridge is disconnected");
        const auto id = ++id_;
        const auto operation = request.operation;
        auto payload = operation == Operation::Get || operation == Operation::Set || operation == Operation::Console
          ? Json(request.text) : Json(1);
        wire_ = Json{{"id", id}, {wire_operation(operation), std::move(payload)}}.dump();
        wire_.push_back('\n');
        if(wire_.size() > frame_limit) throw std::runtime_error("Request exceeds bridge frame limit");
        size_t sent = 0;
        while(sent < wire_.size()) {
          wait(Interest::Write, deadline);
#ifdef MSG_NOSIGNAL
          constexpr int flags = MSG_NOSIGNAL;
#else
          constexpr int flags = 0;
#endif
          const auto count = send(socket_, wire_.data() + sent, static_cast<int>(wire_.size() - sent), flags);
          if(count > 0) sent += static_cast<size_t>(count);
          else if(count == 0) throw std::runtime_error("Bridge closed the connection");
          else {
            const auto error = last_socket_error();
            if(!would_block(error) && !interrupted(error)) fail_socket("Could not send request", error);
          }
        }

        wire_.clear();
        std::array<char, 8192> chunk;
        for(;;) {
          wait(Interest::Read, deadline);
          const auto count = recv(socket_, chunk.data(), static_cast<int>(chunk.size()), 0);
          if(count == 0) throw std::runtime_error("Bridge closed the connection");
          if(count < 0) {
            const auto error = last_socket_error();
            if(!would_block(error) && !interrupted(error)) fail_socket("Could not receive reply", error);
            continue;
          }
          if(wire_.size() + static_cast<size_t>(count) > frame_limit)
            throw std::runtime_error("Reply exceeds bridge frame limit");
          const auto end = chunk.data() + count;
          const auto lf = std::find(chunk.data(), end, '\n');
          wire_.append(chunk.data(), lf);
          if(lf != end) {
            if(lf + 1 != end) throw std::runtime_error("Unexpected data after bridge reply");
            break;
          }
          if(wire_.size() == frame_limit) throw std::runtime_error("Unterminated bridge reply");
        }
        auto reply = Json::parse(wire_);
        if(!reply.at("id").is_number_integer() || reply.at("id") != id || !reply.at("ok").is_boolean())
          throw std::runtime_error("Invalid bridge reply envelope");
        return reply;
      }

      void check_idle() {
        const int status = ready(socket_, Interest::Read, 0ms);
        if(status == 0) return;
        if(status < 0) {
          const auto error = last_socket_error();
          if(interrupted(error)) return;
          fail_socket("Could not check connection", error);
        }
        char byte;
        const auto count = recv(socket_, &byte, 1, 0);
        if(count == 0) throw std::runtime_error("Bridge closed the connection");
        if(count > 0) throw std::runtime_error("Unexpected data from idle bridge");
        const auto error = last_socket_error();
        if(!would_block(error) && !interrupted(error)) fail_socket("Bridge connection failed", error);
      }

    private:
      void wait(Interest interest, Clock::time_point deadline) {
        while(!stopping_.load()) {
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
          if(remaining <= 0ms) throw std::runtime_error("Timed out waiting for the bridge");
          const auto status = ready(socket_, interest, std::min(50ms, remaining));
          if(status > 0) return;
          if(status < 0) {
            const auto error = last_socket_error();
            if(!interrupted(error)) fail_socket("Could not wait for bridge socket", error);
          }
        }
        throw std::runtime_error("Editor is closing");
      }

      const std::atomic<bool> &stopping_;
      Socket socket_ = invalid_socket;
      uint32_t id_ = 0;
      std::string wire_;
    };
  }

  std::optional<uint16_t> bridge_port(std::string_view address) {
    const std::string_view prefix = address_prefix;
    if(address.substr(0, prefix.size()) != prefix) return {};
    address.remove_prefix(prefix.size());
    uint16_t port = 0;
    const auto end = address.data() + address.size();
    const auto parsed = std::from_chars(address.data(), end, port);
    if(parsed.ec != std::errc{} || parsed.ptr != end || port == 0) return {};
    return port;
  }

  BridgeClient::BridgeClient() : worker_(&BridgeClient::run, this) {}

  BridgeClient::~BridgeClient() {
    {
      std::lock_guard lock(mutex_);
      stopping_.store(true);
    }
    changed_.notify_one();
    worker_.join();
  }

  bool BridgeClient::submit(Request request) {
    {
      std::lock_guard lock(mutex_);
      if(request_ || reply_) return false;
      request_.emplace(std::move(request));
    }
    changed_.notify_one();
    return true;
  }

  std::optional<Reply> BridgeClient::take_reply() {
    std::optional<Reply> reply;
    {
      std::lock_guard lock(mutex_);
      if(!reply_) return {};
      reply.swap(reply_);
    }
    changed_.notify_one();
    return reply;
  }

  void BridgeClient::run() {
    const SocketRuntime runtime;
    Connection connection(stopping_);
    std::unique_lock lock(mutex_);
    while(!stopping_.load()) {
      if(!request_) {
        if(!connection.open() || reply_) {
          changed_.wait(lock);
        } else if(!changed_.wait_for(lock, 250ms, [&] { return request_.has_value() || stopping_.load(); })) {
          try {
            connection.check_idle();
          } catch(const std::runtime_error &error) {
            connection.close();
            reply_.emplace(Reply{Operation::Disconnect, {}, error.what()});
          }
        }
        continue;
      }

      // Leave the moved-from request in the mailbox until its reply is ready.
      // This prevents a second submission from overtaking an active request.
      const auto request = std::move(*request_);
      lock.unlock();
      Reply reply{request.operation};
      try {
        if(request.operation == Operation::Disconnect) {
          connection.close();
        } else {
          if(runtime.error) fail_socket("Could not initialize sockets", runtime.error);
          const auto deadline = Clock::now() + 5s;
          if(request.operation == Operation::Connect) connection.connect(request.port, deadline);
          reply.data = connection.exchange(request, deadline);
          if(request.operation == Operation::Connect) {
            if(!reply.data.at("ok").get<bool>()) connection.close();
            else if(reply.data.at("protocol") != 1)
              throw std::runtime_error("Unsupported bridge protocol (expected 1)");
            else if(!reply.data.at("world").is_boolean() || !reply.data.at("title").is_string())
              throw std::runtime_error("Invalid bridge hello reply");
          }
        }
      } catch(const std::exception &error) {
        connection.close();
        reply.transport_error = error.what();
      }
      lock.lock();
      request_.reset();
      reply_.emplace(std::move(reply));
    }
  }
}
