#include <algorithm>
#include <iostream>

#include "Audio.hpp"
#include "audio/audio.hpp"

extern Audio *blit_audio;

Audio::Audio() {
    SDL_AudioSpec desired = {};
    desired.freq = sample_rate;
    desired.format = format;
    desired.channels = channels;
    desired.samples = 256;
    desired.callback = fill;
    desired.userdata = this;

    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if(audio_device == 0) {
        std::cerr << "Audio Init Failed: " << SDL_GetError() << std::endl;
        return;
    }
    SDL_PauseAudioDevice(audio_device, 0);
}

Audio::~Audio() {
    if(audio_device) SDL_CloseAudioDevice(audio_device);
}

bool Audio::cues_available() {
    return blit_audio && blit_audio->audio_device &&
           SDL_GetAudioDeviceStatus(blit_audio->audio_device) == SDL_AUDIO_PLAYING;
}

bool Audio::play_cue(const void *owner, const Uint8 *data, Uint32 size) {
    if(!cues_available()) return false;
    auto &audio = *blit_audio;
    SDL_LockAudioDevice(audio.audio_device);
    audio.cue_owner = owner;
    audio.cue_data = data;
    audio.cue_remaining = size;
    SDL_UnlockAudioDevice(audio.audio_device);
    return true;
}

void Audio::stop_cue(const void *owner) {
    if(!blit_audio || !blit_audio->audio_device) return;
    auto &audio = *blit_audio;
    SDL_LockAudioDevice(audio.audio_device);
    if(audio.cue_owner == owner) {
        audio.cue_owner = nullptr;
        audio.cue_data = nullptr;
        audio.cue_remaining = 0;
    }
    SDL_UnlockAudioDevice(audio.audio_device);
}

void Audio::fill(void *userdata, Uint8 *stream, int len) {
    auto &audio = *static_cast<Audio *>(userdata);
    auto *samples = reinterpret_cast<Sint16 *>(stream);
    for(int i = 0; i < len / static_cast<int>(sizeof(Sint16)); ++i) {
        samples[i] = static_cast<int>(blit::get_audio_frame()) - 0x8000;
    }

    if(audio.cue_remaining) {
        const auto mixed = std::min(static_cast<Uint32>(len), audio.cue_remaining);
        SDL_MixAudioFormat(stream, audio.cue_data, format, mixed, SDL_MIX_MAXVOLUME);
        audio.cue_remaining -= mixed;
        if(audio.cue_remaining) {
            audio.cue_data += mixed;
        } else {
            audio.cue_owner = nullptr;
            audio.cue_data = nullptr;
        }
    }
}
