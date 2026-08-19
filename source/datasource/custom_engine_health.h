#pragma once

#include <engine/engine.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace datasource {

enum class CustomEngineHealthState {
    Healthy,
    Degraded,
    DiscoveryLost,
    TransportLost,
    StarvingInstaller
};

const char* customEngineHealthStateToString(CustomEngineHealthState state);

class CustomEngineHealth {
public:
    using RestartCallback = std::function<void()>;

    CustomEngineHealth() = default;
    ~CustomEngineHealth() = default;

    void init(tsnx_engine* engine, const std::string& hash,
              RestartCallback restart_cb = nullptr);
    void reset();

    /// Call every ~1 second.  `starving` true when the installer waited >500ms
    /// for data it needed immediately.
    CustomEngineHealthState on_tick(int peers, float download_kbps, bool starving);

    CustomEngineHealthState currentState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_state_;
    }

    std::string summaryString() const;

private:
    void evaluate(int peers, float download_kbps, bool starving);
    void executeRecovery();

    tsnx_engine* engine_ = nullptr;
    std::string hash_;
    RestartCallback restart_cb_;

    mutable std::mutex mutex_;
    CustomEngineHealthState current_state_ = CustomEngineHealthState::Healthy;
    std::chrono::steady_clock::time_point bad_health_started_;
    std::chrono::steady_clock::time_point last_stage_time_;
    int consecutive_bad_ticks_ = 0;
    int recovery_stage_ = 0;
    bool last_starving_ = false;
    uint64_t starving_duration_ms_ = 0;
    std::chrono::steady_clock::time_point starving_started_;

    int cached_peers_ = 0;
    float cached_kbps_ = 0.0f;
};

} // namespace datasource
