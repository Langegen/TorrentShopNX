#include "custom_engine_backend.h"

#ifdef TSNX_USE_CUSTOM_ENGINE

#include "../utils/log.h"
#include "range_mapper.h"

#include <engine/engine.h>

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

namespace datasource {

tsnx_engine* CustomEngineBackend::engine_ = nullptr;
std::mutex CustomEngineBackend::engine_mutex_;
int CustomEngineBackend::engine_users_ = 0;

CustomEngineBackend::CustomEngineBackend(const BackendConfig& cfg) : cfg_(cfg) {
    status_.state = StreamState::Idle;
}

CustomEngineBackend::~CustomEngineBackend() {
    close();
}

bool CustomEngineBackend::ensure_engine() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (!engine_) {
        engine_ = tsnx_engine_start(cfg_.local_port);
        if (!engine_) {
            util::logLine("CustomEngineBackend: failed to start engine");
            return false;
        }
    }
    engine_users_++;
    return true;
}

void CustomEngineBackend::set_state(StreamState new_state, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != new_state) {
        StreamState old = state_;
        state_ = new_state;
        status_.state = new_state;
        status_.detail = detail;
        onPhaseTransition(old, new_state);
    }
}

bool CustomEngineBackend::open(const ContentRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) return false;

    if (!ensure_engine()) {
        set_state(StreamState::Error, "engine start failed");
        return false;
    }

    info_hash_str_ = request.info_hash;
    magnet_link_ = request.magnet_link;
    torrent_file_path_ = request.torrent_file_path;
    file_index_ = request.file_index;

    char hash[41] = {0};
    bool added = false;
    if (!torrent_file_path_.empty()) {
        added = tsnx_engine_add_torrent_file(engine_, torrent_file_path_.c_str(), hash, sizeof(hash));
    } else if (!magnet_link_.empty()) {
        added = tsnx_engine_add_magnet(engine_, magnet_link_.c_str(), hash, sizeof(hash));
    } else {
        set_state(StreamState::Error, "no magnet or torrent file");
        return false;
    }

    if (!added) {
        set_state(StreamState::Error, "add torrent failed");
        return false;
    }

    if (info_hash_str_.empty()) {
        info_hash_str_ = hash;
    }

    if (!tsnx_engine_prepare_stream(engine_, info_hash_str_.c_str(), file_index_)) {
        set_state(StreamState::Error, "prepare stream failed");
        return false;
    }

    file_size_ = 0;
    if (file_index_ >= 0 && file_index_ < TSNX_MAX_FILES) {
        // Heap-backed: tsnx_file_info is ~536 bytes; a stack array would
        // overflow the small (64 KB default) libnx pthread stacks.
        std::vector<tsnx_file_info> files(TSNX_MAX_FILES);
        int count = tsnx_engine_get_files(engine_, info_hash_str_.c_str(), files.data(), TSNX_MAX_FILES);
        if (file_index_ < count) {
            file_size_ = files[file_index_].size;
            file_offset_in_torrent_ = files[file_index_].offset;
        }
    }
    if (file_size_ == 0) {
        file_offset_in_torrent_ = tsnx_engine_file_offset(engine_, info_hash_str_.c_str());
    }
    piece_size_ = tsnx_engine_piece_size(engine_, info_hash_str_.c_str());
    if (piece_size_ > 0 && file_size_ > 0) {
        file_first_piece_ = static_cast<int>(file_offset_in_torrent_ / piece_size_);
        file_last_piece_  = static_cast<int>((file_offset_in_torrent_ + file_size_ - 1) / piece_size_);
    } else if (piece_size_ > 0) {
        file_first_piece_ = static_cast<int>(file_offset_in_torrent_ / piece_size_);
        file_last_piece_  = file_first_piece_;
    }

    scheduler_.init(engine_, info_hash_str_, piece_size_, file_offset_in_torrent_,
                    file_first_piece_, file_last_piece_);
    health_.init(engine_, info_hash_str_, nullptr);
    last_scheduler_tick_ = std::chrono::steady_clock::now();

    opened_ = true;
    open_time_ = std::chrono::steady_clock::now();
    set_state(StreamState::MainBuffering);
    return true;
}

bool CustomEngineBackend::prebuffer(std::int64_t offset, std::int64_t size) {
    if (!opened_) return false;
    scheduler_.on_read_request(offset, size);
    return true;
}

std::int64_t CustomEngineBackend::read(std::int64_t offset, void* buffer, std::int64_t size) {
    if (!opened_ || !buffer || size <= 0) return -1;

    scheduler_.on_read_request(offset, size);

    std::int64_t total = 0;
    char* out = static_cast<char*>(buffer);
    int empty_loops = 0;

    while (total < size) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_scheduler_tick_).count() >= 1) {
            last_scheduler_tick_ = now;
            scheduler_.on_tick();
            float kbps = static_cast<float>(downloadSpeedKBps());
            tsnx_torrent_item items[8];
            int peers = 0;
            int n = tsnx_engine_get_torrents(engine_, items, 8);
            for (int i = 0; i < n; i++) {
                if (info_hash_str_ == items[i].hash) { peers = items[i].peers; break; }
            }
            health_.on_tick(peers, kbps, starving_.load());
        }

        std::int64_t got = tsnx_engine_read(engine_, info_hash_str_.c_str(),
                                            offset + total, out + total, size - total);
        if (got < 0) {
            set_state(StreamState::Error, "read error");
            return -1;
        }
        if (got == 0) {
            empty_loops++;
            starving_ = empty_loops > 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        empty_loops = 0;
        starving_ = false;
        total += got;
        set_state(StreamState::StreamingOrInstalling);

        /* Keep the RAM window following the installer. */
        tsnx_engine_set_min_keep_offset(engine_, info_hash_str_.c_str(),
                                        file_offset_in_torrent_ + offset + total);
    }

    return total;
}

BackendStatus CustomEngineBackend::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BackendStatus s = status_;

    tsnx_torrent_item items[8];
    int n = tsnx_engine_get_torrents(engine_, items, 8);
    for (int i = 0; i < n; i++) {
        if (info_hash_str_ == items[i].hash) {
            s.peers = items[i].peers;
            s.seeds = items[i].seeds;
            s.known_peers = items[i].known_peers;
            break;
        }
    }

    if (piece_size_ > 0) {
        auto urgent = scheduler_.currentUrgentPieceRange();
        s.urgent_start = urgent.first;
        s.urgent_end   = urgent.second;
        s.readahead_start = s.urgent_end + 1;
        s.readahead_end   = s.readahead_start + cfg_.readahead_pieces;
        s.tail_start = -1;
        s.tail_end = -1;
    }

    s.detail = health_.summaryString();
    if (scheduler_.in_stall_mode()) {
        s.detail += " [STALL]";
    }

    return s;
}

bool CustomEngineBackend::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_ && state_ != StreamState::Error;
}

void CustomEngineBackend::notifyStreamingComplete(bool success) {
    (void)success;
    close();
}

void CustomEngineBackend::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_) return;
        scheduler_.reset();
        health_.reset();
        if (!info_hash_str_.empty()) {
            tsnx_engine_cancel_read(engine_, info_hash_str_.c_str());
            tsnx_engine_remove_torrent(engine_, info_hash_str_.c_str());
        }
        opened_ = false;
        state_ = StreamState::Idle;
        status_.state = StreamState::Idle;
    }

    std::lock_guard<std::mutex> lock(engine_mutex_);
    if (engine_users_ > 0) engine_users_--;
    if (engine_users_ == 0 && engine_) {
        tsnx_engine_stop(engine_);
        engine_ = nullptr;
    }
}

int CustomEngineBackend::downloadSpeedKBps() const {
    tsnx_torrent_item items[8];
    int n = tsnx_engine_get_torrents(engine_, items, 8);
    for (int i = 0; i < n; i++) {
        if (info_hash_str_ == items[i].hash) {
            return static_cast<int>(items[i].download_kbps);
        }
    }
    return 0;
}

int CustomEngineBackend::pieceSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return piece_size_;
}

uint64_t CustomEngineBackend::fileOffsetInTorrent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint64_t>(file_offset_in_torrent_);
}

} // namespace datasource

#endif // TSNX_USE_CUSTOM_ENGINE
