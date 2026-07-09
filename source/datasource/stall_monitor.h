#pragma once
// =============================================================================
// StallMonitor — Мониторинг зависаний и отключение медленных пиров.
//
// Проблема: при стриминге пир может "держать" urgent кусок, но качать его
// со скоростью < 1 KB/s. libtorrent не отключит такого пира — он технически
// "качает". Мы должны обнаружить это и disconnect_peer() вручную.
//
// Вызывается из on_tick (раз в ~1 секунду).
// =============================================================================

#ifdef TSNX_USE_LIBTORRENT

#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/peer_info.hpp>

#include <chrono>
#include <vector>
#include <map>
#include <string>

namespace datasource {

namespace lt = libtorrent;

class StallMonitor {
public:
    StallMonitor() = default;

    /// Инициализация с торрент-хэндлом.
    void init(lt::torrent_handle handle, std::shared_ptr<lt::session> session);

    /// Сброс в исходное состояние (при смене торрента/файла).
    void reset();

    /// Основной метод — вызывается каждую секунду из tick-потока.
    ///
    /// Алгоритм:
    /// 1. Проверяем общую скорость загрузки
    /// 2. Если < kStallThresholdBps, увеличиваем счётчик stall_ticks_
    /// 3. Если stall_ticks_ >= kStallTicksBeforeAction:
    ///    a. Получаем список пиров через get_peer_info()
    ///    b. Находим пиров со скоростью < 5 KB/s
    ///    c. Среди них ищем тех, кто "висит" на urgent кусках
    ///    d. disconnect_peer() для таких пиров
    ///
    /// @param urgent_start  первый urgent кусок
    /// @param urgent_end    последний urgent кусок
    /// @param is_stalled_state  находится ли бэкенд в состоянии Stall
    /// @return количество отключённых пиров
    int on_tick(int urgent_start, int urgent_end, bool is_stalled_state = false);

    /// Возвращает true если скорость ниже порога >= kStallTicksBeforeAction тиков подряд.
    bool is_stalled() const;

    /// Сколько миллисекунд мы в состоянии stall (0 если не в stall).
    int stall_duration_ms() const;

    /// Текущая скорость загрузки (обновляется в on_tick).
    int download_rate_bps() const { return last_download_rate_; }

private:
    /// Проверяет, загружает ли пир один из urgent кусков.
    /// Смотрит downloading_piece_index в peer_info.
    bool is_peer_on_urgent_piece(const lt::peer_info& pi,
                                  int urgent_start, int urgent_end) const;

    lt::torrent_handle handle_;
    std::shared_ptr<lt::session> session_;
    bool initialized_ = false;

    int stall_ticks_ = 0;
    int last_download_rate_ = 0;
    std::chrono::steady_clock::time_point stall_started_{};
    std::chrono::steady_clock::time_point init_time_{};
    std::map<std::string, std::chrono::steady_clock::time_point> peer_first_seen_;

    // --- Конфигурация ---
    // Порог скорости, ниже которого считаем "stall" (50 KB/s)
    static constexpr int kStallThresholdBps = 50 * 1024;
    // Скорость пира, ниже которой считаем его "медленным" (10 KB/s)
    // Не ставить выше: при stall мгновенная скорость временно падает у хороших пиров
    static constexpr int kSlowPeerThresholdBps = 10 * 1024;
    // Сколько тиков подряд должен быть stall до начала отключения пиров (3 секунды)
    static constexpr int kStallTicksBeforeAction = 3;
    // Максимум пиров для отключения за один тик (увеличено для быстрого освобождения слотов)
    static constexpr int kMaxDisconnectsPerTick = 3;
    // Общий период прогрева после инициализации перед началом работы монитора (секунд)
    static constexpr int kWarmupGracePeriodSeconds = 15;
    // Индивидуальный период прогрева для пиров после их первого обнаружения (секунд)
    static constexpr int kMinPeerConnectionAgeSeconds = 10;
    // Порог возраста idle-пира (speed=0) для проактивного отключения (секунд)
    static constexpr int kIdlePeerMaxAgeSeconds = 20;
    // Минимальное количество пиров в рое: не отключаем, если останется меньше
    static constexpr int kMinSwarmPeers = 3;
};

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
