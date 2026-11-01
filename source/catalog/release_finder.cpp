#include "release_finder.h"

#include <borealis.hpp>
#include <mutex>

#include "game_stats_manager.h"
#include "game_version.hpp"
#include "release_picker.hpp"

namespace catalog {

uint64_t releaseAppId(const Game& g) {
    uint64_t tid = parseTitleIdFromGame(g);
    // Application IDs have the low 13 bits clear; patch (|0x800) and DLC
    // (+0x1000..) IDs found in the catalog map back to their game.
    return tid ? (tid & ~0x1FFFULL) : 0;
}

std::shared_ptr<const ReleaseIndex> releaseIndex() {
    static std::mutex mtx;
    static std::shared_ptr<const ReleaseIndex> cached;

    auto snap = getCatalogSnapshot();
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (cached && cached->snapshot == snap) return cached;
    }
    auto idx = std::make_shared<ReleaseIndex>();
    idx->snapshot = snap;
    if (snap) {
        idx->byApp.reserve(snap->size());
        for (const Game& g : *snap) {
            uint64_t app = releaseAppId(g);
            // 0100000000000000 is a placeholder ID homebrew releases carry,
            // not one game.
            if (app == 0 || app == 0x0100000000000000ULL) continue;
            // The catalog does carry patch and DLC IDs: the scraper takes the
            // first ID from the torrent's file list, e.g. Zelda BotW NSZ
            // (6702315) is filed under its DLC 01007EF00011F000.
            idx->byApp[app].push_back(&g);
        }
    }
    std::lock_guard<std::mutex> lock(mtx);
    cached = idx;
    return cached;
}

std::vector<const Game*> rankReleasePtrs(std::vector<const Game*> releases, const std::string& installedName) {
    if (releases.size() <= 1) return releases;
    std::vector<ReleaseInfo> infos;
    infos.reserve(releases.size());
    for (const Game* g : releases) {
        ReleaseInfo ri;
        ri.title = g->title;
        ri.topic_id = g->topic_id;
        ri.homebrew = isHomebrewGame(*g);
        ri.nameMatch = installedName.empty() || releaseNameMatches(installedName, g->title);
        ri.updateVersion = g->update_version;
        ri.version = g->version;
        GameStats st;
        if (GameStatsManager::instance().copySwitchStats(g->topic_id, st)) {
            ri.registered = st.latest_timestamp;
            ri.seeds = st.seeds;
        }
        infos.push_back(std::move(ri));
    }
    std::vector<const Game*> out;
    out.reserve(releases.size());
    for (size_t i : rankReleases(infos)) out.push_back(releases[i]);
    return out;
}

std::vector<Game> rankReleasesOf(std::vector<Game> releases, const std::string& installedName) {
    if (releases.size() <= 1) return releases;
    std::vector<const Game*> ptrs;
    ptrs.reserve(releases.size());
    for (const Game& g : releases) ptrs.push_back(&g);
    std::vector<Game> out;
    out.reserve(releases.size());
    for (const Game* g : rankReleasePtrs(std::move(ptrs), installedName)) out.push_back(*g);
    return out;
}

std::vector<Game> findReleases(uint64_t appId, const std::string& installedName) {
    std::vector<Game> found;
    if (appId == 0) return found;
    auto idx = releaseIndex();
    auto it = idx->byApp.find(appId);
    if (it == idx->byApp.end()) return found;
    for (const Game* g : rankReleasePtrs(it->second, installedName)) found.push_back(*g);
    return found;
}

std::string versionLabel(const std::string& version, uint32_t update) {
    std::string v = trimVersion(version);
    if (!v.empty() || !update) return v;
    return brls::getStr("app/library/update_label", std::to_string(updateOrdinal(update)));
}

} // namespace catalog
