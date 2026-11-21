#include "documents.hpp"
#include "tabs.hpp"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace editor {
  namespace fs = std::filesystem;
  namespace {
    constexpr int text_limit = 256 * 1024;
    constexpr int png_limit = 16 * 1024 * 1024;
    constexpr std::string_view utf8_bom = "\xef\xbb\xbf";

    fs::path project_file(const fs::path &root, const fs::path &relative) {
      if(root.empty() || relative.has_root_path()) return {};
      std::error_code error;
      const auto path = fs::canonical(root / relative, error);
      if(error) return {};
      const auto local = path.lexically_relative(root);
      if(local.empty() || *local.begin() == ".." || !fs::is_regular_file(path, error)) return {};
      return path;
    }

    bool is_utf8_text(std::string_view text) {
      for(size_t i = 0; i < text.size();) {
        const auto ch = static_cast<unsigned char>(text[i++]);
        if(ch == 0) return false;
        if(ch < 0x80) continue;
        if(ch < 0xc2 || ch > 0xf4) return false;
        const size_t tail = ch < 0xe0 ? 1 : ch < 0xf0 ? 2 : 3;
        if(tail > text.size() - i) return false;
        const auto next = static_cast<unsigned char>(text[i]);
        if((ch == 0xe0 && next < 0xa0) || (ch == 0xed && next >= 0xa0) ||
           (ch == 0xf0 && next < 0x90) || (ch == 0xf4 && next >= 0x90)) return false;
        for(size_t n = 0; n < tail; ++n)
          if((static_cast<unsigned char>(text[i++]) & 0xc0) != 0x80) return false;
      }
      return true;
    }

    size_t newline_size(std::string_view text, size_t pos) {
      return text[pos] == '\r' && pos + 1 < text.size() && text[pos + 1] == '\n' ? 2 : 1;
    }

    bool same_text(std::string_view original, std::string_view content) {
      // The editor uses LF internally; compare without allocating a second normalized snapshot.
      for(auto pos = original.find('\r'); pos != original.npos; pos = original.find('\r')) {
        if(pos >= content.size() || content[pos] != '\n' || content.substr(0, pos) != original.substr(0, pos)) return false;
        content.remove_prefix(pos + 1);
        original.remove_prefix(pos + newline_size(original, pos));
      }
      return original == content;
    }

    bool matches_disk(const fs::path &path, std::string_view original) {
      std::ifstream in(path, std::ios::binary);
      if(!in) return false;
      std::array<char, 4096> bytes;
      while(!original.empty()) {
        const auto n = std::min(original.size(), bytes.size());
        if(!in.read(bytes.data(), static_cast<std::streamsize>(n)) || std::memcmp(bytes.data(), original.data(), n) != 0)
          return false;
        original.remove_prefix(n);
      }
      return in.peek() == std::char_traits<char>::eof() && in.eof() && !in.bad();
    }

    bool is_tileset(std::string_view name) {
      constexpr std::string_view suffix = ".tileset.json";
      return name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
    }
    enum class FileKind { Hidden, Text, Asset, Png, Viewport };

    FileKind file_kind(const fs::path &path) {
      if(path.extension() == ".scene" || path.extension() == ".map") return FileKind::Viewport;
      auto ext = path.extension().u8string();
      for(auto &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if(ext == ".png") return FileKind::Png;
      constexpr std::string_view text[] = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".cmake",
        ".scene", ".json", ".yaml", ".yml", ".toml",
        ".ini", ".cfg", ".csv", ".lua", ".py", ".glsl", ".vert", ".frag", ".txt", ".md"
      };
      constexpr std::string_view assets[] = {
        ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".svg", ".ase", ".aseprite",
        ".wav", ".ogg", ".flac", ".mp3", ".mod", ".xm", ".ttf", ".otf", ".glb", ".gltf"
      };
      for(const auto type : text) if(ext == type) return FileKind::Text;
      for(const auto type : assets) if(ext == type) return FileKind::Asset;
      return FileKind::Hidden;
    }

    bool skip_directory(std::string_view name) {
      return name == "build" || name.substr(0, 6) == "build-" || name.substr(0, 12) == "cmake-build-" ||
             name == "CMakeFiles" || name == "_deps" || name == "__pycache__";
    }
  }

  Documents::Documents(const fs::path &root, SDL_Renderer *renderer) : root_(root), renderer_(renderer) {
    if(root_.empty()) return;
    open("CMakeLists.txt");
    refresh_files();
  }

  void Documents::Tab::index_lines() {
    line_starts.clear();
    line_starts.push_back(0);
    for(auto next = content.find('\n'); next != content.npos; next = content.find('\n', next + 1))
      line_starts.push_back(static_cast<int>(next + 1));
  }

  void Documents::Tab::update_cursor(int offset) {
    cursor = std::clamp(offset, 0, static_cast<int>(content.size()));
    const auto start = std::upper_bound(line_starts.begin(), line_starts.end(), cursor);
    line = static_cast<int>(start - line_starts.begin());
    column = ImTextCountCharsFromUtf8(content.data() + start[-1], content.data() + cursor) + 1;
  }

  bool Documents::dirty() const {
    return std::any_of(tabs_.begin(), tabs_.end(), [](const Tab &tab) { return tab.modified; });
  }

  bool Documents::active_dirty() const {
    return std::any_of(tabs_.begin(), tabs_.end(), [&](const Tab &tab) { return tab.modified && tab.path == active_; });
  }

  void Documents::save(bool all) {
    for(auto &tab : tabs_) if(tab.modified && (all || tab.path == active_)) save(tab);
  }

  bool Documents::save(Tab &tab) {
    if(!tab.modified) return true;
    try {
      const auto relative = fs::u8path(tab.path);
      const auto path = root_ / relative;
      const auto check_disk = [&] {
        if(project_file(root_, relative) != path || !matches_disk(path, tab.original))
          throw std::runtime_error("File changed on disk or is no longer readable. Keep a copy of your edits before discarding and reopening it.");
      };
      check_disk();

      const auto bom = tab.bom ? utf8_bom : std::string_view{};
      const auto size = bom.size() + tab.content.size() + (tab.line_starts.size() - 1) * (tab.line_ending.size() - 1);
      if(size > text_limit) throw std::runtime_error("Cannot save text larger than 256 KiB.");
      std::string output;
      output.reserve(size);
      output.append(bom);
      if(tab.line_ending == "\n") output += tab.content;
      else {
        size_t start = 0;
        for(size_t i = 1; i < tab.line_starts.size(); ++i) {
          const auto next = static_cast<size_t>(tab.line_starts[i]);
          output.append(tab.content, start, next - start - 1);
          output.append(tab.line_ending);
          start = next;
        }
        output.append(tab.content, start, tab.content.npos);
      }

      auto directory = path;
      directory += ".editor-save";
      const auto staged = directory / path.filename();
      if(!fs::create_directory(directory)) throw std::runtime_error("Text save staging directory already exists.");
      struct Cleanup {
        const fs::path &file, &directory;
        ~Cleanup() { std::error_code error; fs::remove(file, error); fs::remove(directory, error); }
      } cleanup{staged, directory};
      std::ofstream out(staged, std::ios::binary);
      out.write(output.data(), static_cast<std::streamsize>(output.size()));
      out.close();
      if(!out) throw std::runtime_error("Could not write staged text.");
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
      fs::permissions(staged, fs::status(path).permissions() & fs::perms::all);
#endif
      check_disk();
      fs::rename(staged, path);
      tab.original = std::move(output);
      tab.modified = false;
      tab.save_error.clear();
      return true;
    } catch(const std::exception &error) {
      tab.save_error = error.what();
      return false;
    }
  }

  bool Documents::open(const std::string &name) {
    if(name.find('\0') != name.npos || name.find(':') != name.npos) {
      error_ = "Enter a project-relative file path.";
      return false;
    }
    const auto path = fs::u8path(name).lexically_normal();
    const auto kind = file_kind(path);
    if(kind == FileKind::Hidden || kind == FileKind::Asset) { error_ = "Choose an openable project file."; return false; }
    const auto target = project_file(root_, path);
    std::ifstream in(target, std::ios::binary | std::ios::ate);
    if(!in) { error_ = "Cannot open a file outside the project or a file that does not exist."; return false; }
    const auto key = path.generic_u8string();
    if(kind == FileKind::Viewport || is_tileset(key) ||
       (path.extension() == ".json" && path.stem().extension() == ".sprite")) {
      asset_request_ = key;
      error_.clear();
      return true;
    }
    if(kind == FileKind::Text && target != root_ / path) {
      error_ = "Open the original project file to edit it, not a symbolic link or alias.";
      return false;
    }
    error_.clear();
    for(const auto &tab : tabs_) if(tab.path == key) { selected_ = key; return true; }
    const auto size = in.tellg();
    Tab tab;
    tab.path = key;
    tab.label = path.filename().u8string() + "###" + key;
    if(kind == FileKind::Png) {
      if(size <= 0 || size > png_limit) { error_ = "PNG is empty, unreadable, or exceeds 16 MiB."; return false; }
      in.seekg(0);
      std::vector<uint8_t> bytes(static_cast<size_t>(size));
      if(!in.read(reinterpret_cast<char *>(bytes.data()), size)) { error_ = "Cannot read PNG."; return false; }
      error_ = tab.image.load_png(renderer_, bytes);
      if(!error_.empty()) return false;
    } else {
      if(size < 0 || size > text_limit) { error_ = "File exceeds 256 KiB or is not readable."; return false; }
      tab.original.resize(static_cast<size_t>(size));
      if(!in.seekg(0).read(tab.original.data(), static_cast<std::streamsize>(size)) || !is_utf8_text(tab.original)) {
        error_ = "File is not readable UTF-8 text.";
        return false;
      }
      std::string_view text(tab.original);
      tab.bom = text.substr(0, utf8_bom.size()) == utf8_bom;
      if(tab.bom) text.remove_prefix(utf8_bom.size());
      if(const auto pos = text.find_first_of("\r\n"); pos != text.npos && text[pos] == '\r')
        tab.line_ending = newline_size(text, pos) == 2 ? "\r\n" : "\r";
      tab.content.reserve(text.size());
      for(auto pos = text.find('\r'); pos != text.npos; pos = text.find('\r')) {
        tab.content.append(text.substr(0, pos));
        tab.content += '\n';
        text.remove_prefix(pos + newline_size(text, pos));
      }
      tab.content.append(text);
      tab.index_lines();
    }
    tabs_.push_back(std::move(tab));
    selected_ = key;
    return true;
  }

  void Documents::draw_text(Tab &tab) {
    ImGui::TextDisabled("%s", tab.path.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!tab.modified);
    if(ImGui::SmallButton("Save")) save(tab);
    ImGui::EndDisabled();
    if(!tab.save_error.empty()) ImGui::TextWrapped("%s", tab.save_error.c_str());
    ImGui::Separator();

    const auto &style = ImGui::GetStyle();
    const auto padding = style.FramePadding;
    const float row_height = ImGui::GetTextLineHeight();
    const auto available = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(1.0f, available.x),
                      std::max(row_height + 2 * padding.y, available.y - ImGui::GetTextLineHeightWithSpacing()));
    const float gutter_width = std::min(ImGui::CalcTextSize("888888").x + 2 * padding.x, size.x * .4f);
    const auto origin = ImGui::GetCursorScreenPos();
    auto *parent = ImGui::GetCurrentWindow();
    const int children = parent->DC.ChildWindows.Size;
    auto *background = parent->DrawList;
    background->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, ImGui::GetColorU32(ImGuiCol_WindowBg), style.FrameRounding);

    constexpr auto label = "##source";
    const auto id = ImGui::GetID(label);
    // The widget owns editing, selection and scrolling. Leave its background transparent
    // so the highlighted row can be drawn under its text, using the final scroll position.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding.x + gutter_width, padding.y));
    const bool edited = ImGui::InputTextMultiline(label, &tab.content, size, ImGuiInputTextFlags_AllowTabInput);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if(edited) {
      tab.index_lines();
      tab.modified = !same_text(std::string_view(tab.original).substr(tab.bom ? utf8_bom.size() : 0), tab.content);
      if(!tab.modified) tab.save_error.clear();
    }
    const auto *state = ImGui::GetInputTextState(id);
    const int cursor = state && (edited || ImGui::IsItemActive()) ? state->GetCursorPos() : tab.cursor;
    if(edited || cursor != tab.cursor) tab.update_cursor(cursor);

    if(parent->DC.ChildWindows.Size > children) {
      // In the pinned ImGui version InputTextMultiline submits one scrolling child window.
      auto *child = parent->DC.ChildWindows[children];
      const auto &clip = child->InnerClipRect;
      if(child->ChildId == id && !child->SkipItems && clip.GetWidth() > 0 && clip.GetHeight() > 0) {
        const float top = child->Pos.y + padding.y - child->Scroll.y;
        const float active_y = top + (tab.line - 1) * row_height;
        const bool active_visible = active_y + row_height > clip.Min.y && active_y < clip.Max.y;
        const auto highlight = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        if(active_visible) {
          background->PushClipRect(clip.Min, clip.Max, true);
          background->AddRectFilled({clip.Min.x, active_y}, {clip.Max.x, active_y + row_height}, highlight);
          background->PopClipRect();
        }

        // Paint the gutter over the child, masking text that scrolls horizontally into it.
        auto *gutter = child->DrawList;
        const float left = child->Pos.x, right = std::min(left + gutter_width, child->InnerRect.Max.x);
        gutter->PushClipRect(clip.Min, clip.Max, true);
        gutter->AddRectFilled({left, child->Pos.y}, {right, child->Pos.y + child->Size.y}, ImGui::GetColorU32(ImGuiCol_FrameBg));
        if(active_visible) gutter->AddRectFilled({left, active_y}, {right, active_y + row_height}, highlight);
        gutter->AddLine({right, clip.Min.y}, {right, clip.Max.y}, ImGui::GetColorU32(ImGuiCol_Separator));
        const int first = std::max(0, static_cast<int>(std::floor((clip.Min.y - top) / row_height)));
        const int last = std::min(static_cast<int>(tab.line_starts.size()), static_cast<int>(std::ceil((clip.Max.y - top) / row_height)));
        const auto normal = ImGui::GetColorU32(ImGuiCol_TextDisabled), active = ImGui::GetColorU32(ImGuiCol_Text);
        for(int row = first; row < last; ++row) {
          char number[16];
          const int len = ImFormatString(number, sizeof(number), "%d", row + 1);
          const float x = right - padding.x - ImGui::CalcTextSize(number, number + len).x;
          gutter->AddText({x, top + row * row_height}, row + 1 == tab.line ? active : normal, number, number + len);
        }
        gutter->PopClipRect();
      }
    }
    ImGui::TextDisabled("Ln %d, Col %d%s", tab.line, tab.column, tab.modified ? "  |  Unsaved" : "");
  }

  void Documents::draw_close_prompt() {
    if(pending_close_.empty()) return;
    const auto tab = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab &entry) { return entry.path == pending_close_; });
    constexpr auto title = "Close unsaved document?";
    if(!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    if(ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("Save changes to %s before closing?", tab->path.c_str());
      if(ImGui::Button("Save and Close") && save(*tab)) tab->open = false;
      ImGui::SameLine();
      if(ImGui::Button("Discard and Close")) tab->open = false;
      ImGui::SameLine();
      if(ImGui::Button("Cancel") || !tab->open) {
        pending_close_.clear();
        ImGui::CloseCurrentPopup();
      } else if(!tab->save_error.empty()) ImGui::TextWrapped("%s", tab->save_error.c_str());
      ImGui::EndPopup();
    }
  }

  void Documents::draw() {
    if(root_.empty()) { ImGui::TextDisabled("Open the editor with --project to browse scene and source files."); return; }
    if(tabs_.empty()) ImGui::TextDisabled("Open a file from Project Files or the File menu.");
    const auto failed = std::find_if(tabs_.begin(), tabs_.end(), [&](const Tab &tab) { return tab.path != active_ && !tab.save_error.empty(); });
    if(failed != tabs_.end()) ImGui::TextWrapped("Could not save %s: %s", failed->path.c_str(), failed->save_error.c_str());
    if(ImGui::BeginTabBar("documents", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
      for(auto &tab : tabs_) {
        ImGuiTabItemFlags flags = tab.path == selected_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if(tab.modified) flags |= ImGuiTabItemFlags_UnsavedDocument;
        const bool show = begin_tab(tab.label.c_str(), &tab.open, flags);
        if(!tab.open && tab.modified) {
          pending_close_ = tab.path;
          tab.open = true;
        }
        if(show) {
          if(tab.open) {
            if(active_ != tab.path) active_ = tab.path;
            if(tab.image.texture) {
              ImGui::TextDisabled("%s  |  Read-only", tab.path.c_str());
              ImGui::Separator();
              ImGui::TextDisabled("%d x %d px", tab.image.width, tab.image.height);
              const float scale = std::min(1.0f, ImGui::GetContentRegionAvail().x / tab.image.width);
              if(scale > 0) ImGui::Image(reinterpret_cast<ImTextureID>(tab.image.texture.get()),
                                        {tab.image.width * scale, tab.image.height * scale});
            } else draw_text(tab);
          }
          ImGui::EndTabItem();
        }
      }
      selected_.clear();
      ImGui::EndTabBar();
    }
    draw_close_prompt();
    tabs_.erase(std::remove_if(tabs_.begin(), tabs_.end(), [&](const Tab &tab) {
      if(tab.open) return false;
      if(tab.path == active_) active_.clear();
      return true;
    }), tabs_.end());
    if(open_requested_) {
      open_requested_ = false;
      ImGui::OpenPopup("Open document");
    }
    if(ImGui::BeginPopup("Open document")) {
      if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      const bool enter = ImGui::InputTextWithHint("##open path", "Project-relative path", &path_, ImGuiInputTextFlags_EnterReturnsTrue);
      if((ImGui::Button("Open") || enter) && open(path_)) ImGui::CloseCurrentPopup();
      if(!error_.empty()) ImGui::TextWrapped("%s", error_.c_str());
      ImGui::EndPopup();
    }
  }

  void Documents::refresh_files() {
    files_.clear();
    browser_error_.clear();
    if(root_.empty()) return;
    std::error_code error;
    for(fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, error), end;
        !error && it != end; it.increment(error)) {
      const auto &entry = *it;
      const auto name = entry.path().filename().u8string();
      if(name.front() == '.' || skip_directory(name)) {
        it.disable_recursion_pending();
        continue;
      }
      std::error_code type_error;
      if(!fs::is_regular_file(entry.symlink_status(type_error))) continue;
      const auto kind = file_kind(entry.path());
      if(kind != FileKind::Hidden)
        files_.push_back({entry.path().lexically_relative(root_).generic_u8string(), kind != FileKind::Asset});
    }
    std::sort(files_.begin(), files_.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    if(error) browser_error_ = "Could not finish scanning project files: " + error.message();
  }

  bool Documents::draw_files() {
    if(root_.empty()) { ImGui::TextDisabled("Open the editor with --project to browse files."); return false; }
    if(ImGui::Button("Refresh files")) refresh_files();
    if(files_.empty()) ImGui::TextDisabled("No project files found.");
    if(!browser_error_.empty()) ImGui::TextWrapped("%s", browser_error_.c_str());
    const auto padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding.x * .25f, padding.y));
    const bool opened = draw_file_tree(0, files_.size(), 0);
    ImGui::PopStyleVar();
    return opened;
  }

  bool Documents::draw_file_tree(std::size_t first, std::size_t last, std::size_t offset) {
    bool opened = false;
    // Sorted paths sharing a folder prefix form a contiguous range.
    for(auto i = first; i < last;) {
      const auto &path = files_[i].path;
      const auto slash = path.find('/', offset);
      if(slash == path.npos) { ++i; continue; }
      auto end = i + 1;
      while(end < last && files_[end].path.compare(0, slash + 1, path, 0, slash + 1) == 0) ++end;
      ImGui::PushID(path.data() + offset, path.data() + slash);
      if(ImGui::TreeNodeEx("folder", ImGuiTreeNodeFlags_SpanAvailWidth, "%.*s",
                          static_cast<int>(slash - offset), path.data() + offset)) {
        opened |= draw_file_tree(i, end, slash + 1);
        ImGui::TreePop();
      }
      ImGui::PopID();
      i = end;
    }
    const float label_offset = ImGui::GetTreeNodeToLabelSpacing();
    ImGui::Indent(label_offset);
    for(auto i = first; i < last; ++i) {
      const auto &file = files_[i];
      if(file.path.find('/', offset) != file.path.npos) continue;
      const char *name = file.path.c_str() + offset;
      if(!file.previewable) { ImGui::TextDisabled("%s", name); continue; }
      if(ImGui::Selectable(name)) {
        opened = open(file.path);
        browser_error_.swap(error_);
        error_.clear();
      }
    }
    ImGui::Unindent(label_offset);
    return opened;
  }

  std::string Documents::take_asset_request() {
    return std::exchange(asset_request_, {});
  }

  std::vector<std::string> Documents::tilesets() const {
    std::vector<std::string> paths;
    for(const auto &file : files_) if(is_tileset(file.path)) paths.push_back(file.path);
    return paths;
  }

  std::vector<std::string> Documents::pngs() const {
    std::vector<std::string> paths;
    for(const auto &file : files_) if(file_kind(fs::u8path(file.path)) == FileKind::Png) paths.push_back(file.path);
    return paths;
  }

  void workspace_style() {
    auto &s = ImGui::GetStyle();
    const ImVec4 ink{.16f,.20f,.23f,1}, muted{.35f,.40f,.43f,1}, paper{.98f,.97f,.94f,1};
    const ImVec4 chrome{.88f,.89f,.86f,1}, line{.75f,.78f,.75f,1};
    const ImVec4 accent{.24f,.45f,.40f,1}, hover{.77f,.87f,.82f,1};
    s.WindowPadding = {10, 9};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {7, 6};
    s.WindowRounding = s.FrameRounding = s.TabRounding = 3;
    s.WindowBorderSize = 1;
    auto &c = s.Colors;
    c[ImGuiCol_Text] = ink;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = c[ImGuiCol_PopupBg] = paper;
    c[ImGuiCol_ChildBg] = paper;
    c[ImGuiCol_MenuBarBg] = c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = chrome;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = chrome;
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = hover;
    c[ImGuiCol_Header] = c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = chrome;
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_FrameBgActive] = hover;
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_CheckMark] = c[ImGuiCol_NavHighlight] = accent;
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = c[ImGuiCol_ScrollbarGrab] = line;
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_DockingPreview] = accent;
  }
}
