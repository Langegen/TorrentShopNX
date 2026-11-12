#pragma once

// Homebrew store catalog on the console: downloads repo.json (fortheusers)
// and homebrew.json (our index) at most every 6 hours with ETag, keeps them
// in cache/homebrew, merges them (homebrew_package.hpp) and reads what is
// installed from the libget registry shared with the Homebrew App Store.
//
// Network and clock are passed in, so the whole class runs in pctest
// (pctest/test_homebrew_catalog.cpp) with fake responses. The app's instance
// with the real HttpClient and SD paths is in homebrew_catalog_app.cpp.
//
// Thread-safe: refresh() and rescanInstalled() run on a worker thread, the UI
// reads snapshot()/installedSnapshot() and watches generation().

#include "homebrew_package.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace homebrew {

struct FetchResult {
    int status = 0;    // HTTP code; 304 = not modified; 0 = network error
    std::string body;  // for 200
    std::string etag;  // for 200, may be empty
};

// GET url; sends If-None-Match when etag is not empty.
using Fetcher = std::function<FetchResult(const std::string& url, const std::string& etag)>;
using Clock = std::function<long long()>;  // seconds since epoch

// The app's Fetcher (HttpClient), defined in homebrew_catalog_app.cpp.
FetchResult appHttpFetch(const std::string& url, const std::string& etag);

struct CatalogConfig {
    std::string cache_dir;     // repo.json, homebrew.json, their .etag, last_check
    std::string registry_dir;  // .../switch/appstore/.get/packages
    std::string sd_root;       // "sdmc:/" on the console; prefix for package paths
    std::string fortheusers_url;  // .../repo.json
    std::string index_url;        // .../homebrew.json
    long long refresh_interval_sec = 6 * 3600;
};

struct RefreshResult {
    bool attempted = false;  // false: data was fresh, nothing downloaded
    bool ok = false;         // both sources answered (200 or 304) and parsed
    bool changed = false;    // the package list was rebuilt
    int fortheusers_status = 0;
    int index_status = 0;
    std::string error;
};

using PackageList = std::vector<Package>;
using InstalledMap = std::map<std::string, Installed>;  // key: package name, lower case

class Catalog {
public:
    Catalog(CatalogConfig cfg, Fetcher fetch, Clock now);

    // The app's catalog (real network and SD paths), defined in homebrew_catalog_app.cpp.
    static Catalog& instance();

    // Reads the cached files. Returns false when there is no usable cache.
    bool loadCached();
    bool loaded() const { return loaded_.load(); }

    // Downloads what changed (ETag) when the last check is older than the
    // interval, or always with force. Keeps the old files on any failure.
    RefreshResult refresh(bool force = false);

    // Reads the registry and looks for hand-installed .nro of unregistered packages.
    void rescanInstalled();

    std::shared_ptr<const PackageList> snapshot() const;
    std::shared_ptr<const InstalledMap> installedSnapshot() const;
    bool find(const std::string& name, Package& out) const;
    // By GitHub repository, "owner/repo" in any case.
    bool findByRepo(const std::string& repo, Package& out) const;
    Installed installed(const std::string& name) const;
    Status status(const Package& pkg) const;
    int updatesAvailable() const;  // registered packages with a newer version in the store

    long long lastCheck() const;  // last successful refresh, 0 if never
    // Bumped whenever the package list or the installed state changes.
    uint64_t generation() const { return generation_.load(); }

    const CatalogConfig& config() const { return cfg_; }

private:
    bool rebuildFromFiles(std::string& err);
    bool fetchOne(const std::string& url, const std::string& file, bool (*validate)(const std::string&),
                  int& status, bool& changed, std::string& err);

    CatalogConfig cfg_;
    Fetcher fetch_;
    Clock now_;

    mutable std::mutex mutex_;
    std::mutex refresh_mutex_;  // one refresh at a time
    std::shared_ptr<const PackageList> packages_;
    std::shared_ptr<const InstalledMap> installed_;
    std::atomic<bool> loaded_{false};
    std::atomic<uint64_t> generation_{0};
};

}  // namespace homebrew
