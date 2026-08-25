#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>

#if defined(__MINGW32__)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

namespace util {

static const char* kLogPath = "sdmc:/switch/TorrentShopNX/log.txt";
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

void logInit() {
    ensureDirRecursive("sdmc:/switch/TorrentShopNX");
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream file(logPath(), std::ios::trunc);
    if (file.is_open()) {
        file << "TorrentShopNX log start\n";
        file.close();
    }
}

void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream file(logPath(), std::ios::app);
    if (file.is_open()) {
        file << line << "\n";
        file.close();
    }
}

void logClose() {
    // File is closed after every line write to avoid file locks
}

} // namespace util
