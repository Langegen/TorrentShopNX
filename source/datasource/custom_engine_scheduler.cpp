#include "custom_engine_scheduler.h"

#include "../utils/log.h"

#include <algorithm>
#include <cstring>

namespace datasource {

namespace {

constexpr float EWMA_ALPHA = 0.25f;

PieceRange clamp_range(const PieceRange& r, int min_piece, int max_piece) {
    if (!r.valid() || max_piece < min_piece) return {};
    PieceRange out;
    out.start = std::max(r.start, min_piece);
    out.end   = std::min(r.end, max_piece);
    if (out.end < out.start) return {};
    return out;
}

} // namespace

CustomEngineScheduler::CustomEngineScheduler() = default;

CustomEngineScheduler::CustomEngineScheduler(const CustomSchedulerConfig& cfg)
    : cfg_(cfg) {}

void CustomEngineScheduler::init(tsnx_engine* engine,
                                 const std::string& hash,
                                 int piece_size,
                                 std::int64_t file_offset_in_torrent,
                                 int file_first_piece,
                                 int file_last_piece) {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = engine;
    hash_ = hash;
    piece_size_ = piece_size;
    file_offset_in_torrent_ = file_offset_in_torrent;
    file_first_piece_ = file_first_piece;
    last_current_piece_ = -1;
    last_log_piece_ = -1;
    stall_level_ = 0;
    last_boosted_count_ = 0;
    last_apply_at_ = {};
    last_log_at_ = {};
    last_boost_log_at_ = {};
    peer_ewma_.clear();
    last_snapshot_ = {};
}

int CustomEngineScheduler::offset_to_piece(std::int64_t offset) const {
    if (piece_size_ <= 0) return file_first_piece_;
    std::int64_t abs = file_offset_in_torrent_ + offset;
    int p = static_cast<int>(abs / piece_size_);
    if (p < file_first_piece_) p = file_first_piece_;
    if (p > file_last_piece_)  p = file_last_piece_;
    return p;
}

CustomSchedulerSnapshot CustomEngineScheduler::rebuild(int current_piece) {
    CustomSchedulerSnapshot snap;
    snap.stall_mode = stall_level_ > 0;

    int urgent_count = cfg_.urgent_pieces +
        (stall_level_ > 0 ? cfg_.stall_extra_urgent : 0);
    if (urgent_count < 2) urgent_count = 2;

    int prefetch_count = cfg_.prefetch_pieces +
        (stall_level_ > 0 ? cfg_.stall_extra_prefetch : 0);
    if (prefetch_count < 2) prefetch_count = 2;

    int speculative_count = cfg_.speculative_pieces;
    if (stall_level_ > 0) speculative_count = 0;

    // Critical: current piece and the next one.
    snap.critical.start = current_piece;
    snap.critical.end   = std::min(file_last_piece_, current_piece + cfg_.critical_pieces - 1);

    // Urgent: just ahead.
    snap.urgent.start = snap.critical.end + 1;
    snap.urgent.end   = std::min(file_last_piece_, snap.urgent.start + urgent_count - 1);

    // Prefetch: sequential read-ahead.
    snap.prefetch.start = snap.urgent.end + 1;
    snap.prefetch.end   = std::min(file_last_piece_, snap.prefetch.start + prefetch_count - 1);

    // Speculative: further ahead.
    snap.speculative.start = snap.prefetch.end + 1;
    snap.speculative.end   = std::min(file_last_piece_,
                                      snap.speculative.start + speculative_count - 1);

    // Tail: a few pieces behind the playhead for cache/seek resilience.
    snap.tail.start = std::max(file_first_piece_, current_piece - cfg_.tail_pieces);
    snap.tail.end   = std::max(file_first_piece_, current_piece - 1);
    if (snap.tail.end < snap.tail.start) snap.tail = {};

    snap.critical    = clamp_range(snap.critical,    file_first_piece_, file_last_piece_);
    snap.urgent      = clamp_range(snap.urgent,      file_first_piece_, file_last_piece_);
    snap.prefetch    = clamp_range(snap.prefetch,    file_first_piece_, file_last_piece_);
    snap.speculative = clamp_range(snap.speculative, file_first_piece_, file_last_piece_);
    snap.tail        = clamp_range(snap.tail,        file_first_piece_, file_last_piece_);

    last_snapshot_ = snap;
    return snap;
}

void CustomEngineScheduler::apply_zones(const CustomSchedulerSnapshot& snap) {
    if (!engine_ || hash_.empty()) return;

    tsnx_engine_clear_piece_zones(engine_, hash_.c_str());

    auto apply = [&](const PieceRange& r, tsnx_piece_zone zone) {
        if (!r.valid()) return;
        tsnx_engine_set_piece_zone(engine_, hash_.c_str(), r.start,
                                   r.end - r.start + 1, zone);
    };

    apply(snap.critical,    TSNX_ZONE_CRITICAL);
    apply(snap.urgent,      TSNX_ZONE_URGENT);
    apply(snap.prefetch,    TSNX_ZONE_PREFETCH);
    apply(snap.speculative, TSNX_ZONE_SPECULATIVE);
    apply(snap.tail,        TSNX_ZONE_TAIL);
}

CustomSchedulerSnapshot CustomEngineScheduler::on_read_request(std::int64_t offset,
                                                                std::int64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) return {};

    int current_piece = offset_to_piece(offset);

    // The engine now streams partial pieces, so reads come back at block
    // granularity (as little as 16 KB) instead of one 4 MB chunk per piece.
    // Rebuilding and re-applying the zones -- and logging -- on every one of
    // those is pure noise; the zones are piece-granular anyway, so they only
    // change when the piece moves on. Rebuild at most once per second.
    const auto now = std::chrono::steady_clock::now();
    if (last_current_piece_ == current_piece &&
        last_apply_at_.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_apply_at_).count() < 1000) {
        return last_snapshot_;
    }
    last_apply_at_ = now;
    last_current_piece_ = current_piece;

    CustomSchedulerSnapshot snap = rebuild(current_piece);
    apply_zones(snap);

    bool should_log = (last_log_piece_ != current_piece) ||
                      (last_log_at_.time_since_epoch().count() == 0) ||
                      (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_at_).count() >= 5000);
    if (should_log) {
        last_log_piece_ = current_piece;
        last_log_at_ = now;
        std::string msg = "scheduler: current=" + std::to_string(current_piece) +
            " critical=" + std::to_string(snap.critical.start) + ".." + std::to_string(snap.critical.end) +
            " urgent=" + std::to_string(snap.urgent.start) + ".." + std::to_string(snap.urgent.end);
        util::logLine(msg);
    }

    return snap;
}

CustomSchedulerSnapshot CustomEngineScheduler::on_stall() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) return {};
    stall_level_ = 1;
    CustomSchedulerSnapshot snap = last_current_piece_ >= 0
        ? rebuild(last_current_piece_)
        : CustomSchedulerSnapshot{};
    apply_zones(snap);
    util::logLine("scheduler: entered stall mode");
    return snap;
}

CustomSchedulerSnapshot CustomEngineScheduler::on_stall_recovered() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) return {};
    stall_level_ = 0;
    CustomSchedulerSnapshot snap = last_current_piece_ >= 0
        ? rebuild(last_current_piece_)
        : CustomSchedulerSnapshot{};
    apply_zones(snap);
    util::logLine("scheduler: stall recovered");
    return snap;
}

CustomSchedulerSnapshot CustomEngineScheduler::on_tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_ || hash_.empty()) return {};

    tsnx_peer_info peers[64];
    int n = tsnx_engine_get_peers(engine_, hash_.c_str(), peers, 64);

    int slow_count = 0;
    for (int i = 0; i < n; i++) {
        const auto& p = peers[i];
        if (!p.handshaked) continue;

        auto it = std::find_if(peer_ewma_.begin(), peer_ewma_.end(),
            [&](const PeerEwma& e) { return e.key_ip == p.ip && e.key_port == p.port; });
        if (it == peer_ewma_.end()) {
            PeerEwma e;
            e.key_ip   = p.ip;
            e.key_port = p.port;
            e.ewma_speed_bps = static_cast<float>(p.rate_bps);
            peer_ewma_.push_back(e);
            it = peer_ewma_.end() - 1;
        } else {
            it->ewma_speed_bps = it->ewma_speed_bps * (1.0f - EWMA_ALPHA) +
                                 static_cast<float>(p.rate_bps) * EWMA_ALPHA;
        }
        it->is_slow = it->ewma_speed_bps < cfg_.slow_peer_speed_bps;
        if (it->is_slow) slow_count++;
    }

    // Trim stale entries not seen recently.
    peer_ewma_.erase(
        std::remove_if(peer_ewma_.begin(), peer_ewma_.end(),
            [&](const PeerEwma& e) {
                for (int i = 0; i < n; i++) {
                    if (peers[i].ip == e.key_ip && peers[i].port == e.key_port)
                        return false;
                }
                return true;
            }),
        peer_ewma_.end());

    last_snapshot_.slow_peer_count = slow_count;

    // Slow-peer isolation: boost any piece held by a slow peer back to Critical
    // so the internal fast-peer steering will take it over quickly.
    int boosted = 0;
    for (int i = 0; i < n && slow_count > 0; i++) {
        if (!peers[i].handshaked || peers[i].claim_piece < 0) continue;
        auto it = std::find_if(peer_ewma_.begin(), peer_ewma_.end(),
            [&](const PeerEwma& e) { return e.key_ip == peers[i].ip && e.key_port == peers[i].port; });
        if (it != peer_ewma_.end() && it->is_slow) {
            tsnx_engine_set_piece_zone(engine_, hash_.c_str(),
                                       static_cast<int>(peers[i].claim_piece), 1,
                                       TSNX_ZONE_CRITICAL);
            boosted++;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    bool should_log_boost = (boosted > 0) &&
        ((boosted != last_boosted_count_) ||
         (last_boost_log_at_.time_since_epoch().count() == 0) ||
         (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_boost_log_at_).count() >= 5000));
    if (should_log_boost) {
        last_boosted_count_ = boosted;
        last_boost_log_at_ = now;
        util::logLine("scheduler: boosted " + std::to_string(boosted) +
                      " slow-peer claims to Critical");
    } else if (boosted == 0) {
        last_boosted_count_ = 0;
    }

    return last_snapshot_;
}

void CustomEngineScheduler::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (engine_ && !hash_.empty()) {
        tsnx_engine_clear_piece_zones(engine_, hash_.c_str());
    }
    last_current_piece_ = -1;
    last_log_piece_ = -1;
    stall_level_ = 0;
    last_boosted_count_ = 0;
    last_apply_at_ = {};
    last_log_at_ = {};
    last_boost_log_at_ = {};
    peer_ewma_.clear();
    last_snapshot_ = {};
}

} // namespace datasource
