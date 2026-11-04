#pragma once
// Bug reports: one ZIP with what a developer needs to understand a problem.
//
//   log.txt / log.prev.txt        app log, this launch and the previous one
//   engine.log / engine.prev.log  torrent engine log, same two launches
//   config.ini                    settings with logins, passwords and URL
//                                 credentials removed
//   info.txt                      app version, firmware, storage, how the
//                                 previous launch ended, the user's note
//
// Reports are kept in sdmc:/switch/TorrentShopNX/reports (the newest few).
// Two ways to the developer:
//   - uploadReport(): the report server (tools/bug-report-worker) posts the
//     ZIP to the developer's Telegram chat, signed with the user's @username;
//   - the user downloads the ZIP to a phone (QR code) and posts it to the
//     bugs topic of the Telegram group from their own account.
//
// Everything here blocks (file I/O, network): call it off the UI thread.

#include <atomic>
#include <cstdint>
#include <string>

namespace report {

struct BuildResult {
    bool ok = false;
    std::string zip_path;
    uint64_t zip_size = 0;
    std::string error;
};

// Flushes both logs and packs a new report. `user_message` and the saved
// Telegram contact go to info.txt.
BuildResult buildReport(const std::string& user_message);

struct UploadResult {
    bool ok = false;
    std::string report_id;   // short code from the server, e.g. "R-7F3A2C"
    int http_code = 0;
    std::string error;
};

// Sends a report built by buildReport() to the report server.
UploadResult uploadReport(const std::string& zip_path, const std::string& user_message,
                          const std::atomic<bool>* cancel_flag = nullptr);

// Report server URL; empty when sending is not set up in this build.
// On PC the TSNX_REPORT_URL environment variable overrides it (for testing).
std::string reportEndpoint();
inline bool canUpload() { return !reportEndpoint().empty(); }

// "@name" from what the user typed ("name", "@name", "t.me/name"); "" if it
// is not a valid Telegram username (5-32 letters, digits, underscores).
std::string normalizeTelegramContact(const std::string& raw);

// How the previous launch ended, read from log.prev.txt.
enum class PrevSession {
    None,        // first launch, or no previous log
    Clean,       // normal shutdown
    Fatal,       // a FATAL line (abort, terminate, ...) before the end
    Unexpected,  // the log just stops: crash, power loss, killed
};
PrevSession previousSession();
const char* prevSessionName(PrevSession s);

} // namespace report
