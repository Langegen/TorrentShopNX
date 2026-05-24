#pragma once
// =============================================================================
// CongestionController — BBR-подобное управление очередью запросов.
//
// Проблема: request_queue_time зафиксирован в 2 с. При хороших пирах (>5 MB/s)
//           это занижает in-flight данные. При плохих — завышает и вызывает
//           congestion collapse.
//
// Алгоритм (упрощённый BBR для libtorrent 1.2):
//   1. Каждые kSampleIntervalMs измеряем RTT (peer_info::rtt) и delivery_rate.
//   2. Если delivery_rate растёт И RTT не вырос >20% → увеличить окно (до 15 с).
//   3. Если RTT вырос >20% → уменьшить окно.
//   4. Back-pressure от I/O: если write_buffer_fill_pct > 80% → экстренное
//      снижение окна (не ждём RTT feedback).
//
// Интеграция:
//   Вызывается из LocalLibtorrentBackend::tick_thread_func каждые kSampleIntervalMs.
//   Возвращает новый request_queue_time → caller применяет через apply_settings().
//
// Ограничения:
//   - libtorrent 1.2 не имеет per-peer cwnd; request_queue_time глобальный.
//   - peer_info::rtt в libtorrent 1.2 — приблизительный (EWMA по TCP ACK gaps).
// =============================================================================

#ifdef TSNX_USE_LIBTORRENT

#include <libtorrent/peer_info.hpp>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <vector>

namespace datasource {

struct CongestionSample {
    int    min_rtt_ms        = INT_MAX; ///< Минимальный RTT среди всех пиров
    int    delivery_rate_bps = 0;       ///< Суммарный download_rate
    int    active_peers      = 0;       ///< Пиры с down_speed > 0
    int    write_buffer_pct  = 0;       ///< Заполненность Tier-3 буфера (0–100%)
};

class CongestionController {
public:
    // =========================================================================
    // Константы
    // =========================================================================

    static constexpr int kMinQueueTimeSec  = 2;
    static constexpr int kMaxQueueTimeSec  = 8;
    static constexpr int kInitQueueTimeSec = 4;   ///< Старт с консервативного значения

    /// Порог роста RTT для deteckt congestion (%)
    /// 35% вместо 20%: при одном медленном пире RTT прыгает 80→300мс из-за
    /// джиттера сети — это шум, не реальный congestion.
    static constexpr int kRttGrowthThresholdPct = 35;

    /// Минимальный RTT baseline (мс) для активации congestion-детектора.
    /// При RTT < 50мс пир находится в LAN или рядом, и любые вариации (OS jitter,
    /// планировщик TCP) выглядят как congestion. При RTT < 50мс — не трогаем окно.
    static constexpr int kMinRttForCongestionMs = 50;

    /// Сколько подряд «растущих» RTT-сэмплов нужно для снижения окна.
    /// Защита от однократных spike: 1 выброс не должен уменьшать очередь.
    static constexpr int kCongestionConsecutiveThreshold = 2;

    /// Порог заполненности write buffer для back-pressure
    static constexpr int kWriteBufferBackpressurePct = 80;

    /// Интервал сбора сэмплов
    static constexpr int kSampleIntervalMs = 500;

    // =========================================================================

    CongestionController()
        : queue_time_sec_(kInitQueueTimeSec)
        , min_queue_time_sec_(kMinQueueTimeSec)
        , max_queue_time_sec_(kMaxQueueTimeSec)
        , min_rtt_baseline_ms_(INT_MAX)
        , last_delivery_rate_bps_(0) {}

    void set_limits(int min_sec, int max_sec) {
        min_queue_time_sec_ = min_sec;
        max_queue_time_sec_ = max_sec;
        queue_time_sec_ = std::min(max_sec, std::max(min_sec, queue_time_sec_));
    }

    // =========================================================================
    // Главный метод обновления
    // =========================================================================

    /// Обновить состояние контроллера и вернуть новый request_queue_time.
    ///
    /// @param peers            текущий список peer_info из get_peer_info()
    /// @param write_buffer_pct заполненность Tier-3 буфера (0–100)
    /// @return новый request_queue_time в секундах (применить через settings_pack)
    int update(const std::vector<lt::peer_info>& peers, int write_buffer_pct) {
        const auto now = std::chrono::steady_clock::now();
        if (last_sample_at_.time_since_epoch().count() > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sample_at_).count()
                < kSampleIntervalMs) {
            return queue_time_sec_; // Ещё не время
        }
        last_sample_at_ = now;

        CongestionSample sample = collect_sample(peers, write_buffer_pct);

        // ── Back-pressure от I/O ──────────────────────────────────────────────
        if (sample.write_buffer_pct >= kWriteBufferBackpressurePct) {
            queue_time_sec_ = std::max(min_queue_time_sec_, queue_time_sec_ / 2);
            io_backpressure_count_++;
            return queue_time_sec_;
        }
        if (io_backpressure_count_ > 0) {
            io_backpressure_count_ = 0; // Сбросить счётчик при снятии давления
        }

        // ── Нет активных пиров — не трогаем окно ─────────────────────────────
        if (sample.active_peers == 0 || sample.min_rtt_ms == INT_MAX) {
            return queue_time_sec_;
        }

        // ── Инициализация baseline RTT ────────────────────────────────────────
        if (min_rtt_baseline_ms_ == INT_MAX) {
            min_rtt_baseline_ms_ = sample.min_rtt_ms;
            last_delivery_rate_bps_ = sample.delivery_rate_bps;
            return queue_time_sec_;
        }

        // ── BBR-style решение ─────────────────────────────────────────────────
        const bool rtt_growing = is_rtt_growing(sample.min_rtt_ms);
        const bool rate_growing = sample.delivery_rate_bps > last_delivery_rate_bps_;
        // LAN-guard: если RTT baseline < kMinRttForCongestionMs — игнорируем congestion,
        // т.к. при RTT 3-12мс даже OS-jitter (10мс) выглядит как 42% рост.
        const bool rtt_too_low_for_cc = (min_rtt_baseline_ms_ < kMinRttForCongestionMs &&
                                         sample.min_rtt_ms    < kMinRttForCongestionMs);

        if (!rtt_growing && rate_growing) {
            // Хорошо: скорость растёт, RTT не деградирует → расширить окно
            consecutive_congestion_ = 0;
            queue_time_sec_ = std::min(max_queue_time_sec_, queue_time_sec_ + 1);
        } else if (rtt_growing && !rtt_too_low_for_cc) {
            // Признак перегрузки: нужно kCongestionConsecutiveThreshold подряд
            if (++consecutive_congestion_ >= kCongestionConsecutiveThreshold) {
                queue_time_sec_ = std::max(min_queue_time_sec_, queue_time_sec_ - 1);
                consecutive_congestion_ = 0;
                // Обновить baseline после коррекции (slow start reset)
                min_rtt_baseline_ms_ = sample.min_rtt_ms;
            }
        } else {
            consecutive_congestion_ = 0; // LAN или нейтрально — сброс счётчика
        }
        // else: нейтральная ситуация — оставить как есть

        // Обновить EWMA baseline RTT (медленное смягчение)
        if (!rtt_growing) {
            // Обновлять baseline только вниз — защита от ложного baseline роста
            min_rtt_baseline_ms_ = std::min(min_rtt_baseline_ms_,
                                            ewma(min_rtt_baseline_ms_, sample.min_rtt_ms, 0.1f));
        }

        last_delivery_rate_bps_ = sample.delivery_rate_bps;
        return queue_time_sec_;
    }

    // =========================================================================
    // Запрос состояния
    // =========================================================================

    int current_queue_time() const { return queue_time_sec_; }
    int baseline_rtt_ms()    const { return min_rtt_baseline_ms_; }
    uint32_t io_backpressure_count() const { return io_backpressure_count_; }

    /// Принудительный сброс при переключении стрима
    void reset() {
        queue_time_sec_          = kInitQueueTimeSec;
        min_rtt_baseline_ms_     = INT_MAX;
        last_delivery_rate_bps_  = 0;
        io_backpressure_count_   = 0;
        consecutive_congestion_  = 0;
        last_sample_at_          = {};
    }

private:
    // =========================================================================
    // Вспомогательные методы
    // =========================================================================

    CongestionSample collect_sample(const std::vector<lt::peer_info>& peers,
                                    int write_buffer_pct) {
        CongestionSample s;
        s.write_buffer_pct = write_buffer_pct;

        for (const auto& pi : peers) {
            if (pi.down_speed > 0) {
                ++s.active_peers;
                s.delivery_rate_bps += pi.down_speed;

                // peer_info::rtt в libtorrent 1.2 (тип int, миллисекунды)
                if (pi.rtt > 0 && pi.rtt < s.min_rtt_ms) {
                    s.min_rtt_ms = pi.rtt;
                }
            }
        }
        return s;
    }

    bool is_rtt_growing(int current_rtt_ms) const {
        if (min_rtt_baseline_ms_ == INT_MAX) return false;
        const int threshold = min_rtt_baseline_ms_ +
            (min_rtt_baseline_ms_ * kRttGrowthThresholdPct / 100);
        return current_rtt_ms > threshold;
    }

    static int ewma(int old_val, int new_val, float alpha) {
        return static_cast<int>(old_val * (1.0f - alpha) + new_val * alpha);
    }

    // =========================================================================
    // Состояние
    // =========================================================================

    int      queue_time_sec_;
    int      min_queue_time_sec_;
    int      max_queue_time_sec_;
    int      min_rtt_baseline_ms_;
    int      last_delivery_rate_bps_;
    int      consecutive_congestion_ = 0; ///< Подряд идущих RTT-роста
    uint32_t io_backpressure_count_ = 0;
    std::chrono::steady_clock::time_point last_sample_at_;
};

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
