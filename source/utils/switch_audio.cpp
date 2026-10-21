#include <borealis/core/logger.hpp>
#include <borealis/core/application.hpp>
#include <borealis/platforms/switch/switch_audio.hpp>
#include "../config/config.h"

#ifdef __SWITCH__
#include <SDL2/SDL.h>
#else
#include <windows.h>
#include <mmsystem.h>
#endif

namespace brls
{

static const char* getSoundFilename(enum Sound sound) {
    switch (sound) {
        case SOUND_FOCUS_CHANGE:
        case SOUND_FOCUS_SIDEBAR:
        case SOUND_TOUCH:
        case SOUND_TOUCH_UNFOCUS:
        case SOUND_SLIDER_TICK:
            return "focus.wav";
        case SOUND_CLICK:
        case SOUND_CLICK_SIDEBAR:
        case SOUND_HONK:
        case SOUND_SLIDER_RELEASE:
            return "click.wav";
        case SOUND_BACK:
            return "back.wav";
        case SOUND_FOCUS_ERROR:
        case SOUND_CLICK_ERROR:
            return "error.wav";
        default:
            return nullptr;
    }
}

SwitchAudioPlayer::SwitchAudioPlayer()
{
#ifdef __SWITCH__
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        Logger::error("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
        return;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = nullptr;

    deviceId = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (deviceId == 0) {
        Logger::error("SDL_OpenAudioDevice failed: {}", SDL_GetError());
        return;
    }

    SDL_PauseAudioDevice(deviceId, 0);
    this->init = true;
    Logger::info("SwitchAudioPlayer: initialized SDL2 audio device {}", deviceId);
#else
    this->init = true;
#endif

    for (int i = 1; i < (int)_SOUND_MAX; i++) {
        this->load((enum Sound)i);
    }
}

bool SwitchAudioPlayer::load(enum Sound sound)
{
    if (!this->init || sound <= SOUND_NONE || sound >= _SOUND_MAX)
        return false;

    if (this->sounds[sound].loaded)
        return true;

    const char* filename = getSoundFilename(sound);
    if (!filename)
        return false;

    std::string path = std::string("romfs:/sound/") + filename;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        path = std::string("resources/sound/") + filename;
        f = fopen(path.c_str(), "rb");
    }

    if (f) {
        fclose(f);
    } else {
        Logger::warning("SwitchAudioPlayer: sound file not found: {}", filename);
        return false;
    }

#ifdef __SWITCH__
    SDL_AudioSpec wavSpec;
    Uint8* wavBuffer = nullptr;
    Uint32 wavLength = 0;
    if (SDL_LoadWAV(path.c_str(), &wavSpec, &wavBuffer, &wavLength) == nullptr) {
        Logger::error("SwitchAudioPlayer: SDL_LoadWAV failed for {}: {}", path, SDL_GetError());
        return false;
    }

    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, wavSpec.format, wavSpec.channels, wavSpec.freq,
                          AUDIO_S16SYS, 1, 44100) > 0) {
        cvt.buf = (Uint8*)malloc(wavLength * cvt.len_mult);
        if (cvt.buf) {
            memcpy(cvt.buf, wavBuffer, wavLength);
            cvt.len = wavLength;
            SDL_ConvertAudio(&cvt);
            this->sounds[sound].data.assign(cvt.buf, cvt.buf + cvt.len_cvt);
            free(cvt.buf);
        }
    } else {
        this->sounds[sound].data.assign(wavBuffer, wavBuffer + wavLength);
    }

    SDL_FreeWAV(wavBuffer);
    this->sounds[sound].loaded = true;
    this->sounds[sound].path = path;
    return true;
#else
    this->sounds[sound].loaded = true;
    this->sounds[sound].path = path;
    return true;
#endif
}

bool SwitchAudioPlayer::play(enum Sound sound, float pitch)
{
    if (!this->init || sound <= SOUND_NONE || sound >= _SOUND_MAX)
        return false;

    if (!config::ConfigManager::instance().getSoundEffectsEnabled())
        return false;

    if (!this->sounds[sound].loaded) {
        if (!this->load(sound))
            return false;
    }

#ifdef __SWITCH__
    if (this->deviceId != 0 && !this->sounds[sound].data.empty()) {
        SDL_ClearQueuedAudio(this->deviceId);
        SDL_QueueAudio(this->deviceId, this->sounds[sound].data.data(), this->sounds[sound].data.size());
        return true;
    }
#else
    if (!this->sounds[sound].path.empty()) {
        PlaySoundA(this->sounds[sound].path.c_str(), NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
        return true;
    }
#endif
    return false;
}

SwitchAudioPlayer::~SwitchAudioPlayer()
{
#ifdef __SWITCH__
    if (this->deviceId != 0) {
        SDL_CloseAudioDevice(this->deviceId);
        this->deviceId = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
    this->init = false;
}

} // namespace brls
