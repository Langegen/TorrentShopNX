#include "perf_stats.h"

#include "log.h"
#include "../engine/perf_hist.h"
#include "../engine/engine_log.h"

#include <cstdio>
#include <functional>
#include <mutex>

namespace perf {

namespace {

constexpr uint64_t kWindowUs = 5ULL * 1000 * 1000;       // busy: one line per 5 s
constexpr uint64_t kIdleWindowUs = 60ULL * 1000 * 1000;  // nothing going on: per 60 s
constexpr uint64_t kMaxFrameGapUs = 10ULL * 1000 * 1000; // longer = app was suspended

// Frame state: UI thread only, no lock.
perf_hist g_frames;
uint64_t g_last_frame_us = 0;
uint64_t g_window_start_us = 0;

// Shared by the progress thread, the UI thread and the image workers.
std::mutex g_mtx;
struct LockStats {
    uint32_t n = 0;
    uint64_t hold_max = 0, hold_sum = 0, wait_max = 0;
};
LockStats g_locks[static_cast<int>(LockSite::Count)];
perf_hist g_img_disk;
perf_hist g_img_net;

const char* siteName(int i) {
    switch (static_cast<LockSite>(i)) {
        case LockSite::TrackProgress: return "track";
        case LockSite::UiAdd: return "add";
        default: return "?";
    }
}

void emit(uint64_t now_us) {
    LockStats locks[static_cast<int>(LockSite::Count)];
    perf_hist disk, net;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        for (int i = 0; i < static_cast<int>(LockSite::Count); i++) {
            locks[i] = g_locks[i];
            g_locks[i] = LockStats{};
        }
        disk = g_img_disk;
        net = g_img_net;
        perf_hist_reset(&g_img_disk);
        perf_hist_reset(&g_img_net);
    }

    const double secs = static_cast<double>(now_us - g_window_start_us) / 1e6;
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "[perf] ui window=%.0fs frames=%u fps=%.1f frame_ms p50=%.1f p95=%.1f max=%.1f "
        "jank33=%u freeze100=%u",
        secs, g_frames.n, secs > 0 ? g_frames.n / secs : 0.0,
        perf_hist_pct(&g_frames, 0.50) / 1000.0,
        perf_hist_pct(&g_frames, 0.95) / 1000.0,
        g_frames.max / 1000.0,
        perf_hist_count_ge(&g_frames, 33000),
        perf_hist_count_ge(&g_frames, 100000));
    for (int i = 0; i < static_cast<int>(LockSite::Count) && len > 0 && len < (int)sizeof(buf); i++) {
        const LockStats& l = locks[i];
        len += snprintf(buf + len, sizeof(buf) - len,
                        " | qlock %s n=%u hold_avg=%.1fms hold_max=%.1fms wait_max=%.1fms",
                        siteName(i), l.n,
                        l.n ? l.hold_sum / 1000.0 / l.n : 0.0,
                        l.hold_max / 1000.0, l.wait_max / 1000.0);
    }
    if (len > 0 && len < (int)sizeof(buf)) {
        snprintf(buf + len, sizeof(buf) - len,
                 " | img net n=%u p50=%llums p95=%llums max=%llums disk n=%u p95=%llums max=%llums",
                 net.n,
                 (unsigned long long)perf_hist_pct(&net, 0.50),
                 (unsigned long long)perf_hist_pct(&net, 0.95),
                 (unsigned long long)net.max,
                 disk.n,
                 (unsigned long long)perf_hist_pct(&disk, 0.95),
                 (unsigned long long)disk.max);
    }
    util::logLine(buf);
    engine_log(ENGINE_LOG_INFO, "%s", buf);   // no-op until the engine log is open

    perf_hist_reset(&g_frames);
    g_window_start_us = now_us;
}

// Anything worth a line every 5 s rather than every 60 s.
bool windowBusy() {
    if (perf_hist_count_ge(&g_frames, 33000) > 0) return true;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_img_disk.n || g_img_net.n) return true;
    for (const auto& l : g_locks)
        if (l.hold_max >= 5000 || l.wait_max >= 5000) return true;
    return false;
}

} // namespace

void frame() {
    const uint64_t now = nowUs();
    if (g_window_start_us == 0) g_window_start_us = now;
    if (g_last_frame_us != 0) {
        const uint64_t dt = now - g_last_frame_us;
        if (dt < kMaxFrameGapUs) perf_hist_add(&g_frames, dt);
    }
    g_last_frame_us = now;

    const uint64_t elapsed = now - g_window_start_us;
    if (elapsed >= kIdleWindowUs || (elapsed >= kWindowUs && windowBusy())) emit(now);
}

void queueLock(LockSite site, uint64_t wait_us, uint64_t hold_us) {
    const int i = static_cast<int>(site);
    if (i < 0 || i >= static_cast<int>(LockSite::Count)) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    LockStats& l = g_locks[i];
    l.n++;
    l.hold_sum += hold_us;
    if (hold_us > l.hold_max) l.hold_max = hold_us;
    if (wait_us > l.wait_max) l.wait_max = wait_us;
}

void imageLoaded(uint64_t ms, bool from_disk) {
    std::lock_guard<std::mutex> lock(g_mtx);
    perf_hist_add(from_disk ? &g_img_disk : &g_img_net, ms);
}

} // namespace perf
