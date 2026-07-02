#include "log.h"

#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <cstdio>

namespace util {

static const char* kLogPath = "sdmc:/switch/TorrentShopNX/log.txt";
static std::mutex g_log_mutex;
static std::ofstream g_log_file;

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
    g_log_file.open(kLogPath, std::ios::trunc);
    if (g_log_file.is_open()) {
        g_log_file << "TorrentShopNX log start" << std::endl;
        g_log_file.flush();
    }
}

void logLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file << line << "\n";
        g_log_file.flush();
    }
}

void logClose() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file.close();
    }
}

} // namespace util
