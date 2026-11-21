// WriteBehind: ordered writes on a background thread, error propagation, cancel.
// Build: g++ -std=c++17 -pthread -Isource pctest/test_write_behind.cpp -o pctest/test_write_behind
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../source/installer/write_behind.h"

using installer::WriteBehind;

int main() {
    {
        // Writes land in submit order with the bytes the producer filled.
        std::mutex m;
        std::vector<std::pair<uint64_t, uint8_t>> seen;
        WriteBehind wb([&](uint64_t off, const uint8_t* p, size_t n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::lock_guard<std::mutex> l(m);
            assert(n == 16);
            seen.push_back({off, p[0]});
            return true;
        }, 16);
        for (int i = 0; i < 20; i++) {
            uint8_t* b = wb.acquire();
            assert(b);
            std::memset(b, i, 16);
            assert(wb.submit(b, static_cast<uint64_t>(i) * 16, 16));
        }
        assert(wb.finish());
        assert(seen.size() == 20);
        for (int i = 0; i < 20; i++) {
            assert(seen[i].first == static_cast<uint64_t>(i) * 16);
            assert(seen[i].second == i);
        }
        std::puts("ordered_writes ... OK");
    }
    {
        // A failed write stops the pipeline: later acquire/submit fail, finish reports it.
        int calls = 0;
        WriteBehind wb([&](uint64_t, const uint8_t*, size_t) { return ++calls < 3; }, 8);
        bool stopped = false;
        for (int i = 0; i < 50 && !stopped; i++) {
            uint8_t* b = wb.acquire();
            if (!b) { stopped = true; break; }
            if (!wb.submit(b, 0, 8)) stopped = true;
        }
        assert(stopped);
        assert(!wb.finish());
        assert(calls == 3);
        std::puts("error_stops_pipeline ... OK");
    }
    {
        // Destroying without finish (cancel) returns promptly even with queued work.
        auto t0 = std::chrono::steady_clock::now();
        {
            WriteBehind wb([](uint64_t, const uint8_t*, size_t) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return true;
            }, 8);
            for (int i = 0; i < 2; i++) assert(wb.submit(wb.acquire(), 0, 8));
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        assert(ms < 120);  // at most the one in-flight write, queued work is dropped
        std::puts("cancel_drops_queued ... OK");
    }
    {
        // release() returns an unused buffer so the producer can stop early.
        WriteBehind wb([](uint64_t, const uint8_t*, size_t) { return true; }, 8);
        uint8_t* a = wb.acquire();
        uint8_t* b = wb.acquire();
        assert(a && b && a != b);
        wb.release(a);
        wb.release(b);
        assert(wb.finish());
        std::puts("release_unused ... OK");
    }
    std::puts("all write_behind tests passed");
    return 0;
}
