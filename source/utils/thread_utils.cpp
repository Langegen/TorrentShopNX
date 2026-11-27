#include "thread_utils.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
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
std::atomic<bool> g_shutting_down{false};

// Leaked on purpose: destroying a vector that still holds a running
// (joinable) std::thread at exit would itself call std::terminate.
std::vector<BackgroundThread>& threads() {
    static auto* list = new std::vector<BackgroundThread>();
    return *list;
}

std::condition_variable& finishedCv() {
    static auto* cv = new std::condition_variable();
    return *cv;
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
    if (g_shutting_down.load(std::memory_order_acquire)) {
        return false;
    }

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
            {
                std::lock_guard<std::mutex> lock(g_mutex);
            }
            finishedCv().notify_all();
        });
    } catch (const std::exception& e) {
        logLine("thread[" + tag + "]: could not start: " + e.what());
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shutting_down.load(std::memory_order_acquire)) {
        // Rare race: joinBackgroundThreads started while creating this thread.
        // Wait for it to finish outside the lock? Keep it in list so joinBackgroundThreads
        // or the process handles it safely.
        threads().push_back({std::move(thread), std::move(finished)});
        return true;
    }
    reapFinishedLocked();
    threads().push_back({std::move(thread), std::move(finished)});
    return true;
}

bool joinBackgroundThreads(int timeout_ms) {
    g_shutting_down.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    std::vector<BackgroundThread> toJoin;
    bool allJoined = false;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        finishedCv().wait_until(lock, deadline, []() {
            for (const auto& entry : threads()) {
                if (!entry.finished->load(std::memory_order_acquire)) {
                    return false;
                }
            }
            return true;
        });

        auto& list = threads();
        for (auto it = list.begin(); it != list.end();) {
            if (it->finished->load(std::memory_order_acquire)) {
                toJoin.push_back(std::move(*it));
                it = list.erase(it);
            } else {
                ++it;
            }
        }
        allJoined = list.empty();
    }

    for (auto& entry : toJoin) {
        if (entry.thread.joinable()) {
            entry.thread.join();
        }
    }
    return allJoined;
}

} // namespace util
