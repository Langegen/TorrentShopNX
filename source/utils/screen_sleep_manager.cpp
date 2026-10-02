#include "screen_sleep_manager.h"
#include "switch_utils.h"
#include "log.h"
#include "../config/config.h"
#include "../ui/DownloadUiManager.hpp"
#include <cmath>
#include <string>

namespace util {

ScreenSleepManager& ScreenSleepManager::instance() {
    static ScreenSleepManager inst;
    return inst;
}

ScreenSleepManager::ScreenSleepManager() {
    lastActivityTime_ = std::chrono::steady_clock::now();
}

ScreenSleepManager::~ScreenSleepManager() {
    shutdown();
}

void ScreenSleepManager::init() {
    if (initialized_) return;
    initialized_ = true;
    lastActivityTime_ = std::chrono::steady_clock::now();

    brls::Application::setInputInterceptor([this](const brls::ControllerState& controller,
                                                  const std::vector<brls::RawTouchState>& rawTouch,
                                                  const brls::RawMouseState& rawMouse) -> bool {
        return this->handleInput(controller, rawTouch, rawMouse);
    });

    util::logLine("ScreenSleepManager: initialized with global input interceptor");
}

void ScreenSleepManager::shutdown() {
    if (!initialized_) return;
    turnOnScreen();
    brls::Application::setInputInterceptor(nullptr);
    initialized_ = false;
    util::logLine("ScreenSleepManager: shut down");
}

void ScreenSleepManager::resetActivity() {
    lastActivityTime_ = std::chrono::steady_clock::now();
}

bool ScreenSleepManager::isScreenOff() const {
    return state_ == ScreenState::SCREEN_OFF || state_ == ScreenState::SCREEN_OFF_COOLDOWN;
}

void ScreenSleepManager::turnOffScreen(bool manual) {
    if (isScreenOff()) return;

    util::setBacklightOff(true);
    state_ = ScreenState::SCREEN_OFF_COOLDOWN;
    screenOffTime_ = std::chrono::steady_clock::now();
    hadActiveDownloads_ = (ui::DownloadManager::instance().getActiveDownloadsCount() > 0);

    util::logLine(std::string("ScreenSleepManager: screen turned OFF (") + (manual ? "manual" : "auto-timeout") +
                  "), hadActiveDownloads=" + std::to_string(hadActiveDownloads_));
}

void ScreenSleepManager::turnOnScreen() {
    if (util::isBacklightOff()) {
        util::setBacklightOff(false);
    }
    state_ = ScreenState::ACTIVE;
    lastActivityTime_ = std::chrono::steady_clock::now();
    hadActiveDownloads_ = false;
    util::logLine("ScreenSleepManager: screen turned ON directly to ACTIVE");
}

void ScreenSleepManager::toggleScreen() {
    if (isScreenOff()) {
        turnOnScreen();
    } else {
        turnOffScreen(true);
    }
}

bool ScreenSleepManager::isStickDeflected(const brls::ControllerState& controller) {
    const float threshold = 0.35f;
    return std::abs(controller.axes[brls::LEFT_X]) > threshold ||
           std::abs(controller.axes[brls::LEFT_Y]) > threshold ||
           std::abs(controller.axes[brls::RIGHT_X]) > threshold ||
           std::abs(controller.axes[brls::RIGHT_Y]) > threshold;
}

bool ScreenSleepManager::isUserInputActive(const brls::ControllerState& controller,
                                           const std::vector<brls::RawTouchState>& rawTouch) {
    for (int i = 0; i < brls::_BUTTON_MAX; ++i) {
        if (controller.buttons[i]) return true;
    }
    if (isStickDeflected(controller)) return true;
    for (const auto& t : rawTouch) {
        if (t.pressed) return true;
    }
    return false;
}

bool ScreenSleepManager::handleInput(const brls::ControllerState& controller,
                                     const std::vector<brls::RawTouchState>& rawTouch,
                                     const brls::RawMouseState& rawMouse) {
    const auto now = std::chrono::steady_clock::now();
    bool userActive = isUserInputActive(controller, rawTouch);

    switch (state_) {
        case ScreenState::ACTIVE: {
            if (userActive) {
                lastActivityTime_ = now;
            }

            // Check auto-dim timeout during active downloads
            int timeoutSec = config::ConfigManager::instance().getBacklightTimeout();
            if (timeoutSec > 0) {
                int activeCount = ui::DownloadManager::instance().getActiveDownloadsCount();
                if (activeCount > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastActivityTime_).count();
                    if (elapsed >= timeoutSec) {
                        turnOffScreen(false /* auto */);
                        return true; // Consume frame
                    }
                }
            }
            return false; // Normal input processing
        }

        case ScreenState::SCREEN_OFF_COOLDOWN: {
            // Must wait at least 500ms AND wait until user has completely released all buttons/touches/sticks
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - screenOffTime_).count();
            if (elapsedMs >= 500 && !userActive) {
                state_ = ScreenState::SCREEN_OFF;
            }
            return true; // Consume all inputs while in cooldown
        }

        case ScreenState::SCREEN_OFF: {
            // 1. Auto-wake if all active downloads have finished
            int activeCount = ui::DownloadManager::instance().getActiveDownloadsCount();
            if (hadActiveDownloads_ && activeCount == 0) {
                turnOnScreen();
                return false;
            }

            // 2. Wake up on any user input (button, stick, or touch)
            if (userActive) {
                util::setBacklightOff(false);
                wakeTime_ = now;
                state_ = ScreenState::WAKING_UP;
                util::logLine("ScreenSleepManager: wake-up triggered by user input, consuming initial press");
                return true; // Consume the wake-up press!
            }

            return true; // Swallow frame while screen is off
        }

        case ScreenState::WAKING_UP: {
            // Must wait at least 300ms AND wait until user has completely released the wake-up button/touch/stick
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - wakeTime_).count();
            if (elapsedMs >= 300 && !userActive) {
                state_ = ScreenState::ACTIVE;
                lastActivityTime_ = now;
                util::logLine("ScreenSleepManager: wake-up grace period ended, inputs unblocked");
                return false; // Inputs unblocked starting now
            }
            return true; // Continue consuming inputs during grace period
        }
    }

    return false;
}

} // namespace util
