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
    brls::Application::setCommonFooter("TorrentShopNX v2.1 | Игр в каталоге: " + std::to_string(g_games.size()));
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
    std::string catalog_url = cfg.getEffectiveCatalogSourceUrl();
    bool should_update = cfg.shouldUpdateCatalogToday();
    bool was_empty = g_games.empty();

    if (should_update || was_empty) {
        g_catalogUpdateRunning = true;
        brls::async([catalog_url, should_update, was_empty]() {
            bool updated = false;
            std::vector<Game> online_games;

            // 1. Fetch catalog online directly in memory
            util::logLine("catalog: background online update started from " + catalog_url);
            net::HttpClient http;
            auto res = http.httpGet(catalog_url);
            if (res.status_code == 200 && !res.body.empty()) {
                online_games = parseGamesFromJsonString(res.body);
                if (!online_games.empty()) {
                    util::logLine("catalog: background online update parsed " + std::to_string(online_games.size()) + " games directly in memory");
                    updated = true;
                }
            } else {
                util::logLine("catalog: background online update failed, status=" + std::to_string(res.status_code));
            }

            // 2. Fallback parser if online fetch failed and catalog was empty
            if (was_empty && !updated) {
                util::logLine("catalog: running background fallback sources parser");
                catalog::CatalogManager catalog_mgr;
                bool sources_loaded = catalog_mgr.loadSourcesWithFallback("sdmc:/switch/TorrentShopNX/sources.json", "romfs:/sources.json");
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
                        updated = true;
                    }
                }
            }

            if (updated && !online_games.empty()) {
                // Update games directly in memory on main thread without downloading to disk file
                brls::sync([online_games]() {
                    g_games = online_games;
                    brls::Application::setCommonFooter("TorrentShopNX v2.1 | Игр в каталоге: " + std::to_string(g_games.size()));
                    
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

    // Trigger asynchronous app update check if enabled and due
    if (cfg.getAutoAppUpdate() && cfg.shouldCheckAppUpdateToday()) {
        std::string app_update_url = cfg.getEffectiveAppUpdateUrl();
        brls::async([app_update_url]() {
            util::logLine("main_menu: background app update check started from " + app_update_url);
            net::HttpClient http;
            auto res = http.httpGet(app_update_url);
            if (res.status_code == 200 && !res.body.empty()) {
                try {
                    auto j = nlohmann::json::parse(res.body);
                    std::string version;
                    std::string url;
                    
                    if (j.contains("tag_name")) {
                        version = j.value("tag_name", "");
                        if (j.contains("assets") && j["assets"].is_array()) {
                            for (const auto& asset : j["assets"]) {
                                std::string assetName = asset.value("name", "");
                                if (assetName.size() >= 4 && assetName.rfind(".nro") == assetName.size() - 4) {
                                    url = asset.value("browser_download_url", "");
                                    break;
                                }
                            }
                        }
                    } else {
                        version = j.value("version", "");
                        url = j.value("url", "");
                    }
                    
                    std::string currentVersion = "2.1"; // Current app version
                    std::string cleanVersion = version;
                    if (!cleanVersion.empty() && (cleanVersion[0] == 'v' || cleanVersion[0] == 'V')) cleanVersion = cleanVersion.substr(1);
                    std::string cleanCurrent = currentVersion;
                    if (!cleanCurrent.empty() && (cleanCurrent[0] == 'v' || cleanCurrent[0] == 'V')) cleanCurrent = cleanCurrent.substr(1);
                    
                    if (!version.empty() && !url.empty() && cleanVersion != cleanCurrent) {
                        brls::sync([url, version]() {
                            std::string msg = "Доступна новая версия: " + version + "\n\nХотите скачать и установить обновление?";
                            brls::Dialog* dialog = new brls::Dialog(msg);
                            dialog->addButton("Да", [url, version, dialog]() {
                                dialog->close([url, version]() {
                                    ui::downloadAndInstallAppUpdate(url, version);
                                });
                            });
                            dialog->addButton("Нет", [dialog]() { dialog->close(); });
                            dialog->open();
                        });
                    }
                } catch (const std::exception& e) {
                    util::logLine("main_menu: background app update parse error: " + std::string(e.what()));
                }
            } else {
                util::logLine("main_menu: background app update failed, status=" + std::to_string(res.status_code));
            }
            
            brls::sync([]() {
                auto& main_cfg = config::ConfigManager::instance();
                main_cfg.setLastAppUpdateCheckDate(config::ConfigManager::currentDateString());
            });
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

