#pragma once

// All catalog releases of one game, best first (see release_picker.hpp), and
// how their versions are shown (see game_version.hpp).

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../GameData.hpp"

namespace catalog {

// Application ID a catalog entry belongs to: its title_id (or the [0100...]
// tag in the title) with the patch/DLC bits cleared, 0 if none.
uint64_t releaseAppId(const Game& g);

// Releases of every game in one catalog snapshot, by application ID. Built
// once per snapshot (the catalog update or diff swaps it) and shared.
struct ReleaseIndex {
    std::shared_ptr<const std::vector<Game>> snapshot;   // keeps the pointers below alive
    std::unordered_map<uint64_t, std::vector<const Game*>> byApp;
};
std::shared_ptr<const ReleaseIndex> releaseIndex();

// Ranks the given releases of one game, best first: the newest version of
// the plain game on top. `installedName` (may be empty) is the installed
// game's name, used to push down entries with a wrong title_id.
std::vector<const Game*> rankReleasePtrs(std::vector<const Game*> releases, const std::string& installedName);
std::vector<Game> rankReleasesOf(std::vector<Game> releases, const std::string& installedName);

// Scans the current catalog snapshot for releases of `appId`, ranked.
std::vector<Game> findReleases(uint64_t appId, const std::string& installedName = "");

// "1.0.3"; "update 3" when only the update number is known; "" if nothing is.
std::string versionLabel(const std::string& version, uint32_t update);

} // namespace catalog
