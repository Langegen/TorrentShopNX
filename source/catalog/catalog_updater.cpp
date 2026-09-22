#include "catalog_updater.h"
#include "catalog_manager.h"
#include "collections_manager.h"
#include "config/config.h"
#include "utils/log.h"
#include "utils/app_paths.h"
#include "net/http_client.h"
#include "ui/CatalogProgressNotification.hpp"
#include "ui/CatalogView.hpp"
#include "ui/FavoritesManager.hpp"
#include <borealis.hpp>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <chrono>

namespace catalog {

CatalogUpdater& CatalogUpdater::instance() {
    static CatalogUpdater inst;
    return inst;
}

static void updateNewReleasesCollectionInBackground() {
    std::string cachePath = std::string(TSNX_CACHE_COLLECTIONS) + "/new_release.json";
    std::string url = std::string(kCollectionsBaseUrl) + "new_release.json";

    struct stat st;
    if (stat(cachePath.c_str(), &st) == 0) {
        std::time_t now = std::time(nullptr);
        if (now > 0 && (now - st.st_mtime) < (4 * 3600)) {
            util::logLine("catalog_updater: new_release.json cache is already fresh");
            return;
        }
    }

    util::logLine("catalog_updater: updating new_release.json from " + url);
    net::HttpClient http;
    http.setTimeout(20);
    auto res = http.httpGet(url);
    if (res.status_code == 200 && !res.body.empty()) {
        writeTextFile(cachePath, res.body);
        util::logLine("catalog_updater: successfully updated new_release.json (" + std::to_string(res.body.size()) + " bytes)");
    } else {
        util::logLine("catalog_updater: failed to update new_release.json, status=" + std::to_string(res.status_code));
    }
}

bool CatalogUpdater::isUpdateRunning() const {
    return g_catalogUpdateRunning.load();
}

void CatalogUpdater::checkAndRunScheduledUpdate() {
    auto initialCatalog = getCatalogSnapshot();
    bool was_empty = !initialCatalog || initialCatalog->empty();
    auto& cfg = config::ConfigManager::instance();

    if (was_empty) {
        util::logLine("catalog_updater: catalog is empty, running initial full update");
        startUpdate(CatalogUpdateMode::ForceFull, nullptr, true);
    } else if (cfg.shouldUpdateCatalogFull()) {
        util::logLine("catalog_updater: scheduled 48h full update is due");
        startUpdate(CatalogUpdateMode::ForceFull, nullptr, true);
    } else if (cfg.shouldUpdateCatalogDiff()) {
        util::logLine("catalog_updater: scheduled 4h diff update is due");
        startUpdate(CatalogUpdateMode::ForceDiff, nullptr, true);
    } else {
        util::logLine("catalog_updater: catalog is up-to-date, skipping update");
    }
}

void CatalogUpdater::startUpdate(CatalogUpdateMode mode,
                                 std::function<void(const CatalogUpdateResult&)> onDone,
                                 bool showNotification) {
    bool expected = false;
    if (!g_catalogUpdateRunning.compare_exchange_strong(expected, true)) {
        util::logLine("catalog_updater: update already running, skipping request");
        if (onDone) {
            CatalogUpdateResult res;
            res.success = false;
            res.message = "Already running";
            onDone(res);
        }
        return;
    }

    ui::CatalogProgressNotification* notif = nullptr;
    std::shared_ptr<bool> notifToken = nullptr;
    if (showNotification) {
        notif = new ui::CatalogProgressNotification();
        if (brls::Application::getNotificationManager()) {
            brls::Application::getNotificationManager()->addView(notif);
        }
        notifToken = notif->getAliveToken();
    }

    brls::async([this, mode, onDone, notif, notifToken]() {
        auto& cfg = config::ConfigManager::instance();
        std::string diffUrl = cfg.getEffectiveCatalogDiffUrl();
        std::string fullUrl = cfg.getEffectiveCatalogSourceUrl();
        std::string langKey = cfg.getActiveCatalogLangKey();
        auto initialCatalog = getCatalogSnapshot();
        bool was_empty = !initialCatalog || initialCatalog->empty();

        CatalogUpdateResult res;

        if (mode == CatalogUpdateMode::ForceFull || was_empty) {
            res = performFullUpdate(fullUrl, notifToken, notif);
        } else if (mode == CatalogUpdateMode::ForceDiff) {
            res = performDiffUpdate(diffUrl, langKey, notifToken, notif);
        } else { // CatalogUpdateMode::Auto
            if (cfg.shouldUpdateCatalogFull()) {
                res = performFullUpdate(fullUrl, notifToken, notif);
            } else {
                res = performDiffUpdate(diffUrl, langKey, notifToken, notif);
                // If diff returned error (e.g. 404 on server), fallback to full update
                if (!res.success && !res.already_up_to_date && !g_appExiting.load()) {
                    util::logLine("catalog_updater: diff update failed, falling back to full catalog check");
                    res = performFullUpdate(fullUrl, notifToken, notif);
                }
            }
        }

        // Also refresh new_release.json collection if due (every 4 hours)
        updateNewReleasesCollectionInBackground();

        // Update notification UI
        if (notifToken && *notifToken && notif) {
            brls::sync([notif, notifToken, res]() {
                if (!notifToken || !*notifToken || !notif) return;
                if (res.already_up_to_date) {
                    notif->setCompleted("app/catalog/db_updated"_i18n);
                } else if (res.success) {
                    if (res.was_full) {
                        notif->setCompleted(brls::getStr("app/catalog/loaded_games", std::to_string(getCatalogSnapshot()->size())));
                    } else {
                        std::string msg = brls::getStr("app/settings/update_catalog_diff_success",
                                                        std::to_string(res.added_count),
                                                        std::to_string(res.updated_count));
                        notif->setCompleted(msg);
                    }
                } else {
                    notif->setFailed("app/catalog/update_failed"_i18n);
                }
            });
        }

        if (onDone) {
            brls::sync([onDone, res]() {
                onDone(res);
            });
        }

        g_catalogUpdateRunning = false;
    });
}

bool CatalogUpdater::applyDiffToGames(std::vector<Game>& currentGames,
                                     const nlohmann::json& diffJson,
                                     const std::string& langKey,
                                     int& outAdded,
                                     int& outUpdated,
                                     int& outDeleted) {
    outAdded = 0;
    outUpdated = 0;
    outDeleted = 0;

    if (!diffJson.is_object() && !diffJson.is_array()) {
        util::logLine("catalog_updater: invalid diff JSON root type");
        return false;
    }

    // 1. Process deletions
    std::unordered_set<std::string> deletedSet;
    if (diffJson.is_object() && diffJson.contains("deleted_topic_ids") && diffJson["deleted_topic_ids"].is_array()) {
        for (const auto& item : diffJson["deleted_topic_ids"]) {
            if (item.is_string()) {
                deletedSet.insert(item.get<std::string>());
            } else if (item.is_number()) {
                deletedSet.insert(std::to_string(item.get<int64_t>()));
            }
        }
    }

    // 2. Locate the array of games for the requested language
    const nlohmann::json* gamesArr = nullptr;
    if (diffJson.is_object()) {
        if (diffJson.contains("added_or_updated")) {
            const auto& aou = diffJson["added_or_updated"];
            if (aou.is_object()) {
                if (aou.contains(langKey) && aou[langKey].is_array()) {
                    gamesArr = &aou[langKey];
                } else if (aou.contains("en") && aou["en"].is_array()) {
                    gamesArr = &aou["en"];
                } else if (aou.contains("ru") && aou["ru"].is_array()) {
                    gamesArr = &aou["ru"];
                } else if (!aou.empty() && aou.begin()->is_array()) {
                    gamesArr = &(*aou.begin());
                }
            } else if (aou.is_array()) {
                gamesArr = &aou;
            }
        }
    } else if (diffJson.is_array()) {
        gamesArr = &diffJson;
    }

    if (!gamesArr || !gamesArr->is_array()) {
        util::logLine("catalog_updater: no game entries found in diff for lang '" + langKey + "'");
        return false;
    }

    // Index existing games by topic_id
    std::unordered_map<std::string, size_t> topicIndex;
    topicIndex.reserve(currentGames.size());
    for (size_t i = 0; i < currentGames.size(); ++i) {
        if (!currentGames[i].topic_id.empty()) {
            topicIndex[currentGames[i].topic_id] = i;
        }
    }

    std::vector<Game> newGames;
    for (const auto& item : *gamesArr) {
        if (!item.is_object()) continue;
        Game g;
        from_json(item, g);
        if (g.topic_id.empty()) continue;

        auto it = topicIndex.find(g.topic_id);
        if (it != topicIndex.end()) {
            currentGames[it->second] = std::move(g);
            outUpdated++;
        } else {
            newGames.push_back(std::move(g));
            outAdded++;
        }
    }

    // Prepend new games to the top of catalog so new releases appear first
    if (!newGames.empty()) {
        currentGames.insert(currentGames.begin(),
                            std::make_move_iterator(newGames.begin()),
                            std::make_move_iterator(newGames.end()));
    }

    // Apply deletions if any
    if (!deletedSet.empty()) {
        auto it = std::remove_if(currentGames.begin(), currentGames.end(), [&](const Game& g) {
            if (deletedSet.count(g.topic_id)) {
                outDeleted++;
                return true;
            }
            return false;
        });
        currentGames.erase(it, currentGames.end());
    }

    util::logLine("catalog_updater: applied diff successfully: added=" + std::to_string(outAdded) +
                  " updated=" + std::to_string(outUpdated) + " deleted=" + std::to_string(outDeleted));
    return true;
}

CatalogUpdateResult CatalogUpdater::performDiffUpdate(const std::string& diffUrl,
                                                      const std::string& langKey,
                                                      const std::shared_ptr<bool>& aliveToken,
                                                      ui::CatalogProgressNotification* notif) {
    CatalogUpdateResult res;
    res.was_full = false;

    util::logLine("catalog_updater: starting diff update from " + diffUrl + " for lang " + langKey);
    if (aliveToken && *aliveToken && notif) {
        brls::sync([notif, aliveToken]() {
            if (aliveToken && *aliveToken && notif) {
                notif->updateProgress(10.0f, "app/settings/update_catalog_downloading_diff"_i18n);
            }
        });
    }

    std::string savedEtag;
    if (std::filesystem::exists(getCatalogBinPath())) {
        savedEtag = readTextFile(getCatalogDiffEtagPath());
        while (!savedEtag.empty() && (savedEtag.back() == '\r' || savedEtag.back() == '\n' || savedEtag.back() == ' '))
            savedEtag.pop_back();
    }

    std::vector<std::string> extra_headers;
    if (!savedEtag.empty()) {
        extra_headers.push_back("If-None-Match: " + savedEtag);
        util::logLine("catalog_updater: checking diff with ETag: " + savedEtag);
    }

    net::HttpClient http;
    std::string tempDiffPath = getCatalogPath() + ".diff.tmp";
    auto dl_res = http.downloadToFileEx(diffUrl, tempDiffPath, extra_headers, &g_appExiting, 60);

    if (dl_res.not_modified && !g_appExiting.load()) {
        util::logLine("catalog_updater: diff HTTP 304 Not Modified - catalog is already up to date");
        res.success = true;
        res.already_up_to_date = true;
        auto& cfg = config::ConfigManager::instance();
        cfg.setLastCatalogDiffTime(std::time(nullptr));
        cfg.save();
        return res;
    }

    if (!dl_res.success || g_appExiting.load()) {
        util::logLine("catalog_updater: diff download failed with HTTP " + std::to_string(dl_res.http_code));
        std::error_code ec;
        std::filesystem::remove(tempDiffPath, ec);
        res.success = false;
        res.message = "HTTP " + std::to_string(dl_res.http_code);
        return res;
    }

    if (aliveToken && *aliveToken && notif) {
        brls::sync([notif, aliveToken]() {
            if (aliveToken && *aliveToken && notif) {
                notif->updateProgress(60.0f, "app/settings/update_catalog_applying_diff"_i18n);
            }
        });
    }

    std::string body;
    if (!readFileFast(tempDiffPath, body)) {
        util::logLine("catalog_updater: failed to read downloaded diff file");
        std::error_code ec;
        std::filesystem::remove(tempDiffPath, ec);
        res.success = false;
        return res;
    }

    nlohmann::json diffJson = nlohmann::json::parse(body, nullptr, false);
    if (diffJson.is_discarded()) {
        util::logLine("catalog_updater: failed to parse diff JSON");
        std::error_code ec;
        std::filesystem::remove(tempDiffPath, ec);
        res.success = false;
        return res;
    }

    auto currentSnapshot = getCatalogSnapshot();
    std::vector<Game> gamesCopy = currentSnapshot ? *currentSnapshot : std::vector<Game>();
    if (gamesCopy.empty()) {
        util::logLine("catalog_updater: local catalog is empty, diff cannot be applied");
        std::error_code ec;
        std::filesystem::remove(tempDiffPath, ec);
        res.success = false;
        return res;
    }

    int added = 0, updated = 0, deleted = 0;
    if (!applyDiffToGames(gamesCopy, diffJson, langKey, added, updated, deleted)) {
        util::logLine("catalog_updater: applyDiffToGames failed");
        std::error_code ec;
        std::filesystem::remove(tempDiffPath, ec);
        res.success = false;
        return res;
    }

    // Save updated catalog to binary cache
    std::string tempBin = getCatalogBinPath() + ".tmp";
    if (!saveGamesToBinaryFile(tempBin, gamesCopy)) {
        util::logLine("catalog_updater: failed to save updated binary cache");
        std::error_code ec;
        std::filesystem::remove(tempBin, ec);
        std::filesystem::remove(tempDiffPath, ec);
        res.success = false;
        return res;
    }

    std::error_code ec;
    std::filesystem::remove(getCatalogBinPath(), ec);
    std::filesystem::rename(tempBin, getCatalogBinPath(), ec);
    if (ec) {
        util::logLine("catalog_updater: rename bin failed: " + ec.message());
        std::filesystem::remove(tempDiffPath, ec);
        res.success = false;
        return res;
    }

    if (!dl_res.etag.empty()) {
        writeTextFile(getCatalogDiffEtagPath(), dl_res.etag);
    }
    std::filesystem::remove(tempDiffPath, ec);

    // Update in-memory snapshot and refresh views
    auto newSnapshot = std::make_shared<const std::vector<Game>>(std::move(gamesCopy));
    brls::sync([newSnapshot]() {
        setCatalogSnapshot(newSnapshot);
        auto snap = getCatalogSnapshot();
        catalog::FavoritesManager::instance().syncLegacyFavorites(*snap);
        if (ui::g_activeCatalogView) {
            ui::g_activeCatalogView->filterCatalog();
        }
    });

    auto& cfg = config::ConfigManager::instance();
    cfg.setLastCatalogDiffTime(std::time(nullptr));
    cfg.save();

    res.success = true;
    res.added_count = added;
    res.updated_count = updated;
    res.deleted_count = deleted;
    return res;
}

CatalogUpdateResult CatalogUpdater::performFullUpdate(const std::string& catalogUrl,
                                                      const std::shared_ptr<bool>& aliveToken,
                                                      ui::CatalogProgressNotification* notif) {
    CatalogUpdateResult res;
    res.was_full = true;

    auto initialCatalog = getCatalogSnapshot();
    bool was_empty = !initialCatalog || initialCatalog->empty();

    util::logLine("catalog_updater: starting full catalog download from " + catalogUrl);
    net::HttpClient http;
    std::string tempJsonPath = getCatalogPath() + ".tmp";
    auto lastProgressUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());

    http.setProgressCallback([notif, aliveToken, lastProgressUpdate](int64_t dltotal, int64_t dlnow) {
        if (g_appExiting.load()) return;
        if (!aliveToken || !*aliveToken || !notif) return;
        if (dltotal <= 0) return;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastProgressUpdate).count();
        if (elapsed < 150 && dlnow < dltotal) {
            return;
        }
        *lastProgressUpdate = now;

        float percent = (static_cast<float>(dlnow) * 75.0f) / static_cast<float>(dltotal);
        float dlMB = static_cast<float>(dlnow) / (1024.0f * 1024.0f);
        float totMB = static_cast<float>(dltotal) / (1024.0f * 1024.0f);
        char bufDl[32], bufTot[32], bufPct[32];
        std::snprintf(bufDl, sizeof(bufDl), "%.1f", dlMB);
        std::snprintf(bufTot, sizeof(bufTot), "%.1f", totMB);
        std::snprintf(bufPct, sizeof(bufPct), "%.0f", (static_cast<float>(dlnow) * 100.0f) / static_cast<float>(dltotal));
        std::string status = brls::getStr("app/catalog/downloading_progress", std::string(bufDl), std::string(bufTot), std::string(bufPct));

        brls::sync([notif, aliveToken, percent, status]() {
            if (aliveToken && *aliveToken && notif) {
                notif->updateProgress(percent, status);
            }
        });
    });

    std::string savedEtag;
    if (!was_empty && std::filesystem::exists(getCatalogBinPath())) {
        savedEtag = readTextFile(getCatalogEtagPath());
        while (!savedEtag.empty() && (savedEtag.back() == '\r' || savedEtag.back() == '\n' || savedEtag.back() == ' '))
            savedEtag.pop_back();
    }

    std::vector<std::string> extra_headers;
    if (!savedEtag.empty()) {
        extra_headers.push_back("If-None-Match: " + savedEtag);
        util::logLine("catalog_updater: checking full catalog with ETag: " + savedEtag);
    }

    auto dl_res = http.downloadToFileEx(catalogUrl, tempJsonPath, extra_headers, &g_appExiting, 180);

    if (dl_res.not_modified && !g_appExiting.load()) {
        util::logLine("catalog_updater: full catalog HTTP 304 Not Modified");
        res.success = true;
        res.already_up_to_date = true;
        auto& cfg = config::ConfigManager::instance();
        std::time_t now = std::time(nullptr);
        cfg.setLastCatalogFullTime(now);
        cfg.setLastCatalogDiffTime(now);
        cfg.setLastCatalogUpdateDate(config::ConfigManager::currentDateString());
        cfg.save();
        return res;
    }

    std::vector<Game> online_games;
    bool updated = false;

    if (dl_res.success && !g_appExiting.load()) {
        if (aliveToken && *aliveToken && notif) {
            brls::sync([notif, aliveToken]() {
                if (aliveToken && *aliveToken && notif) {
                    notif->updateProgress(85.0f, "app/catalog/processing"_i18n);
                }
            });
        }

        online_games = parseGamesFromFileStream(tempJsonPath);
        if (!online_games.empty() && !g_appExiting.load()) {
            util::logLine("catalog_updater: parsed " + std::to_string(online_games.size()) + " games from stream");

            std::string tempBinPath = getCatalogBinPath() + ".tmp";
            saveGamesToBinaryFile(tempBinPath, online_games);

            std::error_code ec;
            bool jsonOk = false;
            bool binOk = false;

            std::filesystem::remove(getCatalogBinPath(), ec);
            std::filesystem::rename(tempBinPath, getCatalogBinPath(), ec);
            binOk = !ec;

            std::filesystem::remove(getCatalogPath(), ec);
            std::filesystem::rename(tempJsonPath, getCatalogPath(), ec);
            jsonOk = !ec;

            updated = jsonOk && binOk;
            if (updated && !dl_res.etag.empty()) {
                writeTextFile(getCatalogEtagPath(), dl_res.etag);
            }
        } else {
            std::error_code ec;
            std::filesystem::remove(tempJsonPath, ec);
        }
    }

    // Fallback parser if online fetch failed and catalog was completely empty
    if (was_empty && !updated && !g_appExiting.load()) {
        util::logLine("catalog_updater: running fallback sources parser");
        catalog::CatalogManager catalog_mgr;
        bool sources_loaded = catalog_mgr.loadSourcesWithFallback(TSNX_SOURCES_PATH, "romfs:/sources.json");
        if (sources_loaded) {
            catalog_mgr.updateCatalogs();
            catalog_mgr.mergeCatalogEntries();

            for (const auto& entry : catalog_mgr.entries()) {
                Game g;
                g.title = entry.title;
                g.size = entry.size;
                g.magnet = entry.magnet;
                g.description = entry.description;
                g.cover = entry.icon;
                g.topic_id = extractBtihHashLocal(entry.magnet);
                online_games.push_back(g);
            }

            if (!online_games.empty()) {
                nlohmann::json jg = online_games;
                writeTextFile(getCatalogPath(), jg.dump(2));
                saveGamesToBinaryFile(getCatalogBinPath(), online_games);
                updated = true;
            }
        }
    }

    if (updated && !online_games.empty()) {
        auto& cfg = config::ConfigManager::instance();
        std::time_t now = std::time(nullptr);
        cfg.setLastCatalogFullTime(now);
        cfg.setLastCatalogDiffTime(now);
        cfg.setLastCatalogUpdateDate(config::ConfigManager::currentDateString());
        cfg.save();

        auto newSnapshot = std::make_shared<const std::vector<Game>>(std::move(online_games));
        brls::sync([newSnapshot]() {
            setCatalogSnapshot(newSnapshot);
            auto snap = getCatalogSnapshot();
            catalog::FavoritesManager::instance().syncLegacyFavorites(*snap);
            if (ui::g_activeCatalogView) {
                ui::g_activeCatalogView->filterCatalog();
            }
        });

        res.success = true;
        return res;
    }

    res.success = false;
    return res;
}

} // namespace catalog
