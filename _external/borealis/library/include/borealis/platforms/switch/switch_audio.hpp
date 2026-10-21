#pragma once

#include <borealis/core/audio.hpp>
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
    uint8_t dummy[216] = {};
};

} // namespace brls
