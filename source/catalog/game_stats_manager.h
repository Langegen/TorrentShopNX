#pragma once

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <functional>
#include <cstdint>

namespace catalog {

struct GameStats {
    int32_t seeds = 0;
    int32_t leeches = 0;
    int64_t downloads = 0;
    std::string registered_at;
    std::string updated_at;
    int64_t latest_timestamp = 0; // YYYYMMDDHHMMSS as int64 for fast O(1) comparison
};

class GameStatsManager {
public:
    static GameStatsManager& instance();

    // Fast O(1) thread-safe lookups
    const GameStats* getSwitchStats(const std::string& topic_id) const;
    const GameStats* getConsoleStats(const std::string& topic_id) const;
    const GameStats* getAnyStats(const std::string& topic_id) const;

    // Load cached stats from SD / disk
    void loadFromDisk();

    // Background update from GitHub
    void updateStatsInBackground(std::function<void(bool success)> onDone = nullptr);

    // Helpers
    static int64_t parseTimestamp(const std::string& date_str);
    static std::string getSwitchStatsCachePath();
    static std::string getConsoleStatsCachePath();

    static constexpr const char* kSwitchStatsUrl =
        "https://raw.githubusercontent.com/Langegen/switch-games/main/switch_games_stats.json";
    static constexpr const char* kConsoleStatsUrl =
        "https://raw.githubusercontent.com/Langegen/console-games/main/data/console_games_stats.json";

private:
    GameStatsManager();
    ~GameStatsManager() = default;

    bool parseStatsJson(const std::string& json_str, std::unordered_map<std::string, GameStats>& out_map);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, GameStats> switch_stats_;
    std::unordered_map<std::string, GameStats> console_stats_;
    bool loaded_from_disk_ = false;
};

} // namespace catalog
