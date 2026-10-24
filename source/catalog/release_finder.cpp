#include "release_finder.h"

#include "game_stats_manager.h"
#include "release_picker.hpp"

namespace catalog {

uint64_t releaseAppId(const Game& g) {
    uint64_t tid = parseTitleIdFromGame(g);
    // Application IDs have the low 13 bits clear; patch (|0x800) and DLC
    // (+0x1000..) IDs found in the catalog map back to their game.
    return tid ? (tid & ~0x1FFFULL) : 0;
}

std::vector<Game> rankReleasesOf(std::vector<Game> releases, const std::string& installedName) {
    if (releases.size() <= 1) return releases;
    std::vector<ReleaseInfo> infos;
    infos.reserve(releases.size());
    for (const Game& g : releases) {
        ReleaseInfo ri;
        ri.title = g.title;
        ri.topic_id = g.topic_id;
        ri.homebrew = isHomebrewGame(g);
        ri.nameMatch = installedName.empty() || releaseNameMatches(installedName, g.title);
        ri.updateVersion = g.update_version;
        GameStats st;
        if (GameStatsManager::instance().copySwitchStats(g.topic_id, st)) {
            ri.registered = st.latest_timestamp;
            ri.seeds = st.seeds;
        }
        infos.push_back(std::move(ri));
    }
    std::vector<Game> out;
    out.reserve(releases.size());
    for (size_t i : rankReleases(infos)) out.push_back(std::move(releases[i]));
    return out;
}

std::vector<Game> findReleases(uint64_t appId, const std::string& installedName) {
    std::vector<Game> found;
    if (appId == 0) return found;
    auto snap = getCatalogSnapshot();
    if (!snap) return found;
    for (const Game& g : *snap) {
        if (releaseAppId(g) == appId) found.push_back(g);
    }
    return rankReleasesOf(std::move(found), installedName);
}

} // namespace catalog
