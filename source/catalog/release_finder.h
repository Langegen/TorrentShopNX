#pragma once

// All catalog releases of one game, best first (see release_picker.hpp).

#include <cstdint>
#include <string>
#include <vector>

#include "../GameData.hpp"

namespace catalog {

// Application ID a catalog entry belongs to: its title_id (or the [0100...]
// tag in the title) with the patch/DLC bits cleared, 0 if none.
uint64_t releaseAppId(const Game& g);

// Ranks the given releases of one game; `installedName` (may be empty) is the
// installed game's name, used to push down entries with a wrong title_id.
std::vector<Game> rankReleasesOf(std::vector<Game> releases, const std::string& installedName);

// Scans the current catalog snapshot for releases of `appId`, ranked.
std::vector<Game> findReleases(uint64_t appId, const std::string& installedName = "");

} // namespace catalog
