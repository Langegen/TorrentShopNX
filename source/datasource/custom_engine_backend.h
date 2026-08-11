#pragma once
// =============================================================================
// CustomEngineBackend — Локальный бэкенд на базе собственного лёгкого движка.
//
// Движок живёт в source/engine/ и предоставляет C API через engine/engine.h.
// =============================================================================

#ifdef TSNX_USE_CUSTOM_ENGINE

#include "i_content_backend.h"
#include "custom_engine_scheduler.h"
#include "custom_engine_health.h"

#include <engine/engine.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace datasource {

class CustomEngineBackend : public IContentBackend {
public:
    explicit CustomEngineBackend(const BackendConfig& cfg);
    ~CustomEngineBackend() override;

    bool open(const ContentRequest& request) override;
    bool prebuffer(std::int64_t offset, std::int64_t size) override;
    std::int64_t read(std::int64_t offset, void* buffer, std::int64_t size) override;
    BackendStatus status() const override;
    bool isAvailable() const override;
    void notifyStreamingComplete(bool success) override;
    BackendType type() const override { return BackendType::CustomEngine; }
    void close() override;
    int downloadSpeedKBps() const override;
    int pieceSize() const override;
    uint64_t fileOffsetInTorrent() const override;

private:
    void set_state(StreamState new_state, const std::string& detail = "");
    bool ensure_engine();

    BackendConfig cfg_;
    mutable std::mutex mutex_;

    StreamState state_ = StreamState::Idle;
    BackendStatus status_{};
    bool opened_ = false;

    std::string info_hash_str_;
    std::string magnet_link_;
    std::string torrent_file_path_;
    int file_index_ = -1;

    std::int64_t file_size_ = 0;
    std::int64_t file_offset_in_torrent_ = 0;
    int piece_size_ = 0;
    int file_first_piece_ = 0;
    int file_last_piece_ = 0;

    CustomEngineScheduler scheduler_;
    CustomEngineHealth health_;
    mutable std::chrono::steady_clock::time_point last_scheduler_tick_{};
    std::atomic<bool> starving_{false};

    std::chrono::steady_clock::time_point open_time_{};

    static tsnx_engine* engine_;
    static std::mutex engine_mutex_;
    static int engine_users_;
};

} // namespace datasource

#endif // TSNX_USE_CUSTOM_ENGINE
