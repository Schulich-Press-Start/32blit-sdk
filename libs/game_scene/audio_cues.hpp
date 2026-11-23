#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace sdk {
  class GameScene;

  /// Short, preloaded WAV cues for the native SDL simulator. Use on the game thread.
  class AudioCues {
  public:
    AudioCues();
    ~AudioCues();

    /// Load a project-relative .audio.json after GameScene::open(). Each key is a cue ID,
    /// each value a project-relative WAV. Failure leaves previous cues available.
    bool load(const std::string &path, const GameScene &game);

    /// Play once on a single shared cue channel, replacing the previous cue without
    /// interrupting the synth. SDL-supported WAVs are kept in memory as 22050 Hz mono.
    bool play(std::string_view cue);

    /// Most recent failure; empty after a successful call. Native Linux has no cue output.
    const std::string &last_error() const { return last_error_; }

  private:
    struct Player;
    std::unique_ptr<Player> player_;
    std::string last_error_;
  };
}
