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
    remoteUrlCell->init("Адрес TorrServer", cfg.getTorrServerUrl(), [&cfg, &dm](std::string value) {
        if (!value.empty()) {
            cfg.setTorrServerUrl(value);
            dm.dataSourceManager().setRemoteUrl(value);
            cfg.save();
            brls::Application::notify("Адрес TorrServer обновлен");
        }
    }, "http://127.0.0.1:8090", "Введите адрес удаленного TorrServer");

    // 4. Catalog JSON URL
    catalogUrlCell->init("URL каталога JSON", cfg.getCatalogSourceUrl(), [&cfg](std::string value) {
        if (!value.empty()) {
            cfg.setCatalogSourceUrl(value);
            cfg.save();
            brls::Application::notify("Адрес каталога обновлен");
        }
    }, "http://example.com/games.json", "Введите адрес JSON файла каталога игр");
}



} // namespace ui
