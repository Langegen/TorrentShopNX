#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef TSNX_USE_LIBTORRENT
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#endif

namespace torrent {

#ifdef TSNX_USE_LIBTORRENT
namespace lt = libtorrent;
#endif

enum class TorrentHealthState {
    Healthy,
    Degraded,
    DiscoveryLost,
    TransportLost,
    StarvingInstaller
};

const char* torrentHealthStateToString(TorrentHealthState state);

#ifdef TSNX_USE_LIBTORRENT

class TorrentSessionHealth {
public:
    using SessionRestartCallback = std::function<void()>;

    TorrentSessionHealth() = default;
    ~TorrentSessionHealth() = default;

    void init(std::shared_ptr<lt::session> session, SessionRestartCallback restart_cb = nullptr);
    void reset();

    /// Call every ~1 second from engine tick thread
    TorrentHealthState on_tick(const std::vector<lt::torrent_handle>& active_torrents, bool ring_buffer_empty = false);

    TorrentHealthState currentState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_state_;
    }

    std::string summaryString() const;

private:
    void evaluate_state_locked(const std::vector<lt::torrent_handle>& active_torrents, bool ring_buffer_empty);
    void execute_recovery_escalation_locked(const std::vector<lt::torrent_handle>& active_torrents);

    // Recovery stage handlers
    void run_stage1_bootstrap_reannounce_locked(const std::vector<lt::torrent_handle>& active_torrents);
    void run_stage2_dns_reconnect_locked(const std::vector<lt::torrent_handle>& active_torrents);
    void run_stage3_soft_dht_reset_locked(const std::vector<lt::torrent_handle>& active_torrents);
    void run_stage4_in_process_session_restart_locked();

    mutable std::mutex mutex_;
    std::shared_ptr<lt::session> session_;
    SessionRestartCallback session_restart_cb_;

    TorrentHealthState current_state_ = TorrentHealthState::Healthy;
    std::chrono::steady_clock::time_point last_log_time_{};
    std::chrono::steady_clock::time_point bad_health_started_{};

    int consecutive_bad_ticks_ = 0;
    int current_recovery_stage_ = 0; // 0 = none, 1 = stage1, 2 = stage2, 3 = stage3, 4 = stage4
    std::chrono::steady_clock::time_point last_stage_executed_time_{};

    // Metrics for log summary
    int cached_dht_nodes_ = 0;
    bool cached_listening_ = false;
    int cached_total_peers_ = 0;
    int cached_connect_candidates_ = 0;
    int cached_dl_rate_bps_ = 0;
    uint64_t starving_duration_ms_ = 0;
    std::chrono::steady_clock::time_point starving_started_{};
};

#endif // TSNX_USE_LIBTORRENT

} // namespace torrent
