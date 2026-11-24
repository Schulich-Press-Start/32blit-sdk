#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <filesystem>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "bridge.hpp"
#include "json.hpp"

#include "actions.hpp"
#include "scene.hpp"
#include "world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

namespace sdk::dev_bridge {
  namespace {
    constexpr size_t frame_limit = 1024 * 1024; // Includes the newline.
    constexpr int io_budget = 32 * 1024;       // Bytes per socket call per poll.
    constexpr size_t client_limit = 2;

#ifdef _WIN32
    using Socket = SOCKET;
    using PollFd = WSAPOLLFD;
    constexpr Socket invalid_socket = INVALID_SOCKET;
    constexpr short read_event = POLLRDNORM, write_event = POLLWRNORM;
    bool winsock_started = false;

    int socket_error() { return WSAGetLastError(); }
    bool retryable(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
    void close_socket(Socket socket) { closesocket(socket); }
    bool abortive_close(Socket socket, bool enabled) {
      const linger mode{static_cast<u_short>(enabled), 0};
      return setsockopt(socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&mode), sizeof(mode)) == 0;
    }
    bool nonblocking(Socket socket) {
      u_long mode = 1;
      return ioctlsocket(socket, FIONBIO, &mode) == 0;
    }
    int poll_sockets(PollFd *fds, unsigned count) { return WSAPoll(fds, count, 0); }
#else
    using Socket = int;
    using PollFd = pollfd;
    constexpr Socket invalid_socket = -1;
    constexpr short read_event = POLLIN, write_event = POLLOUT;

    int socket_error() { return errno; }
    bool retryable(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
    void close_socket(Socket socket) { close(socket); }
    bool nonblocking(Socket socket) {
      const int status = fcntl(socket, F_GETFL);
      return status != -1 && fcntl(socket, F_SETFL, status | O_NONBLOCK) != -1 &&
             fcntl(socket, F_SETFD, FD_CLOEXEC) != -1;
    }
    int poll_sockets(PollFd *fds, unsigned count) { return ::poll(fds, count, 0); }
#endif

    bool set_option(Socket socket, int level, int name) {
      const int one = 1;
      return setsockopt(socket, level, name, reinterpret_cast<const char *>(&one), sizeof(one)) == 0;
    }

    void socket_diagnostic(const char *operation, int error) {
      std::fprintf(stderr, "debug bridge: %s failed (socket error %d)\n", operation, error);
    }

    struct Client {
      Socket socket = invalid_socket;
      std::unique_ptr<char[]> rx, tx;
      size_t head = 0, tail = 0, scanned = 0, newline = frame_limit;
      size_t sent = 0, queued = 0;
      bool responding = false, read_eof = false;

      void disconnect() {
#ifdef _WIN32
        // A peer that has finished sending can close normally; forced game
        // shutdown must not leave a connection holding our exclusive port.
        if(socket != invalid_socket && read_eof && !queued) abortive_close(socket, false);
#endif
        if(socket != invalid_socket) close_socket(socket);
        socket = invalid_socket;
        head = tail = scanned = sent = queued = 0;
        newline = frame_limit;
        responding = read_eof = false;
      }

      void scan() {
        if(newline != frame_limit || scanned == tail) return;
        const auto *lf = static_cast<const char *>(std::memchr(rx.get() + scanned, '\n', tail - scanned));
        if(lf) newline = static_cast<size_t>(lf - rx.get());
        else scanned = tail;
      }

      void finish_frame() {
        head = scanned = newline + 1;
        newline = frame_limit;
        if(head == tail) head = tail = scanned = 0;
      }

      bool can_read() const { return !read_eof && newline == frame_limit; }

      void receive() {
        if(tail == frame_limit && head) {
          std::memmove(rx.get(), rx.get() + head, tail - head);
          tail -= head;
          scanned -= head;
          head = 0;
        }
        const auto got = recv(socket, rx.get() + tail, static_cast<int>(std::min<size_t>(frame_limit - tail, io_budget)), 0);
        if(got > 0) {
          tail += static_cast<size_t>(got);
          scan();
          if(newline == frame_limit && tail - head == frame_limit) {
            std::fputs("debug bridge: request exceeds frame limit; disconnecting client\n", stderr);
            disconnect();
          }
        } else if(!got) {
          read_eof = true;
          if(!queued) disconnect();
        } else if(!retryable(socket_error())) {
          disconnect();
        }
      }

      void send_pending() {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto wrote = send(socket, tx.get() + sent, static_cast<int>(std::min<size_t>(queued - sent, io_budget)), flags);
        if(wrote > 0) {
          sent += static_cast<size_t>(wrote);
          if(sent == queued) {
            sent = queued = 0;
            if(read_eof) disconnect();
          }
        } else if(!wrote || !retryable(socket_error())) {
          disconnect();
        }
      }
    };

    SceneLimits snapshot_limits(const World &world) {
      const auto objects = std::min<uint32_t>(world.capacity(), 4096);
      const uint16_t types = component_count();
      uint32_t fields = 0;
      for(uint16_t i = 0; i < types; ++i) fields += component_by_id(i)->field_count;
      return {objects, std::min(objects * types, uint32_t{8192}), std::min(objects * fields, uint32_t{32768}), 1};
    }

    struct Workspace {
      SceneLimits limits;
      SceneDocument document;
      std::unique_ptr<Entity[]> bindings;

      explicit Workspace(const World &world)
        : limits(snapshot_limits(world)), document(limits), bindings(new Entity[limits.objects]) {}
    };

    Socket listener = invalid_socket;
    std::array<Client, client_limit> clients;
    size_t next_client = 0;
    bool listening = false, paused = false, attach_failed = false, poll_error_reported = false;
    std::optional<uint32_t> held_buttons;
    World *world = nullptr;
    SceneRuntime *scenes = nullptr;
    std::unique_ptr<Workspace> workspace;
    std::unique_ptr<char[]> snapshot_buffer;
    std::string startup_restore;

    std::string_view bounded_text(const char *text, size_t cap) {
      size_t length = 0;
      while(length < cap && text[length]) ++length;
      return {text, length};
    }

    bool snapshot_fits_workspace() {
      const auto &limits = workspace->limits;
      const uint32_t objects = world->size();
      if(objects > limits.objects) return false;
      const uint16_t types = component_count();
      uint32_t total_fields = 0;
      for(uint16_t id = 0; id < types; ++id) total_fields += component_by_id(id)->field_count;
      if(objects * types <= limits.components && objects * total_fields <= limits.fields) return true;

      // Only count live records when the registry-wide upper bound would not fit.
      uint32_t components = 0, fields = 0;
      for(uint16_t i = 0; i < world->capacity(); ++i) {
        const auto entity = world->entity_at(i);
        if(!entity) continue;
        for(uint16_t id = 0; id < types; ++id) {
          if(!world->get(entity, id)) continue;
          fields += component_by_id(id)->field_count;
          if(++components > limits.components || fields > limits.fields) return false;
        }
      }
      return true;
    }

    bool restore(std::string_view text, SceneError &error) {
      return scenes ? scenes->restore(text, error) :
                      load_world(*world, text, workspace->document, workspace->bindings.get(), workspace->limits.objects, error);
    }

    void begin_reply(json::Writer &out, const json::Request &request, bool success) {
      out.append("{\"id\":");
      if(request.has_id) out.number(request.id);
      else out.append("null");
      out.append(success ? ",\"ok\":true" : ",\"ok\":false");
    }

    struct Error {
      std::string_view message;
      std::string_view file;
      uint32_t line = 0;
    };

    void write_error(json::Writer &out, const json::Request &request, Error error) {
      begin_reply(out, request, false);
      out.append(",\"error\":");
      out.string(error.message);
      if(!error.file.empty()) {
        out.append(",\"file\":");
        out.string(error.file);
      }
      if(error.line) {
        out.append(",\"line\":");
        out.number(error.line);
      }
    }

    void write_entities(json::Writer &out) {
      out.append(",\"entities\":[");
      bool separator = false;
      const uint16_t types = component_count();
      for(uint16_t i = 0; out.ok() && i < world->capacity(); ++i) {
        const auto entity = world->entity_at(i);
        if(!entity) continue;
        if(separator) out.append(",");
        separator = true;
        out.append("{\"index\":");
        out.number(i);
        out.append(",\"name\":");
        const auto *name = world->get<Name>(entity);
        if(name && *name->value) out.string(bounded_text(name->value, sizeof(name->value)));
        else out.append("null");
        out.append(",\"components\":[");
        bool component_separator = false;
        for(uint16_t id = 0; out.ok() && id < types; ++id) {
          if(!world->get(entity, id)) continue;
          if(component_separator) out.append(",");
          component_separator = true;
          out.string(component_by_id(id)->name);
        }
        out.append("]}");
      }
      out.append("]");
    }

    void write_components(json::Writer &out) {
      out.append(",\"components\":[");
      const uint16_t types = component_count();
      for(uint16_t id = 0; out.ok() && id < types; ++id) {
        if(id) out.append(",");
        const auto &type = *component_by_id(id);
        out.append("{\"name\":");
        out.string(type.name);
        out.append(",\"fields\":[");
        for(uint8_t i = 0; out.ok() && i < type.field_count; ++i) {
          if(i) out.append(",");
          out.append("{\"name\":");
          out.string(type.fields[i].name);
          out.append(",\"type\":");
          out.string(field_type_name(type.fields[i].type));
          out.append("}");
        }
        out.append("]}");
      }
      out.append("]");
    }

    bool write_field(json::Writer &out, const FieldInfo &field, const char *data) {
      data += field.offset;
      if(field.type == FieldType::Entity) {
        Entity value;
        std::memcpy(&value, data, sizeof(value));
        if(world->alive(value)) out.number(value.index());
        else out.append("null");
      } else if(const auto capacity = text_field_size(field.type)) {
        out.string(bounded_text(data, capacity));
      } else {
        if(field.type == FieldType::Float) {
          float value;
          std::memcpy(&value, data, sizeof(value));
          if(!std::isfinite(value)) return false;
        }
        char text[max_value_text];
        const int length = format_value(field.type, data, text, sizeof(text));
        if(length < 0) return false;
        out.append({text, static_cast<size_t>(length)});
      }
      return true;
    }

    std::string_view write_get(json::Writer &out, std::string_view path) {
      // Match the name from the right so dots inside entity names remain usable.
      const auto last = path.rfind('.');
      if(last == path.npos || last == 0 || last + 1 == path.size()) return "expected name.Component or name.Component.field";
      auto entity_name = path.substr(0, last);
      const auto field_or_component = path.substr(last + 1);
      const auto *type = find_component(field_or_component.data());
      const FieldInfo *field = nullptr;
      char name[string_field_size];
      const auto find_entity = [&](std::string_view prefix) {
        if(prefix.empty() || prefix.size() >= sizeof(name)) return Entity{};
        std::memcpy(name, prefix.data(), prefix.size());
        name[prefix.size()] = '\0';
        return world->find(name);
      };
      auto entity = type ? find_entity(entity_name) : Entity{};
      if(!entity) {
        const auto second = entity_name.rfind('.');
        if(second != path.npos && second > 0 && second + 1 < entity_name.size()) {
          const auto component = entity_name.substr(second + 1);
          for(uint16_t i = 0; i < component_count(); ++i) {
            const auto *candidate = component_by_id(i);
            if(component == candidate->name) { type = candidate; break; }
          }
          if(type && component == type->name) {
            entity = find_entity(entity_name.substr(0, second));
            field = find_field(*type, field_or_component.data());
            if(!field) return "unknown component field";
          }
        }
      }
      if(!type) return "unknown component";
      if(!entity) return "unknown entity";
      const auto *data = static_cast<const char *>(world->get(entity, type->id));
      if(!data) return "entity does not have that component";

      out.append(",\"value\":");
      if(field) {
        if(!write_field(out, *field, data)) return "field is not a finite JSON value";
      } else {
        out.append("{");
        for(uint8_t i = 0; out.ok() && i < type->field_count; ++i) {
          if(i) out.append(",");
          out.string(type->fields[i].name);
          out.append(":");
          if(!write_field(out, type->fields[i], data)) return "field is not a finite JSON value";
        }
        out.append("}");
      }
      return {};
    }

    // At most one command for all clients, so a successful step cannot be
    // overridden by another request before the engine sees this poll's tick.
    void respond(Client &client, const Info &info, Tick &tick) {
      client.responding = true;
      const auto request = json::Reader(client.rx.get() + client.head, client.rx.get() + client.newline).request();
      client.finish_frame();
      json::Writer out(client.tx.get(), frame_limit);
      Error error;
      Result action_result{};
      SceneError scene_error;
      if(!request.valid) {
        error.message = "expected a JSON object with an unsigned id and exactly one operation";
      } else {
        const auto op = request.op;
        const auto &payload = request.payload;
        const bool takes_text = op == "console" || op == "get" || op == "restore";
        const bool takes_mask = op == "buttons";
        const bool known = takes_text || takes_mask || op == "hello" || op == "entities" || op == "components" ||
                           op == "snapshot" || op == "pause" || op == "step" || op == "resume" || op == "stats";
        if(!known) {
          error.message = "unknown operation";
        } else if(!takes_mask && (takes_text ? payload.kind != json::Kind::String || payload.text.find('\0') != payload.text.npos
                                             : payload.kind != json::Kind::Number || payload.text != "1")) {
          error.message = takes_text ? "operation requires a string with no NUL bytes" : "operation requires the number 1";
        } else if(!world && (takes_text || op == "entities" || op == "snapshot")) {
          error.message = "no World attached";
        } else {
          begin_reply(out, request, true);
          if(op == "hello") {
            out.append(",\"protocol\":1,\"title\":");
            out.string(info.title ? info.title : "");
            out.append(world ? ",\"world\":true" : ",\"world\":false");
            out.append(",\"width\":"); out.number(info.width);
            out.append(",\"height\":"); out.number(info.height);
            out.append(",\"format\":"); out.number(info.format);
          } else if(op == "stats") {
            out.append(",\"update_us\":"); out.number(info.update_us);
            out.append(",\"render_us\":"); out.number(info.render_us);
            if(info.memory_capped) {
              out.append(",\"live_bytes\":"); out.number(info.live_bytes);
              out.append(",\"peak_bytes\":"); out.number(info.peak_bytes);
            }
          } else if(op == "pause") {
            paused = true;
            tick = Tick::Pause;
          } else if(op == "step") {
            if(!paused) error.message = "pause before stepping";
            else tick = Tick::Step;
          } else if(op == "resume") {
            paused = false;
            tick = Tick::Run;
          } else if(takes_mask) {
            uint32_t mask;
            if(payload.kind == json::Kind::Null) held_buttons.reset();
            else if(payload.unsigned_integer(mask)) held_buttons = mask;
            else error.message = "buttons require an unsigned 32-bit integer or null";
          } else if(op == "console") {
            Action action;
            if(parse_console(payload.text.data(), action, action_result)) action_result = apply(*world, action, scenes);
            if(!action_result.ok) error = {action_result.message, action_result.file, action_result.line};
          } else if(op == "get") {
            error.message = write_get(out, payload.text);
          } else if(op == "entities") {
            write_entities(out);
          } else if(op == "components") {
            write_components(out);
          } else if(op == "snapshot") {
            if(!scenes && !snapshot_fits_workspace()) {
              error.message = "World exceeds bridge snapshot record limits";
            } else {
              const int length = scenes ? scenes->save(snapshot_buffer.get(), frame_limit) :
                                          save_world(*world, snapshot_buffer.get(), frame_limit);
              if(length < 0) error.message = "World cannot be saved within snapshot limits";
              else {
                out.append(",\"snapshot\":");
                out.string({snapshot_buffer.get(), static_cast<size_t>(length)});
              }
            }
          } else if(op == "restore") {
            if(!restore(payload.text, scene_error)) error = {scene_error.message, scene_error.file, scene_error.line};
          }
        }
      }
      if(error.message.empty()) {
        out.append("}\n");
        if(!out.ok()) error.message = "response exceeds frame limit";
      }
      if(!error.message.empty()) {
        out = json::Writer(client.tx.get(), frame_limit);
        write_error(out, request, error);
        out.append("}\n");
      }
      client.queued = out.size();
      client.responding = false;
      client.send_pending();
    }

    void accept_one(Client &client) {
      const Socket socket = accept(listener, nullptr, nullptr);
      if(socket == invalid_socket) {
        const int error = socket_error();
        if(!retryable(error)) socket_diagnostic("accept", error);
        return;
      }
      if(!nonblocking(socket) || !set_option(socket, IPPROTO_TCP, TCP_NODELAY)
#ifdef _WIN32
         || !abortive_close(socket, true)
#endif
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
         || !set_option(socket, SOL_SOCKET, SO_NOSIGPIPE)
#endif
      ) {
        socket_diagnostic("client setup", socket_error());
        close_socket(socket);
        return;
      }
      client.socket = socket;
    }
  }

  bool start(const char *address, const char *restore_path) {
    stop();
    if(!address) {
      if(restore_path) {
        std::fputs("debug bridge: --restore requires --bridge\n", stderr);
        return false;
      }
      return true;
    }
    if(restore_path && !*restore_path) {
      std::fputs("debug bridge: --restore requires a snapshot path\n", stderr);
      return false;
    }
    constexpr std::string_view prefix = "127.0.0.1:";
    const std::string_view endpoint(address);
    uint16_t port = 0;
    if(endpoint.substr(0, prefix.size()) == prefix) {
      const auto end = endpoint.data() + endpoint.size();
      const auto result = std::from_chars(endpoint.data() + prefix.size(), end, port);
      if(result.ec != std::errc{} || result.ptr != end) port = 0;
    }
    if(!port) {
      std::fputs("debug bridge: expected 127.0.0.1:<port> (1-65535)\n", stderr);
      return false;
    }
#ifdef _WIN32
    WSADATA data;
    if(const int error = WSAStartup(MAKEWORD(2, 2), &data)) {
      socket_diagnostic("WSAStartup", error);
      return false;
    }
    winsock_started = true;
#endif
    const auto fail_socket = [&](const char *operation) {
      socket_diagnostic(operation, socket_error());
      stop();
      return false;
    };
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(listener == invalid_socket) return fail_socket("socket");
#ifdef _WIN32
    constexpr int bind_option = SO_EXCLUSIVEADDRUSE;
#else
    constexpr int bind_option = SO_REUSEADDR;
#endif
    if(!nonblocking(listener) || !set_option(listener, SOL_SOCKET, bind_option)) return fail_socket("listener setup");
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(bind(listener, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0) return fail_socket("bind");
    try {
      for(auto &client : clients) {
        client.rx.reset(new char[frame_limit]);
        client.tx.reset(new char[frame_limit]);
      }
      snapshot_buffer.reset(new char[frame_limit]);
      if(restore_path) startup_restore = restore_path;
    } catch(const std::bad_alloc &) {
      std::fputs("debug bridge: could not reserve bridge buffers\n", stderr);
      stop();
      return false;
    }
    return true;
  }

  bool enabled() { return listener != invalid_socket; }

  bool after_init() {
    if(!enabled()) return true;
    if(attach_failed) return false;
    if(!startup_restore.empty()) {
      if(!world) {
        std::fputs("debug bridge: cannot restore without an attached World\n", stderr);
        return false;
      }
      std::FILE *file = nullptr;
#ifdef _WIN32
      try {
        file = _wfopen(std::filesystem::u8path(startup_restore).c_str(), L"rb");
      } catch(const std::exception &) {
        // SDL supplies UTF-8 CLI arguments, but opening the path needs UTF-16.
      }
#else
      file = std::fopen(startup_restore.c_str(), "rb");
#endif
      if(!file) {
        std::fprintf(stderr, "debug bridge: cannot open snapshot '%s'\n", startup_restore.c_str());
        return false;
      }
      const auto length = std::fread(snapshot_buffer.get(), 1, frame_limit, file);
      const bool read_failed = std::ferror(file) != 0;
      std::fclose(file);
      if(read_failed || length == frame_limit) {
        std::fprintf(stderr, "debug bridge: %s '%s'\n", read_failed ? "cannot read snapshot" : "snapshot exceeds file limit", startup_restore.c_str());
        return false;
      }
      SceneError error;
      if(!restore({snapshot_buffer.get(), length}, error)) {
        std::fprintf(stderr, "debug bridge: snapshot '%s'", startup_restore.c_str());
        if(error.line) std::fprintf(stderr, ":%u", static_cast<unsigned>(error.line));
        std::fprintf(stderr, ": %s\n", error.message);
        return false;
      }
    }
    if(listen(listener, static_cast<int>(client_limit)) != 0) {
      socket_diagnostic("listen", socket_error());
      return false;
    }
    listening = true;
    return true;
  }

  Tick poll(const Info &info, uint32_t &buttons) {
    Tick tick = paused ? Tick::Pause : Tick::Run;
    if(!listening) return tick;

    std::array<PollFd, client_limit + 1> fds;
    std::array<Client *, client_limit + 1> targets;
    unsigned count = 0;
    Client *available = nullptr;
    for(auto &client : clients) {
      if(client.socket == invalid_socket) {
        if(!available) available = &client;
        continue;
      }
      client.scan();
      const auto events = static_cast<short>((client.can_read() ? read_event : 0) | (client.queued ? write_event : 0));
      if(events) {
        fds[count] = {client.socket, events, 0};
        targets[count++] = &client;
      }
    }
    if(available) {
      fds[count] = {listener, read_event, 0};
      targets[count++] = nullptr;
    }

    const int ready = count ? poll_sockets(fds.data(), count) : 0;
    if(ready > 0) {
      poll_error_reported = false;
      for(unsigned i = 0; i < count; ++i) {
        const auto events = fds[i].revents;
        if(!events) continue;
        if(auto *client = targets[i]) {
          if(events & (POLLERR | POLLNVAL)) client->disconnect();
          else {
            if(client->queued && (events & (write_event | POLLHUP))) client->send_pending();
            if(client->socket != invalid_socket && client->can_read() && (events & (read_event | POLLHUP))) client->receive();
          }
        } else if(events & read_event) {
          accept_one(*available);
        }
      }
    } else if(ready < 0) {
      const int error = socket_error();
      if(!retryable(error) && !poll_error_reported) {
        socket_diagnostic("poll", error);
        poll_error_reported = true;
      }
    }

    for(size_t offset = 0; offset < clients.size(); ++offset) {
      const size_t i = (next_client + offset) % clients.size();
      auto &client = clients[i];
      if(client.socket == invalid_socket || client.queued || client.newline == frame_limit) continue;
      next_client = (i + 1) % clients.size();
      respond(client, info, tick);
      break;
    }
    if(held_buttons) buttons = *held_buttons;
    return tick;
  }

  void log(const char *message) {
    if(!listening || !message) return;
    const Client *encoded = nullptr;
    for(auto &client : clients) {
      if(client.socket == invalid_socket || client.queued || client.responding) continue;
      if(encoded) {
        std::memcpy(client.tx.get(), encoded->tx.get(), encoded->queued);
        client.queued = encoded->queued;
      } else {
        json::Writer out(client.tx.get(), frame_limit);
        out.append("{\"log\":");
        out.string(bounded_text(message, 4096));
        out.append("}\n");
        client.queued = out.size();
        encoded = &client;
      }
    }
  }

  void stop() {
    for(auto &client : clients) {
      client.disconnect();
      client.rx.reset();
      client.tx.reset();
    }
    if(listener != invalid_socket) close_socket(listener);
    listener = invalid_socket;
#ifdef _WIN32
    if(winsock_started) WSACleanup();
    winsock_started = false;
#endif
    listening = paused = attach_failed = poll_error_reported = false;
    held_buttons.reset();
    next_client = 0;
    world = nullptr;
    scenes = nullptr;
    workspace.reset();
    snapshot_buffer.reset();
    std::string{}.swap(startup_restore);
  }
}

namespace sdk {
  void bridge_attach(World &world, SceneRuntime *scenes) {
    using namespace dev_bridge;
    if(!enabled()) return;
    if(listening) {
      std::fputs("debug bridge: attach the World during game init\n", stderr);
      return;
    }
    if(scenes && &scenes->world() != &world) {
      std::fputs("debug bridge: SceneRuntime belongs to a different World\n", stderr);
      attach_failed = true;
      return;
    }
    try {
      workspace = scenes ? nullptr : std::make_unique<Workspace>(world);
      dev_bridge::world = &world;
      dev_bridge::scenes = scenes;
      attach_failed = false;
    } catch(const std::bad_alloc &) {
      std::fputs("debug bridge: could not reserve snapshot workspace\n", stderr);
      attach_failed = true;
    }
  }
}
