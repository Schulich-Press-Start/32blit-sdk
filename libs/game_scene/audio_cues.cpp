#include "audio_cues.hpp"

#ifdef BLIT_SCENE_SDL_AUDIO
#include "Audio.hpp"
#include "game_scene.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>
#endif

namespace sdk {
#ifdef BLIT_SCENE_SDL_AUDIO
  namespace {
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    void free_wav(Uint8 *data) { SDL_FreeWAV(data); }
    void free_converted(Uint8 *data) { SDL_free(data); }
    using Buffer = std::unique_ptr<Uint8, void (*)(Uint8 *)>;

    struct Clip {
      Buffer data{nullptr, free_wav};
      Uint32 size = 0;
    };

    std::string regular_asset(const GameScene &game, std::string_view path) {
      auto resolved = game.project_asset_path(path);
      if(resolved.empty()) throw std::runtime_error("Cannot resolve this path inside the open project.");
      if(!fs::is_regular_file(fs::u8path(resolved))) throw std::runtime_error("File is missing or is not a regular file.");
      return resolved;
    }

    Clip load_wav(const GameScene &game, std::string_view path) {
      if(path.size() < 4) throw std::runtime_error("Expected a .wav path.");
      const auto suffix = path.substr(path.size() - 4);
      if(suffix[0] != '.' || (suffix[1] != 'w' && suffix[1] != 'W') ||
         (suffix[2] != 'a' && suffix[2] != 'A') || (suffix[3] != 'v' && suffix[3] != 'V')) {
        throw std::runtime_error("Expected a .wav path.");
      }
      const auto resolved = regular_asset(game, path);
      SDL_AudioSpec source{};
      Uint8 *samples = nullptr;
      Uint32 size = 0;
      if(!SDL_LoadWAV(resolved.c_str(), &source, &samples, &size)) {
        throw std::runtime_error(std::string("Cannot decode WAV: ") + SDL_GetError());
      }
      Clip clip{Buffer{samples, free_wav}, size};
      if(!samples || !size) throw std::runtime_error("WAV contains no samples.");

      SDL_AudioCVT conversion{};
      const auto needed = SDL_BuildAudioCVT(&conversion, source.format, source.channels, source.freq,
                                            Audio::format, Audio::channels, Audio::sample_rate);
      if(needed < 0) throw std::runtime_error(std::string("Unsupported WAV format: ") + SDL_GetError());
      if(needed) {
        if(conversion.len_mult < 1 || size > static_cast<Uint32>(std::numeric_limits<int>::max() / conversion.len_mult)) {
          throw std::runtime_error("WAV is too large to convert with SDL.");
        }
        if(conversion.len_mult > 1) {
          const auto capacity = static_cast<std::size_t>(size) * conversion.len_mult;
          Buffer converted{static_cast<Uint8 *>(SDL_malloc(capacity)), free_converted};
          if(!converted) throw std::runtime_error("Cannot allocate audio conversion buffer.");
          std::memcpy(converted.get(), samples, size);
          clip.data = std::move(converted);
        }
        conversion.buf = clip.data.get();
        conversion.len = static_cast<int>(size);
        if(SDL_ConvertAudio(&conversion) < 0) {
          throw std::runtime_error(std::string("Cannot convert WAV: ") + SDL_GetError());
        }
        if(conversion.len_cvt <= 0) throw std::runtime_error("WAV is too short to convert to simulator audio.");
        clip.size = static_cast<Uint32>(conversion.len_cvt);
      }
      if(clip.size % sizeof(Sint16)) throw std::runtime_error("WAV has an incomplete sample.");
      return clip;
    }
  }

  struct AudioCues::Player {
    std::map<std::string, Clip, std::less<>> clips;
  };

  AudioCues::AudioCues() = default;

  AudioCues::~AudioCues() {
    if(player_) Audio::stop_cue(this);
  }

  bool AudioCues::load(const std::string &path, const GameScene &game) {
    const auto fail = [&](std::string_view reason) {
      last_error_ = path.empty() ? "<empty audio descriptor path>" : path;
      last_error_ += ": ";
      last_error_.append(reason);
      return false;
    };

    auto next = std::make_unique<Player>();
    try {
      const auto resolved = regular_asset(game, path);
      std::ifstream input(fs::u8path(resolved), std::ios::binary);
      if(!input) return fail("Cannot open audio descriptor.");
      // The DOM parser otherwise silently keeps only the last value of a duplicate key.
      bool duplicate = false;
      const auto json = Json::parse(input, [&](int depth, Json::parse_event_t event, Json &value) {
        if(depth == 1 && event == Json::parse_event_t::key) {
          if(!next->clips.try_emplace(value.get_ref<const std::string &>()).second) duplicate = true;
        }
        return true;
      }, false);
      if(input.bad()) return fail("Cannot read audio descriptor.");
      if(json.is_discarded()) return fail("Invalid audio descriptor JSON.");
      if(!json.is_object()) return fail("Expected a JSON object mapping cue IDs to WAV paths.");
      if(duplicate) return fail("Audio cue IDs must be unique.");
      for(auto &entry : next->clips) {
        const auto &id = entry.first;
        if(id.empty()) return fail("Audio cue IDs must not be empty.");
        const auto &value = json.at(id);
        if(!value.is_string()) return fail("Expected a WAV path for cue '" + id + "'.");
        const auto &wav = value.get_ref<const std::string &>();
        try {
          entry.second = load_wav(game, wav);
        } catch(const std::runtime_error &error) {
          return fail("Cue '" + id + "', '" + wav + "': " + error.what());
        }
      }
    } catch(const std::exception &error) {
      return fail(error.what());
    }
    if(!next->clips.empty() && !Audio::cues_available()) return fail("SDL simulator audio output is unavailable.");

    if(player_) Audio::stop_cue(this);
    player_ = std::move(next);
    last_error_.clear();
    return true;
  }

  bool AudioCues::play(std::string_view cue) {
    if(!player_) {
      last_error_ = "No audio descriptor is loaded.";
      return false;
    }
    const auto clip = player_->clips.find(cue);
    if(clip == player_->clips.end()) {
      last_error_ = "Unknown audio cue ID: ";
      last_error_.append(cue);
      return false;
    }
    if(!Audio::play_cue(this, clip->second.data.get(), clip->second.size)) {
      last_error_ = "SDL simulator audio output is unavailable.";
      return false;
    }
    last_error_.clear();
    return true;
  }
#else
  struct AudioCues::Player {};
  AudioCues::AudioCues() = default;
  AudioCues::~AudioCues() = default;

  namespace {
    constexpr auto unavailable = "Audio cues require the native SDL simulator.";
  }

  bool AudioCues::load(const std::string &, const GameScene &) {
    last_error_ = unavailable;
    return false;
  }

  bool AudioCues::play(std::string_view) {
    last_error_ = unavailable;
    return false;
  }
#endif
}
