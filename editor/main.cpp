#include "bridge_client.hpp"

#include <SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
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

    class Editor {
    public:
      explicit Editor(const char *address) {
        if(address) {
          address_ = address;
          connect();
        }
      }

      void draw() {
        if(auto reply = client_.take_reply()) receive(*reply);

        const auto *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("32blit editor", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings);
        draw_connection();
        const float browser_height = std::max(120.0f, ImGui::GetContentRegionAvail().y - 120.0f);
        const float list_width = std::min(280.0f, ImGui::GetContentRegionAvail().x * 0.35f);
        ImGui::BeginDisabled(busy_ || !has_world_);
        if(ImGui::BeginChild("entity list", ImVec2(list_width, browser_height), ImGuiChildFlags_Borders))
          draw_entities();
        ImGui::EndChild();
        ImGui::SameLine();
        if(ImGui::BeginChild("component inspector", ImVec2(0, browser_height), ImGuiChildFlags_Borders))
          draw_inspector();
        ImGui::EndChild();
        ImGui::EndDisabled();
        draw_console();
        ImGui::End();
      }

    private:
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
        selected_ = selection;
        const auto &types = entities_[selected_].components;
        if(std::find(types.begin(), types.end(), component_) == types.end())
          component_ = types.empty() ? std::string{} : types.front();
        get_component();
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

      void draw_connection() {
        ImGui::BeginDisabled(connected_ || busy_);
        ImGui::SetNextItemWidth(225.0f);
        const bool enter = ImGui::InputText("##bridge address", &address_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if(!connected_ && (ImGui::Button("Connect") || enter)) connect();
        ImGui::EndDisabled();
        if(connected_) {
          ImGui::SameLine();
          ImGui::BeginDisabled(busy_);
          if(ImGui::Button("Disconnect")) submit({Operation::Disconnect});
          ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if(connected_) {
          ImGui::Text("Connected%s%s%s", title_.empty() ? "" : " to ", title_.c_str(),
                      has_world_ ? "" : " (no World attached)");
        } else {
          ImGui::TextUnformatted(busy_ ? "Connecting..." : "Disconnected");
        }

        ImGui::BeginDisabled(busy_ || !connected_);
        ImGui::BeginDisabled(!has_world_);
        const bool refresh = ImGui::Button("Refresh entities (F5)");
        if(has_world_ && !busy_ && (refresh || ImGui::IsKeyPressed(ImGuiKey_F5, false)))
          submit({Operation::Entities});
        ImGui::EndDisabled();
        ImGui::SameLine();
        if(ImGui::Button("Pause")) submit({Operation::Pause});
        ImGui::SameLine();
        if(ImGui::Button("Resume")) submit({Operation::Resume});
        ImGui::EndDisabled();
        if(busy_ && connected_) {
          ImGui::SameLine();
          ImGui::TextDisabled("Waiting for bridge...");
        }
      }

      void draw_entities() {
        ImGui::Text("Entities (%zu)", entities_.size());
        ImGui::Separator();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entities_.size()));
        while(clipper.Step()) {
          for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto &row = entities_[i];
            ImGui::PushID(row.index);
            // Draw the label literally: entity names are not ImGui labels (## or ###).
            const auto pos = ImGui::GetCursorScreenPos();
            if(ImGui::Selectable("##entity", i == selected_, 0, ImVec2(0, ImGui::GetTextLineHeight()))) {
              selected_ = i;
              component_ = row.components.empty() ? std::string{} : row.components.front();
              get_component();
            }
            ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), row.label.c_str());
            ImGui::PopID();
          }
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

      void draw_inspector() {
        if(selected_ < 0) {
          ImGui::TextUnformatted("Select an entity to inspect its components.");
          return;
        }
        const auto &row = entities_[selected_];
        ImGui::TextUnformatted(row.label.c_str());
        if(row.components.empty()) {
          ImGui::TextDisabled("No components.");
          return;
        }
        if(ImGui::BeginCombo("Component", component_.c_str())) {
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
        if(!feedback_.empty()) ImGui::TextWrapped("%s", feedback_.c_str());
      }

      BridgeClient client_;
      bool busy_ = false, connected_ = false, has_world_ = false, loaded_ = false;
      std::string address_ = "127.0.0.1:8765";
      std::string title_, feedback_, command_, sent_command_, component_, applied_path_;
      std::optional<std::string> renamed_to_;
      std::vector<EntityRow> entities_;
      Json schemas_;
      int selected_ = -1;
      std::vector<Field> fields_;
    };

    int event_loop(SDL_Renderer *renderer, const char *address) {
      Editor editor(address);
      for(;;) {
        const auto frame_start = SDL_GetTicks();
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
          if(event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
            return 0;
          ImGui_ImplSDL2_ProcessEvent(&event);
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        editor.draw();
        ImGui::Render();

        const auto *data = ImGui::GetDrawData();
        SDL_RenderSetScale(renderer, data->FramebufferScale.x, data->FramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 25, 25, 25, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        const auto elapsed = SDL_GetTicks() - frame_start;
        if(elapsed < 16) SDL_Delay(16 - elapsed);
      }
    }

    int run(const char *address) {
      SDL_SetMainReady();
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
      SDL_SetWindowMinimumSize(window.get(), 600, 400);
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
        result = event_loop(renderer.get(), address);
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

int main(int argc, char **argv) {
  const char *address = nullptr;
  if(argc == 2 && std::string_view(argv[1]) == "--help") {
    std::puts("Usage: 32blit-editor [--connect 127.0.0.1:<port>]");
    return 0;
  }
  if(argc != 1) {
    if(argc != 3 || std::string_view(argv[1]) != "--connect" || !editor::bridge_port(argv[2])) {
      std::fputs("Usage: 32blit-editor [--connect 127.0.0.1:<port>]\n", stderr);
      return 2;
    }
    address = argv[2];
  }
  return editor::run(address);
}
