#include "log.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>

#include "../engine/thread_cfg.h"

#if defined(__MINGW32__)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

namespace util {

#ifdef __SWITCH__
static const char* kLogPath = "sdmc:/switch/TorrentShopNX/log.txt";
#else
static const char* kLogPath = "log.txt";
#endif
static std::mutex g_log_mutex;

// PC tests set TSNX_LOG_PATH so the same log lines the app writes to the SD
// card land in a local file; on the console getenv() just returns NULL.
static const char* logPath() {
    static std::string override_path;
    if (override_path.empty()) {
        const char* env = getenv("TSNX_LOG_PATH");
        override_path = (env && env[0]) ? env : kLogPath;
    }
    return override_path.c_str();
}

static bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void ensureDirRecursive(const std::string& path) {
    if (path.empty()) return;
    if (pathExists(path)) return;

    std::string cur;
    size_t pos = 0;
    if (path.rfind("sdmc:/", 0) == 0) {
        cur = "sdmc:/";
        pos = 6;
    }

    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        std::string part = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += part;
            if (!pathExists(cur)) {
                mkdir(cur.c_str(), 0777);
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
}

static void startAsyncWriter();

void logInit() {
#ifdef __SWITCH__
    ensureDirRecursive("sdmc:/switch/TorrentShopNX");
#endif
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const char* curPath = logPath();
    struct stat st;
    if (stat(curPath, &st) == 0 && st.st_size > 0) {
        std::string prevPath = curPath;
        size_t dot = prevPath.find_last_of('.');
        if (dot != std::string::npos) {
            prevPath = prevPath.substr(0, dot) + ".prev" + prevPath.substr(dot);
        } else {
            prevPath += ".prev";
        }
        std::remove(prevPath.c_str());
        std::rename(curPath, prevPath.c_str());
    }
    std::ofstream file(curPath, std::ios::trunc);
    if (file.is_open()) {
        file << "TorrentShopNX log start\n";
        file.close();
    }
    startAsyncWriter();
}

// ---------------------------------------------------------------------------
// Asynchronous writer. logLine() used to open, append and close log.txt for
// every single line -- several fs-service IPCs per line, from the UI thread
// among others (one line per cover load). Now lines go into a memory buffer
// and a writer thread on the work core appends them in batches, at most every
// kFlushMs (or sooner when the buffer fills). The file is still opened and
// closed per batch, so it is never held open between writes (it can be read
// or pulled off the SD card while the app runs, as before).
//
// Before logInit() and after logClose() lines are written synchronously.
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kMaxPending = 512 * 1024;   // beyond this new lines are dropped
constexpr int kFlushMs = 500;

struct AsyncLog {
    std::mutex mtx;
    std::condition_variable cv;
    std::string pending;
    size_t dropped = 0;
    bool running = false;
    bool stop = false;
    uint64_t requested = 0;   // flush generations (logFlush)
    uint64_t written = 0;
    std::condition_variable written_cv;
    std::thread thread;
};

// Leaked on purpose: the console exits through _exit() and the PC build may
// log from static destructors; a destroyed mutex there would crash.
AsyncLog& asyncLog() {
    static AsyncLog* l = new AsyncLog();
    return *l;
}

void appendToFile(const std::string& data) {
    if (data.empty()) return;
    FILE* f = fopen(logPath(), "ab");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

void writerMain() {
    tsnx_thread_pin_self(TSNX_CORE_WORK, TSNX_PRIO_LOGGER);
    AsyncLog& l = asyncLog();
    std::string batch;
    for (;;) {
        size_t dropped = 0;
        bool stop = false;
        uint64_t gen = 0;
        {
            std::unique_lock<std::mutex> lk(l.mtx);
            l.cv.wait_for(lk, std::chrono::milliseconds(kFlushMs), [&l]() {
                return l.stop || l.requested != l.written || l.pending.size() > kMaxPending / 2;
            });
            batch.swap(l.pending);
            dropped = l.dropped;
            l.dropped = 0;
            stop = l.stop;
            gen = l.requested;
        }
        if (dropped > 0)
            batch += "[log] dropped " + std::to_string(dropped) + " lines (buffer full)\n";
        appendToFile(batch);
        batch.clear();
        {
            std::lock_guard<std::mutex> lk(l.mtx);
            l.written = gen;
            if (stop && l.pending.empty()) {
                l.written_cv.notify_all();
                return;
            }
        }
        l.written_cv.notify_all();
    }
}

} // namespace

static void startAsyncWriter() {
    AsyncLog& l = asyncLog();
    std::lock_guard<std::mutex> lk(l.mtx);
    if (l.running) return;
    l.stop = false;
    l.running = true;
    l.thread = std::thread(writerMain);
}

void logLine(const std::string& line) {
    AsyncLog& l = asyncLog();
    {
        std::lock_guard<std::mutex> lk(l.mtx);
        if (l.running) {
            if (l.pending.size() + line.size() + 1 > kMaxPending) {
                l.dropped++;
                return;
            }
            l.pending += line;
            l.pending += '\n';
            if (l.pending.size() > kMaxPending / 2) l.cv.notify_one();
            return;
        }
    }
    // No writer thread (before logInit / after logClose): synchronous append.
    std::lock_guard<std::mutex> lock(g_log_mutex);
    appendToFile(line + "\n");
}

void logFlush() {
    AsyncLog& l = asyncLog();
    std::unique_lock<std::mutex> lk(l.mtx);
    if (!l.running) return;
    const uint64_t gen = ++l.requested;
    l.cv.notify_one();
    l.written_cv.wait_for(lk, std::chrono::milliseconds(1000),
                          [&l, gen]() { return l.written >= gen || !l.running; });
}

void logClose() {
    AsyncLog& l = asyncLog();
    {
        std::lock_guard<std::mutex> lk(l.mtx);
        if (!l.running) return;
        l.stop = true;
    }
    l.cv.notify_one();
    if (l.thread.joinable()) l.thread.join();
    std::lock_guard<std::mutex> lk(l.mtx);
    l.running = false;
    l.stop = false;
    // Anything logged between the writer's last batch and running=false.
    appendToFile(l.pending);
    l.pending.clear();
}

} // namespace util
