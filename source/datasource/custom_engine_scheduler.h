#pragma once

#include "range_mapper.h"

#include <engine/engine.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace datasource {

struct CustomSchedulerConfig {
    // torrserver's setLoadPriority gradient (anacrolix piece priorities):
    // Now (the reader's piece) > Next (reader+1) > Readahead (4 pieces, like
    // torrserver's readahead = pieceLength*4) > High (5) > Normal (the rest of
    // the ConnectionsLimit=25 budget). Every unchoked connection scans this
    // gradient top-down, so the reader's piece gets the whole swarm's
    // in-flight capacity before the next piece gets any.
    int critical_pieces    = 1;    // Now
    int urgent_pieces      = 1;    // Next
    int prefetch_pieces    = 4;    // Readahead
    int speculative_pieces = 5;    // High
    int normal_pieces      = 14;   // Normal (fills the 25-piece budget)
    int tail_pieces        = 0;    // a sequential installer never reads back

    int stall_extra_critical   = 0;
    int stall_extra_urgent     = 1;
    int stall_extra_prefetch   = 0;

    float slow_peer_speed_bps  = 100.0f * 1024.0f;
    int   slow_peer_count_max  = 3;
};

struct CustomSchedulerSnapshot {
    PieceRange critical;
    PieceRange urgent;
    PieceRange prefetch;
    PieceRange speculative;
    PieceRange normal;
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
    int last_log_piece_ = -1;
    int stall_level_ = 0;
    int last_boosted_count_ = 0;
    std::chrono::steady_clock::time_point last_apply_at_;
    std::chrono::steady_clock::time_point last_log_at_;
    std::chrono::steady_clock::time_point last_boost_log_at_;

    struct PeerEwma {
        uint32_t key_ip;
        uint16_t key_port;
        float ewma_speed_bps = 0.0f;
        bool  is_slow = false;
    };
    std::vector<PeerEwma> peer_ewma_;
    std::vector<int> boosted_pieces_;

    mutable std::mutex mutex_;
    CustomSchedulerSnapshot last_snapshot_;
};

} // namespace datasource
