#pragma once

#include <SDL_audio.h>

class Audio {
public:
    Audio();
    ~Audio();

    static constexpr int sample_rate = 22050;
    static constexpr SDL_AudioFormat format = AUDIO_S16SYS;
    static constexpr Uint8 channels = 1;

    static bool cues_available();
    // One borrowed clip at a time. Its owner must stop it before freeing the data.
    static bool play_cue(const void *owner, const Uint8 *data, Uint32 size);
    static void stop_cue(const void *owner);

private:
    static void fill(void *userdata, Uint8 *stream, int len);

    SDL_AudioDeviceID audio_device;
    // The device lock protects these fields on the game thread; SDL holds it in fill().
    const void *cue_owner = nullptr;
    const Uint8 *cue_data = nullptr;
    Uint32 cue_remaining = 0;
};
