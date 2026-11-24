#include "actions.hpp"
#include "scene_text.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace sdk {
  namespace {
    constexpr std::string_view spaces = " \t";

    Result failure(std::string_view message, std::string_view detail = {}) {
      Result result{false, {}};
      constexpr auto limit = sizeof(result.message) - 1;
      message = message.substr(0, detail.empty() ? limit : limit - 3);
      std::memcpy(result.message, message.data(), message.size());
      if(!detail.empty()) {
        auto *suffix = result.message + message.size();
        detail = detail.substr(0, limit - message.size() - 3);
        suffix[0] = ' ';
        suffix[1] = '\'';
        std::memcpy(suffix + 2, detail.data(), detail.size());
        suffix[detail.size() + 2] = '\'';
      }
      return result;
    }

    void skip_spaces(std::string_view &line) {
      const auto start = line.find_first_not_of(spaces);
      line.remove_prefix(start == line.npos ? line.size() : start);
    }

    std::string_view take_word(std::string_view &line) {
      skip_spaces(line);
      const auto word = line.substr(0, line.find_first_of(spaces));
      line.remove_prefix(word.size());
      return word;
    }

    // Return the separator in Component.field, rejecting missing or nested parts.
    size_t field_separator(std::string_view path) {
      const auto dot = path.find('.');
      if(dot == path.npos || dot == 0 || dot + 1 == path.size() || path.find('.', dot + 1) != path.npos)
        return path.npos;
      return dot;
    }

    void copy_text(char *dst, std::string_view text) {
      if(!text.empty()) std::memcpy(dst, text.data(), text.size());
      dst[text.size()] = '\0';
    }

    struct Command {
      ActionType type;
      std::string_view verb, cpp_name;
    };

    constexpr Command commands[] = {
      {ActionType::Set, "set", "Set"},
      {ActionType::Destroy, "destroy", "Destroy"},
      {ActionType::AddComponent, "add", "AddComponent"},
      {ActionType::RemoveComponent, "remove", "RemoveComponent"},
      {ActionType::Spawn, "spawn", "Spawn"},
      {ActionType::LoadScene, "load_scene", "LoadScene"},
    };

    struct ActionText {
      std::string_view target, path, value;
    };

    struct SpawnPosition { float x, y; };

    bool scene_action(ActionType type) {
      return type == ActionType::Spawn || type == ActionType::LoadScene;
    }

    bool command_token(std::string_view text) {
      return !text.empty() && text.find_first_of(" \t\r\n") == text.npos;
    }

    Result parse_position(const char *x, const char *y, SpawnPosition &position) {
      if(!parse_value(FieldType::Float, x, &position.x) ||
         !parse_value(FieldType::Float, y, &position.y))
        return failure("expected finite spawn coordinates");
      return {true, {}};
    }

    bool name_token(std::string_view text) {
      return !text.empty() && text.find_first_of(". \t\r\n") == text.npos;
    }

    Result validate(ActionType type, ActionText text) {
      if(type == ActionType::Spawn) {
        if(!detail::scene_bare_token(text.target) || text.target.size() >= scene_key_size)
          return failure("expected a prefab key");
        if(!command_token(text.path) || !command_token(text.value))
          return failure("expected spawn prefab x y");
        return {true, {}};
      }
      if(type == ActionType::LoadScene) {
        if(!command_token(text.target) || !text.path.empty() || !text.value.empty())
          return failure("expected load_scene project-relative-path");
        return {true, {}};
      }
      if(!name_token(text.target)) return failure("expected an entity name");
      if(text.target.size() >= string_field_size) return failure("entity name too long");
      switch(type) {
        case ActionType::Set: {
          const auto dot = field_separator(text.path);
          if(dot == text.path.npos || text.path.find_first_of(" \t\r\n") != text.path.npos)
            return failure("expected entity.Component.field");
          if(text.value.empty()) break; // A field codec decides whether clearing is valid.
          if(text.value.find_first_of("\r\n") != text.value.npos ||
             spaces.find(text.value.front()) != spaces.npos || spaces.find(text.value.back()) != spaces.npos)
            return failure("expected a single trimmed value");
          break;
        }
        case ActionType::AddComponent:
        case ActionType::RemoveComponent:
          if(!name_token(text.path)) return failure("expected entity.Component");
          if(!text.value.empty()) return failure("unexpected argument");
          break;
        case ActionType::Destroy:
          if(!text.path.empty() || !text.value.empty()) return failure("unexpected argument");
          break;
        default: return failure("unsupported action");
      }
      return {true, {}};
    }

    Result parse_command(std::string_view line, Action &out) {
      if(line.find_first_of("\r\n") != line.npos) return failure("expected one console line");
      const auto verb = take_word(line);
      const Command *command = nullptr;
      for(const auto &candidate : commands)
        if(candidate.verb == verb) { command = &candidate; break; }
      if(!command) return failure("expected set, destroy, add, remove, spawn or load_scene");

      ActionText text{take_word(line), {}, {}};
      if(command->type == ActionType::Spawn) {
        text.path = take_word(line);
        text.value = take_word(line);
        if(!take_word(line).empty()) return failure("unexpected argument");
      } else if(command->type != ActionType::Destroy && command->type != ActionType::LoadScene) {
        const auto dot = text.target.find('.');
        if(dot != text.target.npos) {
          text.path = text.target.substr(dot + 1);
          text.target = text.target.substr(0, dot);
        }
      }
      skip_spaces(line);
      if(!line.empty()) text.value = line.substr(0, line.find_last_not_of(spaces) + 1);
      if(command->type == ActionType::Set) {
        if(text.value.empty()) return failure("missing value");
        if(text.value == "\"\"") text.value = {};
      }
      if(auto result = validate(command->type, text); !result.ok) return result;
      if(text.target.size() >= sizeof(out.target)) return failure("scene path too long");
      if(text.path.size() >= sizeof(out.path)) return failure("component field path too long");
      if(text.value.size() >= sizeof(out.value)) {
        char message[40];
        std::snprintf(message, sizeof(message), "value too long (max %zu bytes)", sizeof(out.value) - 1);
        return failure(message, text.path);
      }
      if(command->type == ActionType::Spawn) {
        char x[sizeof(out.path)], y[sizeof(out.value)];
        copy_text(x, text.path);
        copy_text(y, text.value);
        SpawnPosition position;
        if(auto result = parse_position(x, y, position); !result.ok) return result;
      }

      out.type = command->type;
      copy_text(out.target, text.target);
      copy_text(out.path, text.path);
      copy_text(out.value, text.value);
      return {true, {}};
    }

    template<size_t N> bool bounded_text(const char (&buf)[N], std::string_view &text) {
      const auto *end = static_cast<const char *>(std::memchr(buf, '\0', N));
      if(!end) return false;
      text = {buf, static_cast<size_t>(end - buf)};
      return true;
    }

    Result read_action(const Action &action, ActionText &text, SpawnPosition *position = nullptr) {
      if(!bounded_text(action.target, text.target) || !bounded_text(action.path, text.path) ||
         !bounded_text(action.value, text.value)) return failure("unterminated action text");
      if(auto result = validate(action.type, text); !result.ok) return result;
      if(action.type == ActionType::Spawn) {
        SpawnPosition ignored;
        return parse_position(action.path, action.value, position ? *position : ignored);
      }
      return {true, {}};
    }

    const Command &command_for(ActionType type) {
      // Only called after read_action has validated the enum.
      for(const auto &command : commands)
        if(command.type == type) return command;
      std::terminate();
    }
  }

  bool parse_console(const char *line, Action &out, Result &err) {
    err = line ? parse_command(line, out) : failure("expected a command");
    return err.ok;
  }

  Result apply(World &world, const Action &action, SceneRuntime *scenes) {
    ActionText text;
    SpawnPosition position;
    if(auto result = read_action(action, text, &position); !result.ok) return result;
    if(scene_action(action.type)) {
      if(!scenes) return failure("command requires a SceneRuntime");
      if(&scenes->world() != &world) return failure("SceneRuntime belongs to a different World");
      SceneError error;
      const bool ok = action.type == ActionType::Spawn
        ? static_cast<bool>(scenes->spawn(text.target, position.x, position.y, error))
        : scenes->load(text.target, error);
      if(ok) return {true, {}};
      auto result = failure(error.message);
      copy_text(result.file, error.file);
      result.line = error.line;
      return result;
    }
    const auto entity = world.find(action.target);
    if(!entity) return failure("no entity", action.target);
    if(action.type == ActionType::Destroy)
      return world.destroy(entity) ? Result{true, {}} : failure("cannot destroy entity", action.target);

    if(action.type != ActionType::Set) {
      const auto *info = find_component(action.path);
      if(!info) return failure("unknown component", action.path);
      if(action.type == ActionType::AddComponent)
        return world.add(entity, info->id) ? Result{true, {}} : failure("cannot add component", action.path);
      if(!world.get(entity, info->id)) return failure("entity has no component", action.path);
      return world.remove(entity, info->id) ? Result{true, {}} : failure("cannot remove component", action.path);
    }

    const auto dot = text.path.find('.');
    char component_name[sizeof(action.path)];
    copy_text(component_name, text.path.substr(0, dot));
    const auto *info = find_component(component_name);
    if(!info) return failure("unknown component", component_name);
    auto *component = static_cast<char *>(world.get(entity, info->id));
    if(!component) return failure("entity has no component", component_name);

    const char *field_name = action.path + dot + 1;
    const auto *field = find_field(*info, field_name);
    if(!field) return failure("unknown field", field_name);

    if(info == component_info<Name>()) {
      const auto other = world.find(action.value);
      if(other && other.index() != entity.index()) return failure("name already in use", action.value);
    }
    if(!parse_value(field->type, action.value, component + field->offset)) {
      if(field->type == FieldType::Path) return failure("expected project-relative path using / for", text.path);
      return failure("invalid value for type", field_type_name(field->type));
    }
    return {true, {}};
  }

  int to_console(const Action &action, char *buf, int cap) {
    detail::SceneOutput out(buf, cap);
    ActionText text;
    if(!read_action(action, text).ok) return -1;
    out.append(command_for(action.type).verb);
    out.append(" ");
    out.append(text.target);
    if(!text.path.empty()) {
      out.append(action.type == ActionType::Spawn ? " " : ".");
      out.append(text.path);
    }
    if(!text.value.empty()) {
      out.append(" ");
      out.append(text.value);
    }
    if(action.type == ActionType::Set && text.value.empty()) out.append(" \"\"");
    return out.finish();
  }

  int to_cpp(const Action &action, char *buf, int cap) {
    detail::SceneOutput out(buf, cap);
    ActionText text;
    if(!read_action(action, text).ok) return -1;
    out.append("sdk::apply(world, sdk::Action{sdk::ActionType::");
    out.append(command_for(action.type).cpp_name);
    out.append(", ");
    out.quoted(text.target);
    out.append(", ");
    out.quoted(text.path);
    out.append(", ");
    out.quoted(text.value);
    out.append(scene_action(action.type) ? "}, &scenes);" : "});");
    return out.finish();
  }

  int to_scene(const Action &action, char *buf, int cap) {
    detail::SceneOutput out(buf, cap);
    ActionText text;
    if(!read_action(action, text).ok) return -1;
    if(action.type == ActionType::AddComponent) {
      if(!detail::scene_bare_token(text.path) || text.path == "Name") return -1;
      out.append(text.path);
    } else if(action.type == ActionType::Set) {
      const auto dot = text.path.find('.');
      const auto component = text.path.substr(0, dot), field = text.path.substr(dot + 1);
      if(!detail::scene_bare_token(component) || !detail::scene_bare_token(field) || component == "Name") return -1;
      out.append(component);
      out.append(" ");
      out.append(field);
      out.append("=");
      out.token(text.value);
    } else if(action.type == ActionType::Spawn) {
      out.append("entity ");
      out.token(text.target);
      out.append(" : ");
      out.token(text.target);
      out.append(" name=\"\"\n  Transform2D x=");
      out.token(text.path);
      out.append(" y=");
      out.token(text.value);
    } else {
      return -1;
    }
    return out.finish();
  }
}
