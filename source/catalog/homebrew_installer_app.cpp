// The app's homebrew installer: real HttpClient, SD paths and free space.
// Kept apart from homebrew_installer.cpp so the tests build without them.

#include "homebrew_installer.hpp"

#include "net/http_client.h"
#include "utils/app_paths.h"
#include "utils/file_ops.h"
#include "utils/log.h"

#include <atomic>

namespace homebrew {

namespace {

bool httpDownload(const std::string& url, const std::string& dest,
                  const std::function<void(int64_t, int64_t)>& progress, const std::atomic<bool>* cancel,
                  std::string& err) {
    net::HttpClient http;
    if (progress) http.setProgressCallback([&](int64_t total, int64_t now) { progress(now, total); });
    // Packages go up to ~200 MB: allow a slow connection.
    const bool ok = http.downloadToFile(url, dest, cancel, 30 * 60);
    if (!ok) err = "HTTP download failed";
    util::logLine("homebrew: download " + url + (ok ? " ok" : " failed"));
    return ok;
}

int64_t freeSpace(const std::string& path) {
    uint64_t free_bytes = 0, total = 0;
    return util::getStorageSpace(path, free_bytes, total) ? static_cast<int64_t>(free_bytes) : -1;
}

}  // namespace

Installer& Installer::instance() {
    static Installer installer(
        InstallerConfig{
            TSNX_SD_ROOT,
            std::string(TSNX_SD_ROOT) + "switch/appstore/.get/packages",
            TSNX_CACHE_TMP,
        },
        [](const std::string& url, const std::string&) {
            // Manifests are small: same fetcher as the catalog, no ETag.
            net::HttpClient http;
            http.setTimeout(30);
            const auto r = http.httpGet(url);
            return FetchResult{r.status_code, r.body, ""};
        },
        httpDownload, freeSpace);
    return installer;
}

}  // namespace homebrew
