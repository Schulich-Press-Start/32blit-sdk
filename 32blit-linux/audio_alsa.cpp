#include "audio_alsa.hpp"

#include <cstdio>
#include <vector>

#include "engine/api_private.hpp"

namespace blit_linux {

AudioALSA::AudioALSA() {
  pcm_cfg_ = {};
  pcm_cfg_.channels = 1;
  pcm_cfg_.rate = blit::sample_rate;        // 22050 Hz
  pcm_cfg_.format = PCM_FORMAT_S16_LE;
  pcm_cfg_.period_size = 256;               // 256 frames per period (~11.6 ms)
  pcm_cfg_.period_count = 3;                // 3 periods in the ring buffer
  pcm_cfg_.start_threshold = 256;
  pcm_cfg_.stop_threshold = 0;              // use driver default
  pcm_cfg_.silence_threshold = 0;           // use driver default
}

AudioALSA::~AudioALSA() {
  shutdown();
}

bool AudioALSA::init() {
  // Auto-probe for the first available playback PCM device.
  // Cards 0-7, device 0 (first PCM on each card).
  for (unsigned int card = 0; card < 8; card++) {
    pcm_ = pcm_open(card, 0, PCM_OUT, &pcm_cfg_);
    if (pcm_ && pcm_is_ready(pcm_)) {
      fprintf(stderr, "ALSA: opened hw:%u,0\n", card);
      break;
    }
    if (pcm_) {
      fprintf(stderr, "ALSA: card %u failed: %s\n", card, pcm_get_error(pcm_));
      pcm_close(pcm_);
      pcm_ = nullptr;
    }
  }

  if (!pcm_ || !pcm_is_ready(pcm_)) {
    if (pcm_) {
      fprintf(stderr, "ALSA: cannot open PCM device: %s\n", pcm_get_error(pcm_));
      pcm_close(pcm_);
      pcm_ = nullptr;
    } else {
      fprintf(stderr, "ALSA: cannot allocate PCM device\n");
    }
    return false;
  }

  // Prime the stream so the first write doesn't trip an underrun just
  // because the device was freshly opened and idle.
  if (pcm_prepare(pcm_) < 0) {
    fprintf(stderr, "ALSA: pcm_prepare failed: %s\n", pcm_get_error(pcm_));
    pcm_close(pcm_);
    pcm_ = nullptr;
    return false;
  }

  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void AudioALSA::shutdown() {
  if (!running_)
    return;

  running_ = false;
  if (thread_.joinable())
    thread_.join();

  unsigned int total = underruns_.load();
  if (total > 0)
    fprintf(stderr, "ALSA: %u underrun(s) total\n", total);

  if (pcm_) {
    pcm_drain(pcm_);
    pcm_close(pcm_);
    pcm_ = nullptr;
  }
}

void AudioALSA::run() {
  const unsigned int frame_count = pcm_cfg_.period_size;

  std::vector<int16_t> buffer(frame_count);

  while (running_) {
    // Fill one period from the engine's mixer.
    // get_audio_frame() returns the next mixed sample (unsigned 16-bit,
    // offset by 0x8000 to make it signed).
    for (unsigned int f = 0; f < frame_count; f++) {
      int32_t sample = static_cast<int32_t>(blit::get_audio_frame()) - 0x8000;
      if (sample > 32767)
        sample = 32767;
      if (sample < -32768)
        sample = -32768;
      buffer[f] = static_cast<int16_t>(sample);
    }

    int written = pcm_writei(pcm_, buffer.data(), frame_count);
    if (written < 0) {
      // Underrun (EPIPE) or other error; recover via pcm_prepare().
      unsigned int count = underruns_.fetch_add(1) + 1;
      fprintf(stderr, "ALSA: underrun #%u\n", count);
      if (pcm_prepare(pcm_) < 0) {
        fprintf(stderr, "ALSA: pcm_prepare failed\n");
        break;
      }
      // Retry the write after prepare.
      written = pcm_writei(pcm_, buffer.data(), frame_count);
      if (written < 0) {
        fprintf(stderr, "ALSA: pcm_writei failed: %s\n", pcm_get_error(pcm_));
        break;
      }
    }
  }
}

} // namespace blit_linux