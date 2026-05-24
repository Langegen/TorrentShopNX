#include "torrent_engine.h"

#include "../utils/log.h"
#include "../utils/string_utils.h"
#include "../buffer/piece_pool.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <deque>
#include <unordered_map>
#include <unordered_set>

#ifdef TSNX_USE_LIBTORRENT
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_status.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/time.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/storage.hpp>
#include <boost/shared_array.hpp>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#endif

namespace torrent {


#ifdef TSNX_USE_LIBTORRENT
namespace lt = libtorrent;
#endif


#ifdef __SWITCH__
// libtorrent 1.2.x on Switch treats "sdmc:/..." as a relative path and prefixes
// the current working directory, which produces invalid paths like
// "sdmc:/switch/sdmc:/switch/...". Use a cwd-relative cache root instead.
constexpr const char* kCacheRoot = "TorrentShopNX/cache/local_engine";
#else
constexpr const char* kCacheRoot = "sdmc:/switch/TorrentShopNX/cache/local_engine";
#endif
constexpr int kDefaultPort = 8080;
constexpr int kLibtorrentListenPort = 6881;
constexpr int kCacheBlocks16KiB = 0; // Disabled: we use MemoryStorage instead
constexpr int kConnectionsLimit = 150; // MUST match connections_limit in settings!
constexpr int kMetadataTimeoutMs = 300000; // 5 min
constexpr int kPieceWaitTimeoutMs = 45000;
constexpr int kPollSleepMs = 50; 
constexpr int kReadPieceReissueMs = 1000;
constexpr int64_t kSequentialReadaheadStallBytes = 16LL * 1024 * 1024;
constexpr int64_t kSequentialReadaheadTargetBytes = 0;
constexpr int64_t kSequentialRecoveryWindowBytes = 32LL * 1024 * 1024; // Increased from 16MB
constexpr int64_t kSequentialRecoveryHoldBytes = 48LL * 1024 * 1024; // Increased from 24MB
constexpr int kSequentialReadaheadMinPieces = 16;
constexpr int kSequentialReadaheadMaxPieces = 64; // Increased from 48
constexpr int kTorrestStyleReadAheadDivisor = 100;
constexpr size_t kHttpSequentialPrefetch = 8 * 1024 * 1024;
constexpr size_t kPreparedStreamReadChunk = 8 * 1024 * 1024;
constexpr size_t kPieceCacheEntries = 32; // Match NX2 (512MB RAM) for better system stability

#ifdef TSNX_USE_LIBTORRENT

constexpr lt::download_priority_t kNearReadaheadPiecePriority{3}; 
constexpr lt::download_priority_t kReadaheadPiecePriority{1}; 

// =============================================================================
// Глобальный PiecePool — предвыделённые буферы для MemoryStorage.
// Создаётся при первом вызове (lazy init) и переиспользуется всеми торрентами.
// =============================================================================
static std::unique_ptr<buffer::PiecePool> g_piece_pool;
static std::mutex                          g_piece_pool_mutex;

buffer::PiecePool* getOrCreatePiecePool(int piece_size) {
    std::lock_guard<std::mutex> lock(g_piece_pool_mutex);
    if (!g_piece_pool || g_piece_pool->piece_size() != piece_size) {
        // kPieceCacheEntries + kQueueDepth(8) буферов для запаса
        const int pool_size = kPieceCacheEntries + 8;
        g_piece_pool = buffer::PiecePool::create(piece_size, pool_size);
        util::logLine("torrent_engine: PiecePool created piece_size=" +
                      std::to_string(piece_size) + " capacity=" + std::to_string(pool_size));
    }
    return g_piece_pool.get();
}

#endif

struct LibtorrentLikeSettingsConfig {
    int aio_threads = 2; 
    int max_queued_disk_bytes = 32 * 1024 * 1024; 
    int disk_io_read_mode = 2; 
    int disk_io_write_mode = 2; 
    int request_timeout = 60; // v56: snub timer — bursty peers need 45+ s gap tolerance 
    int peer_timeout = 30; 
    int inactivity_timeout = 60; 
    int num_want = 200; 
    int max_out_request_queue = 1200; 
    int max_allowed_in_request_queue = 2400; 
    int request_queue_time = 2; 
    int whole_pieces_threshold = 20; 
    int half_open_limit = 50; 
    int connection_speed = 30; 
    int peer_connect_timeout = 15; 
    int torrent_connect_boost = 80; 
    int active_downloads = 30; 
    int active_limit = 100; 
    int connections_limit = 200; // Match NX2 swarm size (increased to 200)
    bool prioritize_partial_pieces = false; 
    bool use_parole_mode = true;
    bool strict_end_game_mode = false; // v60: посылаем запрос на последние куски ВСЕМ пирам, а не одному 
    bool rate_limit_utp = false;
    bool ignore_limits_on_local_network = true;
    bool outgoing_utp = true; 
    bool incoming_utp = true; 
};

struct DhtBootstrapNode {
    const char* host;
    int port;
};

const DhtBootstrapNode kDhtBootstrapNodes[] = {
    // Рабочие — подтверждено логом (все резолвились успешно)
    {"router.utorrent.com",    6881},
    {"router.bittorrent.com",  6881},
    {"dht.transmissionbt.com", 6881},
    {"dht.aelitis.com",        6881},
    {"dht.libtorrent.org",     25401},
    // Дополнительные актуальные узлы
    {"dht.opentrackr.org",     1337},
};

const char* kFallbackTrackers[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "http://37.120.182.83:80/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://tracker.openbittorrent.com:6969/announce",
    "udp://tracker.coppersurfer.tk:6969/announce",
    "udp://9.rarbg.to:2710/announce",
    "udp://tracker.leechers-paradise.org:6969/announce",
    "udp://tracker.cyberia.is:6969/announce",
    "udp://tracker.internetwarriors.net:1337/announce",
    "udp://p4p.arenabg.ch:1337/announce",
    "udp://tracker.skyts.net:6969/announce",
    "udp://retracker.lanta-net.ru:2710/announce"
};



void replaceAllInPlace(std::string& value, const std::string& from, const std::string& to) {
    if (from.empty()) return;

    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string normalizeMagnetLink(std::string magnet) {
    replaceAllInPlace(magnet, "&amp;", "&");
    replaceAllInPlace(magnet, "&#38;", "&");
    replaceAllInPlace(magnet, "\\u0026", "&");
    replaceAllInPlace(magnet, "\\u002F", "/");
    replaceAllInPlace(magnet, "\\/", "/");
    return magnet;
}

std::string urlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}


std::string dhtBootstrapNodesSetting() {
    std::string nodes;
    for (const auto& node : kDhtBootstrapNodes) {
        if (!nodes.empty()) nodes += ",";
        nodes += node.host;
        nodes += ":";
        nodes += std::to_string(node.port);
    }
    return nodes;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string resolveIPv4Address(const char* host, const char* service) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(host, service, &hints, &result) != 0 || result == nullptr) {
        if (result) {
            freeaddrinfo(result);
        }
        return {};
    }

    std::string ip;
    for (addrinfo* cur = result; cur != nullptr; cur = cur->ai_next) {
        if (cur->ai_family != AF_INET || cur->ai_addr == nullptr) {
            continue;
        }

        char buffer[INET_ADDRSTRLEN] = {};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(cur->ai_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer)) != nullptr) {
            ip = buffer;
            break;
        }
    }

    freeaddrinfo(result);
    return ip;
}

std::string detectPrimaryIPv4Address() {
    const int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return {};
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    if (::inet_pton(AF_INET, "1.1.1.1", &remote.sin_addr) != 1) {
        ::close(sock);
        return {};
    }

    if (::connect(sock, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) != 0) {
        ::close(sock);
        return {};
    }

    sockaddr_in local{};
    socklen_t local_len = sizeof(local);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &local_len) != 0) {
        ::close(sock);
        return {};
    }

    char buffer[INET_ADDRSTRLEN] = {};
    const char* result = ::inet_ntop(AF_INET, &local.sin_addr, buffer, sizeof(buffer));
    ::close(sock);
    if (result == nullptr) {
        return {};
    }

    const std::string ip = result;
    if (ip.empty() || ip == "0.0.0.0" || startsWith(ip, "127.")) {
        return {};
    }
    return ip;
}

#ifdef TSNX_USE_LIBTORRENT
lt::settings_pack make_torrserver_like_settings(const LibtorrentLikeSettingsConfig& cfg,
                                                const std::string& bind_ip) {
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::tick_interval, 200);
    settings.set_int(lt::settings_pack::cache_size, 512); // 8MB cache
    settings.set_int(lt::settings_pack::connections_limit, cfg.connections_limit);
    settings.set_int(lt::settings_pack::unchoke_slots_limit, 100);
    settings.set_int(lt::settings_pack::choking_algorithm, lt::settings_pack::fixed_slots_choker);
    settings.set_int(lt::settings_pack::connection_speed, cfg.connection_speed);
    settings.set_int(lt::settings_pack::peer_connect_timeout, cfg.peer_connect_timeout);
    settings.set_int(lt::settings_pack::request_timeout, cfg.request_timeout);
    settings.set_int(lt::settings_pack::peer_timeout, cfg.peer_timeout);
    settings.set_int(lt::settings_pack::inactivity_timeout, cfg.inactivity_timeout);
    settings.set_int(lt::settings_pack::max_out_request_queue, cfg.max_out_request_queue);
    settings.set_int(lt::settings_pack::max_allowed_in_request_queue, cfg.max_allowed_in_request_queue);
    settings.set_int(lt::settings_pack::half_open_limit, cfg.half_open_limit);
    settings.set_int(lt::settings_pack::torrent_connect_boost, cfg.torrent_connect_boost);
    // Tier 1: начальный request_queue_time = 5 с.
    // Логи показывают reqq=726-1400/935 при 15s — очередь переполнена, пиры захлёбываются.
    // 5s = баланс между латентностью и throughput (~3MB/s * 5s = ~15MB буфер).
    // CongestionController в LocalLibtorrentBackend дополнительно адаптирует в диапазоне 2-15s.
    settings.set_int(lt::settings_pack::request_queue_time, 5);
    settings.set_int(lt::settings_pack::piece_timeout, 60); // Increased: slow peers (RTT>300ms, 50KB/s) need ~20s per piece
    settings.set_int(lt::settings_pack::whole_pieces_threshold, 20);
    settings.set_bool(lt::settings_pack::prioritize_partial_pieces, false);
    settings.set_int(lt::settings_pack::piece_extent_affinity, 0);
    // Tier 1: suggest_read_cache — просим пиров кэшировать данные для нас
    settings.set_int(lt::settings_pack::suggest_mode, lt::settings_pack::suggest_read_cache);

    settings.set_int(lt::settings_pack::max_failcount, 3);
    settings.set_int(lt::settings_pack::active_downloads, cfg.active_downloads);
    settings.set_int(lt::settings_pack::active_limit, cfg.active_limit);
    settings.set_int(lt::settings_pack::max_peer_recv_buffer_size, 8 * 1024 * 1024); // Increased to 8MB
    settings.set_bool(lt::settings_pack::predictive_piece_announce, true);

    // Let OS handle per-socket buffer sizes. Setting explicit values exhausts
    // the 4MB socket memory pool on Switch and causes ENOBUFS errors.
    settings.set_int(lt::settings_pack::send_socket_buffer_size, 0);
    settings.set_int(lt::settings_pack::recv_socket_buffer_size, 0);
    settings.set_int(lt::settings_pack::send_buffer_low_watermark, 512 * 1024);
    settings.set_int(lt::settings_pack::send_buffer_watermark, 1024 * 1024);
    settings.set_int(lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::prefer_tcp); // TCP is more stable on Switch than UTP under load
    settings.set_int(lt::settings_pack::num_optimistic_unchoke_slots, 30); 
    settings.set_bool(lt::settings_pack::use_parole_mode, cfg.use_parole_mode);
    settings.set_bool(lt::settings_pack::low_prio_disk, false); // Performance is priority
    settings.set_bool(lt::settings_pack::coalesce_reads, false);
    settings.set_bool(lt::settings_pack::coalesce_writes, true); // Enabled for performance
    settings.set_bool(lt::settings_pack::strict_end_game_mode, cfg.strict_end_game_mode);
    settings.set_bool(lt::settings_pack::seeding_outgoing_connections, true);
#if TORRENT_ABI_VERSION == 1
    settings.set_bool(lt::settings_pack::rate_limit_utp, cfg.rate_limit_utp);
    settings.set_bool(lt::settings_pack::ignore_limits_on_local_network,
                      cfg.ignore_limits_on_local_network);
#else
    settings.set_bool(lt::settings_pack::deprecated_rate_limit_utp, cfg.rate_limit_utp);
    settings.set_bool(lt::settings_pack::deprecated_ignore_limits_on_local_network,
                      cfg.ignore_limits_on_local_network);
#endif
    settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    settings.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_enabled);
    settings.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_enabled);
    settings.set_int(lt::settings_pack::allowed_enc_level, lt::settings_pack::pe_both);

    settings.set_int(lt::settings_pack::alert_mask,
                     lt::alert::error_notification        // (1 << 0)
                     | lt::alert::storage_notification    // (1 << 3)
                     | lt::alert::status_notification     // (1 << 6)
                     | lt::alert::performance_warning     // (1 << 9)
                     | lt::alert::tracker_notification);  // (1 << 4)
    settings.set_int(lt::settings_pack::alert_queue_size, 4000);

    settings.set_bool(lt::settings_pack::enable_dht, true);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_outgoing_utp, cfg.outgoing_utp);
    settings.set_bool(lt::settings_pack::enable_incoming_utp, cfg.incoming_utp);
    settings.set_bool(lt::settings_pack::enable_outgoing_tcp, true);
    settings.set_bool(lt::settings_pack::enable_incoming_tcp, true);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_bool(lt::settings_pack::announce_to_all_tiers, true); 
    settings.set_bool(lt::settings_pack::announce_to_all_trackers, true); 
    settings.set_bool(lt::settings_pack::prefer_udp_trackers, false); 
    settings.set_int(lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::prefer_tcp);
    settings.set_int(lt::settings_pack::choking_algorithm, lt::settings_pack::fixed_slots_choker);

#ifdef __SWITCH__
    // On Switch, libtorrent expands 0.0.0.0 through interface enumeration.
    // That path can produce no endpoints, leaving the session non-listening and
    // DHT permanently at zero nodes. Bind the listen socket to the IPv4 address
    // we already detected with getsockname() instead.
    settings.set_str(lt::settings_pack::listen_interfaces,
                     bind_ip + ":" + std::to_string(kLibtorrentListenPort) +
                     ",0.0.0.0:" + std::to_string(kLibtorrentListenPort));
#else
    if (!bind_ip.empty()) {
        settings.set_str(lt::settings_pack::listen_interfaces,
                         bind_ip + ":" + std::to_string(kLibtorrentListenPort) +
                         ",0.0.0.0:" + std::to_string(kLibtorrentListenPort));
    } else {
        settings.set_str(lt::settings_pack::listen_interfaces,
                         "0.0.0.0:" + std::to_string(kLibtorrentListenPort));
    }
#endif
    settings.set_str(lt::settings_pack::dht_bootstrap_nodes, dhtBootstrapNodesSetting());
    return settings;
}

void log_applied_libtorrent_like_settings(const LibtorrentLikeSettingsConfig& cfg) {
    util::logLine("torrent_engine: [Build v62] settings aio_threads=" + std::to_string(cfg.aio_threads) +
                  " req_timeout=" + std::to_string(cfg.request_timeout) +
                  " conn_speed=" + std::to_string(cfg.connection_speed) +
                  " max_out_req=" + std::to_string(cfg.max_out_request_queue) +
                  " RAM_cache_limit=" + std::to_string(kPieceCacheEntries) +
                  " strict_end_game=" + std::string(cfg.strict_end_game_mode ? "true" : "false"));
}

// Адаптирует request_queue_time и whole_pieces_threshold под размер торрента.
// Маленькие торренты захлёбывались очередью запросов при глобальных настройках.
// Вызывается после получения torrent_info (metadata_received или .torrent файл).
void adapt_settings_for_torrent_size(lt::session& sess, int64_t total_size_bytes) {
    lt::settings_pack sp;

    if (total_size_bytes < 200LL * 1024 * 1024) {
        // < 200 МБ: маленький торрент — не захлёбываем пиров очередью запросов
        sp.set_int(lt::settings_pack::request_queue_time, 2);
        sp.set_int(lt::settings_pack::whole_pieces_threshold, 50);
        util::logLine("torrent_engine: [adapt] small torrent (<200MB): rqt=2 wpt=50");
    } else if (total_size_bytes < 2LL * 1024 * 1024 * 1024) {
        // 200 МБ – 2 ГБ: средний торрент
        sp.set_int(lt::settings_pack::request_queue_time, 4);
        sp.set_int(lt::settings_pack::whole_pieces_threshold, 30);
        util::logLine("torrent_engine: [adapt] medium torrent (200MB-2GB): rqt=4 wpt=30");
    } else {
        // > 2 ГБ: большой торрент (NSZ/NSP игры)
        sp.set_int(lt::settings_pack::request_queue_time, 5);
        sp.set_int(lt::settings_pack::whole_pieces_threshold, 20);
        util::logLine("torrent_engine: [adapt] large torrent (>2GB): rqt=5 wpt=20");
    }

    sess.apply_settings(sp);
}
#endif

void addFallbackTrackers(lt::add_torrent_params& atp) {
    for (const char* tracker : kFallbackTrackers) {
        if (std::find(atp.trackers.begin(), atp.trackers.end(), tracker) == atp.trackers.end()) {
            atp.trackers.push_back(tracker);
            atp.tracker_tiers.push_back(0);
        }
    }
}

void addResolvedDhtRouters(lt::session& session) {
    for (const auto& node : kDhtBootstrapNodes) {
        const std::string port = std::to_string(node.port);
        const std::string ip = resolveIPv4Address(node.host, port.c_str());
        if (ip.empty()) {
            util::logLine(std::string("torrent_engine: failed to resolve DHT router ") +
                          node.host + ":" + port);
            continue;
        }

        session.add_dht_router({ip, node.port});
        util::logLine(std::string("torrent_engine: DHT router ") +
                      node.host + ":" + port + " -> " + ip);
    }
}

std::string extractBtihHash(const std::string& magnet) {
    const std::string lower = util::toLowerCopy(magnet);
    const std::string marker = "xt=urn:btih:";
    auto pos = lower.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();

    auto end = lower.find('&', pos);
    if (end == std::string::npos) end = lower.size();
    if (end <= pos) return {};
    return lower.substr(pos, end - pos);
}

int installFilePriority(const std::string& name) {
    const std::string lower = util::toLowerCopy(name);
    if (lower.size() >= 4 && lower.rfind(".nsp") == lower.size() - 4) return 5;
    if (lower.size() >= 4 && lower.rfind(".nsz") == lower.size() - 4) return 4;
    if (lower.size() >= 4 && lower.rfind(".xci") == lower.size() - 4) return 3;
    if (lower.size() >= 4 && lower.rfind(".xcz") == lower.size() - 4) return 3;
    if (lower.size() >= 5 && lower.rfind(".pfs0") == lower.size() - 5) return 2;
    return 1;
}

int resolveFileIndex(int file_count, int external_index) {
    if (file_count <= 0) return -1;
    // TSNX Catalog uses 1-based indexing (1 = first file)
    if (external_index > 0 && external_index <= file_count) {
        return external_index - 1;
    }
    if (external_index >= 0 && external_index < file_count) {
        return external_index;
    }
    return 0;
}



#ifdef TSNX_USE_LIBTORRENT
struct PieceRange {
    int first = -1;
    int last = -1;
};

void discardMemoryStoragePiece(const std::string& hash, int piece_index);

struct TorrentRecord {
    std::mutex mutex;
    std::string hash;
    std::string magnet_link;
    std::string torrent_file_path;
    std::string name;
    std::filesystem::path save_path;
    lt::torrent_handle handle;
    mutable std::mutex pieces_mutex;
    std::condition_variable pieces_cv;
    std::vector<bool> wanted_files;
    std::chrono::steady_clock::time_point last_force_announce_at{};
    int last_dl_rate_bps = 0;    ///< Скорость на предыдущем тике (для детекции sudden drop)
    int last_peer_count  = 0;    ///< Кол-во пиров на предыдущем тике (для peers_just_dropped)
    // std::unordered_map<int, std::vector<char>> cached_pieces; // REMOVED: use MemoryStorage
    // std::deque<int> cached_piece_order; // REMOVED
    int active_file_index = -1;
    TorrentState state = TorrentState::Idle;
    std::set<int> verified_pieces;
    std::deque<lt::tcp::endpoint> recent_peer_endpoints;
    size_t reconnect_cursor = 0;
};

bool readMemoryStorageRange(const TorrentRecord& record,
                            const lt::torrent_info& ti,
                            int file_index,
                            uint64_t offset,
                            void* buf,
                            size_t size);

void handleGlobalErrors(const std::vector<lt::alert*>& alerts);
// Forward declaration: defined after TorrentEngine::Impl, called from alertThreadFunc
struct TorrentRecord;
int reconnectKnownPeersLocked(TorrentRecord& record, int max_attempts);
#endif


#ifdef TSNX_USE_LIBTORRENT
struct TorrentEngine::Impl {

    struct PreparedStream {
        bool open = false;
        std::string hash;
        std::shared_ptr<TorrentRecord> record;
        int file_index = -1;
        int resolved_file_index = -1;
        uint64_t file_size = 0;
    };
    std::mutex mutex;
    std::shared_ptr<lt::session> session;
    std::unordered_map<std::string, std::shared_ptr<TorrentRecord>> torrents;
    PreparedStream prepared_stream;
    TorrentEngineProbeStatus probe_status;
    bool probe_cancel_requested = false;
    std::filesystem::path cache_root = kCacheRoot;
    std::thread alert_thread;
    std::atomic<bool> alert_thread_running{false};

    void distributeAlertsLocked(std::vector<lt::alert*>& alerts) {
        for (lt::alert* alert : alerts) {
            if (auto* ta = lt::alert_cast<lt::torrent_alert>(alert)) {
                auto it = torrents.find(util::toHex(ta->handle.info_hash().to_string()));
                if (it != torrents.end()) {
                    std::lock_guard<std::mutex> rec_lock(it->second->mutex);
                    if (auto* pfa = lt::alert_cast<lt::piece_finished_alert>(alert)) {
                        const int piece_idx = static_cast<int>(pfa->piece_index);
                        {
                            std::lock_guard<std::mutex> pieces_lock(it->second->pieces_mutex);
                            it->second->verified_pieces.insert(piece_idx);
                        }
                        it->second->pieces_cv.notify_all();
                    } else if (auto* hfa = lt::alert_cast<lt::hash_failed_alert>(alert)) {
                        util::logLine("torrent_engine: HASH FAILED index=" + std::to_string(hfa->piece_index));
                        {
                            std::lock_guard<std::mutex> pieces_lock(it->second->pieces_mutex);
                            it->second->verified_pieces.erase(static_cast<int>(hfa->piece_index));
                        }
                        discardMemoryStoragePiece(util::toHex(hfa->handle.info_hash().to_string()), static_cast<int>(hfa->piece_index));
                    } else if (auto* mra = lt::alert_cast<lt::metadata_received_alert>(alert)) {
                        auto ti = mra->handle.torrent_file();
                        if (ti) {
                            // Pre-emptive lockdown: Disable ALL pieces immediately
                            int num_pieces = ti->num_pieces();
                            std::vector<lt::download_priority_t> priorities(num_pieces, lt::dont_download);
                            mra->handle.prioritize_pieces(priorities);
                            util::logLine("torrent_engine: metadata received, lockdown applied for " + std::to_string(num_pieces) + " pieces");
                            // Адаптировать настройки сессии под размер торрента (magnet-link)
                            int64_t total_sz = ti->total_size();
                            util::logLine("torrent_engine: metadata received, total_size=" + std::to_string(total_sz));
                            adapt_settings_for_torrent_size(*session, total_sz);
                        }
                    }
                }
            }

            // Update probe detail if active
            if (probe_status.active) {
                std::string detail;
                if (lt::alert_cast<lt::listen_failed_alert>(alert) != nullptr) {
                    detail = "listen failed: " + alert->message();
                } else if (lt::alert_cast<lt::listen_succeeded_alert>(alert) != nullptr) {
                    detail = "listen succeeded: " + alert->message();
                } else if (auto* sea = lt::alert_cast<lt::session_error_alert>(alert)) {
                    detail = "SESSION ERROR: " + sea->error.message();
                    util::logLine("torrent_engine: CRITICAL " + detail);
                } else if (lt::alert_cast<lt::tracker_error_alert>(alert) != nullptr) {
                    detail = "tracker error: " + alert->message();
                } else if (lt::alert_cast<lt::tracker_reply_alert>(alert) != nullptr) {
                    detail = "tracker reply: " + alert->message();
                } else if (lt::alert_cast<lt::metadata_received_alert>(alert) != nullptr) {
                    detail = "metadata received: " + alert->message();
                } else if (auto* pa = lt::alert_cast<lt::performance_alert>(alert)) {
                    util::logLine("torrent_engine: performance warning: " + pa->message());
                }
                
                if (!detail.empty()) {
                    probe_status.detail = detail;
                    util::logLine("torrent_engine: " + detail);
                }
            }
        }
    }



    void alertThreadFunc() {
        util::logLine("torrent_engine: alert thread started");
        int tick_counter = 0;
        while (alert_thread_running) {
            std::shared_ptr<lt::session> sess;
            {
                std::lock_guard<std::mutex> lock(mutex);
                sess = session;
            }
            if (sess) {
                sess->wait_for_alert(std::chrono::milliseconds(50));
            }

            std::vector<lt::alert*> alerts;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!session) break;
                session->pop_alerts(&alerts);
                if (!alerts.empty()) {
                    handleGlobalErrors(alerts);
                    distributeAlertsLocked(alerts);
                }

                // Build v51: Periodic Torrent Recovery (every ~1s)
                if (++tick_counter >= 20) {
                    tick_counter = 0;
                    for (auto& pair : torrents) {
                        auto& record = pair.second;
                        if (!record || !record->handle.is_valid()) continue;
                        
                        auto status = record->handle.status(lt::torrent_handle::query_accurate_download_counters);
                        const int cur_rate    = status.download_payload_rate;
                        const int cur_peers   = status.num_peers;

                        // Детекция немедленного обрыва: были пиры → стало 0
                        // (не ждём EWMA decay скорости, который занимает 4-5 секунд)
                        const bool peers_just_dropped = (record->last_peer_count > 0 && cur_peers == 0);
                        record->last_peer_count = cur_peers;

                        // Sudden speed drop: ≥500 KB/s → <50 KB/s (fast peer disconnected)
                        const bool sudden_drop = peers_just_dropped ||
                                                 (record->last_dl_rate_bps >= 500 * 1024 &&
                                                  cur_rate                  <  50 * 1024);
                        record->last_dl_rate_bps = cur_rate;

                        const bool no_peers   = (cur_peers == 0);
                        const bool stalled_dl = (status.state == lt::torrent_status::downloading &&
                                                 cur_rate < 50 * 1024);
                        const float progress  = status.progress;
                        const bool near_end   = (progress > 0.90f);

                        // Cooldown: 0 = немедленно (обрыв), 5 = нет пиров, 10 = конец, 30 = норма
                        int cooldown_sec;
                        if (sudden_drop || peers_just_dropped) {
                            cooldown_sec = 0;
                        } else if (no_peers) {
                            cooldown_sec = 5;
                        } else if (near_end) {
                            cooldown_sec = 10;
                        } else {
                            cooldown_sec = 30;
                        }

                        // Условие: no_peers — ANY state (обрыв бывает в любом состоянии).
                        //          stalled/sudden_drop — только при downloading.
                        const bool should_reannounce =
                            (no_peers) ||
                            ((stalled_dl || sudden_drop) &&
                             status.state == lt::torrent_status::downloading);

                        if (should_reannounce) {
                            auto now = std::chrono::steady_clock::now();
                            if (std::chrono::duration_cast<std::chrono::seconds>(
                                    now - record->last_force_announce_at).count() >= cooldown_sec) {
                                // ignore_min_interval: принудительно обойти ограничение трекера
                                record->handle.force_reannounce(
                                    0, -1, lt::torrent_handle::ignore_min_interval);
                                record->handle.force_dht_announce();
                                record->last_force_announce_at = now;

                                // При обрыве: сразу переподключить известные адреса пиров
                                if (no_peers || peers_just_dropped) {
                                    record->handle.resume(); // на случай если torrent был pause'd
                                    reconnectKnownPeersLocked(*record, 10);
                                }

                                const char* reason =
                                    peers_just_dropped ? "peers_dropped" :
                                    sudden_drop        ? "sudden_drop"   :
                                    no_peers           ? "no_peers"      :
                                    near_end           ? "near_end"      : "stalled";
                                util::logLine("torrent_engine: [v62] reannounce(" +
                                              std::string(reason) + "): peers=" +
                                              std::to_string(cur_peers) +
                                              " dl_rate=" + std::to_string(cur_rate / 1024) +
                                              "KB/s progress=" + std::to_string((int)(progress * 100)) +
                                              "% state=" + std::to_string((int)status.state) +
                                              " hash=" + record->hash);
                            }
                        }
                    }
                }
            }
        }
        util::logLine("torrent_engine: alert thread stopped");
    }
#else
    int unused = 0;
#endif
};

#ifdef TSNX_USE_LIBTORRENT

class MemoryStorage;

std::mutex g_memory_storage_mutex;
std::unordered_map<std::string, MemoryStorage*> g_memory_storages;

class MemoryStorage : public lt::storage_interface {
    std::map<lt::piece_index_t, char*> pieces_;
    std::map<lt::piece_index_t, std::vector<std::pair<int, int>>> written_ranges_;
    mutable std::deque<lt::piece_index_t> lru_order_;
    std::set<int> logged_write_pieces_;
    std::string info_hash_;
    int piece_size_;
    std::set<lt::piece_index_t> pinned_pieces_;
    uint64_t min_keep_offset_ = 0;
    mutable std::mutex mutex_;

public:
    explicit MemoryStorage(const lt::storage_params& params) 
        : lt::storage_interface(params.files)
        , info_hash_(util::toHex(params.info_hash.to_string()))
        , piece_size_(params.files.piece_length()) {
        std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
        g_memory_storages[info_hash_] = this;
    }

    void notifyPieceWritten() {
        // Find the record and notify its CV
        // This is called from store_piece equivalents
        std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
        // We don't have direct access to TorrentRecord here, 
        // but TorrentEngine::Impl will handle notifications via alerts.
    }

    void freePieceBuffer(char* ptr) {
        if (!ptr) return;
        buffer::PiecePool* pool = getOrCreatePiecePool(piece_size_);
        if (!pool || !pool->release(ptr)) {
            delete[] ptr;
        }
    }

    void clearAllPieces() {
        for (auto& pair : pieces_) {
            freePieceBuffer(pair.second);
        }
        pieces_.clear();
        written_ranges_.clear();
        lru_order_.clear();
    }

    ~MemoryStorage() override {
        clearAllPieces();
        std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
        auto it = g_memory_storages.find(info_hash_);
        if (it != g_memory_storages.end() && it->second == this) {
            g_memory_storages.erase(it);
        }
    }

    void initialize(lt::storage_error&) override {}

    bool isPieceAvailable(int piece_index, int offset, int size) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size < 0) size = piece_size_ - offset;
        return isRangeAvailableLocked(piece_index, offset, size);
    }

    void markRangeLocked(lt::piece_index_t piece, int offset, int size) {
        if (size <= 0) return;

        int begin = offset;
        int end = offset + size;
        auto& ranges = written_ranges_[piece];
        auto it = ranges.begin();

        while (it != ranges.end() && it->second < begin) {
            ++it;
        }
        if (it != ranges.begin()) {
            auto prev = it;
            --prev;
            if (prev->second >= begin) {
                it = prev;
            }
        }

        while (it != ranges.end() && it->first <= end) {
            begin = std::min(begin, it->first);
            end = std::max(end, it->second);
            it = ranges.erase(it);
        }
        ranges.insert(it, std::make_pair(begin, end));
    }

    void setPinnedPiecesLocked(const std::set<int>& pieces) {
        pinned_pieces_.clear();
        for (int p : pieces) {
            pinned_pieces_.insert(lt::piece_index_t(p));
        }
    }

    void setMinKeepOffsetLocked(uint64_t offset) {
        min_keep_offset_ = offset;
    }

    bool readRange(int first_piece, int piece_offset, void* dst, int size) const {
        std::lock_guard<std::mutex> lock(mutex_);

        // CRITICAL: We MUST check if the WHOLE range is actually written
        // before we start copying, otherwise we might read 0s from partially 
        // allocated pieces.
        if (!isRangeAvailableLocked(first_piece, piece_offset, size)) {
            return false;
        }

        char* out = static_cast<char*>(dst);
        int remaining = size;
        int current_piece = first_piece;
        int current_offset = piece_offset;

        while (remaining > 0) {
            auto it = pieces_.find(lt::piece_index_t(current_piece));
            if (it == pieces_.end()) return false;

            int to_copy = std::min(remaining, piece_size_ - current_offset);
            if (to_copy <= 0) break;

            std::memcpy(out, it->second + current_offset, static_cast<size_t>(to_copy));
            
            // Promotion for LRU: Move to the back of the queue (most recently used)
            // Match NX2 behavior to prevent eviction of recently read pieces
            lt::piece_index_t idx(current_piece);
            auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), idx);
            if (lru_it != lru_order_.end()) {
                lru_order_.erase(lru_it);
            }
            lru_order_.push_back(idx);

            out += to_copy;
            remaining -= to_copy;
            current_piece++;
            current_offset = 0;
        }
        return remaining == 0;
    }

    bool isRangeAvailableLocked(int first_piece, int piece_offset, int size) const {
        int remaining = size;
        int current_piece = first_piece;
        int current_offset = piece_offset;

        while (remaining > 0) {
            auto range_it = written_ranges_.find(lt::piece_index_t(current_piece));
            if (range_it == written_ranges_.end()) return false;

            int piece_needed = std::min(remaining, piece_size_ - current_offset);
            int piece_end = current_offset + piece_needed;
            
            int covered = current_offset;
            bool piece_ok = false;
            for (const auto& range : range_it->second) {
                if (range.second <= covered) continue;
                if (range.first > covered) break;
                covered = std::max(covered, range.second);
                if (covered >= piece_end) {
                    piece_ok = true;
                    break;
                }
            }
            if (!piece_ok) {
                return false;
            }

            remaining -= piece_needed;
            current_piece++;
            current_offset = 0;
        }
        return true;
    }

    bool isRangeAvailable(int piece, int offset, int size) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return isRangeAvailableLocked(piece, offset, size);
    }

    int availableContiguousBytes(int first_piece, int piece_offset, int max_size) const {
        if (max_size <= 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);

        int total_available = 0;
        int remaining_max = max_size;
        int current_piece = first_piece;
        int current_offset = piece_offset;

        while (remaining_max > 0) {
            auto range_it = written_ranges_.find(lt::piece_index_t(current_piece));
            if (range_it == written_ranges_.end()) break;

            const int piece_limit = std::min(remaining_max, piece_size_ - current_offset);
            const int end_in_piece = current_offset + piece_limit;
            
            int covered = current_offset;
            for (const auto& range : range_it->second) {
                if (range.second <= covered) continue;
                if (range.first > covered) break;
                covered = std::max(covered, std::min(range.second, end_in_piece));
                if (covered >= end_in_piece) break;
            }

            int available_in_piece = covered - current_offset;
            total_available += available_in_piece;

            if (available_in_piece < piece_limit) {
                // Gap found in this piece
                break;
            }

            remaining_max -= piece_limit;
            current_piece++;
            current_offset = 0;
        }

        return total_available;
    }

    int readv(lt::span<lt::iovec_t const> bufs, lt::piece_index_t piece, int offset, lt::open_mode_t flags, lt::storage_error& ec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pieces_.find(piece);
        if (it == pieces_.end()) return 0;
        
        auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), piece);
        if (lru_it != lru_order_.end()) lru_order_.erase(lru_it);
        lru_order_.push_back(piece);

        int copied = 0;
        for (const auto& buf : bufs) {
            const int to_copy = std::min<int>(static_cast<int>(buf.size()), piece_size_ - offset);
            if (to_copy <= 0) continue;
            
            std::memcpy(buf.data(), it->second + offset, to_copy);
            offset += to_copy;
            copied += to_copy;
        }
        return copied;
    }

    int writev(lt::span<lt::iovec_t const> bufs, lt::piece_index_t piece, int offset, lt::open_mode_t flags, lt::storage_error& ec) override {
        std::lock_guard<std::mutex> lock(mutex_);
        char* data_ptr = nullptr;
        auto it = pieces_.find(piece);
        if (it == pieces_.end()) {
            buffer::PiecePool* pool = getOrCreatePiecePool(piece_size_);
            data_ptr = pool ? pool->acquire() : nullptr;
            if (!data_ptr) {
                data_ptr = new (std::nothrow) char[piece_size_];
                if (!data_ptr) {
                    util::logLine("torrent_engine: OOM! failed to allocate piece " + std::to_string(static_cast<int>(piece)));
                    ec = lt::storage_error(lt::error_code(ENOMEM, lt::generic_category()));
                    return 0;
                }
            }
            std::memset(data_ptr, 0, piece_size_);
            pieces_[piece] = data_ptr;
        } else {
            data_ptr = it->second;
        }

        // Track first write per piece (offset == 0 means piece just started downloading)
        bool first_write = (offset == 0 && bufs.size() > 0);

        int copied = 0;
        for (const auto& buf : bufs) {
            const int to_copy = std::min<int>(static_cast<int>(buf.size()), piece_size_ - offset);
            if (to_copy <= 0) continue;
            
            std::memcpy(data_ptr + offset, buf.data(), to_copy);
            markRangeLocked(piece, offset, to_copy);
             
            offset += to_copy;
            copied += to_copy;
        }

        // Log only once when a piece starts receiving data (first write at offset 0)
        if (first_write) {
            util::logLine("torrent_engine: storage piece " +
                           std::to_string(static_cast<int>(piece)) +
                           " started, total_in_ram=" + std::to_string(pieces_.size()));
        }
        
        auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), piece);
        if (lru_it != lru_order_.end()) lru_order_.erase(lru_it);
        lru_order_.push_back(piece);

        // Limit RAM usage: Reduce to 24-32 pieces to prevent OOM with NCZ buffers
        while (pieces_.size() > kPieceCacheEntries && !lru_order_.empty()) {
            bool evicted = false;
            for (auto it = lru_order_.begin(); it != lru_order_.end(); ++it) {
                const lt::piece_index_t p = *it;
                
                // Safety: Don't evict if pinned (piece 0 is ALWAYS pinned) OR if it contains data at/after min_keep_offset_
                bool is_pinned = (pinned_pieces_.find(p) != pinned_pieces_.end()) || (static_cast<int>(p) == 0);
                bool is_needed = (static_cast<uint64_t>(static_cast<int>(p)) * piece_size_ + piece_size_ > min_keep_offset_);

                if (!is_pinned && !is_needed) {
                    lru_order_.erase(it);
                    auto pit = pieces_.find(p);
                    if (pit != pieces_.end()) {
                        freePieceBuffer(pit->second);
                        pieces_.erase(pit);
                    }
                    written_ranges_.erase(p);
                    evicted = true;
                    break;
                }
            }
            if (!evicted) break; // All remaining pieces are either pinned or needed
        }
        return copied;
    }


    bool has_any_file(lt::storage_error&) override { return false; }
    void set_file_priority(lt::aux::vector<lt::download_priority_t, lt::file_index_t>&, lt::storage_error&) override {}
    lt::status_t move_storage(std::string const&, lt::move_flags_t, lt::storage_error&) override { return lt::status_t::no_error; }
    bool verify_resume_data(lt::add_torrent_params const&, lt::aux::vector<std::string, lt::file_index_t> const&, lt::storage_error&) override { return false; }
    void release_files(lt::storage_error&) override { std::lock_guard<std::mutex> lock(mutex_); clearAllPieces(); }
    void rename_file(lt::file_index_t, std::string const&, lt::storage_error&) override {}
    void delete_files(lt::remove_flags_t, lt::storage_error&) override { std::lock_guard<std::mutex> lock(mutex_); clearAllPieces(); }

    void discardPiece(int piece_index) {
        std::lock_guard<std::mutex> lock(mutex_);
        lt::piece_index_t p(piece_index);
        auto pit = pieces_.find(p);
        if (pit != pieces_.end()) {
            freePieceBuffer(pit->second);
            pieces_.erase(pit);
        }
        written_ranges_.erase(p);
        auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), p);
        if (lru_it != lru_order_.end()) lru_order_.erase(lru_it);
    }
};

lt::storage_interface* memory_storage_constructor(lt::storage_params const& params, lt::file_pool&) {
    return new MemoryStorage(params);
}

MemoryStorage* findMemoryStorageLocked(const std::string& hash) {
    auto storage_it = g_memory_storages.find(hash);
    if (storage_it != g_memory_storages.end()) {
        return storage_it->second;
    }

    if (g_memory_storages.size() == 1) {
        return g_memory_storages.begin()->second;
    }
    return nullptr;
}

void discardMemoryStoragePiece(const std::string& hash, int piece_index) {
    std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
    MemoryStorage* storage = findMemoryStorageLocked(hash);
    if (storage) {
        storage->discardPiece(piece_index);
    }
}

bool memoryStorageRangeAvailable(const TorrentRecord& record,
                                 const lt::torrent_info& ti,
                                 int file_index,
                                 uint64_t offset,
                                 size_t size) {
    if (size == 0) return true;
    const auto& fs = ti.files();
    const uint64_t piece_len = static_cast<uint64_t>(ti.piece_length());
    if (piece_len == 0 || file_index < 0 || file_index >= fs.num_files()) {
        return false;
    }

    std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
    MemoryStorage* storage = findMemoryStorageLocked(record.hash);
    if (storage == nullptr) {
        return false;
    }

    uint64_t global = static_cast<uint64_t>(fs.file_offset(file_index)) + offset;
    const int start_piece = static_cast<int>(global / piece_len);
    const int piece_offset = static_cast<int>(global % piece_len);
    return storage->isRangeAvailable(start_piece, piece_offset, static_cast<int>(size));
}

bool readMemoryStorageRange(const TorrentRecord& record,
                            const lt::torrent_info& ti,
                            int file_index,
                            uint64_t offset,
                            void* buf,
                            size_t size) {
    if (!buf || size == 0) return size == 0;
    const auto& fs = ti.files();
    const uint64_t piece_len = static_cast<uint64_t>(ti.piece_length());
    if (piece_len == 0 || file_index < 0 || file_index >= fs.num_files()) {
        return false;
    }

    std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
    MemoryStorage* storage = findMemoryStorageLocked(record.hash);
    if (storage == nullptr) {
        return false;
    }

    uint64_t global = static_cast<uint64_t>(fs.file_offset(file_index)) + offset;
    int start_piece = static_cast<int>(global / piece_len);
    int piece_offset = static_cast<int>(global % piece_len);

    if (storage->readRange(start_piece, piece_offset, buf, static_cast<int>(size))) {
        return true;
    }
    return false;
}

size_t memoryStorageAvailableBytes(const TorrentRecord& record,
                                   const lt::torrent_info& ti,
                                   int file_index,
                                   uint64_t offset,
                                   size_t size) {
    if (size == 0) return 0;
    const auto& fs = ti.files();
    const uint64_t piece_len = static_cast<uint64_t>(ti.piece_length());
    if (piece_len == 0 || file_index < 0 || file_index >= fs.num_files()) {
        return 0;
    }

    std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
    MemoryStorage* storage = findMemoryStorageLocked(record.hash);
    if (storage == nullptr) {
        return 0;
    }

    uint64_t global = static_cast<uint64_t>(fs.file_offset(file_index)) + offset;
    size_t remaining = size;
    size_t available = 0;

    while (remaining > 0) {
        const int piece = static_cast<int>(global / piece_len);
        const int piece_offset = static_cast<int>(global % piece_len);
        const size_t part_limit = static_cast<size_t>(
            std::min<uint64_t>(static_cast<uint64_t>(remaining), piece_len - piece_offset));
        const int piece_available = storage->availableContiguousBytes(
            piece, piece_offset, static_cast<int>(part_limit));
        if (piece_available <= 0) {
            break;
        }

        available += static_cast<size_t>(piece_available);
        remaining -= static_cast<size_t>(piece_available);
        global += static_cast<uint64_t>(piece_available);

        if (static_cast<size_t>(piece_available) < part_limit) {
            break;
        }
    }

    return available;
}

void cachePieceBufferLocked(TorrentRecord& record,
                            int piece,
                            const char* data,
                            size_t size);
bool waitForMetadataLocked(TorrentEngine::Impl& impl,
                           TorrentRecord& record,
                           std::string& error_out,
                           std::unique_lock<std::mutex>& lock);
bool applyFilePrioritiesLocked(TorrentEngine::Impl& impl,
                               TorrentRecord& record,
                               int preferred_file_index,
                               std::string& error_out,
                               std::unique_lock<std::mutex>& lock);
bool resolveFileAccessLocked(TorrentEngine::Impl& impl,
                            TorrentRecord& record,
                            int external_file_index,
                            int& resolved_file_index,
                            uint64_t* out_file_size,
                            std::string& error_out,
                            std::unique_lock<std::mutex>& lock);
int chooseDefaultFileIndexLocked(TorrentEngine::Impl& impl,
                                 TorrentRecord& record,
                                 std::string& error_out,
                                 std::unique_lock<std::mutex>& lock);
bool hasCachedPieceDataLocked(const TorrentRecord& record,
                              int piece);
const char* torrentStateName(TorrentState state);
void transitionTorrentStateLocked(TorrentRecord& record,
                                  TorrentState new_state,
                                  int file_index);
void rememberPeerEndpointFromAlertLocked(TorrentEngine::Impl& impl,
                                         const lt::peer_alert& peer_event);
int reconnectKnownPeersLocked(TorrentRecord& record, int max_attempts);
int effectiveConnectedPeerCount(const lt::torrent_handle& handle, int status_num_peers);

void handleGlobalErrors(const std::vector<lt::alert*>& alerts) {
    static int io_recovery_count = 0;
    static constexpr int kMaxRecoveries = 20;

    for (lt::alert* alert : alerts) {
        if (auto* te = lt::alert_cast<lt::torrent_error_alert>(alert)) {
            if (io_recovery_count < kMaxRecoveries) {
                util::logLine("torrent_engine: torrent error (auto-recovering " +
                              std::to_string(io_recovery_count) + "): " + alert->message());
                if (te->handle.is_valid()) {
                    te->handle.clear_error();
                    te->handle.resume();
                }
                ++io_recovery_count;
            }
        } else if (auto* fe = lt::alert_cast<lt::file_error_alert>(alert)) {
            if (io_recovery_count < kMaxRecoveries) {
                std::string filepath(fe->filename());
                
                // Фикс: std::filesystem на Switch ломается на "sdmc:/"
                std::string fs_path = filepath;
                if (fs_path.find("sdmc:/") == 0) {
                    fs_path = fs_path.substr(5); // оставляем "/switch/..."
                }

                auto parent = std::filesystem::path(fs_path).parent_path();
                util::logLine("torrent_engine: file error, creating dirs: " + parent.string());
                
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    util::logLine("torrent_engine: mkdir failed: " + ec.message());
                } else {
                    util::logLine("torrent_engine: mkdir OK");
                }

                if (fe->handle.is_valid()) {
                    fe->handle.clear_error();
                    fe->handle.resume();
                }
                ++io_recovery_count;
            }
        }
    }
}

bool ensureSessionLocked(TorrentEngine::Impl& impl, std::string& error_out) {
    if (impl.session) return true;

    std::error_code dir_ec;
    std::filesystem::create_directories(impl.cache_root, dir_ec);
    const std::string bind_ip = detectPrimaryIPv4Address();
#ifdef __SWITCH__
    if (bind_ip.empty()) {
        error_out = "failed to detect local IPv4 for libtorrent listen";
        util::logLine("torrent_engine: " + error_out);
        return false;
    }
#endif

    const LibtorrentLikeSettingsConfig tuning_cfg{};
    lt::settings_pack settings = make_torrserver_like_settings(tuning_cfg, bind_ip);

    try {
        const auto flags = lt::session::start_default_features | lt::session::add_default_plugins;
        impl.session = std::make_shared<lt::session>(settings, flags);
    } catch (const std::exception& e) {
        error_out = std::string("failed to start libtorrent session: ") + e.what();
        return false;
    }

    addResolvedDhtRouters(*impl.session);
    impl.session->start_dht();
    util::logLine("torrent_engine: detected local IP " + bind_ip);
    util::logLine("torrent_engine: listen interface " + bind_ip + ":" +
                  std::to_string(kLibtorrentListenPort));
    log_applied_libtorrent_like_settings(tuning_cfg);
    util::logLine("torrent_engine: libtorrent session started (cache=" +
                  std::to_string((kCacheBlocks16KiB * 16) / 1024) + "MiB, connections=" +
                  std::to_string(kConnectionsLimit) + ")");
    return true;
}

void updateProbeStatusLocked(TorrentEngine::Impl& impl,
                             const std::string& phase,
                             const std::string& hash,
                             int peers,
                             int seeds,
                             bool has_metadata,
                             int known_peers = 0,
                             int connect_candidates = 0,
                             int torrent_state = 0) {
    if (!impl.probe_status.active) return;

    impl.probe_status.phase = phase;

    if (!hash.empty()) {
        impl.probe_status.hash = hash;
    }
    impl.probe_status.peers = peers;
    impl.probe_status.seeds = seeds;
    impl.probe_status.has_metadata = has_metadata;
    impl.probe_status.known_peers = known_peers;
    impl.probe_status.connect_candidates = connect_candidates;
    impl.probe_status.torrent_state = torrent_state;
    if (impl.session) {
        auto session_status = impl.session->status();
        impl.probe_status.dht_nodes = session_status.dht_nodes;
        impl.probe_status.session_listening = impl.session->is_listening();
        impl.probe_status.listen_port = static_cast<int>(impl.session->listen_port());
        if (!impl.probe_status.session_listening && impl.probe_status.detail.empty()) {
            impl.probe_status.detail = "libtorrent session is not listening";
        }
    }
}

bool waitForMetadataLocked(TorrentEngine::Impl& impl,
                           TorrentRecord& record,
                           std::string& error_out,
                           std::unique_lock<std::mutex>& lock) {
    if (!record.handle.is_valid()) {
        transitionTorrentStateLocked(record, TorrentState::Error, -1);
        error_out = "torrent handle is invalid";
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMetadataTimeoutMs);
    auto next_log_at = std::chrono::steady_clock::now();
    auto next_reannounce_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (impl.probe_status.active && impl.probe_cancel_requested) {
            updateProbeStatusLocked(impl, "cancelled", record.hash, 0, 0, false);
            error_out = "cancelled";
            return false;
        }

        auto status = record.handle.status(lt::torrent_handle::query_accurate_download_counters);
        if (status.has_metadata) {
            if (!status.name.empty()) {
                record.name = status.name;
            }
            auto ti = record.handle.torrent_file();
            if (ti && record.wanted_files.size() != static_cast<size_t>(ti->files().num_files())) {
                record.wanted_files.assign(static_cast<size_t>(ti->files().num_files()), true);
            }
            updateProbeStatusLocked(impl, "metadata received", record.hash,
                                    status.num_peers, status.num_seeds, true,
                                    status.list_peers, status.connect_candidates, static_cast<int>(status.state));
            return true;
        }
        transitionTorrentStateLocked(record, TorrentState::FetchingMetadata, -1);
        updateProbeStatusLocked(impl, "waiting for metadata", record.hash,
                                status.num_peers, status.num_seeds, false,
                                status.list_peers, status.connect_candidates, static_cast<int>(status.state));
        const auto now = std::chrono::steady_clock::now();
        const int connected_peers = effectiveConnectedPeerCount(record.handle, status.num_peers);
        if (connected_peers == 0 &&
            (status.list_peers > 0 || status.connect_candidates > 0) &&
            now >= next_reannounce_at) {
            const int reconnect_attempts = reconnectKnownPeersLocked(record, 8);
            record.handle.resume();
            record.handle.force_reannounce(0, -1, lt::torrent_handle::ignore_min_interval);
            record.handle.force_dht_announce();
            record.last_force_announce_at = now;
            next_reannounce_at = now + std::chrono::seconds(3);
            util::logLine("torrent_engine: metadata reconnect hash=" + record.hash +
                          " peers=" + std::to_string(connected_peers) +
                          " known=" + std::to_string(status.list_peers) +
                          " candidates=" + std::to_string(status.connect_candidates) +
                          " reconnect_attempts=" + std::to_string(reconnect_attempts));
        } else if (now >= next_reannounce_at) {
            record.handle.resume();
            record.handle.force_reannounce(0, -1, lt::torrent_handle::ignore_min_interval);
            record.handle.force_dht_announce();
            record.last_force_announce_at = now;
            next_reannounce_at = now + std::chrono::seconds(6);
        }
        if (now >= next_log_at) {
            int dht_nodes = -1;
            bool listening = false;
            int listen_port = 0;
            if (impl.session) {
                auto session_status = impl.session->status();
                dht_nodes = session_status.dht_nodes;
                listening = impl.session->is_listening();
                listen_port = static_cast<int>(impl.session->listen_port());
            }
            util::logLine("torrent_engine: metadata wait hash=" + record.hash +
                          " state=" + std::to_string(static_cast<int>(status.state)) +
                          " peers=" + std::to_string(status.num_peers) +
                          " seeds=" + std::to_string(status.num_seeds) +
                          " known=" + std::to_string(status.list_peers) +
                          " candidates=" + std::to_string(status.connect_candidates) +
                          " dht_nodes=" + std::to_string(dht_nodes) +
                          " listening=" + std::string(listening ? "yes" : "no") +
                          " port=" + std::to_string(listen_port));
            next_log_at = now + std::chrono::seconds(5);
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollSleepMs));
        lock.lock();
    }

    updateProbeStatusLocked(impl, "metadata timeout", record.hash, 0, 0, false);
    transitionTorrentStateLocked(record, TorrentState::Error, -1);
    error_out = "timeout waiting for torrent metadata";
    return false;
}

void removeTorrentLocked(TorrentEngine::Impl& impl,
                         std::unordered_map<std::string, std::shared_ptr<TorrentRecord>>::iterator it) {
    if (it == impl.torrents.end()) return;

    if (impl.session && it->second && it->second->handle.is_valid()) {
        impl.session->remove_torrent(it->second->handle, lt::session::delete_files | lt::session::delete_partfile);
    }
    impl.torrents.erase(it);
}

bool applyFilePrioritiesLocked(TorrentEngine::Impl& impl,
                               TorrentRecord& record,
                               int preferred_file_index,
                               std::string& error_out,
                               std::unique_lock<std::mutex>& lock) {
    if (!waitForMetadataLocked(impl, record, error_out, lock)) {
        return false;
    }

    auto ti = record.handle.torrent_file();
    if (!ti) {
        error_out = "torrent metadata is unavailable";
        return false;
    }

    const int file_count = ti->files().num_files();
    if (record.wanted_files.size() != static_cast<size_t>(file_count)) {
        record.wanted_files.assign(static_cast<size_t>(file_count), true);
    }

    std::vector<lt::download_priority_t> priorities(static_cast<size_t>(file_count), lt::dont_download);
    bool any_wanted = false;
    for (size_t i = 0; i < record.wanted_files.size(); ++i) {
        if (record.wanted_files[i]) {
            priorities[i] = lt::download_priority_t(1);
            any_wanted = true;
        }
    }

    if (!any_wanted) {
        priorities.assign(static_cast<size_t>(file_count), lt::download_priority_t(1));
    }

    if (preferred_file_index >= 0 && preferred_file_index < file_count) {
        for (int i = 0; i < file_count; ++i) {
            priorities[static_cast<size_t>(i)] = (i == preferred_file_index) ? lt::download_priority_t(1) : lt::dont_download;
        }
    }

    const bool same_active_file =
        preferred_file_index >= 0 &&
        record.active_file_index == preferred_file_index;
    const bool keep_streaming_state =
        record.state == TorrentState::PrebufferInstallInfo ||
        record.state == TorrentState::InstallInfoParsed ||
        record.state == TorrentState::MainWindowBuffering ||
        record.state == TorrentState::Installing ||
        record.state == TorrentState::Stalled;

    record.handle.prioritize_files(priorities);
    record.handle.set_max_connections(kConnectionsLimit);
    record.handle.resume();
    if (preferred_file_index >= 0) {
        record.active_file_index = preferred_file_index;
    }
    if (!(same_active_file && keep_streaming_state)) {
        transitionTorrentStateLocked(record, TorrentState::FileSelected,
                                     preferred_file_index >= 0 ? preferred_file_index : -1);
    }
    return true;
}

bool resolveFileAccessLocked(TorrentEngine::Impl& impl,
                            TorrentRecord& record,
                            int external_file_index,
                            int& resolved_file_index,
                            uint64_t* out_file_size,
                            std::string& error_out,
                            std::unique_lock<std::mutex>& lock) {
    if (external_file_index < 0) {
        external_file_index = chooseDefaultFileIndexLocked(impl, record, error_out, lock);
        if (external_file_index < 0) {
            return false;
        }
    }
    auto ti = record.handle.torrent_file();
    if (!ti) {
        if (!waitForMetadataLocked(impl, record, error_out, lock)) {
            return false;
        }
        ti = record.handle.torrent_file();
    }
    if (!ti) {
        error_out = "torrent metadata is unavailable";
        return false;
    }

    resolved_file_index = resolveFileIndex(ti->files().num_files(), external_file_index);
    if (!applyFilePrioritiesLocked(impl, record, resolved_file_index, error_out, lock)) {
        return false;
    }

    if (out_file_size) {
        *out_file_size = static_cast<uint64_t>(ti->files().file_size(resolved_file_index));
    }
    return true;
}

// REMOVED cachePieceBufferLocked

bool hasCachedPieceDataLocked(const TorrentRecord& record,
                               int piece) {
    std::lock_guard<std::mutex> registry_lock(g_memory_storage_mutex);
    MemoryStorage* storage = findMemoryStorageLocked(record.hash);
    if (storage == nullptr) {
        return false;
    }
    // Fix: Accurate piece length check including the last piece
    auto ti = record.handle.torrent_file();
    if (!ti) return false;
    const int expected_size = (piece == ti->num_pieces() - 1) 
        ? static_cast<int>(ti->total_size() % ti->piece_length()) 
        : ti->piece_length();
    return storage->isRangeAvailable(piece, 0, expected_size == 0 ? ti->piece_length() : expected_size);
}



const char* torrentStateName(TorrentState state) {
    switch (state) {
        case TorrentState::Idle: return "Idle";
        case TorrentState::FetchingMetadata: return "FetchingMetadata";
        case TorrentState::FileSelected: return "FileSelected";
        case TorrentState::PrebufferInstallInfo: return "PrebufferInstallInfo";
        case TorrentState::InstallInfoParsed: return "InstallInfoParsed";
        case TorrentState::MainWindowBuffering: return "MainWindowBuffering";
        case TorrentState::Installing: return "Installing";
        case TorrentState::Stalled: return "Stalled";
        case TorrentState::Completed: return "Completed";
        case TorrentState::Error: return "Error";
    }
    return "Unknown";
}

void transitionTorrentStateLocked(TorrentRecord& record,
                                  TorrentState new_state,
                                  int file_index) {
    if (record.state == new_state) {
        return;
    }
    util::logLine("torrent_engine: state " + std::string(torrentStateName(record.state)) +
                  " -> " + std::string(torrentStateName(new_state)) +
                  " hash=" + record.hash +
                  " file_index=" + std::to_string(file_index));
    record.state = new_state;
}

bool shouldEmitPieceLog(int piece,
                        int& last_piece,
                        std::chrono::steady_clock::time_point& last_at,
                        std::chrono::milliseconds throttle = std::chrono::milliseconds(3000)) {
    const auto now = std::chrono::steady_clock::now();
    if (last_piece != piece ||
        last_at.time_since_epoch().count() == 0 ||
        now - last_at >= throttle) {
        last_piece = piece;
        last_at = now;
        return true;
    }
    return false;
}

void rememberPeerEndpointLocked(TorrentRecord& record,
                                const lt::tcp::endpoint& endpoint) {
    if (endpoint.port() == 0) {
        return;
    }
    lt::error_code ec;
    if (endpoint.address().is_unspecified() || endpoint.address().is_multicast()) {
        return;
    }
    const std::string host = endpoint.address().to_string(ec);
    if (ec || host.empty()) {
        return;
    }

    for (const auto& known_endpoint : record.recent_peer_endpoints) {
        if (known_endpoint == endpoint) {
            return;
        }
    }

    record.recent_peer_endpoints.push_back(endpoint);
    constexpr size_t kMaxTrackedPeers = 64;
    while (record.recent_peer_endpoints.size() > kMaxTrackedPeers) {
        record.recent_peer_endpoints.pop_front();
    }
}

void rememberPeerEndpointFromAlertLocked(TorrentEngine::Impl& impl,
                                         const lt::peer_alert& peer_event) {
    for (auto& entry : impl.torrents) {
        auto& record = entry.second;
        if (!record || record->handle != peer_event.handle) {
            continue;
        }
        std::lock_guard<std::mutex> record_lock(record->mutex);
        rememberPeerEndpointLocked(*record, peer_event.endpoint);
        return;
    }
}

int reconnectKnownPeersLocked(TorrentRecord& record, int max_attempts) {
    if (max_attempts <= 0 || record.recent_peer_endpoints.empty()) {
        return 0;
    }

    int attempted = 0;
    const size_t total = record.recent_peer_endpoints.size();
    for (int i = 0; i < max_attempts && i < static_cast<int>(total); ++i) {
        const size_t cursor = (record.reconnect_cursor + static_cast<size_t>(i)) % total;
        const auto endpoint = record.recent_peer_endpoints[cursor];
        try {
            record.handle.connect_peer(endpoint);
            ++attempted;
        } catch (...) {
            // Ignore stale endpoints and continue with next candidate.
        }
    }

    record.reconnect_cursor = (record.reconnect_cursor + static_cast<size_t>(attempted)) % total;
    return attempted;
}

void rememberConnectedPeerInfosLocked(TorrentRecord& record) {
    std::vector<lt::peer_info> peers;
    try {
        record.handle.get_peer_info(peers);
    } catch (...) {
        return;
    }
    for (const auto& peer : peers) {
        rememberPeerEndpointLocked(record, peer.ip);
    }
}

int effectiveConnectedPeerCount(const lt::torrent_handle& handle, int status_num_peers) {
    if (status_num_peers > 0) {
        return status_num_peers;
    }

    std::vector<lt::peer_info> peer_infos;
    try {
        handle.get_peer_info(peer_infos);
    } catch (...) {
        return status_num_peers;
    }
    return static_cast<int>(peer_infos.size());
}

struct PeerHealthSnapshot {
    int total = 0;
    int interesting = 0;
    int unchoked = 0;
    int active_downloaders = 0;
    int urgent_downloaders = 0;
    int payload_down_speed = 0;
    int urgent_down_speed = 0;
    int queued_bytes = 0;
    int queued_requests = 0;
    int target_requests = 0;
};

PeerHealthSnapshot capturePeerHealthSnapshot(const lt::torrent_handle& handle,
                                             int urgent_start,
                                             int urgent_end) {
    PeerHealthSnapshot snapshot;
    std::vector<lt::peer_info> peer_infos;
    try {
        handle.get_peer_info(peer_infos);
    } catch (...) {
        return snapshot;
    }

    snapshot.total = static_cast<int>(peer_infos.size());
    for (const auto& pi : peer_infos) {
        if (pi.flags & lt::peer_info::interesting) {
            ++snapshot.interesting;
        }
        if (!(pi.flags & lt::peer_info::remote_choked)) {
            ++snapshot.unchoked;
        }

        const bool active = pi.down_speed > 0 || pi.download_queue_length > 0;
        if (active) {
            ++snapshot.active_downloaders;
            snapshot.payload_down_speed += std::max(pi.down_speed, 0);
        }

        snapshot.queued_bytes += std::max(pi.queue_bytes, 0);
        snapshot.queued_requests += std::max(pi.download_queue_length, 0);
        snapshot.target_requests += std::max(pi.target_dl_queue_length, 0);

        if (pi.downloading_piece_index >= urgent_start &&
            pi.downloading_piece_index <= urgent_end &&
            active) {
            ++snapshot.urgent_downloaders;
            snapshot.urgent_down_speed += std::max(pi.down_speed, 0);
        }
    }
    return snapshot;
}

std::string formatPeerHealthSnapshot(const PeerHealthSnapshot& snapshot,
                                     const lt::torrent_status& status) {
    return " peers=" + std::to_string(status.num_peers) +
           " active=" + std::to_string(snapshot.active_downloaders) +
           " urgent=" + std::to_string(snapshot.urgent_downloaders) +
           " unchoked=" + std::to_string(snapshot.unchoked) +
           " interesting=" + std::to_string(snapshot.interesting) +
           " payload=" + std::to_string(status.download_payload_rate / 1024) + "KB/s" +
           " peer_down=" + std::to_string(snapshot.payload_down_speed / 1024) + "KB/s" +
           " urgent_down=" + std::to_string(snapshot.urgent_down_speed / 1024) + "KB/s" +
           " queued_kb=" + std::to_string(snapshot.queued_bytes / 1024) +
           " reqq=" + std::to_string(snapshot.queued_requests) +
           "/" + std::to_string(snapshot.target_requests) +
           " known=" + std::to_string(status.list_peers) +
           " candidates=" + std::to_string(status.connect_candidates);
}

std::string formatPeerSourceFlags(const lt::peer_info& pi) {
    std::string out;
    const auto append = [&](const char* tag) {
        if (!out.empty()) out += "+";
        out += tag;
    };
    if (pi.source & lt::peer_info::tracker) append("trk");
    if (pi.source & lt::peer_info::dht) append("dht");
    if (pi.source & lt::peer_info::pex) append("pex");
    if (pi.source & lt::peer_info::lsd) append("lsd");
    if (pi.source & lt::peer_info::resume_data) append("res");
    if (pi.source & lt::peer_info::incoming) append("in");
    if (out.empty()) out = "-";
    return out;
}

std::string formatPeerFlagsCompact(const lt::peer_info& pi) {
    std::string out;
    if (pi.flags & lt::peer_info::interesting) out += "I";
    if (!(pi.flags & lt::peer_info::remote_choked)) out += "U";
    if (pi.flags & lt::peer_info::snubbed) out += "S";
    if (pi.flags & lt::peer_info::seed) out += "Z";
    if (pi.flags & lt::peer_info::optimistic_unchoke) out += "O";
    if (pi.flags & lt::peer_info::handshake) out += "H";
    if (pi.flags & lt::peer_info::connecting) out += "C";
    if (out.empty()) out = "-";
    return out;
}

std::string formatPeerTransport(const lt::peer_info& pi) {
    if (pi.flags & lt::peer_info::utp_socket) return "utp";
    if (pi.flags & lt::peer_info::i2p_socket) return "i2p";
    return "tcp";
}

std::string formatStallPeerDetails(const lt::torrent_handle& handle,
                                   int urgent_start,
                                   int urgent_end) {
    std::vector<lt::peer_info> peer_infos;
    try {
        handle.get_peer_info(peer_infos);
    } catch (...) {
        return {};
    }
    if (peer_infos.empty()) {
        return {};
    }

    std::sort(peer_infos.begin(), peer_infos.end(), [urgent_start, urgent_end](const lt::peer_info& a,
                                                                                const lt::peer_info& b) {
        const bool a_active = a.down_speed > 0 || a.download_queue_length > 0;
        const bool b_active = b.down_speed > 0 || b.download_queue_length > 0;
        if (a_active != b_active) return a_active > b_active;

        const bool a_urgent = a.downloading_piece_index >= urgent_start && a.downloading_piece_index <= urgent_end;
        const bool b_urgent = b.downloading_piece_index >= urgent_start && b.downloading_piece_index <= urgent_end;
        if (a_urgent != b_urgent) return a_urgent > b_urgent;

        if (a.down_speed != b.down_speed) return a.down_speed > b.down_speed;
        return a.download_queue_length < b.download_queue_length;
    });

    std::ostringstream oss;
    int emitted = 0;
    for (const auto& pi : peer_infos) {
        const bool active = pi.down_speed > 0 || pi.download_queue_length > 0;
        if (!active && emitted >= 3) {
            continue;
        }

        lt::error_code ec;
        const std::string host = pi.ip.address().to_string(ec);
        if (!oss.str().empty()) oss << " | ";
        oss << "#" << emitted
            << " " << (ec ? "?" : host) << ":" << pi.ip.port()
            << " " << formatPeerTransport(pi)
            << " src=" << formatPeerSourceFlags(pi)
            << " flags=" << formatPeerFlagsCompact(pi)
            << " piece=" << pi.downloading_piece_index
            << " down=" << (std::max(pi.down_speed, 0) / 1024) << "KB/s"
            << " q=" << std::max(pi.download_queue_length, 0)
            << "/" << std::max(pi.target_dl_queue_length, 0)
            << " qtime=" << lt::total_milliseconds(pi.download_queue_time) << "ms";

        ++emitted;
        if (emitted >= 5) break;
    }
    return oss.str();
}




bool getTorrentFilesLocked(TorrentEngine::Impl& impl,
                           TorrentRecord& record,
                           std::vector<TorrentEngineFileInfo>& out_files,
                           std::string& error_out,
                           std::unique_lock<std::mutex>& lock) {
    out_files.clear();
    if (!waitForMetadataLocked(impl, record, error_out, lock)) {
        return false;
    }
    auto status = record.handle.status(lt::torrent_handle::query_accurate_download_counters);
    updateProbeStatusLocked(impl, "reading file list", record.hash,
                            status.num_peers, status.num_seeds, true,
                            status.list_peers, status.connect_candidates, static_cast<int>(status.state));

    auto ti = record.handle.torrent_file();
    if (!ti) {
        error_out = "torrent metadata is unavailable";
        return false;
    }

    const auto& fs = ti->files();
    if (record.wanted_files.size() != static_cast<size_t>(fs.num_files())) {
        record.wanted_files.assign(static_cast<size_t>(fs.num_files()), true);
    }

    out_files.reserve(static_cast<size_t>(fs.num_files()));
    for (int i = 0; i < fs.num_files(); ++i) {
        TorrentEngineFileInfo file_info;
        file_info.index = i + 1;
        file_info.name = fs.file_path(i);
        file_info.size = static_cast<unsigned long long>(fs.file_size(i));
        file_info.wanted = record.wanted_files[static_cast<size_t>(i)];
        out_files.push_back(std::move(file_info));
    }
    updateProbeStatusLocked(impl, "file list ready", record.hash,
                            status.num_peers, status.num_seeds, true,
                            status.list_peers, status.connect_candidates, static_cast<int>(status.state));
    return true;
}

int chooseDefaultFileIndexLocked(TorrentEngine::Impl& impl,
                                 TorrentRecord& record,
                                 std::string& error_out,
                                 std::unique_lock<std::mutex>& lock) {
    std::vector<TorrentEngineFileInfo> files;
    if (!getTorrentFilesLocked(impl, record, files, error_out, lock) || files.empty()) {
        return -1;
    }

    int best_index = files.front().index;
    unsigned long long best_size = files.front().size;
    int best_priority = installFilePriority(files.front().name);

    for (const auto& file : files) {
        const int priority = installFilePriority(file.name);
        if (priority > best_priority || (priority == best_priority && file.size > best_size)) {
            best_priority = priority;
            best_size = file.size;
            best_index = file.index;
        }
    }
    return best_index;
}

bool ensureTorrentLocked(TorrentEngine::Impl& impl,
                         const std::string& info_hash,
                         const std::string& magnet_link,
                         const std::string& torrent_file_path,
                         std::shared_ptr<TorrentRecord>& out_record,
                         std::string& out_hash,
                         std::string& error_out) {
    out_record.reset();
    const std::string normalized_magnet = normalizeMagnetLink(magnet_link);
    out_hash = util::toLowerCopy(!info_hash.empty() ? info_hash : extractBtihHash(normalized_magnet));

    if (!out_hash.empty()) {
        auto existing = impl.torrents.find(out_hash);
        if (existing != impl.torrents.end()) {
            out_record = existing->second;
            // Clear verified pieces to force re-validation on stream start
            {
                std::lock_guard<std::mutex> rec_lock(out_record->mutex);
                out_record->verified_pieces.clear();
                if (out_record->handle.is_valid()) {
                    // Sync verified pieces with libtorrent state
                    const auto& ti = out_record->handle.get_torrent_info();
                    if (ti.is_valid()) {
                        int num_pieces = ti.num_pieces();
                        for (int i = 0; i < num_pieces; ++i) {
                            if (out_record->handle.have_piece(lt::piece_index_t(i))) {
                                out_record->verified_pieces.insert(i);
                            }
                        }
                        util::logLine("torrent_engine: synced verified pieces count=" + std::to_string(out_record->verified_pieces.size()));
                    }
                    util::logLine("torrent_engine: re-using existing torrent hash=" + out_hash);
                }
            }
            return true;
        }
    }

    if (!ensureSessionLocked(impl, error_out)) {
        return false;
    }

    lt::error_code ec;
    lt::add_torrent_params atp;
    std::string normalized_hash = out_hash;

    if (!normalized_magnet.empty()) {
        atp = lt::parse_magnet_uri(normalized_magnet, ec);
        if (ec) {
            error_out = "parse_magnet_uri failed: " + ec.message();
            return false;
        }
        addFallbackTrackers(atp);
    } else if (!torrent_file_path.empty()) {
        auto ti = std::make_shared<lt::torrent_info>(torrent_file_path, ec);
        if (ec || !ti) {
            error_out = "torrent_info load failed: " + ec.message();
            return false;
        }
        atp.ti = ti;
        normalized_hash = util::toHex(ti->info_hash().to_string());
        addFallbackTrackers(atp);
    } else {
        error_out = "no magnet or .torrent path provided";
        return false;
    }

    if (normalized_hash.empty()) {
        normalized_hash = util::toLowerCopy(extractBtihHash(normalized_magnet));
    }
    if (normalized_hash.empty()) {
        error_out = "failed to resolve torrent hash";
        return false;
    }

    const std::filesystem::path save_path = impl.cache_root / normalized_hash;
    if (std::filesystem::exists(save_path)) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(save_path, cleanup_ec);
        if (cleanup_ec) {
            util::logLine("torrent_engine: failed to remove stale cache path " + save_path.string() +
                          ": " + cleanup_ec.message());
        } else {
            util::logLine("torrent_engine: removed stale cache path " + save_path.string());
        }
    }
    std::error_code dir_ec;
    std::filesystem::create_directories(save_path, dir_ec);

    atp.save_path = save_path.string();
    util::logLine("torrent_engine: cache root=" + impl.cache_root.string());
    util::logLine("torrent_engine: save path=" + atp.save_path);
    
    // Torrest-style local engine with TSNX RAM storage: libtorrent still drives
    // the torrent/session logic, while completed pieces stay in memory.
    atp.storage = memory_storage_constructor;
    atp.storage_mode = lt::storage_mode_sparse;
    atp.flags &= ~lt::torrent_flags::sequential_download; // Disable: we use manual waterfall priorities
    util::logLine("torrent_engine: storage=memory (torrest-style RAM cache)");
    atp.flags |= lt::torrent_flags::paused;
    atp.flags &= ~lt::torrent_flags::auto_managed;

    lt::torrent_handle handle = impl.session->add_torrent(atp, ec);
    if (ec || !handle.is_valid()) {
        error_out = "add_torrent failed: " + ec.message();
        return false;
    }

    handle.set_max_connections(kConnectionsLimit);
    // Stay paused until we receive metadata and set priorities
    // handle.force_recheck(); // Redundant for MemoryStorage
    handle.force_reannounce();
    handle.force_dht_announce();
    util::logLine("torrent_engine: torrent started with sequential_download=false hash=" + normalized_hash);

    // Для .torrent-файлов torrent_info известен сразу — адаптируем настройки без ожидания metadata_received_alert
    if (atp.ti) {
        int64_t total_sz = atp.ti->total_size();
        util::logLine("torrent_engine: .torrent file known, total_size=" + std::to_string(total_sz));
        adapt_settings_for_torrent_size(*impl.session, total_sz);
    }

    auto record = std::make_shared<TorrentRecord>();
    record->hash = normalized_hash;
    record->magnet_link = normalized_magnet;
    record->torrent_file_path = torrent_file_path;
    record->name = normalized_hash;
    record->save_path = save_path;
    record->handle = handle;

    auto inserted = impl.torrents.emplace(normalized_hash, record);
    out_record = inserted.first->second;
    out_hash = normalized_hash;
    updateProbeStatusLocked(impl, "torrent added", normalized_hash, 0, 0, false);

    util::logLine("torrent_engine: added torrent hash=" + normalized_hash);
    return true;
}


#endif

TorrentEngine::TorrentEngine()
    : impl_(std::make_unique<Impl>())
    , port_(kDefaultPort) {}

TorrentEngine::~TorrentEngine() {
    stop();
}

TorrentEngine& TorrentEngine::instance() {
    static TorrentEngine engine;
    return engine;
}

bool TorrentEngine::isEnabled() const {
#ifdef TSNX_USE_LIBTORRENT
    return true;
#else
    return false;
#endif
}

bool TorrentEngine::isRunning() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return server_running_;
}

std::string TorrentEngine::serverUrl() const {
    return "torrest-memory://local";
}

bool TorrentEngine::start(int port) {
#ifndef TSNX_USE_LIBTORRENT
    (void)port;
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = "TSNX_USE_LIBTORRENT is disabled";
    return false;
#else
    if (port <= 0) port = kDefaultPort;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (server_running_ && port_ == port && impl_->session) {
            return true;
        }
    }

    std::string error_out;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!ensureSessionLocked(*impl_, error_out)) {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            last_error_ = error_out;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        port_ = port;
        server_running_ = true;
        last_error_.clear();
        
        if (!impl_->alert_thread_running) {
            impl_->alert_thread_running = true;
            impl_->alert_thread = std::thread([this]() { impl_->alertThreadFunc(); });
        }
    }

    util::logLine("torrent_engine: torrest core started (memory cache, no local HTTP server)");
    return true;

#endif
}

void TorrentEngine::stop() {
#ifndef TSNX_USE_LIBTORRENT
    return;
#else
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->session) {
            for (auto it = impl_->torrents.begin(); it != impl_->torrents.end(); ++it) {
                if (it->second && it->second->handle.is_valid()) {
                    impl_->session->remove_torrent(it->second->handle,
                                                   lt::session::delete_files | lt::session::delete_partfile);
                }
            }
        }
        impl_->torrents.clear();
        impl_->prepared_stream = {};
        impl_->probe_status = {};
        impl_->probe_cancel_requested = false;
        
        if (impl_->alert_thread_running) {
            impl_->alert_thread_running = false;
            if (impl_->alert_thread.joinable()) {
                impl_->alert_thread.join();
            }
        }
        
        impl_->session.reset();
    }

    std::error_code ec;
    std::filesystem::remove_all(impl_->cache_root, ec);
#ifdef __SWITCH__
    std::filesystem::remove_all("sdmc:/switch/sdmc:/switch/TorrentShopNX/cache/local_engine", ec);
#endif

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    server_running_ = false;
    last_error_.clear();
    util::logLine("torrent_engine: torrest core stopped and RAM cache released");
    return;

#endif
}

bool TorrentEngine::addMagnet(const std::string& magnet, std::string* out_hash) {
#ifndef TSNX_USE_LIBTORRENT
    (void)magnet;
    (void)out_hash;
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = "TSNX_USE_LIBTORRENT is disabled";
    return false;
#else
    if (magnet.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "empty magnet link";
        return false;
    }

    if (!start(port_)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::shared_ptr<TorrentRecord> record;
    std::string hash;
    std::string error;
    if (!ensureTorrentLocked(*impl_, "", magnet, "", record, hash, error) || !record) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = error;
        return false;
    }

    if (out_hash) {
        *out_hash = hash;
    }
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    last_error_.clear();
    return true;
#endif
}

bool TorrentEngine::addTorrentFile(const std::string& torrent_file_path, std::string* out_hash) {
#ifndef TSNX_USE_LIBTORRENT
    (void)torrent_file_path;
    (void)out_hash;
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = "TSNX_USE_LIBTORRENT is disabled";
    return false;
#else
    if (torrent_file_path.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "empty .torrent path";
        return false;
    }

    if (!start(port_)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::shared_ptr<TorrentRecord> record;
    std::string hash;
    std::string error;
    if (!ensureTorrentLocked(*impl_, "", "", torrent_file_path, record, hash, error) || !record) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = error;
        return false;
    }

    if (out_hash) {
        *out_hash = hash;
    }
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    last_error_.clear();
    return true;
#endif
}



bool TorrentEngine::getTorrentList(std::vector<TorrentEngineItem>& out_items) {
    out_items.clear();
#ifndef TSNX_USE_LIBTORRENT
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = "TSNX_USE_LIBTORRENT is disabled";
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& entry : impl_->torrents) {
        auto& record = entry.second;
        if (!record || !record->handle.is_valid()) continue;

        std::lock_guard<std::mutex> record_lock(record->mutex);
        auto status = record->handle.status(lt::torrent_handle::query_accurate_download_counters);
        if (!status.name.empty()) {
            record->name = status.name;
        }

        TorrentEngineItem item;
        item.hash = record->hash;
        item.name = !record->name.empty() ? record->name : record->hash;
        item.progress = status.progress;
        item.download_speed_kbps = static_cast<float>(status.download_rate / 1024.0f);
        item.loaded_size = static_cast<unsigned long long>(status.total_wanted_done);
        item.torrent_size = static_cast<unsigned long long>(status.total_wanted);
        out_items.push_back(std::move(item));
    }

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    last_error_.clear();
    return true;
#endif
}

bool TorrentEngine::getTorrentFiles(const std::string& hash, std::vector<TorrentEngineFileInfo>& out_files) {
    out_files.clear();
#ifndef TSNX_USE_LIBTORRENT
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = "TSNX_USE_LIBTORRENT is disabled";
    return false;
#else
    std::unique_lock<std::mutex> lock(impl_->mutex);
    auto it = impl_->torrents.find(util::toLowerCopy(hash));
    if (it == impl_->torrents.end()) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = "torrent is not found";
        return false;
    }

    std::string error;
    if (!it->second) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = "torrent is not found";
        return false;
    }

    std::lock_guard<std::mutex> record_lock(it->second->mutex);
    if (!getTorrentFilesLocked(*impl_, *it->second, out_files, error, lock)) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = error;
        return false;
    }

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    last_error_.clear();
    return true;
#endif
}

bool TorrentEngine::setFileWanted(const std::string& hash, int file_index, bool wanted) {
#ifndef TSNX_USE_LIBTORRENT
    (void)hash;
    (void)file_index;
    (void)wanted;
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = "TSNX_USE_LIBTORRENT is disabled";
    return false;
#else
    std::unique_lock<std::mutex> lock(impl_->mutex);
    auto it = impl_->torrents.find(util::toLowerCopy(hash));
    if (it == impl_->torrents.end()) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = "torrent is not found";
        return false;
    }

    std::string error;
    if (!it->second) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = "torrent is not found";
        return false;
    }

    std::lock_guard<std::mutex> record_lock(it->second->mutex);
    if (!waitForMetadataLocked(*impl_, *it->second, error, lock)) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = error;
        return false;
    }

    auto ti = it->second->handle.torrent_file();
    if (!ti) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = "torrent metadata is unavailable";
        return false;
    }

    const int resolved = resolveFileIndex(ti->files().num_files(), file_index);
    if (resolved < 0) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = "invalid torrent file index";
        return false;
    }

    if (it->second->wanted_files.size() != static_cast<size_t>(ti->files().num_files())) {
        it->second->wanted_files.assign(static_cast<size_t>(ti->files().num_files()), true);
    }
    it->second->wanted_files[static_cast<size_t>(resolved)] = wanted;
    if (!applyFilePrioritiesLocked(*impl_, *it->second, -1, error, lock)) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = error;
        return false;
    }

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    last_error_.clear();
    return true;
#endif
}

bool TorrentEngine::removeTorrent(const std::string& hash) {
#ifndef TSNX_USE_LIBTORRENT
    (void)hash;
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->torrents.find(util::toLowerCopy(hash));
    if (it == impl_->torrents.end()) {
        return false;
    }

    if (impl_->prepared_stream.open && impl_->prepared_stream.hash == it->first) {
        impl_->prepared_stream = {};
    }

    removeTorrentLocked(*impl_, it);
    return true;
#endif
}

bool TorrentEngine::pauseTorrent(const std::string& hash) {
#ifndef TSNX_USE_LIBTORRENT
    (void)hash;
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->torrents.find(util::toLowerCopy(hash));
    if (it == impl_->torrents.end() || !it->second || !it->second->handle.is_valid()) {
        return false;
    }
    std::lock_guard<std::mutex> record_lock(it->second->mutex);
    it->second->handle.pause();
    return true;
#endif
}

bool TorrentEngine::resumeTorrent(const std::string& hash) {
#ifndef TSNX_USE_LIBTORRENT
    (void)hash;
    return false;
#else
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->torrents.find(util::toLowerCopy(hash));
    if (it == impl_->torrents.end() || !it->second || !it->second->handle.is_valid()) {
        return false;
    }
    std::lock_guard<std::mutex> record_lock(it->second->mutex);
    it->second->handle.resume();
    return true;
#endif
}

bool TorrentEngine::prepareStream(const std::string& info_hash,
                                  const std::string& magnet_link,
                                  const std::string& torrent_file_path,
                                  int file_index) {
#ifndef TSNX_USE_LIBTORRENT
    (void)info_hash;
    (void)magnet_link;
    (void)torrent_file_path;
    (void)file_index;
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = "TSNX_USE_LIBTORRENT is disabled";
    return false;
#else
    if (!start(port_)) {
        return false;
    }

    std::unique_lock<std::mutex> lock(impl_->mutex);
    std::shared_ptr<TorrentRecord> record;
    std::string hash;
    std::string error;
    if (!ensureTorrentLocked(*impl_, info_hash, magnet_link, torrent_file_path, record, hash, error) || !record) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = error;
        return false;
    }

    int resolved_file_index = -1;
    uint64_t file_size = 0;
    if (!resolveFileAccessLocked(*impl_, *record, file_index, resolved_file_index, &file_size, error, lock)) {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        last_error_ = error;
        return false;
    }
    {
        std::lock_guard<std::mutex> record_lock(record->mutex);
        record->active_file_index = resolved_file_index;
        record->state = TorrentState::FileSelected;
        record->reconnect_cursor = 0;
    }

    impl_->prepared_stream.open = true;
    impl_->prepared_stream.hash = hash;
    impl_->prepared_stream.record = record;
    impl_->prepared_stream.file_index = file_index;
    impl_->prepared_stream.resolved_file_index = resolved_file_index;
    impl_->prepared_stream.file_size = file_size;

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    last_error_.clear();
    return true;
#endif
}





void TorrentEngine::setStreamMinKeepOffset(const std::string& hash, uint64_t offset) {
#ifndef TSNX_USE_LIBTORRENT
    (void)hash;
    (void)offset;
#else
    std::lock_guard<std::mutex> lock(g_memory_storage_mutex);
    auto it = g_memory_storages.find(util::toLowerCopy(hash));
    if (it != g_memory_storages.end() && it->second) {
        it->second->setMinKeepOffsetLocked(offset);
    }
#endif
}





size_t TorrentEngine::readPreparedAvailable(uint64_t offset, void* buf, size_t size) {
#ifndef TSNX_USE_LIBTORRENT
    (void)offset;
    (void)buf;
    (void)size;
    return 0;
#else
    if (!buf || size == 0) {
        return 0;
    }

    std::shared_ptr<TorrentRecord> record;
    int file_index = -1;
    uint64_t file_size = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->prepared_stream.open || !impl_->prepared_stream.record) {
            return 0;
        }
        record = impl_->prepared_stream.record;
        file_index = impl_->prepared_stream.resolved_file_index;
        file_size = impl_->prepared_stream.file_size;
    }

    if (!record || offset >= file_size) {
        return 0;
    }

    std::lock_guard<std::mutex> record_lock(record->mutex);
    auto ti = record->handle.torrent_file();
    if (!ti) {
        return 0;
    }

    const size_t max_read = static_cast<size_t>(
        std::min<uint64_t>(file_size - offset, static_cast<uint64_t>(size)));
    if (max_read == 0) {
        return 0;
    }

    size_t available = memoryStorageAvailableBytes(*record, *ti, file_index, offset, max_read);
    if (available == 0) {
        return 0;
    }

    if (!readMemoryStorageRange(*record, *ti, file_index, offset, buf, available)) {
        util::logLine("torrent_engine: ERROR: readMemoryStorageRange failed despite available bytes! offset=" + 
                      std::to_string(offset) + " available=" + std::to_string(available));
        return 0;
    }
    return available;
#endif
}





void TorrentEngine::beginProbe(const std::string& hash_hint) {
#ifdef TSNX_USE_LIBTORRENT
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->probe_status = {};
    impl_->probe_status.active = true;
    impl_->probe_status.phase = "starting torrent";
    impl_->probe_status.hash = util::toLowerCopy(hash_hint);
    impl_->probe_cancel_requested = false;
#else
    (void)hash_hint;
#endif
}

void TorrentEngine::finishProbe() {
#ifdef TSNX_USE_LIBTORRENT
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->probe_status.active = false;
    impl_->probe_cancel_requested = false;
#endif
}

void TorrentEngine::cancelProbe() {
#ifdef TSNX_USE_LIBTORRENT
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->probe_cancel_requested = true;
    if (impl_->probe_status.active) {
        impl_->probe_status.phase = "cancelled";
    }
#endif
}

TorrentEngineProbeStatus TorrentEngine::probeStatus() const {
#ifdef TSNX_USE_LIBTORRENT
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->probe_status;
#else
    return {};
#endif
}

TorrentEngine::StreamAccessInfo TorrentEngine::getStreamAccess() {
    StreamAccessInfo info{};
#ifdef TSNX_USE_LIBTORRENT
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->session || !impl_->prepared_stream.open || !impl_->prepared_stream.record) {
        return info;
    }
    auto& record = *impl_->prepared_stream.record;
    std::lock_guard<std::mutex> rec_lock(record.mutex);
    auto ti = record.handle.torrent_file();
    if (!ti) {
        return info;
    }
    const auto& fs = ti->files();
    const int fi = impl_->prepared_stream.resolved_file_index;
    if (fi < 0 || fi >= fs.num_files()) {
        return info;
    }

    info.session_ptr = &impl_->session;
    info.handle_ptr = &record.handle;
    info.file_index = fi;
    info.file_size = static_cast<int64_t>(fs.file_size(fi));
    info.file_offset = static_cast<int64_t>(fs.file_offset(fi));
    info.piece_size = ti->piece_length();
    info.first_piece = static_cast<int>(info.file_offset / info.piece_size);
    info.last_piece = static_cast<int>((info.file_offset + info.file_size - 1) / info.piece_size);
    info.valid = true;
#endif
    return info;
}

} // namespace torrent









