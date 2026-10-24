#pragma once

// Self-update helpers shared by the downloader (SettingsTab) and the applier (main.cpp).
//
// update.log is a separate, append-only journal of the self-update steps
// (download, apply at exit, apply at start). log.txt is rotated on every
// launch, so a failed update usually spans more launches than log.txt and
// log.prev.txt keep; this journal survives them.

#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "app_paths.h"
#include "log.h"

namespace util {

inline const char* updateJournalPath() { return TSNX_BASE_DIR "/update.log"; }

inline void updateJournal(const std::string& msg) {
    util::logLine("update: " + msg);

    const char* path = updateJournalPath();
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 64 * 1024) {
        std::string prev = std::string(TSNX_BASE_DIR) + "/update.prev.log";
        std::remove(prev.c_str());
        std::rename(path, prev.c_str());
    }

    FILE* f = std::fopen(path, "ab");
    if (!f) return;
    char ts[32] = "????-??-?? ??:??:??";
    std::time_t now = std::time(nullptr);
    if (std::tm* t = std::localtime(&now)) {
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    }
    std::fprintf(f, "[%s] %s\n", ts, msg.c_str());
    std::fclose(f);
}

// True when the file looks like a homebrew NRO: big enough and carries the
// "NRO0" magic at offset 0x10. Guards against replacing the working NRO with
// an HTML error page or a truncated download.
inline bool isValidNroFile(const std::string& path, long minSize = 100 * 1024) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size < minSize) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char hdr[0x14] = {0};
    size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    return got == sizeof(hdr) && std::memcmp(hdr + 0x10, "NRO0", 4) == 0;
}

} // namespace util
