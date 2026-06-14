#include "local_libtorrent_backend.h"

#ifdef TSNX_USE_LIBTORRENT

#include "../torrent/torrent_engine.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include "congestion_controller.h"

#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/settings_pack.hpp>
#include <boost/shared_array.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace datasource {

LocalLibtorrentBackend::LocalLibtorrentBackend(const BackendConfig& cfg)
    : cfg_(cfg)
    , scheduler_() {
    status_.state = StreamState::Idle;
}

LocalLibtorrentBackend::~LocalLibtorrentBackend() {
    close();
}

bool LocalLibtorrentBackend::open(const ContentRequest& request) {
    stop_tick_thread();

    std::unique_lock<std::mutex> lock(mutex_);
    state_ = StreamState::Idle;
    status_ = {};
    opened_ = false;
    session_.reset();
    handle_ = {};
    io_recovery_count_ = 0;
    metrics_ = {};
    latency_mode_ = false;
    latency_mode_until_ = {};
    open_time_ = std::chrono::steady_clock::now();
    summary_logged_ = false;

    if (request.info_hash.empty() && request.magnet_link.empty() && request.torrent_file_path.empty()) {
        set_state(StreamState::Error, "пустой запрос: нет hash/magnet/file");
        return false;
    }

    info_hash_str_ = util::toLowerCopy(request.info_hash);

    set_state(StreamState::FetchingMetadata);
    lock.unlock();
    if (!prepare_via_engine(request)) {
        lock.lock();
        set_state(StreamState::Error, "prepareStream failed");
        return false;
    }
    lock.lock();

    if (!acquire_handle()) {
        set_state(StreamState::Error, "не удалось получить handle из TorrentEngine");
        return false;
    }

    // Инициализировать 5-зонный планировщик
    scheduler_.init(handle_, piece_size_, file_offset_in_torrent_, file_first_piece_, file_last_piece_);
    stall_monitor_.init(handle_, session_);
    stall_monitor_initialized_ = true;

    set_state(StreamState::FileSelected);
    set_state(StreamState::PrebufferInstallInfo);
    opened_ = true;
    status_.total_size = static_cast<uint64_t>(file_size_);

    util::logLine("backend/local: open OK file_index=" + std::to_string(file_index_) +
                  " size=" + std::to_string(file_size_) +
                  " pieces=" + std::to_string(file_first_piece_) + "-" + std::to_string(file_last_piece_));

    // Запустить tick-поток (CongestionController + планировщик)
    tick_running_ = true;
    tick_thread_ = std::thread(&LocalLibtorrentBackend::tick_thread_func, this);

    return true;
}

bool LocalLibtorrentBackend::prebuffer(std::int64_t offset, std::int64_t size) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!opened_ || !handle_.is_valid()) return false;

    set_state(StreamState::InstallInfoParsed);
    lock.unlock();
    // Мы больше не вызываем notifyPreparedStreamInstallInfoParsed, так как он
    // инициализирует старый планировщик TorrentEngine::StreamScheduler,
    // который ломает нам приоритеты (устанавливает dont_download на все).
    // LocalLibtorrentBackend полностью независим.
    lock.lock();

    set_state(StreamState::MainBuffering);
    return true;
}

std::int64_t LocalLibtorrentBackend::read(std::int64_t offset, void* buffer, std::int64_t size) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!opened_ || !handle_.is_valid() || offset < 0 || size <= 0 || !buffer) return 0;

    if (offset >= file_size_) {
        return 0; // EOF
    }
    if (offset + size > file_size_) {
        size = file_size_ - offset;
    }

    status_.last_offset = static_cast<uint64_t>(offset);
    status_.last_size   = static_cast<size_t>(size);

    lock.unlock();
    auto& engine = torrent::TorrentEngine::instance();

    // Обновляем наш 5-зонный планировщик
    {
        std::lock_guard<std::mutex> peers_lock(cached_peers_mutex_);
        if (scheduler_.is_initialized()) {
            if (!cached_peers_.empty()) {
                scheduler_.on_read_request_with_peers(offset, size, cached_peers_);
            } else {
                scheduler_.on_read_request(offset, size);
            }
        }
    }

    const int piece_start = static_cast<int>((file_offset_in_torrent_ + offset) / piece_size_);


    const auto wait_started_at = std::chrono::steady_clock::now();
    auto next_slow_read_log_at = wait_started_at + std::chrono::seconds(10);

    while (true) {
        // Читаем всё что уже доступно без блокировки
        size_t available = engine.readPreparedAvailable(static_cast<uint64_t>(offset), buffer, static_cast<size_t>(size));
        if (available > 0) {
            lock.lock();
            status_.stall_count = 0;
            const bool was_stalled = (state_ == StreamState::Stalled);
            if (state_ != StreamState::StreamingOrInstalling) {
                set_state(StreamState::StreamingOrInstalling);
            }
            lock.unlock();

            // Сообщаем планировщику о восстановлении после stall — сбрасываем stall_level,
            // восстанавливаем speculative зону и нормальный deadline_step.
            if (was_stalled) {
                const auto stall_recovered_at = std::chrono::steady_clock::now();
                lock.lock();
                ++metrics_.stall_recoveries;
                if (metrics_.current_stall_started.time_since_epoch().count() > 0) {
                    const auto stall_ms = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            stall_recovered_at - metrics_.current_stall_started).count());
                    metrics_.total_stall_ms += stall_ms;
                    metrics_.max_stall_ms = std::max(metrics_.max_stall_ms, stall_ms);
                    metrics_.current_stall_started = {};
                }
                lock.unlock();
                if (scheduler_.is_initialized()) {
                    scheduler_.on_stall_recovered();
                }
            }

            // Сообщаем движку минимальный оффсет, который нам всё ещё нужен.
            // Все предыдущие куски будут своевременно вытеснены из RAM для предотвращения OOM!
            torrent::TorrentEngine::setStreamMinKeepOffset(info_hash_str_, static_cast<uint64_t>(file_offset_in_torrent_ + offset) + available);
            return static_cast<std::int64_t>(available);
        }

        const auto now = std::chrono::steady_clock::now();
        const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_started_at).count();
        
        if (waited_ms >= 180000) { // 3 минуты таймаут
            lock.lock();
            ++status_.stall_count;
            set_state(StreamState::Stalled, "timeout waiting for pieces");
            log_session_summary_locked("timeout");
            return 0;
        }

        if (waited_ms >= 3000) {
            if (now >= next_slow_read_log_at) {
                util::logLine("backend/local: slow read wait_ms=" + std::to_string(waited_ms) +
                              " piece=" + std::to_string(piece_start) +
                              " offset=" + std::to_string(offset));
                next_slow_read_log_at = now + std::chrono::seconds(10);

                // Диагностика: сводка по пирам на момент ожидания
                std::lock_guard<std::mutex> peers_lock(cached_peers_mutex_);
                if (!cached_peers_.empty()) {
                    int total = static_cast<int>(cached_peers_.size());
                    int choked = 0, interesting = 0, on_target_piece = 0, active = 0;
                    int best_speed = 0;
                    for (const auto& pi : cached_peers_) {
                        if (pi.flags & lt::peer_info::remote_choked)  ++choked;
                        if (pi.flags & lt::peer_info::interesting)    ++interesting;
                        if (pi.down_speed > 0) { ++active; best_speed = std::max(best_speed, pi.down_speed); }
                        if (pi.downloading_piece_index == piece_start) ++on_target_piece;
                    }
                    const bool no_peer_on_piece = (on_target_piece == 0);
                    const bool all_choked = (total > 0 && choked == total);
                    const bool no_active_download = (active == 0);
                    const char* diagnosis = no_peer_on_piece ? "NO_PEER_ON_PIECE (priority/announce issue?)" :
                                             all_choked ? "ALL_CHOKED (choking algo issue?)" :
                                             no_active_download ? "NO_ACTIVE_DOWNLOAD (pipeline empty?)" :
                                             "SLOW_DELIVERY (RTT/bandwidth)";
                    util::logLine("backend/local: PIECE_WAIT_STATE piece=" + std::to_string(piece_start) +
                                  " peers=" + std::to_string(total) +
                                  " choked=" + std::to_string(choked) +
                                  " interesting=" + std::to_string(interesting) +
                                  " active=" + std::to_string(active) +
                                  " on_target=" + std::to_string(on_target_piece) +
                                  " best_speed=" + std::to_string(best_speed / 1024) + "KB/s" +
                                  " => " + diagnosis);
                    lock.lock();
                    ++metrics_.slow_read_logs;
                    if (no_peer_on_piece) ++metrics_.no_peer_on_piece;
                    else if (all_choked) ++metrics_.all_choked;
                    else if (no_active_download) ++metrics_.no_active_download;
                    else ++metrics_.slow_delivery;
                    enter_latency_mode_locked(no_peer_on_piece ? "NO_PEER_ON_PIECE" : "slow_read", now);
                    lock.unlock();

                    if (scheduler_.is_initialized()) {
                        if (no_peer_on_piece || waited_ms >= 10000) {
                            scheduler_.on_piece_starvation(piece_start, cached_peers_, no_peer_on_piece);
                            lock.lock();
                            ++metrics_.starvation_recoveries;
                            lock.unlock();
                        } else {
                            scheduler_.on_read_request_with_peers(offset, size, cached_peers_);
                        }
                    }
                } else {
                    util::logLine("backend/local: PIECE_WAIT_STATE piece=" + std::to_string(piece_start) +
                                  " NO_CACHED_PEERS (peer list not populated yet)");
                    lock.lock();
                    ++metrics_.slow_read_logs;
                    enter_latency_mode_locked("NO_CACHED_PEERS", now);
                    lock.unlock();
                    if (scheduler_.is_initialized()) {
                        scheduler_.on_read_request(offset, size);
                    }
                }
            } // end: if (now >= next_slow_read_log_at)

            // Paradox recovery: If libtorrent thinks it's done (seeding or finished), but we are stalled
            // waiting for a piece, it means MemoryStorage dropped it or it was never fully written (e.g. zero padding pieces).
            // Mark all pieces as available since libtorrent verified them, avoiding destructive force_recheck which resets RAM cache.
            if (waited_ms >= 5000 && handle_.is_valid()) {
                auto status = handle_.status(lt::torrent_handle::query_accurate_download_counters);
                if (status.state == lt::torrent_status::seeding || status.state == lt::torrent_status::finished) {
                    if (now >= next_slow_read_log_at - std::chrono::seconds(5)) {
                        util::logLine("backend/local: PARADOX DETECTED (state=" + std::to_string(status.state) + " but missing piece " + std::to_string(piece_start) + "). Recovering by marking all pieces available!");
                        torrent::markMemoryStorageAllPiecesAvailable(info_hash_str_);
                    }
                }
            }

            bool first_stall_for_this_wait = false;
            lock.lock();
            if (state_ != StreamState::Stalled) {
                ++metrics_.stall_entries;
                metrics_.current_stall_started = now;
                enter_latency_mode_locked("stall", now);
                set_state(StreamState::Stalled, "waiting for piece " + std::to_string(piece_start));
                first_stall_for_this_wait = true;
                if (scheduler_.is_initialized()) scheduler_.on_stall();
            } else {
                enter_latency_mode_locked("stall", now);
            }
            lock.unlock();

            // Не ждём 10-секундного slow-read лога: как только read() перешёл в Stalled,
            // немедленно сжимаем окно вокруг текущего куска. Это закрывает случай из log.txt,
            // где быстрые пиры качают future pieces, а текущий piece остаётся без on_target.
            if (first_stall_for_this_wait && scheduler_.is_initialized()) {
                std::vector<lt::peer_info> peers_snapshot;
                {
                    std::lock_guard<std::mutex> peers_lock(cached_peers_mutex_);
                    peers_snapshot = cached_peers_;
                }

                int on_target_piece = 0;
                int active = 0;
                int best_speed = 0;
                for (const auto& pi : peers_snapshot) {
                    if (pi.down_speed > 0) {
                        ++active;
                        best_speed = std::max(best_speed, pi.down_speed);
                    }
                    if (pi.downloading_piece_index == piece_start) {
                        ++on_target_piece;
                    }
                }

                const bool no_peer_on_piece = !peers_snapshot.empty() && active > 0 && on_target_piece == 0;
                scheduler_.on_piece_starvation(piece_start, peers_snapshot, no_peer_on_piece);
                lock.lock();
                ++metrics_.starvation_recoveries;
                lock.unlock();

                util::logLine("backend/local: STALL_RECOVERY piece=" + std::to_string(piece_start) +
                              " waited_ms=" + std::to_string(waited_ms) +
                              " peers=" + std::to_string(peers_snapshot.size()) +
                              " active=" + std::to_string(active) +
                              " on_target=" + std::to_string(on_target_piece) +
                              " best_speed=" + std::to_string(best_speed / 1024) + "KB/s" +
                              " reason=" + std::string(no_peer_on_piece ? "NO_PEER_ON_PIECE" : "STALL"));
            }
        } // end: if (waited_ms >= 3000)


        // Мягкое ожидание поступления новых 16KB блоков от пиров.
        // Читаем сырые данные мгновенно по мере их скачивания,
        // не дожидаясь окончания проверки хэша целого 8MB куска.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

BackendStatus LocalLibtorrentBackend::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool LocalLibtorrentBackend::isAvailable() const {
    return opened_ && handle_.is_valid();
}

int LocalLibtorrentBackend::downloadSpeedKBps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handle_.is_valid()) return 0;
    auto ts = handle_.status(lt::torrent_handle::query_accurate_download_counters);
    return ts.download_payload_rate / 1024;
}

int LocalLibtorrentBackend::pieceSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return piece_size_;
}

uint64_t LocalLibtorrentBackend::fileOffsetInTorrent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return file_offset_in_torrent_ > 0 ? static_cast<uint64_t>(file_offset_in_torrent_) : 0;
}

void LocalLibtorrentBackend::notifyStreamingComplete(bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) return;
    set_state(success ? StreamState::Completed : StreamState::Error,
              success ? "streaming finished" : "streaming failed");
    log_session_summary_locked(success ? "completed" : "failed");
}

void LocalLibtorrentBackend::close() {
    stop_tick_thread();
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != StreamState::Error && state_ != StreamState::Completed) {
        set_state(StreamState::Stopping);
    }

    log_session_summary_locked("close");

    handle_ = {};
    session_.reset();
    stall_monitor_.reset();
    stall_monitor_initialized_ = false;
    opened_ = false;
    util::logLine("backend/local: closed");
}

void LocalLibtorrentBackend::set_state(StreamState new_state, const std::string& detail) {
    if (state_ == new_state && detail.empty()) return;
    const StreamState old = state_;
    state_ = new_state;
    status_.state = new_state;
    if (!detail.empty()) status_.detail = detail;

    util::logLine("backend/local: state " + std::string(streamStateName(old)) +
                  " -> " + std::string(streamStateName(new_state)) +
                  (detail.empty() ? "" : " (" + detail + ")"));
}


void LocalLibtorrentBackend::enter_latency_mode_locked(const char* reason,
                                                       std::chrono::steady_clock::time_point now) {
    // Во время прогрева (первые 30 секунд после открытия) не входим в LATENCY_MODE,
    // так как на старте скорость раздачи еще только раскачивается и ограничение
    // очереди запросов в 1 секунду задушит скорость пиров.
    if (std::chrono::duration_cast<std::chrono::seconds>(now - open_time_).count() < 30) {
        return;
    }

    constexpr auto kLatencyHold = std::chrono::seconds(10);
    const bool was_in_latency = latency_mode_ && now < latency_mode_until_;
    latency_mode_ = true;
    latency_mode_until_ = now + kLatencyHold;
    if (!was_in_latency) {
        ++metrics_.latency_mode_entries;
        util::logLine(std::string("backend/local: LATENCY_MODE enter reason=") +
                      (reason ? reason : "unknown") + " hold=10s");
    }
}

void LocalLibtorrentBackend::log_session_summary_locked(const char* reason) {
    if (summary_logged_) return;
    if (!opened_ && metrics_.slow_read_logs == 0 && metrics_.stall_entries == 0 &&
        metrics_.starvation_recoveries == 0) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (metrics_.current_stall_started.time_since_epoch().count() > 0) {
        const auto stall_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - metrics_.current_stall_started).count());
        metrics_.total_stall_ms += stall_ms;
        metrics_.max_stall_ms = std::max(metrics_.max_stall_ms, stall_ms);
        metrics_.current_stall_started = {};
    }

    summary_logged_ = true;
    util::logLine("backend/local: SESSION_SUMMARY reason=" +
                  std::string(reason ? reason : "unknown") +
                  " slow_read_logs=" + std::to_string(metrics_.slow_read_logs) +
                  " stalls=" + std::to_string(metrics_.stall_entries) +
                  " stall_recoveries=" + std::to_string(metrics_.stall_recoveries) +
                  " stall_total_ms=" + std::to_string(metrics_.total_stall_ms) +
                  " stall_max_ms=" + std::to_string(metrics_.max_stall_ms) +
                  " no_peer_on_piece=" + std::to_string(metrics_.no_peer_on_piece) +
                  " all_choked=" + std::to_string(metrics_.all_choked) +
                  " no_active_download=" + std::to_string(metrics_.no_active_download) +
                  " slow_delivery=" + std::to_string(metrics_.slow_delivery) +
                  " starvation_recoveries=" + std::to_string(metrics_.starvation_recoveries) +
                  " latency_entries=" + std::to_string(metrics_.latency_mode_entries));
}

bool LocalLibtorrentBackend::prepare_via_engine(const ContentRequest& request) {
    auto& engine = torrent::TorrentEngine::instance();
    if (!engine.isRunning() && !engine.start()) return false;
    
    return engine.prepareStream(request.info_hash, request.magnet_link, request.torrent_file_path, request.file_index);
}

bool LocalLibtorrentBackend::acquire_handle() {
    auto access = torrent::TorrentEngine::instance().getStreamAccess();
    if (!access.valid || !access.session_ptr || !access.handle_ptr) return false;

    session_ = *static_cast<std::shared_ptr<lt::session>*>(access.session_ptr);
    handle_ = *static_cast<lt::torrent_handle*>(access.handle_ptr);

    file_index_ = access.file_index;
    file_size_ = access.file_size;
    file_offset_in_torrent_ = access.file_offset;
    piece_size_ = access.piece_size;
    file_first_piece_ = access.first_piece;
    file_last_piece_ = access.last_piece;

    return true;
}



std::int64_t LocalLibtorrentBackend::read_from_pieces(std::int64_t file_offset, void* buffer, std::int64_t size) {
    (void)file_offset; (void)buffer; (void)size;
    // Redundant now as we use engine.readPreparedStream() which calls readPreparedAvailable()
    return 0;
}

void LocalLibtorrentBackend::stop_tick_thread() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tick_running_ = false;
        tick_cv_.notify_all();
        piece_ready_cv_.notify_all();
    }
    if (tick_thread_.joinable()) tick_thread_.join();
}

void LocalLibtorrentBackend::tick_thread_func() {
    util::logLine("backend/local: tick thread started");
    CongestionController congestion_ctrl;
    int applied_queue_time = congestion_ctrl.current_queue_time();

    while (true) {
        // Ждём с разблокированным mutex — read() не блокируется
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tick_cv_.wait_for(lock, std::chrono::milliseconds(kTickIntervalMs),
                              [this] { return !tick_running_; });
            if (!tick_running_) break;
        }

        // Кратко берём mutex только для копирования handle/session
        lt::torrent_handle handle_copy;
        std::shared_ptr<lt::session> session_copy;
        bool latency_mode_active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!tick_running_) break;
            handle_copy  = handle_;
            session_copy = session_;
            const auto now = std::chrono::steady_clock::now();
            latency_mode_active = latency_mode_ && now < latency_mode_until_;
            if (latency_mode_ && !latency_mode_active) {
                latency_mode_ = false;
                util::logLine("backend/local: LATENCY_MODE exit");
            }
        }

        if (!handle_copy.is_valid()) continue;

        // ── Состояние торрента (handle thread-safe в libtorrent) ──────────────
        auto ts = handle_copy.status(lt::torrent_handle::query_accurate_download_counters);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.peers = ts.num_peers;
            status_.seeds = ts.num_seeds;

            // I/O error auto-recovery
            if (ts.errc && io_recovery_count_ < kMaxIoRecoveries) {
                util::logLine("backend/local: auto-recovering error: " + ts.errc.message());
                handle_copy.clear_error();
                handle_copy.resume();
                ++io_recovery_count_;
            }
        }

        // ── peer_info — вызываем БЕЗ mutex (libtorrent handle thread-safe) ───
        std::vector<lt::peer_info> peers;
        try {
            handle_copy.get_peer_info(peers);
        } catch (...) {}

        {
            std::lock_guard<std::mutex> peers_lock(cached_peers_mutex_);
            cached_peers_ = peers;
        }

        // ── BBR CongestionController ─────────────────────────────────────────
        {
            int min_limit = 2;
            int max_limit = 5;
            int rtt_base = congestion_ctrl.baseline_rtt_ms();
            if (rtt_base != INT_MAX) {
                if (rtt_base < 50) {
                    min_limit = 1;
                    max_limit = 3;
                } else if (rtt_base < 150) {
                    min_limit = 2;
                    max_limit = 5;
                } else if (rtt_base < 300) {
                    min_limit = 3;
                    max_limit = 8;
                } else {
                    min_limit = 4;
                    max_limit = 12;
                }
            }
            congestion_ctrl.set_limits(min_limit, max_limit);

            static int prev_min_limit = -1;
            static int prev_max_limit = -1;
            if (min_limit != prev_min_limit || max_limit != prev_max_limit) {
                util::logLine("backend/local: CC limits changed to [" + 
                              std::to_string(min_limit) + ", " + std::to_string(max_limit) + "]s (baseline=" +
                              (rtt_base == INT_MAX ? "none" : std::to_string(rtt_base) + "ms") + ")");
                prev_min_limit = min_limit;
                prev_max_limit = max_limit;
            }
        }

        int new_queue_time  = congestion_ctrl.update(peers, 0);
        if (latency_mode_active) {
            new_queue_time = std::min(new_queue_time, 2);
        }

        if (session_copy && new_queue_time != applied_queue_time) {
            try {
                lt::settings_pack pack;
                pack.set_int(lt::settings_pack::request_queue_time, new_queue_time);
                session_copy->apply_settings(pack);
                util::logLine("backend/local: CC queue_time " +
                              std::to_string(applied_queue_time) + "s -> " +
                              std::to_string(new_queue_time) + "s" +
                              " rtt_baseline=" + std::to_string(congestion_ctrl.baseline_rtt_ms()) + "ms" +
                              (latency_mode_active ? " mode=latency" : " mode=throughput"));
                applied_queue_time = new_queue_time;
            } catch (const std::exception& e) {
                util::logLine("backend/local: CC apply_settings ERROR: " + std::string(e.what()));
            } catch (...) {
                util::logLine("backend/local: CC apply_settings UNKNOWN ERROR");
            }
        }

        // ── Планировщик on_tick (не держим mutex — scheduler thread-safe) ────
        if (scheduler_.is_initialized()) scheduler_.on_tick_with_peers(peers);

        // ── StallMonitor ──
        if (stall_monitor_initialized_ && handle_copy.is_valid()) {
            int monitor_start = -1;
            int monitor_end = -1;
            if (scheduler_.is_initialized()) {
                auto snap = scheduler_.last_snapshot();
                if (snap.critical.valid()) {
                    monitor_start = snap.critical.start;
                    monitor_end = snap.critical.end;
                }
                if (snap.urgent.valid()) {
                    if (monitor_start == -1) {
                        monitor_start = snap.urgent.start;
                    }
                    monitor_end = snap.urgent.end;
                }
            }
            if (monitor_start != -1 && monitor_end != -1) {
                bool is_stalled_state = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    is_stalled_state = (state_ == StreamState::Stalled);
                }
                stall_monitor_.on_tick(monitor_start, monitor_end, is_stalled_state);
            }
        }

        // ── Периодическое логирование состояния роя (каждые 5 секунд) ────────
        static int log_counter = 0;
        if (++log_counter >= 5) {
            log_counter = 0;
            int active_peers = 0;
            for (const auto& pi : peers) {
                if (pi.down_speed > 0) active_peers++;
            }
            util::logLine("backend/local: SWARM status: total_peers=" + std::to_string(peers.size()) +
                          " active_peers=" + std::to_string(active_peers) +
                          " seeds=" + std::to_string(ts.num_seeds) +
                          " dl_rate=" + std::to_string(ts.download_payload_rate / 1024) + "KB/s");
            
            // Сортируем копию списка пиров по скорости скачивания
            std::vector<lt::peer_info> sorted_peers = peers;
            std::sort(sorted_peers.begin(), sorted_peers.end(), [](const lt::peer_info& a, const lt::peer_info& b) {
                return a.down_speed > b.down_speed;
            });

            int logged = 0;
            for (const auto& pi : sorted_peers) {
                if (logged >= 5) break;
                std::string flags_str = "";
                if (pi.flags & lt::peer_info::interesting) flags_str += "I";
                if (pi.flags & lt::peer_info::choked) flags_str += "C";
                if (pi.flags & lt::peer_info::remote_choked) flags_str += "R";
                if (pi.flags & lt::peer_info::seed) flags_str += "D";
                if (pi.flags & lt::peer_info::snubbed) flags_str += "S";
                if (pi.flags & lt::peer_info::local_connection) flags_str += "L";
                if (pi.flags & lt::peer_info::utp_socket) flags_str += "U";
                
                util::logLine("   peer[" + std::to_string(logged) + "]: " +
                              pi.ip.address().to_string() + ":" + std::to_string(pi.ip.port()) +
                              " speed=" + std::to_string(pi.down_speed / 1024) + "KB/s" +
                              " rtt=" + std::to_string(pi.rtt) + "ms" +
                              " piece=" + std::to_string(pi.downloading_piece_index) +
                              " reqq=" + std::to_string(pi.download_queue_length) + "/" + std::to_string(pi.target_dl_queue_length) +
                              " flags=" + flags_str +
                              " client=" + pi.client);
                logged++;
            }
        }

        piece_ready_cv_.notify_all();
    }
    util::logLine("backend/local: tick thread stopped");
}

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
