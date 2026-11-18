#include "pc_catalog_manager.h"
#include "game_stats_manager.h"
#include "../GameData.hpp"
#include "../net/http_client.h"
#include "../utils/app_paths.h"
#include "../utils/log.h"
#include "../utils/atomic_file.h"
#include "../utils/switch_utils.h"
#include <borealis.hpp>
#include <borealis/extern/nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <ctime>
#include <sys/stat.h>

extern std::atomic<bool> g_appExiting;

namespace catalog {

namespace {

class PcGameSaxConsumer {
public:
    std::vector<Game>& games;
    Game currentGame;
    std::string currentKey;
    std::string rawTitle;
    std::string subforumId;
    std::string coverUrl;
    std::string coverStandardUrl;
    std::string releaseType;
    bool isPortable = false;

    int arrayDepth = 0;
    int objectDepth = 0;
    int gameObjectDepth = -1;
    bool inGamesArray = false;
    bool inGameObject = false;
    bool inScreenshots = false;

    explicit PcGameSaxConsumer(std::vector<Game>& out_games) : games(out_games) {}

    bool null() { return true; }

    bool boolean(bool val) {
        if (inGameObject && !inScreenshots) {
            if (currentKey == "is_portable") {
                isPortable = val;
            }
        }
        return true;
    }

    bool number_integer(int64_t val) {
        if (inGameObject && !inScreenshots) {
            if (currentKey == "is_portable") {
                isPortable = (val != 0);
            } else if (currentKey == "subforum_id") {
                subforumId = std::to_string(val);
            } else if (currentKey == "topic_id") {
                currentGame.topic_id = std::to_string(val);
            }
        }
        return true;
    }

    bool number_unsigned(uint64_t val) {
        if (inGameObject && !inScreenshots) {
            if (currentKey == "is_portable") {
                isPortable = (val != 0);
            } else if (currentKey == "subforum_id") {
                subforumId = std::to_string(val);
            } else if (currentKey == "topic_id") {
                currentGame.topic_id = std::to_string(val);
            }
        }
        return true;
    }

    bool number_float(double, const std::string&) { return true; }

    bool string(std::string& val) {
        if (inScreenshots) {
            currentGame.screenshots.push_back(std::move(val));
        } else if (inGameObject) {
            if (currentKey == "topic_id") currentGame.topic_id = std::move(val);
            else if (currentKey == "title") currentGame.title = std::move(val);
            else if (currentKey == "raw_title") rawTitle = std::move(val);
            else if (currentKey == "size") currentGame.size = std::move(val);
            else if (currentKey == "subforum_id") subforumId = std::move(val);
            else if (currentKey == "magnet") currentGame.magnet = std::move(val);
            else if (currentKey == "url") currentGame.url = std::move(val);
            else if (currentKey == "year") currentGame.year = std::move(val);
            else if (currentKey == "genre") currentGame.genre = std::move(val);
            else if (currentKey == "developer") currentGame.developer = std::move(val);
            else if (currentKey == "publisher") currentGame.publisher = std::move(val);
            else if (currentKey == "version") currentGame.version = std::move(val);
            else if (currentKey == "release_type") releaseType = std::move(val);
            else if (currentKey == "interface_lang") currentGame.interface_lang = std::move(val);
            else if (currentKey == "voice_lang") currentGame.voice_lang = std::move(val);
            else if (currentKey == "cover") coverUrl = std::move(val);
            else if (currentKey == "cover_standard") coverStandardUrl = std::move(val);
            else if (currentKey == "description") currentGame.description = std::move(val);
        }
        return true;
    }

    bool binary(nlohmann::json::binary_t&) { return true; }

    bool start_object(std::size_t) {
        objectDepth++;
        if (inGamesArray && !inGameObject && !inScreenshots) {
            inGameObject = true;
            gameObjectDepth = objectDepth;
            currentGame = Game{};
            rawTitle.clear();
            subforumId.clear();
            coverUrl.clear();
            coverStandardUrl.clear();
            releaseType.clear();
            isPortable = false;
        }
        return true;
    }

    bool key(std::string& val) {
        currentKey = std::move(val);
        return true;
    }

    bool end_object() {
        if (inGameObject && objectDepth == gameObjectDepth) {
            if (currentGame.title.empty()) {
                currentGame.title = std::move(rawTitle);
            }
            if (!currentGame.topic_id.empty() && !currentGame.title.empty()) {
                currentGame.content_type = "pc:" + subforumId;
                if (!coverStandardUrl.empty()) {
                    currentGame.cover = std::move(coverStandardUrl);
                } else {
                    currentGame.cover = std::move(coverUrl);
                }
                if (releaseType.empty() && isPortable) {
                    currentGame.image_format = "Portable";
                } else {
                    currentGame.image_format = std::move(releaseType);
                }
                currentGame.sort_key = computeSortKey(currentGame.title);
                games.push_back(std::move(currentGame));
            }
            inGameObject = false;
            gameObjectDepth = -1;
        }
        objectDepth--;
        return true;
    }

    bool start_array(std::size_t) {
        arrayDepth++;
        if (!inGamesArray) {
            if (arrayDepth == 1 && objectDepth == 0) {
                inGamesArray = true;
            }
        } else if (inGameObject && currentKey == "screenshots") {
            inScreenshots = true;
        }
        return true;
    }

    bool end_array() {
        if (inScreenshots) {
            inScreenshots = false;
        } else if (inGamesArray && arrayDepth == 1 && objectDepth == 0) {
            inGamesArray = false;
        }
        arrayDepth--;
        return true;
    }

    bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& ex) {
        util::logLine("PcCatalogManager: SAX parse error at " + std::to_string(position) + ": " + ex.what());
        return false;
    }
};

} // namespace

PcCatalogManager& PcCatalogManager::instance() {
    static PcCatalogManager inst;
    return inst;
}

PcCatalogManager::PcCatalogManager() = default;

static std::string getTimestampPath() {
    return std::string(TSNX_PC_DATA_DIR) + "/last_update.txt";
}

bool PcCatalogManager::hasLocalCache() const {
    struct stat st;
    if (stat(TSNX_PC_GAMES_BIN, &st) == 0 && st.st_size > 0) return true;
    if (stat(TSNX_PC_GAMES_JSON, &st) == 0 && st.st_size > 0) return true;
    return false;
}

bool PcCatalogManager::isLoaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ && !all_games_.empty();
}

const std::vector<PcSubforum>& PcCatalogManager::getSubforums() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subforums_;
}

const PcSubforum* PcCatalogManager::findSubforum(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subforums_map_.find(id);
    if (it != subforums_map_.end()) return &it->second;
    return nullptr;
}

const std::vector<Game>& PcCatalogManager::getAllGames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return all_games_;
}

std::vector<const Game*> PcCatalogManager::getGamesForSubforum(const std::string& subforum_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const Game*> res;
    auto it = subforum_to_game_indices_.find(subforum_id);
    if (it != subforum_to_game_indices_.end()) {
        res.reserve(it->second.size());
        for (size_t idx : it->second) {
            if (idx < all_games_.size()) {
                res.push_back(&all_games_[idx]);
            }
        }
    }
    return res;
}

size_t PcCatalogManager::getGamesCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return all_games_.size();
}

size_t PcCatalogManager::getGamesCountForSubforum(const std::string& subforum_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subforum_to_game_indices_.find(subforum_id);
    if (it != subforum_to_game_indices_.end()) {
        return it->second.size();
    }
    return 0;
}

bool PcCatalogManager::shouldAutoUpdate() const {
    std::string tsStr;
    if (!readFileFast(getTimestampPath(), tsStr) || tsStr.empty()) {
        return true;
    }
    try {
        time_t lastTime = static_cast<time_t>(std::stoll(tsStr));
        time_t now = std::time(nullptr);
        if (now > lastTime && (now - lastTime) >= 86400) {
            return true;
        }
    } catch (...) {
        return true;
    }
    return false;
}

void PcCatalogManager::recordUpdateTimestamp() {
    time_t now = std::time(nullptr);
    writeTextFile(getTimestampPath(), std::to_string(now));
}

bool PcCatalogManager::parseSubforumsJson(const std::string& content) {
    if (content.empty()) return false;
    try {
        auto j = nlohmann::json::parse(content);
        if (!j.is_array()) return false;

        std::vector<PcSubforum> list;
        std::unordered_map<std::string, PcSubforum> map;
        list.reserve(j.size());

        for (const auto& item : j) {
            if (!item.is_object()) continue;
            PcSubforum sf;
            sf.id = safeGetStr(item, "id");
            sf.name = safeGetStr(item, "name");
            sf.slug = safeGetStr(item, "slug");
            sf.parent = safeGetStr(item, "parent");
            if (item.contains("is_retro") && item["is_retro"].is_boolean()) {
                sf.is_retro = item["is_retro"].get<bool>();
            }
            if (item.contains("games_count") && item["games_count"].is_number()) {
                sf.games_count = item["games_count"].get<int>();
            }
            if (!sf.id.empty()) {
                map[sf.id] = sf;
                list.push_back(std::move(sf));
            }
        }

        subforums_ = std::move(list);
        subforums_map_ = std::move(map);
        util::logLine("PcCatalogManager: parsed " + std::to_string(subforums_.size()) + " subforums");
        return true;
    } catch (const std::exception& e) {
        util::logLine("PcCatalogManager: error parsing subforums: " + std::string(e.what()));
        return false;
    }
}

size_t PcCatalogManager::getTotalKnownGamesCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!all_games_.empty()) return all_games_.size();
    size_t total = 0;
    for (const auto& sf : subforums_) {
        total += sf.games_count;
    }
    return total;
}

bool PcCatalogManager::loadSubforums(bool allow_download) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!subforums_.empty()) return true;
    }

    std::string sfContent;
    if (readFileFast(TSNX_PC_SUBFORUMS_JSON, sfContent) && !sfContent.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (parseSubforumsJson(sfContent)) return true;
    }

    if (allow_download) {
        net::HttpClient client;
        client.setTimeout(15);
        auto res = client.httpGet(kSubforumsUrl);
        if (res.status_code == 200 && !res.body.empty()) {
            writeTextFile(TSNX_PC_SUBFORUMS_JSON, res.body);
            std::lock_guard<std::mutex> lock(mutex_);
            return parseSubforumsJson(res.body);
        }
    }
    return false;
}

bool PcCatalogManager::parsePcGamesFromFile(const std::string& filePath, std::vector<Game>& out_games) {
    std::string content;
    if (!readFileFast(filePath, content) || content.empty()) {
        util::logLine("PcCatalogManager: failed to read file: " + filePath);
        return false;
    }
    bool ok = parsePcGamesJson(content, out_games);
    std::string().swap(content); // Release buffer immediately
    return ok;
}

bool PcCatalogManager::parsePcGamesJson(const std::string& content, std::vector<Game>& out_games) {
    if (content.empty()) return false;
    out_games.clear();
    out_games.reserve(8000);
    PcGameSaxConsumer consumer(out_games);
    bool ok = nlohmann::json::sax_parse(content, &consumer);
    if (!ok || out_games.empty()) {
        util::logLine("PcCatalogManager: SAX parsing failed or empty result");
        out_games.clear();
        return false;
    }
    util::logLine("PcCatalogManager: parsed " + std::to_string(out_games.size()) + " PC games via SAX");
    return true;
}

void PcCatalogManager::rebuildIndex() {
    subforum_to_game_indices_.clear();
    std::unordered_map<std::string, int> counts;

    for (size_t i = 0; i < all_games_.size(); ++i) {
        std::string sfId = getPcSubforumId(all_games_[i]);
        if (!sfId.empty()) {
            subforum_to_game_indices_[sfId].push_back(i);
            counts[sfId]++;
        }
    }

    for (auto& sf : subforums_) {
        auto it = counts.find(sf.id);
        if (it != counts.end()) {
            sf.games_count = it->second;
            subforums_map_[sf.id].games_count = it->second;
        }
    }
}

bool PcCatalogManager::loadCatalog(bool allow_download,
                                   ProgressCallback progress_cb) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(TSNX_PC_DATA_DIR), ec);

    // 1. Ensure subforums loaded
    loadSubforums(allow_download);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_ && !all_games_.empty()) return true;
    }

    // 2. Load games from binary cache if valid
    struct stat stBin, stJson;
    bool hasBin = (stat(TSNX_PC_GAMES_BIN, &stBin) == 0 && stBin.st_size > 0);
    bool hasJson = (stat(TSNX_PC_GAMES_JSON, &stJson) == 0 && stJson.st_size > 0);

    if (hasBin && (!hasJson || stBin.st_mtime >= stJson.st_mtime)) {
        std::vector<Game> loaded;
        if (loadGamesFromBinaryFile(TSNX_PC_GAMES_BIN, loaded) && !loaded.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            all_games_ = std::move(loaded);
            rebuildIndex();
            loaded_ = true;
            util::logLine("PcCatalogManager: loaded " + std::to_string(all_games_.size()) + " games from binary cache");
            return true;
        }
    }

    // 3. Fallback: Parse JSON if present (streaming SAX, never blocks memory)
    if (hasJson) {
        if (progress_cb) progress_cb(0, 0, "app/common/processing"_i18n);
        std::vector<Game> parsed;
        if (parsePcGamesFromFile(TSNX_PC_GAMES_JSON, parsed) && !parsed.empty()) {
            saveGamesToBinaryFile(TSNX_PC_GAMES_BIN, parsed);
            std::lock_guard<std::mutex> lock(mutex_);
            all_games_ = std::move(parsed);
            rebuildIndex();
            loaded_ = true;
            return true;
        }
    }

    // 4. Download if missing and allowed
    if (allow_download) {
        return downloadCatalog(progress_cb);
    }

    return false;
}

bool PcCatalogManager::downloadCatalog(ProgressCallback progress_cb) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(TSNX_PC_DATA_DIR), ec);

    net::HttpClient client;
    client.setTimeout(120);

    // 1. Subforums
    util::logLine("PcCatalogManager: downloading subforums from " + std::string(kSubforumsUrl));
    auto sfRes = client.httpGet(kSubforumsUrl);
    if (sfRes.status_code == 200 && !sfRes.body.empty()) {
        writeTextFile(TSNX_PC_SUBFORUMS_JSON, sfRes.body);
        std::lock_guard<std::mutex> lock(mutex_);
        parseSubforumsJson(sfRes.body);
    }

    // 2. PC Games JSON
    std::string tmpJson = std::string(TSNX_PC_GAMES_JSON) + ".tmp";
    util::logLine("PcCatalogManager: downloading PC games catalog from " + std::string(kPcGamesUrl));

    if (progress_cb) {
        client.setProgressCallback([progress_cb](int64_t total, int64_t now) {
            if (total > 0 && now >= 0) {
                progress_cb(static_cast<size_t>(now), static_cast<size_t>(total), "app/common/loading"_i18n);
            }
        });
    }

    bool dlOk = client.downloadToFile(kPcGamesUrl, tmpJson, &g_appExiting, 180);
    if (!dlOk || g_appExiting.load()) {
        util::logLine("PcCatalogManager: download failed or canceled");
        std::filesystem::remove(tmpJson, ec);
        return false;
    }

    if (progress_cb) {
        progress_cb(100, 100, "app/common/processing"_i18n);
    }

    std::vector<Game> parsed;
    if (!parsePcGamesFromFile(tmpJson, parsed) || parsed.empty()) {
        util::logLine("PcCatalogManager: SAX parse failed for downloaded catalog");
        std::filesystem::remove(tmpJson, ec);
        return false;
    }

    // Replace JSON atomically
    std::filesystem::remove(TSNX_PC_GAMES_JSON, ec);
    std::filesystem::rename(tmpJson, TSNX_PC_GAMES_JSON, ec);

    // Save binary cache
    saveGamesToBinaryFile(TSNX_PC_GAMES_BIN, parsed);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        all_games_ = std::move(parsed);
        rebuildIndex();
        loaded_ = true;
    }

    recordUpdateTimestamp();

    // 3. Stats update
    GameStatsManager::instance().updateStatsInBackground();

    util::logLine("PcCatalogManager: successfully updated catalog with " +
                  std::to_string(all_games_.size()) + " games");
    return true;
}

void PcCatalogManager::updateCatalogInBackground(std::function<void(bool)> on_done) {
    bool expected = false;
    if (!updating_.compare_exchange_strong(expected, true)) {
        if (on_done) {
            brls::sync([on_done]() { on_done(false); });
        }
        return;
    }

    brls::async([this, on_done]() {
        bool ok = downloadCatalog(ProgressCallback(nullptr));
        updating_.store(false, std::memory_order_release);
        if (on_done) {
            brls::sync([on_done, ok]() {
                on_done(ok);
            });
        }
    });
}

} // namespace catalog
