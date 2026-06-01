#pragma once
// =============================================================================
// LocalLibtorrentBackend — Локальный бэкенд на базе libtorrent.
//
// КРИТИЧЕСКИ ВАЖНАЯ АРХИТЕКТУРА:
// Этот бэкенд НЕ создаёт свою lt::session!
// Он переиспользует session/handle из TorrentEngine через getStreamAccess().
// Создание второй session приводит к отказу пиров (duplicate connection от
// одного IP) и OOM на Switch.
//
// Принцип работы:
// 1. open()      → TorrentEngine::prepareStream() → getStreamAccess() → scheduler
// 2. prebuffer() → scheduler_.reset() → rebuild_window() с нового оффсета
// 3. read()      → scheduler → wait_for_pieces → read_piece → buffer
// 4. close()     → scheduler_.reset(), release handle
// =============================================================================

#ifdef TSNX_USE_LIBTORRENT


#include "i_content_backend.h"
#include "torrserver_scheduler.h"
#include "stall_monitor.h"

#include <libtorrent/peer_info.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/alert_types.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace datasource {

namespace lt = libtorrent;

class LocalLibtorrentBackend : public IContentBackend {
public:
    explicit LocalLibtorrentBackend(const BackendConfig& cfg);
    ~LocalLibtorrentBackend() override;

    // === IContentBackend =====================================================

    bool open(const ContentRequest& request) override;
    bool prebuffer(std::int64_t offset, std::int64_t size) override;
    std::int64_t read(std::int64_t offset, void* buffer, std::int64_t size) override;
    BackendStatus status() const override;
    bool isAvailable() const override;
    void notifyStreamingComplete(bool success) override;
    BackendType type() const override { return BackendType::LocalLibtorrent; }
    void close() override;
    int downloadSpeedKBps() const override;
    int pieceSize() const override;
    uint64_t fileOffsetInTorrent() const override;

private:
    // === Машина состояний ====================================================

    void set_state(StreamState new_state, const std::string& detail = "");

    // === Инициализация через TorrentEngine ===================================

    /// Подготовить стрим через TorrentEngine (вызов prepareStream).
    bool prepare_via_engine(const ContentRequest& request);

    /// Получить session/handle из TorrentEngine через getStreamAccess().
    bool acquire_handle();

    // === Чтение данных ======================================================

    /// Прочитать данные из скачанных кусков.
    std::int64_t read_from_pieces(std::int64_t file_offset, void* buffer,
                                   std::int64_t size);


    // === Фоновый тик-поток ==================================================

    void tick_thread_func();

    void enter_latency_mode_locked(const char* reason, std::chrono::steady_clock::time_point now);
    void log_session_summary_locked(const char* reason);

    /// Безопасная остановка tick thread (вызывать БЕЗ мьютекса).
    void stop_tick_thread();

    // === Состояние ===========================================================

    BackendConfig cfg_;
    mutable std::mutex mutex_;

    // Машина состояний
    StreamState state_ = StreamState::Idle;
    BackendStatus status_{};
    bool opened_ = false;

    // libtorrent — ПЕРЕИСПОЛЬЗУЕМЫЕ из TorrentEngine (НЕ владеем!)
    std::shared_ptr<lt::session> session_;  // shared_ptr копия
    lt::torrent_handle handle_;             // копия handle
    std::string info_hash_str_;             // info_hash в строковом виде

    // Информация о файле
    int file_index_ = -1;
    std::int64_t file_size_ = 0;
    std::int64_t file_offset_in_torrent_ = 0;
    int file_first_piece_ = 0;
    int file_last_piece_ = 0;
    int piece_size_ = 0;

    // Шедулер и stall monitor
    TorrServerScheduler scheduler_;
    StallMonitor stall_monitor_;
    bool stall_monitor_initialized_ = false;


    // Фоновый тик-поток
    std::thread tick_thread_;
    bool tick_running_ = false;
    std::condition_variable tick_cv_;
    std::condition_variable piece_ready_cv_;

    // Таймауты и лимиты
    static constexpr int kPieceWaitTimeoutMs = 45000;
    static constexpr int kPieceWaitPollMs = 100;
    static constexpr int kTickIntervalMs = 1000;
    static constexpr int kMaxIoRecoveries = 5;

    struct SessionMetrics {
        uint64_t slow_read_logs = 0;
        uint64_t no_peer_on_piece = 0;
        uint64_t all_choked = 0;
        uint64_t no_active_download = 0;
        uint64_t slow_delivery = 0;
        uint64_t starvation_recoveries = 0;
        uint64_t latency_mode_entries = 0;
        uint64_t stall_entries = 0;
        uint64_t stall_recoveries = 0;
        uint64_t total_stall_ms = 0;
        uint64_t max_stall_ms = 0;
        std::chrono::steady_clock::time_point current_stall_started{};
    };

    SessionMetrics metrics_{};
    bool latency_mode_ = false;
    std::chrono::steady_clock::time_point latency_mode_until_{};
    bool summary_logged_ = false;

    // I/O error recovery
    int io_recovery_count_ = 0;

    // Кэш peer_info для EDF планировщика (обновляется в tick_thread)
    mutable std::mutex          cached_peers_mutex_;
    std::vector<lt::peer_info>  cached_peers_;
};

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
