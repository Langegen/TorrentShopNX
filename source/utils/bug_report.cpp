#include "bug_report.h"

#include "app_paths.h"
#include "archive_utils.h"
#include "file_ops.h"
#include "log.h"
#include "switch_utils.h"
#include "../config/config.h"
#include "../engine/engine_log.h"
#include "../net/http_client.h"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

// Report server (tools/report-server). Empty hides "Send to the developer";
// set it after deploying the server, e.g. "https://reports.example.com/v1/report"
// (behind Caddy with an RSA certificate) or "http://<vps-ip>:8787/v1/report".
#ifndef TSNX_REPORT_URL
#define TSNX_REPORT_URL "http://177.3.213.173:8787/v1/report"
#endif

namespace report {

namespace fs = std::filesystem;

namespace {

constexpr const char* kReportsDir = TSNX_BASE_DIR "/reports";
constexpr const char* kStagingDir = TSNX_CACHE_TMP "/report";
constexpr size_t kKeepReports = 5;
constexpr size_t kMaxMessageBytes = 600;   // the server cuts the caption anyway
constexpr uint64_t kMaxUploadBytes = 9ull * 1024 * 1024;

// PC test runs move log.txt and config.ini with environment variables
// (log.cpp, config.cpp); the console always uses the fixed paths.
std::string envOr(const char* name, const char* fallback) {
#ifndef __SWITCH__
    if (const char* v = std::getenv(name)) {
        if (*v) return v;
    }
#else
    (void)name;
#endif
    return fallback;
}

std::string logPath() { return envOr("TSNX_LOG_PATH", TSNX_LOG_PATH); }
std::string configPath() { return envOr("TSNX_CONFIG_PATH", TSNX_CONFIG_PATH); }

// The previous launch's copy, named like log.cpp / engine_log.c rotate it:
// "log.txt" -> "log.prev.txt", "engine.log" -> "engine.prev.log".
std::string prevPathOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + ".prev";
    return path.substr(0, dot) + ".prev" + path.substr(dot);
}

std::string timestamp(const char* fmt) {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm_buf);
    return buf;
}

std::string firmwareString() {
#ifdef __SWITCH__
    std::string fw = util::getConsoleSdkVersion().toString();
    return fw.empty() ? "unknown" : fw;
#else
    return "pc";
#endif
}

std::string storageLine(int storage_id) {
    int64_t free_b = 0, total_b = 0;
    if (!util::getStorageStats(storage_id, free_b, total_b)) return "unknown";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.2f GB free of %.2f GB",
                  free_b / 1073741824.0, total_b / 1073741824.0);
    return buf;
}

// Last `max_bytes` of a file ("" if it cannot be read).
std::string readTail(const char* path, size_t max_bytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return {};
    std::streamoff size = in.tellg();
    if (size <= 0) return {};
    std::streamoff start = size > static_cast<std::streamoff>(max_bytes)
                               ? size - static_cast<std::streamoff>(max_bytes) : 0;
    in.seekg(start);
    std::string out(static_cast<size_t>(size - start), '\0');
    in.read(&out[0], static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(in.gcount()));
    return out;
}

// "scheme://user:pass@host/..." -> "scheme://host/..."
std::string stripUrlCredentials(const std::string& value) {
    size_t scheme = value.find("://");
    if (scheme == std::string::npos) return value;
    size_t host_start = scheme + 3;
    size_t path_start = value.find('/', host_start);
    size_t at = value.find('@', host_start);
    if (at == std::string::npos || (path_start != std::string::npos && at > path_start)) return value;
    return value.substr(0, host_start) + value.substr(at + 1);
}

bool isSecretKey(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    return key.find("password") != std::string::npos || key.find("login") != std::string::npos ||
           key.find("token") != std::string::npos || key.find("secret") != std::string::npos ||
           key.find("cookie") != std::string::npos;
}

bool writeSanitizedConfig(const std::string& dst) {
    std::ifstream in(configPath());
    if (!in.is_open()) return false;
    std::ofstream out(dst, std::ios::trunc);
    if (!out.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos && line[0] != '#' && line[0] != ';' && line[0] != '[') {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            if (isSecretKey(key)) {
                value = value.empty() ? "" : "<removed>";
            } else {
                value = stripUrlCredentials(value);
            }
            line = key + "=" + value;
        }
        out << line << "\n";
    }
    return out.good();
}

bool writeInfo(const std::string& dst, const std::string& user_message) {
    auto& cfg = config::ConfigManager::instance();
    std::ofstream out(dst, std::ios::trunc);
    if (!out.is_open()) return false;
    out << "TorrentShopNX bug report\n";
    out << "created: " << timestamp("%Y-%m-%d %H:%M:%S") << "\n";
    out << "app_version: " << config::ConfigManager::APP_VERSION << "\n";
#ifdef __SWITCH__
    out << "platform: switch\n";
#else
    out << "platform: pc\n";
#endif
    out << "firmware: " << firmwareString() << "\n";
    out << "previous_launch: " << prevSessionName(previousSession()) << "\n";
    out << "language: " << cfg.getLanguage() << "\n";
    out << "catalog: " << cfg.getActiveCatalogLangKey() << "\n";
    out << "data_mode: " << cfg.getDataMode() << "\n";
    out << "install_location: " << cfg.getInstallLocation() << "\n";
    out << "theme: " << cfg.getTheme() << "\n";
    const std::string contact = cfg.getReportContact();
    out << "telegram: " << (contact.empty() ? "(not given)" : contact) << "\n";
    out << "sd_card: " << storageLine(1) << "\n";
    out << "nand: " << storageLine(0) << "\n";
    out << "\nuser_message:\n" << (user_message.empty() ? "(none)" : user_message) << "\n";
    return out.good();
}

void pruneOldReports() {
    std::error_code ec;
    std::vector<fs::path> zips;
    for (const auto& e : fs::directory_iterator(kReportsDir, ec)) {
        if (e.path().extension() == ".zip" && e.path().filename().string().rfind("report-", 0) == 0) {
            zips.push_back(e.path());
        }
    }
    if (zips.size() <= kKeepReports) return;
    // Names carry the timestamp, so name order is age order.
    std::sort(zips.begin(), zips.end());
    for (size_t i = 0; i + kKeepReports < zips.size(); ++i) {
        fs::remove(zips[i], ec);
    }
}

std::string percentEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Cut to at most `max` bytes without splitting a UTF-8 character.
std::string truncateUtf8(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end);
}

} // namespace

std::string reportEndpoint() {
#ifndef __SWITCH__
    if (const char* env = std::getenv("TSNX_REPORT_URL")) {
        if (*env) return env;
    }
#endif
    return TSNX_REPORT_URL;
}

std::string normalizeTelegramContact(const std::string& raw) {
    std::string s;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    }
    for (const char* prefix : {"https://", "http://"}) {
        if (s.rfind(prefix, 0) == 0) s = s.substr(std::strlen(prefix));
    }
    for (const char* prefix : {"t.me/", "telegram.me/", "@"}) {
        if (s.rfind(prefix, 0) == 0) s = s.substr(std::strlen(prefix));
    }
    if (s.size() < 5 || s.size() > 32) return {};
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return {};
    }
    return "@" + s;
}

const char* prevSessionName(PrevSession s) {
    switch (s) {
        case PrevSession::None: return "none";
        case PrevSession::Clean: return "clean";
        case PrevSession::Fatal: return "fatal";
        case PrevSession::Unexpected: return "unexpected";
    }
    return "?";
}

PrevSession previousSession() {
    // The shutdown line is the last one, FATAL lines come right before the
    // end too: the tail is enough and keeps a 1 MB log off the UI thread.
    std::string tail = readTail(prevPathOf(logPath()).c_str(), 64 * 1024);
    if (tail.empty()) return PrevSession::None;
    if (tail.find("Goodbye!") != std::string::npos) return PrevSession::Clean;
    if (tail.find("FATAL:") != std::string::npos) return PrevSession::Fatal;
    // A launch that ended before the main loop (a few lines) tells nothing.
    if (tail.find("main: entering mainLoop") == std::string::npos && tail.size() < 4096) {
        return PrevSession::None;
    }
    return PrevSession::Unexpected;
}

BuildResult buildReport(const std::string& user_message) {
    BuildResult result;
    util::logLine("report: building, message_len=" + std::to_string(user_message.size()));

    std::error_code ec;
    fs::remove_all(kStagingDir, ec);
    fs::create_directories(kStagingDir, ec);
    fs::create_directories(kReportsDir, ec);

    const std::string staging = kStagingDir;
    std::vector<std::string> files;
    // This launch's logs are copied by their own modules: the engine keeps
    // engine.log open for the whole session and the console cannot open such
    // a file a second time; log.txt is appended to in the background. The
    // previous launch's logs are closed files.
    const std::string log = logPath();
    const std::string engine_log = TSNX_ENGINE_LOG_PATH;
    const std::string log_dst = staging + "/log.txt";
    if (util::logCopyTo(log_dst)) {
        files.push_back(log_dst);
    } else {
        util::logLine("report: could not copy " + log);
    }
    const std::string engine_dst = staging + "/engine.log";
    std::string copy_err;
    if (engine_log_copy_to(engine_dst.c_str()) == 0 ||
        (fs::exists(engine_log, ec) && util::copyFile(engine_log, engine_dst, copy_err))) {
        files.push_back(engine_dst);   // the second branch: engine not started this launch
    } else if (fs::exists(engine_log, ec)) {
        util::logLine("report: could not copy " + engine_log + ": " + copy_err);
    }
    const std::pair<std::string, const char*> prev_logs[] = {
        {prevPathOf(log), "log.prev.txt"},
        {prevPathOf(engine_log), "engine.prev.log"},
    };
    for (const auto& [src, name] : prev_logs) {
        if (!fs::exists(src, ec)) continue;
        const std::string dst = staging + "/" + name;
        copy_err.clear();
        if (util::copyFile(src, dst, copy_err)) {
            files.push_back(dst);
        } else {
            util::logLine("report: could not copy " + src + ": " + copy_err);
        }
    }
    if (writeSanitizedConfig(staging + "/config.ini")) files.push_back(staging + "/config.ini");
    if (!writeInfo(staging + "/info.txt", user_message)) {
        result.error = "cannot write info.txt";
        fs::remove_all(kStagingDir, ec);
        return result;
    }
    files.push_back(staging + "/info.txt");

    const std::string zip_path = std::string(kReportsDir) + "/report-" + timestamp("%Y%m%d-%H%M%S") + ".zip";
    std::string err;
    bool ok = false;
    try {
        ok = util::createZipArchive(zip_path, files, staging, nullptr, nullptr, err);
    } catch (const std::exception& e) {
        err = e.what();
    }
    fs::remove_all(kStagingDir, ec);
    if (!ok) {
        result.error = err.empty() ? "zip failed" : err;
        util::logLine("report: build failed: " + result.error);
        return result;
    }

    result.ok = true;
    result.zip_path = zip_path;
    result.zip_size = fs::file_size(zip_path, ec);
    util::logLine("report: built " + zip_path + " size=" + std::to_string(result.zip_size));
    pruneOldReports();
    return result;
}

UploadResult uploadReport(const std::string& zip_path, const std::string& user_message,
                          const std::atomic<bool>* cancel_flag) {
    UploadResult result;
    const std::string url = reportEndpoint();
    if (url.empty()) {
        result.error = "not_configured";
        return result;
    }

    std::ifstream in(zip_path, std::ios::binary);
    if (!in.is_open()) {
        result.error = "cannot read report";
        return result;
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (body.empty() || body.size() > kMaxUploadBytes) {
        result.error = "report size " + std::to_string(body.size());
        return result;
    }

    auto& cfg = config::ConfigManager::instance();
    std::vector<std::string> headers = {
        "Content-Type: application/zip",
        std::string("X-TSNX-Version: ") + config::ConfigManager::APP_VERSION,
        "X-TSNX-Firmware: " + firmwareString(),
        "X-TSNX-Lang: " + cfg.getLanguage(),
        std::string("X-TSNX-Prev: ") + prevSessionName(previousSession()),
        "X-TSNX-Contact: " + percentEncode(cfg.getReportContact()),
        "X-TSNX-Message: " + percentEncode(truncateUtf8(user_message, kMaxMessageBytes)),
    };

    net::HttpClient client;
    client.setTimeout(90);
    client.setKeepAlive(false);
    client.setCancelFlag(cancel_flag);
    util::logLine("report: uploading " + zip_path + " (" + std::to_string(body.size()) + " bytes)");
    net::HttpResponse resp = client.httpPost(url, body, headers);
    result.http_code = resp.status_code;

    try {
        auto j = nlohmann::json::parse(resp.body);
        if (resp.status_code == 200 && j.value("ok", false)) {
            result.ok = true;
            result.report_id = j.value("id", std::string());
        } else {
            result.error = j.value("error", std::string("http ") + std::to_string(resp.status_code));
        }
    } catch (...) {
        result.error = resp.status_code == 0 ? resp.body : "http " + std::to_string(resp.status_code);
    }
    if (result.ok && result.report_id.empty()) {
        result.ok = false;
        result.error = "no report id in reply";
    }
    util::logLine("report: upload " + std::string(result.ok ? "ok id=" + result.report_id
                                                            : "failed: " + result.error) +
                  " http=" + std::to_string(result.http_code));
    return result;
}

} // namespace report
