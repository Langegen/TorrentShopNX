#include "torrserver_scheduler.h"

#ifdef TSNX_USE_LIBTORRENT

#include "../utils/log.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <future>

namespace datasource {

// =============================================================================
// Конструкторы
// =============================================================================

TorrServerScheduler::TorrServerScheduler()
    : cfg_{} {}

TorrServerScheduler::TorrServerScheduler(const SchedulerConfig& cfg)
    : cfg_(cfg) {}

// =============================================================================
// Инициализация
// =============================================================================

void TorrServerScheduler::init(lt::torrent_handle handle,
                                std::shared_ptr<lt::session> session,
                                int piece_size,
                                uint64_t file_offset_in_torrent,
                                int file_first_piece,
                                int file_last_piece) {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_            = std::move(handle);
    session_           = std::move(session);
    piece_size_        = piece_size;
    file_offset_in_torrent_ = file_offset_in_torrent;
    file_first_piece_  = file_first_piece;
    file_last_piece_   = file_last_piece;
    last_urgent_start_ = -1;
    stall_level_       = 0;
    last_snapshot_     = {};
    peer_ewma_stats_.clear();
    last_isolated_.clear();
    last_starvation_recovery_.clear();
    initialized_       = true;

    util::logLine("scheduler: init piece_size=" + std::to_string(piece_size) +
                  " file_pieces=" + std::to_string(file_first_piece) +
                  "-" + std::to_string(file_last_piece) +
                  " total_pieces=" + std::to_string(file_last_piece - file_first_piece + 1));
}

// =============================================================================
// on_read_request
// =============================================================================

SchedulerSnapshot TorrServerScheduler::on_read_request(std::int64_t offset,
                                                        std::int64_t /*size*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || piece_size_ <= 0) return {};

    const int critical_start = offset_to_piece(offset);
    if (critical_start < file_first_piece_ || critical_start > file_last_piece_) {
        return last_snapshot_;
    }

    if (critical_start == last_urgent_start_ && stall_level_ == 0) {
        return last_snapshot_;
    }

    return rebuild_window(critical_start);
}

SchedulerSnapshot TorrServerScheduler::on_read_request_with_peers(
        std::int64_t offset,
        std::int64_t /*size*/,
        const std::vector<lt::peer_info>& peers) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || piece_size_ <= 0) return {};

    const int critical_start = offset_to_piece(offset);
    if (critical_start < file_first_piece_ || critical_start > file_last_piece_) {
        return last_snapshot_;
    }

    // Обновляем EWMA для всех пиров
    update_peer_ewma(peers);

    // Если позиция не изменилась и нет stall — обновляем только EWMA + slow peer isolation
    if (critical_start == last_urgent_start_ && stall_level_ == 0) {
        // Переодическая изоляция медленных пиров без полного rebuild
        PieceRange critical;
        critical.start = critical_start;
        critical.end   = critical_start + cfg_.critical_pieces - 1;
        critical = clamp_to_file(critical);
        apply_slow_peer_isolation(peers, critical);
        return last_snapshot_;
    }

    return rebuild_5zone_window(critical_start, peers);
}

// =============================================================================
// reset
// =============================================================================

void TorrServerScheduler::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !handle_.is_valid()) return;

    util::logLine("scheduler: RESET — clearing all deadlines and priorities");
    clear_all_deadlines();
    set_all_dont_download();
    last_urgent_start_ = -1;
    stall_level_       = 0;
    last_snapshot_     = {};
    peer_ewma_stats_.clear();
    last_isolated_.clear();
    last_starvation_recovery_.clear();
    session_           = nullptr;
    initialized_       = false;
}

// =============================================================================
// Stall management
// =============================================================================

void TorrServerScheduler::on_stall() {
    std::lock_guard<std::mutex> lock(mutex_);
    stall_level_ = std::min(stall_level_ + 1, 4);
    util::logLine("scheduler: stall level=" + std::to_string(stall_level_));
    if (last_urgent_start_ >= 0) {
        rebuild_window(last_urgent_start_);
    }
}

void TorrServerScheduler::on_stall_recovered() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stall_level_ > 0) {
        stall_level_ = 0;
        util::logLine("scheduler: stall recovered");
        if (last_urgent_start_ >= 0) {
            rebuild_window(last_urgent_start_);
        }
    }
}

SchedulerSnapshot TorrServerScheduler::on_tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || last_urgent_start_ < 0) {
        return last_snapshot_;
    }
    // НЕ перестраиваем каждый тик — только по on_read_request / on_stall
    return last_snapshot_;
}

void TorrServerScheduler::on_tick_with_peers(const std::vector<lt::peer_info>& peers) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || last_urgent_start_ < 0) return;
    
    // Обновляем статистику скорости пиров в фоне
    update_peer_ewma(peers);
    
    // Во время stall инсталлер заблокирован в read(), поэтому он не вызывает on_read_request_with_peers.
    // Включаем slow peer isolation во время тиков, чтобы она работала во время stall
    apply_slow_peer_isolation(peers, last_snapshot_.critical);
}


SchedulerSnapshot TorrServerScheduler::on_piece_starvation(
        int target_piece,
        const std::vector<lt::peer_info>& peers,
        bool no_peer_on_piece) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !handle_.is_valid()) return last_snapshot_;

    target_piece = std::max(file_first_piece_, std::min(file_last_piece_, target_piece));

    const auto now = std::chrono::steady_clock::now();
    auto last_it = last_starvation_recovery_.find(target_piece);
    if (last_it != last_starvation_recovery_.end() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_it->second).count() < 1200) {
        apply_slow_peer_isolation(peers, last_snapshot_.critical);
        return last_snapshot_;
    }
    last_starvation_recovery_[target_piece] = now;

    // Escalate the normal stall level so the regular rebuild keeps prefetch/speculative collapsed.
    stall_level_ = std::max(stall_level_, no_peer_on_piece ? 2 : 1);

    PieceRange critical;
    critical.start = target_piece;
    critical.end   = target_piece + (no_peer_on_piece ? 1 : cfg_.critical_pieces - 1);
    critical = clamp_to_file(critical);

    PieceRange urgent;
    urgent.start = critical.end + 1;
    urgent.end   = critical.end + (no_peer_on_piece ? 2 : 4);
    urgent = clamp_to_file(urgent);

    PieceRange tail;
    tail.start = target_piece - cfg_.tail_pieces;
    tail.end   = target_piece - 1;
    tail = clamp_to_file(tail);

    const int window_min = tail.valid() ? tail.start : critical.start;
    const int window_max = urgent.valid() ? urgent.end : critical.end;
    for (int p = file_first_piece_; p <= file_last_piece_; ++p) {
        if (p < window_min || p > window_max) {
            // ВАЖНО: НИКОГДА не ставим dont_download (0) для кусков, которые вышли из окна.
            handle_.piece_priority(lt::piece_index_t(p), lt::download_priority_t(1));
            handle_.reset_piece_deadline(lt::piece_index_t(p));
        }
    }

    apply_tail(tail);
    if (urgent.valid()) {
        int deadline = no_peer_on_piece ? 100 : 200;
        for (int p = urgent.start; p <= urgent.end; ++p) {
            handle_.piece_priority(lt::piece_index_t(p), cfg_.urgent_priority);
            handle_.set_piece_deadline(p, deadline);
            deadline += no_peer_on_piece ? 100 : 200;
        }
    }

    if (critical.valid()) {
        for (int p = critical.start; p <= critical.end; ++p) {
            handle_.piece_priority(lt::piece_index_t(p), cfg_.critical_priority);
            handle_.reset_piece_deadline(lt::piece_index_t(p));
            handle_.set_piece_deadline(p, p == target_piece ? 0 : 1000,
                                       lt::torrent_handle::alert_when_available);
        }
    }

    const int slow_count = apply_slow_peer_isolation(peers, critical);

    SchedulerSnapshot snap;
    snap.critical = critical;
    snap.urgent = urgent;
    snap.tail = tail;
    snap.prefetch = {};
    snap.speculative = {};
    snap.deadline_step_ms = no_peer_on_piece ? 100 : 200;
    snap.slow_peers_count = slow_count;
    snap.window_changed = has_window_changed(snap);

    last_urgent_start_ = target_piece;
    last_snapshot_ = snap;

    util::logLine("scheduler: STARVATION recovery piece=" + std::to_string(target_piece) +
                  " reason=" + std::string(no_peer_on_piece ? "NO_PEER_ON_PIECE" : "SLOW_DELIVERY") +
                  " critical=" + std::to_string(critical.start) + "-" + std::to_string(critical.end) +
                  " urgent=" + std::to_string(urgent.valid() ? urgent.start : -1) +
                  "-" + std::to_string(urgent.valid() ? urgent.end : -1) +
                  " slow_peers=" + std::to_string(slow_count));

    return snap;
}

// =============================================================================
// rebuild_window — базовый вариант без peer_info
// =============================================================================

SchedulerSnapshot TorrServerScheduler::rebuild_window(int critical_start) {
    if (!initialized_ || !handle_.is_valid()) return {};

    const bool is_prebuffering = (critical_start - file_first_piece_ <= 2);
    const int crit_count  = cfg_.critical_pieces  + (stall_level_ > 0 ? cfg_.stall_extra_critical  : 0);
    const int urg_count   = is_prebuffering ? 8 : (cfg_.urgent_pieces     + (stall_level_ > 0 ? cfg_.stall_extra_urgent    : 0));
    const int pre_count   = is_prebuffering ? 0 : (cfg_.prefetch_pieces   + (stall_level_ > 0 ? cfg_.stall_extra_prefetch  : 0));
    const int spec_count  = is_prebuffering ? 0 : ((stall_level_ > 0) ? 0 : cfg_.speculative_pieces); // схлопнуть при stall
    const int tail_count  = cfg_.tail_pieces;
    const int d_step      = (stall_level_ > 0) ? cfg_.stall_deadline_step_ms : cfg_.deadline_step_ms;

    // Вычисляем зоны
    PieceRange critical;
    critical.start = critical_start;
    critical.end   = critical_start + crit_count - 1;
    critical = clamp_to_file(critical);

    PieceRange urgent;
    if (critical.valid()) {
        urgent.start = critical.end + 1;
        urgent.end   = critical.end + urg_count;
        urgent = clamp_to_file(urgent);
    }

    PieceRange prefetch;
    if (urgent.valid()) {
        prefetch.start = urgent.end + 1;
        prefetch.end   = urgent.end + pre_count;
        prefetch = clamp_to_file(prefetch);
    } else if (critical.valid()) {
        prefetch.start = critical.end + 1;
        prefetch.end   = critical.end + pre_count;
        prefetch = clamp_to_file(prefetch);
    }

    PieceRange speculative;
    if (spec_count > 0) {
        const int spec_base = prefetch.valid() ? prefetch.end : (urgent.valid() ? urgent.end : critical.end);
        speculative.start = spec_base + 1;
        speculative.end   = spec_base + spec_count;
        speculative = clamp_to_file(speculative);
    }

    PieceRange tail;
    tail.start = critical_start - tail_count;
    tail.end   = critical_start - 1;
    tail = clamp_to_file(tail);

    // Selective dont_download (только вне всего окна — не рвём пиры!)
    {
        const int window_min = tail.valid()   ? tail.start  : critical.start;
        const int window_max = speculative.valid() ? speculative.end :
                               (prefetch.valid()   ? prefetch.end    :
                               (urgent.valid()     ? urgent.end      : critical.end));
        for (int p = file_first_piece_; p <= file_last_piece_; ++p) {
            if (p < window_min || p > window_max) {
                // ВАЖНО: НИКОГДА не ставим dont_download (0) для кусков, которые вышли из окна.
                // Если кусок начал качаться (инсталлятор прыгнул к NCA заголовкам в конце файла),
                // а потом инсталлятор вернулся к piece 0, этот кусок окажется за пределами окна.
                // Установка 0 отменит in-flight запросы к пирам, что убьет их скорость до 0,
                // их target_dl_queue_length упадет до 1, и возникнет request queue death spiral!
                handle_.piece_priority(lt::piece_index_t(p), lt::download_priority_t(1));
            }
        }
    }

    // Сбросить дедлайны перед применением новых
    clear_all_deadlines();

    // Применяем зоны в порядке убывания важности
    apply_tail(tail);
    if (spec_count > 0) apply_speculative(speculative);
    apply_prefetch(prefetch);
    apply_critical(critical);
    // Urgent без peer_info — простой нарастающий deadline
    if (urgent.valid()) {
        int dl = cfg_.deadline_base_ms + crit_count * d_step;
        for (int p = urgent.start; p <= urgent.end; ++p) {
            handle_.piece_priority(lt::piece_index_t(p), cfg_.urgent_priority);
            handle_.set_piece_deadline(p, dl);
            dl += d_step;
        }
    }

    SchedulerSnapshot snap;
    snap.critical       = critical;
    snap.urgent         = urgent;
    snap.prefetch       = prefetch;
    snap.speculative    = speculative;
    snap.tail           = tail;
    snap.deadline_step_ms = d_step;
    snap.slow_peers_count = 0;
    snap.window_changed = has_window_changed(snap);

    last_urgent_start_ = critical_start;
    last_snapshot_     = snap;

    if (snap.window_changed) {
        util::logLine("scheduler: 5zone critical=" +
                      std::to_string(critical.start) + "-" + std::to_string(critical.end) +
                      " urgent=" + std::to_string(urgent.valid() ? urgent.start : -1) +
                      "-" + std::to_string(urgent.valid() ? urgent.end : -1) +
                      " prefetch=" + std::to_string(prefetch.valid() ? prefetch.start : -1) +
                      "-" + std::to_string(prefetch.valid() ? prefetch.end : -1) +
                      " spec=" + std::to_string(speculative.valid() ? speculative.start : -1) +
                      "-" + std::to_string(speculative.valid() ? speculative.end : -1) +
                      " stall=" + std::to_string(stall_level_));
    }

    return snap;
}

// =============================================================================
// rebuild_5zone_window — с peer_info (EDF + slow peer isolation)
// =============================================================================

SchedulerSnapshot TorrServerScheduler::rebuild_5zone_window(
        int critical_start,
        const std::vector<lt::peer_info>& peers) {

    if (!initialized_ || !handle_.is_valid()) return {};

    const bool is_prebuffering = (critical_start - file_first_piece_ <= 2);
    const int crit_count = cfg_.critical_pieces + (stall_level_ > 0 ? cfg_.stall_extra_critical : 0);
    const int urg_count  = is_prebuffering ? 8 : (cfg_.urgent_pieces   + (stall_level_ > 0 ? cfg_.stall_extra_urgent   : 0));
    const int pre_count  = is_prebuffering ? 0 : (cfg_.prefetch_pieces + (stall_level_ > 0 ? cfg_.stall_extra_prefetch : 0));
    const int spec_count = is_prebuffering ? 0 : ((stall_level_ > 0) ? 0 : cfg_.speculative_pieces);
    const int tail_count = cfg_.tail_pieces;
    const int d_step     = (stall_level_ > 0) ? cfg_.stall_deadline_step_ms : cfg_.deadline_step_ms;

    PieceRange critical;
    critical.start = critical_start;
    critical.end   = critical_start + crit_count - 1;
    critical = clamp_to_file(critical);

    PieceRange urgent;
    if (critical.valid()) {
        urgent.start = critical.end + 1;
        urgent.end   = critical.end + urg_count;
        urgent = clamp_to_file(urgent);
    }

    PieceRange prefetch;
    {
        const int pre_base = urgent.valid() ? urgent.end : critical.end;
        prefetch.start = pre_base + 1;
        prefetch.end   = pre_base + pre_count;
        prefetch = clamp_to_file(prefetch);
    }

    PieceRange speculative;
    if (spec_count > 0) {
        const int spec_base = prefetch.valid() ? prefetch.end : (urgent.valid() ? urgent.end : critical.end);
        speculative.start = spec_base + 1;
        speculative.end   = spec_base + spec_count;
        speculative = clamp_to_file(speculative);
    }

    PieceRange tail;
    tail.start = critical_start - tail_count;
    tail.end   = critical_start - 1;
    tail = clamp_to_file(tail);

    // Selective dont_download
    {
        const int window_min = tail.valid()       ? tail.start       : critical.start;
        const int window_max = speculative.valid() ? speculative.end  :
                               (prefetch.valid()   ? prefetch.end     :
                               (urgent.valid()     ? urgent.end       : critical.end));
        for (int p = file_first_piece_; p <= file_last_piece_; ++p) {
            if (p < window_min || p > window_max) {
                // ВАЖНО: НИКОГДА не ставим dont_download (0) для кусков, которые вышли из окна.
                handle_.piece_priority(lt::piece_index_t(p), lt::download_priority_t(1));
            }
        }
    }

    clear_all_deadlines();

    // Применяем зоны
    apply_tail(tail);
    if (spec_count > 0) apply_speculative(speculative);
    apply_prefetch(prefetch);
    apply_urgent_edf(urgent, peer_ewma_stats_, d_step, crit_count);
    apply_critical(critical);

    // Slow Peer Isolation для Critical зоны
    const int slow_count = apply_slow_peer_isolation(peers, critical);

    SchedulerSnapshot snap;
    snap.critical         = critical;
    snap.urgent           = urgent;
    snap.prefetch         = prefetch;
    snap.speculative      = speculative;
    snap.tail             = tail;
    snap.deadline_step_ms = d_step;
    snap.slow_peers_count = slow_count;
    snap.window_changed   = has_window_changed(snap);

    last_urgent_start_ = critical_start;
    last_snapshot_     = snap;

    if (snap.window_changed) {
        util::logLine("scheduler: 5zone+EDF critical=" +
                      std::to_string(critical.start) + "-" + std::to_string(critical.end) +
                      " urgent=" + std::to_string(urgent.valid() ? urgent.start : -1) +
                      "-" + std::to_string(urgent.valid() ? urgent.end : -1) +
                      " prefetch=" + std::to_string(prefetch.valid() ? prefetch.end : -1) +
                      " slow_peers=" + std::to_string(slow_count) +
                      " stall=" + std::to_string(stall_level_));
    }

    return snap;
}

// =============================================================================
// Зональные методы
// =============================================================================

void TorrServerScheduler::apply_critical(const PieceRange& range) {
    if (!range.valid()) return;
    int crit_dl = cfg_.critical_deadline_ms;
    for (int p = range.start; p <= range.end; ++p) {
        handle_.piece_priority(lt::piece_index_t(p), cfg_.critical_priority);
        handle_.set_piece_deadline(p, crit_dl, lt::torrent_handle::alert_when_available);
        crit_dl += 1000;
    }
}

void TorrServerScheduler::apply_urgent_edf(const PieceRange& range,
                                           const std::vector<PeerEwmaStats>& peer_stats,
                                           int deadline_step_ms,
                                           int crit_count) {
    if (!range.valid()) return;

    // EDF: сортируем куски по ожидаемому времени доставки
    // (упрощение: линейный deadline по позиции, step_ms уже учитывает EWMA скорость)
    // Для каждого куска deadline пропорционален его удалению от начала urgent зоны.

    // Определяем эффективный шаг на основе средней EWMA скорости быстрых пиров
    int effective_step = deadline_step_ms;
    if (!peer_stats.empty()) {
        float total_fast_speed = 0.0f;
        int fast_count = 0;
        for (const auto& ps : peer_stats) {
            if (!ps.is_slow && ps.ewma_speed_bps > 0) {
                total_fast_speed += ps.ewma_speed_bps;
                ++fast_count;
            }
        }
        if (fast_count > 0 && piece_size_ > 0) {
            const float avg_speed = total_fast_speed / fast_count;
            // Время доставки одного куска быстрым пиром (мс)
            const int piece_delivery_ms = static_cast<int>(
                (static_cast<float>(piece_size_) / avg_speed) * 1000.0f);
            // Шаг = время доставки / число кусков в urgent зоне
            const int num_urgent = range.end - range.start + 1;
            if (num_urgent > 0) {
                effective_step = std::max(10, std::min(piece_delivery_ms / num_urgent,
                                                       deadline_step_ms * 2));
            }
        }
    }

    int dl = cfg_.deadline_base_ms + 100;
    for (int p = range.start; p <= range.end; ++p) {
        handle_.piece_priority(lt::piece_index_t(p), cfg_.urgent_priority);
        handle_.set_piece_deadline(p, dl);
        dl += effective_step;
    }
}

void TorrServerScheduler::apply_prefetch(const PieceRange& range) {
    if (!range.valid()) return;
    constexpr lt::download_priority_t kPrefetchPrio{4};
    for (int p = range.start; p <= range.end; ++p) {
        handle_.piece_priority(lt::piece_index_t(p), kPrefetchPrio);
    }
}

void TorrServerScheduler::apply_speculative(const PieceRange& range) {
    if (!range.valid()) return;
    constexpr lt::download_priority_t kSpecPrio{2};
    for (int p = range.start; p <= range.end; ++p) {
        handle_.piece_priority(lt::piece_index_t(p), kSpecPrio);
    }
}

void TorrServerScheduler::apply_tail(const PieceRange& range) {
    if (!range.valid()) return;
    constexpr lt::download_priority_t kTailPrio{1};
    for (int p = range.start; p <= range.end; ++p) {
        handle_.piece_priority(lt::piece_index_t(p), kTailPrio);
    }
}

// =============================================================================
// Slow Peer Isolation
// =============================================================================

int TorrServerScheduler::apply_slow_peer_isolation(const std::vector<lt::peer_info>& peers,
                                                   const PieceRange& critical_range) {
    if (!critical_range.valid() || peers.empty()) return 0;

    // Карта: piece_index -> список IP-адресов пиров, у которых запрошены блоки этого куска
    std::unordered_map<int, std::vector<lt::address>> piece_to_peer_ips;

    // 1. Безопасно получаем подробную очередь закачки из libtorrent, запуская опрос в потоке io_service
    if (session_ && handle_.is_valid()) {
        struct PromiseData {
            std::promise<std::unordered_map<int, std::vector<lt::address>>> promise;
            lt::torrent_handle handle;
            PieceRange critical_range;
        };
        auto data = std::make_shared<PromiseData>();
        data->handle = handle_;
        data->critical_range = critical_range;
        auto future = data->promise.get_future();

        session_->get_io_service().post([data]() {
            std::unordered_map<int, std::vector<lt::address>> result;
            try {
                if (data->handle.is_valid()) {
                    std::vector<lt::partial_piece_info> download_queue;
                    data->handle.get_download_queue(download_queue);
                    for (const auto& ppi : download_queue) {
                        int p_idx = static_cast<int>(ppi.piece_index);
                        if (p_idx < data->critical_range.start || p_idx > data->critical_range.end) continue;
                        if (ppi.blocks == nullptr) continue;

                        for (int i = 0; i < ppi.blocks_in_piece; ++i) {
                            if (ppi.blocks[i].state == lt::block_info::requested) {
                                result[p_idx].push_back(ppi.blocks[i].peer().address());
                            }
                        }
                    }
                }
            } catch (...) {}
            try {
                data->promise.set_value(std::move(result));
            } catch (...) {}
        });

        // Ждем выполнения до 150мс. Если таймаут — пропускаем эту итерацию изоляции ради стабильности
        if (future.wait_for(std::chrono::milliseconds(150)) == std::future_status::ready) {
            try {
                piece_to_peer_ips = future.get();
            } catch (...) {}
        } else {
            util::logLine("scheduler: WARNING: apply_slow_peer_isolation get_download_queue timed out!");
        }
    } else if (handle_.is_valid()) {
        // Fallback: если session_ недоступна, пытаемся прочесть напрямую (но ловим исключения)
        std::vector<lt::partial_piece_info> download_queue;
        try {
            handle_.get_download_queue(download_queue);
            for (const auto& ppi : download_queue) {
                int p_idx = static_cast<int>(ppi.piece_index);
                if (p_idx < critical_range.start || p_idx > critical_range.end) continue;
                if (ppi.blocks == nullptr) continue;

                for (int i = 0; i < ppi.blocks_in_piece; ++i) {
                    if (ppi.blocks[i].state == lt::block_info::requested) {
                        piece_to_peer_ips[p_idx].push_back(ppi.blocks[i].peer().address());
                    }
                }
            }
        } catch (...) {}
    }

    int isolated = 0;
    for (const auto& pi : peers) {
        // Пир считается медленным если:
        // (1) RTT > порога (2000мс), ИЛИ 
        // (2) down_speed ненулевой, но ниже порога (100 KB/s), ИЛИ
        // (3) пир застрял (down_speed == 0 И выставлен флаг snubbed, то есть нет данных > 2 секунд).
        const bool slow_rtt   = (pi.rtt > 0 && pi.rtt > cfg_.slow_peer_rtt_ms);
        const bool slow_speed = (pi.down_speed > 0 && pi.down_speed < cfg_.slow_peer_speed_bps) ||
                                (pi.down_speed == 0 && (pi.flags & lt::peer_info::snubbed));
        if (!slow_rtt && !slow_speed) continue;

        // Определяем, какие критические куски обслуживает этот пир
        std::vector<int> target_pieces;
        
        // Способ A: через direct downloading_piece_index
        const int dl_piece = pi.downloading_piece_index;
        if (dl_piece >= critical_range.start && dl_piece <= critical_range.end) {
            target_pieces.push_back(dl_piece);
        }

        // Способ B: через get_download_queue блоки
        for (const auto& pair : piece_to_peer_ips) {
            int p_idx = pair.first;
            const auto& ips = pair.second;
            if (std::find(ips.begin(), ips.end(), pi.ip.address()) != ips.end()) {
                if (std::find(target_pieces.begin(), target_pieces.end(), p_idx) == target_pieces.end()) {
                    target_pieces.push_back(p_idx);
                }
            }
        }

        if (target_pieces.empty()) continue;

        for (int dl_piece_to_isolate : target_pieces) {
            // Ищем, есть ли в рое другие быстрые кандидаты на этот кусок, которые не задушили нас
            int fast_candidates = 0;
            for (const auto& other_pi : peers) {
                if (other_pi.ip == pi.ip) continue; // Пропускаем самого себя

                const bool other_slow_rtt   = (other_pi.rtt > 0 && other_pi.rtt > cfg_.slow_peer_rtt_ms);
                const bool other_slow_speed = (other_pi.down_speed > 0 && other_pi.down_speed < cfg_.slow_peer_speed_bps) ||
                                              (other_pi.down_speed == 0 && (other_pi.flags & lt::peer_info::snubbed));
                const bool other_choked_us  = (other_pi.flags & lt::peer_info::remote_choked);
                const bool other_has_piece  = (other_pi.flags & lt::peer_info::seed) ||
                                              (dl_piece_to_isolate >= 0 && dl_piece_to_isolate < static_cast<int>(other_pi.pieces.size()) && other_pi.pieces[lt::piece_index_t(dl_piece_to_isolate)]);
                const bool other_actively_downloading = (other_pi.down_speed > 0 || other_pi.download_queue_length > 0)
                                                        || (other_pi.flags & lt::peer_info::seed);

                // Защита от перегрузки очереди: кандидат должен иметь свободные слоты в очереди
                // ИЛИ иметь очень хороший RTT (< 150мс) при условии, что у медленного пира RTT > 1500мс
                // и очередь кандидата не забита полностью (менее 500 запросов).
                const bool other_has_queue_space = 
                    (other_pi.download_queue_length < other_pi.target_dl_queue_length + 8) ||
                    (other_pi.rtt > 0 && other_pi.rtt < 150 && pi.rtt > 1500 && other_pi.download_queue_length < 500);

                if (!other_slow_rtt && !other_slow_speed && !other_choked_us && other_has_piece && other_actively_downloading && other_has_queue_space) {
                    fast_candidates++;
                }
            }

            // Если альтернативных быстрых кандидатов на этот кусок нет — не изолируем медленного пира!
            if (fast_candidates == 0) {
                continue;
            }

            // RATE-LIMIT: не сбрасываем дедлайн одного и того же куска чаще, чем раз в 5 секунд.
            auto now = std::chrono::steady_clock::now();
            if (last_isolated_.count(dl_piece_to_isolate) &&
                std::chrono::duration_cast<std::chrono::seconds>(now - last_isolated_[dl_piece_to_isolate]).count() < 5) {
                continue;
            }
            last_isolated_[dl_piece_to_isolate] = now;

            // Стратегия изоляции: сбросить deadline на этом куске и выставить снова
            try {
                handle_.reset_piece_deadline(lt::piece_index_t(dl_piece_to_isolate));
                handle_.set_piece_deadline(dl_piece_to_isolate, 0, lt::torrent_handle::alert_when_available);
                boost::system::error_code ec;
                std::string ip_str = pi.ip.address().to_string(ec);
                if (ec) ip_str = "unknown";
                util::logLine("scheduler: ISOLATED slow peer " + ip_str +
                              " for piece " + std::to_string(dl_piece_to_isolate) +
                              " (active blocks detected!)" +
                              " speed=" + std::to_string(pi.down_speed / 1024) + "KB/s" +
                              " rtt=" + std::to_string(pi.rtt) + "ms" +
                              " snubbed=" + std::string((pi.flags & lt::peer_info::snubbed) ? "yes" : "no"));
            } catch (...) {}

            ++isolated;
        }
    }

    return isolated;
}

// =============================================================================
// EWMA управление пирами
// =============================================================================

void TorrServerScheduler::update_peer_ewma(const std::vector<lt::peer_info>& peers) {
    // Строим map endpoint → текущая скорость
    for (const auto& pi : peers) {
        bool found = false;
        for (auto& stats : peer_ewma_stats_) {
            if (stats.endpoint == pi.ip) {
                // EWMA обновление
                stats.ewma_speed_bps = stats.ewma_speed_bps * (1.0f - cfg_.ewma_alpha) +
                                       static_cast<float>(std::max(pi.down_speed, 0)) * cfg_.ewma_alpha;
                stats.last_rtt_ms = pi.rtt;
                stats.is_slow = (pi.rtt > cfg_.slow_peer_rtt_ms) ||
                                (pi.down_speed < cfg_.slow_peer_speed_bps);
                found = true;
                break;
            }
        }
        if (!found) {
            PeerEwmaStats new_stats;
            new_stats.endpoint       = pi.ip;
            new_stats.ewma_speed_bps = static_cast<float>(pi.down_speed);
            new_stats.last_rtt_ms    = pi.rtt;
            new_stats.is_slow        = (pi.rtt > cfg_.slow_peer_rtt_ms) ||
                                       (pi.down_speed < cfg_.slow_peer_speed_bps);
            peer_ewma_stats_.push_back(new_stats);
        }
    }

    // Удаляем пиров, которых больше нет в активном списке (старые записи)
    // Увеличено с 64 до 256, чтобы вмещать все 150 соединений роя без циклического удаления!
    constexpr size_t kMaxTrackedPeers = 256;
    if (peer_ewma_stats_.size() > kMaxTrackedPeers) {
        peer_ewma_stats_.erase(peer_ewma_stats_.begin(),
                               peer_ewma_stats_.begin() +
                               static_cast<int>(peer_ewma_stats_.size() - kMaxTrackedPeers));
    }
}

// =============================================================================
// Базовые операции
// =============================================================================

void TorrServerScheduler::clear_all_deadlines() {
    for (int p = file_first_piece_; p <= file_last_piece_; ++p) {
        handle_.reset_piece_deadline(lt::piece_index_t(p));
    }
}

void TorrServerScheduler::set_all_dont_download() {
    for (int p = file_first_piece_; p <= file_last_piece_; ++p) {
        handle_.piece_priority(lt::piece_index_t(p), lt::dont_download);
    }
}

PieceRange TorrServerScheduler::clamp_to_file(const PieceRange& range) const {
    PieceRange out;
    out.start = std::max(range.start, file_first_piece_);
    out.end   = std::min(range.end,   file_last_piece_);
    if (out.end < out.start) return {};
    return out;
}

bool TorrServerScheduler::has_window_changed(const SchedulerSnapshot& snap) const {
    return snap.critical.start   != last_snapshot_.critical.start   ||
           snap.critical.end     != last_snapshot_.critical.end     ||
           snap.urgent.start     != last_snapshot_.urgent.start     ||
           snap.urgent.end       != last_snapshot_.urgent.end       ||
           snap.prefetch.end     != last_snapshot_.prefetch.end     ||
           snap.speculative.end  != last_snapshot_.speculative.end;
}

int TorrServerScheduler::offset_to_piece(std::int64_t offset) const {
    if (piece_size_ <= 0 || offset < 0) return file_first_piece_;
    return static_cast<int>((file_offset_in_torrent_ + offset) / piece_size_);
}

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
