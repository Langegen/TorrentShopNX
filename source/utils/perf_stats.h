#pragma once

// App-side [perf] metrics (phase 0 of docs/engine-optimization-plan.md):
// UI frame time, download-queue mutex hold/wait, cover load latency. Every
// window the UI thread emits one "[perf] ui ..." line to log.txt and, when the
// engine log is open, to engine.log too, so both timelines line up with the
// engine's "[perf] net ..." lines.

#include <chrono>
#include <cstdint>

namespace perf {

enum class LockSite {
    TrackProgress = 0,  // DownloadManager::trackProgress (progress thread)
    UiAdd,              // ui::DownloadManager::addDownload (UI thread)
    Count
};

inline uint64_t nowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Once per rendered frame, from the UI thread. Also emits the periodic line.
void frame();

// Queue mutex: `wait_us` spent acquiring it, `hold_us` spent holding it.
void queueLock(LockSite site, uint64_t wait_us, uint64_t hold_us);

// A cover/screenshot became ready to display, `ms` after it was enqueued.
void imageLoaded(uint64_t ms, bool from_disk);

// RAII: construct right after acquiring the queue mutex, passing the time the
// acquisition started; records wait and hold when it goes out of scope.
class LockTimer {
public:
    LockTimer(LockSite site, uint64_t wait_start_us)
        : site_(site), acquired_us_(nowUs()), wait_us_(acquired_us_ - wait_start_us) {}
    ~LockTimer() { queueLock(site_, wait_us_, nowUs() - acquired_us_); }
    LockTimer(const LockTimer&) = delete;
    LockTimer& operator=(const LockTimer&) = delete;

private:
    LockSite site_;
    uint64_t acquired_us_;
    uint64_t wait_us_;
};

} // namespace perf
