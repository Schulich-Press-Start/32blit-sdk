#pragma once

#include "font_path.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <numeric>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace sdk::ui {
  enum class Kind { Panel, Label, Button };

  inline constexpr int max_coordinate = 32767;
  inline constexpr int min_coordinate = -max_coordinate;
  inline constexpr int button_text_inset = 2;

  struct Rgba { uint8_t r, g, b, a = 255; };
  struct WidgetStyle { Rgba fill, border, text; };
  struct StyleOverrides { std::optional<Rgba> fill, border, text; };

  struct StyleField {
    const char *key;
    const char *label;
    std::optional<Rgba> StyleOverrides::*override_color;
    Rgba WidgetStyle::*default_color;
  };
  inline constexpr StyleField style_fields[] = {
    {"fill", "Fill color", &StyleOverrides::fill, &WidgetStyle::fill},
    {"border", "Border color", &StyleOverrides::border, &WidgetStyle::border},
    {"text", "Text color", &StyleOverrides::text, &WidgetStyle::text},
  };

  struct Widget {
    std::string id, text;
    Kind kind = Kind::Label;
    int x = 0, y = 0, w = 32, h = 12;
    bool enabled = true;
    int layer = 0;
    StyleOverrides style;
    bool wrap = false;
  };

  struct Layout {
    int width = 160, height = 120;
    std::string initial_focus;
    std::vector<Widget> widgets;
    std::string font;
  };

  /// Back to front, as in the world renderer. Equal layers retain authored order.
  inline std::vector<std::size_t> paint_order(const Layout &layout) {
    std::vector<std::size_t> order(layout.widgets.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      const int left = layout.widgets[a].layer, right = layout.widgets[b].layer;
      return left == right ? a < b : left > right;
    });
    return order;
  }

  constexpr const char *kind_name(Kind kind) {
    switch(kind) {
      case Kind::Panel: return "panel";
      case Kind::Label: return "label";
      case Kind::Button: return "button";
    }
    return "";
  }

  inline bool is_focusable(const Layout &layout, const Widget &widget) {
    return widget.kind == Kind::Button && widget.enabled &&
           widget.x < layout.width && widget.y < layout.height &&
           widget.x + widget.w > 0 && widget.y + widget.h > 0;
  }

  /// Returns widgets.size() if there is no enabled button on the canvas.
  inline std::size_t initial_focus_index(const Layout &layout) {
    auto first = layout.widgets.size();
    for(std::size_t i = 0; i < layout.widgets.size(); ++i) {
      const auto &widget = layout.widgets[i];
      if(!is_focusable(layout, widget)) continue;
      if(widget.id == layout.initial_focus) return i;
      if(first == layout.widgets.size()) first = i;
    }
    return first;
  }

  /// Palette shared by the runtime and the editor's preview.
  constexpr WidgetStyle default_widget_style(Kind kind, bool enabled, bool focused = false) {
    constexpr Rgba no_color{0, 0, 0, 0};
    constexpr Rgba light{239, 245, 252};
    constexpr Rgba muted{151, 159, 172};
    switch(kind) {
      case Kind::Panel:
        if(!enabled) return {{28, 31, 38}, {62, 67, 79}, no_color};
        return {{20, 29, 47}, {88, 111, 143}, no_color};
      case Kind::Label:
        return {no_color, no_color, enabled ? light : muted};
      case Kind::Button:
        if(!enabled) return {{45, 49, 59}, {79, 86, 101}, muted};
        if(focused) return {{51, 92, 137}, {255, 223, 117}, light};
        return {{35, 54, 82}, {117, 149, 187}, light};
    }
    return {no_color, no_color, no_color};
  }

  inline WidgetStyle widget_style(const Widget &widget, bool focused) {
    auto style = default_widget_style(widget.kind, widget.enabled, focused);
    if(widget.style.fill) style.fill = *widget.style.fill;
    // Keep the focus outline visible even when an authored border is transparent.
    if(widget.style.border && !focused) style.border = *widget.style.border;
    if(widget.style.text) style.text = *widget.style.text;
    return style;
  }

  // Fill and border never overlap: translucent fill must blend with the scene, not the border.
  template<class Draw>
  void draw_widget_box(int w, int h, const WidgetStyle &style, Draw draw) {
    if(style.border.a) {
      draw(0, 0, w, 1, style.border);
      if(h > 1) draw(0, h - 1, w, 1, style.border);
      if(h > 2) {
        draw(0, 1, 1, h - 2, style.border);
        if(w > 1) draw(w - 1, 1, 1, h - 2, style.border);
      }
    }
    if(style.fill.a && w > 2 && h > 2) draw(1, 1, w - 2, h - 2, style.fill);
  }

  inline bool read_style(const nlohmann::json &json, StyleOverrides &style) {
    if(!json.is_object()) return false;
    for(const auto &field : style_fields) {
      const auto color = json.find(field.key);
      if(color == json.end()) continue;
      if(!color->is_array() || color->size() != 4) return false;
      uint8_t rgba[4];
      for(std::size_t i = 0; i < 4; ++i) {
        const auto &channel = (*color)[i];
        if(!channel.is_number_integer() || channel < 0 || channel > 255) return false;
        rgba[i] = channel.get<uint8_t>();
      }
      style.*field.override_color = Rgba{rgba[0], rgba[1], rgba[2], rgba[3]};
    }
    return true;
  }

  /// Returns an error or nullptr. Invalid input does not replace the output.
  inline const char *read_layout(const nlohmann::json &json, Layout &output) {
    using Json = nlohmann::json;
    const auto read_integer = [](const Json &object, const char *key, int min, int &value) {
      const auto field = object.find(key);
      if(field == object.end() || !field->is_number_integer()) return false;
      if(field->is_number_unsigned()) {
        const auto number = field->get<Json::number_unsigned_t>();
        if(number > max_coordinate || (min > 0 && number < static_cast<unsigned>(min))) return false;
        value = static_cast<int>(number);
      } else {
        const auto number = field->get<Json::number_integer_t>();
        if(number < min || number > max_coordinate) return false;
        value = static_cast<int>(number);
      }
      return true;
    };
    const auto optional_string = [](const Json &object, const char *key, std::string &value) {
      const auto field = object.find(key);
      if(field == object.end()) return true;
      if(!field->is_string()) return false;
      const auto &text = field->get_ref<const std::string &>();
      if(text.find('\0') != text.npos) return false;
      value = text;
      return true;
    };

    if(!json.is_object()) return "Expected a UI layout object.";
    Layout layout;
    if(!read_integer(json, "width", 1, layout.width) || !read_integer(json, "height", 1, layout.height))
      return "UI width and height must be integers from 1 to 32767.";
    if(!optional_string(json, "initial_focus", layout.initial_focus))
      return "UI initial_focus must be a string without NUL characters.";
    if(const auto font = json.find("font"); font != json.end()) {
      if(!font->is_string() || !is_project_font_path(font->get_ref<const std::string &>()))
        return "UI font must be a project-relative .font path using /, or omitted for the default font.";
      layout.font = font->get_ref<const std::string &>();
    }
    const auto widgets = json.find("widgets");
    if(widgets == json.end() || !widgets->is_array()) return "UI widgets must be an array.";

    layout.widgets.reserve(widgets->size());
    std::unordered_set<std::string_view> ids;
    ids.reserve(widgets->size());
    for(const auto &item : *widgets) {
      if(!item.is_object()) return "Each UI widget must be an object.";
      const auto id = item.find("id");
      if(id == item.end() || !id->is_string()) return "UI widget id must be a nonempty string.";
      const auto &name = id->get_ref<const std::string &>();
      if(name.empty() || name.find('\0') != name.npos) return "UI widget id must be nonempty and contain no NUL characters.";
      if(!ids.emplace(name).second) return "UI widget ids must be unique.";

      const auto kind = item.find("kind");
      if(kind == item.end() || !kind->is_string()) return "UI widget kind must be panel, label or button.";
      Widget widget;
      if(*kind == "panel") widget.kind = Kind::Panel;
      else if(*kind == "label") widget.kind = Kind::Label;
      else if(*kind == "button") widget.kind = Kind::Button;
      else return "UI widget kind must be panel, label or button.";

      if(!read_integer(item, "x", min_coordinate, widget.x) || !read_integer(item, "y", min_coordinate, widget.y))
        return "UI widget x and y must be integers from -32767 to 32767.";
      if(!read_integer(item, "w", 1, widget.w) || !read_integer(item, "h", 1, widget.h))
        return "UI widget w and h must be integers from 1 to 32767.";
      if(item.contains("layer") && !read_integer(item, "layer", min_coordinate, widget.layer))
        return "UI widget layer must be an integer from -32767 to 32767.";
      if(!optional_string(item, "text", widget.text)) return "UI widget text must be a string without NUL characters.";
      if(const auto wrap = item.find("wrap"); wrap != item.end()) {
        if(!wrap->is_boolean() || widget.kind == Kind::Panel) return "UI wrap must be a boolean on a label or button.";
        widget.wrap = wrap->get<bool>();
      }
      if(const auto enabled = item.find("enabled"); enabled != item.end()) {
        if(!enabled->is_boolean()) return "UI widget enabled must be a boolean.";
        widget.enabled = enabled->get<bool>();
      }
      if(const auto style = item.find("style"); style != item.end() && !read_style(*style, widget.style))
        return "UI widget style must be an object with RGBA arrays of four integers from 0 to 255.";
      widget.id = name;
      layout.widgets.push_back(std::move(widget));
    }
    output = std::move(layout);
    return nullptr;
  }

  /// Write the canonical layout, omitting optional fields at their defaults.
  inline nlohmann::json write_layout(const Layout &layout) {
    using Json = nlohmann::json;
    auto widgets = Json::array();
    widgets.get_ref<Json::array_t &>().reserve(layout.widgets.size());
    for(const auto &widget : layout.widgets) {
      Json item = {{"id", widget.id}, {"kind", kind_name(widget.kind)},
                   {"x", widget.x}, {"y", widget.y}, {"w", widget.w}, {"h", widget.h}};
      if(!widget.text.empty()) item["text"] = widget.text;
      if(widget.wrap) item["wrap"] = true;
      if(!widget.enabled) item["enabled"] = false;
      if(widget.layer != 0) item["layer"] = widget.layer;
      for(const auto &field : style_fields) {
        if(const auto &color = widget.style.*field.override_color)
          item["style"][field.key] = {color->r, color->g, color->b, color->a};
      }
      widgets.push_back(std::move(item));
    }
    Json json = {{"width", layout.width}, {"height", layout.height}, {"widgets", std::move(widgets)}};
    if(!layout.initial_focus.empty()) json["initial_focus"] = layout.initial_focus;
    if(!layout.font.empty()) json["font"] = layout.font;
    return json;
  }
}
