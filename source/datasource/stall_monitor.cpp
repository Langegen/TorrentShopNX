#include "stall_monitor.h"

#ifdef TSNX_USE_LIBTORRENT

#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_handle.hpp>
#include <libtorrent/torrent.hpp>
#include <libtorrent/peer_connection.hpp>
#include <libtorrent/bt_peer_connection.hpp>
#include <libtorrent/error_code.hpp>
#include "../utils/log.h"

#include <algorithm>

namespace datasource {

void StallMonitor::init(lt::torrent_handle handle, std::shared_ptr<lt::session> session) {
    handle_ = std::move(handle);
    session_ = std::move(session);
    initialized_ = true;
    stall_ticks_ = 0;
    last_download_rate_ = 0;
    stall_started_ = {};
    init_time_ = std::chrono::steady_clock::now();
}

void StallMonitor::reset() {
    initialized_ = false;
    handle_ = {};
    session_ = nullptr;
    stall_ticks_ = 0;
    last_download_rate_ = 0;
    stall_started_ = {};
    init_time_ = {};
    peer_first_seen_.clear();
}

int StallMonitor::on_tick(int urgent_start, int urgent_end, bool is_stalled_state) {
    if (!initialized_ || !handle_.is_valid()) {
        return 0;
    }

    auto now_time = std::chrono::steady_clock::now();

    // Проверяем глобальный период прогрева после старта/инициализации
    auto elapsed_since_init = std::chrono::duration_cast<std::chrono::seconds>(now_time - init_time_).count();
    if (elapsed_since_init < kWarmupGracePeriodSeconds) {
        stall_ticks_ = 0;
        stall_started_ = {};
        peer_first_seen_.clear();
        return 0;
    }

    // 1. Получаем текущую скорость загрузки торрента
    auto status = handle_.status(lt::torrent_handle::query_accurate_download_counters);
    last_download_rate_ = status.download_rate;

    // 2. Проверяем порог stall
    if (is_stalled_state) {
        // Если явно находимся в состоянии Stall, форсируем stall_ticks_
        // чтобы незамедлительно начать проверку медленных пиров.
        if (stall_ticks_ < kStallTicksBeforeAction) {
            stall_ticks_ = kStallTicksBeforeAction;
        }
        if (stall_started_.time_since_epoch().count() == 0) {
            stall_started_ = now_time;
        }
    } else if (last_download_rate_ >= kStallThresholdBps) {
        // Скорость нормальная — сбрасываем счётчик
        stall_ticks_ = 0;
        stall_started_ = {};
        return 0;
    } else {
        // Скорость ниже порога — инкрементируем
        ++stall_ticks_;
        if (stall_started_.time_since_epoch().count() == 0) {
            stall_started_ = now_time;
        }
    }

    // 3. Ждём kStallTicksBeforeAction тиков перед действием
    if (stall_ticks_ < kStallTicksBeforeAction) {
        return 0;
    }

    // 5. Получаем список пиров и ищем медленных на urgent кусках
    std::vector<lt::peer_info> peers;
    handle_.get_peer_info(peers);

    // Обновляем карту времени первого обнаружения пиров
    std::map<std::string, std::chrono::steady_clock::time_point> new_seen;
    for (const auto& pi : peers) {
        std::string ip_port = pi.ip.address().to_string() + ":" + std::to_string(pi.ip.port());
        auto it = peer_first_seen_.find(ip_port);
        if (it != peer_first_seen_.end()) {
            new_seen[ip_port] = it->second;
        } else {
            new_seen[ip_port] = now_time;
        }
    }
    peer_first_seen_ = std::move(new_seen);

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

        // Новички имеют индивидуальный grace period
        std::string ip_port = pi.ip.address().to_string() + ":" + std::to_string(pi.ip.port());
        auto seen_it = peer_first_seen_.find(ip_port);
        if (seen_it != peer_first_seen_.end()) {
            auto elapsed_peer_sec = std::chrono::duration_cast<std::chrono::seconds>(now_time - seen_it->second).count();
            if (elapsed_peer_sec < kMinPeerConnectionAgeSeconds) {
                continue; // Не трогаем новичка
            }
        }

        // Медленный пир на urgent куске — отключаем
        util::logLine("stall_monitor: disconnecting slow peer " +
                      pi.ip.address().to_string() + ":" +
                      std::to_string(pi.ip.port()) +
                      " speed=" + std::to_string(pi.down_speed) + " B/s"
                      " pieces=" + std::to_string(pi.num_pieces));

        if (session_) {
            lt::torrent_handle h = handle_;
            lt::tcp::endpoint ep = pi.ip;
            session_->get_io_service().post([h, ep]() {
                if (h.is_valid()) {
                    auto t = h.native_handle();
                    if (t) {
                        auto* peer_conn = t->find_peer(ep);
                        if (peer_conn) {
                            peer_conn->disconnect(lt::error_code(lt::errors::optimistic_disconnect, lt::libtorrent_category()), lt::operation_t::unknown);
                        }
                    }
                }
            });
        }
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
