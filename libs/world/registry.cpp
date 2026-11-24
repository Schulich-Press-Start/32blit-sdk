/**
 * \file registry.cpp
 * \brief Storage and lookup for registered component types.
 *
 * The table is a plain array with no initialiser, so it is ready before any
 * registrar runs and their order does not matter.
 */
#include "registry.hpp"

#include <charconv>
#include <cmath>
#include <cstring>

namespace sdk {
  SDK_COMPONENT(Name, value);
  SDK_COMPONENT(Transform2D, x, y, rotation);
  SDK_COMPONENT(BoxCollider2D, offset_x, offset_y, width, height, category);
  SDK_COMPONENT(Sprite, asset, clip, offset_x, offset_y, layer, flip_x);
  SDK_COMPONENT(Camera2D, clamp_to_map, follow);

  namespace {
    const ComponentInfo *registry[max_components];
    uint16_t registry_count;

    /// Reads a whole number or float, rejecting anything left over.
    template<class T>
    bool parse_number(const char *text, void *dst) {
      const char *end = text + strlen(text);

      T value{};
      auto [stopped_at, error] = std::from_chars(text, end, value);

      if(error != std::errc{} || stopped_at != end)
        return false;

      memcpy(dst, &value, sizeof(T));
      return true;
    }

    /// Writes a number in its shortest round-trip form, terminated.
    template<class T>
    int format_number(const void *src, char *buf, int cap) {
      T value;
      memcpy(&value, src, sizeof(T));

      auto [stopped_at, error] = std::to_chars(buf, buf + cap - 1, value);  // -1 leaves room for the terminator

      if(error != std::errc{})
        return -1;

      *stopped_at = '\0';
      return static_cast<int>(stopped_at - buf);
    }

    int copy_text(const char *text, int length, char *buf, int cap) {
      if(length + 1 > cap)
        return -1;

      memcpy(buf, text, length);
      buf[length] = '\0';
      return length;
    }
  }

  bool valid_project_relative_path(std::string_view path) {
    for(unsigned char c : path)
      if(c < ' ' || c == 127 || c == '\\' || c == ':') return false;
    for(;;) {
      const auto slash = path.find('/');
      const auto part = path.substr(0, slash);
      if(part.empty() || part == "." || part == "..") return false;
      if(slash == path.npos) return true;
      path.remove_prefix(slash + 1);
    }
  }

  bool parse_value(FieldType type, const char *text, void *dst) {
    if(!text || !dst || (!*text && type != FieldType::Path))
      return false;

    switch(type) {
      case FieldType::Bool: {
        bool value;

        if(strcmp(text, "true") == 0 || strcmp(text, "1") == 0)
          value = true;
        else if(strcmp(text, "false") == 0 || strcmp(text, "0") == 0)
          value = false;
        else
          return false;

        memcpy(dst, &value, sizeof(value));
        return true;
      }

      case FieldType::Int32:  return parse_number<int32_t>(text, dst);
      case FieldType::UInt32: return parse_number<uint32_t>(text, dst);
      case FieldType::Float: {
        float value = 0.0f;

        if(!parse_number<float>(text, &value))
          return false;

        // std::from_chars reads "inf" and "nan", and a game has no use for
        // either: a nan spreads through every sum it touches and makes
        // comparisons fail silently. Refuse them here, where a scene file or a
        // console line can still be pointed at, rather than at the frame where
        // a sprite vanishes. format_value() still writes them, so a field that
        // has already gone wrong is visible in a dump and refused on reload.
        if(!std::isfinite(value))
          return false;

        memcpy(dst, &value, sizeof(value));
        return true;
      }

      case FieldType::String:
      case FieldType::Path: {
        const auto capacity = static_cast<size_t>(text_field_size(type));
        const std::string_view value(text, strnlen(text, capacity));
        if(value.size() >= capacity || (type == FieldType::Path && !value.empty() && !valid_project_relative_path(value)))
          return false;

        auto chars = static_cast<char *>(dst);
        memcpy(chars, text, value.size());
        memset(chars + value.size(), 0, capacity - value.size());
        return true;
      }

      case FieldType::Entity: return false; // needs a scene's key-to-entity mapping
    }

    return false;
  }

  int format_value(FieldType type, const void *src, char *buf, int cap) {
    if(!src || !buf || cap <= 0)
      return -1;

    switch(type) {
      case FieldType::Bool: {
        bool value;
        memcpy(&value, src, sizeof(value));

        const char *text = value ? "true" : "false";
        return copy_text(text, static_cast<int>(strlen(text)), buf, cap);
      }

      case FieldType::Int32:  return format_number<int32_t>(src, buf, cap);
      case FieldType::UInt32: return format_number<uint32_t>(src, buf, cap);
      case FieldType::Float:  return format_number<float>(src, buf, cap);

      case FieldType::String: {
        auto chars = static_cast<const char *>(src);
        // a field filled to the brim has no terminator of its own
        const auto length = static_cast<int>(strnlen(chars, string_field_size));
        return copy_text(chars, length, buf, cap);
      }

      case FieldType::Path: {
        const auto *chars = static_cast<const char *>(src);
        const auto length = static_cast<int>(strnlen(chars, path_field_size));
        if(length == path_field_size || (length && !valid_project_relative_path({chars, static_cast<size_t>(length)}))) return -1;
        return copy_text(chars, length, buf, cap);
      }

      case FieldType::Entity: return -1;
    }

    return -1;
  }

  const char *field_type_name(FieldType type) {
    switch(type) {
      case FieldType::Bool:   return "bool";
      case FieldType::Int32:  return "int32";
      case FieldType::UInt32: return "uint32";
      case FieldType::Float:  return "float";
      case FieldType::String: return "string";
      case FieldType::Entity: return "entity";
      case FieldType::Path:   return "path";
    }

    return "?";
  }

  namespace detail {

    ComponentRegistrar::ComponentRegistrar(const char *name, uint16_t size,
                                           const FieldInfo *fields, uint8_t field_count,
                                           const ComponentInfo *&type_info)
      : info{name, registry_count, size, fields, field_count} {

      if(registry_count == max_components)
        return;  // over the limit: reported by find_component failing, not by crashing here

      registry[registry_count++] = &info;
      type_info = &info;
    }
  }

  const ComponentInfo *find_component(const char *name) {
    if(!name)
      return nullptr;

    for(uint16_t i = 0; i < registry_count; i++) {
      if(strcmp(registry[i]->name, name) == 0)
        return registry[i];
    }

    return nullptr;
  }

  const ComponentInfo *component_by_id(uint16_t id) {
    return id < registry_count ? registry[id] : nullptr;
  }

  const FieldInfo *find_field(const ComponentInfo &component, const char *name) {
    if(name)
      for(uint8_t i = 0; i < component.field_count; ++i)
        if(strcmp(component.fields[i].name, name) == 0)
          return &component.fields[i];
    return nullptr;
  }

  uint16_t component_count() {
    return registry_count;
  }
}
