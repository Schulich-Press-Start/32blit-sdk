#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "audio/audio.hpp"
#include "tinyalsa/include/tinyalsa/pcm.h"

namespace blit_linux {

/// ALSA audio output for the 32blit-linux port.
///
/// Opens the codec's PCM device (\c hw:0,0), fills buffers from
/// \c blit::get_audio_frame(), and writes them on a dedicated
/// \c std::thread. The game thread never blocks on audio.
///
/// Format: mono, signed 16-bit, 22050 Hz (\c blit::sample_rate).
/// Buffering: 256-frame periods (~11.6 ms), 3 periods in the ring.
/// Underruns recover via pcm_prepare() and are counted.
class AudioALSA {
public:
  AudioALSA();
  ~AudioALSA();

  /// Open the PCM device and start the audio thread.
  /// Returns false if the device cannot be opened (the game still runs,
  /// just silently).
  bool init();

  /// Stop the audio thread and close the PCM device.
  void shutdown();

  /// Number of underruns since init() (for diagnostics / acceptance testing).
  unsigned int underruns() const { return underruns_; }

private:
  void run();                  // audio thread entry point

  struct pcm_config pcm_cfg_;
  struct pcm *pcm_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<unsigned int> underruns_{0};
};

} // namespace blit_linux