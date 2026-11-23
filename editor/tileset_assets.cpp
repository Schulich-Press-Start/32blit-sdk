#include "tileset_assets.hpp"
#include "windows_png_picker.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <misc/cpp/imgui_stdlib.h>

namespace editor {
  namespace {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    constexpr int max_id = sdk::TilesetDescriptor::tile_id_count - 1;
    constexpr std::string_view descriptor_suffix = ".tileset.json";

    fs::path relative_path(std::string_view name) {
      if(name.empty() || name.find('\0') != name.npos || name.find(':') != name.npos)
        throw std::runtime_error("Expected a project-relative path.");
      const auto path = fs::u8path(name);
      if(path.has_root_path()) throw std::runtime_error("Expected a project-relative path.");
      for(const auto &part : path)
        if(part == "..") throw std::runtime_error("Path cannot contain '..'.");
      return path.lexically_normal();
    }

    fs::path project_path(const fs::path &root, const fs::path &relative) {
      const auto path = root / relative;
      const auto canonical = fs::weakly_canonical(path);
      const auto parent = fs::weakly_canonical(path.parent_path());
      const auto inside = [&](const fs::path &candidate) {
        const auto mismatch = std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end());
        return mismatch.first == root.end();
      };
      if(!inside(parent) || !inside(canonical)) throw std::runtime_error("Path must stay within the project.");
      return path;
    }

    std::string descriptor_path(std::string_view name) {
      auto path = relative_path(name).generic_u8string();
      if(path.size() <= descriptor_suffix.size() ||
         path.compare(path.size() - descriptor_suffix.size(), descriptor_suffix.size(), descriptor_suffix) != 0)
        throw std::runtime_error("Expected a .tileset.json descriptor path.");
      return path;
    }

    bool is_png(const fs::path &path) {
      auto ext = path.extension().u8string();
      for(auto &c : ext) if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
      return ext == ".png";
    }

    template<class Buffer>
    Buffer read_asset(const fs::path &path) {
      std::ifstream in(path, std::ios::binary | std::ios::ate);
      const auto size = in.tellg();
      if(size <= 0 || size > 16 * 1024 * 1024) throw std::runtime_error("Cannot read " + path.filename().u8string());
      Buffer bytes;
      bytes.resize(static_cast<size_t>(size));
      if(!in.seekg(0).read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read " + path.filename().u8string());
      return bytes;
    }

    int tile_count(const PreviewImage &image, int size) {
      return blit::atlas_tile_count(image.width, image.height, size);
    }

    int check_atlas(const PreviewImage &image, int size) {
      const int count = tile_count(image, size);
      if(!count) throw std::runtime_error("Atlas must contain at most 256 whole square cells of the selected size (IDs 1–255).");
      return count;
    }

    void check_atlas_properties(const sdk::TilesetDescriptor &descriptor, int count) {
      if(count == max_id) return;
      for(const auto &property : descriptor.properties)
        if((property.second >> (count + 1)).any())
          throw std::runtime_error("Atlas does not contain all tiles with properties.");
    }

    PreviewImage load_preview(SDL_Renderer *renderer, const fs::path &path, int size, std::string_view transparent = {}) {
      if(!is_png(path)) throw std::runtime_error("The tileset image must be a PNG.");
      PreviewImage image;
      const auto error = image.load_png(renderer, read_asset<std::vector<uint8_t>>(path), transparent);
      if(!error.empty()) throw std::runtime_error(error);
      check_atlas(image, size);
      return image;
    }

    void write_descriptor(const fs::path &path, const std::string &text, const std::string *original = nullptr) {
      auto directory = path;
      directory += ".editor-save";
      if(!fs::create_directory(directory)) throw std::runtime_error("Tileset save staging directory already exists.");
      const auto staged = directory / path.filename();
      struct Cleanup {
        const fs::path &file, &directory;
        ~Cleanup() { std::error_code error; fs::remove(file, error); fs::remove(directory, error); }
      } cleanup{staged, directory};
      std::ofstream out(staged, std::ios::binary);
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
      out.close();
      if(!out) throw std::runtime_error("Could not write staged tileset.");
      if(original) {
        if(read_asset<std::string>(path) != *original) throw std::runtime_error("Tileset changed on disk; reopen before editing.");
        fs::rename(staged, path);
      } else if(!fs::copy_file(staged, path)) {
        throw std::runtime_error("Tileset descriptor already exists.");
      }
    }
  }

  TilesetAssets::TilesetAssets(SDL_Renderer *renderer, const fs::path &root)
    : renderer_(renderer), root_(root.empty() ? root : fs::weakly_canonical(root)) {}

  void TilesetAssets::open(const std::string &path) {
    try {
      const auto name = descriptor_path(path);
      auto [it, inserted] = tabs_.try_emplace(name);
      if(inserted || !it->second.loaded) load(it->first, it->second);
      pending_tab_ = name;
      error_.clear();
    } catch(const std::exception &error) { error_ = error.what(); }
  }

  void TilesetAssets::load(const std::string &path, Tab &tab) {
    Tab loaded;
    try {
      loaded.original = read_asset<std::string>(project_path(root_, fs::u8path(path)));
      if(const auto error = sdk::read_tileset_descriptor(Json::parse(loaded.original), loaded.descriptor))
        throw std::runtime_error(error);
      const auto image = relative_path(loaded.descriptor.image);
      loaded.descriptor.image = image.generic_u8string();
      const auto &transparent = loaded.descriptor.transparent;
      if(transparent.find('\0') != transparent.npos) throw std::runtime_error("Invalid image transparency color.");
      loaded.tile_size_draft = loaded.descriptor.tile_size;
      loaded.preview = load_preview(renderer_, project_path(root_, image), loaded.descriptor.tile_size, transparent);
      std::copy(transparent.begin(), transparent.end(), loaded.transparent.begin());
      check_atlas_properties(loaded.descriptor, tile_count(loaded.preview, loaded.descriptor.tile_size));
      loaded.loaded = true;
      tab = std::move(loaded);
    } catch(const std::exception &error) {
      if(!tab.loaded) tab.descriptor.image = std::move(loaded.descriptor.image);
      tab.error = error.what();
    }
  }

  bool TilesetAssets::uses_file(const std::function<bool(std::string_view)> &matches) const {
    return std::any_of(tabs_.begin(), tabs_.end(), [&](const auto &entry) {
      const auto &[path, tab] = entry;
      return matches(path) || (!tab.descriptor.image.empty() && matches(tab.descriptor.image));
    });
  }

  bool TilesetAssets::dirty() const {
    return std::any_of(tabs_.begin(), tabs_.end(), [](const auto &item) { return item.second.dirty; });
  }

  bool TilesetAssets::active_dirty() const {
    const auto active = tabs_.find(active_tab_);
    return active != tabs_.end() && active->second.dirty;
  }

  void TilesetAssets::revert(const std::string &path) {
    if(auto tab = tabs_.find(path); tab != tabs_.end()) {
      const int selected = tab->second.selected_tile;
      load(path, tab->second);
      if(!tab->second.dirty) {
        tab->second.selected_tile = std::min(selected, tile_count(tab->second.preview, tab->second.descriptor.tile_size));
        refresh_pending_ = true;
      }
    }
  }

  void TilesetAssets::save(bool all) {
    for(auto &[path, tab] : tabs_)
      if(tab.dirty && (all || path == active_tab_)) save(path, tab);
  }

  void TilesetAssets::save(const std::string &path, Tab &tab) {
    const int previous_size = tab.descriptor.tile_size;
    try {
      Json json = Json::parse(tab.original);
      const std::string_view transparent(tab.transparent.data());
      PreviewImage preview;
      if(json.value("transparent", std::string{}) != transparent)
        preview = load_preview(renderer_, project_path(root_, fs::u8path(tab.descriptor.image)), tab.tile_size_draft, transparent);
      const int count = check_atlas(preview.texture ? preview : tab.preview, tab.tile_size_draft);
      check_atlas_properties(tab.descriptor, count);
      tab.descriptor.tile_size = tab.tile_size_draft;
      tab.descriptor.transparent = transparent;
      sdk::write_tileset_descriptor(json, tab.descriptor);
      auto text = json.dump(2) + '\n';
      write_descriptor(project_path(root_, fs::u8path(path)), text, &tab.original);
      tab.original = std::move(text);
      if(preview.texture) tab.preview = std::move(preview);
      tab.selected_tile = std::min(tab.selected_tile, count);
      tab.dirty = false;
      tab.error.clear();
      refresh_pending_ = true;
    } catch(const std::exception &error) {
      tab.descriptor.tile_size = previous_size;
      tab.error = error.what();
    }
  }

  void TilesetAssets::draw(const std::string &map_tileset, int brush, const std::function<void(int)> &select_brush,
                           const std::function<void()> &refresh_files,
                           const std::function<std::vector<std::string>()> &list_pngs) {
    if(map_tileset_ != map_tileset) {
      map_tileset_ = map_tileset;
      map_tab_.clear();
      if(!map_tileset.empty()) {
        try { map_tab_ = descriptor_path(map_tileset); }
        catch(const std::exception &error) { error_ = error.what(); }
        if(!map_tab_.empty()) {
          const auto requested = std::move(pending_tab_);
          open(map_tab_);
          if(!requested.empty()) pending_tab_ = requested;
        }
      }
    }
    if(!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
    if(ImGui::BeginTabBar("Tileset assets")) {
      if(ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) {
        import_source_ = import_descriptor_ = import_image_ = {};
        import_tile_size_ = blit::default_tile_size;
        import_error_.clear();
        project_pngs_ = list_pngs();
        if(project_pngs_.size() == 1) choose_png(project_pngs_.front());
        ImGui::OpenPopup("Import Tileset PNG");
      }
      draw_import();
      const auto requested = std::move(pending_tab_);
      pending_tab_.clear();
      for(auto it = tabs_.begin(); it != tabs_.end();) {
        auto &[path, tab] = *it;
        bool keep = true;
        ImGuiTabItemFlags flags = path == requested ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if(tab.dirty) flags |= ImGuiTabItemFlags_UnsavedDocument;
        // Save or discard intentionally before closing a dirty document.
        ImGui::PushID(path.c_str());
        const auto filename = path.c_str() + (path.find_last_of('/') + 1);
        if(ImGui::BeginTabItem(filename, tab.dirty || path == map_tab_ ? nullptr : &keep, flags)) {
          ImGui::SetItemTooltip("%s", path.c_str());
          active_tab_ = path;
          if(tab.loaded) {
            if(!tab.error.empty()) ImGui::TextWrapped("%s", tab.error.c_str());
            const float atlas_width = ImGui::GetContentRegionAvail().x * .60f;
            if(ImGui::BeginChild("Atlas", {atlas_width, 0})) draw_tiles(tab, path == map_tab_, brush, select_brush);
            ImGui::EndChild();
            ImGui::SameLine();
            if(ImGui::BeginChild("Properties")) draw_properties(tab);
            ImGui::EndChild();
          } else {
            if(ImGui::Button("Reload")) load(path, tab);
            if(!tab.error.empty()) ImGui::TextWrapped("%s", tab.error.c_str());
          }
          ImGui::EndTabItem();
        }
        ImGui::PopID();
        if(keep) ++it;
        else {
          if(active_tab_ == path) active_tab_.clear();
          it = tabs_.erase(it);
        }
      }
      ImGui::EndTabBar();
    }
    if(tabs_.empty()) ImGui::TextDisabled("Open a tileset from Project Files or use + to import one.");
    if(refresh_pending_ && refresh_files) {
      refresh_pending_ = false;
      refresh_files();
    }
  }

  void TilesetAssets::draw_tiles(Tab &tab, bool matches_map, int brush, const std::function<void(int)> &select_brush) {
    const auto highlight = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
    if(matches_map && select_brush) {
      if(brush == 0) ImGui::PushStyleColor(ImGuiCol_Button, highlight);
      const bool erase = ImGui::Button("Eraser");
      if(brush == 0) ImGui::PopStyleColor();
      if(erase) select_brush(0);
    } else ImGui::TextUnformatted("Editing tile properties only (not the active map's tileset).");
    const int cell = tab.descriptor.tile_size, columns = tab.preview.width / cell;
    const int count = tile_count(tab.preview, cell);
    if(matches_map && brush > 0 && brush <= count) tab.selected_tile = brush;

    const auto &style = ImGui::GetStyle();
    const float size = ImGui::GetFontSize() * 1.5f;
    const float stride = size + 2 * style.FramePadding.x + style.ItemSpacing.x;
    const int wrap = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / stride));
    const auto texture = reinterpret_cast<ImTextureID>(tab.preview.texture.get());
    for(int i = 0; i < count; ++i) {
      if(i % wrap) ImGui::SameLine();
      const int x = i % columns * cell, y = i / columns * cell;
      const ImVec2 uv0(float(x) / tab.preview.width, float(y) / tab.preview.height);
      const ImVec2 uv1(float(x + cell) / tab.preview.width, float(y + cell) / tab.preview.height);
      const bool selected = (matches_map ? brush : tab.selected_tile) == i + 1;
      ImGui::PushID(i);
      if(selected) ImGui::PushStyleColor(ImGuiCol_Button, highlight);
      if(ImGui::ImageButton("Tile", texture, {size, size}, uv0, uv1)) {
        tab.selected_tile = i + 1;
        if(matches_map && select_brush) select_brush(i + 1);
      }
      if(selected) ImGui::PopStyleColor();
      ImGui::SetItemTooltip("Tile %d", i + 1);
      ImGui::PopID();
    }

  }

  void TilesetAssets::draw_properties(Tab &tab) {
    ImGui::Text("Tile %d properties", tab.selected_tile);
    ImGui::SameLine();
    if(ImGui::SmallButton("Settings")) ImGui::OpenPopup("Tileset Settings");
    ImGui::SetItemTooltip("Tileset Settings");
    if(ImGui::BeginPopup("Tileset Settings")) {
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
      if(ImGui::InputInt("Tile size (px)", &tab.tile_size_draft, 0)) {
        tab.dirty = true;
        tab.error.clear();
      }
      ImGui::SetItemTooltip("Square cell edge length. The atlas and associated maps update after saving.");
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
      if(ImGui::InputText("Transparent RGB (optional)", tab.transparent.data(), tab.transparent.size(), ImGuiInputTextFlags_CharsHexadecimal)) {
        tab.dirty = true;
        tab.error.clear();
      }
      ImGui::SeparatorText("Tileset Properties");
      for(auto &[name, assignments] : tab.descriptor.properties) {
        ImGui::PushID(&assignments);
        const bool remove = ImGui::SmallButton("Delete");
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopID();
        if(remove) tab.delete_property = name;
      }
      const bool enter = ImGui::InputTextWithHint("##property name", "New property name", tab.property_name.data(),
                                                 tab.property_name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      const bool add = ImGui::Button("Add Property");
      if((enter || add) && tab.property_name[0] && tab.descriptor.properties.try_emplace(tab.property_name.data()).second) {
        tab.property_name = {};
        tab.dirty = true;
      }
      ImGui::EndPopup();
    }
    if(!tab.delete_property.empty() && !ImGui::IsPopupOpen("Delete Tileset Property")) {
      ImGui::OpenPopup("Delete Tileset Property");
    }
    if(tab.descriptor.properties.empty()) ImGui::TextDisabled("Define properties in Settings.");
    for(auto &[name, assignments] : tab.descriptor.properties) {
      ImGui::PushID(&assignments);
      bool assigned = assignments.test(tab.selected_tile);
      if(ImGui::Checkbox("##assigned", &assigned)) {
        assignments.set(tab.selected_tile, assigned);
        tab.dirty = true;
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(name.c_str());
      ImGui::PopID();
    }
    if(ImGui::BeginPopupModal("Delete Tileset Property", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      const auto property = tab.descriptor.properties.find(tab.delete_property);
      if(property != tab.descriptor.properties.end()) {
        const auto count = property->second.count();
        ImGui::TextWrapped("Delete '%s' from this tileset? This removes it from %zu tile%s.", property->first.c_str(),
                           count, count == 1 ? "" : "s");
        if(ImGui::Button("Delete Property")) {
          tab.descriptor.properties.erase(property);
          tab.dirty = true;
          tab.delete_property.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) {
          tab.delete_property.clear();
          ImGui::CloseCurrentPopup();
        }
      } else ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::TextWrapped("Image: %s", tab.descriptor.image.c_str());
  }

  void TilesetAssets::choose_png(std::string path) {
    const auto source = fs::u8path(path);
    auto descriptor = source.is_absolute() ? source.filename() : source;
    descriptor.replace_extension(descriptor_suffix);
    import_source_ = std::move(path);
    import_descriptor_ = descriptor.generic_u8string();
    import_image_.clear();
    import_error_.clear();
  }

  void TilesetAssets::draw_import() {
    if(!ImGui::BeginPopupModal("Import Tileset PNG", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if(ImGui::BeginCombo("Project PNG", import_source_.empty() ? "Choose an image" : import_source_.c_str())) {
      if(project_pngs_.empty()) ImGui::TextDisabled("No PNGs in this project.");
      for(const auto &path : project_pngs_)
        if(ImGui::Selectable(path.c_str(), path == import_source_)) choose_png(path);
      ImGui::EndCombo();
    }
#ifdef _WIN32
    if(ImGui::Button("Browse for PNG...")) {
      try {
        if(auto source = browse_png(root_); !source.empty()) choose_png(std::move(source));
      } catch(const std::exception &error) { import_error_ = error.what(); }
    }
#elif !defined(__EMSCRIPTEN__)
    if(ImGui::InputText("External PNG path (optional)", &import_source_)) {
      auto descriptor = fs::u8path(import_source_).filename();
      if(!descriptor.empty()) descriptor.replace_extension(descriptor_suffix);
      import_descriptor_ = descriptor.generic_u8string();
    }
#endif
    ImGui::InputText("Tileset file", &import_descriptor_);
    ImGui::InputInt("Tile size (px)", &import_tile_size_, 0);
    if(fs::u8path(import_source_).is_absolute())
      ImGui::InputTextWithHint("Project PNG copy path", "Defaults to beside the tileset", &import_image_);
    ImGui::BeginDisabled(import_source_.empty() || import_descriptor_.empty());
    if(ImGui::Button("Import")) {
      try {
        import_png();
        ImGui::CloseCurrentPopup();
      } catch(const std::exception &error) { import_error_ = error.what(); }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    if(!import_error_.empty()) ImGui::TextWrapped("%s", import_error_.c_str());
    ImGui::EndPopup();
  }

  void TilesetAssets::import_png() {
    if(root_.empty()) throw std::runtime_error("Open a project before importing a tileset.");
    const auto name = descriptor_path(import_descriptor_);
    if(const auto tab = tabs_.find(name); tab != tabs_.end() && tab->second.dirty)
      throw std::runtime_error("This descriptor has unsaved edits in an open tab.");
    const auto descriptor = fs::u8path(name);
    const auto output = project_path(root_, descriptor);
    if(fs::symlink_status(output).type() != fs::file_type::not_found)
      throw std::runtime_error("Tileset descriptor already exists.");
    if(!fs::is_directory(output.parent_path())) throw std::runtime_error("The descriptor folder must already exist.");
    const auto source = fs::u8path(import_source_);
    const bool copy = source.is_absolute();
    if(!copy && !import_image_.empty()) throw std::runtime_error("A PNG copy path is only used with an absolute source path.");
    const auto image = copy ? (!import_image_.empty() ? relative_path(import_image_) : descriptor.parent_path() / source.filename())
                            : relative_path(import_source_);
    const auto image_path = project_path(root_, image);
    if(!is_png(image)) throw std::runtime_error("The tileset image path must end with .png.");
    auto preview = load_preview(renderer_, copy ? source : image_path, import_tile_size_);
    sdk::TilesetDescriptor asset;
    asset.image = image.generic_u8string();
    asset.tile_size = import_tile_size_;
    auto json = Json::object();
    sdk::write_tileset_descriptor(json, asset);
    auto text = json.dump(2) + '\n';
    if(copy) {
      if(fs::symlink_status(image_path).type() != fs::file_type::not_found) throw std::runtime_error("PNG copy destination already exists.");
      if(!fs::is_directory(image_path.parent_path())) throw std::runtime_error("The PNG copy folder must already exist.");
      if(!fs::copy_file(source, image_path)) throw std::runtime_error("Could not copy PNG without overwriting.");
    }
    try { write_descriptor(output, text); }
    catch(...) {
      if(copy) { std::error_code error; fs::remove(image_path, error); }
      throw;
    }
    auto &tab = tabs_[name];
    tab = {};
    tab.descriptor = std::move(asset);
    tab.tile_size_draft = tab.descriptor.tile_size;
    tab.original = std::move(text);
    tab.preview = std::move(preview);
    tab.loaded = true;
    pending_tab_ = name;
    refresh_pending_ = true;
  }
}
