#pragma once

#include <borealis/core/audio.hpp>
#include <vector>
#include <string>
#include <cstdint>

namespace brls
{

class SwitchAudioPlayer : public AudioPlayer
{
  public:
    SwitchAudioPlayer();
    ~SwitchAudioPlayer();

    bool load(enum Sound sound) override;
    bool play(enum Sound sound, float pitch = 1.0f) override;

  private:
    bool init = false;
    uint32_t deviceId = 0;

    struct SoundBuffer {
        std::vector<uint8_t> data;
        std::string path;
        bool loaded = false;
    };

    SoundBuffer sounds[_SOUND_MAX];
};

} // namespace brls
