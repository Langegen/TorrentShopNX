#include "SettingsTab.hpp"
#include "DownloadUiManager.hpp"
#include "../config/config.h"

namespace ui {

SettingsTab::SettingsTab() {
}

void SettingsTab::onContentAvailable() {

    auto& cfg = config::ConfigManager::instance();
    auto& dm = ui::DownloadManager::instance().getImpl();

    // 1. Prevent sleep
    keepAwakeCell->init("Предотвращать сон при скачивании", cfg.getKeepAwakeDuringDownloads(), [&cfg](bool value) {
        cfg.setKeepAwakeDuringDownloads(value);
        cfg.save();
    });

    // 2. Download mode
    std::vector<std::string> modes = { "Удаленный TorrServer", "Локальный клиент (libtorrent)" };
    int initialMode = (cfg.getDataMode() == "local_client") ? 1 : 0;
    
    modeCell->init("Режим работы", modes, initialMode, [](int selected) {}, [&cfg, &dm](int selected) {
        if (selected == 1) {
            cfg.setDataMode("local_client");
            dm.dataSourceManager().setMode(datasource::DataSourceMode::LocalClient);
        } else {
            cfg.setDataMode("torrserver");
            dm.dataSourceManager().setMode(datasource::DataSourceMode::Remote);
        }
        cfg.save();
        brls::Application::notify("Режим загрузки изменен");
    });

    // 3. Remote TorrServer URL
    remoteUrlCell->setText("Адрес TorrServer");
    auto updateRemoteUrlDisplay = [this, &cfg]() {
        std::string url = cfg.getTorrServerUrl();
        if (url.length() > 35) {
            url = url.substr(0, 32) + "...";
        } else if (url.empty()) {
            url = "http://127.0.0.1:8090";
        }
        remoteUrlCell->setDetailText(url);
    };
    updateRemoteUrlDisplay();
    remoteUrlCell->registerClickAction([this, updateRemoteUrlDisplay, &cfg, &dm](brls::View* view) {
        brls::Application::getImeManager()->openForText(
            [updateRemoteUrlDisplay, &cfg, &dm](std::string text) {
                if (!text.empty()) {
                    cfg.setTorrServerUrl(text);
                    dm.dataSourceManager().setRemoteUrl(text);
                    cfg.save();
                    brls::Application::notify("Адрес TorrServer обновлен");
                    updateRemoteUrlDisplay();
                }
            },
            "Адрес TorrServer",
            "Введите адрес удаленного TorrServer",
            255,
            cfg.getTorrServerUrl(),
            0
        );
        return true;
    });

    // 4. Catalog JSON URL
    catalogUrlCell->setText("URL каталога JSON");
    auto updateCatalogUrlDisplay = [this, &cfg]() {
        std::string url = cfg.getCatalogSourceUrl();
        if (url.length() > 35) {
            url = url.substr(0, 32) + "...";
        } else if (url.empty()) {
            url = "http://example.com/games.json";
        }
        catalogUrlCell->setDetailText(url);
    };
    updateCatalogUrlDisplay();
    catalogUrlCell->registerClickAction([this, updateCatalogUrlDisplay, &cfg](brls::View* view) {
        brls::Application::getImeManager()->openForText(
            [updateCatalogUrlDisplay, &cfg](std::string text) {
                if (!text.empty()) {
                    cfg.setCatalogSourceUrl(text);
                    cfg.save();
                    brls::Application::notify("Адрес каталога обновлен");
                    updateCatalogUrlDisplay();
                }
            },
            "URL каталога JSON",
            "Введите адрес JSON файла каталога игр",
            255,
            cfg.getCatalogSourceUrl(),
            0
        );
        return true;
    });
}

void SettingsTab::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (resetState) {
        brls::Application::giveFocus(this->keepAwakeCell);
    }
}

void SettingsTab::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    brls::Application::giveFocus(nullptr);
}

} // namespace ui
