#include "project_assets.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#include <stb_image.h>

namespace sdk {
  namespace {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    constexpr size_t ownership_cost = 4 * sizeof(void *);

    bool inside(const fs::path &root, const fs::path &absolute) {
      const auto relative = absolute.lexically_relative(root);
      return !relative.empty() && *relative.begin() != "..";
    }

    fs::path validated_root(const fs::path &root) {
      std::error_code error;
      const auto resolved = fs::canonical(root, error);
      if(error || !fs::is_directory(resolved, error) || error)
        throw ProjectAssetError(root.u8string(), "Expected an existing project directory.");
      return resolved;
    }

    template<class T> struct AssetDelete {
      size_t byte_cost;
      void operator()(const T *value) const { delete value; }
    };

    template<class T> size_t shared_cost(const std::shared_ptr<const T> &asset) {
      const auto metadata = std::get_deleter<AssetDelete<T>>(asset);
      return metadata ? metadata->byte_cost : 0;
    }

    size_t json_cost(const Json &value) {
      size_t cost = sizeof(Json);
      if(value.is_object()) {
        const auto &object = value.get_ref<const Json::object_t &>();
        cost += sizeof(object) + object.size() * (sizeof(Json::object_t::value_type) + 5 * sizeof(void *));
        for(const auto &[key, child] : object)
          cost += key.capacity() + 1 + json_cost(child);
      } else if(value.is_array()) {
        const auto &array = value.get_ref<const Json::array_t &>();
        cost += sizeof(array) + array.capacity() * sizeof(Json);
        for(const auto &child : array) cost += json_cost(child);
      } else if(value.is_string()) {
        const auto &text = value.get_ref<const std::string &>();
        cost += sizeof(text) + text.capacity() + 1;
      }
      return cost;
    }

    size_t sprite_cost(const SpriteClips &clips) {
      size_t cost = sizeof(SpriteAsset) + ownership_cost + clips.image.capacity() + 1;
      for(const auto &[name, clip] : clips.clips)
        cost += sizeof(decltype(clips.clips)::value_type) + 5 * sizeof(void *) + name.capacity() + 1
              + clip.frames.capacity() * sizeof(SpriteFrame);
      return cost;
    }

    template<class Map, class Expired> void prune_map(Map &map, Expired expired) {
      for(auto it = map.begin(); it != map.end();)
        if(expired(it->second)) it = map.erase(it);
        else ++it;
    }
  }

  ProjectAssetError::ProjectAssetError(std::string_view path, const std::string &reason)
    : std::runtime_error(reason), path(path) {}

  void ImageAsset::FreePixels::operator()(uint8_t *pixels) const { stbi_image_free(pixels); }

  ImageAsset::ImageAsset(Pixels pixels, int width, int height)
    : pixels(std::move(pixels)), surface(this->pixels.get(), blit::PixelFormat::RGBA, {width, height}),
      byte_cost(sizeof(ImageAsset) + ownership_cost + size_t(width) * size_t(height) * 4) {}

  ProjectAssets::ProjectAssets(const fs::path &root) : root_(validated_root(root)) {}

  fs::path ProjectAssets::path(std::string_view name) {
    if(const auto found = paths_.find(name); found != paths_.end()) return found->second;
    if(name.empty() || name.find_first_of(":\\") != name.npos ||
       std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
      throw ProjectAssetError(name, "Expected a project-relative path using /.");
    const auto requested = fs::u8path(name);
    if(requested.has_root_path() || std::find(requested.begin(), requested.end(), "..") != requested.end())
      throw ProjectAssetError(name, "Path must stay inside the project.");
    std::error_code error;
    auto resolved = fs::weakly_canonical(root_ / requested, error);
    if(error || !inside(root_, resolved))
      throw ProjectAssetError(name, "Cannot resolve this path inside the project.");
    if(paths_.size() >= path_cache_limit) paths_.erase(paths_.begin());
    return paths_.emplace(std::string(name), std::move(resolved)).first->second;
  }

  fs::path ProjectAssets::resource_path(std::string_view name) {
    const auto &resolved = path(name);
    if(resolved == root_) throw ProjectAssetError(name, "Expected a project file, not the project directory.");
    return resolved;
  }

  void ProjectAssets::prune() {
    prune_map(texts_, [](const auto &entry) { return entry.resource.expired(); });
    prune_map(jsons_, [](const auto &entry) { return entry.resource.expired(); });
    prune_map(images_, [](const auto &entry) { return entry.expired(); });
    prune_map(sprites_, [](const auto &entry) { return entry.expired(); });
  }

  std::string ProjectAssets::read(const fs::path &path, std::string_view name, size_t limit) {
    std::error_code error;
    if(!fs::is_regular_file(path, error) || error)
      throw ProjectAssetError(name, "File is missing or is not a regular file.");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::streamoff>(input.tellg());
    if(size < 0) throw ProjectAssetError(name, "Cannot open file.");
    if(static_cast<uintmax_t>(size) > limit || static_cast<uintmax_t>(size) > std::numeric_limits<size_t>::max())
      throw ProjectAssetError(name, "File exceeds the supported size limit.");
    std::string bytes(static_cast<size_t>(size), '\0');
    ++read_count_;
    if(!input.seekg(0).read(bytes.data(), static_cast<std::streamsize>(size)))
      throw ProjectAssetError(name, "Cannot read file.");
    return bytes;
  }

  std::shared_ptr<const std::string> ProjectAssets::text(std::string_view name, size_t limit) {
    prune();
    const auto &resolved = resource_path(name);
    if(const auto found = texts_.find(resolved); found != texts_.end()) {
      if(auto value = found->second.resource.lock()) {
        if(found->second.source_size > limit)
          throw ProjectAssetError(name, "File exceeds the supported size limit.");
        return value;
      }
    }
    auto bytes = read(resolved, name, limit);
    const auto source_size = bytes.size();
    const auto cost = sizeof(std::string) + ownership_cost + bytes.capacity() + 1;
    auto value = std::shared_ptr<const std::string>(new std::string(std::move(bytes)), AssetDelete<std::string>{cost});
    texts_.emplace(resolved, Entry<const std::string>{value, source_size});
    return value;
  }

  std::shared_ptr<const Json> ProjectAssets::json(std::string_view name, size_t limit) {
    prune();
    const auto &resolved = resource_path(name);
    if(const auto found = jsons_.find(resolved); found != jsons_.end()) {
      if(auto value = found->second.resource.lock()) {
        if(found->second.source_size > limit)
          throw ProjectAssetError(name, "File exceeds the supported size limit.");
        return value;
      }
    }
    const auto bytes = text(name, limit);
    ++decode_count_;
    auto parsed = Json::parse(*bytes, nullptr, false);
    if(parsed.is_discarded()) throw ProjectAssetError(name, "Invalid JSON; check this file's syntax.");
    const auto cost = json_cost(parsed) + ownership_cost;
    auto value = std::shared_ptr<const Json>(new Json(std::move(parsed)), AssetDelete<Json>{cost});
    jsons_.emplace(resolved, Entry<const Json>{value, bytes->size()});
    return value;
  }

  std::shared_ptr<ImageAsset> ProjectAssets::image(std::string_view name, int color_key) {
    prune();
    auto key = std::make_pair(resource_path(name), color_key);
    if(const auto found = images_.find(key); found != images_.end())
      if(auto value = found->second.lock()) return value;
    const auto bytes = text(name, asset_limit);
    int width, height;
    ++decode_count_;
    ImageAsset::Pixels pixels(stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes->data()),
      static_cast<int>(bytes->size()), &width, &height, nullptr, STBI_rgb_alpha));
    if(!pixels) throw ProjectAssetError(name, "Cannot decode PNG (maximum 2048 x 2048 pixels).");
    if(color_key >= 0) {
      for(auto *pixel = pixels.get(), *end = pixel + size_t(width) * size_t(height) * 4; pixel != end; pixel += 4)
        if(((pixel[0] << 16) | (pixel[1] << 8) | pixel[2]) == color_key) pixel[3] = 0;
    }
    auto value = std::make_shared<ImageAsset>(std::move(pixels), width, height);
    images_.emplace(std::move(key), value);
    return value;
  }

  std::shared_ptr<const SpriteAsset> ProjectAssets::sprite(std::string_view stem) {
    prune();
    // Appending a suffix must not turn the root-only name into a resource.
    if(stem.empty() || fs::u8path(stem).lexically_normal() == ".")
      throw ProjectAssetError(stem, "Expected a project-relative sprite stem.");
    const auto name = std::string(stem) + ".sprite.json";
    const auto &resolved = resource_path(name);
    if(const auto found = sprites_.find(resolved); found != sprites_.end())
      if(auto value = found->second.lock()) return value;
    SpriteClips clips;
    if(const char *error = read_sprite_clips(*json(name), clips)) throw ProjectAssetError(name, error);
    auto sheet = image(clips.image);
    const auto width = sheet->surface.bounds.w, height = sheet->surface.bounds.h;
    if(!width || !height || width % clips.frame_width || height % clips.frame_height)
      throw ProjectAssetError(name, "Frame dimensions must divide the PNG into complete cells.");
    const auto cells = (width / clips.frame_width) * (height / clips.frame_height);
    for(const auto &[clip_name, clip] : clips.clips)
      for(const auto &frame : clip.frames)
        if(frame.cell >= cells)
          throw ProjectAssetError(name, "Clip '" + clip_name + "' has a cell outside the PNG.");
    const auto cost = sprite_cost(clips);
    auto value = std::make_shared<const SpriteAsset>(SpriteAsset{std::move(clips), std::move(sheet), cost});
    sprites_.emplace(resolved, value);
    return value;
  }

  size_t ProjectAssets::byte_cost(const std::shared_ptr<const std::string> &asset) { return shared_cost(asset); }
  size_t ProjectAssets::byte_cost(const std::shared_ptr<const Json> &asset) { return shared_cost(asset); }
}
