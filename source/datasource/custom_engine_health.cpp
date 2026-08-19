#include "custom_engine_health.h"

#include "../utils/log.h"

#include <sstream>

namespace datasource {

const char* customEngineHealthStateToString(CustomEngineHealthState state) {
    switch (state) {
        case CustomEngineHealthState::Healthy:           return "Healthy";
        case CustomEngineHealthState::Degraded:          return "Degraded";
        case CustomEngineHealthState::DiscoveryLost:     return "DiscoveryLost";
        case CustomEngineHealthState::TransportLost:     return "TransportLost";
        case CustomEngineHealthState::StarvingInstaller: return "StarvingInstaller";
    }
    return "Unknown";
}

void CustomEngineHealth::init(tsnx_engine* engine, const std::string& hash,
                              RestartCallback restart_cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = engine;
    hash_ = hash;
    restart_cb_ = restart_cb;
    current_state_ = CustomEngineHealthState::Healthy;
    consecutive_bad_ticks_ = 0;
    recovery_stage_ = 0;
    last_starving_ = false;
    starving_duration_ms_ = 0;
    cached_peers_ = 0;
    cached_kbps_ = 0.0f;
}

void CustomEngineHealth::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    current_state_ = CustomEngineHealthState::Healthy;
    consecutive_bad_ticks_ = 0;
    recovery_stage_ = 0;
    last_starving_ = false;
    starving_duration_ms_ = 0;
}

void CustomEngineHealth::evaluate(int peers, float download_kbps, bool starving) {
    cached_peers_ = peers;
    cached_kbps_ = download_kbps;

    const auto now = std::chrono::steady_clock::now();

    if (starving) {
        current_state_ = CustomEngineHealthState::StarvingInstaller;
    } else if (peers == 0 && download_kbps < 1.0f) {
        current_state_ = CustomEngineHealthState::DiscoveryLost;
    } else if (peers > 0 && download_kbps < 10.0f) {
        current_state_ = CustomEngineHealthState::TransportLost;
    } else if (download_kbps < 50.0f) {
        current_state_ = CustomEngineHealthState::Degraded;
    } else {
        current_state_ = CustomEngineHealthState::Healthy;
        consecutive_bad_ticks_ = 0;
        recovery_stage_ = 0;
        return;
    }

    if (consecutive_bad_ticks_ == 0) {
        bad_health_started_ = now;
    }
    consecutive_bad_ticks_++;

    // Recovery escalation.
    if (consecutive_bad_ticks_ >= 3) executeRecovery();
}

void CustomEngineHealth::executeRecovery() {
    const auto now = std::chrono::steady_clock::now();
    if (recovery_stage_ == 0) {
        util::logLine("health: stage1 reannounce");
        if (engine_ && !hash_.empty()) tsnx_engine_announce_now(engine_, hash_.c_str());
        recovery_stage_ = 1;
        last_stage_time_ = now;
        return;
    }

    const auto since_stage = std::chrono::duration_cast<std::chrono::seconds>(now - last_stage_time_).count();
    if (since_stage < 10) return;

    if (recovery_stage_ == 1) {
        util::logLine("health: stage2 DHT bootstrap / reannounce");
        if (engine_ && !hash_.empty()) {
            tsnx_engine_announce_now(engine_, hash_.c_str());
            // If a future DHT reset API is added, call it here.
        }
        recovery_stage_ = 2;
        last_stage_time_ = now;
        return;
    }

    if (recovery_stage_ == 2) {
        util::logLine("health: stage3 session restart requested");
        if (restart_cb_) restart_cb_();
        recovery_stage_ = 3;
        last_stage_time_ = now;
    }
}

CustomEngineHealthState CustomEngineHealth::on_tick(int peers, float download_kbps,
                                                    bool starving) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = std::chrono::steady_clock::now();
    if (starving) {
        if (!last_starving_) starving_started_ = now;
        starving_duration_ms_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - starving_started_).count());
    }
    last_starving_ = starving;

    evaluate(peers, download_kbps, starving);
    return current_state_;
}

std::string CustomEngineHealth::summaryString() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    oss << customEngineHealthStateToString(current_state_)
        << " peers=" << cached_peers_
        << " kbps=" << static_cast<int>(cached_kbps_)
        << " bad_ticks=" << consecutive_bad_ticks_
        << " stage=" << recovery_stage_;
    return oss.str();
}

} // namespace datasource
