#ifndef TSNX_PERF_HIST_H
#define TSNX_PERF_HIST_H

// Tiny fixed-size latency histogram for the [perf] log lines (p50/p95/max).
// Log-linear buckets: exact below 16 us, then 4 buckets per power of two, so a
// percentile is reported within ~25% of the true value. No allocation, no
// locks -- each histogram has a single writer.

#include <stdint.h>
#include <string.h>

#define PERF_HIST_BUCKETS 104   // covers up to 2^26 us (~67 s)

typedef struct {
    uint32_t b[PERF_HIST_BUCKETS];
    uint32_t n;
    uint64_t max;
    uint64_t sum;
} perf_hist;

static inline int perf_hist_bucket(uint64_t v) {
    if (v < 16) return (int)v;
    int o = 63 - __builtin_clzll(v);           // floor(log2(v)), >= 4
    int sub = (int)((v >> (o - 2)) & 3);
    int idx = 16 + (o - 4) * 4 + sub;
    return idx < PERF_HIST_BUCKETS ? idx : PERF_HIST_BUCKETS - 1;
}

// Upper bound of a bucket's range (what a percentile reports).
static inline uint64_t perf_hist_upper(int idx) {
    if (idx < 16) return (uint64_t)idx;
    int o = (idx - 16) / 4 + 4;
    int sub = (idx - 16) % 4;
    return ((uint64_t)(4 + sub + 1) << (o - 2)) - 1;
}

static inline void perf_hist_reset(perf_hist *h) { memset(h, 0, sizeof(*h)); }

static inline void perf_hist_add(perf_hist *h, uint64_t v) {
    h->b[perf_hist_bucket(v)]++;
    h->n++;
    h->sum += v;
    if (v > h->max) h->max = v;
}

// q in (0,1]: the value below which a fraction q of the samples fall.
static inline uint64_t perf_hist_pct(const perf_hist *h, double q) {
    if (h->n == 0) return 0;
    uint32_t target = (uint32_t)((double)h->n * q + 0.5);
    if (target < 1) target = 1;
    uint32_t acc = 0;
    for (int i = 0; i < PERF_HIST_BUCKETS; i++) {
        acc += h->b[i];
        if (acc >= target) {
            uint64_t u = perf_hist_upper(i);
            return u < h->max ? u : h->max;
        }
    }
    return h->max;
}

// Samples at or above `v` (e.g. frames slower than 33 ms). Bucket-granular:
// counts every bucket whose range starts at or above v's bucket.
static inline uint32_t perf_hist_count_ge(const perf_hist *h, uint64_t v) {
    uint32_t c = 0;
    for (int i = perf_hist_bucket(v); i < PERF_HIST_BUCKETS; i++) c += h->b[i];
    return c;
}

#endif
