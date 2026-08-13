#include "download_manager.h"

#include <sys/stat.h>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <filesystem>

#include "../config/config.h"
#include "../datasource/custom_engine_client.h"
#include "../net/http_client.h"
#include "../utils/log.h"
#include "../utils/switch_utils.h"

#include <engine/engine.h>

namespace ui {
    extern std::atomic<bool> g_file_select_view_active;
}

namespace {

bool customEngineGetFiles(const std::string& hash, std::vector<torrent::TorrentFileInfo>& out) {
    if (hash.empty()) return false;
    // Heap-backed: tsnx_file_info is ~536 bytes; a stack array would overflow
    // the small (64 KB default) libnx pthread stack of the progress thread.
    std::vector<tsnx_file_info> files(TSNX_MAX_FILES);
    int n = tsnx_engine_get_files(nullptr, hash.c_str(), files.data(), TSNX_MAX_FILES);
    if (n <= 0) {
        // Torrent may not be added yet; try a one-shot probe.
        std::vector<datasource::CustomEngineFileInfo> probed;
        std::string err;
        if (!datasource::CustomEngineClient::instance().probeFiles(hash, "", "", probed, &err))
            return false;
        out.clear();
        out.reserve(probed.size());
        for (const auto& f : probed) {
            torrent::TorrentFileInfo tf;
            tf.index = f.index;
            tf.name = f.path;
            tf.size = f.size;
            tf.wanted = true;
            out.push_back(std::move(tf));
        }
        return !out.empty();
    }
    out.clear();
    out.reserve(n);
    for (int i = 0; i < n; i++) {
        torrent::TorrentFileInfo tf;
        tf.index = files[i].index;
        tf.name = files[i].path;
        tf.size = files[i].size;
        tf.wanted = files[i].wanted;
        out.push_back(std::move(tf));
    }
    return true;
}

bool customEngineSetFileWanted(const std::string& hash, int file_index, bool wanted) {
    if (hash.empty()) return false;
    return tsnx_engine_set_file_wanted(nullptr, hash.c_str(), file_index, wanted);
}

void customEngineGetTorrentList(std::vector<torrent::TorrentInfo>& out) {
    out.clear();
    tsnx_torrent_item items[8];
    int n = tsnx_engine_get_torrents(nullptr, items, 8);
    for (int i = 0; i < n; i++) {
        torrent::TorrentInfo ti;
        ti.id = -1;
        ti.name = items[i].name;
        ti.hash = items[i].hash;
        // Same 0..1 convention as the TorrServer path (which normalises its
        // percent_done in torrent_manager.cpp); the UI treats it as 0..1.
        ti.percent_done = items[i].progress;
        ti.download_speed_kbps = items[i].download_kbps;
        ti.loaded_size = items[i].loaded_size;
        ti.torrent_size = items[i].total_size;
        ti.seeds = items[i].seeds;
        ti.peers = items[i].peers;
        ti.known_peers = items[i].known_peers;
        ti.dht = items[i].dht_nodes;
        out.push_back(std::move(ti));
    }
}

} // namespace

namespace download {

static bool isTransferActive(download::DownloadState state) {
    return state == download::DownloadState::Downloading ||
           state == download::DownloadState::StreamPreparing ||
           state == download::DownloadState::StreamInstalling ||
           state == download::DownloadState::Installing;
}

bool DownloadManager::isLocalBackend() const {
    return ds_manager_.mode() == datasource::DataSourceMode::LocalClient ||
           ds_manager_.mode() == datasource::DataSourceMode::CustomEngine;
}

static void updateSleepPolicy(bool has_active_transfers) {
#ifdef __SWITCH__
    static bool initialized = false;
    static bool last_keep_awake = false;

    const bool enabled = config::ConfigManager::instance().getKeepAwakeDuringDownloads();
    const bool keep_awake = enabled && has_active_transfers;
    if (initialized && keep_awake == last_keep_awake) {
        return;
    }

    appletSetMediaPlaybackState(keep_awake);
    util::logLine(std::string("power: keep-awake ") + (keep_awake ? "enabled" : "disabled"));
    last_keep_awake = keep_awake;
    initialized = true;
#else
    (void)has_active_transfers;
#endif
}

DownloadManager::DownloadManager() {
    progress_thread_running_.store(true);
    progress_thread_ = std::thread([this]() {
        while (progress_thread_running_.load()) {
            trackProgress();
            
            if (progress_callback_) {
                progress_callback_();
            }

            std::unique_lock<std::mutex> lock(progress_cv_mutex_);
            if (has_open_pending_.load()) {
                progress_cv_.wait_for(lock, std::chrono::milliseconds(100));
            } else {
                progress_cv_.wait_for(lock, std::chrono::milliseconds(1000));
            }
        }
    });
}

DownloadManager::~DownloadManager() {
    shutdown();
}

void DownloadManager::shutdown() {
    g_appExiting.store(true);

    if (progress_thread_running_.load()) {
        progress_thread_running_.store(false);
        progress_cv_.notify_all();
        if (progress_thread_.joinable()) {
            progress_thread_.join();
        }
    }

    for (auto& item : queue_) {
        if (item.cancel_flag) {
            item.cancel_flag->store(true);
        }
        if (item.hybrid_installer) {
            item.hybrid_installer->cancel();
            item.hybrid_installer.reset();
        }
        if (item.open_future) {
            item.open_future.reset();
        }
        if (item.start_future) {
            item.start_future.reset();
        }
    }

    stopAllStreamConsumers();

    datasource::IDataSource* source = ds_manager_.getSource();
    if (source) {
        source->close();
    }
}

size_t DownloadManager::addToQueue(const std::string& title,
                                   const std::string& magnet,
                                   int forced_file_index,
                                   const std::string& forced_stream_name) {
    DownloadItem item;
    item.title = title;
    item.magnet = magnet;
    item.forced_file_index = forced_file_index;
    item.forced_stream_name = forced_stream_name;
    item.state = DownloadState::Queued;
    item.installer = installer::StreamInstaller(64 * 1024 * 1024);
    queue_.push_back(std::move(item));
    return queue_.size() - 1;
}

static bool getFileSize(const std::string& path, uint64_t& size_out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    size_out = (uint64_t)st.st_size;
    return true;
}

static size_t readChunkToInstaller(const std::string& path, uint64_t& offset, installer::StreamInstaller& installer, size_t max_bytes) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    if (std::fseek(f, (long)offset, SEEK_SET) != 0) {
        std::fclose(f);
        return 0;
    }

    static std::vector<unsigned char> buf(1024 * 1024);
    size_t to_read = max_bytes < buf.size() ? max_bytes : buf.size();
    size_t n = std::fread(buf.data(), 1, to_read, f);
    std::fclose(f);
    if (n > 0) {
        installer.readChunk(buf.data(), n);
        offset += n;
    }
    return n;
}

static void replaceAllInPlace(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string normalizeTorrentLink(std::string link) {
    // Catalogs often escape query separators; recover a usable magnet URL.
    replaceAllInPlace(link, "&amp;", "&");
    replaceAllInPlace(link, "&#38;", "&");
    replaceAllInPlace(link, "\\u0026", "&");
    replaceAllInPlace(link, "\\u002F", "/");
    replaceAllInPlace(link, "\\/", "/");
    return link;
}

static std::string extractBtihHash(std::string magnet) {
    std::transform(magnet.begin(), magnet.end(), magnet.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    const std::string marker = "xt=urn:btih:";
    size_t pos = magnet.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();
    size_t end = magnet.find('&', pos);
    if (end == std::string::npos) end = magnet.size();
    if (end <= pos) return {};
    return magnet.substr(pos, end - pos);
}

static bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

static int installFilePriority(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lower.size() >= 4 && lower.rfind(".nsp") == lower.size() - 4) return 4;
    if (lower.size() >= 4 && lower.rfind(".nsz") == lower.size() - 4) return 3;
    if (lower.size() >= 4 && lower.rfind(".xci") == lower.size() - 4) return 2;
    if (lower.size() >= 4 && lower.rfind(".xcz") == lower.size() - 4) return 2;
    if (lower.size() >= 5 && lower.rfind(".pfs0") == lower.size() - 5) return 1;
    return 0;
}

static std::string ensureHttpUrl(std::string url) {
    if (url.empty()) return "http://127.0.0.1:8090";
    if (url.find("://") == std::string::npos) {
        url = "http://" + url;
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

static std::string urlEncodeLocal(const std::string& value) {
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

static uint64_t parseTitleIdFromFileName(const std::string& name) {
    size_t start = name.find('[');
    while (start != std::string::npos) {
        size_t end = name.find(']', start);
        if (end != std::string::npos && (end - start) == 17) {
            std::string tid_str = name.substr(start + 1, 16);
            try {
                return std::stoull(tid_str, nullptr, 16);
            } catch (...) {}
        }
        start = name.find('[', start + 1);
    }
    return 0;
}

static bool isSwitchGameFile(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 4 &&
           (lower.rfind(".nsp") == lower.size() - 4 ||
            lower.rfind(".nsz") == lower.size() - 4 ||
            lower.rfind(".xci") == lower.size() - 4 ||
            lower.rfind(".xcz") == lower.size() - 4);
}

static void copyDownloadedOtherFiles(const download::DownloadItem& item) {
    std::filesystem::path srcDir;
#ifndef __SWITCH__
    srcDir = std::filesystem::path("./cache/local_engine") / item.torrent_hash;
#else
    srcDir = std::filesystem::path("sdmc:/switch/TorrentShopNX/cache/local_engine") / item.torrent_hash;
#endif

    std::filesystem::path destDir;
#ifndef __SWITCH__
    destDir = "./downloads";
#else
    destDir = "sdmc:/switch/TorrentShopNX/downloads";
#endif

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    // Custom engine streams into RAM and does not keep a local SD cache,
    // so there are no "other files" to copy after installation.
    (void)srcDir; (void)destDir;
}

static void clearTorrentCache(const std::string& hash) {
    if (hash.empty()) return;

    std::filesystem::path cachePath;
#ifndef __SWITCH__
    cachePath = std::filesystem::path("./cache/local_engine") / hash;
#else
    cachePath = std::filesystem::path("sdmc:/switch/TorrentShopNX/cache/local_engine") / hash;
#endif

    std::error_code ec;
    if (std::filesystem::exists(cachePath)) {
        util::logLine("download: explicitly deleting cache path " + cachePath.string());
        std::filesystem::remove_all(cachePath, ec);
        if (ec) {
            util::logLine("download: failed to delete cache path: " + ec.message());
        }
    }
}

static std::string streamRouteNameFromPath(const std::string& path_or_name, const std::string& fallback) {
    std::string name = path_or_name;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    if (name.empty()) {
        name = fallback;
    }
    return name;
}

static bool chooseInstallFile(const std::vector<torrent::TorrentFileInfo>& files,
                               int& out_index,
                               std::string& out_stream_name) {
    if (files.empty()) return false;

    int chosen_index = -1;
    std::string chosen_stream_name;
    unsigned long long chosen_size = 0;
    int chosen_priority = -1;

    for (const auto& f : files) {
        const int priority = installFilePriority(f.name);
        if (priority <= 0) {
            continue;
        }
        if (chosen_index < 0 ||
            priority > chosen_priority ||
            (priority == chosen_priority && f.size > chosen_size)) {
            chosen_priority = priority;
            chosen_size = f.size;
            chosen_index = f.index;
            chosen_stream_name = f.name;
        }
    }

    if (chosen_index < 0) {
        return false;
    }

    out_index = chosen_index;
    out_stream_name = chosen_stream_name;
    return true;
}

static std::string buildStreamPlayUrl(const std::string& base_url,
                                      const std::string& route_name,
                                      const std::string& hash,
                                      int file_index) {
    return base_url + "/stream/" + urlEncodeLocal(route_name) + "?link=" + urlEncodeLocal(hash) +
           "&index=" + std::to_string(file_index) + "&play";
}

static std::string buildStreamPreloadUrl(const std::string& base_url,
                                         const std::string& route_name,
                                         const std::string& hash,
                                         int file_index) {
    return base_url + "/stream/" + urlEncodeLocal(route_name) + "?link=" + urlEncodeLocal(hash) +
           "&index=" + std::to_string(file_index) + "&preload";
}

static float smoothDownloadSpeedKbps(float displayed_kbps,
                                     float sampled_kbps,
                                     std::chrono::steady_clock::time_point last_sample_at,
                                     std::chrono::steady_clock::time_point now) {
    if (!(sampled_kbps >= 0.0f)) sampled_kbps = 0.0f;
    if (!(displayed_kbps >= 0.0f)) displayed_kbps = 0.0f;

    if (last_sample_at.time_since_epoch().count() == 0 || displayed_kbps == 0.0f) {
        return sampled_kbps;
    }

    double dt_sec = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sample_at).count() / 1000.0;
    if (dt_sec <= 0.0) dt_sec = 0.8;

    // Use a much larger smoothing window (20 seconds) to mask short-term
    // disk IO / OS stalls so the instantaneous speed does not drop to 0.
    const double window_sec = 20.0;
    double alpha = dt_sec / window_sec;
    if (alpha < 0.02) alpha = 0.02;
    if (alpha > 0.15) alpha = 0.15;

    return static_cast<float>(displayed_kbps + (sampled_kbps - displayed_kbps) * alpha);
}

static constexpr uint64_t kPumpStartOffset = 28ull * 1024ull * 1024ull;
static constexpr uint64_t kPumpRewindBytes = 1024ull * 1024ull;
static constexpr uint64_t kPumpChunkBytes = 8ull * 1024ull * 1024ull;
static constexpr int kPumpTimeoutSec = 6;
static constexpr size_t kLocalHeaderProbeBytes = 4096;
static constexpr int kPumpIdleSleepMs = 250;
static constexpr int kPumpRepreloadEveryZeroReads = 12;
static constexpr int kPumpRewindEveryZeroReads = 24;

torrent::TorrentManager* DownloadManager::getTorrent() {
    if (!torrent_) {
        util::logLine("download: init torrent manager");
        torrent_.reset(new torrent::TorrentManager());
    }

    torrent_->setServerUrl(ds_manager_.remoteUrl());
    return torrent_.get();
}

void DownloadManager::ensureStreamConsumer(DownloadItem& item) {
    if (!item.preload_started || item.preload_file_index < 0 || item.torrent_id < 0) return;
    if (item.stream_consumer_started) return;

    std::string hash = item.torrent_hash;
    if (hash.empty() && torrent_) {
        torrent_->getTorrentHash(item.torrent_id, hash);
    }
    if (hash.empty()) return;

    const bool local_engine = (isLocalBackend());
    const std::string base_url = ensureHttpUrl(
        (torrent_ != nullptr && !torrent_->getServerUrl().empty())
            ? torrent_->getServerUrl()
            : ds_manager_.remoteUrl());
    const std::string route_name = streamRouteNameFromPath(item.preload_stream_name, hash);
    const int torrent_id = item.torrent_id;
    const int file_index = item.preload_file_index;

    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        for (const auto& existing : stream_consumers_) {
            if (existing.torrent_id == torrent_id) {
                item.stream_consumer_started = true;
                return;
            }
        }
    }

    auto stop_flag = std::make_shared<std::atomic<bool>>(false);
    StreamConsumer consumer;
    consumer.torrent_id = torrent_id;
    consumer.stop = stop_flag;
    consumer.worker = std::thread([stop_flag, base_url, route_name, hash, file_index, local_engine]() {
        net::HttpClient http;
        net::HttpClient preload_http;
        http.setKeepAlive(true);
        const int timeout_sec = local_engine ? 20 : kPumpTimeoutSec;
        const uint64_t chunk_bytes = local_engine ? (512ull * 1024ull) : kPumpChunkBytes;
        const uint64_t start_offset = local_engine ? 0ull : kPumpStartOffset;
        const int repreload_every = local_engine ? 4 : kPumpRepreloadEveryZeroReads;
        http.setTimeout(timeout_sec);
        preload_http.setTimeout(3);

        const std::string play_url = buildStreamPlayUrl(base_url, route_name, hash, file_index);
        const std::string preload_url = buildStreamPreloadUrl(base_url, route_name, hash, file_index);

        uint64_t offset = start_offset;
        int zero_reads = 0;
        bool logged_first_success = false;

        while (!stop_flag->load()) {
            size_t received = 0;
            int code = http.httpGetStream(play_url, offset, chunk_bytes,
                                          [&](const void*, size_t n) -> size_t {
                                              if (stop_flag->load()) return 0;
                                              received += n;
                                              return n;
                                          });
            if ((code == 200 || code == 206 || code == 0) && received > 0) {
                if (!logged_first_success) {
                    util::logLine("download: stream consumer received " + std::to_string(received) +
                                  " bytes from " + hash + " at offset=" + std::to_string(offset));
                    logged_first_success = true;
                }
                offset += static_cast<uint64_t>(received);
                zero_reads = 0;
                continue;
            }

            ++zero_reads;
            if ((zero_reads % repreload_every) == 0) {
                preload_http.httpGet(preload_url);
            }
            if (zero_reads == 1 || (zero_reads % 8) == 0) {
                util::logLine("download: stream consumer waiting hash=" + hash +
                              " offset=" + std::to_string(offset) +
                              " code=" + std::to_string(code) +
                              " zero_reads=" + std::to_string(zero_reads));
            }
            if ((zero_reads % kPumpRewindEveryZeroReads) == 0) {
                if (offset > kPumpRewindBytes) {
                    offset -= kPumpRewindBytes;
                } else {
                    offset = start_offset;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPumpIdleSleepMs));
        }
    });

    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        stream_consumers_.push_back(std::move(consumer));
    }

    item.stream_consumer_started = true;
    util::logLine("download: stream consumer started hash=" + hash +
                  " file_index=" + std::to_string(file_index) +
                  " base_url=" + base_url +
                  (local_engine ? " mode=local" : " mode=remote"));
}

void DownloadManager::stopStreamConsumer(int torrent_id) {
    if (torrent_id < 0) return;

    std::thread worker;
    std::shared_ptr<std::atomic<bool>> stop_flag;

    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        for (auto it = stream_consumers_.begin(); it != stream_consumers_.end(); ++it) {
            if (it->torrent_id != torrent_id) continue;
            stop_flag = it->stop;
            worker = std::move(it->worker);
            stream_consumers_.erase(it);
            break;
        }
    }

    if (stop_flag) {
        stop_flag->store(true);
    }
    if (worker.joinable()) {
        worker.join();
    }
    if (stop_flag) {
        util::logLine("download: stream consumer stopped torrent_id=" + std::to_string(torrent_id));
    }
}

void DownloadManager::stopAllStreamConsumers() {
    std::vector<std::thread> workers;
    workers.reserve(stream_consumers_.size());
    {
        std::lock_guard<std::mutex> lock(stream_consumers_mtx_);
        for (auto& consumer : stream_consumers_) {
            if (consumer.stop) {
                consumer.stop->store(true);
            }
            if (consumer.worker.joinable()) {
                workers.push_back(std::move(consumer.worker));
            }
        }
        stream_consumers_.clear();
    }
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    for (auto& item : queue_) {
        item.stream_consumer_started = false;
    }
}

bool DownloadManager::startDownload(size_t index) {
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.state != DownloadState::Queued) return false;

    if (item.stream_consumer_started && item.torrent_id >= 0) {
        stopStreamConsumer(item.torrent_id);
        item.stream_consumer_started = false;
    }

    if (item.magnet.empty()) {
        item.state = DownloadState::Failed;
        item.error_message = "Empty torrent link";
        util::logLine("download: empty torrent link for " + item.title);
        return false;
    }

    const std::string link = normalizeTorrentLink(item.magnet);
    const bool local_mode_only = (isLocalBackend());
    std::string local_hash;
    int id = -1;
    if (local_mode_only) {
        local_hash = extractBtihHash(link);
        if (local_hash.empty()) {
            item.state = DownloadState::Failed;
            item.error_message = "Failed to resolve BTIH hash from magnet";
            util::logLine("download: local client cannot start without BTIH hash for " + item.title);
            return false;
        }
        // Adopt the probe's kept torrent (if any) so the file-select cleanup
        // does not remove it from under the download.
        datasource::CustomEngineClient::instance().markInUse(local_hash);
        util::logLine("download: local client queued hash=" + local_hash);
    } else {
        id = getTorrent()->addMagnet(link);
        if (id < 0) {
            item.state = DownloadState::Failed;
            item.error_message = "Failed to add torrent to TorrServer";
            util::logLine("download: failed to add torrent to TorrServer for " + item.title);
            return false;
        }
    }

    item.state = DownloadState::Downloading;
    item.torrent_id = id;
    item.preload_started = false;
    item.preload_file_index = -1;
    item.preload_stream_name.clear();
    item.auto_hybrid_started = false;
    item.stream_consumer_started = false;
    item.hybrid_installer.reset();
    item.pump_offset = 0;
    item.pump_zero_reads = 0;
    item.pump_last_at = std::chrono::steady_clock::time_point{};
    item.download_speed_kbps = 0.0f;
    item.speed_sample_at = std::chrono::steady_clock::time_point{};
    item.start_time = std::chrono::steady_clock::now();
    item.error_message.clear();
    item.torrent_hash.clear();

    if (local_mode_only) {
        item.torrent_hash = local_hash;
    } else if (item.torrent_id >= 0) {
        std::string hash;
        if (torrent_ && torrent_->getTorrentHash(item.torrent_id, hash)) {
            item.torrent_hash = hash;
            util::logLine("download: torrent hash: " + hash);
        }
    }

    if (item.torrent_hash.empty()) {
        item.torrent_hash = extractBtihHash(link);
        if (item.torrent_hash.empty()) {
            item.state = DownloadState::Failed;
            item.error_message = "Failed to resolve BTIH hash from magnet";
            util::logLine("download: addMagnet failed and BTIH is unavailable for " + item.title);
            return false;
        }
        if (local_mode_only) {
            util::logLine("download: local client hash=" + item.torrent_hash);
        } else {
            util::logLine("download: TorrServer add failed, using local client hash=" + item.torrent_hash);
        }
    }

    return true;
}

void DownloadManager::startNextDownload() {
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (queue_[i].state != DownloadState::Queued) continue;
        if (startDownload(i)) return;
    }
}

void DownloadManager::trackProgress() {
    auto now = std::chrono::steady_clock::now();
    bool refreshed_torrent_list = false;
    const bool is_local_client = (isLocalBackend());
    if (torrent_ && !is_local_client) {
        const auto poll_interval = std::chrono::milliseconds(800);
        if (last_torrent_list_.empty() ||
            last_torrent_list_poll_.time_since_epoch().count() == 0 ||
            (now - last_torrent_list_poll_) >= poll_interval) {
            auto fresh_list = torrent_->getTorrentList();
            if (!fresh_list.empty()) {
                last_torrent_list_ = std::move(fresh_list);
            }
            last_torrent_list_poll_ = now;
            refreshed_torrent_list = true;
        }
    } else if (is_local_client) {
        const auto poll_interval = std::chrono::milliseconds(800);
        if (last_torrent_list_poll_.time_since_epoch().count() == 0 ||
            (now - last_torrent_list_poll_) >= poll_interval) {
            std::vector<torrent::TorrentInfo> fresh_list;
            if (ds_manager_.mode() == datasource::DataSourceMode::CustomEngine ||
                ds_manager_.mode() == datasource::DataSourceMode::LocalClient) {
                customEngineGetTorrentList(fresh_list);
            } else if (torrent_) {
                fresh_list = torrent_->getTorrentList();
            }
            last_torrent_list_ = std::move(fresh_list);
            last_torrent_list_poll_ = now;
            refreshed_torrent_list = true;
        }
    }

    const auto& list = last_torrent_list_;
    for (size_t i = 0; i < queue_.size(); ++i) {
        auto& item = queue_[i];
        const bool downloading_active =
            (item.state == DownloadState::Downloading || item.state == DownloadState::StreamInstalling);
        float hybrid_speed_kbps = -1.0f;

        if (!downloading_active && item.stream_consumer_started && item.torrent_id >= 0) {
            stopStreamConsumer(item.torrent_id);
            item.stream_consumer_started = false;
        }

        if (item.state == DownloadState::Cancelled ||
            item.state == DownloadState::Completed ||
            item.state == DownloadState::Failed) {
            continue;
        }

        if (item.hybrid_installer && downloading_active) {
            float install_p = item.hybrid_installer->progress();
            if (!(install_p >= 0.0f)) install_p = 0.0f;
            if (install_p > 1.0f) install_p = 1.0f;
            item.install_progress = install_p;

            float dl_p = item.hybrid_installer->downloadProgress();
            if (!(dl_p >= 0.0f)) dl_p = 0.0f;
            if (dl_p > 1.0f) dl_p = 1.0f;
            item.progress = dl_p;
            hybrid_speed_kbps = static_cast<float>(item.hybrid_installer->downloadSpeedKbps());
            if (!(hybrid_speed_kbps >= 0.0f)) hybrid_speed_kbps = 0.0f;
            item.install_written = item.hybrid_installer->bytesInstalled();
            item.install_total = item.hybrid_installer->totalBytes();

            if (item.hybrid_installer->isFinished()) {
                if (item.hybrid_installer->hasError()) {
                    item.state = DownloadState::Failed;
                    item.error_message = item.hybrid_installer->errorMessage();
                    item.download_speed_kbps = 0.0f;
                    item.speed_sample_at = std::chrono::steady_clock::time_point{};
                    if (item.torrent_id >= 0) {
                        torrent_->cancelTorrent(item.torrent_id);
                    }
                    util::logLine("download: hybrid install failed: " + item.error_message);
                } else {
                    item.state = DownloadState::Completed;
                    copyDownloadedOtherFiles(item);
                    item.download_speed_kbps = 0.0f;
                    item.speed_sample_at = std::chrono::steady_clock::time_point{};
                    if (item.torrent_id >= 0) {
                        torrent_->cancelTorrent(item.torrent_id);
                    } else if (!item.torrent_hash.empty()) {
                        bool still_needed = false;
                        for (size_t j = 0; j < queue_.size(); ++j) {
                            if (i == j) continue;
                            const auto& other = queue_[j];
                            std::string other_hash = other.torrent_hash;
                            if (other_hash.empty() && !other.magnet.empty()) {
                                other_hash = extractBtihHash(normalizeTorrentLink(other.magnet));
                            }
                            if (other_hash == item.torrent_hash &&
                                (other.state == DownloadState::Queued ||
                                 other.state == DownloadState::Downloading ||
                                 other.state == DownloadState::StreamPreparing ||
                                 other.state == DownloadState::StreamInstalling)) {
                                still_needed = true;
                                break;
                            }
                        }
                        if (!still_needed) {
                            clearTorrentCache(item.torrent_hash);
                        } else {
                            util::logLine("download: keeping torrent in cache as it is needed by another queued item, hash=" + item.torrent_hash);
                        }
                    }
                    util::logLine("download: hybrid install completed: " + item.title);
                }
                if (item.stream_consumer_started && item.torrent_id >= 0) {
                    stopStreamConsumer(item.torrent_id);
                    item.stream_consumer_started = false;
                }
                continue;
            }
        }

        if (item.state == DownloadState::StreamPreparing) {
            startHybridInstall(i);
            continue;
        }

        if (isLocalBackend() &&
            item.state == DownloadState::Downloading &&
            item.forced_file_index >= 0 &&
            isSwitchGameFile(item.forced_stream_name) &&
            !item.auto_hybrid_started &&
            !item.hybrid_installer) {
            item.preload_started = true;
            item.preload_file_index = item.forced_file_index;
            item.preload_stream_name = item.forced_stream_name;
            util::logLine("download: local client starts selected file without HTTP preload hash=" +
                          item.torrent_hash +
                          " file_index=" + std::to_string(item.preload_file_index));
            startHybridInstall(i);
            continue;
        }

        if (item.state == DownloadState::Downloading ||
            item.state == DownloadState::StreamInstalling ||
            item.state == DownloadState::StreamPreparing) {
            if (!item.priorities_set && !item.selected_files.empty()) {
                std::vector<torrent::TorrentFileInfo> files;
                bool got_files = false;
                if (item.torrent_id >= 0 && torrent_) {
                    got_files = torrent_->getTorrentFiles(item.torrent_id, files);
                } else if (!item.torrent_hash.empty() && isLocalBackend()) {
                    got_files = customEngineGetFiles(item.torrent_hash, files);
                }

                if (got_files && !files.empty()) {
                    for (const auto& f : files) {
                        bool wanted = std::find(item.selected_files.begin(), item.selected_files.end(), f.index) != item.selected_files.end();
                        if (item.torrent_id >= 0 && torrent_) {
                            setFileWanted(i, f.index, wanted);
                        } else if (!item.torrent_hash.empty() && isLocalBackend()) {
                            customEngineSetFileWanted(item.torrent_hash, f.index, wanted);
                        }

                        if (wanted && f.index == item.forced_file_index) {
                            item.install_total = f.size;
                        }
                    }
                    item.priorities_set = true;
                    util::logLine("download: configured file priorities and size=" + std::to_string(item.install_total) + " for " + item.title);
                }
            }
            bool matched = false;
            for (const auto& t : list) {
                bool match = false;
                if (item.torrent_id >= 0 && t.id == item.torrent_id) {
                    match = true;
                } else if (!item.torrent_hash.empty() && !t.hash.empty() &&
                           equalsIgnoreCase(item.torrent_hash, t.hash)) {
                    match = true;
                } else if (item.torrent_id < 0 && !item.title.empty() && t.name == item.title) {
                    match = true;
                }

                if (!match) {
                    continue;
                }

                matched = true;
                item.seeds = t.seeds;
                item.peers = t.peers;
                item.known_peers = t.known_peers;
                item.dht = t.dht;

                // Self-heal a pause that raced the stream open: the engine
                // torrent may have landed after pause_torrent() ran.
                if (item.state == DownloadState::Paused &&
                    !item.torrent_hash.empty()) {
                    tsnx_engine_pause_torrent(nullptr, item.torrent_hash.c_str());
                }

                // Only the actively transferring item takes the torrent's
                // progress/speed: several queued items can share the same
                // info-hash (base game + DLC), and they would otherwise all
                // display the running torrent's numbers.
                const bool active_transfer =
                    item.state == DownloadState::Downloading ||
                    item.state == DownloadState::StreamInstalling ||
                    item.state == DownloadState::StreamPreparing;

                if (active_transfer) {
                    // If hybrid installer is active, its downloadProgress() is already the overall progress.
                    // Otherwise, use the engine's per-torrent completion.
                    if (!item.hybrid_installer) {
                        item.progress = t.percent_done;
                    }

                    float sampled_speed_kbps = t.download_speed_kbps;
                    if (hybrid_speed_kbps >= 0.0f) {
                        const bool local_hybrid_stream =
                            isLocalBackend() &&
                            item.state == DownloadState::StreamInstalling &&
                            item.hybrid_installer != nullptr;

                        // In LocalClient mode, prefer the engine's actual network speed
                        // (t.download_speed_kbps) over the installer's internal reading speed
                        // (hybrid_speed_kbps), as the latter can drop to 0 when the ring buffer
                        // is full even if the download is active.
                        sampled_speed_kbps = local_hybrid_stream
                            ? t.download_speed_kbps
                            : std::max(sampled_speed_kbps, hybrid_speed_kbps);
                    }
                    if (refreshed_torrent_list ||
                        hybrid_speed_kbps >= 0.0f ||
                        item.speed_sample_at.time_since_epoch().count() == 0) {
                        item.download_speed_kbps = smoothDownloadSpeedKbps(item.download_speed_kbps,
                                                                           sampled_speed_kbps,
                                                                           item.speed_sample_at,
                                                                           now);
                        item.speed_sample_at = now;
                    }
                }
                if (item.torrent_hash.empty() && !t.hash.empty()) {
                    item.torrent_hash = t.hash;
                }

                if (!item.preload_started && item.torrent_id >= 0) {
                    int chosen_index = -1;
                    std::string chosen_stream_name;

                    if (item.forced_file_index >= 0) {
                        chosen_index = item.forced_file_index;
                        chosen_stream_name = item.forced_stream_name;
                    } else {
                        std::vector<torrent::TorrentFileInfo> files;
                        if (torrent_->getTorrentFiles(item.torrent_id, files) && !files.empty()) {
                            chooseInstallFile(files, chosen_index, chosen_stream_name);
                        }
                    }

                    if (chosen_index >= 0 &&
                        torrent_->preloadTorrentFile(item.torrent_id, chosen_index, chosen_stream_name)) {
                        item.preload_started = true;
                        item.preload_file_index = chosen_index;
                        item.preload_stream_name = chosen_stream_name;
                        util::logLine("download: preload started hash=" + item.torrent_hash +
                                      " file_index=" + std::to_string(chosen_index));
                    }
                }

                if (item.preload_started) {
                    if (item.state == DownloadState::Downloading &&
                        !item.auto_hybrid_started &&
                        !item.hybrid_installer &&
                        isSwitchGameFile(item.preload_stream_name)) {
                        if (startHybridInstall(i)) {
                            item.auto_hybrid_started = true;
                        }
                    }
                    if (item.state == DownloadState::Downloading &&
                        !item.hybrid_installer &&
                        isSwitchGameFile(item.preload_stream_name) &&
                        !isLocalBackend()) {
                        ensureStreamConsumer(item);
                    }
                }

                if (!item.hybrid_installer && !item.preload_started &&
                    !item.stream_ready && item.torrent_id >= 0) {
                    std::vector<std::string> paths;
                    std::string name;
                    if (torrent_->getStreamFiles(item.torrent_id, paths, name)) {
                        item.stream_ready = true;
                        item.stream_name = name;
                        item.stream_files.clear();
                        for (const auto& p : paths) {
                            DownloadItem::StreamFile sf;
                            sf.path = p;
                            sf.offset = 0;
                            item.stream_files.push_back(sf);
                        }
                        item.stream_index = 0;
                        item.installer.openStream(item.stream_name);
                        item.install_total = item.installer.totalSize();
                        item.install_written = item.installer.writtenSize();
                    }
                }

                if (!item.hybrid_installer && !item.preload_started &&
                    item.stream_ready && !item.stream_done) {
                    size_t max_bytes = 1024 * 1024;
                    if (item.stream_index < item.stream_files.size()) {
                        auto& sf = item.stream_files[item.stream_index];
                        uint64_t fsize = 0;
                        if (getFileSize(sf.path, fsize) && fsize > sf.offset) {
                            readChunkToInstaller(sf.path, sf.offset, item.installer, max_bytes);
                            item.installer.installChunk();
                            item.install_total = item.installer.totalSize();
                            item.install_written = item.installer.writtenSize();
                            if (item.install_total > 0) {
                                item.install_progress = (float)((double)item.install_written / (double)item.install_total);
                            }
                        } else if (getFileSize(sf.path, fsize) && fsize == sf.offset) {
                            if (item.progress >= 1.0f || fsize > 0) {
                                item.stream_index++;
                            }
                        }
                    }
                    if (item.installer.isComplete()) {
                        item.stream_done = true;
                        item.state = DownloadState::Installing;
                    }
                }

                if (!item.hybrid_installer && !item.preload_started &&
                    item.progress >= 1.0f && !item.stream_ready) {
                    item.state = DownloadState::Installing;
                }
                break;
            }

            if (!matched && item.preload_started &&
                item.state == DownloadState::Downloading &&
                !item.hybrid_installer) {
                ensureStreamConsumer(item);
            }

            if (!matched && hybrid_speed_kbps >= 0.0f) {
                item.download_speed_kbps = smoothDownloadSpeedKbps(item.download_speed_kbps,
                                                                   hybrid_speed_kbps,
                                                                   item.speed_sample_at,
                                                                   now);
                item.speed_sample_at = now;
            }

            if (!matched &&
                item.state == DownloadState::Downloading &&
                !item.auto_hybrid_started &&
                !item.hybrid_installer &&
                ds_manager_.mode() != datasource::DataSourceMode::LocalClient) {
                if (startHybridInstall(i)) {
                    item.auto_hybrid_started = true;
                }
            }
        } else if (item.state == DownloadState::Installing) {
            if (item.install_total > 0) {
                item.install_progress = (float)((double)item.install_written / (double)item.install_total);
            }
            if (item.stream_done || item.stream_ready || (!item.stream_ready && item.progress >= 1.0f)) {
                item.state = DownloadState::Completed;
                copyDownloadedOtherFiles(item);
                item.download_speed_kbps = 0.0f;
                item.speed_sample_at = std::chrono::steady_clock::time_point{};
                if (item.stream_consumer_started && item.torrent_id >= 0) {
                    stopStreamConsumer(item.torrent_id);
                    item.stream_consumer_started = false;
                }
            }
        }

        // Calculate and smooth installation speed
        if (item.state == DownloadState::StreamInstalling || item.state == DownloadState::Installing) {
            unsigned long long current_install_written = item.install_written;
            if (item.install_speed_sample_at.time_since_epoch().count() == 0) {
                item.install_speed_sample_at = now;
                item.last_install_written = current_install_written;
                item.install_speed_kbps = 0.0f;
            } else {
                double dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - item.install_speed_sample_at).count() / 1000.0;
                if (dt >= 0.5) { // update speed sample every 500ms or more
                    unsigned long long delta = 0;
                    if (current_install_written >= item.last_install_written) {
                        delta = current_install_written - item.last_install_written;
                    }
                    float sampled_speed_kbps = static_cast<float>(delta) / 1024.0f / dt;
                    item.install_speed_kbps = smoothDownloadSpeedKbps(item.install_speed_kbps,
                                                                      sampled_speed_kbps,
                                                                      item.install_speed_sample_at,
                                                                      now);
                    item.install_speed_sample_at = now;
                    item.last_install_written = current_install_written;
                }
            }
        } else {
            item.install_speed_kbps = 0.0f;
            item.install_speed_sample_at = std::chrono::steady_clock::time_point{};
            item.last_install_written = 0;
        }
    }

    bool has_active = false;
    bool has_queued = false;
    bool has_paused = false;
    for (const auto& item : queue_) {
        if (isTransferActive(item.state)) {
            has_active = true;
        } else if (item.state == DownloadState::Queued) {
            has_queued = true;
        } else if (item.state == DownloadState::Paused) {
            has_paused = true;
        }
    }

    if (!has_active && !has_paused && has_queued) {
        startNextDownload();
        has_active = hasActiveTransfers();
    }

    updateSleepPolicy(has_active);

}

bool DownloadManager::hasActiveTransfers() const {
    for (const auto& item : queue_) {
        if (isTransferActive(item.state)) {
            return true;
        }
    }
    return false;
}

bool DownloadManager::startHybridInstall(size_t index) {
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];

    if (item.state == DownloadState::Queued) {
        if (!startDownload(index)) {
            return false;
        }
    }

    if (item.state != DownloadState::Downloading &&
        item.state != DownloadState::StreamPreparing &&
        item.state != DownloadState::StreamInstalling) {
        util::logLine("download: hybrid install cannot start in current state=" +
                      std::to_string(static_cast<int>(item.state)));
        return false;
    }

    if (item.hybrid_installer && !item.hybrid_installer->isFinished() &&
        item.state != DownloadState::StreamPreparing) {
        return true;
    }

    if (item.torrent_hash.empty()) {
        item.torrent_hash = extractBtihHash(normalizeTorrentLink(item.magnet));
    }
    if (item.torrent_hash.empty()) {
        util::logLine("download: hybrid install missing torrent hash");
        return false;
    }

    const bool local_client_mode = (isLocalBackend());
    const bool has_remote_torrent = (!local_client_mode &&
                                     item.torrent_id >= 0 &&
                                     torrent_ != nullptr &&
                                     torrent_->isServerReachable());

    int install_file_index = item.preload_file_index;
    std::string install_stream_name = item.preload_stream_name;
    if (install_file_index < 0) {
        if (item.forced_file_index >= 0) {
            install_file_index = item.forced_file_index;
            install_stream_name = item.forced_stream_name;
        } else {
            if (has_remote_torrent) {
                std::vector<torrent::TorrentFileInfo> files;
                if (!torrent_->getTorrentFiles(item.torrent_id, files) || files.empty()) {
                    return false;
                }
                if (!chooseInstallFile(files, install_file_index, install_stream_name)) {
                    util::logLine("download: no installable NSP/NSZ/XCI file found in torrent");
                    return false;
                }
            } else {
                util::logLine("download: hybrid install requires an explicit file selection in local mode");
                return false;
            }
        }

        if (install_file_index < 0) {
            util::logLine("download: hybrid install invalid file index");
            return false;
        }
        item.preload_file_index = install_file_index;
        item.preload_stream_name = install_stream_name;
    }

    if (!item.preload_started) {
        if (has_remote_torrent) {
            if (torrent_->preloadTorrentFile(item.torrent_id, install_file_index, install_stream_name)) {
                item.preload_started = true;
                util::logLine("download: preload started for hybrid hash=" + item.torrent_hash +
                              " file_index=" + std::to_string(install_file_index));
            } else {
                util::logLine("download: hybrid install preload failed on remote, switching to internal stream");
                item.preload_started = true;
            }
        } else {
            item.preload_started = true;
            util::logLine("download: internal mode, preload skipped");
        }
    }

    if (item.stream_consumer_started && item.torrent_id >= 0) {
        stopStreamConsumer(item.torrent_id);
        item.stream_consumer_started = false;
    }

    auto* source = ds_manager_.getSource();
    if (!source) {
        util::logLine("download: data source is unavailable");
        return false;
    }

    if (item.state == DownloadState::Downloading) {
        if (!item.cancel_flag) {
            item.cancel_flag = std::make_shared<std::atomic<bool>>(false);
        }
        item.cancel_flag->store(false);
        source->setTorrentContext(item.torrent_hash, normalizeTorrentLink(item.magnet), "");

        item.state = DownloadState::StreamPreparing;
        util::logLine("download: starting async open for hybrid hash=" + item.torrent_hash +
                      " index=" + std::to_string(install_file_index));
        
        auto cancel_flag = item.cancel_flag;
        item.open_future = std::make_shared<std::future<bool>>(
            std::async(std::launch::async, [source, hash = item.torrent_hash, idx = install_file_index, cancel_flag]() {
                if (source) source->setCancelFlag(cancel_flag.get());
                return source ? source->open(hash, idx) : false;
            })
        );
        return true;
    }

    if (item.state == DownloadState::StreamPreparing) {
        if (item.start_future) {
            if (item.start_future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                return true; // Installer still preparing
            }

            bool started = item.start_future->get();
            item.start_future.reset();

            if (!started) {
                item.error_message = item.hybrid_installer ? item.hybrid_installer->errorMessage()
                                                           : "failed to start hybrid install";
                util::logLine("download: failed to start hybrid install: " + item.error_message);
                item.hybrid_installer.reset();
                item.state = DownloadState::Failed;
                return false;
            }

            item.auto_hybrid_started = true;
            item.state = DownloadState::StreamInstalling;
            util::logLine("download: hybrid install started for " + item.title +
                          " (index=" + std::to_string(install_file_index) + ")");
            return true;
        }

        if (!item.open_future) {
            item.state = DownloadState::Failed;
            item.error_message = "Stream open task lost.";
            return false;
        }

        if (item.open_future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return true; // Still preparing
        }

        bool success = item.open_future->get();
        item.open_future.reset();

        if (!success) {
            util::logLine("download: failed to open hybrid stream hash=" + item.torrent_hash +
                          " index=" + std::to_string(install_file_index));
            item.state = DownloadState::Failed;
            item.error_message = "Failed to open stream (metadata timeout or error).";
            return false;
        }

        item.hybrid_installer = std::make_unique<installer::HybridNspInstaller>();

        installer::InstallConfig config;
        config.buffer_size = 64 * 1024 * 1024;
        config.chunk_size = 8 * 1024 * 1024;
        config.verify_sha256 = true;
        config.install_ticket = true;
#ifdef __SWITCH__
        std::string loc = config::ConfigManager::instance().getInstallLocation();
        if (loc == "sd") {
            config.storage = NcmStorageId_SdCard;
        } else if (loc == "nand") {
            config.storage = NcmStorageId_BuiltInUser;
        } else { // "auto"
            s64 sdFree = 0;
            s64 nandFree = 0;
            uint64_t needed = source->totalSize();
            util::getStorageFreeSpace(1, sdFree);
            util::getStorageFreeSpace(0, nandFree);
            if (static_cast<s64>(needed) <= sdFree) {
                config.storage = NcmStorageId_SdCard;
            } else if (static_cast<s64>(needed) <= nandFree) {
                config.storage = NcmStorageId_BuiltInUser;
            } else {
                config.storage = NcmStorageId_SdCard; // Default fallback
            }
        }
#endif

        item.hybrid_installer->setSourceFileNameHint(install_stream_name);
        item.hybrid_installer->setHintTitleId(parseTitleIdFromFileName(install_stream_name));

        // Run start() off the progress thread: the header read blocks until the
        // engine delivers the first pieces, and the progress thread must keep
        // polling engine stats for the UI.
        auto installer_ptr = item.hybrid_installer;
        item.start_future = std::make_shared<std::future<bool>>(
            std::async(std::launch::async, [source, installer_ptr, config]() {
                return installer_ptr->start(source, config);
            }));
        return true;
    }

    return true;
}

bool DownloadManager::cancelDownload(size_t index) {
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];
    if (item.state == DownloadState::Completed ||
        item.state == DownloadState::Cancelled) return false;

    if (item.cancel_flag) {
        item.cancel_flag->store(true);
    }

    if (item.hybrid_installer) {
        item.hybrid_installer->cancel();
    }

    // Close the local engine's stream BEFORE joining the futures: the
    // installer's start() may be blocked inside tsnx_engine_read waiting for
    // a piece, and only the torrentfs teardown unblocks it. This also drops
    // the engine torrent + RAM window, which otherwise leaked for good.
    if (isLocalBackend()) {
        if (auto* source = ds_manager_.getSource()) {
            source->close();
            util::logLine("download: closed local data source after cancel hash=" +
                          item.torrent_hash);
        }
    }

    if (item.open_future) {
        item.open_future.reset();
    }

    if (item.start_future) {
        item.start_future.reset();
    }

    if (item.stream_consumer_started && item.torrent_id >= 0) {
        stopStreamConsumer(item.torrent_id);
        item.stream_consumer_started = false;
    }

    if (item.torrent_id >= 0 && torrent_) {
        torrent_->cancelTorrent(item.torrent_id);
    } else if (!item.torrent_hash.empty()) {
        bool still_needed = false;
        for (size_t j = 0; j < queue_.size(); ++j) {
            if (index == j) continue;
            const auto& other = queue_[j];
            std::string other_hash = other.torrent_hash;
            if (other_hash.empty() && !other.magnet.empty()) {
                other_hash = extractBtihHash(normalizeTorrentLink(other.magnet));
            }
            if (other_hash == item.torrent_hash &&
                (other.state == DownloadState::Queued ||
                 other.state == DownloadState::Downloading ||
                 other.state == DownloadState::StreamPreparing ||
                 other.state == DownloadState::StreamInstalling)) {
                still_needed = true;
                break;
            }
        }
        if (!still_needed) {
            clearTorrentCache(item.torrent_hash);
        } else {
            util::logLine("download: keeping torrent in cache on cancellation as it is needed by another queued item, hash=" + item.torrent_hash);
        }
    }
    item.state = DownloadState::Cancelled;
    item.download_speed_kbps = 0.0f;
    item.speed_sample_at = std::chrono::steady_clock::time_point{};
    return true;
}

bool DownloadManager::getTorrentFiles(size_t index, std::vector<torrent::TorrentFileInfo>& out_files) {
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];

    if (isLocalBackend()) {
        std::vector<datasource::CustomEngineFileInfo> local_files;
        std::string probe_err;
        if (!datasource::CustomEngineClient::instance().probeFiles(item.torrent_hash, item.magnet, "", local_files, &probe_err)) {
            util::logLine("download: local getTorrentFiles probe failed: " +
                          (probe_err.empty() ? std::string("unknown error") : probe_err));
            return false;
        }
        out_files.clear();
        out_files.reserve(local_files.size());
        for (const auto& lf : local_files) {
            torrent::TorrentFileInfo tf;
            tf.index = lf.index;
            tf.name = lf.path;
            tf.size = lf.size;
            tf.wanted = true;
            out_files.push_back(std::move(tf));
        }
        return !out_files.empty();
    }

    if (item.torrent_id < 0) return false;
    if (!torrent_) return false;
    return torrent_->getTorrentFiles(item.torrent_id, out_files);
}

bool DownloadManager::probeTorrentFiles(const std::string& magnet,
                                        std::vector<torrent::TorrentFileInfo>& out_files,
                                        std::string* out_error) {
    out_files.clear();
    if (out_error) out_error->clear();

    if (magnet.empty()) {
        if (out_error) *out_error = "Empty magnet link";
        return false;
    }

    const std::string link = normalizeTorrentLink(magnet);
    if (isLocalBackend()) {
        std::vector<datasource::CustomEngineFileInfo> local_files;
        std::string probe_err;
        if (!datasource::CustomEngineClient::instance().probeFiles("", link, "", local_files, &probe_err)) {
            util::logLine("download: local probe failed: " +
                          (probe_err.empty() ? std::string("unknown error") : probe_err));

            if (out_error) {
                *out_error = probe_err.empty()
                    ? "Failed to load torrent file list from custom engine"
                    : probe_err;
            }
            return false;
        }

        out_files.reserve(local_files.size());
        for (const auto& lf : local_files) {
            torrent::TorrentFileInfo tf;
            tf.index = lf.index;
            tf.name = lf.path;
            tf.size = lf.size;
            tf.wanted = true;
            out_files.push_back(std::move(tf));
        }

        return !out_files.empty();
    }

    int id = getTorrent()->addMagnet(link);
    if (id < 0) {
        if (out_error) *out_error = "Failed to add torrent";
        return false;
    }

    if (!torrent_ || !torrent_->isServerReachable()) {
        if (torrent_) torrent_->cancelTorrent(id);
        if (out_error) *out_error = "TorrServer is unavailable (file list cannot be fetched)";
        return false;
    }

    constexpr int kMaxAttempts = 72; // ~18s with 250ms polling.
    for (int i = 0; i < kMaxAttempts; ++i) {
        if (torrent_ && torrent_->getTorrentFiles(id, out_files) && !out_files.empty()) {
            if (torrent_) torrent_->cancelTorrent(id);
            return true;
        }
#ifdef __SWITCH__
        svcSleepThread(250000000LL);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
#endif
    }

    if (torrent_) torrent_->cancelTorrent(id);
    if (out_error) *out_error = "Timeout while loading torrent file list";
    return false;
}

bool DownloadManager::setFileWanted(size_t index, int file_index, bool wanted) {
    if (index >= queue_.size()) return false;
    auto& item = queue_[index];

    if (isLocalBackend()) {
        return customEngineSetFileWanted(item.torrent_hash, file_index, wanted);
    }

    if (item.torrent_id < 0) return false;
    if (!torrent_) return false;
    return torrent_->setFileWanted(item.torrent_id, file_index, wanted);
}

} // namespace download
