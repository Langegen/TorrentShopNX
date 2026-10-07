#include "game_stats_manager.h"
#include "../GameData.hpp"
#include "../net/http_client.h"
#include "../utils/app_paths.h"
#include "../utils/log.h"
#include <borealis.hpp>
#include <borealis/extern/nlohmann/json.hpp>
#include <filesystem>
#include <algorithm>

namespace catalog {

GameStatsManager& GameStatsManager::instance() {
    static GameStatsManager inst;
    return inst;
}

GameStatsManager::GameStatsManager() {
    loadFromDisk();
}

std::string GameStatsManager::getSwitchStatsCachePath() {
    return std::string(TSNX_CACHE_DIR) + "/switch_games_stats.json";
}

std::string GameStatsManager::getConsoleStatsCachePath() {
    return std::string(TSNX_CACHE_DIR) + "/console_games_stats.json";
}

int64_t GameStatsManager::parseTimestamp(const std::string& date_str) {
    if (date_str.empty()) return 0;
    int64_t ts = 0;
    int digitCount = 0;
    for (char c : date_str) {
        if (c >= '0' && c <= '9') {
            ts = ts * 10 + (c - '0');
            digitCount++;
        }
    }
    // Normalize to 14 digits (YYYYMMDDHHMMSS)
    if (digitCount == 8) { // YYYYMMDD -> YYYYMMDD000000
        ts *= 1000000LL;
    } else if (digitCount == 12) { // YYYYMMDDHHMM -> YYYYMMDDHHMM00
        ts *= 100LL;
    }
    return ts;
}

bool GameStatsManager::parseStatsJson(const std::string& json_str, std::unordered_map<std::string, GameStats>& out_map) {
    if (json_str.empty()) return false;
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.is_object()) return false;

        out_map.clear();
        out_map.reserve(j.size());

        for (auto it = j.begin(); it != j.end(); ++it) {
            const auto& key = it.key();
            const auto& val = it.value();
            if (!val.is_object()) continue;

            GameStats s;
            if (val.contains("seeds") && val["seeds"].is_number()) {
                s.seeds = val["seeds"].get<int32_t>();
            }
            if (val.contains("leeches") && val["leeches"].is_number()) {
                s.leeches = val["leeches"].get<int32_t>();
            }
            if (val.contains("downloads") && val["downloads"].is_number()) {
                s.downloads = val["downloads"].get<int64_t>();
            }
            if (val.contains("registered_at") && val["registered_at"].is_string()) {
                s.registered_at = val["registered_at"].get<std::string>();
            }
            if (val.contains("updated_at") && val["updated_at"].is_string()) {
                s.updated_at = val["updated_at"].get<std::string>();
            }

            int64_t regTs = parseTimestamp(s.registered_at);
            int64_t upTs = parseTimestamp(s.updated_at);
            s.latest_timestamp = std::max(regTs, upTs);

            out_map[key] = std::move(s);
        }
        return true;
    } catch (const std::exception& e) {
        util::logLine("GameStatsManager: parse error: " + std::string(e.what()));
        return false;
    } catch (...) {
        util::logLine("GameStatsManager: unknown parse error");
        return false;
    }
}

void GameStatsManager::loadFromDisk() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // Load switch stats cache
    std::string switchPath = getSwitchStatsCachePath();
    std::string switchBody;
    if (readFileFast(switchPath, switchBody) && !switchBody.empty()) {
        std::unordered_map<std::string, GameStats> loaded;
        if (parseStatsJson(switchBody, loaded)) {
            switch_stats_ = std::move(loaded);
            util::logLine("GameStatsManager: loaded " + std::to_string(switch_stats_.size()) + " switch stats from disk");
        }
    }

    // Load console stats cache
    std::string consolePath = getConsoleStatsCachePath();
    std::string consoleBody;
    if (readFileFast(consolePath, consoleBody) && !consoleBody.empty()) {
        std::unordered_map<std::string, GameStats> loaded;
        if (parseStatsJson(consoleBody, loaded)) {
            console_stats_ = std::move(loaded);
            util::logLine("GameStatsManager: loaded " + std::to_string(console_stats_.size()) + " console stats from disk");
        }
    }

    loaded_from_disk_ = true;
}

const GameStats* GameStatsManager::getSwitchStats(const std::string& topic_id) const {
    if (topic_id.empty()) return nullptr;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = switch_stats_.find(topic_id);
    if (it != switch_stats_.end()) return &it->second;
    return nullptr;
}

const GameStats* GameStatsManager::getConsoleStats(const std::string& topic_id) const {
    if (topic_id.empty()) return nullptr;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = console_stats_.find(topic_id);
    if (it != console_stats_.end()) return &it->second;
    return nullptr;
}

const GameStats* GameStatsManager::getAnyStats(const std::string& topic_id) const {
    if (topic_id.empty()) return nullptr;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = switch_stats_.find(topic_id);
    if (it != switch_stats_.end()) return &it->second;
    auto itC = console_stats_.find(topic_id);
    if (itC != console_stats_.end()) return &itC->second;
    return nullptr;
}

void GameStatsManager::updateStatsInBackground(std::function<void(bool success)> onDone) {
    brls::async([this, onDone]() {
        net::HttpClient client;
        client.setTimeout(30);

        bool anySuccess = false;

        // 1. Download Switch stats
        util::logLine("GameStatsManager: downloading switch stats from " + std::string(kSwitchStatsUrl));
        auto switchRes = client.httpGet(kSwitchStatsUrl);
        if (switchRes.status_code == 200 && !switchRes.body.empty()) {
            std::unordered_map<std::string, GameStats> newMap;
            if (parseStatsJson(switchRes.body, newMap)) {
                writeTextFile(getSwitchStatsCachePath(), switchRes.body);
                {
                    std::unique_lock<std::shared_mutex> lock(mutex_);
                    switch_stats_ = std::move(newMap);
                }
                anySuccess = true;
                util::logLine("GameStatsManager: successfully updated switch stats (" +
                              std::to_string(switch_stats_.size()) + " entries)");
            }
        } else {
            util::logLine("GameStatsManager: switch stats fetch failed with status " +
                          std::to_string(switchRes.status_code));
        }

        // 2. Download Console stats (gracefully handles 404)
        util::logLine("GameStatsManager: downloading console stats from " + std::string(kConsoleStatsUrl));
        auto consoleRes = client.httpGet(kConsoleStatsUrl);
        if (consoleRes.status_code == 200 && !consoleRes.body.empty()) {
            std::unordered_map<std::string, GameStats> newMap;
            if (parseStatsJson(consoleRes.body, newMap)) {
                writeTextFile(getConsoleStatsCachePath(), consoleRes.body);
                {
                    std::unique_lock<std::shared_mutex> lock(mutex_);
                    console_stats_ = std::move(newMap);
                }
                anySuccess = true;
                util::logLine("GameStatsManager: successfully updated console stats (" +
                              std::to_string(console_stats_.size()) + " entries)");
            }
        } else if (consoleRes.status_code == 404) {
            util::logLine("GameStatsManager: console stats file not found on GitHub (404), keeping existing");
        } else {
            util::logLine("GameStatsManager: console stats fetch failed with status " +
                          std::to_string(consoleRes.status_code));
        }

        if (onDone) {
            brls::sync([onDone, anySuccess]() {
                onDone(anySuccess);
            });
        }
    });
}

} // namespace catalog
