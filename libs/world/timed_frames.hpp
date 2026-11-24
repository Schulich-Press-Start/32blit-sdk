#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sdk::detail {
  template<class Frames>
  uint64_t timed_frame_duration(const Frames &frames) {
    uint64_t duration = 0;
    for(const auto &frame : frames) duration += frame.duration_ms;
    return duration;
  }

  // For validated descriptor frames (at most 1024 frames of at most 60,000 ms).
  class PreparedFrameTimes {
  public:
    template<class Frames>
    explicit PreparedFrameTimes(const Frames &frames) {
      ends_.reserve(frames.size());
      uint32_t end = 0;
      for(const auto &frame : frames) {
        end += frame.duration_ms;
        ends_.push_back(end);
      }
    }

    uint32_t duration() const { return ends_.empty() ? 0 : ends_.back(); }
    std::size_t index_at(uint64_t time) const {
      return std::upper_bound(ends_.begin(), ends_.end(), time) - ends_.begin();
    }

  private:
    std::vector<uint32_t> ends_;
  };

  // Each frame occupies [start, end). The caller decides what to draw when no
  // interval matches (empty clip, zero duration, or beyond a non-looping clip).
  // When provided, prepared times must have been built from these same frames.
  template<class Frames>
  std::size_t timed_frame_index(const Frames &frames, uint64_t elapsed, bool loop,
                               const PreparedFrameTimes *prepared = nullptr) {
    if(loop) {
      const auto period = prepared ? prepared->duration() : timed_frame_duration(frames);
      if(!period) return frames.size();
      elapsed %= period;
    }
    if(prepared) return prepared->index_at(elapsed);
    for(std::size_t i = 0; i < frames.size(); ++i) {
      const auto duration = frames[i].duration_ms;
      if(elapsed < duration) return i;
      elapsed -= duration;
    }
    return frames.size();
  }
}
