#pragma once

// Which games have mods in ModCD (github.com/kawaii-flesh/ModCD): the list
// behind the "mods in ModCD" mark on the game card. We only point to the app;
// the mods themselves are downloaded and installed by ModCD.
//
// modcd.json is built from ModCD's table.md by a GitHub Action in
// Langegen/switch-game-collection (scripts/modcd_indexer.py), downloaded at
// most every 6 hours with ETag and kept in the homebrew cache folder.
//
// Network and clock are passed in, so it runs in pctest
// (pctest/test_modcd_index.cpp). The app's instance is in modcd_index_app.cpp.
// Thread-safe: refresh() runs on a worker thread, the UI calls find().

#include "homebrew_catalog.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace modcd {

constexpr const char* kModCdRepo = "kawaii-flesh/modcd";
constexpr const char* kModCdUrl = "https://github.com/kawaii-flesh/ModCD";
constexpr const char* kModKitRepo = "qret/modkitnx";
constexpr const char* kModKitUrl = "https://github.com/qret/ModKitNX";

struct TitleMods {
    int mods = 0;
    std::vector<uint32_t> versions;  // game updates (N << 16, 0 = none) the mods are made for
    std::vector<std::string> types;  // "gfx", ...
};

using TitleMap = std::unordered_map<uint64_t, TitleMods>;  // key: application ID

// False (with err) for anything that is not a modcd.json with at least one game.
bool parseIndex(const std::string& json, TitleMap& out, std::string& err);

// Returns true if ModCD is installed (via libget or on SD card).
bool isModCdInstalled();

// Returns true if ModKitNX is installed (via libget or on SD card).
bool isModKitInstalled();

// ModCD lists a mod only for the game updates it was made for.
bool fitsVersion(const TitleMods& mods, uint32_t installed_patch_version);

class Index {
public:
    Index(std::string cache_dir, std::string url, homebrew::Fetcher fetch, homebrew::Clock now,
          long long refresh_interval_sec = 6 * 3600);

    // The app's index (real network and SD paths), defined in modcd_index_app.cpp.
    static Index& instance();

    // Reads the cached file. False when there is no usable cache.
    bool loadCached();
    bool loaded() const { return loaded_.load(); }

    // Downloads the list when the last check is older than the interval, or
    // always with force. Keeps the old file on any failure. Returns loaded().
    bool refresh(bool force = false);

    // title_id may be the game, its update or a DLC.
    bool find(uint64_t title_id, TitleMods& out) const;
    size_t size() const;

private:
    long long lastCheck() const;

    std::string cache_dir_;
    std::string url_;
    homebrew::Fetcher fetch_;
    homebrew::Clock now_;
    long long interval_;

    mutable std::mutex mutex_;
    std::mutex refresh_mutex_;  // one refresh at a time
    std::shared_ptr<const TitleMap> titles_;
    std::atomic<bool> loaded_{false};
};

}  // namespace modcd
