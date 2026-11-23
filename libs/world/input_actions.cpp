#include "input_actions.hpp"
#include "../../32blit-sdl/ActionInput.hpp"

#include <SDL_gamecontroller.h>
#include <SDL_keyboard.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <utility>

namespace sdk {
  namespace {
    constexpr std::string_view whitespace = " \t\r\v\f";

    void trim_left(std::string_view &text) {
      const auto first = text.find_first_not_of(whitespace);
      text.remove_prefix(first == text.npos ? text.size() : first);
    }

    std::string_view take_word(std::string_view &text) {
      const auto end = text.find_first_of(whitespace);
      const auto word = text.substr(0, end);
      text.remove_prefix(word.size());
      trim_left(text);
      return word;
    }
  }

  InputActions::Handle InputActions::find(std::string_view name) const {
    for(std::size_t i = 0; i < entries_.size(); ++i) {
      if(std::string_view(entries_[i].name) == name) return Handle(i);
    }
    return {};
  }

  InputActions::Handle InputActions::action(std::string_view name) {
    const auto handle = find(name);
    if(handle != Handle{}) return handle;
    entries_.push_back({std::string(name), {}});
    return Handle(entries_.size() - 1);
  }

  bool InputActions::bind(Handle action, Device device, const char *physical) {
    if(action.index_ >= entries_.size() || !physical || !*physical) return false;
    int code;
    switch(device) {
      case Device::Keyboard:
        code = SDL_GetScancodeFromName(physical);
        if(code <= SDL_SCANCODE_UNKNOWN || code >= SDL_NUM_SCANCODES) return false;
        break;
      case Device::Gamepad:
        code = SDL_GameControllerGetButtonFromString(physical);
        if(code < 0 || code >= SDL_CONTROLLER_BUTTON_MAX || code >= 32) return false;
        break;
      default:
        return false;
    }
    auto &bindings = entries_[action.index_].bindings;
    for(const auto &binding : bindings) {
      if(binding.device == device && binding.physical == code) return true;
    }
    bindings.push_back({static_cast<uint16_t>(code), device});
    return true;
  }

  bool InputActions::load(const std::string &path) {
    std::size_t line_number = 0;
    const auto fail = [&](const char *reason) {
      std::fprintf(stderr, "%s", path.empty() ? "<empty input bindings path>" : path.c_str());
      if(line_number) std::fprintf(stderr, ":%zu", line_number);
      std::fprintf(stderr, ": %s\n", reason);
      return false;
    };

    std::ifstream input;
    try {
      input.open(std::filesystem::u8path(path), std::ios::binary);
    } catch(const std::filesystem::filesystem_error &e) {
      return fail(e.what());
    }
    if(!input) return fail("Cannot open input bindings file (missing or unreadable).");

    InputActions staged;
    std::string line;
    while(std::getline(input, line)) {
      ++line_number;
      std::string_view row(line);
      if(line_number == 1 && row.substr(0, 3) == "\xef\xbb\xbf") row.remove_prefix(3);
      trim_left(row);
      if(row.empty() || row.front() == '#') continue;
      if(row.find('\0') != row.npos) return fail("Unexpected NUL in input binding.");
      const auto name = take_word(row);
      const auto device = take_word(row);
      if(device.empty() || row.empty()) return fail("Expected: action keyboard|gamepad physical name.");
      if(device != "keyboard" && device != "gamepad") return fail("Expected device 'keyboard' or 'gamepad'.");
      const auto type = device == "keyboard" ? Device::Keyboard : Device::Gamepad;
      const auto handle = staged.action(name);
      const auto physical_start = static_cast<std::size_t>(row.data() - line.data());
      const auto physical_end = row.find_last_not_of(whitespace) + 1;
      line.resize(physical_start + physical_end);
      if(!staged.bind(handle, type, line.c_str() + physical_start)) {
        std::fprintf(stderr, "%s:%zu: Unknown SDL %s name '%s'.\n", path.c_str(), line_number,
                     type == Device::Keyboard ? "scancode" : "controller button", line.c_str() + physical_start);
        return false;
      }
    }
    if(input.bad() || !input.eof()) return fail("Cannot read input bindings file.");

    for(auto &entry : entries_) entry.bindings.clear();
    for(auto &entry : staged.entries_) {
      const auto handle = find(entry.name);
      if(handle == Handle{}) entries_.push_back(std::move(entry));
      else entries_[handle.index_].bindings = std::move(entry.bindings);
    }
    return true;
  }

  void InputActions::update() {
    const auto &input = blit::action_input();
    for(auto &entry : entries_) {
      bool active = false;
      for(const auto &binding : entry.bindings) {
        active = binding.device == Device::Keyboard ? input.keys[binding.physical]
                                                     : (input.pad & (uint32_t{1} << binding.physical)) != 0;
        if(active) break;
      }
      const bool was_active = (entry.state & Down) != 0;
      entry.state = active ? (was_active ? Down : Down | Pressed) : (was_active ? Released : 0);
    }
  }
}
