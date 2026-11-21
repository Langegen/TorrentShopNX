#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../engine/thread_cfg.h"

namespace installer {

// Overlaps placeholder writes with decompression: the producer fills one buffer
// while a background thread writes the other, in submit order.
class WriteBehind {
public:
    using WriteFn = std::function<bool(uint64_t offset, const uint8_t* data, size_t size)>;

    WriteBehind(WriteFn write, size_t buf_size) : write_(std::move(write)) {
        for (auto& b : bufs_) {
            b.reset(new uint8_t[buf_size]);
            free_.push_back(b.get());
        }
        thread_ = std::thread([this]() { run(); });
    }

    // Without finish() (cancel/error path) queued writes are dropped; only the
    // one in flight completes.
    ~WriteBehind() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
            queue_.clear();
        }
        cv_.notify_all();
        thread_.join();
    }

    WriteBehind(const WriteBehind&) = delete;
    WriteBehind& operator=(const WriteBehind&) = delete;

    // A free buffer, or nullptr once a write has failed.
    uint8_t* acquire() {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this]() { return failed_ || !free_.empty(); });
        if (failed_) return nullptr;
        uint8_t* b = free_.back();
        free_.pop_back();
        return b;
    }

    // Give back a buffer that was acquired but not submitted.
    void release(uint8_t* buf) {
        {
            std::lock_guard<std::mutex> l(m_);
            free_.push_back(buf);
        }
        cv_.notify_all();
    }

    // Queue `size` bytes of `buf` for writing at `offset`; false once a write has failed.
    bool submit(uint8_t* buf, uint64_t offset, size_t size) {
        {
            std::lock_guard<std::mutex> l(m_);
            if (failed_) {
                free_.push_back(buf);
                return false;
            }
            queue_.push_back({buf, offset, size});
        }
        cv_.notify_all();
        return true;
    }

    // Wait until every submitted write is done; false if any failed.
    bool finish() {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [this]() { return failed_ || (queue_.empty() && !busy_); });
        return !failed_;
    }

private:
    struct Job {
        uint8_t* buf;
        uint64_t offset;
        size_t size;
    };

    void run() {
        tsnx_thread_pin_self(TSNX_CORE_WORK, TSNX_PRIO_WRITER);
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this]() { return stop_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stop_ with nothing left
                j = queue_.front();
                queue_.pop_front();
                busy_ = true;
            }
            bool ok = write_(j.offset, j.buf, j.size);
            {
                std::lock_guard<std::mutex> l(m_);
                busy_ = false;
                free_.push_back(j.buf);
                if (!ok) {
                    failed_ = true;
                    for (auto& q : queue_) free_.push_back(q.buf);
                    queue_.clear();
                }
            }
            cv_.notify_all();
        }
    }

    WriteFn write_;
    std::unique_ptr<uint8_t[]> bufs_[2];
    std::vector<uint8_t*> free_;
    std::deque<Job> queue_;
    std::mutex m_;
    std::condition_variable cv_;
    bool busy_ = false;
    bool failed_ = false;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace installer
