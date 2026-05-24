#pragma once

#include <algorithm>
#include <cstdint>

namespace datasource {

struct PieceRange {
    int start = -1;
    int end = -1;

    bool valid() const { return start >= 0 && end >= start; }
    int count() const { return valid() ? (end - start + 1) : 0; }
    bool contains(int piece) const { return valid() && piece >= start && piece <= end; }
};

inline PieceRange map_bytes_to_pieces(std::int64_t offset,
                                      std::int64_t size,
                                      int piece_size) {
    PieceRange out{};
    if (piece_size <= 0 || size <= 0 || offset < 0) {
        return out;
    }

    const std::int64_t start = offset / piece_size;
    const std::int64_t end = (offset + size - 1) / piece_size;
    out.start = static_cast<int>(start);
    out.end = static_cast<int>(end);
    return out;
}

inline PieceRange clamp_piece_range(const PieceRange& range, int min_piece, int max_piece) {
    if (!range.valid() || max_piece < min_piece) {
        return {};
    }
    PieceRange out;
    out.start = std::max(range.start, min_piece);
    out.end = std::min(range.end, max_piece);
    if (out.end < out.start) {
        return {};
    }
    return out;
}

inline PieceRange make_urgent_window(int piece_start, int urgent_count, int max_piece) {
    if (piece_start < 0 || urgent_count <= 0 || max_piece < 0) {
        return {};
    }
    PieceRange out;
    out.start = piece_start;
    out.end = std::min(max_piece, piece_start + urgent_count - 1);
    return out;
}

inline PieceRange make_readahead_window(int piece_start, int readahead_count, int max_piece) {
    if (piece_start < 0 || readahead_count <= 0 || max_piece < 0) {
        return {};
    }
    PieceRange out;
    out.start = piece_start;
    out.end = std::min(max_piece, piece_start + readahead_count - 1);
    return out;
}

inline PieceRange make_tail_window(int piece_start, int tail_count, int min_piece) {
    if (piece_start < 0 || tail_count <= 0) {
        return {};
    }
    PieceRange out;
    out.start = std::max(min_piece, piece_start - tail_count);
    out.end = std::max(min_piece, piece_start - 1);
    if (out.end < out.start) {
        return {};
    }
    return out;
}

} // namespace datasource

