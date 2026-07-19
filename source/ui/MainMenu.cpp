#include "MainMenu.hpp"
#include "DownloadUiManager.hpp"
#include "CatalogView.hpp"
#include "FavoritesView.hpp"
#include "DownloadsView.hpp"
#include "SettingsTab.hpp"
#include "RemoteAddView.hpp"
#include "../GameData.hpp"
#include "../config/config.h"
#include "../utils/log.h"
#include "../catalog/catalog_manager.h"
#include "../net/http_client.h"
#include <borealis/extern/nlohmann/json.hpp>

extern std::vector<Game> g_games;

MainMenu::MainMenu() {
    util::logLine("MainMenu: constructor start");
    brls::Application::setCommonFooter("TorrentShopNX v2.0 | Игр в каталоге: " + std::to_string(g_games.size()));
    util::logLine("MainMenu: constructor end");
}

void MainMenu::onContentAvailable() {
    util::logLine("MainMenu: onContentAvailable start");
    
    // Register button click actions
    btnCatalog->registerClickAction([](brls::View* view) {
        brls::Application::pushActivity(new ui::CatalogView());
        return true;
    });

    btnRemoteAdd->registerClickAction([](brls::View* view) {
        brls::Application::pushActivity(new ui::RemoteAddView());
        return true;
    });

    btnFavorites->registerClickAction([](brls::View* view) {
        brls::Application::pushActivity(new ui::FavoritesView());
        return true;
    });

    btnDownloads->registerClickAction([](brls::View* view) {
        brls::Application::pushActivity(new ui::DownloadsView());
        return true;
    });

    btnSettings->registerClickAction([](brls::View* view) {
        brls::Application::pushActivity(new ui::SettingsTab());
        return true;
    });

    brls::RepeatingTimer* timer = new brls::RepeatingTimer();
    timer->setPeriod(1000); 
    timer->setCallback([this]() {
        updateDownloadsBadge(ui::DownloadManager::instance().getActiveDownloadsCount());
    });
    timer->start();

    updateDownloadsBadge(ui::DownloadManager::instance().getActiveDownloadsCount());

    // Trigger asynchronous catalog update using brls::async with safety flags
    auto& cfg = config::ConfigManager::instance();
    std::string catalog_url = cfg.getCatalogSourceUrl();
    bool should_update = cfg.shouldUpdateCatalogToday();
    bool was_empty = g_games.empty();

    if ((!catalog_url.empty() && should_update) || was_empty) {
        g_catalogUpdateRunning = true;
        brls::async([catalog_url, should_update, was_empty]() {
            bool updated = false;

            // 1. Check for daily updates
            if (!catalog_url.empty() && should_update) {
                util::logLine("catalog: background daily update started from " + catalog_url);
                net::HttpClient http;
                auto res = http.httpGet(catalog_url);
                if (res.status_code == 200 && !res.body.empty()) {
                    if (writeTextFile("sdmc:/switch/TorrentShopNX/switch_games.json", res.body)) {
                        util::logLine("catalog: background daily update completed");
                        updated = true;
                    }
                } else {
                    util::logLine("catalog: background daily update failed, status=" + std::to_string(res.status_code));
                }
            }

            // 2. Fallback parser if JSON games catalog doesn't exist and update didn't succeed
            if (was_empty && !updated) {
                util::logLine("catalog: running background fallback sources parser");
                catalog::CatalogManager catalog_mgr;
                bool sources_loaded = catalog_mgr.loadSourcesWithFallback("sdmc:/switch/TorrentShopNX/sources.json", "romfs:/sources.json");
                if (sources_loaded) {
                    catalog_mgr.updateCatalogs();
                    catalog_mgr.mergeCatalogEntries();
                    
                    std::vector<Game> fallback_games;
                    for (const auto& entry : catalog_mgr.entries()) {
                        Game g;
                        g.title = entry.title;
                        g.size = entry.size;
                        g.magnet = entry.magnet;
                        g.description = entry.description;
                        g.cover = entry.icon;
                        g.topic_id = extractBtihHashLocal(entry.magnet);
                        fallback_games.push_back(g);
                    }

                    if (!fallback_games.empty()) {
                        // Cache the fallback games to switch_games.json file
                        std::ofstream out("sdmc:/switch/TorrentShopNX/switch_games.json", std::ios::binary);
                        if (out.is_open()) {
                            try {
                                nlohmann::json j = fallback_games;
                                std::string dumped = j.dump(4);
                                out.write(dumped.data(), dumped.size());
                                util::logLine("catalog: wrote fallback games to switch_games.json cache");
                            } catch (...) {}
                        }

                        brls::sync([fallback_games]() {
                            g_games = fallback_games;
                            brls::Application::setCommonFooter("TorrentShopNX v2.0 | Игр в каталоге: " + std::to_string(g_games.size()));
                            if (ui::g_activeCatalogView) {
                                ui::g_activeCatalogView->filterCatalog();
                            }
                        });
                    }
                }
            } else if (updated) {
                // Reload games from updated file in main thread
                brls::sync([]() {
                    g_games = loadGamesFromFile("sdmc:/switch/TorrentShopNX/switch_games.json");
                    brls::Application::setCommonFooter("TorrentShopNX v2.0 | Игр в каталоге: " + std::to_string(g_games.size()));
                    
                    // Write new update date in config on main thread
                    auto& main_cfg = config::ConfigManager::instance();
                    main_cfg.setLastCatalogUpdateDate(config::ConfigManager::currentDateString());
                    main_cfg.save();

                    if (ui::g_activeCatalogView) {
                        ui::g_activeCatalogView->filterCatalog();
                    }
                });
            }
            g_catalogUpdateRunning = false;
        });
    }

    // Delete renamed cache folders iteratively using brls::async to support clean cancellation on exit
    if (!g_pathsToDelete.empty()) {
        g_cleanupRunning = true;
        brls::async([]() {
            for (const auto& path : g_pathsToDelete) {
                if (g_cleanupCancelled.load()) break;
                deleteDirectoryIterative(path);
            }
            g_pathsToDelete.clear();
            g_cleanupRunning = false;
        });
    }
}

void MainMenu::updateDownloadsBadge(int count) {
    if (lblDownloads) {
        if (count > 0) {
            lblDownloads->setText("Загрузки (" + std::to_string(count) + ")");
        } else {
            lblDownloads->setText("Загрузки");
        }
    }
}

