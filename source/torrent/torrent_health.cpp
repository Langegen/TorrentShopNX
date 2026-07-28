#include "torrent_health.h"

#ifdef TSNX_USE_LIBTORRENT

#include <libtorrent/session_status.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/error_code.hpp>

#include <iostream>
#include <sstream>
#include <thread>

namespace torrent {

const char* torrentHealthStateToString(TorrentHealthState state) {
    switch (state) {
        case TorrentHealthState::Healthy: return "Healthy";
        case TorrentHealthState::Degraded: return "Degraded";
        case TorrentHealthState::DiscoveryLost: return "DiscoveryLost";
        case TorrentHealthState::TransportLost: return "TransportLost";
        case TorrentHealthState::StarvingInstaller: return "StarvingInstaller";
        default: return "Unknown";
    }
}

void TorrentSessionHealth::init(std::shared_ptr<lt::session> session, SessionRestartCallback restart_cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_ = session;
    session_restart_cb_ = restart_cb;
    reset();
}

void TorrentSessionHealth::reset() {
    current_state_ = TorrentHealthState::Healthy;
    consecutive_bad_ticks_ = 0;
    current_recovery_stage_ = 0;
    last_log_time_ = std::chrono::steady_clock::now();
    bad_health_started_ = std::chrono::steady_clock::time_point{};
    last_stage_executed_time_ = std::chrono::steady_clock::time_point{};
    starving_duration_ms_ = 0;
    starving_started_ = std::chrono::steady_clock::time_point{};
}

TorrentHealthState TorrentSessionHealth::on_tick(const std::vector<lt::torrent_handle>& active_torrents, bool ring_buffer_empty) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!session_) {
        return TorrentHealthState::TransportLost;
    }

    evaluate_state_locked(active_torrents, ring_buffer_empty);
    execute_recovery_escalation_locked(active_torrents);

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time_).count() >= 10) {
        last_log_time_ = now;
        std::cout << "[TORRENT_HEALTH] " << summaryString() << std::endl;
    }

    return current_state_;
}

std::string TorrentSessionHealth::summaryString() const {
    std::ostringstream ss;
    ss << "state=" << torrentHealthStateToString(current_state_)
       << " dht=" << cached_dht_nodes_
       << " listening=" << (cached_listening_ ? "yes" : "no")
       << " peers=" << cached_total_peers_
       << " candidates=" << cached_connect_candidates_
       << " dl=" << (cached_dl_rate_bps_ / 1024) << "KB/s"
       << " starving_ms=" << starving_duration_ms_
       << " recovery_stage=" << current_recovery_stage_;
    return ss.str();
}

void TorrentSessionHealth::evaluate_state_locked(const std::vector<lt::torrent_handle>& active_torrents, bool ring_buffer_empty) {
    auto now = std::chrono::steady_clock::now();

    // Collect session metrics
    lt::session_status sstat = session_->status();
    cached_dht_nodes_ = sstat.dht_nodes;
    cached_listening_ = session_->is_listening();
    cached_dl_rate_bps_ = sstat.download_rate;

    cached_total_peers_ = 0;
    cached_connect_candidates_ = 0;

    for (const auto& h : active_torrents) {
        if (!h.is_valid()) continue;
        lt::torrent_status ts = h.status();
        cached_total_peers_ += ts.num_peers;
        cached_connect_candidates_ += ts.connect_candidates;
    }

    // Track starving duration
    if (ring_buffer_empty) {
        if (starving_started_ == std::chrono::steady_clock::time_point{}) {
            starving_started_ = now;
        }
        starving_duration_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(now - starving_started_).count();
    } else {
        starving_started_ = std::chrono::steady_clock::time_point{};
        starving_duration_ms_ = 0;
    }

    // State evaluation
    if (!cached_listening_) {
        current_state_ = TorrentHealthState::TransportLost;
    } else if (starving_duration_ms_ > 3000) {
        current_state_ = TorrentHealthState::StarvingInstaller;
    } else if (cached_dht_nodes_ == 0 && cached_total_peers_ == 0 && cached_connect_candidates_ == 0) {
        current_state_ = TorrentHealthState::DiscoveryLost;
    } else if (cached_total_peers_ == 0 || (cached_dl_rate_bps_ < 5 * 1024 && cached_connect_candidates_ < 2)) {
        current_state_ = TorrentHealthState::Degraded;
    } else {
        current_state_ = TorrentHealthState::Healthy;
    }

    if (current_state_ != TorrentHealthState::Healthy) {
        if (bad_health_started_ == std::chrono::steady_clock::time_point{}) {
            bad_health_started_ = now;
        }
        consecutive_bad_ticks_++;
    } else {
        bad_health_started_ = std::chrono::steady_clock::time_point{};
        consecutive_bad_ticks_ = 0;
        current_recovery_stage_ = 0;
    }
}

void TorrentSessionHealth::execute_recovery_escalation_locked(const std::vector<lt::torrent_handle>& active_torrents) {
    if (current_state_ == TorrentHealthState::Healthy) return;

    auto now = std::chrono::steady_clock::now();
    int bad_duration_sec = std::chrono::duration_cast<std::chrono::seconds>(now - bad_health_started_).count();

    // Stage 1: 5 seconds bad health -> Bootstrap re-add + re-announce
    if (bad_duration_sec >= 5 && current_recovery_stage_ < 1) {
        current_recovery_stage_ = 1;
        last_stage_executed_time_ = now;
        run_stage1_bootstrap_reannounce_locked(active_torrents);
    }
    // Stage 2: 15 seconds bad health -> DNS reconnect & force announce
    else if (bad_duration_sec >= 15 && current_recovery_stage_ < 2) {
        current_recovery_stage_ = 2;
        last_stage_executed_time_ = now;
        run_stage2_dns_reconnect_locked(active_torrents);
    }
    // Stage 3: 35 seconds bad health -> Soft DHT reset
    else if (bad_duration_sec >= 35 && current_recovery_stage_ < 3) {
        current_recovery_stage_ = 3;
        last_stage_executed_time_ = now;
        run_stage3_soft_dht_reset_locked(active_torrents);
    }
    // Stage 4: 65 seconds bad health -> In-process session restart with BSD socket cooldown
    else if (bad_duration_sec >= 65 && current_recovery_stage_ < 4) {
        current_recovery_stage_ = 4;
        last_stage_executed_time_ = now;
        run_stage4_in_process_session_restart_locked();
    }
}

void TorrentSessionHealth::run_stage1_bootstrap_reannounce_locked(const std::vector<lt::torrent_handle>& active_torrents) {
    std::cout << "[TORRENT_HEALTH] Executing Recovery Stage 1: Re-adding DHT bootstrap IP fallbacks & re-announcing" << std::endl;
    if (!session_) return;

    session_->start_dht();
    static const std::vector<std::pair<std::string, int>> kFallbackNodes = {
        {"138.197.181.189", 6881},
        {"67.215.246.10", 6881},
        {"82.221.103.223", 6881},
        {"212.129.33.59", 6881}
    };
    for (const auto& node : kFallbackNodes) {
        session_->add_dht_node(node);
    }

    for (const auto& h : active_torrents) {
        if (!h.is_valid()) continue;
        h.force_dht_announce();
        h.force_reannounce(0, -1, lt::torrent_handle::ignore_min_interval);
        h.resume();
    }
}

void TorrentSessionHealth::run_stage2_dns_reconnect_locked(const std::vector<lt::torrent_handle>& active_torrents) {
    std::cout << "[TORRENT_HEALTH] Executing Recovery Stage 2: Re-resolving DNS bootstrap & forcing tracker announces" << std::endl;
    if (!session_) return;

    static const std::vector<std::pair<std::string, int>> kDhtRouters = {
        {"router.bittorrent.com", 6881},
        {"router.utorrent.com", 6881},
        {"dht.transmissionbt.com", 6881},
        {"dht.aelitis.com", 6881}
    };

    for (const auto& router : kDhtRouters) {
        session_->add_dht_node(router);
    }

    for (const auto& h : active_torrents) {
        if (!h.is_valid()) continue;
        h.force_dht_announce();
        h.force_reannounce();
    }
}

void TorrentSessionHealth::run_stage3_soft_dht_reset_locked(const std::vector<lt::torrent_handle>& active_torrents) {
    std::cout << "[TORRENT_HEALTH] Executing Recovery Stage 3: Soft DHT reset" << std::endl;
    if (!session_) return;

    for (const auto& h : active_torrents) {
        if (h.is_valid()) h.pause();
    }

    session_->stop_dht();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    session_->start_dht();

    run_stage1_bootstrap_reannounce_locked(active_torrents);

    for (const auto& h : active_torrents) {
        if (h.is_valid()) h.resume();
    }
}

void TorrentSessionHealth::run_stage4_in_process_session_restart_locked() {
    std::cout << "[TORRENT_HEALTH] Executing Recovery Stage 4: Triggering in-process session restart" << std::endl;
    if (session_restart_cb_) {
        // Trigger session restart callback (will perform BSD socket drain cooldown)
        session_restart_cb_();
    }
}

} // namespace torrent

#endif // TSNX_USE_LIBTORRENT
