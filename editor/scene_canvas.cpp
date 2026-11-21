#include "scene_preview.hpp"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace editor {
  namespace {
    const sdk::SceneComponent *component_on(const sdk::SceneDocument &scene, const sdk::SceneObject &object, uint16_t type) {
      for(auto i = object.first_component; i != sdk::scene_end; i = scene.component(i).next)
        if(scene.component(i).type == type) return &scene.component(i);
      return nullptr;
    }

    const char *field_on(const sdk::SceneDocument &scene, const sdk::SceneComponent *component, uint8_t field) {
      if(component) for(auto i = component->first_field; i != sdk::scene_end; i = scene.field(i).next)
        if(scene.field(i).field == field) return scene.field(i).value;
      return nullptr;
    }

    bool collider_on(const sdk::SceneDocument &scene, const sdk::SceneObject &object,
                     const sdk::SceneObject *prefab, sdk::BoxCollider2D &box) {
      const sdk::ComponentInfo *type = nullptr;
      for(uint16_t id = 0; (type = scene.type(id)); ++id)
        if(std::string_view(type->name) == "BoxCollider2D") break;
      if(!type) return false;
      const auto *local = component_on(scene, object, type->id);
      const auto *inherited = prefab ? component_on(scene, *prefab, type->id) : nullptr;
      if(!local && !inherited) return false;
      const auto load = [&](const char *name, float &value) {
        const auto *field = sdk::find_field(*type, name);
        if(!field || field->type != sdk::FieldType::Float) return false;
        const auto index = static_cast<uint8_t>(field - type->fields);
        const auto *text = field_on(scene, local, index);
        if(!text) text = field_on(scene, inherited, index);
        return (!text || sdk::parse_value(sdk::FieldType::Float, text, &value)) && std::isfinite(value);
      };
      return load("offset_x", box.offset_x) && load("offset_y", box.offset_y) &&
             load("width", box.width) && load("height", box.height) && box.width > 0 && box.height > 0;
    }
  }

  void ScenePreview::refresh_markers() {
    markers_dirty_ = false;
    markers_.clear();
    const sdk::ComponentInfo *type = nullptr;
    for(uint16_t id = 0; (type = scene_.type(id)); ++id)
      if(std::string_view(type->name) == "Transform2D") break;
    if(!type) return;
    const auto *x = sdk::find_field(*type, "x"), *y = sdk::find_field(*type, "y");
    if(!x || !y || x->type != sdk::FieldType::Float || y->type != sdk::FieldType::Float) return;
    transform_ = type->id;
    x_field_ = static_cast<uint8_t>(x - type->fields);
    y_field_ = static_cast<uint8_t>(y - type->fields);
    const sdk::ComponentInfo *sprite = nullptr;
    for(uint16_t id = 0; (sprite = scene_.type(id)); ++id)
      if(std::string_view(sprite->name) == "Sprite") break;
    const auto &objects = scene_.objects();
    for(uint32_t i = 0; i < objects.size(); ++i) {
      const auto &object = objects[i];
      if(object.prefab) continue;
      const auto *local = component_on(scene_, object, transform_);
      const auto *base = prefab_for(object);
      const auto *inherited = base ? component_on(scene_, *base, transform_) : nullptr;
      if(!local && !inherited) continue;
      sdk::Transform2D defaults;
      Marker marker{i, defaults.x, defaults.y};
      const auto load = [&](uint8_t field, float &value) {
        auto *text = field_on(scene_, local, field);
        if(!text) text = field_on(scene_, inherited, field);
        if(text) sdk::parse_value(sdk::FieldType::Float, text, &value);
      };
      load(x_field_, marker.x);
      load(y_field_, marker.y);
      if(sprite) {
        const auto *sprite_local = component_on(scene_, object, sprite->id);
        const auto *sprite_base = base ? component_on(scene_, *base, sprite->id) : nullptr;
        const auto load_sprite = [&](const char *name, sdk::FieldType kind, void *value) {
          const auto *field = sdk::find_field(*sprite, name);
          if(!field || field->type != kind) return;
          const auto index = static_cast<uint8_t>(field - sprite->fields);
          auto *text = field_on(scene_, sprite_local, index);
          if(!text) text = field_on(scene_, sprite_base, index);
          if(text) sdk::parse_value(kind, text, value);
        };
        load_sprite("asset", sdk::FieldType::String, marker.sprite.asset);
        load_sprite("clip", sdk::FieldType::String, marker.sprite.clip);
        load_sprite("offset_x", sdk::FieldType::Float, &marker.sprite.offset_x);
        load_sprite("offset_y", sdk::FieldType::Float, &marker.sprite.offset_y);
      }
      markers_.push_back(marker);
    }
  }

  void ScenePreview::move_marker(uint32_t index, double x, double y) {
    constexpr double limit = std::numeric_limits<float>::max();
    if(std::abs(x) > limit || std::abs(y) > limit) { notice_ = "Position is out of range."; return; }
    const auto marker = std::find_if(markers_.begin(), markers_.end(), [=](const auto &m) { return m.object == index; });
    if(marker == markers_.end()) return;
    const float px = static_cast<float>(x), py = static_cast<float>(y);
    const bool move_x = px != marker->x, move_y = py != marker->y;
    if(!move_x && !move_y) return;
    const auto set = [&](uint8_t field, float value, sdk::SceneError &error) {
      char buf[sdk::max_value_text];
      const int len = sdk::format_value(sdk::FieldType::Float, &value, buf, sizeof(buf));
      if(len < 0) return false;
      const std::string_view text(buf, static_cast<size_t>(len));
      return scene_.edit_field(index, transform_, field, &text, error);
    };
    edit([&](auto &error) { return (!move_x || set(x_field_, px, error)) && (!move_y || set(y_field_, py, error)); });
  }

  void ScenePreview::draw_canvas(const std::filesystem::path &root, bool frame_selection, bool frame_screen,
                                 ImGuiKey frame_key, ImGuiKey pan_key) {
    if(!map_only_ && markers_dirty_) refresh_markers();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetContentRegionAvail().y;
    const float inset = ImGui::GetFontSize();
    if(width <= 2 * inset || height <= 2 * inset) return;
    const auto top = ImGui::GetCursorScreenPos();
    const ImVec2 bottom(top.x + width, top.y + height);
    ImGui::InvisibleButton("scene canvas", {width, height}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const auto &io = ImGui::GetIO();
    const ImVec2 center(top.x + width * .5f, top.y + height * .5f);
    const auto project = [&](double x, double y) {
      return ImVec2(float(center.x + (x - camera_x_) * zoom_), float(center.y + (y - camera_y_) * zoom_));
    };
    const auto *map = tiles().map();
    const auto map_size = map ? map->pixel_size() : blit::Size{};
    const int world_width = map_only_ && map ? std::max(1, map_size.w) : screen_size_[0];
    const int world_height = map_only_ && map ? std::max(1, map_size.h) : screen_size_[1];
    if(!zoom_ || frame_screen) {
      camera_x_ = world_width * .5;
      camera_y_ = world_height * .5;
      zoom_ = std::min((width - 2 * inset) / world_width, (height - 2 * inset) / world_height);
    }
    frame_selection |= !map_only_ && hovered && !io.WantTextInput && ImGui::IsKeyPressed(frame_key, false);
    if(frame_selection) {
      const auto marker = std::find_if(markers_.begin(), markers_.end(), [&](const auto &m) { return m.object == selected_; });
      if(marker != markers_.end()) { camera_x_ = marker->x; camera_y_ = marker->y; }
    }
    const auto mods = SDL_GetModState();
    const bool pan_modifier = !io.WantTextInput && (pan_key == ImGuiMod_Shift ? (mods & KMOD_SHIFT) != 0 :
      pan_key == ImGuiMod_Alt ? (mods & KMOD_ALT) != 0 : ImGui::IsKeyDown(pan_key));
    const bool paint = paint_mode() && !pan_modifier;
    if(active && hovered) {
      if(ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) pan_button_ = ImGuiMouseButton_Middle;
      else if(pan_modifier && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) pan_button_ = ImGuiMouseButton_Left;
    }
    const auto mouse = ImGui::GetMousePos();
    const float radius = inset * .34f;
    const Marker *hit = nullptr;
    if(!map_only_ && hovered && show_entities_) {
      float closest = inset * inset * .55f;
      for(const auto &m : markers_) {
        const auto pos = project(m.x, m.y);
        const float dx = mouse.x - pos.x, dy = mouse.y - pos.y;
        if(dx * dx + dy * dy < closest) { closest = dx * dx + dy * dy; hit = &m; }
      }
    }
    if(active && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !paint && !hit)
      pan_button_ = ImGuiMouseButton_Left;
    if(!active || (pan_button_ >= 0 && !ImGui::IsMouseDown(pan_button_))) pan_button_ = -1;
    const bool panning = pan_button_ >= 0;
    if(paint && hovered && !panning && !paint_problem()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if(active && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        paint_cell(camera_x_ + (mouse.x - center.x) / zoom_, camera_y_ + (mouse.y - center.y) / zoom_);
    }
    if((hovered && pan_modifier) || panning) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if(dragging_ == sdk::scene_end) {
      if(panning && !ImGui::IsMouseClicked(pan_button_) && ImGui::IsMouseDragging(pan_button_, 0)) {
        camera_x_ -= io.MouseDelta.x / zoom_;
        camera_y_ -= io.MouseDelta.y / zoom_;
      }
      if(hovered && io.MouseWheel && !panning) {
        const double next = std::clamp(zoom_ * std::pow(1.25, io.MouseWheel), .005, 64.0);
        camera_x_ += (io.MousePos.x - center.x) * (1 / zoom_ - 1 / next);
        camera_y_ += (io.MousePos.y - center.y) * (1 / zoom_ - 1 / next);
        zoom_ = next;
      }
    }
    auto *draw = ImGui::GetWindowDrawList();
    const auto grid = ImGui::GetColorU32(ImGuiCol_Border, .55f);
    draw->PushClipRect(top, bottom, true);
    draw->AddRectFilled(top, bottom, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
    tiles().draw(*draw, top, bottom, camera_x_, camera_y_, zoom_);
    const auto origin = project(0, 0);
    const auto bounds = project(world_width, world_height);
    if(show_grid_) {
      double step = map && (map_only_ || paint_mode()) ? map->tile_size() : grid_size_;
      while(step * zoom_ < inset) step *= 2;
      const auto phase = [&](double camera, float dimension) {
        const double offset = std::fmod(camera - dimension * .5 / zoom_, step);
        return float((offset < 0 ? -offset : step - offset) * zoom_);
      };
      const float spacing = float(step * zoom_);
      for(float x = top.x + phase(camera_x_, width); x < bottom.x; x += spacing) draw->AddLine({x, top.y}, {x, bottom.y}, grid);
      for(float y = top.y + phase(camera_y_, height); y < bottom.y; y += spacing) draw->AddLine({top.x, y}, {bottom.x, y}, grid);
    }
    const auto guide = ImGui::GetColorU32(ImGuiCol_CheckMark);
    draw->AddRect(origin, bounds, guide, 0, 0, 2);
    if(origin.x >= top.x && origin.y >= top.y && origin.x < bottom.x && origin.y < bottom.y) {
      char label[32];
      std::snprintf(label, sizeof(label), "%d x %d", world_width, world_height);
      draw->AddText({origin.x + 4, origin.y + 3}, guide, label);
    }
    if(!tiles().error().empty())
      draw->AddText(ImGui::GetFont(), inset, {top.x + inset, top.y + inset}, ImGui::GetColorU32(ImGuiCol_Text),
                    tiles().error().c_str(), nullptr, width - 2 * inset);
    if(!map_only_ && (!show_entities_ || markers_.empty()) && tiles().error().empty()) {
      draw->AddText({top.x + inset, top.y + inset}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    show_entities_ ? "No entities with Transform2D" : "Entities hidden in Layers");
    } else if(!map_only_ && show_entities_ && !markers_.empty()) {
      const auto point = [&](const Marker &m) { return project(m.x, m.y); };
      if(hit && !panning && !paint) {
        if(!writable_ || scene_.objects()[hit->object].source != 0)
          ImGui::SetTooltip("Read-only entity. Open its source scene to edit it.");
        if(active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          selected_ = hit->object;
          dragging_ = writable_ && scene_.objects()[selected_].source == 0 ? selected_ : sdk::scene_end;
        }
      }
      const bool released = ImGui::IsMouseReleased(0);
      const auto delta = ImGui::GetMouseDragDelta();
      for(const auto &m : markers_) {
        auto pos = point(m);
        if(m.object == dragging_ && (active || released) && (delta.x || delta.y)) {
          double x = m.x + delta.x / zoom_, y = m.y + delta.y / zoom_;
          if(snap_) { x = std::round(x / grid_size_) * grid_size_; y = std::round(y / grid_size_) * grid_size_; }
          pos = project(x, y);
          if(released) move_marker(m.object, x, y);
        }
        bool drawn = false;
        if(*m.sprite.asset) {
          const auto &preview = sprite_preview(root, m.sprite.asset);
          const auto &asset = preview.descriptor;
          const auto &image = preview.image;
          const auto clip = *m.sprite.clip ? asset.clips.find(m.sprite.clip) : asset.clips.begin();
          if(image.texture && (clip != asset.clips.end() || !*m.sprite.clip) &&
             image.width % asset.frame_width == 0 && image.height % asset.frame_height == 0) {
            const unsigned columns = image.width / asset.frame_width;
            const unsigned cell = clip == asset.clips.end() ? 0 : clip->second.frames.front().cell;
            if(columns && cell / columns < image.height / asset.frame_height) {
              const double x = pos.x + m.sprite.offset_x * zoom_, y = pos.y + m.sprite.offset_y * zoom_;
              const double right = x + asset.frame_width * zoom_, lower = y + asset.frame_height * zoom_;
              if(x < bottom.x && right > top.x && y < bottom.y && lower > top.y) {
                const auto sx = cell % columns * asset.frame_width, sy = cell / columns * asset.frame_height;
                draw->AddImage(reinterpret_cast<ImTextureID>(image.texture.get()), {float(x), float(y)}, {float(right), float(lower)},
                               {float(sx) / image.width, float(sy) / image.height},
                               {float(sx + asset.frame_width) / image.width, float(sy + asset.frame_height) / image.height});
                drawn = true;
              }
            }
          }
        }
        if(m.object == selected_) {
          const auto &object = scene_.objects()[m.object];
          sdk::BoxCollider2D box;
          if(collider_on(scene_, object, prefab_for(object), box)) {
            const double x = pos.x + box.offset_x * zoom_, y = pos.y + box.offset_y * zoom_;
            const double right = x + box.width * zoom_, bottom_edge = y + box.height * zoom_;
            if(x < bottom.x && right > top.x && y < bottom.y && bottom_edge > top.y) {
              // Bound vertices before converting to floats, but let the canvas clip the true edges.
              const auto color = IM_COL32(233, 151, 55, 255);
              draw->AddRect({float(std::max(x, double(top.x) - 4)), float(std::max(y, double(top.y) - 4))},
                            {float(std::min(right, double(bottom.x) + 4)), float(std::min(bottom_edge, double(bottom.y) + 4))},
                            color, 0, 0, 2);
            }
          }
        }
        if(pos.x < top.x - radius || pos.x > bottom.x + radius || pos.y < top.y - radius || pos.y > bottom.y + radius) continue;
        const bool editable = writable_ && scene_.objects()[m.object].source == 0;
        const auto color = ImGui::GetColorU32(m.object == selected_ ? ImGuiCol_CheckMark : editable ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        if(!drawn || m.object == selected_) draw->AddCircleFilled(pos, drawn ? 2.0f : radius, color, 12);
        if(m.object == selected_) {
          draw->AddCircle(pos, radius + 3, color, 12);
          const auto &object = scene_.objects()[m.object];
          draw->AddText({pos.x + inset * .6f, pos.y - inset * .5f}, color, *object.name ? object.name : object.key);
        }
      }
      if(released) {
        dragging_ = sdk::scene_end;
      } else if(!active) dragging_ = sdk::scene_end;
    }
    draw->PopClipRect();
    draw->AddRect(top, bottom, ImGui::GetColorU32(ImGuiCol_Border));
  }
}
