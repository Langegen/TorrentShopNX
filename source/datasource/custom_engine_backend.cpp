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

CustomEngineBackend::CustomEngineBackend(const BackendConfig& cfg) : cfg_(cfg) {
    status_.state = StreamState::Idle;
}

CustomEngineBackend::~CustomEngineBackend() {
    close();
}

bool CustomEngineBackend::ensure_engine() {
    // The engine is shared with the file-list probe (CustomEngineClient), so a
    // torrent probed moments ago is still registered here and the download can
    // start without a second metadata fetch.
    engine_ = CustomEngineClient::instance().sharedEngine();
    if (!engine_) {
        util::logLine("CustomEngineBackend: failed to get shared engine");
        return false;
    }
    return true;
}

void CustomEngineBackend::set_state(StreamState new_state, const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    set_state_locked(new_state, detail);
}

void CustomEngineBackend::set_state_locked(StreamState new_state, const std::string& detail) {
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
        set_state_locked(StreamState::Error, "engine start failed");
        return false;
    }

    info_hash_str_ = request.info_hash;
    magnet_link_ = request.magnet_link;
    torrent_file_path_ = request.torrent_file_path;
    file_index_ = request.file_index;

    const volatile bool* cancel =
        reinterpret_cast<const volatile bool*>(cancel_flag_);

    char hash[41] = {0};
    if (!info_hash_str_.empty() &&
        tsnx_engine_has_torrent(engine_, info_hash_str_.c_str())) {
        // The probe kept this torrent registered: metadata is already known,
        // so prepare_stream() below opens the download instantly.
        snprintf(hash, sizeof(hash), "%s", info_hash_str_.c_str());
        util::logLine("CustomEngineBackend: reusing probed torrent " + info_hash_str_);
    } else {
        bool added = false;
        if (!torrent_file_path_.empty()) {
            added = tsnx_engine_add_torrent_file(engine_, torrent_file_path_.c_str(),
                                                 hash, sizeof(hash));
        } else if (!magnet_link_.empty()) {
            added = tsnx_engine_add_magnet_ex(engine_, magnet_link_.c_str(),
                                              file_index_, false, cancel,
                                              hash, sizeof(hash));
        } else {
            set_state_locked(StreamState::Error, "no magnet or torrent file");
            return false;
        }

        if (!added) {
            set_state_locked(StreamState::Error, "add torrent failed");
            return false;
        }
    }

    if (info_hash_str_.empty()) {
        info_hash_str_ = hash;
    }

    // The slot survives the probe cleanup and this download owns it now.
    CustomEngineClient::instance().markInUse(info_hash_str_);
    // The probe's cancel flag may have been tripped by the file-select view
    // closing; from here on the slot follows the download's own flag.
    tsnx_engine_set_cancel(engine_, info_hash_str_.c_str(), cancel);

    if (!tsnx_engine_prepare_stream(engine_, info_hash_str_.c_str(), file_index_)) {
        // The slot was just added: do not leave a half-open torrent behind.
        tsnx_engine_remove_torrent(engine_, info_hash_str_.c_str());
        CustomEngineClient::instance().unmarkInUse(info_hash_str_);
        set_state_locked(StreamState::Error, "prepare stream failed");
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
    // The status snapshot feeds BackendDataSource::totalSize(); leave it 0 and
    // XCI-installers and the UI see an unknown size.
    status_.total_size = static_cast<uint64_t>(file_size_);
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
    set_state_locked(StreamState::MainBuffering);
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
                                        offset, buffer, size);
    if (got < 0) {
        set_state(StreamState::Error, "read error");
        return -1;
    }
    if (got == 0) {
        starving_ = true;
        return 0;
    }

    starving_ = false;
    set_state(StreamState::StreamingOrInstalling);

    /* Keep the RAM window following the installer. */
    tsnx_engine_set_min_keep_offset(engine_, info_hash_str_.c_str(),
                                    file_offset_in_torrent_ + offset + got);
    return got;
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) return;
    scheduler_.reset();
    health_.reset();
    if (!info_hash_str_.empty()) {
        tsnx_engine_cancel_read(engine_, info_hash_str_.c_str());
        tsnx_engine_remove_torrent(engine_, info_hash_str_.c_str());
        CustomEngineClient::instance().unmarkInUse(info_hash_str_);
    }
    opened_ = false;
    state_ = StreamState::Idle;
    status_.state = StreamState::Idle;
    // The engine itself stays alive: it is owned by CustomEngineClient and
    // shared with the probe.
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
