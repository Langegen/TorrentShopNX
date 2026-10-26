#pragma once

// Version data used by the game manager:
//   versions.txt  - "id|version" from github.com/16BitWonder/nx-versions:
//                   latest version of every patch and DLC (updated daily;
//                   base games are not listed);
//   versions.json - {"<app id>": {"<version>": "YYYY-MM-DD", ...}} from
//                   github.com/blawar/titledb: release date of every update.
// Both are refreshed at most once a day and parsed once per file change.
//
// Nintendo numbers updates as N << 16 (65536 = 1st update, 1441792 = 22nd),
// so the raw number can't be turned into the game's "1.2.3" string, but the
// update ordinal and its date can be shown.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace titledb {

using VersionMap = std::unordered_map<uint64_t, uint32_t>;

// Downloads versions.txt / versions.json when missing or older than 24 h.
// Returns true when versions.txt is usable (fresh or an older copy).
// Blocking (network): call from a worker thread.
bool ensureDownloaded();

// title id (patch id for updates) -> latest version. Parsed on first use and
// re-parsed only when the file on disk changes. Never null.
std::shared_ptr<const VersionMap> latestVersions();

// Latest update version of the game (looks up appId | 0x800), 0 if unknown.
uint32_t latestUpdateVersion(uint64_t appId);

// Release date "YYYY-MM-DD" of `version` of the game `appId`, "" if unknown.
std::string updateDate(uint64_t appId, uint32_t version);

// Game is listed in versions.json (it knows games without any update too);
// `latestVersion` gets its newest update there, 0 if none.
bool knownGame(uint64_t appId, uint32_t* latestVersion = nullptr);

inline uint32_t updateOrdinal(uint32_t version) { return version >> 16; }

// "2026-09-01" -> "01.09.2026"; anything else is returned unchanged.
std::string formatDate(const std::string& isoDate);

// Parsers, exposed for tests.
VersionMap parseVersionsTxt(const std::string& text);
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>  // app id -> (version, YYYYMMDD)
parseVersionsJson(const std::string& text);

} // namespace titledb
