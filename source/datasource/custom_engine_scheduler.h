#pragma once

#include "range_mapper.h"

#include <engine/engine.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace datasource {

struct CustomSchedulerConfig {
    int critical_pieces    = 2;
    int urgent_pieces      = 8;
    int prefetch_pieces    = 10;
    int speculative_pieces = 6;
    int tail_pieces        = 2;

    int stall_extra_critical   = 0;
    int stall_extra_urgent     = -4;
    int stall_extra_prefetch   = -12;

    float slow_peer_speed_bps  = 100.0f * 1024.0f;
    int   slow_peer_count_max  = 3;
};

struct CustomSchedulerSnapshot {
    PieceRange critical;
    PieceRange urgent;
    PieceRange prefetch;
    PieceRange speculative;
    PieceRange tail;
    bool       stall_mode = false;
    int        slow_peer_count = 0;
};

class CustomEngineScheduler {
public:
    CustomEngineScheduler();
    explicit CustomEngineScheduler(const CustomSchedulerConfig& cfg);

    void init(tsnx_engine* engine,
              const std::string& hash,
              int piece_size,
              std::int64_t file_offset_in_torrent,
              int file_first_piece,
              int file_last_piece);

    CustomSchedulerSnapshot on_read_request(std::int64_t offset, std::int64_t size);
    CustomSchedulerSnapshot on_stall();
    CustomSchedulerSnapshot on_stall_recovered();
    CustomSchedulerSnapshot on_tick();
    void reset();

    bool in_stall_mode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stall_level_ > 0;
    }

    CustomSchedulerSnapshot last_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_snapshot_;
    }

    std::pair<int, int> currentUrgentPieceRange() const {
        std::lock_guard<std::mutex> lock(mutex_);
        int s = last_snapshot_.critical.start >= 0 ? last_snapshot_.critical.start
                                                   : last_snapshot_.urgent.start;
        int e = last_snapshot_.urgent.end >= 0 ? last_snapshot_.urgent.end
                                               : last_snapshot_.critical.end;
        return {s, e};
    }

private:
    CustomSchedulerSnapshot rebuild(int current_piece);
    int offset_to_piece(std::int64_t offset) const;
    void apply_zones(const CustomSchedulerSnapshot& snap);

    CustomSchedulerConfig cfg_;
    tsnx_engine* engine_ = nullptr;
    std::string hash_;
    int piece_size_ = 0;
    std::int64_t file_offset_in_torrent_ = 0;
    int file_first_piece_ = 0;
    int file_last_piece_ = 0;
    int last_current_piece_ = -1;
    int stall_level_ = 0;

    struct PeerEwma {
        uint32_t key_ip;
        uint16_t key_port;
        float ewma_speed_bps = 0.0f;
        bool  is_slow = false;
    };
    std::vector<PeerEwma> peer_ewma_;

    mutable std::mutex mutex_;
    CustomSchedulerSnapshot last_snapshot_;
};

} // namespace datasource
