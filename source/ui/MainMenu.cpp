#include "MainMenu.hpp"
#include "DownloadUiManager.hpp"
#include "CatalogView.hpp"
#include "FavoritesView.hpp"
#include "DownloadsView.hpp"
#include "SettingsTab.hpp"
#include "../GameData.hpp"
#include "../config/config.h"
#include "../utils/log.h"

extern std::vector<Game> g_games;

MainMenu::MainMenu() {
    util::logLine("MainMenu: constructor start");
    brls::Application::setCommonFooter("TorrentShopNX v1.0.0 | Игр в каталоге: " + std::to_string(g_games.size()));
    util::logLine("MainMenu: constructor end");
}

void MainMenu::onContentAvailable() {
    util::logLine("MainMenu: onContentAvailable start");
    
    // Register button click actions
    btnCatalog->registerClickAction([](brls::View* view) {
        brls::Application::pushActivity(new ui::CatalogView());
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
        ui::DownloadManager::instance().getImpl().trackProgress();
        ui::DownloadManager::instance().triggerCallback();
        updateDownloadsBadge(ui::DownloadManager::instance().getActiveDownloadsCount());
    });
    timer->start();

    updateDownloadsBadge(ui::DownloadManager::instance().getActiveDownloadsCount());
}

void MainMenu::updateDownloadsBadge(int count) {
    if (btnDownloads) {
        if (count > 0) {
            btnDownloads->setText("Загрузки (" + std::to_string(count) + ")");
        } else {
            btnDownloads->setText("Загрузки");
        }
    }
}

