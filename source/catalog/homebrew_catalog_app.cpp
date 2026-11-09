// The app's homebrew catalog: real HttpClient and SD paths. Kept apart from
// homebrew_catalog.cpp so the tests build without the network code.

#include "homebrew_catalog.hpp"

#include "collections_manager.h"
#include "net/http_client.h"
#include "utils/app_paths.h"
#include "utils/log.h"

#include <atomic>
#include <cstdio>
#include <ctime>

namespace homebrew {

namespace {

FetchResult httpFetch(const std::string& url, const std::string& etag) {
    // Download into a temp file (downloadToFileEx reports 304 and the ETag),
    // then hand the body to the catalog, which validates and saves it.
    static std::atomic<unsigned> counter{0};
    const std::string tmp = std::string(TSNX_CACHE_TMP) + "/homebrew_" + std::to_string(counter.fetch_add(1)) + ".part";
    TSNX_MKDIR(TSNX_CACHE_DIR);
    TSNX_MKDIR(TSNX_CACHE_TMP);
    std::vector<std::string> headers;
    if (!etag.empty()) headers.push_back("If-None-Match: " + etag);

    net::HttpClient http;
    const auto res = http.downloadToFileEx(url, tmp, headers, &g_appExiting, 60);
    FetchResult out;
    out.status = res.not_modified ? 304 : res.http_code;
    out.etag = res.etag;
    if (res.success && !res.not_modified) {
        if (FILE* f = std::fopen(tmp.c_str(), "rb")) {
            char buf[64 * 1024];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.body.append(buf, n);
            std::fclose(f);
        } else {
            out.status = 0;
        }
    }
    std::remove(tmp.c_str());
    util::logLine("homebrew: GET " + url + " -> " + std::to_string(out.status) + ", " +
                  std::to_string(out.body.size()) + " bytes");
    return out;
}

}  // namespace

Catalog& Catalog::instance() {
    static Catalog catalog(
        CatalogConfig{
            TSNX_CACHE_HOMEBREW,
            std::string(TSNX_SD_ROOT) + "switch/appstore/.get/packages",
            TSNX_SD_ROOT,
            std::string(kFortheusersCdn) + "/repo.json",
            std::string(catalog::kCollectionsBaseUrl) + "homebrew.json",
        },
        httpFetch, [] { return static_cast<long long>(std::time(nullptr)); });
    return catalog;
}

}  // namespace homebrew
