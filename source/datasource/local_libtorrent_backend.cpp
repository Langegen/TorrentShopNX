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
            if (was_stalled && scheduler_.is_initialized()) {
                scheduler_.on_stall_recovered();
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
            return 0;
        }

        if (waited_ms >= 3000) {
            if (now >= next_slow_read_log_at) {
                util::logLine("backend/local: slow read wait_ms=" + std::to_string(waited_ms) + " piece=" + std::to_string(piece_start));
                next_slow_read_log_at = now + std::chrono::seconds(10);
                std::lock_guard<std::mutex> peers_lock(cached_peers_mutex_);
                if (scheduler_.is_initialized() && !cached_peers_.empty()) {
                    scheduler_.on_read_request_with_peers(offset, size, cached_peers_);
                }
            }

            // Paradox recovery: If libtorrent thinks it's done (seeding or finished), but we are stalled
            // waiting for a piece, it means MemoryStorage dropped it or it was never fully written.
            // Force a recheck so libtorrent hashes RAM, realizes it's missing, and redownloads it.
            if (waited_ms >= 5000 && handle_.is_valid()) {
                auto status = handle_.status(lt::torrent_handle::query_accurate_download_counters);
                if (status.state == lt::torrent_status::seeding || status.state == lt::torrent_status::finished) {
                    if (now >= next_slow_read_log_at - std::chrono::seconds(5)) { // Don't spam force_recheck
                        util::logLine("backend/local: PARADOX DETECTED (state=" + std::to_string(status.state) + " but missing piece " + std::to_string(piece_start) + "). Forcing recheck!");
                        handle_.force_recheck();
                        handle_.resume(); // ensure it resumes after checking
                    }
                }
            }

            lock.lock();
            if (state_ != StreamState::Stalled) {
                set_state(StreamState::Stalled, "waiting for piece " + std::to_string(piece_start));
                if (scheduler_.is_initialized()) scheduler_.on_stall();
            }
            lock.unlock();
        }

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
}

void LocalLibtorrentBackend::close() {
    stop_tick_thread();
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ != StreamState::Error && state_ != StreamState::Completed) {
        set_state(StreamState::Stopping);
    }

    handle_ = {};
    session_.reset();
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!tick_running_) break;
            handle_copy  = handle_;
            session_copy = session_;
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
        auto ti = handle_copy.torrent_file();
        if (ti) {
            int64_t total_size = ti->total_size();
            if (total_size < 200LL * 1024 * 1024) {
                // < 200 MB: cap max at 4s (не 2s!) — при RTT > 300мс жёсткий лимит 2s
                // оставляет < 1 блока in-flight и вызывает постоянные stall
                congestion_ctrl.set_limits(2, 4);
            } else if (total_size < 2LL * 1024 * 1024 * 1024) {
                congestion_ctrl.set_limits(2, 4);
            } else {
                congestion_ctrl.set_limits(2, 5);
            }
        }

        // ВАЖНО: prev берём ДО update(), т.к. update() меняет queue_time_sec_.
        const int prev_queue_time = congestion_ctrl.current_queue_time();
        const int new_queue_time  = congestion_ctrl.update(peers, 0);

        if (session_copy && new_queue_time != prev_queue_time) {
            try {
                lt::settings_pack pack;
                pack.set_int(lt::settings_pack::request_queue_time, new_queue_time);
                session_copy->apply_settings(pack);
                util::logLine("backend/local: CC queue_time " +
                              std::to_string(prev_queue_time) + "s -> " +
                              std::to_string(new_queue_time) + "s" +
                              " rtt_baseline=" + std::to_string(congestion_ctrl.baseline_rtt_ms()) + "ms");
            } catch (...) {}
        }

        // ── Планировщик on_tick (не держим mutex — scheduler thread-safe) ────
        if (scheduler_.is_initialized()) scheduler_.on_tick_with_peers(peers);

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
