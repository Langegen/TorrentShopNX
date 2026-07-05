#pragma once
// =============================================================================
// TorrServerScheduler — 5-зонный планировщик кусков (High-Performance P2P).
//
// КЛЮЧЕВОЙ ПРИНЦИП: НЕ использовать sequential_download!
// Вместо этого — ручное управление приоритетами и дедлайнами.
//
// 5 ЗОН ПРИОРИТЕТОВ (относительно current_piece):
//
//  Зона          | ETA      | Стратегия
//  --------------|----------|-----------------------------------------
//  Critical      | 0–6 с    | top_priority + deadline=0, дубликаты на 2–3 быстрых пира
//  Urgent        | 6–15 с   | EDF по EWMA скорости пиров (peer_info::rtt)
//  Prefetch      | 15–45 с  | priority=4, sequential pipe fill
//  Speculative   | 45–90 с  | priority=2, схлопывается при деградации
//  Rarest        | 90 с+    | priority=1, slow peer isolation (snubbed)
//
// При каждом rebuild_5zone_window():
//   1. clear_all_deadlines() — жёсткий сброс ВСЕХ старых дедлайнов
//   2. Selective dont_download (только вне окна — не рвём пиры!)
//   3. Применяем 5 зон по порядку важности
// =============================================================================

#ifdef TSNX_USE_LIBTORRENT

#include "range_mapper.h"

#include <libtorrent/peer_info.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/session.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace datasource {

namespace lt = libtorrent;

/// Конфигурация 5-зонного планировщика.
struct SchedulerConfig {
    // === Размеры зон (в кусках) ===
    int critical_pieces    = 2;    ///< Зона 1: ~0.6 с при 24MB/s (было 4)
    int urgent_pieces      = 8;    ///< Зона 2: ~2.6 с (было 16)
    int prefetch_pieces    = 16;   ///< Зона 3: последовательная загрузка (было 24)
    int speculative_pieces = 8;    ///< Зона 4: speculative (было 16)
    int tail_pieces        = 2;    ///< Куски позади (кэш-пинне)

    // === Дедлайны ===
    int deadline_base_ms       = 0;
    int deadline_step_ms       = 100;  ///< Шаг для Urgent (EDF)
    int critical_deadline_ms   = 0;    ///< Все Critical куски = 0ms

    // === Stall-режим ===
    int stall_extra_critical   = 0;
    int stall_extra_urgent     = -4;   ///< Сжимаем Urgent зону с 8 до 4 кусков при stall (было -12)
    int stall_extra_prefetch   = -12;  ///< Сжимаем Prefetch при stall (с 16 до 4) (было -16)
    int stall_deadline_step_ms = 50;

    // === EDF / EWMA ===
    float ewma_alpha           = 0.25f; ///< Коэффициент сглаживания скорости пира
    int   slow_peer_rtt_ms     = 2000;  ///< Порог RTT для идентификации «медленного» пира
    int   slow_peer_speed_bps  = 100 * 1024; ///< Порог скорости (100 KB/s)

    // === Приоритеты ===
    lt::download_priority_t critical_priority = lt::top_priority; ///< Приоритет Critical (7)
    lt::download_priority_t urgent_priority   = lt::download_priority_t(6); ///< Приоритет Urgent (6)
};

/// Статистика EWMA для одного пира.
struct PeerEwmaStats {
    lt::tcp::endpoint endpoint;
    float ewma_speed_bps = 0.0f;  ///< Сглаженная скорость
    int   last_rtt_ms    = 0;
    bool  is_slow        = false; ///< Классифицирован как медленный
};

/// Результат rebuild — для логирования.
struct SchedulerSnapshot {
    PieceRange critical;
    PieceRange urgent;
    PieceRange prefetch;
    PieceRange speculative;
    PieceRange tail;
    int  deadline_step_ms  = 100;
    bool window_changed    = false;
    int  slow_peers_count  = 0;   ///< Число изолированных медленных пиров
};

class TorrServerScheduler {
public:
    TorrServerScheduler();
    explicit TorrServerScheduler(const SchedulerConfig& cfg);

    // =========================================================================
    // Инициализация
    // =========================================================================

    void init(lt::torrent_handle handle,
              std::shared_ptr<lt::session> session,
              int piece_size,
              uint64_t file_offset_in_torrent,
              int file_first_piece,
              int file_last_piece);

    // =========================================================================
    // Главные методы
    // =========================================================================

    /// Реакция на read request: пересчитывает все 5 зон.
    SchedulerSnapshot on_read_request(std::int64_t offset, std::int64_t size);

    /// Вариант с peer_info для EDF и slow peer isolation.
    SchedulerSnapshot on_read_request_with_peers(std::int64_t offset,
                                                  std::int64_t size,
                                                  const std::vector<lt::peer_info>& peers);

    /// Полный сброс (ОБЯЗАТЕЛЬНО при смене фазы Prebuffer → MainBuffering).
    void reset();

    void on_stall();
    void on_stall_recovered();
    SchedulerSnapshot on_tick();
    
    /// Фоновый тик с проверкой пиров (для применения изоляции во время stall)
    void on_tick_with_peers(const std::vector<lt::peer_info>& peers);

    /// Аварийное восстановление, когда read() ждёт текущий кусок, но быстрые
    /// пиры не переключаются на него. Сжимает окно вокруг target_piece,
    /// сбрасывает stale deadlines и форсирует deadline=0 для текущего куска.
    SchedulerSnapshot on_piece_starvation(int target_piece,
                                           const std::vector<lt::peer_info>& peers,
                                           bool no_peer_on_piece);

    // =========================================================================
    // Запрос состояния
    // =========================================================================

    bool is_initialized()    const { std::lock_guard<std::mutex> lock(mutex_); return initialized_; }
    int  piece_size()        const { std::lock_guard<std::mutex> lock(mutex_); return piece_size_; }
    int  file_first_piece()  const { std::lock_guard<std::mutex> lock(mutex_); return file_first_piece_; }
    int  file_last_piece()   const { std::lock_guard<std::mutex> lock(mutex_); return file_last_piece_; }
    int  last_urgent_start() const { std::lock_guard<std::mutex> lock(mutex_); return last_urgent_start_; }
    bool in_stall_mode()     const { std::lock_guard<std::mutex> lock(mutex_); return stall_level_ > 0; }
    SchedulerSnapshot last_snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return last_snapshot_; }

private:
    // =========================================================================
    // Ядро 5-зонного планировщика
    // =========================================================================

    /// Пересчёт 5 зон без данных о пирах.
    SchedulerSnapshot rebuild_window(int urgent_piece_start);

    /// Пересчёт 5 зон с EDF и slow peer isolation.
    SchedulerSnapshot rebuild_5zone_window(int critical_start,
                                           const std::vector<lt::peer_info>& peers);

    // =========================================================================
    // Зональные методы
    // =========================================================================

    /// Зона 1 Critical: top_priority + deadline=0.
    /// Дублирование запросов на быстрые пиры через set_piece_deadline(0) на
    /// всех пирах (libtorrent сам выберёт наилучший).
    void apply_critical(const PieceRange& range);

    /// Зона 2 Urgent: EDF по EWMA скорости пира.
    /// Назначает deadline_ms = rank * deadline_step_ms (ранг по убыванию скорости).
    void apply_urgent_edf(const PieceRange& range,
                          const std::vector<PeerEwmaStats>& peer_stats,
                          int deadline_step_ms,
                          int crit_count);

    /// Зона 3 Prefetch: последовательная загрузка, priority=4.
    void apply_prefetch(const PieceRange& range);

    /// Зона 4 Speculative: priority=2. Схлопывается при stall.
    void apply_speculative(const PieceRange& range);

    /// Tail (позади): priority=1, кэш-пинне.
    void apply_tail(const PieceRange& range);

    /// Slow Peer Isolation: для пиров с rtt > slow_peer_rtt_ms — сбросить
    /// deadline на куске, который они обслуживают → другой пир перехватит.
    int apply_slow_peer_isolation(const std::vector<lt::peer_info>& peers,
                                   const PieceRange& critical_range);

    // =========================================================================
    // EWMA управление пирами
    // =========================================================================

    /// Обновить EWMA статистику пиров.
    void update_peer_ewma(const std::vector<lt::peer_info>& peers);

    // =========================================================================
    // Базовые операции
    // =========================================================================

    void       clear_all_deadlines();
    void       clear_deadlines_for_removed_pieces(const PieceRange& new_critical, const PieceRange& new_urgent);
    void       set_all_dont_download();
    PieceRange clamp_to_file(const PieceRange& range) const;
    bool       has_window_changed(const SchedulerSnapshot& snap) const;
    int        offset_to_piece(std::int64_t offset) const;

    // =========================================================================
    // Состояние
    // =========================================================================

    SchedulerConfig    cfg_;
    lt::torrent_handle handle_;
    std::shared_ptr<lt::session> session_;
    bool               initialized_    = false;

    int piece_size_        = 0;
    uint64_t file_offset_in_torrent_ = 0;
    int file_first_piece_  = 0;
    int file_last_piece_   = 0;

    int last_urgent_start_ = -1;
    int stall_level_       = 0;

    SchedulerSnapshot      last_snapshot_;
    std::vector<PeerEwmaStats> peer_ewma_stats_; ///< EWMA-статистика пиров
    std::unordered_map<int, std::chrono::steady_clock::time_point> last_isolated_; ///< Rate-limit изоляций
    std::unordered_map<int, std::chrono::steady_clock::time_point> last_starvation_recovery_; ///< Rate-limit starvation recovery

    mutable std::mutex mutex_;
};

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
