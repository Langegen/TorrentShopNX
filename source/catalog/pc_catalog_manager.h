#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <atomic>
#include "../GameData.hpp"

namespace catalog {

struct PcSubforum {
    std::string id;
    std::string name;
    std::string slug;
    std::string parent; // empty if top-level
    bool is_retro = false;
    int games_count = 0;
};

class PcCatalogManager {
public:
    using ProgressCallback = std::function<void(size_t dl, size_t total, const std::string& step)>;

    static PcCatalogManager& instance();

    bool loadSubforums(bool allow_download = true);

    bool loadCatalog(bool allow_download = true,
                     ProgressCallback progress_cb = nullptr);

    bool loadCatalog(bool allow_download, std::function<void(size_t, size_t)> legacy_cb) {
        if (!legacy_cb) return loadCatalog(allow_download, ProgressCallback(nullptr));
        return loadCatalog(allow_download, [legacy_cb](size_t dl, size_t tot, const std::string&) {
            legacy_cb(dl, tot);
        });
    }

    bool downloadCatalog(ProgressCallback progress_cb = nullptr);

    bool downloadCatalog(std::function<void(size_t, size_t)> legacy_cb) {
        if (!legacy_cb) return downloadCatalog(ProgressCallback(nullptr));
        return downloadCatalog([legacy_cb](size_t dl, size_t tot, const std::string&) {
            legacy_cb(dl, tot);
        });
    }

    const std::vector<PcSubforum>& getSubforums() const;
    const PcSubforum* findSubforum(const std::string& id) const;

    const std::vector<Game>& getAllGames() const;
    std::vector<const Game*> getGamesForSubforum(const std::string& subforum_id) const;

    size_t getGamesCount() const;
    size_t getGamesCountForSubforum(const std::string& subforum_id) const;
    size_t getTotalKnownGamesCount() const;

    bool isLoaded() const;
    bool hasLocalCache() const;

    bool shouldAutoUpdate() const;
    void recordUpdateTimestamp();
    void updateCatalogInBackground(std::function<void(bool)> on_done = nullptr);

    static constexpr const char* kPcGamesUrl =
        "https://raw.githubusercontent.com/Langegen/pc-games/main/data/pc_games.json";
    static constexpr const char* kSubforumsUrl =
        "https://raw.githubusercontent.com/Langegen/pc-games/main/data/subforums.json";
    static constexpr const char* kPcStatsUrl =
        "https://raw.githubusercontent.com/Langegen/pc-games/main/data/pc_games_stats.json";

    bool parsePcGamesFromFile(const std::string& filePath, std::vector<Game>& out_games);

private:
    PcCatalogManager();
    ~PcCatalogManager() = default;

    bool parseSubforumsJson(const std::string& content);
    bool parsePcGamesJson(const std::string& content, std::vector<Game>& out_games);
    void rebuildIndex();

    mutable std::mutex mutex_;
    std::vector<PcSubforum> subforums_;
    std::unordered_map<std::string, PcSubforum> subforums_map_;
    std::vector<Game> all_games_;
    std::unordered_map<std::string, std::vector<size_t>> subforum_to_game_indices_;
    bool loaded_ = false;
    std::atomic<bool> updating_{false};
};

} // namespace catalog
