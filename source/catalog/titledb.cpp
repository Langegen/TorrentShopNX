#include "titledb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

#ifndef TITLEDB_NO_APP
#include "utils/app_paths.h"
#include "utils/log.h"
#include "net/http_client.h"
#endif

namespace titledb {

#ifndef TITLEDB_NO_APP
namespace {

bool readFile(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.clear();
    if (len > 0) {
        out.resize(static_cast<size_t>(len));
        out.resize(std::fread(&out[0], 1, out.size(), f));
    }
    std::fclose(f);
    return true;
}

time_t fileMtime(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

}  // namespace
#endif

VersionMap parseVersionsTxt(const std::string& text) {
    VersionMap out;
    out.reserve(text.size() / 60);
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
        if (!eol) eol = end;
        char* tidEnd = nullptr;
        uint64_t tid = std::strtoull(p, &tidEnd, 16);
        if (tidEnd != p && tidEnd < eol && *tidEnd == '|') {
            const char* last = eol;
            while (last > tidEnd && *(last - 1) != '|') --last;   // just after the last '|'
            char* verEnd = nullptr;
            unsigned long ver = std::strtoul(last, &verEnd, 10);
            if (verEnd != last) out[tid] = static_cast<uint32_t>(ver);
        }
        p = eol + 1;
    }
    return out;
}

std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>
parseVersionsJson(const std::string& text) {
    // Tolerant scanner for {"<hex id>": {"<version>": "YYYY-MM-DD", ...}, ...}.
    // A full JSON DOM of the 1.7 MB file would cost tens of MB on the console.
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> out;
    const char* p = text.data();
    const char* end = p + text.size();

    auto readString = [&](std::string& s) -> bool {
        while (p < end && *p != '"') ++p;
        if (p >= end) return false;
        const char* start = ++p;
        while (p < end && *p != '"') ++p;
        if (p >= end) return false;
        s.assign(start, p);
        ++p;
        return true;
    };
    auto skipTo = [&](char c) -> bool {
        while (p < end && *p != c) ++p;
        if (p >= end) return false;
        ++p;
        return true;
    };

    if (!skipTo('{')) return out;
    std::string key, ver, date;
    for (;;) {
        // next id key or the end of the top object
        while (p < end && *p != '"' && *p != '}') ++p;
        if (p >= end || *p == '}') break;
        if (!readString(key) || !skipTo('{')) break;
        uint64_t id = std::strtoull(key.c_str(), nullptr, 16);
        auto& list = out[id];
        for (;;) {
            while (p < end && *p != '"' && *p != '}') ++p;
            if (p >= end) return out;
            if (*p == '}') { ++p; break; }
            if (!readString(ver) || !skipTo(':')) return out;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
            uint32_t ymd = 0;
            if (p < end && *p == '"') {
                if (!readString(date)) return out;
                if (date.size() >= 10) {
                    ymd = static_cast<uint32_t>(std::atoi(date.substr(0, 4).c_str()) * 10000 +
                                                std::atoi(date.substr(5, 2).c_str()) * 100 +
                                                std::atoi(date.substr(8, 2).c_str()));
                }
            } else {
                while (p < end && *p != ',' && *p != '}') ++p;   // null or a number
            }
            list.emplace_back(static_cast<uint32_t>(std::strtoul(ver.c_str(), nullptr, 10)), ymd);
        }
    }
    return out;
}

std::string formatDate(const std::string& iso) {
    if (iso.size() == 10 && iso[4] == '-' && iso[7] == '-') {
        return iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4);
    }
    return iso;
}

#ifndef TITLEDB_NO_APP

namespace {

std::mutex g_mtx;
std::shared_ptr<const VersionMap> g_latest;
time_t g_latestMtime = -1;
std::shared_ptr<const std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>> g_dates;
time_t g_datesMtime = -1;

bool downloadIfStale(const char* path, const char* name) {
    struct stat st;
    bool exists = (stat(path, &st) == 0 && st.st_size > 0);
    if (exists && std::time(nullptr) - st.st_mtime < 24 * 3600) return true;

    std::string cdn = std::string("https://cdn.jsdelivr.net/gh/blawar/titledb@master/") + name;
    std::string raw = std::string("https://raw.githubusercontent.com/blawar/titledb/master/") + name;
    util::logLine(std::string("titledb: downloading ") + name);
    net::HttpClient http;
    auto res = http.httpGet(cdn);
    if (res.status_code != 200 || res.body.empty()) res = http.httpGet(raw);
    if (res.status_code == 200 && !res.body.empty()) {
        std::string tmp = std::string(path) + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            bool ok = std::fwrite(res.body.data(), 1, res.body.size(), f) == res.body.size();
            ok = (std::fclose(f) == 0) && ok;
            if (ok) {
                std::remove(path);
                if (std::rename(tmp.c_str(), path) == 0) {
                    util::logLine(std::string("titledb: ") + name + " updated (" + std::to_string(res.body.size()) + " bytes)");
                    return true;
                }
            }
            std::remove(tmp.c_str());
        }
    }
    util::logLine(std::string("titledb: ") + name + " download failed, status=" + std::to_string(res.status_code));
    return exists;
}

}  // namespace

bool ensureDownloaded() {
    bool ok = downloadIfStale(TSNX_VERSIONS_PATH, "versions.txt");
    downloadIfStale(TSNX_VERSION_DATES_PATH, "versions.json");   // optional: dates only
    return ok;
}

std::shared_ptr<const VersionMap> latestVersions() {
    time_t mt = fileMtime(TSNX_VERSIONS_PATH);
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_latest && mt == g_latestMtime) return g_latest;
    }
    std::string text;
    auto parsed = std::make_shared<VersionMap>();
    if (readFile(TSNX_VERSIONS_PATH, text)) *parsed = parseVersionsTxt(text);
    util::logLine("titledb: parsed versions.txt, " + std::to_string(parsed->size()) + " entries");
    std::lock_guard<std::mutex> lock(g_mtx);
    g_latest = parsed;
    g_latestMtime = mt;
    return g_latest;
}

uint32_t latestUpdateVersion(uint64_t appId) {
    auto db = latestVersions();
    auto it = db->find((appId & ~0x1FFFULL) | 0x800ULL);
    return it != db->end() ? it->second : 0;
}

std::string updateDate(uint64_t appId, uint32_t version) {
    if (version == 0) return "";
    time_t mt = fileMtime(TSNX_VERSION_DATES_PATH);
    std::shared_ptr<const std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>> dates;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (g_dates && mt == g_datesMtime) dates = g_dates;
    }
    if (!dates) {
        std::string text;
        auto parsed = std::make_shared<std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>>();
        if (readFile(TSNX_VERSION_DATES_PATH, text)) *parsed = parseVersionsJson(text);
        util::logLine("titledb: parsed versions.json, " + std::to_string(parsed->size()) + " titles");
        std::lock_guard<std::mutex> lock(g_mtx);
        g_dates = parsed;
        g_datesMtime = mt;
        dates = g_dates;
    }
    auto it = dates->find(appId & ~0x1FFFULL);
    if (it == dates->end()) return "";
    for (const auto& vd : it->second) {
        if (vd.first == version && vd.second > 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", vd.second / 10000, (vd.second / 100) % 100, vd.second % 100);
            return buf;
        }
    }
    return "";
}

#endif  // TITLEDB_NO_APP

}  // namespace titledb
