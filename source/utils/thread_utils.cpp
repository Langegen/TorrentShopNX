#include "thread_utils.h"
#include "log.h"

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace util {
namespace {

struct BackgroundThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};

std::mutex g_mutex;

// Leaked on purpose: destroying a vector that still holds a running
// (joinable) std::thread at exit would itself call std::terminate.
std::vector<BackgroundThread>& threads() {
    static auto* list = new std::vector<BackgroundThread>();
    return *list;
}

// Joins the threads that are done. Caller holds g_mutex.
void reapFinishedLocked() {
    auto& list = threads();
    for (auto it = list.begin(); it != list.end();) {
        if (it->finished->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();   // already past fn, returns at once
            it = list.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

bool runInBackground(std::function<void()> fn, const char* name) {
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::string tag = name ? name : "bg";

    std::thread thread;
    try {
        thread = std::thread([fn = std::move(fn), finished, tag]() mutable {
            try {
                fn();
            } catch (const std::exception& e) {
                logLine("thread[" + tag + "]: exception: " + e.what());
            } catch (...) {
                logLine("thread[" + tag + "]: unknown exception");
            }
            finished->store(true, std::memory_order_release);
        });
    } catch (const std::exception& e) {
        logLine("thread[" + tag + "]: could not start: " + e.what());
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    reapFinishedLocked();
    threads().push_back({std::move(thread), std::move(finished)});
    return true;
}

} // namespace util
