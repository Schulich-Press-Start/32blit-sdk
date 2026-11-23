#include "ui_preview.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace editor {
  namespace ui = sdk::ui;

  namespace {
    struct Box { ImVec2 min, max; };

    ImU32 color(ui::Rgba rgba) { return IM_COL32(rgba.r, rgba.g, rgba.b, rgba.a); }

    Box widget_box(const ui::Widget &widget, ImVec2 origin, float scale) {
      return {{origin.x + widget.x * scale, origin.y + widget.y * scale},
              {origin.x + (widget.x + widget.w) * scale, origin.y + (widget.y + widget.h) * scale}};
    }

    bool contains(Box box, ImVec2 point) {
      return point.x >= box.min.x && point.x < box.max.x && point.y >= box.min.y && point.y < box.max.y;
    }

    bool overlaps(Box a, Box b) {
      return a.max.x > b.min.x && a.min.x < b.max.x && a.max.y > b.min.y && a.min.y < b.max.y;
    }

    void draw_widget(ImDrawList &draw, const ui::Widget &widget, Box box, float scale, bool focused) {
      const auto style = ui::widget_style(widget, focused);
      ui::draw_widget_box(widget.w, widget.h, style, [&](int x, int y, int w, int h, ui::Rgba rgba) {
        draw.AddRectFilled({box.min.x + x * scale, box.min.y + y * scale},
                           {box.min.x + (x + w) * scale, box.min.y + (y + h) * scale}, color(rgba));
      });
      if(!style.text.a || widget.kind == ui::Kind::Panel || widget.text.empty()) return;
      auto *font = ImGui::GetFont();
      const float font_size = 8 * scale;
      ImVec2 position = box.min;
      const auto *end = widget.text.data() + widget.text.size();
      if(widget.kind == ui::Kind::Button) {
        constexpr int inset = ui::button_text_inset;
        if(widget.w <= 2 * inset || widget.h <= 2 * inset) return;
        const auto measured = font->CalcTextSizeA(font_size, FLT_MAX, 0, widget.text.data(), end);
        position = {(box.min.x + box.max.x - measured.x) * .5f, (box.min.y + box.max.y - measured.y) * .5f};
        box.min.x += inset * scale;
        box.min.y += inset * scale;
        box.max.x -= inset * scale;
        box.max.y -= inset * scale;
      }
      const ImVec4 clip{box.min.x, box.min.y, box.max.x, box.max.y};
      draw.AddText(font, font_size, position, color(style.text), widget.text.data(), end, 0, &clip);
    }

    bool edit_pair(const char *label, const char *id, int &a, int &b, int lower) {
      ImGui::TextUnformatted(label);
      int values[] = {a, b};
      if(!ImGui::InputInt2(id, values)) return false;
      values[0] = std::clamp(values[0], lower, ui::max_coordinate);
      values[1] = std::clamp(values[1], lower, ui::max_coordinate);
      if(a == values[0] && b == values[1]) return false;
      a = values[0];
      b = values[1];
      return true;
    }
    bool edit_style(ui::Widget &widget) {
      const auto defaults = ui::default_widget_style(widget.kind, widget.enabled);
      bool changed = false;
      for(const auto &field : ui::style_fields) {
        if(widget.kind == ui::Kind::Panel && field.default_color == &ui::WidgetStyle::text) continue;
        auto &override_color = widget.style.*field.override_color;
        const auto rgba = override_color.value_or(defaults.*field.default_color);
        float values[] = {rgba.r / 255.f, rgba.g / 255.f, rgba.b / 255.f, rgba.a / 255.f};
        ImGui::PushID(field.key);
        ImGui::TextUnformatted(field.label);
        ImGui::SameLine();
        ImGui::BeginDisabled(!override_color);
        if(ImGui::SmallButton("Reset")) {
          override_color.reset();
          changed = true;
        }
        ImGui::EndDisabled();
        if(ImGui::ColorEdit4("##rgba", values, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar)) {
          const auto byte = [](float v) { return static_cast<uint8_t>(std::round(std::clamp(v, 0.f, 1.f) * 255.f)); };
          override_color = ui::Rgba{byte(values[0]), byte(values[1]), byte(values[2]), byte(values[3])};
          changed = true;
        }
        ImGui::PopID();
      }
      return changed;
    }

  }

  bool UiPreview::draw(std::string &source) {
    if(needs_parse_) {
      needs_parse_ = false;
      drag_ = Drag::None;
      select(std::string::npos);
      const auto json = nlohmann::json::parse(source, nullptr, false);
      error_ = json.is_discarded() ? "Invalid JSON." : ui::read_layout(json, layout_);
      if(error_) paint_order_.clear();
      else paint_order_ = ui::paint_order(layout_);
    }
    if(error_) {
      ImGui::TextWrapped("Cannot preview this UI layout: %s", error_);
      ImGui::TextUnformatted("Use Show Text to correct the source.");
      return false;
    }

    const float width = ImGui::GetContentRegionAvail().x;
    bool changed = false;
    if(ImGui::BeginChild("UI canvas", {width * .58f, 0}, ImGuiChildFlags_Borders,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
      changed = draw_canvas();
    ImGui::EndChild();
    ImGui::SameLine();
    bool inspector_changed = false;
    if(ImGui::BeginChild("UI inspector", {0, 0}, ImGuiChildFlags_Borders)) inspector_changed = draw_inspector();
    ImGui::EndChild();
    if(inspector_changed) paint_order_ = ui::paint_order(layout_);
    changed |= inspector_changed;
    if(changed) source = ui::write_layout(layout_).dump(2) + '\n';
    return changed;
  }

  void UiPreview::select(std::size_t index) {
    if(index == selected_) return;
    selected_ = index;
    if(index < layout_.widgets.size()) id_draft_ = layout_.widgets[index].id;
    else id_draft_.clear();
  }

  bool UiPreview::draw_canvas() {
    if(ImGui::Button("Fit View")) {
      zoom_ = 1.f;
      pan_x_ = pan_y_ = 0.f;
    }
    ImGui::SetItemTooltip("Reset pan and fit the layout to this pane. Wheel: zoom at the cursor. Middle drag: pan. Left click: select. Left drag: move widget.");
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d px", layout_.width, layout_.height);
    ImGui::TextDisabled("Wheel: zoom | Middle drag: pan | Left drag: move");
    const auto area = ImGui::GetContentRegionAvail();
    const auto top = ImGui::GetCursorScreenPos();
    const float inset = ImGui::GetFontSize() * .5f;
    if(area.x <= 2 * inset || area.y <= 2 * inset) return false;
    const float fit = std::min((area.x - 2 * inset) / layout_.width, (area.y - 2 * inset) / layout_.height);
    const ImVec2 center{top.x + area.x * .5f, top.y + area.y * .5f};
    const Box viewport{top, {top.x + area.x, top.y + area.y}};

    ImGui::InvisibleButton("##canvas", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const auto &io = ImGui::GetIO();
    const auto mouse = io.MousePos;
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    if(activated) {
      drag_ = ImGui::IsItemClicked(ImGuiMouseButton_Middle) ? Drag::Pan : Drag::None;
    }
    if(!activated && drag_ == Drag::Pan &&
       (active || (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Middle)))) {
      pan_x_ += io.MouseDelta.x;
      pan_y_ += io.MouseDelta.y;
    }
    if(ImGui::IsItemHovered() && drag_ != Drag::Widget && io.MouseWheel != 0.f) {
      const float next = std::clamp(zoom_ * std::pow(1.2f, io.MouseWheel), .1f, 32.f);
      const float ratio = next / zoom_;
      // Scale the layout center about the cursor so the point under it stays put.
      pan_x_ = mouse.x - center.x + (center.x + pan_x_ - mouse.x) * ratio;
      pan_y_ = mouse.y - center.y + (center.y + pan_y_ - mouse.y) * ratio;
      zoom_ = next;
    }

    const float scale = fit * zoom_;
    const auto width = layout_.width * scale, height = layout_.height * scale;
    const ImVec2 origin{center.x + pan_x_ - width * .5f, center.y + pan_y_ - height * .5f};
    const Box canvas{origin, {origin.x + width, origin.y + height}};
    if(activated && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      auto index = std::string::npos;
      if(contains(canvas, mouse)) {
        for(auto it = paint_order_.rbegin(); it != paint_order_.rend(); ++it) {
          if(contains(widget_box(layout_.widgets[*it], origin, scale), mouse)) {
            index = *it;
            break;
          }
        }
      }
      select(index);
      if(index != std::string::npos) {
        drag_ = Drag::Widget;
        drag_x_ = layout_.widgets[index].x;
        drag_y_ = layout_.widgets[index].y;
      }
    }

    bool moved = false;
    if(drag_ == Drag::Widget && selected_ < layout_.widgets.size() &&
       (active || (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)))) {
      // Convert total displacement before rounding so small motions accumulate.
      const auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
      const auto coordinate = [scale](int start, float pixels) {
        return static_cast<int>(std::round(std::clamp(start + pixels / scale,
          static_cast<float>(ui::min_coordinate), static_cast<float>(ui::max_coordinate))));
      };
      auto &widget = layout_.widgets[selected_];
      const int x = coordinate(drag_x_, delta.x), y = coordinate(drag_y_, delta.y);
      moved = x != widget.x || y != widget.y;
      widget.x = x;
      widget.y = y;
    }
    if(!active) drag_ = Drag::None;

    auto &draw = *ImGui::GetWindowDrawList();
    draw.PushClipRect(viewport.min, viewport.max, true);
    if(overlaps(canvas, viewport)) {
      draw.PushClipRect(canvas.min, canvas.max, true);
      draw.AddRectFilled(canvas.min, canvas.max, ImGui::GetColorU32(ImGuiCol_WindowBg));
      const auto focused = ui::initial_focus_index(layout_);
      for(auto i : paint_order_) {
        const auto &widget = layout_.widgets[i];
        const auto box = widget_box(widget, origin, scale);
        if(overlaps(box, canvas) && overlaps(box, viewport)) draw_widget(draw, widget, box, scale, i == focused);
      }
      if(selected_ < layout_.widgets.size()) {
        const auto box = widget_box(layout_.widgets[selected_], origin, scale);
        draw.AddRect(box.min, box.max, ImGui::GetColorU32(ImGuiCol_TextLink), 0, 0, 2.f);
      }
      draw.PopClipRect();
      draw.AddRect(canvas.min, canvas.max, ImGui::GetColorU32(ImGuiCol_Border));
    }
    draw.PopClipRect();
    return moved;
  }

  void UiPreview::add_widget(ui::Kind kind) {
    const auto *name = ui::kind_name(kind);
    ui::Widget widget;
    widget.kind = kind;
    widget.enabled = true;
    widget.w = std::min(layout_.width, kind == ui::Kind::Panel ? 144 : 72);
    widget.h = std::min(layout_.height, kind == ui::Kind::Panel ? 64 : 18);
    widget.x = (layout_.width - widget.w) / 2;
    widget.y = (layout_.height - widget.h) / 2;
    if(kind != ui::Kind::Panel) widget.text = kind == ui::Kind::Label ? "Label" : "Button";
    for(unsigned number = 1;; ++number) {
      widget.id = std::string(name) + '_' + std::to_string(number);
      if(std::none_of(layout_.widgets.begin(), layout_.widgets.end(), [&](const auto &other) { return other.id == widget.id; })) break;
    }
    layout_.widgets.push_back(std::move(widget));
    select(layout_.widgets.size() - 1);
  }

  bool UiPreview::draw_inspector() {
    ImGui::PushItemWidth(-FLT_MIN);
    bool changed = edit_pair("Canvas size (w / h)", "##canvas size", layout_.width, layout_.height, 1);
    if(ImGui::Button("Add Widget")) ImGui::OpenPopup("Add UI widget");
    if(ImGui::BeginPopup("Add UI widget")) {
      if(ImGui::MenuItem("Panel")) { add_widget(ui::Kind::Panel); changed = true; }
      if(ImGui::MenuItem("Label")) { add_widget(ui::Kind::Label); changed = true; }
      if(ImGui::MenuItem("Button")) { add_widget(ui::Kind::Button); changed = true; }
      ImGui::EndPopup();
    }
    ImGui::BeginDisabled(layout_.widgets.empty());
    ImGui::TextUnformatted("Widget");
    if(ImGui::BeginCombo("##widget picker", nullptr, ImGuiComboFlags_CustomPreview)) {
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(layout_.widgets.size()));
      if(selected_ < layout_.widgets.size() && ImGui::IsWindowAppearing()) clipper.IncludeItemByIndex(static_cast<int>(selected_));
      while(clipper.Step()) for(int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
        const auto i = static_cast<std::size_t>(index);
        ImGui::PushID(index);
        const auto pos = ImGui::GetCursorScreenPos();
        if(ImGui::Selectable("##widget", i == selected_, 0, {0, ImGui::GetTextLineHeight()})) select(i);
        ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), layout_.widgets[i].id.c_str());
        if(i == selected_) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if(ImGui::BeginComboPreview()) {
      ImGui::TextUnformatted(selected_ < layout_.widgets.size() ? layout_.widgets[selected_].id.c_str() :
                             layout_.widgets.empty() ? "No widgets" : "Select a widget");
      ImGui::EndComboPreview();
    }
    ImGui::EndDisabled();

    if(selected_ < layout_.widgets.size()) {
      ImGui::PushID(static_cast<int>(selected_));
      auto &widget = layout_.widgets[selected_];
      ImGui::Separator();
      ImGui::TextDisabled("Kind: %s", ui::kind_name(widget.kind));
      ImGui::TextUnformatted("ID");
      if(ImGui::InputText("##widget id", &id_draft_) && !id_draft_.empty() && id_draft_ != widget.id &&
         std::none_of(layout_.widgets.begin(), layout_.widgets.end(), [&](const auto &other) { return other.id == id_draft_; })) {
        if(layout_.initial_focus == widget.id) layout_.initial_focus = id_draft_;
        widget.id = id_draft_;
        changed = true;
      }
      if(id_draft_ != widget.id) ImGui::TextWrapped("ID must be nonempty and unique; the previous ID was kept.");
      changed |= edit_pair("Position (x / y)", "##widget position", widget.x, widget.y, ui::min_coordinate);
      changed |= edit_pair("Size (w / h)", "##widget size", widget.w, widget.h, 1);
      ImGui::TextUnformatted("Layer");
      int layer = widget.layer;
      if(ImGui::InputInt("##widget layer", &layer)) {
        layer = std::clamp(layer, ui::min_coordinate, ui::max_coordinate);
        if(widget.layer != layer) {
          widget.layer = layer;
          changed = true;
        }
      }
      ImGui::SetItemTooltip("Lower layers draw in front. Ties follow widget order.");
      if(widget.kind != ui::Kind::Panel) {
        ImGui::TextUnformatted("Text");
        changed |= ImGui::InputTextMultiline("##widget text", &widget.text, {-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 3});
      }
      changed |= ImGui::Checkbox("Enabled", &widget.enabled);
      if(widget.kind == ui::Kind::Button) {
        bool initial_focus = layout_.initial_focus == widget.id;
        ImGui::BeginDisabled(!ui::is_focusable(layout_, widget) && !initial_focus);
        if(ImGui::Checkbox("Initial focus", &initial_focus)) {
          if(initial_focus) layout_.initial_focus = widget.id;
          else layout_.initial_focus.clear();
          changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("If unset or unavailable, the first enabled Button on the canvas receives focus.");
      }
      ImGui::SeparatorText("Style");
      changed |= edit_style(widget);
      if(ImGui::Button("Remove Widget")) {
        if(layout_.initial_focus == widget.id) layout_.initial_focus.clear();
        layout_.widgets.erase(layout_.widgets.begin() + static_cast<std::ptrdiff_t>(selected_));
        select(std::string::npos);
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::PopItemWidth();
    return changed;
  }
}
