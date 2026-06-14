#pragma once
// =============================================================================
// lt_settings.h — Генерация lt::settings_pack для стримингового режима.
//
// Эти настройки оптимизированы для последовательного чтения одного файла
// (установка игр на Switch), а НЕ для обычного скачивания торрентов.
// =============================================================================

#ifdef TSNX_USE_LIBTORRENT

#include <libtorrent/settings_pack.hpp>
#include <string>

namespace datasource {

namespace lt = libtorrent;

/// Генерирует lt::settings_pack, оптимизированный для стриминга.
///
/// Ключевые решения:
/// - disable_os_cache для disk_io — меньше давления на RAM Switch'а
/// - max_out_request_queue = 1500 — агрессивный запрос кусков у пиров
/// - UPNP/NATPMP отключены — Switch за NAT, бесполезно
/// - DHT включён — нужен для трекерлесс магнетов
inline lt::settings_pack make_streaming_settings_pack(const std::string& bind_ip) {
    lt::settings_pack s;

    // --- Диск ---
    // disable_os_cache = 2 в libtorrent 1.2.x
    s.set_int(lt::settings_pack::disk_io_read_mode, 2);   // disable_os_cache
    s.set_int(lt::settings_pack::disk_io_write_mode, 2);   // disable_os_cache
    s.set_int(lt::settings_pack::aio_threads, 4);
    s.set_int(lt::settings_pack::max_queued_disk_bytes, 16 * 1024 * 1024);
    s.set_int(lt::settings_pack::cache_size, 1024);  // 16 MiB в блоках по 16 KiB

    // --- Сеть ---
    s.set_int(lt::settings_pack::max_out_request_queue, 500);
    s.set_int(lt::settings_pack::request_queue_time, 2);
    s.set_int(lt::settings_pack::max_allowed_in_request_queue, 9000);
    s.set_int(lt::settings_pack::connections_limit, 80);
    s.set_int(lt::settings_pack::connection_speed, 100);
    s.set_int(lt::settings_pack::request_timeout, 30);
    s.set_int(lt::settings_pack::peer_timeout, 90);
    s.set_int(lt::settings_pack::inactivity_timeout, 120);
    s.set_int(lt::settings_pack::tick_interval, 1000);
    s.set_bool(lt::settings_pack::prioritize_partial_pieces, true);

    // --- Лимиты ---
    s.set_int(lt::settings_pack::active_downloads, 2);
    s.set_int(lt::settings_pack::active_limit, 4);

    // --- UPnP/NAT — отключены для Switch ---
    s.set_bool(lt::settings_pack::enable_upnp, false);
    s.set_bool(lt::settings_pack::enable_natpmp, false);

    // --- DHT — оставляем включённым (трекерлесс магнеты) ---
    s.set_bool(lt::settings_pack::enable_dht, true);
    s.set_bool(lt::settings_pack::enable_lsd, false);

    // --- Протоколы ---
    s.set_bool(lt::settings_pack::enable_outgoing_utp, true);
    s.set_bool(lt::settings_pack::enable_incoming_utp, true);
    s.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
    s.set_bool(lt::settings_pack::enable_incoming_tcp, true);

    // --- Трекеры ---
    s.set_bool(lt::settings_pack::announce_to_all_tiers, true);
    s.set_bool(lt::settings_pack::announce_to_all_trackers, true);
    s.set_bool(lt::settings_pack::prefer_udp_trackers, true);

    // --- Алерты ---
    s.set_int(lt::settings_pack::alert_mask,
              (1 << 0)   // error_notification
              | (1 << 1) // peer_notification
              | (1 << 3) // performance_warning
              | (1 << 4) // tracker_notification
              | (1 << 5) // connect_notification
              | (1 << 6) // status_notification
              | (1 << 10) // dht_notification
              | (1 << 14) // piece_progress_notification
    );

    // --- Listen ---
    if (!bind_ip.empty()) {
        s.set_str(lt::settings_pack::listen_interfaces,
                  bind_ip + ":50575,0.0.0.0:50575");
    } else {
        s.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:50575");
    }
    s.set_str(lt::settings_pack::dht_bootstrap_nodes,
              "router.bittorrent.com:6881,"
              "dht.transmissionbt.com:6881,"
              "router.utorrent.com:6881");

    return s;
}

} // namespace datasource

#endif // TSNX_USE_LIBTORRENT
