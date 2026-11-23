#ifdef __EMSCRIPTEN__
#include "web_project.hpp"
#include <emscripten.h>
#else
#include "bridge_client.hpp"
#endif
#include "appearance.hpp"
#include "project.hpp"
#include "documents.hpp"
#include "scene_preview.hpp"
#include "sprite_assets.hpp"
#include "tabs.hpp"
#include "tileset_assets.hpp"
#include "ui_preview.hpp"

#include <SDL.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SDL_syswm.h>
#include <shobjidl.h>
#endif
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace editor {
  namespace {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    fs::path new_project_file(const fs::path &root, const fs::path &relative) {
      if(relative.empty() || relative.has_root_path() || relative.filename().empty())
        throw std::runtime_error("Enter a project-relative file path.");
      std::error_code error;
      const auto parent = fs::canonical(root / relative.parent_path(), error);
      if(error || !fs::is_directory(parent, error)) throw std::runtime_error("Choose an existing project folder.");
      const auto location = parent.lexically_relative(root);
      if(location.empty() || *location.begin() == "..") throw std::runtime_error("Choose a path inside the project.");
      return parent / relative.filename();
    }

    void write_new_file(const fs::path &path, const void *data, size_t size) {
#ifdef _WIN32
      auto *out = _wfopen(path.c_str(), L"wbx");
#else
      auto *out = std::fopen(path.c_str(), "wbx");
#endif
      if(!out) throw std::runtime_error("Cannot create file (it may already exist): " + path.filename().u8string());
      const bool written = std::fwrite(data, 1, size, out) == size;
      if(std::fclose(out) != 0 || !written) {
        std::error_code error;
        fs::remove(path, error);
        throw std::runtime_error("Cannot write file: " + path.filename().u8string());
      }
    }

#ifndef __EMSCRIPTEN__
    struct EntityRow {
      int index;
      std::string name, label;
      std::vector<std::string> components;
    };

    struct Field {
      std::string name, type, value, draft;

      bool editable() const {
        return type == "bool" || type == "int32" || type == "uint32" || type == "float" || type == "string";
      }
    };

    bool console_name(const std::string &name) {
      return !name.empty() && name.find_first_of(". \t\r\n") == std::string::npos;
    }

    const char *description(Operation op) {
      switch(op) {
        case Operation::Connect: return "Connect";
        case Operation::Disconnect: return "Disconnect";
        case Operation::Entities: return "List entities";
        case Operation::Components: return "List component types";
        case Operation::Get: return "Read component";
        case Operation::Set: return "Apply field";
        case Operation::Console: return "Console";
        case Operation::Pause: return "Pause";
        case Operation::Resume: return "Resume";
      }
      return "Bridge request";
    }
#endif

    class Workspace {
    public:
      enum PanelId { Viewport2DPanel, SpritesPanel, DocumentPanel, UiPanel, AudioPanel, EntitiesPanel, PrefabsPanel, LayersPanel,
#ifndef __EMSCRIPTEN__
                     LivePanel,
#endif
                     FilesPanel, TilesetsPanel,
#ifndef __EMSCRIPTEN__
                     ToolsPanel,
#endif
                     PanelCount };
      PanelId focused_panel() const { return focused_panel_; }

      template<class Draw> void draw(const Draw &content) {
        const auto pos = ImGui::GetCursorScreenPos();
        const auto size = ImGui::GetContentRegionAvail();
        const float gap = std::max(3.0f, ImGui::GetStyle().ItemSpacing.x * .5f);
        if(size.x < gap + 2 || size.y < gap + 2) return;
        const float top = split("vertical split", pos, size, ImGuiAxis_Y, gap, top_ratio_);
        const float left = split("horizontal split", pos, {size.x, top}, ImGuiAxis_X, gap, left_ratio_);
        draw_region(Main, {left, top}, content);
        ImGui::SetCursorScreenPos({pos.x + left + gap, pos.y});
        draw_region(Right, {size.x - left - gap, top}, content);
        ImGui::SetCursorScreenPos({pos.x, pos.y + top + gap});
        draw_region(Bottom, {size.x, size.y - top - gap}, content);
      }

      void show(PanelId index, bool focus = false) {
        panels_[index].open = panels_[index].select = true;
        if(focus) focus_request_ = index;
      }

      bool is_open(PanelId index) const { return panels_[index].open; }

    private:
      enum Region { Main, Right, Bottom };

      static float split(const char *id, ImVec2 pos, ImVec2 size, ImGuiAxis axis, float gap, float &ratio) {
        const bool horizontal = axis == ImGuiAxis_X;
        const float total = (horizontal ? size.x : size.y) - gap;
        const float minimum = std::min(total * .5f, ImGui::GetFrameHeight() * 2.5f);
        float first = std::clamp(std::floor(total * ratio), minimum, total - minimum);
        float second = total - first;
        if(horizontal) pos.x += first;
        else pos.y += first;
        const ImRect bar(pos, {pos.x + (horizontal ? gap : size.x), pos.y + (horizontal ? size.y : gap)});
        if(ImGui::SplitterBehavior(bar, ImGui::GetID(id), axis, &first, &second, minimum, minimum))
          ratio = first / total;
        return first;
      }

      void move_panel(PanelId index, Region region) {
        panels_[index].region = region;
        show(index);
      }

      template<class Draw> void draw_region(Region region, ImVec2 size, const Draw &content) {
        ImGui::PushID(region);
        if(ImGui::BeginChild("dock", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar) &&
           ImGui::BeginTabBar("panels")) {
          auto *bar = GImGui->CurrentTabBar;
          ImGuiID hovered_tab = 0;
          std::optional<PanelId> selected;
          for(int i = 0; i < PanelCount; ++i) {
            const auto &panel = panels_[i];
            if(panel.region != region || !panel.open) ImGui::SetTabItemClosed(panel.name);
          }
          for(int i = 0; i < PanelCount; ++i) {
            auto &panel = panels_[i];
            if(panel.region != region || !panel.open) continue;
            const bool visible = begin_tab(panel.name, &panel.open, panel.select ? ImGuiTabItemFlags_SetSelected : 0);
            panel.select = false;
            const auto id = ImGui::GetItemID();
            if(ImGui::IsItemClicked()) focused_panel_ = static_cast<PanelId>(i);
            if(ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) hovered_tab = id;
            if(ImGui::BeginDragDropSource()) {
              ImGui::SetDragDropPayload("EDITOR_PANEL", &i, sizeof(i), ImGuiCond_Once);
              ImGui::TextUnformatted(panel.name);
              ImGui::EndDragDropSource();
            }
            if(visible) {
              if(panel.open) selected = static_cast<PanelId>(i);
              ImGui::EndTabItem();
            }
          }
          const bool add = ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing);
          ImGui::SetItemTooltip("Open or move a panel into this dock");
          // Keep the visual order after adding tabs; only a committed drop reorders them.
          bar->TabsAddedNew = false;
          ImGui::EndTabBar();
          if(add) ImGui::OpenPopup("Add panel");
          if(ImGui::BeginPopup("Add panel")) {
            for(int i = 0; i < PanelCount; ++i) {
              const auto &panel = panels_[i];
              const bool here = panel.open && panel.region == region;
              if(ImGui::MenuItem(panel.name, nullptr, here, !here))
                move_panel(static_cast<PanelId>(i), region);
            }
            ImGui::EndPopup();
          }
          if(ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->Rect(), ImGui::GetID("drop"))) {
            if(const auto *payload = ImGui::AcceptDragDropPayload("EDITOR_PANEL")) {
              const auto index = static_cast<PanelId>(*static_cast<const int *>(payload->Data));
              if(panels_[index].region != region) move_panel(index, region);
              else if(hovered_tab && hovered_tab != payload->SourceId) {
                auto *source = ImGui::TabBarFindTabByID(bar, payload->SourceId);
                auto *target = ImGui::TabBarFindTabByID(bar, hovered_tab);
                if(source && target) {
                  const int offset = ImGui::TabBarGetTabOrder(bar, target) - ImGui::TabBarGetTabOrder(bar, source);
                  ImGui::TabBarQueueReorder(bar, source, offset);
                }
              }
            }
            ImGui::EndDragDropTarget();
          }
          if(focused_panel_ != PanelCount && panels_[focused_panel_].region == region && !panels_[focused_panel_].open)
            focused_panel_ = selected.value_or(PanelCount);
          if(selected) {
            ImGui::PushID(*selected);
            if(ImGui::BeginChild("content")) {
              if(focus_request_ == selected) {
                ImGui::SetWindowFocus();
                focus_request_.reset();
              }
              if(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) focused_panel_ = *selected;
              content(*selected);
            }
            ImGui::EndChild();
            ImGui::PopID();
          }
        }
        ImGui::EndChild();
        ImGui::PopID();
      }
      struct Panel { const char *name; Region region; bool open = true, select = false; };
      Panel panels_[PanelCount] = {{"2D Viewport", Main}, {"Sprites", Main}, {"Documents", Main}, {"UI", Main}, {"Audio", Main},
                                   {"Entities", Right}, {"Prefabs", Right},
                                   {"Layers", Right},
#ifndef __EMSCRIPTEN__
                                   {"Live", Right},
#endif
                                   {"Project Files", Bottom}, {"Tilesets", Bottom},
#ifndef __EMSCRIPTEN__
                                   {"Tools", Bottom},
#endif
                                  };
      PanelId focused_panel_ = Viewport2DPanel;
      std::optional<PanelId> focus_request_;
      float top_ratio_ = .64f, left_ratio_ = .66f;
    };

    class Editor {
    public:
      Editor([[maybe_unused]] const char *address, const char *project, SDL_Renderer *renderer) : renderer_(renderer), project_(project),
        documents_(project_.root(), renderer), tilesets_(renderer, project_.root()), sprites_(renderer, project_.root()), maps_(project_.root()) {
        if(project_.loaded()) open_scene(project_.scene());
#ifndef __EMSCRIPTEN__
        if(address) {
          address_ = address;
          if(!project) connect();
        }
#endif
      }

      bool has_unsaved_files() const {
        return documents_.dirty() || tilesets_.dirty() || sprites_.dirty() ||
          std::any_of(scenes_.begin(), scenes_.end(), [](const auto &scene) { return scene->dirty(); });
      }

#ifdef __EMSCRIPTEN__
      const fs::path &project_root() const { return project_.root(); }
#else
      bool animate_preview() const { return workspace_.is_open(Workspace::SpritesPanel) && sprites_.playing(); }
      bool needs_poll() const {
        return busy_ || project_.active();
      }
#endif
#ifdef _WIN32
      bool take_open_project_request() { return std::exchange(open_project_requested_, false); }
      std::optional<std::string> take_new_project_request() { return std::exchange(new_project_request_, std::nullopt); }
#endif

      void invalidate_maps() {
        for(const auto &scene : scenes_) scene->invalidate_maps();
      }

#ifndef __EMSCRIPTEN__
      void tick() {
        if(auto reply = client_.take_reply()) receive(*reply);
        project_.tick();
        if(project_.running() && !connected_ && !busy_ && auto_connect_ && SDL_GetTicks64() >= next_connect_) {
          next_connect_ = SDL_GetTicks64() + 750;
          connect();
        }
      }
#endif

      bool draw(Appearance &appearance) {
        maps_.finish_frame();

        const auto *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("32blit editor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);
        const bool running = draw_header(appearance);
#ifndef __EMSCRIPTEN__
        if(has_world_ && !busy_ && ImGui::IsKeyPressed(ImGuiKey_F5, false))
          submit({Operation::Entities});
#endif
        workspace_.draw([this](Workspace::PanelId panel) { draw_panel(panel); });
        documents_.draw_dialogs();
        route_file_request();
        ImGui::End();
        return running;
      }

    private:
      using PanelId = Workspace::PanelId;

      ScenePreview *scene_context() const {
        return workspace_.is_open(Workspace::Viewport2DPanel) ? active_scene_ : nullptr;
      }

      void draw_panel(PanelId index) {
#ifdef __EMSCRIPTEN__
        if(!project_.loaded()) {
          ImGui::TextDisabled("Create a new project or import a project ZIP.");
          return;
        }
#endif
        switch(index) {
          case Workspace::Viewport2DPanel: draw_scenes(); break;
          case Workspace::DocumentPanel: documents_.draw(Documents::Text); break;
          case Workspace::UiPanel: documents_.draw(Documents::UI); break;
          case Workspace::AudioPanel: documents_.draw(Documents::Audio); break;
          case Workspace::TilesetsPanel: {
            auto *scene = scene_context();
            if(scene) scene->prepare_map(project_);
            static const std::string no_tileset;
            tilesets_.draw(scene ? scene->tileset_path() : no_tileset, scene ? scene->selected_brush() : -1,
                           [scene](int tile) { if(scene) scene->select_brush(tile); },
                           [this] { documents_.refresh_files(); invalidate_maps(); },
                           [this] { documents_.refresh_files(); return documents_.pngs(); });
            break;
          }
          case Workspace::SpritesPanel:
            sprites_.draw([this] { documents_.refresh_files(); return documents_.pngs(); },
                          [this] { documents_.refresh_files(); });
            break;
          case Workspace::EntitiesPanel:
          case Workspace::PrefabsPanel:
          case Workspace::LayersPanel:
            if(auto *scene = scene_context()) {
              ImGui::PushID(scene->path().c_str());
              if(index == Workspace::LayersPanel) scene->draw_layers(project_);
              else if(!scene->map_only()) scene->draw_objects(project_, index == Workspace::PrefabsPanel, documents_);
              else ImGui::TextDisabled("Open a scene to edit entities or prefabs.");
              ImGui::PopID();
            } else ImGui::TextDisabled("No scene open.");
            break;
#ifndef __EMSCRIPTEN__
          case Workspace::LivePanel: draw_live(); break;
          case Workspace::ToolsPanel: draw_tools(); break;
#endif
          case Workspace::FilesPanel:
            documents_.draw_files([this](const fs::path &file) {
              const auto matches = [&](std::string_view relative) {
                if(relative.empty()) return false;
                std::error_code error;
                return fs::equivalent(file, project_.root() / fs::u8path(relative), error);
              };
              if(matches("blit-project.json") || matches(project_.registry()) || matches(project_.scene())) return true;
              if(tilesets_.uses_file(matches) || sprites_.uses_file(matches)) return true;
              for(const auto &view : scenes_)
                if(matches(view->path()) || matches(view->file_path(true)) || matches(view->file_path(true) + ".json") ||
                   matches(view->tileset_path())) return true;
              return false;
            });
            break;
          case Workspace::PanelCount: break;
        }
      }

      void open_scene(std::string path) {
        const auto existing = std::find_if(scenes_.begin(), scenes_.end(), [&](const auto &scene) { return scene->path() == path; });
        if(existing == scenes_.end()) {
          scenes_.push_back(std::make_unique<ScenePreview>(std::move(path), renderer_, maps_));
          active_scene_ = scenes_.back().get();
        } else active_scene_ = existing->get();
        focus_scene_ = true;
        workspace_.show(Workspace::Viewport2DPanel);
      }

      void route_file_request() {
        if(const auto panel = documents_.take_panel_request())
          workspace_.show(*panel == Documents::Audio ? Workspace::AudioPanel :
                          *panel == Documents::UI ? Workspace::UiPanel : Workspace::DocumentPanel, true);
        auto path = documents_.take_asset_request();
        if(path.empty()) return;
        const auto ext = fs::u8path(path).extension();
        if(ext == ".scene" || ext == ".map") open_scene(std::move(path));
        else if(fs::u8path(path).stem().extension() == ".sprite") {
          sprites_.open(path);
          workspace_.show(Workspace::SpritesPanel);
        }
        else {
          tilesets_.open(path);
          workspace_.show(Workspace::TilesetsPanel);
        }
      }

      bool loses_map(const ScenePreview &view) const {
        return view.dirty(true) && std::none_of(scenes_.begin(), scenes_.end(), [&](const auto &other) {
          return other.get() != &view && view.other_map_view(*other);
        });
      }

      void create_viewport_asset() {
        auto relative = fs::u8path(new_asset_path_).lexically_normal();
        if(!relative.has_extension()) relative += new_map_ ? ".map" : ".scene";
        if(relative.extension() != (new_map_ ? ".map" : ".scene") || relative.stem().empty())
          throw std::runtime_error(new_map_ ? "Use a .map filename." : "Use a .scene filename.");
        const auto dest = new_project_file(project_.root(), relative);
        if(new_map_) {
          if(new_cells_[0] < 1 || new_cells_[0] > 1024 || new_cells_[1] < 1 || new_cells_[1] > 1024)
            throw std::runtime_error("Map dimensions must be between 1 and 1024 cells.");
          if(new_tileset_.empty()) throw std::runtime_error("Import and select a tileset first.");
          blit::Map map({0, 0, new_cells_[0], new_cells_[1]});
          map.layer_order.emplace_back("Ground");
          const auto cells = static_cast<size_t>(new_cells_[0]) * new_cells_[1];
          map.layers.emplace("Ground", blit::MapLayer{&map, "Ground", std::vector<uint8_t>(cells), std::vector<uint8_t>(cells)});
          const auto bytes = blit::write_map(map);
          if(bytes.empty()) throw std::runtime_error("Cannot serialize the new tilemap.");
          auto metadata = dest;
          metadata += ".json";
          const auto json = Json{{"tileset", new_tileset_}}.dump(2) + '\n';
          write_new_file(metadata, json.data(), json.size());
          try { write_new_file(dest, bytes.data(), bytes.size()); }
          catch(...) {
            std::error_code error;
            fs::remove(metadata, error);
            throw;
          }
        } else {
          constexpr std::string_view scene = "scene 1 Untitled\n";
          write_new_file(dest, scene.data(), scene.size());
        }
        documents_.refresh_files();
        invalidate_maps();
        open_scene(dest.lexically_relative(project_.root()).generic_u8string());
      }

      void draw_new_asset(bool add) {
        if(add) ImGui::OpenPopup("New viewport file");
        bool create = false;
        if(ImGui::BeginPopup("New viewport file")) {
          if(ImGui::MenuItem("New Scene")) { new_map_ = false; create = true; }
          if(ImGui::MenuItem("New Tilemap")) { new_map_ = true; create = true; }
          ImGui::EndPopup();
        }
        if(create) {
          new_asset_path_.clear();
          new_asset_error_.clear();
          new_tileset_.clear();
          if(new_map_) {
            documents_.refresh_files();
            new_tilesets_ = documents_.tilesets();
            if(new_tilesets_.size() == 1) new_tileset_ = new_tilesets_.front();
            if(active_scene_ && !active_scene_->map_only()) {
              auto sibling = fs::u8path(active_scene_->path());
              sibling.replace_extension(".map");
              std::error_code error;
              if(!fs::exists(project_.root() / sibling, error) && !error) new_asset_path_ = sibling.generic_u8string();
            }
          }
          ImGui::OpenPopup("Create viewport file");
        }
        if(ImGui::BeginPopupModal("Create viewport file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          ImGui::SeparatorText(new_map_ ? "New Tilemap" : "New Scene");
          ImGui::InputTextWithHint("Path", new_map_ ? "scenes/level.map" : "scenes/level.scene", &new_asset_path_);
          if(new_map_) {
            ImGui::InputInt2("Width / Height (cells)", new_cells_);
            if(ImGui::BeginCombo("Tileset", new_tileset_.empty() ? "Choose a tileset" : new_tileset_.c_str())) {
              for(const auto &path : new_tilesets_)
                if(ImGui::Selectable(path.c_str(), new_tileset_ == path)) new_tileset_ = path;
              ImGui::EndCombo();
            }
            if(new_tilesets_.empty()) ImGui::TextDisabled("Use Tilesets > + to import an atlas first.");
          }
          ImGui::BeginDisabled(new_asset_path_.empty() || (new_map_ && new_tileset_.empty()));
          if(ImGui::Button("Create")) {
            try { create_viewport_asset(); ImGui::CloseCurrentPopup(); }
            catch(const std::exception &e) { new_asset_error_ = e.what(); }
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
          if(!new_asset_error_.empty()) ImGui::TextWrapped("%s", new_asset_error_.c_str());
          ImGui::EndPopup();
        }
      }

      void draw_new_ui_layout(bool show) {
        if(show) {
          new_ui_path_ = "assets/menu.ui.json";
          new_ui_error_.clear();
          ImGui::OpenPopup("New UI Layout");
        }
        if(!ImGui::BeginPopupModal("New UI Layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("Path", &new_ui_path_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Create a .ui.json file in an existing project folder.");
        ImGui::BeginDisabled(new_ui_path_.empty());
        if((ImGui::Button("Create") || enter) && !new_ui_path_.empty()) {
          try {
            if(new_ui_path_.find('\0') != new_ui_path_.npos || new_ui_path_.find(':') != new_ui_path_.npos)
              throw std::runtime_error("Enter a project-relative file path.");
            auto relative = fs::u8path(new_ui_path_).lexically_normal();
            if(!relative.filename().empty() && !relative.has_extension()) relative += ".ui.json";
            if(!is_ui_layout(relative.filename().u8string())) throw std::runtime_error("Use a .ui.json filename.");
            const auto dest = new_project_file(project_.root(), relative);
            sdk::ui::Layout layout;
            layout.width = 320;
            layout.height = 240;
            const auto json = sdk::ui::write_layout(layout).dump(2) + '\n';
            write_new_file(dest, json.data(), json.size());
            documents_.refresh_files();
            if(!documents_.open(dest.lexically_relative(project_.root()).generic_u8string()))
              throw std::runtime_error("Created the UI layout but could not open it. Try opening it from Project Files.");
            ImGui::CloseCurrentPopup();
          } catch(const std::exception &error) { new_ui_error_ = error.what(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        if(!new_ui_error_.empty()) ImGui::TextWrapped("%s", new_ui_error_.c_str());
        ImGui::EndPopup();
      }

      void draw_scenes() {
        ScenePreview *closed = nullptr;
        bool add = false;
        if(ImGui::BeginTabBar("viewport files", ImGuiTabBarFlags_FittingPolicyScroll)) {
          for(const auto &scene : scenes_) {
            ImGuiTabItemFlags flags = scene->dirty() ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
            if(focus_scene_ && scene.get() == active_scene_) flags |= ImGuiTabItemFlags_SetSelected;
            bool open = true;
            if(begin_tab(scene->path().c_str(), &open, flags)) {
              if(!focus_scene_ || scene.get() == active_scene_) {
                active_scene_ = scene.get();
                focus_scene_ = false;
              }
              ImGui::EndTabItem();
            }
            if(!open) {
              if(scene->dirty(false) || loses_map(*scene)) {
                pending_close_ = scene.get();
              } else closed = scene.get();
            }
          }
          if(project_.loaded()) {
            add = ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing);
            ImGui::SetItemTooltip("Create a scene or tilemap");
          }
          ImGui::EndTabBar();
        }
        draw_new_asset(add);
        if(pending_close_) {
          if(!ImGui::IsPopupOpen("Close unsaved file?")) ImGui::OpenPopup("Close unsaved file?");
          if(ImGui::BeginPopupModal("Close unsaved file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(pending_close_->path().c_str());
            if(ImGui::Button("Discard and Close")) {
              closed = std::exchange(pending_close_, nullptr);
              ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if(ImGui::Button("Cancel")) {
              pending_close_ = nullptr;
              ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
          }
        }
        if(closed) {
          if(closed == active_scene_) active_scene_ = nullptr;
          scenes_.erase(std::find_if(scenes_.begin(), scenes_.end(), [=](const auto &scene) { return scene.get() == closed; }));
          if(!active_scene_ && !scenes_.empty()) {
            active_scene_ = scenes_.front().get();
            focus_scene_ = true;
          }
        }
        if(active_scene_) {
          ImGui::PushID(active_scene_->path().c_str());
          active_scene_->draw_viewport(project_, frame_key_, pan_key_);
          ImGui::PopID();
        } else ImGui::TextDisabled("Open a scene or tilemap from Project Files, or create one with +.");
      }

#ifndef __EMSCRIPTEN__
      void draw_live() {
        if(!has_world_) {
          ImGui::TextWrapped("Play or attach to a running game with a World to inspect live entities.");
          return;
        }
        ImGui::TextDisabled("Temporary changes only");
        ImGui::BeginDisabled(busy_);
        draw_entity_picker();
        if(selected_ >= 0) draw_component_fields(entities_[selected_]);
        ImGui::EndDisabled();
      }

      void draw_tools() {
        constexpr const char *tools[] = {"Console", "Build / Game Log"};
        ImGui::SetNextItemWidth(200);
        ImGui::Combo("##bottom tool", &tool_, tools, IM_ARRAYSIZE(tools));
        ImGui::Separator();
        if(tool_ == 0) draw_console();
        else project_.draw_log();
      }
#endif

      ScenePreview *file_target(bool &map) const {
        auto *scene = scene_context();
        if(!scene) return nullptr;
        switch(workspace_.focused_panel()) {
          case Workspace::LayersPanel: map = true; return scene;
          case Workspace::Viewport2DPanel: map = scene->map_only() || scene->paint_mode(); return scene;
          case Workspace::EntitiesPanel:
          case Workspace::PrefabsPanel: return scene->map_only() ? nullptr : scene;
          default: return nullptr;
        }
      }

      bool draw_header(Appearance &appearance) {
#ifndef __EMSCRIPTEN__
        const bool can_start = !project_.active() && !connected_ && !busy_;
        bool show_attach = false;
#endif
        bool exit = false;
        bool map = false;
        auto *target = file_target(map);
        const bool tileset = workspace_.focused_panel() == Workspace::TilesetsPanel && !tilesets_.active_path().empty();
        const bool sprite = workspace_.focused_panel() == Workspace::SpritesPanel && !sprites_.active_path().empty();
        const bool ui = workspace_.focused_panel() == Workspace::UiPanel;
        const bool audio = workspace_.focused_panel() == Workspace::AudioPanel;
        const bool document = ui || audio || workspace_.focused_panel() == Workspace::DocumentPanel;
        const auto document_panel = audio ? Documents::Audio : ui ? Documents::UI : Documents::Text;
        const bool can_save = document ? documents_.active_dirty(document_panel) : sprite ? sprites_.active_dirty() :
          tileset ? tilesets_.active_dirty() : target && target->dirty(map);
        const bool can_save_all = has_unsaved_files();
        const bool can_undo = sprite ? sprites_.can_history(false) : target && target->can_history(map, false);
        const bool can_redo = sprite ? sprites_.can_history(true) : target && target->can_history(map, true);
        bool save = false, save_all = false, revert = false, undo = false, redo = false;
        bool new_ui = false;
        if(ImGui::BeginMenuBar()) {
          if(ImGui::BeginMenu("File")) {
#ifdef _WIN32
            if(ImGui::MenuItem("New Project...", nullptr, false, can_start && !can_save_all)) {
              new_project_name_.clear();
              show_new_project_ = true;
            }
            if(ImGui::MenuItem("Open Project...", nullptr, false, can_start && !can_save_all)) open_project_requested_ = true;
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && (!can_start || can_save_all))
              ImGui::SetTooltip("Save or discard your edits and stop or disconnect the game before switching projects.");
#endif
            new_ui = ImGui::MenuItem("New UI Layout...", nullptr, false, project_.loaded());
            if(ImGui::MenuItem("Open File...", nullptr, false, project_.loaded())) documents_.request_open();
            save = ImGui::MenuItem(audio ? "Save Audio Cues" : ui ? "Save UI Layout" : document ? "Save Document" : sprite ? "Save Sprite" : tileset ? "Save Tileset" : map ? "Save Map" : "Save Scene", "Ctrl+S", false, can_save);
            save_all = ImGui::MenuItem("Save All", "Ctrl+Shift+S", false, can_save_all);
            if(!document) revert = ImGui::MenuItem(sprite ? "Revert Sprite..." : tileset ? "Revert Tileset..." : map ? "Revert Map..." : "Revert Scene...", nullptr, false, can_save);
#ifndef __EMSCRIPTEN__
            ImGui::Separator();
            show_attach = ImGui::MenuItem("Attach to Running Game...", nullptr, false, can_start);
            if(project_.build_configured()) {
              ImGui::Separator();
              if(ImGui::MenuItem("Reconfigure", nullptr, false, can_start)) project_.reconfigure();
            }
            ImGui::Separator();
            exit = ImGui::MenuItem("Exit");
#endif
            ImGui::EndMenu();
          }
          if(ImGui::BeginMenu("Edit")) {
            undo = ImGui::MenuItem(sprite ? "Undo Sprite Edit" : map ? "Undo Map Edit" : "Undo Scene Edit", "Ctrl+Z", false, can_undo);
            redo = ImGui::MenuItem(sprite ? "Redo Sprite Edit" : map ? "Redo Map Edit" : "Redo Scene Edit", "Ctrl+Shift+Z", false, can_redo);
            ImGui::EndMenu();
          }
          appearance.draw_menu();
          if(ImGui::BeginMenu("Shortcuts")) {
            if(ImGui::BeginMenu("Frame Selection")) {
              for(int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key) {
                const auto choice = static_cast<ImGuiKey>(key);
                if(ImGui::MenuItem(ImGui::GetKeyName(choice), nullptr, frame_key_ == choice)) frame_key_ = choice;
              }
              ImGui::EndMenu();
            }
            if(ImGui::BeginMenu("Pan + left drag")) {
              const std::pair<ImGuiKey, const char *> options[] = {
                {ImGuiMod_Shift, "Shift"}, {ImGuiKey_Space, "Space"}, {ImGuiMod_Alt, "Alt"}
              };
              for(const auto &[key, name] : options)
                if(ImGui::MenuItem(name, nullptr, pan_key_ == key)) pan_key_ = key;
              ImGui::EndMenu();
            }
            ImGui::EndMenu();
          }
#ifndef __EMSCRIPTEN__
          if(ImGui::BeginMenu("Game")) {
            if(ImGui::MenuItem("Refresh entities", "F5", false, has_world_ && !busy_)) submit({Operation::Entities});
            if(ImGui::MenuItem("Pause", nullptr, false, connected_ && !busy_)) submit({Operation::Pause});
            if(ImGui::MenuItem("Resume", nullptr, false, connected_ && !busy_)) submit({Operation::Resume});
            if(ImGui::MenuItem("Disconnect", nullptr, false, connected_ && !busy_)) {
              auto_connect_ = false;
              submit({Operation::Disconnect});
            }
            ImGui::EndMenu();
          }
          if(project_.has_scene_preview() || project_.has_packaged_game() || project_.build_configured()) {
            if(project_.scene_preview_running()) {
              if(ImGui::MenuItem("Restart Scene Preview", nullptr, false, !can_save_all && !connected_ && !busy_))
                project_.restart_scene_preview();
              if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Stop the preview and reload the saved scene in the generic player.\nSave All first if you have unsaved changes.");
            } else if(project_.has_scene_preview()) {
              if(ImGui::MenuItem("Play Scene", nullptr, false, can_start && !can_save_all)) start_game(Project::Launch::ScenePreview);
              if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Run the saved scene with the generic player. No compiler or game.cpp.\nSave All first if you have unsaved changes.");
            }
            if(project_.has_packaged_game() && ImGui::MenuItem("Play", nullptr, false, can_start)) start_game(Project::Launch::PackagedGame);
            if(project_.build_configured()) {
              if(ImGui::MenuItem("Build & Run", nullptr, false, can_start && !can_save_all)) start_game(Project::Launch::Build);
              if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Compile and run your own C++ game with the saved scene.\nSave All first if you have unsaved changes.");
              if(ImGui::MenuItem("Dev", nullptr, false, can_start)) start_game(Project::Launch::Dev);
            }
            if(ImGui::MenuItem("Stop", nullptr, false, project_.active())) {
              project_.stop();
              auto_connect_ = false;
              if(connected_) submit({Operation::Disconnect});
            }
          }
          if(connected_)
            ImGui::TextDisabled("Connected%s%s%s", title_.empty() ? "" : " to ", title_.c_str(),
                                has_world_ ? "" : " (no World)");
          else if(busy_) ImGui::TextDisabled("Connecting...");
          else if(project_.loaded() && project_.status() != "Ready") ImGui::TextDisabled("%s", project_.status().c_str());
          else ImGui::TextDisabled("Disconnected");
          if((project_.loaded() || !feedback_.empty()) && ImGui::BeginItemTooltip()) {
            if(project_.loaded()) ImGui::Text("%s: %s", project_.name().c_str(), project_.status().c_str());
            if(!feedback_.empty()) ImGui::TextUnformatted(feedback_.c_str());
            ImGui::EndTooltip();
          }
#endif
          ImGui::EndMenuBar();
        }
        draw_new_ui_layout(new_ui);
        if(!ImGui::GetTopMostPopupModal()) {
          constexpr auto global = ImGuiInputFlags_RouteGlobal;
          if(can_save_all) save_all |= ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global);
          if(can_save) save |= ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global);
          if(!ImGui::GetIO().WantTextInput) {
            if(can_undo) undo |= ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global);
            if(can_redo) redo |= ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global);
          }
        }
        if(save_all) {
          documents_.save_all();
          sprites_.save(true);
          if(tilesets_.dirty()) {
            tilesets_.save(true);
            invalidate_maps();
          }
          for(auto it = scenes_.begin(); it != scenes_.end(); ++it) {
            auto &view = **it;
            view.save_file(project_, false);
            if(view.dirty(true) && std::none_of(scenes_.begin(), it, [&](const auto &other) { return view.other_map_view(*other); }))
              view.save_file(project_, true);
          }
        } else if(save) {
          if(document) documents_.save_active(document_panel);
          else if(sprite) sprites_.save(false);
          else if(tileset) {
            tilesets_.save(false);
            invalidate_maps();
          } else target->save_file(project_, map);
        }
        if(undo || redo) {
          if(sprite) sprites_.history(redo);
          else target->history(map, redo);
        }
        if(revert) {
          revert_path_ = sprite ? sprites_.active_path() : tileset ? tilesets_.active_path() : target->file_path(map);
          revert_sprite_ = sprite;
          revert_view_ = target;
          revert_map_ = map;
          ImGui::OpenPopup("Revert unsaved changes?");
        }
        if(ImGui::BeginPopupModal("Revert unsaved changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          ImGui::TextWrapped("Discard unsaved changes to %s?", revert_path_.c_str());
          if(revert_map_) ImGui::TextUnformatted("This also affects other views of the same map.");
          if(ImGui::Button("Discard Changes")) {
            if(revert_view_) revert_view_->revert_file(project_, revert_map_);
            else if(revert_sprite_) sprites_.revert(revert_path_);
            else tilesets_.revert(revert_path_);
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
#ifdef _WIN32
        if(std::exchange(show_new_project_, false)) ImGui::OpenPopup("New Project");
        if(ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
          ImGui::InputText("Project name", &new_project_name_);
          ImGui::TextDisabled("Create a blank scene and a C++ game in a new folder.");
          ImGui::BeginDisabled(new_project_name_.empty());
          if(ImGui::Button("Create...")) {
            new_project_request_ = new_project_name_;
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
#endif
#ifndef __EMSCRIPTEN__
        draw_attach_dialog(show_attach);
#endif
        return !exit;
      }

#ifndef __EMSCRIPTEN__
      void draw_attach_dialog(bool show_attach) {
        constexpr const char *attach = "Attach to Running Game";
        if(show_attach) {
          attach_address_ = address_;
          feedback_.clear();
          ImGui::OpenPopup(attach);
        }
        if(ImGui::BeginPopupModal(attach, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          if(connected_) ImGui::CloseCurrentPopup();
          else {
            ImGui::TextUnformatted("Enter the local bridge address for the running game.");
            ImGui::BeginDisabled(busy_);
            if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const bool enter = ImGui::InputText("##bridge address", &attach_address_, ImGuiInputTextFlags_EnterReturnsTrue);
            if(ImGui::Button("Attach") || enter) {
              if(const auto port = bridge_port(attach_address_)) {
                feedback_.clear();
                submit({Operation::Connect, {}, *port});
              } else feedback_ = "Enter 127.0.0.1:<port>, with a port from 1 to 65535.";
            }
            ImGui::SameLine();
            if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();
            if(busy_) ImGui::TextDisabled("Connecting...");
            else if(!feedback_.empty()) ImGui::TextWrapped("%s", feedback_.c_str());
          }
          ImGui::EndPopup();
        }
      }

      void start_game(Project::Launch mode) {
        auto_connect_ = mode == Project::Launch::Build || mode == Project::Launch::Dev;
        project_.start(mode, address_);
      }

      void submit(Request request) {
        if(!busy_ && client_.submit(std::move(request))) busy_ = true;
      }

      void connect() {
        if(const auto port = bridge_port(address_)) {
          feedback_.clear();
          submit({Operation::Connect, {}, *port});
        } else {
          feedback_ = "Enter 127.0.0.1:<port>, with a port from 1 to 65535.";
        }
      }

      void clear_selection() {
        selected_ = -1;
        component_.clear();
        fields_.clear();
        loaded_ = false;
      }

      void disconnected() {
        connected_ = has_world_ = false;
        title_.clear();
        entities_.clear();
        schemas_.clear();
        clear_selection();
      }

      void get_component() {
        fields_.clear();
        loaded_ = false;
        if(selected_ >= 0 && !entities_[selected_].name.empty() && !component_.empty())
          submit({Operation::Get, entities_[selected_].name + '.' + component_});
      }

      void update_entities(const Json &list) {
        if(!list.is_array()) throw std::invalid_argument("expected an entity list");
        std::vector<EntityRow> next;
        next.reserve(list.size());
        int selection = -1;
        const EntityRow *previous = selected_ < 0 ? nullptr : &entities_[selected_];
        for(const auto &item : list) {
          EntityRow entity{item.at("index").get<int>(), {}, {}, item.at("components").get<std::vector<std::string>>()};
          const auto &name = item.at("name");
          if(!name.is_null()) entity.name = name.get<std::string>();
          entity.label = "#" + std::to_string(entity.index) + "  ";
          if(entity.name.empty()) entity.label += "(unnamed)";
          else if(entity.name.find_first_of("\r\n\t") != std::string::npos) entity.label += name.dump();
          else entity.label += entity.name;
          if(previous && entity.index == previous->index && entity.name == previous->name)
            selection = static_cast<int>(next.size());
          next.push_back(std::move(entity));
        }
        entities_ = std::move(next);
        if(selection < 0) {
          clear_selection();
          return;
        }
        select_entity(selection);
      }

      void update_fields(const Json &values) {
        if(!values.is_object()) throw std::invalid_argument("expected a component object");
        const auto schema = std::find_if(schemas_.begin(), schemas_.end(), [&](const Json &item) {
          return item.at("name") == component_;
        });
        const auto *types = schema != schemas_.end() ? &schema->at("fields") : nullptr;
        std::vector<Field> next;
        next.reserve(values.size());
        for(auto item = values.begin(); item != values.end(); ++item) {
          Field field{item.key(), "unknown", item->is_string() ? item->get<std::string>() : item->dump(), {}};
          if(types) {
            const auto type = std::find_if(types->begin(), types->end(), [&](const Json &entry) {
              return entry.at("name") == field.name;
            });
            if(type != types->end()) field.type = type->at("type").get<std::string>();
          }
          if(field.editable()) field.draft = field.value;
          next.push_back(std::move(field));
        }
        fields_ = std::move(next);
        loaded_ = true;
      }

      void receive(Reply &reply) {
        busy_ = false;
        const auto op = reply.operation;
        if(!reply.transport_error.empty() || op == Operation::Disconnect) {
          disconnected();
          feedback_ = reply.transport_error.empty() ? "Disconnected." : std::move(reply.transport_error);
          return;
        }
        auto &data = reply.data;
        try {
          if(!data.at("ok").get<bool>()) {
            feedback_ = std::string(description(op)) + " failed: " + data.at("error").get<std::string>();
            if(data.contains("file")) feedback_ += " (" + data.at("file").get<std::string>() + ')';
            if(data.contains("line")) feedback_ += " at line " + data.at("line").dump();
            if(op == Operation::Components) submit({Operation::Entities});
            return;
          }
          switch(op) {
            case Operation::Connect:
              connected_ = true;
              has_world_ = data.at("world").get<bool>();
              title_ = data.at("title").get<std::string>();
              if(has_world_) submit({Operation::Components});
              break;
            case Operation::Components:
              schemas_ = std::move(data.at("components"));
              submit({Operation::Entities});
              break;
            case Operation::Entities:
              update_entities(data.at("entities"));
              break;
            case Operation::Get:
              update_fields(data.at("value"));
              break;
            case Operation::Set:
              feedback_ = "Applied " + applied_path_ + '.';
              if(renamed_to_) {
                // Name.value is the addressing key for get and console.
                entities_[selected_].name = std::move(*renamed_to_);
                renamed_to_.reset();
                submit({Operation::Entities});
              } else {
                get_component();
              }
              break;
            case Operation::Console:
              feedback_ = "Console OK: " + sent_command_;
              submit({Operation::Entities});
              break;
            case Operation::Pause:
              feedback_ = "Paused.";
              get_component();
              break;
            case Operation::Resume:
              feedback_ = "Resumed.";
              break;
            case Operation::Disconnect:
              break;
          }
        } catch(const std::exception &error) {
          feedback_ = std::string(description(op)) + ": invalid bridge response: " + error.what();
        }
      }

      void select_entity(int index) {
        selected_ = index;
        const auto &types = entities_[index].components;
        if(std::find(types.begin(), types.end(), component_) == types.end())
          component_ = types.empty() ? std::string{} : types.front();
        get_component();
      }

      void draw_entity_picker() {
        if(entities_.empty()) { ImGui::TextDisabled("No entities."); return; }
        ImGui::TextUnformatted("Entity");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##entity picker", nullptr, ImGuiComboFlags_CustomPreview)) {
          ImGuiListClipper clipper;
          clipper.Begin(static_cast<int>(entities_.size()));
          if(selected_ >= 0 && ImGui::IsWindowAppearing()) clipper.IncludeItemByIndex(selected_);
          while(clipper.Step()) for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto &row = entities_[i];
            ImGui::PushID(row.index);
            // Entity names are literal text, not ImGui labels (## or ###).
            const auto pos = ImGui::GetCursorScreenPos();
            if(ImGui::Selectable("##entity", i == selected_, 0, {0, ImGui::GetTextLineHeight()}) && i != selected_)
              select_entity(i);
            ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), row.label.c_str());
            if(i == selected_) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
        if(ImGui::BeginComboPreview()) {
          ImGui::TextUnformatted(selected_ < 0 ? "Select an entity" : entities_[selected_].label.c_str());
          ImGui::EndComboPreview();
        }
      }

      void apply_field(const EntityRow &row, const Field &field) {
        const auto &value = field.draft;
        if(value.empty() || value.find_first_of("\r\n") != std::string::npos ||
           value.front() == ' ' || value.front() == '\t' || value.back() == ' ' || value.back() == '\t') {
          feedback_ = "The console requires a nonempty, trimmed, single-line value.";
          return;
        }
        applied_path_ = row.name + '.' + component_ + '.' + field.name;
        renamed_to_.reset();
        if(component_ == "Name" && field.name == "value") renamed_to_ = value;
        submit({Operation::Set, "set " + applied_path_ + ' ' + value});
      }

      void draw_component_fields(const EntityRow &row) {
        ImGui::Spacing();
        if(row.components.empty()) {
          ImGui::TextDisabled("No components.");
          return;
        }
        ImGui::TextUnformatted("Component");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo("##component picker", component_.c_str())) {
          for(const auto &type : row.components) {
            const bool active = component_ == type;
            if(ImGui::Selectable(type.c_str(), active) && !active) {
              component_ = type;
              get_component();
            }
            if(active) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        if(row.name.empty()) {
          ImGui::TextWrapped("The bridge can only read component values by entity name.");
          return;
        }
        const bool can_edit = console_name(row.name);
        if(!can_edit) ImGui::TextWrapped("This entity name cannot be addressed by the console. Values are read-only.");
        if(loaded_ && fields_.empty()) ImGui::TextDisabled("No reflected fields.");
        for(auto &field : fields_) {
          ImGui::PushID(field.name.c_str());
          ImGui::Text("%s (%s)", field.name.c_str(), field.type.c_str());
          if(can_edit && field.editable()) {
            const float apply_width = ImGui::CalcTextSize("Apply").x + 2 * ImGui::GetStyle().FramePadding.x;
            ImGui::SetNextItemWidth(-apply_width - ImGui::GetStyle().ItemSpacing.x);
            const bool enter = ImGui::InputText("##field value", &field.draft, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool changed = field.draft != field.value;
            ImGui::BeginDisabled(busy_ || !changed);
            const bool apply = ImGui::Button("Apply");
            ImGui::EndDisabled();
            if(!busy_ && changed && (apply || enter)) apply_field(row, field);
            if(changed) ImGui::TextDisabled("Current: %s", field.value.c_str());
          } else {
            ImGui::TextWrapped("%s (read-only)", field.value.c_str());
          }
          ImGui::PopID();
        }
      }

      void draw_console() {
        ImGui::SeparatorText("Console");
        ImGui::BeginDisabled(busy_ || !has_world_);
        const float send_width = ImGui::CalcTextSize("Send").x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SetNextItemWidth(-send_width - ImGui::GetStyle().ItemSpacing.x);
        const bool enter = ImGui::InputTextWithHint("##console command", "set entity.Component.field value", &command_,
                                                   ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if((ImGui::Button("Send") || enter) && !command_.empty()) {
          sent_command_ = command_;
          submit({Operation::Console, command_});
        }
        ImGui::EndDisabled();
        if(!feedback_.empty()) ImGui::InputTextMultiline("##console output", feedback_.data(), feedback_.size() + 1,
                                                        ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
      }
#endif

      SDL_Renderer *renderer_;
      Project project_;
      Documents documents_;
      TilesetAssets tilesets_;
      SpriteAssets sprites_;
      MapDocuments maps_;
      std::vector<std::unique_ptr<ScenePreview>> scenes_;
      ScenePreview *active_scene_ = nullptr, *pending_close_ = nullptr;
      ScenePreview *revert_view_ = nullptr;
      std::string revert_path_;
      bool revert_map_ = false, revert_sprite_ = false;
      bool focus_scene_ = false;
      bool new_map_ = false;
      int new_cells_[2] = {32, 24};
      std::string new_asset_path_, new_asset_error_, new_tileset_;
      std::string new_ui_path_, new_ui_error_;
      std::vector<std::string> new_tilesets_;
      Workspace workspace_;
      ImGuiKey frame_key_ = ImGuiKey_F, pan_key_ = ImGuiMod_Shift;
#ifdef _WIN32
      bool open_project_requested_ = false;
      bool show_new_project_ = false;
      std::string new_project_name_;
      std::optional<std::string> new_project_request_;
#endif
#ifndef __EMSCRIPTEN__
      int tool_ = 0;
      bool auto_connect_ = false;
      Uint64 next_connect_ = 0;
      BridgeClient client_;
      bool busy_ = false, connected_ = false, has_world_ = false, loaded_ = false;
      std::string address_ = "127.0.0.1:8765";
      std::string attach_address_;
      std::string title_, feedback_, command_, sent_command_, component_, applied_path_;
      std::optional<std::string> renamed_to_;
      std::vector<EntityRow> entities_;
      Json schemas_;
      int selected_ = -1;
      std::vector<Field> fields_;
#endif
    };

#ifdef _WIN32
    class EditorDpi {
    public:
      EditorDpi(SDL_Window *window, const Appearance &appearance) : appearance_(appearance) {
        SDL_SysWMinfo info{};
        SDL_VERSION(&info.version);
        if(SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_WINDOWS)
          window_ = info.info.win.window;
        if(const char *windows = SDL_getenv("WINDIR")) {
          const auto font = std::filesystem::u8path(windows) / "Fonts" / "segoeui.ttf";
          std::error_code error;
          if(std::filesystem::is_regular_file(font, error)) font_ = font.u8string();
        }
        refresh();
      }

      void refresh() {
        const unsigned dpi = window_ ? GetDpiForWindow(window_) : 96;
        if(!dpi || dpi == dpi_) return;
        dpi_ = dpi;
        const float scale = dpi / 96.0f;
        appearance_.apply_scale(scale);

        // Re-rasterize at the new size, rather than stretching an old atlas.
        auto &io = ImGui::GetIO();
        ImGui_ImplSDLRenderer2_DestroyFontsTexture();
        io.FontDefault = nullptr;
        io.Fonts->Clear();
        if(font_.empty() || !io.Fonts->AddFontFromFileTTF(font_.c_str(), 16.0f * scale)) {
          ImFontConfig config;
          config.SizePixels = 16.0f * scale;
          io.Fonts->AddFontDefault(&config);
        }
      }

    private:
      const Appearance &appearance_;
      std::string font_;
      HWND window_ = nullptr;
      unsigned dpi_ = 0;
    };
#endif

    bool process_editor_event(Editor &editor, const SDL_Event &event) {
      const bool input = ImGui_ImplSDL2_ProcessEvent(&event);
      if(event.type == SDL_RENDER_DEVICE_RESET) {
        ImGui_ImplSDLRenderer2_DestroyFontsTexture();
        editor.invalidate_maps();
      }
      return input;
    }

    bool render_frame(SDL_Renderer *renderer, Editor &editor, Appearance &appearance) {
      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      const bool running = editor.draw(appearance);
      ImGui::Render();

      auto *data = ImGui::GetDrawData();
      SDL_RenderSetScale(renderer, data->FramebufferScale.x, data->FramebufferScale.y);
      SDL_SetRenderDrawColor(renderer, 25, 25, 25, 255);
      SDL_RenderClear(renderer);
      ImGui_ImplSDLRenderer2_RenderDrawData(data, renderer);
      SDL_RenderPresent(renderer);
      return running;
    }

#ifdef __EMSCRIPTEN__
    class BrowserEditor {
    public:
      BrowserEditor(SDL_Window *window, SDL_Renderer *renderer)
        : window_(window), renderer_(renderer), editor_(std::make_unique<Editor>(nullptr, nullptr, renderer)) {}

      void import_project() {
        replace_project(unpack_web_project);
      }

      void create_project(const char *name) {
        replace_project([name](const fs::path &staging) { return create_web_project(staging, name ? name : ""); });
      }

      void export_project() {
        if(editor_->project_root().empty()) throw std::runtime_error("Create or import a project first.");
        if(editor_->has_unsaved_files()) throw std::runtime_error("Use File > Save All or discard unsaved edits before exporting.");
        pack_web_project(editor_->project_root());
      }

      void resize(int width, int height) {
        int old_width = 0, old_height = 0;
        SDL_GetWindowSize(window_, &old_width, &old_height);
        if(width != old_width || height != old_height) SDL_SetWindowSize(window_, width, height);
      }

      void frame() {
        SDL_Event event;
        while(SDL_PollEvent(&event)) process_editor_event(*editor_, event);
        int width = 0, height = 0, pixels = 0, pixel_height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        if(width > 0 && SDL_GetRendererOutputSize(renderer_, &pixels, &pixel_height) == 0) {
          // Quantize to avoid rebuilding the font when a one-pixel rounding difference changes the ratio.
          const float density = std::max(1.0f, std::round(16.0f * pixels / width) / 16.0f);
          if(density != font_density_) {
            font_density_ = density;
            auto &io = ImGui::GetIO();
            ImGui_ImplSDLRenderer2_DestroyFontsTexture();
            io.FontDefault = nullptr;
            io.Fonts->Clear();
            ImFontConfig font;
            font.SizePixels = 16.0f;
            font.RasterizerDensity = density;
            io.FontDefault = io.Fonts->AddFontDefault(&font);
          }
        }
        render_frame(renderer_, *editor_, appearance_);
      }

    private:
      template<class Prepare> void replace_project(Prepare prepare) {
        const auto next = 1 - slot_;
        const auto staging = slot_path(next);
        fs::remove_all(staging);
        try {
          const auto root = prepare(staging);
          // Keep the current project until staging and editor initialization succeed.
          editor_ = std::make_unique<Editor>(nullptr, root.c_str(), renderer_);
          if(GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);
        } catch(...) {
          std::error_code error;
          fs::remove_all(staging, error);
          throw;
        }
        std::error_code error;
        fs::remove_all(slot_path(slot_), error);
        slot_ = next;
      }

      static const char *slot_path(int slot) { return slot == 0 ? "/editor-project-a" : "/editor-project-b"; }
      SDL_Window *window_;
      SDL_Renderer *renderer_;
      Appearance appearance_;
      std::unique_ptr<Editor> editor_;
      int slot_ = 0;
      float font_density_ = 0;
    };

    std::unique_ptr<BrowserEditor> browser_editor;

    template<class Action, class... Args>
    const char *browser_project_action(Action action, Args... args) {
      static std::string error;
      try {
        if(!browser_editor) throw std::runtime_error("Editor has not started.");
        (browser_editor.get()->*action)(args...);
        return "";
      } catch(const std::exception &e) {
        error = e.what();
        return error.c_str();
      }
    }

    int event_loop(SDL_Window *window, SDL_Renderer *renderer, const char *, const char *) {
      browser_editor = std::make_unique<BrowserEditor>(window, renderer);
      EM_ASM({ Module['editorReady'](); });
      // Simulate an infinite loop so run() does not destroy its SDL/ImGui state.
      emscripten_set_main_loop_arg([](void *state) { static_cast<BrowserEditor *>(state)->frame(); }, browser_editor.get(), 0, true);
      return 0;
    }
#else
#ifdef _WIN32
    fs::path pick_project_folder(HWND owner, bool creating) {
      if(FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        throw std::runtime_error("Could not initialize the Windows folder picker.");
      struct Uninitialize { ~Uninitialize() { CoUninitialize(); } } uninitialize;
      IFileOpenDialog *dialog = nullptr;
      if(FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("Could not create the Windows folder picker.");
      const auto release = [](auto *item) { item->Release(); };
      std::unique_ptr<IFileOpenDialog, decltype(release)> picker(dialog, release);
      FILEOPENDIALOGOPTIONS options;
      if(FAILED(picker->GetOptions(&options)) || FAILED(picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM)))
        throw std::runtime_error("Could not configure the Windows folder picker.");
      picker->SetTitle(creating ? L"Choose where to create your project" : L"Open 32blit project folder (contains blit-project.json)");
      const auto shown = picker->Show(owner);
      if(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
      IShellItem *selection = nullptr;
      if(FAILED(shown) || FAILED(picker->GetResult(&selection))) throw std::runtime_error("Could not open the selected folder.");
      std::unique_ptr<IShellItem, decltype(release)> item(selection, release);
      PWSTR path = nullptr;
      if(FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) throw std::runtime_error("Could not resolve the selected folder.");
      std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> name(path, CoTaskMemFree);
      return fs::path(name.get());
    }

    fs::path create_native_project(const fs::path &parent, const std::string &name,
                                   const fs::path &editor_dir, const fs::path &sdk_dir) {
      if(name.empty() || name.size() > 80 || name.front() == ' ' || name.back() == ' ' || name.back() == '.' ||
         !std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
         })) throw std::runtime_error("Use 1–80 letters, digits, spaces, hyphens or underscores for the project name.");
      const auto packaged_seed = editor_dir / "SceneStarter";
      const bool packaged = !editor_dir.empty() && fs::is_directory(packaged_seed);
      const auto seed = packaged ? packaged_seed : sdk_dir / "templates/scene-game";
      if(!packaged && (sdk_dir.empty() || !fs::is_directory(seed)))
        throw std::runtime_error("Scene starter not found. Extract the entire editor kit (including Editor/SceneStarter), "
                                 "or set 32BLIT_DIR to an SDK containing templates/scene-game and restart the editor.");
      constexpr const char *sources[] = {"game.cpp", "CMakeLists.txt", "CMakePresets.json"};
      for(const auto file : sources) {
        if(!fs::is_regular_file(seed / file))
          throw std::runtime_error("Missing starter file: " + (seed / file).u8string() + ". Restore the complete scene starter.");
      }
      const bool preview = fs::is_regular_file(seed / "Runtime/game.exe");
      if(packaged && !preview)
        throw std::runtime_error("Missing " + (seed / "Runtime/game.exe").u8string() + ". Extract the entire kit, including the preview player and its DLLs.");

      const auto root = parent / fs::u8path(name);
      if(!fs::create_directory(root)) throw std::runtime_error("A folder with that project name already exists.");
      constexpr char scene[] = "scenes/main.scene";
      const auto write_text = [&](const fs::path &path, std::string_view text) {
        write_new_file(root / path, text.data(), text.size());
      };
      try {
        fs::create_directory(root / "scenes");
        fs::create_directory(root / "assets");
        for(const auto file : sources) fs::copy_file(seed / file, root / file);
        Json manifest{{"name", name}, {"scene", scene}, {"registry", "registry.json"}, {"target", "game"},
                      {"windows", {{"preset", "windows"}, {"build_dir", "build-windows"}, {"exe", "build-windows/Debug/game.exe"}}},
                      {"linux", {{"preset", "linux"}, {"build_dir", "build-linux"}, {"exe", "build-linux/game"}}}};
        if(preview) {
          fs::copy(seed / "Runtime", root / "Runtime", fs::copy_options::recursive);
          manifest["windows"]["preview_exe"] = "Runtime/game.exe";
        }
        auto components = Json::object();
        for(uint16_t i = 0; i < sdk::component_count(); ++i) {
          const auto &type = *sdk::component_by_id(i);
          auto fields = Json::object();
          for(uint8_t j = 0; j < type.field_count; ++j)
            fields[type.fields[j].name] = sdk::field_type_name(type.fields[j].type);
          components[type.name] = std::move(fields);
        }
        write_text("registry.json", Json{{"version", 1}, {"components", std::move(components)}}.dump(2) + '\n');
        write_text(scene, "scene 1 main\n");
        write_text("blit-project.json", manifest.dump(2) + '\n');
      } catch(...) {
        std::error_code error;
        fs::remove_all(root, error);
        throw;
      }
      return root;
    }
#endif

    int event_loop(SDL_Window *window, SDL_Renderer *renderer, const char *address, const char *project) {
      Appearance appearance;
#ifdef _WIN32
      EditorDpi dpi(window, appearance);
#endif
      auto editor = std::make_unique<Editor>(address, project, renderer);
#ifdef _WIN32
      const std::unique_ptr<char, decltype(&SDL_free)> base(SDL_GetBasePath(), SDL_free);
      auto executable_dir = base ? fs::u8path(base.get()) : fs::path{};
      if(!executable_dir.has_filename()) executable_dir = executable_dir.parent_path();
      auto sdk_dir = executable_dir.empty() ? fs::path{} : executable_dir.parent_path() / "32blit-sdk";
      if(const auto *configured = SDL_getenv("32BLIT_DIR"); configured && *configured) {
        sdk_dir = fs::u8path(configured);
      } else {
        if(sdk_dir.empty() || !fs::is_regular_file(sdk_dir / "32blit-config.cmake")) sdk_dir = fs::u8path(BLIT_EDITOR_SOURCE_SDK);
        if(fs::is_regular_file(sdk_dir / "32blit-config.cmake"))
          SetEnvironmentVariableW(L"32BLIT_DIR", sdk_dir.c_str()); // inherited by CMake; a project's cache still wins
      }
#endif
      Uint64 redraw_until = SDL_GetTicks64() + 1000;
      for(;;) {
        const auto frame_start = SDL_GetTicks64();
        const bool visible = !(SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
        const int wait = visible && (frame_start < redraw_until || editor->animate_preview()) ? 0 : 100;
        SDL_Event event;
        bool repaint = false;
        for(bool pending = wait ? SDL_WaitEventTimeout(&event, wait) : SDL_PollEvent(&event);
            pending; pending = SDL_PollEvent(&event)) {
          if(event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
            return 0;
          const bool input = process_editor_event(*editor, event);
          repaint |= input || event.type == SDL_RENDER_TARGETS_RESET || event.type == SDL_RENDER_DEVICE_RESET;
          if(input && (event.type != SDL_WINDOWEVENT || event.window.event != SDL_WINDOWEVENT_EXPOSED))
            redraw_until = SDL_GetTicks64() + 1000;
        }
        const bool polling = editor->needs_poll();
        editor->tick();
        if(SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) continue;
        if(!repaint && wait && !polling && !ImGui::GetIO().WantTextInput) continue;
#ifdef _WIN32
        dpi.refresh();
#endif
        if(!render_frame(renderer, *editor, appearance)) return 0;
#ifdef _WIN32
        auto creation = editor->take_new_project_request();
        if(creation || editor->take_open_project_request()) {
          SDL_SysWMinfo info{};
          SDL_VERSION(&info.version);
          const auto owner = SDL_GetWindowWMInfo(window, &info) ? info.info.win.window : nullptr;
          try {
            if(auto folder = pick_project_folder(owner, creation.has_value()); !folder.empty()) {
              if(creation) folder = create_native_project(folder, *creation, executable_dir, sdk_dir);
              editor = std::make_unique<Editor>(address, folder.u8string().c_str(), renderer);
            }
          } catch(const std::exception &error) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, creation ? "Could not create project" : "Could not open project", error.what(), window);
          }
          redraw_until = SDL_GetTicks64() + 1000;
        }
#endif
        const auto elapsed = SDL_GetTicks64() - frame_start;
        if(elapsed < 16) SDL_Delay(static_cast<Uint32>(16 - elapsed));
      }
    }
#endif

    int run(const char *address, const char *project) {
      SDL_SetMainReady();
#ifdef _WIN32
      SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#elif defined(__EMSCRIPTEN__)
      SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif
      if(SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "Could not initialize SDL: %s\n", SDL_GetError());
        return 1;
      }
      struct QuitSDL { ~QuitSDL() { SDL_Quit(); } } quit;
      std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("32blit editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 680,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI), SDL_DestroyWindow);
      if(!window) {
        std::fprintf(stderr, "Could not create editor window: %s\n", SDL_GetError());
        return 1;
      }
#ifdef _WIN32
      SDL_SysWMinfo info{};
      SDL_VERSION(&info.version);
      if(SDL_GetWindowWMInfo(window.get(), &info) && info.subsystem == SDL_SYSWM_WINDOWS) {
        const auto icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        if(icon) {
          SendMessageW(info.info.win.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
          SendMessageW(info.info.win.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }
      }
#endif
#ifndef __EMSCRIPTEN__
      SDL_SetWindowMinimumSize(window.get(), 600, 400);
#endif
      std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC), SDL_DestroyRenderer);
      if(!renderer) renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
      if(!renderer) {
        std::fprintf(stderr, "Could not create SDL renderer: %s\n", SDL_GetError());
        return 1;
      }

      IMGUI_CHECKVERSION();
      ImGui::CreateContext();
      ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
      ImGui::GetIO().IniFilename = nullptr;
      const bool platform = ImGui_ImplSDL2_InitForSDLRenderer(window.get(), renderer.get());
      const bool graphics = platform && ImGui_ImplSDLRenderer2_Init(renderer.get());
      int result = 1;
      if(graphics) {
        result = event_loop(window.get(), renderer.get(), address, project);
        ImGui_ImplSDLRenderer2_Shutdown();
      } else {
        std::fprintf(stderr, "Could not initialize the ImGui SDL2 backends\n");
      }
      if(platform) ImGui_ImplSDL2_Shutdown();
      ImGui::DestroyContext();
      return result;
    }
  }
}

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE const char *web_create_project(const char *name) {
  return editor::browser_project_action(&editor::BrowserEditor::create_project, name);
}

extern "C" EMSCRIPTEN_KEEPALIVE const char *web_import_project() {
  return editor::browser_project_action(&editor::BrowserEditor::import_project);
}

extern "C" EMSCRIPTEN_KEEPALIVE const char *web_export_project() {
  return editor::browser_project_action(&editor::BrowserEditor::export_project);
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_resize_canvas(int width, int height) {
  if(editor::browser_editor) editor::browser_editor->resize(width, height);
}

int main() {
  try { return editor::run(nullptr, nullptr); }
  catch(const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
#else
int main(int argc, char **argv) {
  const char *address = nullptr, *project = nullptr;
  constexpr auto usage = "Usage: 32blit-editor [--project <directory>] [--connect 127.0.0.1:<port>]";
  for(int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if(flag == "--help") { std::puts(usage); return 0; }
    if(i + 1 == argc) { std::puts(usage); return 2; }
    if(flag == "--project" && !project) project = argv[++i];
    else if(flag == "--connect" && !address && editor::bridge_port(argv[i + 1])) address = argv[++i];
    else { std::puts(usage); return 2; }
  }
  try { return editor::run(address, project); }
  catch(const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
#endif
