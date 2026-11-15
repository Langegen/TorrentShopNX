// PC window with the homebrew store screens (HomebrewView, HomebrewDetailView)
// and nothing else of the app: no torrent engine, no libarchive. Build:
//   cmake --build build_pc --target homebrew_ui
// Run from build_pc (resources/ is next to the exe):
//   HB_UI_DATA=<folder with repo.json, homebrew.json, icons/, manifests/>
//   HB_UI_LANG=ru|en-US|de|...   HB_UI_THEME=light|dark   HB_UI_OPEN=<package name>
// The window is 1280x720, the Switch screen.

#include <borealis.hpp>

#include <atomic>
#include <cstdlib>
#include <string>

#include "config/config.h"
#include "catalog/homebrew_catalog.hpp"
#include "ui/ThemeManager.hpp"
#include "ui/homebrew/HomebrewDetailView.hpp"
#include "ui/homebrew/HomebrewView.hpp"

std::atomic<bool> g_appExiting{false};

int main(int, char**) {
    // Borealis locales are plain strings: "ru", "en-US", "zh-Hans"...
    const char* lang = std::getenv("HB_UI_LANG");
    brls::Platform::APP_LOCALE_DEFAULT = lang ? lang : brls::LOCALE_RU;

    if (!brls::Application::init()) return EXIT_FAILURE;
    brls::Application::createWindow("TorrentShopNX - homebrew store (PC)");

    // App themes: light (default), graphite, emerald, cyberpunk, ruby, amethyst, amber.
    const char* theme = std::getenv("HB_UI_THEME");
    const std::string themeName = theme ? theme : "light";
    config::ConfigManager::instance().setTheme(themeName);
    brls::Application::getPlatform()->setThemeVariant(themeName == "light" ? brls::ThemeVariant::LIGHT
                                                                           : brls::ThemeVariant::DARK);
    ui::ThemeManager::instance().init();
    brls::Application::setGlobalWallpaper(ui::ThemeManager::instance().getEffectiveWallpaperPath());

    brls::Application::pushActivity(new ui::HomebrewView());

    // Straight to one package card, for screenshots.
    if (const char* open = std::getenv("HB_UI_OPEN")) {
        const std::string name = open;
        brls::delay(1500, [name]() {
            homebrew::Package p;
            if (homebrew::Catalog::instance().find(name, p))
                brls::Application::pushActivity(new ui::HomebrewDetailView(p));
        });
    }

    while (brls::Application::mainLoop()) {
    }
    g_appExiting.store(true);
    return EXIT_SUCCESS;
}
