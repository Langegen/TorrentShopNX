#pragma once

#include <borealis.hpp>
#include <chrono>
#include <vector>

namespace util {

enum class ScreenState {
    ACTIVE,
    SCREEN_OFF_COOLDOWN,
    SCREEN_OFF,
    WAKING_UP
};

class ScreenSleepManager {
public:
    static ScreenSleepManager& instance();

    void init();
    void shutdown();

    void turnOffScreen(bool manual = true);
    void turnOnScreen();
    void toggleScreen();

    bool isScreenOff() const;
    ScreenState getState() const { return state_; }
    void resetActivity();

private:
    ScreenSleepManager();
    ~ScreenSleepManager();
    ScreenSleepManager(const ScreenSleepManager&) = delete;
    ScreenSleepManager& operator=(const ScreenSleepManager&) = delete;

    bool handleInput(const brls::ControllerState& controller,
                     const std::vector<brls::RawTouchState>& rawTouch,
                     const brls::RawMouseState& rawMouse);

    static bool isStickDeflected(const brls::ControllerState& controller);
    static bool isUserInputActive(const brls::ControllerState& controller,
                                  const std::vector<brls::RawTouchState>& rawTouch);

    ScreenState state_ = ScreenState::ACTIVE;
    std::chrono::steady_clock::time_point lastActivityTime_;
    std::chrono::steady_clock::time_point screenOffTime_;
    std::chrono::steady_clock::time_point wakeTime_;
    bool hadActiveDownloads_ = false;
    bool initialized_ = false;
};

} // namespace util
