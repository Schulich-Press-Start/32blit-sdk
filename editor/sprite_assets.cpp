#include "sprite_assets.hpp"
#include "windows_png_picker.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace editor {
  namespace {
    namespace fs = std::filesystem;
    constexpr std::string_view suffix = ".sprite.json";
    constexpr int max_png_size = 16 * 1024 * 1024;
    constexpr int max_descriptor_size = 16 * 1024 * 1024;
    constexpr int max_dimension = 2048; // PreviewImage's PNG decoder limit.
    constexpr size_t max_frames = 1024;

    fs::path relative_path(std::string_view name) {
      if(name.empty() || name.find('\0') != name.npos || name.find(':') != name.npos)
        throw std::runtime_error("Expected a project-relative path.");
      const auto path = fs::u8path(name);
      if(path.has_root_path()) throw std::runtime_error("Expected a project-relative path.");
      for(const auto &part : path) {
        if(part == "..") throw std::runtime_error("Path cannot contain '..'.");
#ifdef _WIN32
        const auto &component = part.native();
        if(part != "." && !component.empty() && (component.back() == L'.' || component.back() == L' '))
          throw std::runtime_error("Path components cannot end with a space or dot.");
#endif
      }
      return path.lexically_normal();
    }

    fs::path project_file(const fs::path &root, const fs::path &relative) {
      if(root.empty()) throw std::runtime_error("Open a project first.");
      const auto path = root / relative;
      const auto canonical = fs::weakly_canonical(path);
      if(std::mismatch(root.begin(), root.end(), canonical.begin(), canonical.end()).first != root.end())
        throw std::runtime_error("Path must stay within the project.");
      const auto status = fs::symlink_status(path);
      if(fs::exists(status) && !fs::is_regular_file(status))
        throw std::runtime_error("Expected a regular project file, not a directory or link.");
      return path;
    }

    std::string descriptor_path(std::string_view input) {
      auto path = relative_path(input).generic_u8string();
      const auto filename = fs::u8path(path).filename().generic_u8string();
      if(filename.size() <= suffix.size() || filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
        throw std::runtime_error("The sprite file is separate from the PNG. Use a name like assets/player.sprite.json.");
      return path;
    }

    template<class Buffer>
    Buffer read_asset(const fs::path &path, int limit) {
      std::ifstream in(path, std::ios::binary | std::ios::ate);
      const auto size = in.tellg();
      if(size <= 0 || size > limit) throw std::runtime_error("File is empty, unreadable, or exceeds 16 MiB: " + path.filename().u8string());
      Buffer bytes(static_cast<size_t>(size), 0);
      if(!in.seekg(0).read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read " + path.filename().u8string());
      return bytes;
    }

    std::vector<uint8_t> read_png(const fs::path &path) {
      auto ext = path.extension().u8string();
      for(auto &c : ext) if(c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if(ext != ".png") throw std::runtime_error("The sprite atlas must be a PNG.");
      return read_asset<std::vector<uint8_t>>(path, max_png_size);
    }

    PreviewImage decode_png(SDL_Renderer *renderer, const std::vector<uint8_t> &bytes) {
      PreviewImage image;
      const auto error = image.load_png(renderer, bytes);
      if(!error.empty()) throw std::runtime_error(error);
      return image;
    }

    PreviewImage load_png(SDL_Renderer *renderer, const fs::path &root, const std::string &name) {
      return decode_png(renderer, read_png(project_file(root, relative_path(name))));
    }

    unsigned check_grid(const sdk::SpriteClips &descriptor, const PreviewImage &image, unsigned width, unsigned height) {
      if(!width || !height || width > max_dimension || height > max_dimension)
        throw std::runtime_error("Frame width and height must be between 1 and 2048 pixels.");
      if(!image.texture || image.width % width || image.height % height)
        throw std::runtime_error("The PNG must contain a whole number of frames in both dimensions.");
      const unsigned count = (image.width / width) * (image.height / height);
      for(const auto &[name, clip] : descriptor.clips)
        for(const auto &frame : clip.frames)
          if(frame.cell >= count) throw std::runtime_error("Grid does not contain every cell used by clip: " + name);
      return count;
    }

    void write_asset(const fs::path &path, std::string_view bytes, const std::optional<std::string> &original = std::nullopt) {
      auto staging = path;
      staging += ".editor-save";
      if(!fs::create_directory(staging)) throw std::runtime_error("Sprite save staging directory already exists.");
      const auto file = staging / path.filename();
      struct Cleanup {
        const fs::path &file, &directory;
        ~Cleanup() { std::error_code error; fs::remove(file, error); fs::remove(directory, error); }
      } cleanup{file, staging};
      std::ofstream out(file, std::ios::binary);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      out.close();
      if(!out) throw std::runtime_error("Could not write staged sprite asset.");
      if(original) {
        if(!fs::is_regular_file(fs::symlink_status(path)) || read_asset<std::string>(path, max_descriptor_size) != *original)
          throw std::runtime_error("Sprite descriptor changed on disk. Revert to reload it before saving.");
        fs::rename(file, path);
      } else {
        // Publish a complete file without replacing a concurrently created asset.
        if(fs::exists(fs::symlink_status(path))) throw std::runtime_error("Sprite asset already exists: " + path.filename().u8string());
#if defined(_WIN32)
        if(!MoveFileW(file.c_str(), path.c_str()))
          throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Could not publish sprite asset");
#elif defined(__EMSCRIPTEN__)
        // This project's MEMFS is single-threaded and does not support hard links.
        fs::rename(file, path);
#else
        fs::create_hard_link(file, path);
#endif
      }
    }

    bool pick_png(const std::vector<std::string> &pngs, std::string &selected) {
      bool changed = false;
      if(pngs.empty()) ImGui::TextDisabled("No project PNGs found.");
      else if(ImGui::BeginListBox("##pngs", {ImGui::GetFontSize() * 25, 8 * ImGui::GetTextLineHeightWithSpacing()})) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(pngs.size()));
        while(clipper.Step()) for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          ImGui::PushID(i);
          const bool active = pngs[i] == selected;
          if(ImGui::Selectable(pngs[i].c_str(), active) && !active) { selected = pngs[i]; changed = true; }
          ImGui::SetItemTooltip("%s", pngs[i].c_str());
          ImGui::PopID();
        }
        ImGui::EndListBox();
      }
      return changed;
    }

    void cell_uv(const PreviewImage &image, const sdk::SpriteClips &grid, unsigned cell, ImVec2 &uv0, ImVec2 &uv1) {
      const unsigned columns = image.width / grid.frame_width;
      const unsigned x = cell % columns * grid.frame_width, y = cell / columns * grid.frame_height;
      uv0 = {float(x) / image.width, float(y) / image.height};
      uv1 = {float(x + grid.frame_width) / image.width, float(y + grid.frame_height) / image.height};
    }

    uint64_t now_ms() { return static_cast<uint64_t>(ImGui::GetTime() * 1000.0); }
  }

  SpriteAssets::SpriteAssets(SDL_Renderer *renderer, const fs::path &root)
    : renderer_(renderer), root_(root.empty() ? root : fs::weakly_canonical(root)) {}

  void SpriteAssets::open(const std::string &path) {
    try {
      const auto name = descriptor_path(path);
      auto [it, inserted] = tabs_.try_emplace(name);
      if(inserted || !it->second.loaded) load(name, it->second);
      pending_tab_ = name;
      error_.clear();
    } catch(const std::exception &error) { error_ = error.what(); }
  }

  void SpriteAssets::load(const std::string &path, Tab &tab) {
    try {
      Tab loaded;
      loaded.original = read_asset<std::string>(project_file(root_, fs::u8path(path)), max_descriptor_size);
      if(const auto error = sdk::read_sprite_clips(nlohmann::json::parse(*loaded.original), loaded.descriptor))
        throw std::runtime_error(error);
      loaded.width = std::min(loaded.descriptor.frame_width, unsigned(max_dimension));
      loaded.height = std::min(loaded.descriptor.frame_height, unsigned(max_dimension));
      if(!loaded.descriptor.clips.empty()) loaded.clip = loaded.descriptor.clips.begin()->first;
      loaded.loaded = true;
      try {
        loaded.preview = load_png(renderer_, root_, loaded.descriptor.image);
        check_grid(loaded.descriptor, loaded.preview, loaded.descriptor.frame_width, loaded.descriptor.frame_height);
      } catch(const std::exception &error) { loaded.error = error.what(); }
      tab = std::move(loaded);
    } catch(const std::exception &error) { tab.error = error.what(); }
  }

  void SpriteAssets::suggest_draft(std::string image) {
    new_image_ = std::move(image);
    new_error_.clear();
    auto source = fs::u8path(new_image_);
    new_external_ = source.is_absolute();
    if(new_external_) {
      // Projects without an assets folder can still import beside a descriptor at the root.
      source = fs::is_directory(root_ / "assets") ? fs::path("assets") / source.filename() : source.filename();
    }
    auto candidate = source;
    candidate.replace_extension(suffix);
    for(unsigned n = 2; ; ++n) {
      new_path_ = candidate.generic_u8string();
      std::error_code error;
      const auto status = fs::symlink_status(root_ / candidate, error);
      if(!tabs_.count(new_path_) && (error || status.type() == fs::file_type::not_found)) return;
      candidate = source.parent_path() / fs::u8path(source.stem().u8string() + "-" + std::to_string(n) + std::string(suffix));
    }
  }

  void SpriteAssets::create() {
    auto requested = relative_path(new_path_);
    if(!requested.has_extension() && requested.has_filename()) requested += suffix;
    const auto path = descriptor_path(requested.generic_u8string());
    const auto file = project_file(root_, fs::u8path(path));
    if(tabs_.count(path) || fs::exists(fs::symlink_status(file))) throw std::runtime_error("Sprite descriptor already exists.");
    if(!fs::is_directory(file.parent_path())) throw std::runtime_error("The destination folder must exist.");
    Tab tab;
    std::vector<uint8_t> imported;
    fs::path destination;
    if(new_external_) {
      const auto source = fs::u8path(new_image_);
      const auto image = relative_path((requested.parent_path() / source.filename()).generic_u8string());
      destination = project_file(root_, image);
      if(fs::exists(fs::symlink_status(destination)))
        throw std::runtime_error("PNG copy destination already exists. Select the existing project PNG instead.");
      imported = read_png(source);
      tab.preview = decode_png(renderer_, imported);
      tab.descriptor.image = image.generic_u8string();
    } else {
      tab.descriptor.image = new_image_;
      tab.preview = load_png(renderer_, root_, new_image_);
    }
    tab.width = tab.descriptor.frame_width = tab.preview.width % 8 ? tab.preview.width : 8;
    tab.height = tab.descriptor.frame_height = tab.preview.height % 8 ? tab.preview.height : 8;
    tab.loaded = true;
    tab.dirty = !new_external_;
    if(new_external_) {
      tab.original = sdk::write_sprite_clips(tab.descriptor).dump(2) + '\n';
      write_asset(destination, {reinterpret_cast<const char *>(imported.data()), imported.size()});
      try { write_asset(file, *tab.original); }
      catch(...) {
        std::error_code error;
        fs::remove(destination, error);
        throw;
      }
      refresh_pending_ = true;
    }
    tabs_.emplace(path, std::move(tab));
    pending_tab_ = path;
  }

  bool SpriteAssets::active_dirty() const {
    const auto tab = tabs_.find(active_tab_);
    return tab != tabs_.end() && tab->second.dirty;
  }

  bool SpriteAssets::playing() const {
    const auto tab = tabs_.find(active_tab_);
    return tab != tabs_.end() && tab->second.playing;
  }

  bool SpriteAssets::dirty() const {
    return std::any_of(tabs_.begin(), tabs_.end(), [](const auto &item) { return item.second.dirty; });
  }

  bool SpriteAssets::can_history(bool redo) const {
    const auto tab = tabs_.find(active_tab_);
    return tab != tabs_.end() && !(redo ? tab->second.redo : tab->second.undo).empty();
  }

  void SpriteAssets::history(bool redo) {
    if(!can_history(redo)) return;
    auto &tab = tabs_.find(active_tab_)->second;
    auto &from = redo ? tab.redo : tab.undo;
    auto &to = redo ? tab.undo : tab.redo;
    auto &entry = from.back();
    try {
      if(entry.descriptor.image != tab.descriptor.image) {
        auto preview = load_png(renderer_, root_, entry.descriptor.image);
        check_grid(entry.descriptor, preview, entry.descriptor.frame_width, entry.descriptor.frame_height);
        tab.preview = std::move(preview);
      }
      to.push_back({std::move(tab.descriptor), std::move(tab.clip), tab.frame, tab.revision});
      tab.descriptor = std::move(entry.descriptor);
      tab.clip = std::move(entry.clip);
      tab.frame = entry.frame;
      tab.revision = entry.id;
      from.pop_back();
      tab.width = tab.descriptor.frame_width;
      tab.height = tab.descriptor.frame_height;
      const auto cells = (tab.preview.width / tab.width) * (tab.preview.height / tab.height);
      tab.cell = cells > 0 ? std::min(tab.cell, unsigned(cells - 1)) : 0;
      tab.playing = tab.editing_duration = false;
      changed(tab);
      tab.dirty = !tab.original || tab.revision != tab.saved_revision;
    } catch(const std::exception &error) { tab.error = error.what(); }
  }

  void SpriteAssets::save(bool all) {
    for(auto &[path, tab] : tabs_) {
      if(!tab.dirty || (!all && path != active_tab_)) continue;
      try {
        auto preview = load_png(renderer_, root_, tab.descriptor.image);
        check_grid(tab.descriptor, preview, tab.descriptor.frame_width, tab.descriptor.frame_height);
        auto text = sdk::write_sprite_clips(tab.descriptor).dump(2) + '\n';
        if(text.size() > max_descriptor_size) throw std::runtime_error("Sprite descriptor exceeds 16 MiB.");
        write_asset(project_file(root_, fs::u8path(path)), text, tab.original);
        tab.original = std::move(text);
        tab.preview = std::move(preview);
        tab.saved_revision = tab.revision;
        tab.editing_duration = false;
        tab.dirty = false;
        tab.error.clear();
        refresh_pending_ = true;
      } catch(const std::exception &error) { tab.error = error.what(); }
    }
  }

  void SpriteAssets::revert(const std::string &path) {
    const auto tab = tabs_.find(path);
    if(tab == tabs_.end()) return;
    if(tab->second.original || !tab->second.loaded) load(path, tab->second);
    else {
      if(pending_tab_ == path) pending_tab_.clear();
      if(active_tab_ == path) active_tab_.clear();
      tabs_.erase(tab);
    }
  }

  void SpriteAssets::record_edit(Tab &tab) {
    if(tab.undo.size() == 20) tab.undo.erase(tab.undo.begin());
    tab.undo.push_back({tab.descriptor, tab.clip, tab.frame, tab.revision});
    tab.redo.clear();
    tab.revision = ++tab.next_revision;
  }

  void SpriteAssets::changed(Tab &tab) {
    tab.dirty = true;
    tab.error.clear();
    tab.elapsed = 0;
    tab.started = now_ms();
  }

  void SpriteAssets::draw(const std::function<std::vector<std::string>()> &list_pngs,
                          const std::function<void()> &refresh_files) {
    if(root_.empty()) { ImGui::TextDisabled("Open a project to author sprite clips."); return; }
    if(!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
    if(ImGui::BeginTabBar("Sprite assets")) {
      if(ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) {
        pngs_ = list_pngs();
        new_image_.clear();
        new_external_ = false;
        new_path_.clear();
        new_error_.clear();
        if(pngs_.size() == 1) suggest_draft(pngs_.front());
        ImGui::OpenPopup("New sprite descriptor");
      }
      if(ImGui::BeginPopup("New sprite descriptor")) {
        ImGui::TextUnformatted("Choose the source PNG, then name the new sprite file.");
        if(pick_png(pngs_, new_image_)) suggest_draft(new_image_);
#ifdef _WIN32
        if(ImGui::Button("Browse for PNG...")) {
          try {
            if(auto image = browse_png(root_); !image.empty()) suggest_draft(std::move(image));
          } catch(const std::exception &error) { new_error_ = error.what(); }
        }
        if(new_external_) ImGui::TextWrapped("Copy %s beside the new sprite file.", new_image_.c_str() + (new_image_.find_last_of('/') + 1));
#endif
        const bool enter = ImGui::InputTextWithHint("Sprite file", "Project-relative name", &new_path_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetItemTooltip("Project-relative destination; .sprite.json is added to a name with no extension.");
        ImGui::BeginDisabled(new_image_.empty() || new_path_.empty());
        const bool add = ImGui::Button(new_external_ ? "Import" : "Create draft") || enter;
        ImGui::EndDisabled();
        if(add && !new_image_.empty() && !new_path_.empty()) {
          try { create(); ImGui::CloseCurrentPopup(); }
          catch(const std::exception &error) { new_error_ = error.what(); }
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        if(!new_error_.empty()) ImGui::TextWrapped("%s", new_error_.c_str());
        ImGui::EndPopup();
      }
      const auto requested = std::exchange(pending_tab_, {});
      for(auto it = tabs_.begin(); it != tabs_.end();) {
        auto &[path, tab] = *it;
        bool keep = true;
        ImGuiTabItemFlags flags = path == requested ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if(tab.dirty) flags |= ImGuiTabItemFlags_UnsavedDocument;
        ImGui::PushID(path.c_str());
        const auto label = fs::u8path(path).filename().u8string() + "###" + path;
        if(ImGui::BeginTabItem(label.c_str(), tab.dirty ? nullptr : &keep, flags)) {
          active_tab_ = path;
          ImGui::SetItemTooltip("%s", path.c_str());
          if(tab.loaded) draw_tab(tab, list_pngs);
          else {
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
    if(tabs_.empty()) ImGui::TextDisabled("Open a .sprite.json descriptor from Project Files or use + to create one.");
    if(refresh_pending_) { refresh_pending_ = false; refresh_files(); }
  }

  void SpriteAssets::draw_tab(Tab &tab, const std::function<std::vector<std::string>()> &list_pngs) {
    if(ImGui::SmallButton("Sheet settings")) {
      pngs_ = list_pngs();
      ImGui::OpenPopup("Sprite sheet settings");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%u x %u px frames", tab.descriptor.frame_width, tab.descriptor.frame_height);
    ImGui::SetItemTooltip("%s", tab.descriptor.image.c_str());
    if(ImGui::BeginPopup("Sprite sheet settings")) {
      auto selected = tab.descriptor.image;
      if(pick_png(pngs_, selected)) {
        try {
          auto preview = load_png(renderer_, root_, selected);
          const auto count = check_grid(tab.descriptor, preview, tab.width, tab.height);
          const bool modified = tab.descriptor.image != selected || tab.descriptor.frame_width != unsigned(tab.width) ||
                                tab.descriptor.frame_height != unsigned(tab.height);
          if(modified) record_edit(tab);
          tab.descriptor.image = std::move(selected);
          tab.descriptor.frame_width = tab.width;
          tab.descriptor.frame_height = tab.height;
          tab.preview = std::move(preview);
          tab.cell = std::min(tab.cell, count - 1);
          if(modified) changed(tab);
          else tab.error.clear();
        } catch(const std::exception &error) { tab.error = error.what(); }
        if(tab.error.empty()) ImGui::CloseCurrentPopup();
      }
      ImGui::Separator();
      int dimensions[] = {tab.width, tab.height};
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
      if(ImGui::InputInt2("Frame size (px)", dimensions)) {
        tab.width = std::clamp(dimensions[0], 1, max_dimension);
        tab.height = std::clamp(dimensions[1], 1, max_dimension);
      }
      ImGui::SameLine();
      if(ImGui::Button("Apply grid")) {
        try {
          const auto count = check_grid(tab.descriptor, tab.preview, tab.width, tab.height);
          if(tab.descriptor.frame_width != unsigned(tab.width) || tab.descriptor.frame_height != unsigned(tab.height)) {
            record_edit(tab);
            tab.descriptor.frame_width = tab.width;
            tab.descriptor.frame_height = tab.height;
            tab.cell = std::min(tab.cell, count - 1);
            changed(tab);
          } else tab.error.clear();
          ImGui::CloseCurrentPopup();
        } catch(const std::exception &error) { tab.error = error.what(); }
      }
      if(!tab.error.empty()) ImGui::TextWrapped("%s", tab.error.c_str());
      ImGui::EndPopup();
    }
    if(!tab.error.empty()) ImGui::TextWrapped("%s", tab.error.c_str());
    const float width = ImGui::GetContentRegionAvail().x;
    if(ImGui::BeginChild("Atlas", {width * .48f, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
      draw_atlas(tab);
    ImGui::EndChild();
    ImGui::SameLine();
    if(ImGui::BeginChild("Clips", {0, 0}, ImGuiChildFlags_Borders)) draw_clip(tab);
    ImGui::EndChild();
  }

  void SpriteAssets::draw_atlas(Tab &tab) {
    ImGui::SeparatorText("Atlas cells");
    const auto &grid = tab.descriptor;
    if(!tab.preview.texture || tab.preview.width % grid.frame_width || tab.preview.height % grid.frame_height) {
      ImGui::TextWrapped("Choose a PNG and apply a frame size that divides its dimensions.");
      return;
    }
    const auto columns = tab.preview.width / grid.frame_width;
    const auto rows = tab.preview.height / grid.frame_height;
    int cell = static_cast<int>(tab.cell);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if(ImGui::InputInt("Cell (zero-based)", &cell)) tab.cell = std::clamp(cell, 0, int(rows * columns - 1));
    const float scale = 2.0f * ImGui::GetFontSize() / std::max(grid.frame_width, grid.frame_height);
    const ImVec2 size{std::max(1.f, grid.frame_width * scale), std::max(1.f, grid.frame_height * scale)};
    const auto texture = reinterpret_cast<ImTextureID>(tab.preview.texture.get());
    ImGuiListClipper clipper;
    clipper.Begin(rows, size.y + 2 * ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().ItemSpacing.y);
    while(clipper.Step()) for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      for(unsigned column = 0; column < columns; ++column) {
        if(column) ImGui::SameLine();
        const unsigned index = row * columns + column;
        ImVec2 uv0, uv1;
        cell_uv(tab.preview, grid, index, uv0, uv1);
        const bool selected = tab.cell == index;
        if(selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushID(static_cast<int>(index));
        if(ImGui::ImageButton("Cell", texture, size, uv0, uv1)) tab.cell = index;
        ImGui::SetItemTooltip("Cell %u", index);
        ImGui::PopID();
        if(selected) ImGui::PopStyleColor();
      }
    }
  }

  void SpriteAssets::draw_clip(Tab &tab) {
    ImGui::SeparatorText("Named clips");
    auto &grid = tab.descriptor;
    if(ImGui::BeginCombo("Clip", tab.clip.empty() ? "Select a clip" : tab.clip.c_str())) {
      for(const auto &[name, clip] : grid.clips) {
        ImGui::PushID(&clip);
        if(ImGui::Selectable(name.c_str(), name == tab.clip)) {
          tab.clip = name;
          tab.frame = tab.elapsed = 0;
          tab.playing = false;
        }
        if(name == tab.clip) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    const auto count = tab.preview.texture && !(tab.preview.width % grid.frame_width) && !(tab.preview.height % grid.frame_height)
      ? (tab.preview.width / grid.frame_width) * (tab.preview.height / grid.frame_height) : 0;
    const bool enter = ImGui::InputTextWithHint("Name", "e.g. walk", tab.clip_name.data(), tab.clip_name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string_view proposed = tab.clip_name.data();
    const bool fresh_name = !proposed.empty() && proposed.find_first_not_of(" \t\r\n") != proposed.npos && !grid.clips.count(proposed);
    ImGui::BeginDisabled(!fresh_name || !count);
    const bool add = ImGui::Button("Add clip") || enter;
    ImGui::EndDisabled();
    if(add && fresh_name && count) {
      record_edit(tab);
      tab.clip = proposed;
      auto &clip = grid.clips[tab.clip];
      clip.frames.push_back({tab.cell, 100});
      tab.clip_name = {};
      tab.frame = 0;
      tab.playing = false;
      changed(tab);
    }
    auto selected = grid.clips.find(tab.clip);
    if(selected == grid.clips.end()) return;
    ImGui::SameLine();
    ImGui::BeginDisabled(!fresh_name || add);
    const bool rename = ImGui::Button("Rename");
    ImGui::EndDisabled();
    if(rename) {
      record_edit(tab);
      auto clip = grid.clips.extract(selected);
      tab.clip = proposed;
      clip.key() = tab.clip;
      selected = grid.clips.insert(std::move(clip)).position;
      tab.clip_name = {};
      changed(tab);
    }
    ImGui::SameLine();
    if(ImGui::Button("Delete clip")) ImGui::OpenPopup("Delete selected clip?");
    if(ImGui::BeginPopup("Delete selected clip?")) {
      ImGui::TextUnformatted(selected->first.c_str());
      if(ImGui::Button("Delete")) {
        record_edit(tab);
        grid.clips.erase(selected);
        tab.clip = grid.clips.empty() ? std::string{} : grid.clips.begin()->first;
        tab.frame = 0;
        tab.playing = false;
        changed(tab);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
      }
      ImGui::SameLine();
      if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    auto &clip = selected->second;
    bool loop = clip.loop;
    if(ImGui::Checkbox("Loop", &loop)) { record_edit(tab); clip.loop = loop; changed(tab); }
    uint64_t total = 0;
    if(!clip.loop) for(const auto &frame : clip.frames) total += frame.duration_ms;
    const auto now = now_ms();
    auto elapsed = tab.elapsed + (tab.playing ? now - tab.started : 0);
    if(!clip.loop && elapsed >= total) { tab.playing = false; tab.elapsed = elapsed = total; }
    if(ImGui::Button(tab.playing ? "Pause###playback" : "Play###playback")) {
      if(tab.playing) tab.elapsed = elapsed;
      else {
        if(!clip.loop && tab.elapsed >= total) tab.elapsed = elapsed = 0;
        tab.started = now;
      }
      tab.playing = !tab.playing;
    }
    ImGui::SameLine();
    if(ImGui::Button("Restart")) { tab.elapsed = elapsed = 0; tab.started = now; }
    const unsigned shown = sdk::sprite_cell_at(clip, elapsed);
    if(shown < count) {
      ImVec2 uv0, uv1;
      cell_uv(tab.preview, grid, shown, uv0, uv1);
      const float scale = std::min(7.f * ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x) /
                          std::max(grid.frame_width, grid.frame_height);
      if(scale > 0) ImGui::Image(reinterpret_cast<ImTextureID>(tab.preview.texture.get()),
                                 {grid.frame_width * scale, grid.frame_height * scale}, uv0, uv1);
    } else ImGui::TextDisabled("Preview cell is outside the atlas.");

    ImGui::SeparatorText("Frames");
    ImGui::BeginDisabled(!count || clip.frames.size() >= max_frames);
    if(ImGui::Button("Append picked cell")) {
      record_edit(tab);
      tab.frame = clip.frames.size();
      clip.frames.push_back({tab.cell, 100});
      changed(tab);
    }
    ImGui::EndDisabled();
    if(clip.frames.size() >= max_frames) ImGui::TextDisabled("Maximum 1024 frames per clip.");
    if(ImGui::BeginListBox("##frames", {-1, 6 * ImGui::GetTextLineHeightWithSpacing()})) {
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(clip.frames.size()));
      while(clipper.Step()) for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const auto &frame = clip.frames[i];
        char label[80];
        SDL_snprintf(label, sizeof(label), "%d. Cell %u - %u ms###frame%d", i + 1, frame.cell, frame.duration_ms, i);
        if(ImGui::Selectable(label, tab.frame == static_cast<size_t>(i))) tab.frame = i;
      }
      ImGui::EndListBox();
    }
    tab.frame = std::min(tab.frame, clip.frames.size() - 1);
    auto &frame = clip.frames[tab.frame];
    int duration = frame.duration_ms;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::PushID(static_cast<int>(tab.frame));
    if(ImGui::InputInt("Duration (ms)", &duration, 0)) {
      const auto ms = static_cast<unsigned>(std::clamp(duration, 1, 60000));
      if(ms != frame.duration_ms) {
        if(!tab.editing_duration) record_edit(tab);
        tab.editing_duration = true;
        frame.duration_ms = ms;
        changed(tab);
      }
    }
    if(!ImGui::IsItemActive()) tab.editing_duration = false;
    ImGui::PopID();
    ImGui::SameLine();
    const bool mixed = std::any_of(clip.frames.begin(), clip.frames.end(), [&](const auto &entry) {
      return entry.duration_ms != frame.duration_ms;
    });
    ImGui::BeginDisabled(!mixed);
    if(ImGui::Button("Set all")) {
      record_edit(tab);
      const auto ms = frame.duration_ms;
      for(auto &entry : clip.frames) entry.duration_ms = ms;
      changed(tab);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Apply the selected duration to every frame in this clip.");
    ImGui::BeginDisabled(!count || tab.cell == frame.cell);
    if(ImGui::Button("Use picked cell")) { record_edit(tab); frame.cell = tab.cell; changed(tab); }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(tab.frame == 0);
    if(ImGui::Button("Move up")) { record_edit(tab); std::swap(clip.frames[tab.frame], clip.frames[tab.frame - 1]); --tab.frame; changed(tab); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(tab.frame + 1 == clip.frames.size());
    if(ImGui::Button("Move down")) { record_edit(tab); std::swap(clip.frames[tab.frame], clip.frames[tab.frame + 1]); ++tab.frame; changed(tab); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(clip.frames.size() <= 1);
    if(ImGui::Button("Remove frame")) {
      record_edit(tab);
      clip.frames.erase(clip.frames.begin() + tab.frame);
      tab.frame = std::min(tab.frame, clip.frames.size() - 1);
      changed(tab);
    }
    ImGui::EndDisabled();
  }
}
