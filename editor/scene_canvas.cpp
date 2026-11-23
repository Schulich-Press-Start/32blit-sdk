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

    bool camera_on(const sdk::SceneDocument &scene, const sdk::SceneObject &object,
                   const sdk::SceneObject *prefab, bool &clamp, const char *&follow) {
      const sdk::ComponentInfo *type = nullptr;
      for(uint16_t id = 0; (type = scene.type(id)); ++id)
        if(std::string_view(type->name) == "Camera2D") break;
      if(!type) return false;
      const auto *local = component_on(scene, object, type->id);
      const auto *inherited = prefab ? component_on(scene, *prefab, type->id) : nullptr;
      if(!local && !inherited) return false;
      const auto value = [&](const char *name, sdk::FieldType kind) {
        const auto *field = sdk::find_field(*type, name);
        if(!field || field->type != kind) return static_cast<const char *>(nullptr);
        const auto index = static_cast<uint8_t>(field - type->fields);
        const auto *text = field_on(scene, local, index);
        return text ? text : field_on(scene, inherited, index);
      };
      if(const auto *text = value("clamp_to_map", sdk::FieldType::Bool))
        sdk::parse_value(sdk::FieldType::Bool, text, &clamp);
      follow = value("follow", sdk::FieldType::Entity);
      return true;
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
        load_sprite("layer", sdk::FieldType::Int32, &marker.sprite.layer);
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

  void ScenePreview::place_sprite(std::string_view stem, float x, float y) {
    if(stem.empty() || stem.size() >= sdk::string_field_size) {
      notice_ = "Sprite path is too long for Sprite.asset; shorten the descriptor name or folder.";
      return;
    }
    const sdk::ComponentInfo *sprite = nullptr;
    for(uint16_t id = 0; (sprite = scene_.type(id)); ++id)
      if(std::string_view(sprite->name) == "Sprite") break;
    const auto *transform = scene_.type(transform_);
    const auto *asset = sprite ? sdk::find_field(*sprite, "asset") : nullptr;
    if(!asset || asset->type != sdk::FieldType::String || !transform || std::string_view(transform->name) != "Transform2D") {
      notice_ = "Placing a sprite requires the built-in Transform2D and Sprite components.";
      return;
    }
    std::string base(stem.substr(stem.find_last_of('/') + 1));
    for(char &c : base) if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
    std::string name = base;
    const auto &objects = scene_.objects();
    for(unsigned n = 2; std::any_of(objects.begin(), objects.end(), [&](const auto &object) {
      return !object.prefab && (name == object.key || name == object.name);
    }); ++n) {
      const auto suffix = std::to_string(n);
      name = base.substr(0, sdk::string_field_size - 1 - suffix.size()) + suffix;
    }
    const auto index = static_cast<uint32_t>(objects.size());
    if(edit([&](sdk::SceneError &error) {
      if(!scene_.add_object(name, {}, false, error) || !scene_.edit_component(index, transform_, true, error) ||
         !scene_.edit_component(index, sprite->id, true, error)) return false;
      const auto coordinate = [&](uint8_t field, float value) {
        char buf[sdk::max_value_text];
        const auto n = sdk::format_value(sdk::FieldType::Float, &value, buf, sizeof(buf));
        if(n < 0) return false;
        const std::string_view text(buf, static_cast<size_t>(n));
        return scene_.edit_field(index, transform_, field, &text, error);
      };
      return coordinate(x_field_, x) && coordinate(y_field_, y) &&
        scene_.edit_field(index, sprite->id, static_cast<uint8_t>(asset - sprite->fields), &stem, error);
    })) selected_ = index;
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
    if(!map_only_ && writable_ && show_entities_ && ImGui::BeginDragDropTarget()) {
      if(const auto *payload = ImGui::AcceptDragDropPayload("BLIT_SPRITE_STEM")) {
        double x = camera_x_ + (mouse.x - center.x) / zoom_, y = camera_y_ + (mouse.y - center.y) / zoom_;
        if(snap_) { x = std::round(x / grid_size_) * grid_size_; y = std::round(y / grid_size_) * grid_size_; }
        place_sprite({static_cast<const char *>(payload->Data), static_cast<size_t>(payload->DataSize)}, float(x), float(y));
      }
      ImGui::EndDragDropTarget();
    }
    const float radius = inset * .34f;
    const float stroke = std::max(1.0f, inset / 16.0f);
    const bool entities_visible = !map_only_ && show_entities_;
    struct SpriteFrame {
      ImTextureID texture{};
      ImVec2 min, max, uv_min, uv_max;
    };
    const auto sprite_frame = [&](const Marker &m, ImVec2 pos) -> SpriteFrame {
      if(!*m.sprite.asset) return {};
      const auto &preview = sprite_preview(root, m.sprite.asset);
      const auto &asset = preview.descriptor;
      const auto &image = preview.image;
      const auto clip = *m.sprite.clip ? asset.clips.find(m.sprite.clip) : asset.clips.begin();
      if(!image.texture || (clip == asset.clips.end() && *m.sprite.clip) ||
         image.width % asset.frame_width != 0 || image.height % asset.frame_height != 0) return {};
      const unsigned columns = image.width / asset.frame_width;
      const unsigned cell = clip == asset.clips.end() ? 0 : clip->second.frames.front().cell;
      if(!columns || cell / columns >= image.height / asset.frame_height) return {};
      const double x = pos.x + m.sprite.offset_x * zoom_, y = pos.y + m.sprite.offset_y * zoom_;
      const auto sx = cell % columns * asset.frame_width, sy = cell / columns * asset.frame_height;
      return {reinterpret_cast<ImTextureID>(image.texture.get()),
              {float(x), float(y)}, {float(x + asset.frame_width * zoom_), float(y + asset.frame_height * zoom_)},
              {float(sx) / image.width, float(sy) / image.height},
              {float(sx + asset.frame_width) / image.width, float(sy + asset.frame_height) / image.height}};
    };
    const auto in_frame = [&](const SpriteFrame &frame) {
      return frame.texture && frame.min.x < bottom.x && frame.max.x > top.x &&
             frame.min.y < bottom.y && frame.max.y > top.y;
    };
    const Marker *hit = nullptr;
    if(entities_visible && hovered && !paint && !pan_modifier && dragging_ == sdk::scene_end) {
      float closest = inset * inset * .55f;
      const Marker *sprite_hit = nullptr;
      for(const auto &m : markers_) {
        const auto pos = project(m.x, m.y);
        const auto frame = sprite_frame(m, pos);
        const float dx = mouse.x - pos.x, dy = mouse.y - pos.y;
        const float distance = dx * dx + dy * dy;
        const float selected_handle = radius + 3 * stroke;
        if(m.object == selected_ && distance <= selected_handle * selected_handle) {
          hit = &m;
          break;
        }
        if(frame.texture && mouse.x >= frame.min.x && mouse.x < frame.max.x &&
           mouse.y >= frame.min.y && mouse.y < frame.max.y &&
           (!sprite_hit || m.sprite.layer <= sprite_hit->sprite.layer)) sprite_hit = &m;
        if(!in_frame(frame) && distance < closest) { closest = distance; hit = &m; }
      }
      // A visible editor handle wins over artwork; its larger pick halo does not.
      const float marker_edge = radius + stroke;
      if(sprite_hit && (!hit || (hit->object != selected_ && closest > marker_edge * marker_edge))) hit = sprite_hit;
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
    const auto paper = ImGui::GetColorU32(ImGuiCol_WindowBg);
    const auto outline = [&](ImVec2 a, ImVec2 b, ImU32 edge, ImU32 backing) {
      draw->AddRect(a, b, backing, 0, 0, 4 * stroke);
      draw->AddRect(a, b, edge, 0, 0, 2 * stroke);
    };
    const auto label = [&](ImVec2 pos, ImU32 color, const char *text, float wrap = 0.0f) {
      const auto size = ImGui::CalcTextSize(text, nullptr, false, wrap);
      pos.x = std::clamp(pos.x, top.x + stroke, std::max(top.x + stroke, bottom.x - size.x - stroke));
      pos.y = std::clamp(pos.y, top.y + stroke, std::max(top.y + stroke, bottom.y - size.y - stroke));
      draw->AddRectFilled({pos.x - stroke, pos.y - stroke}, {pos.x + size.x + stroke, pos.y + size.y + stroke}, paper);
      draw->AddText(ImGui::GetFont(), inset, pos, color, text, nullptr, wrap);
    };
    const auto visible_marker = [&](ImVec2 p) {
      return p.x >= top.x - radius && p.x <= bottom.x + radius && p.y >= top.y - radius && p.y <= bottom.y + radius;
    };
    draw->PushClipRect(top, bottom, true);
    draw->AddRectFilled(top, bottom, ImGui::GetColorU32(ImGuiCol_MenuBarBg));
    if(hit && !panning) {
      if(!writable_ || scene_.objects()[hit->object].source != 0)
        ImGui::SetTooltip("Read-only entity. Open its source scene to edit it.");
      if(active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selected_ = hit->object;
        dragging_ = writable_ && scene_.objects()[selected_].source == 0 ? selected_ : sdk::scene_end;
      }
    }
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const auto delta = ImGui::GetMouseDragDelta();
    const auto marker_world = [&](const Marker &m) {
      double x = m.x, y = m.y;
      if(m.object == dragging_ && (active || released) && (delta.x || delta.y)) {
        x += delta.x / zoom_;
        y += delta.y / zoom_;
        if(snap_) { x = std::round(x / grid_size_) * grid_size_; y = std::round(y / grid_size_) * grid_size_; }
      }
      return std::pair{x, y};
    };
    world_draw_.clear();
    if(map) {
      for(size_t i = 0; i < map->layer_order.size(); ++i) {
        if(!tiles().visible(i)) continue;
        const auto &layer = map->layers.at(map->layer_order[i]);
        world_draw_.push_back({layer.layer, i, &layer});
      }
    }
    if(entities_visible) {
      for(size_t i = 0; i < markers_.size(); ++i) {
        auto &m = markers_[i];
        const auto [x, y] = marker_world(m);
        m.canvas_pos = project(x, y);
        const auto frame = sprite_frame(m, m.canvas_pos);
        m.sprite_drawn = in_frame(frame);
        if(m.sprite_drawn) {
          world_draw_.push_back({m.sprite.layer, i, nullptr, frame.texture,
                                 frame.min, frame.max, frame.uv_min, frame.uv_max});
        }
        if(m.object == dragging_ && released && (delta.x || delta.y)) move_marker(m.object, x, y);
      }
      if(released || !active) dragging_ = sdk::scene_end;
    }
    // Draw back to front, preserving authored order within tiles and sprites on ties.
    std::sort(world_draw_.begin(), world_draw_.end(), [](const auto &a, const auto &b) {
      if(a.order != b.order) return a.order > b.order;
      if(bool(a.tile) != bool(b.tile)) return a.tile != nullptr;
      return a.index < b.index;
    });
    for(const auto &item : world_draw_) {
      if(item.tile) tiles().draw_layer(*draw, *item.tile, top, bottom, camera_x_, camera_y_, zoom_);
      else draw->AddImage(item.texture, item.min, item.max, item.uv_min, item.uv_max);
    }
    tiles().draw_property(*draw, shown_tile_property(), top, bottom, camera_x_, camera_y_, zoom_);
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
    std::optional<ImVec2> selected_pos;
    float selected_radius = radius;
    if(entities_visible) {
      for(const auto &m : markers_) {
        const auto pos = m.canvas_pos;
        const bool drawn = m.sprite_drawn;
        if(m.object == selected_) {
          selected_pos = pos;
          selected_radius = drawn ? 2 * stroke : radius;
        } else if(!drawn && visible_marker(pos)) {
          const bool editable = writable_ && scene_.objects()[m.object].source == 0;
          draw->AddCircleFilled(pos, radius + stroke, paper, 12);
          draw->AddCircleFilled(pos, radius, ImGui::GetColorU32(editable ? ImGuiCol_Text : ImGuiCol_TextDisabled), 12);
        }
      }
    }

    // Draw guides and selection last: overlapping artwork must not cover them.
    const auto guide = ImGui::GetColorU32(ImGuiCol_CheckMark);
    outline(origin, bounds, guide, paper);
    if(origin.x >= top.x && origin.y >= top.y && origin.x < bottom.x && origin.y < bottom.y) {
      char dimensions[32];
      std::snprintf(dimensions, sizeof(dimensions), "%d x %d", world_width, world_height);
      label({origin.x + 4 * stroke, origin.y + 3 * stroke}, guide, dimensions);
    }
    if(!map_only_ && show_entities_ && selected_ < scene_.objects().size()) {
      const auto &objects = scene_.objects();
      const auto &object = objects[selected_];
      bool clamp = true;
      const char *follow = nullptr;
      if(!object.prefab && camera_on(scene_, object, prefab_for(object), clamp, follow)) {
        auto target = markers_.end();
        if(follow && *follow == '@') {
          const std::string_view key(follow + 1);
          target = std::find_if(markers_.begin(), markers_.end(), [&](const auto &marker) {
            return objects[marker.object].key == key;
          });
        }
        if(target == markers_.end()) target = std::find_if(markers_.begin(), markers_.end(), [&](const auto &marker) {
          return marker.object == selected_;
        });
        if(target != markers_.end()) {
          const auto axis = [&](float coordinate, int extent, int map_extent) {
            const double offset = std::floor(double(coordinate) - extent * .5);
            return clamp && map_extent > 0 ? std::clamp(offset, 0.0, double(std::max(0, map_extent - extent))) : offset;
          };
          const double x = axis(target->x, world_width, map_size.w), y = axis(target->y, world_height, map_size.h);
          const auto a = project(x, y), b = project(x + world_width, y + world_height);
          if(a.x < bottom.x && b.x > top.x && a.y < bottom.y && b.y > top.y) {
            const auto color = IM_COL32(83, 195, 248, 255);
            outline(a, b, color, paper);
            if(visible_marker(a)) label({a.x + 4 * stroke, a.y + 3 * stroke}, color, "Camera2D");
          }
        }
      }
    }
    if(selected_pos) {
      const auto pos = *selected_pos;
      const auto &object = scene_.objects()[selected_];
      sdk::BoxCollider2D box;
      if(collider_on(scene_, object, prefab_for(object), box)) {
        const double x = pos.x + box.offset_x * zoom_, y = pos.y + box.offset_y * zoom_;
        const double right = x + box.width * zoom_, lower = y + box.height * zoom_;
        if(x < bottom.x && right > top.x && y < bottom.y && lower > top.y) {
          // Clip offscreen vertices without introducing false edges at the canvas border.
          const double pad = 4 * stroke + 1;
          outline({float(std::max(x, top.x - pad)), float(std::max(y, top.y - pad))},
                  {float(std::min(right, bottom.x + pad)), float(std::min(lower, bottom.y + pad))},
                  IM_COL32(233, 151, 55, 255), IM_COL32_BLACK);
        }
      }
      if(visible_marker(pos)) {
        draw->AddCircleFilled(pos, selected_radius, guide, 12);
        draw->AddCircle(pos, radius + 3 * stroke, paper, 12, 3 * stroke);
        draw->AddCircle(pos, radius + 3 * stroke, guide, 12, stroke);
        label({pos.x + inset * .7f, pos.y - inset * .5f}, guide, *object.name ? object.name : object.key);
      }
    }
    if(!tiles().error().empty())
      label({top.x + inset, top.y + inset}, ImGui::GetColorU32(ImGuiCol_Text), tiles().error().c_str(), width - 2 * inset);
    else if(!map_only_ && (!show_entities_ || markers_.empty()))
      label({top.x + inset, top.y + inset}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
            show_entities_ ? "No entities with Transform2D" : "Entities hidden in Layers");
    if(show_route_) {
      const auto error_color = IM_COL32(255, 100, 100, 255);
      const auto route_color = IM_COL32(87, 232, 255, 255);
      const ImVec2 status(top.x + inset, bottom.y - 2 * inset);
      if(!can_validate_route()) {
        label(status, ImGui::GetColorU32(ImGuiCol_Text),
              "Route unavailable: this scene needs a map with a saved 'path' property.", width - 2 * inset);
      } else {
        refresh_route();
        const double tile = map->tile_size();
        const auto cell_pos = [&](sdk::RouteCell cell, double offset) {
          return project((cell.x + offset) * tile, (cell.y + offset) * tile);
        };
        char text[256];
        if(route_issue_.message) {
          const auto cell = route_issue_.cell;
          if(cell.x >= 0 && cell.y >= 0 && cell.x < map->bounds.w && cell.y < map->bounds.h) {
            const auto a = cell_pos(cell, 0), b = cell_pos(cell, 1);
            draw->AddRectFilled(a, b, IM_COL32(255, 70, 70, 80));
            outline(a, b, error_color, IM_COL32_BLACK);
            std::snprintf(text, sizeof(text), "Route (%d, %d): %s", cell.x, cell.y, route_issue_.message);
          } else std::snprintf(text, sizeof(text), "Route: %s", route_issue_.message);
        } else {
          const float spacing = float(tile * zoom_);
          for(size_t i = 1; i < route_cells_.size(); ++i) {
            const auto a = cell_pos(route_cells_[i - 1], .5), b = cell_pos(route_cells_[i], .5);
            if(std::max(a.x, b.x) < top.x || std::min(a.x, b.x) > bottom.x ||
               std::max(a.y, b.y) < top.y || std::min(a.y, b.y) > bottom.y) continue;
            const float dx = float(route_cells_[i].x - route_cells_[i - 1].x);
            const float dy = float(route_cells_[i].y - route_cells_[i - 1].y);
            const ImVec2 tip(a.x + dx * spacing * .68f, a.y + dy * spacing * .68f);
            const float head = std::min(spacing * .24f, inset * .6f);
            draw->AddLine(a, b, IM_COL32_BLACK, 4 * stroke);
            draw->AddLine(a, b, route_color, 2 * stroke);
            if(head >= stroke)
              draw->AddTriangleFilled(tip, {tip.x - head * (dx + dy), tip.y - head * (dy - dx)},
                                      {tip.x - head * (dx - dy), tip.y - head * (dy + dx)}, route_color);
          }
          std::snprintf(text, sizeof(text), "Route valid: %zu cells, spawn to goal.", route_cells_.size());
        }
        label(status, ImGui::GetColorU32(ImGuiCol_Text), text, width - 2 * inset);
      }
    }
    draw->PopClipRect();
    draw->AddRect(top, bottom, ImGui::GetColorU32(ImGuiCol_Border));
  }
}
