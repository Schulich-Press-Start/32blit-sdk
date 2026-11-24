#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>
#include "graphics/surface.hpp"
#include "../world/sprite_clips.hpp"

namespace sdk {
  class ProjectAssetError : public std::runtime_error {
  public:
    ProjectAssetError(std::string_view path, const std::string &reason);
    const std::string path;
  };

  struct ImageAsset {
    struct FreePixels { void operator()(uint8_t *pixels) const; };
    using Pixels = std::unique_ptr<uint8_t, FreePixels>;

    ImageAsset(Pixels pixels, int width, int height);
    ImageAsset(const ImageAsset &) = delete;
    ImageAsset &operator=(const ImageAsset &) = delete;

    Pixels pixels;
    blit::Surface surface; // Borrows pixels; the asset must outlive every draw using it.
    const size_t byte_cost;
  };

  struct SpriteAsset {
    SpriteClips clips;
    std::shared_ptr<ImageAsset> image;
    // Exclusive descriptor cost; account image->byte_cost once per distinct image.
    size_t byte_cost = 0;
  };

  // One service per project generation. Owners pin resources; this index never does.
  // Loading and path resolution belong to preparation, never Render/Update.
  class ProjectAssets {
  public:
    static constexpr size_t asset_limit = 16 * 1024 * 1024;
    static constexpr size_t path_cache_limit = 256;
    struct Stats {
      uint64_t read_count = 0;
      uint64_t decode_count = 0;
      size_t path_cache_count = 0;
    };

    explicit ProjectAssets(const std::filesystem::path &root);
    ProjectAssets(const ProjectAssets &) = delete;
    ProjectAssets &operator=(const ProjectAssets &) = delete;

    const std::filesystem::path &root() const { return root_; }
    // Missing paths may resolve. Only path() accepts "." to resolve the root.
    std::filesystem::path path(std::string_view name);
    std::shared_ptr<const std::string> text(std::string_view name, size_t limit = asset_limit);
    std::shared_ptr<const nlohmann::json> json(std::string_view name, size_t limit = asset_limit);
    std::shared_ptr<ImageAsset> image(std::string_view name, int color_key = -1);
    std::shared_ptr<const SpriteAsset> sprite(std::string_view stem);
    Stats stats() const { return {read_count_, decode_count_, paths_.size()}; }

    // Metadata travels with the shared ownership, even after the service dies.
    static size_t byte_cost(const std::shared_ptr<const std::string> &asset);
    static size_t byte_cost(const std::shared_ptr<const nlohmann::json> &asset);

  private:
    template<class T> struct Entry {
      std::weak_ptr<T> resource;
      size_t source_size = 0;
    };
    void prune();
    std::filesystem::path resource_path(std::string_view name);
    std::string read(const std::filesystem::path &path, std::string_view name, size_t limit);

    const std::filesystem::path root_;
    std::map<std::string, std::filesystem::path, std::less<>> paths_;
    std::map<std::filesystem::path, Entry<const std::string>> texts_;
    std::map<std::filesystem::path, Entry<const nlohmann::json>> jsons_;
    std::map<std::pair<std::filesystem::path, int>, std::weak_ptr<ImageAsset>> images_;
    std::map<std::filesystem::path, std::weak_ptr<const SpriteAsset>> sprites_;
    uint64_t read_count_ = 0;
    uint64_t decode_count_ = 0;
  };
}
