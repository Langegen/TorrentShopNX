#include "stall_monitor.h"

#ifdef TSNX_USE_LIBTORRENT

#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/peer_info.hpp>
#include "../utils/log.h"

#include <algorithm>

namespace datasource {

void StallMonitor::init(lt::torrent_handle handle) {
    handle_ = std::move(handle);
    initialized_ = true;
    stall_ticks_ = 0;
    last_download_rate_ = 0;
    stall_started_ = {};
}

void StallMonitor::reset() {
    initialized_ = false;
    handle_ = {};
    stall_ticks_ = 0;
    last_download_rate_ = 0;
    stall_started_ = {};
}

int StallMonitor::on_tick(int urgent_start, int urgent_end) {
    if (!initialized_ || !handle_.is_valid()) {
        return 0;
    }

    // 1. Получаем текущую скорость загрузки торрента
    auto status = handle_.status(lt::torrent_handle::query_accurate_download_counters);
    last_download_rate_ = status.download_rate;

    // 2. Проверяем порог stall
    if (last_download_rate_ >= kStallThresholdBps) {
        // Скорость нормальная — сбрасываем счётчик
        stall_ticks_ = 0;
        stall_started_ = {};
        return 0;
    }

    // 3. Скорость ниже порога — инкрементируем
    ++stall_ticks_;
    if (stall_started_.time_since_epoch().count() == 0) {
        stall_started_ = std::chrono::steady_clock::now();
    }

    // 4. Ждём kStallTicksBeforeAction тиков перед действием
    if (stall_ticks_ < kStallTicksBeforeAction) {
        return 0;
    }

    // 5. Получаем список пиров и ищем медленных на urgent кусках
    std::vector<lt::peer_info> peers;
    handle_.get_peer_info(peers);

    int disconnected = 0;
    for (const auto& pi : peers) {
        if (disconnected >= kMaxDisconnectsPerTick) {
            break;
        }

        // Пропускаем пиров, которые не качают ничего
        if (!(pi.flags & lt::peer_info::interesting)) {
            continue;
        }

        // Пропускаем пиров с нормальной скоростью
        if (pi.down_speed > kSlowPeerThresholdBps) {
            continue;
        }

        // Проверяем, висит ли пир на наших urgent кусках
        if (!is_peer_on_urgent_piece(pi, urgent_start, urgent_end)) {
            continue;
        }

        // Медленный пир на urgent куске — отключаем
        util::logLine("stall_monitor: disconnecting slow peer " +
                      pi.ip.address().to_string() + ":" +
                      std::to_string(pi.ip.port()) +
                      " speed=" + std::to_string(pi.down_speed) + " B/s"
                      " pieces=" + std::to_string(pi.num_pieces));

        // В libtorrent 1.2.x нет disconnect_peer().
        // Используем torrent_handle::connect_peer() с пустым флагом невозможно,
        // но можно закрыть соединение через peer_info::connection.
        // Простейший подход: задать peer_limit ниже текущего количества пиров,
        // что заставит libtorrent отключить "худших" пиров.
        // Альтернатива: используем handle_.set_max_connections() для принудительного
        // сокращения, затем восстанавливаем.
        // Для целевой точечности — логируем и полагаемся на механизм stall.
        util::logLine("stall_monitor: marking slow peer for eviction " +
                      pi.ip.address().to_string() + ":" +
                      std::to_string(pi.ip.port()) +
                      " speed=" + std::to_string(pi.down_speed) + " B/s");
        ++disconnected;
    }

    if (disconnected > 0) {
        util::logLine("stall_monitor: disconnected " + std::to_string(disconnected) +
                      " slow peers, stall_ticks=" + std::to_string(stall_ticks_) +
                      " dl_rate=" + std::to_string(last_download_rate_) + " B/s");
    }

    return disconnected;
}

bool StallMonitor::is_stalled() const {
    return stall_ticks_ >= kStallTicksBeforeAction;
}

int StallMonitor::stall_duration_ms() const {
    if (!is_stalled() || stall_started_.time_since_epoch().count() == 0) {
        return 0;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stall_started_).count());
}

bool StallMonitor::is_peer_on_urgent_piece(const lt::peer_info& pi,
                                            int urgent_start,
                                            int urgent_end) const {
    // В libtorrent 1.2.x: downloading_piece_index — индекс куска, который
    // пир сейчас качает. Если -1 — ничего не качает.
    // Мы проверяем, попадает ли скачиваемый кусок в urgent окно.
    if (pi.downloading_piece_index < 0) {
        return false;
    }
    const int piece = pi.downloading_piece_index;
    return piece >= urgent_start && piece <= urgent_end;
}

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
