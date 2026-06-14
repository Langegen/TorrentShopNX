#include <switch.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "catalog/catalog_manager.h"
#include "catalog/favorites_manager.h"
#include "download/download_manager.h"
#include "datasource/data_source_manager.h"
#include "net/http_client.h"
#include "ui/menu.h"
#include "ui/browser.h"
#include "ui/console_font.h"
#include "ui/details.h"
#include "ui/downloads.h"
#include "ui/input.h"
#include "utils/log.h"
#include "config/config.h"

extern "C" {
    u32 __nx_socket_mem_size = 0x10000000; // 256MB socket memory pool for 300+ peers
    size_t __nx_socket_tcp_tx_buf_size = 0x100000; // 1MB
    size_t __nx_socket_tcp_rx_buf_size = 0x100000; // 1MB
}

static const char* kCatalogPath = "sdmc:/switch/TorrentShopNX/switch_games.json";

static void setKeepAwake(bool enabled) {
#ifdef __SWITCH__
    appletSetMediaPlaybackState(enabled);
#else
    (void)enabled;
#endif
}

static std::string showKeyboard(const char* hint) {
    SwkbdConfig kbd;
    swkbdCreate(&kbd, 0);
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, hint);
    char out[256] = {0};
    Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return "";
    return std::string(out);
}

static bool writeTextFile(const std::string& path, const std::string& body) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(body.data(), body.size());
    return true;
}

static bool refreshCatalog(catalog::CatalogManager& catalog_mgr) {
    if (catalog_mgr.loadCatalogFromFile(kCatalogPath)) {
        util::logLine(std::string("catalog: loaded from switch_games.json, entries=") +
                      std::to_string((int)catalog_mgr.entries().size()));
        return true;
    }

    util::logLine("catalog: failed to load switch_games.json");
    return false;
}

static bool updateCatalogFromUrlIfNeeded(config::ConfigManager& cfg) {
    const std::string& url = cfg.getCatalogSourceUrl();
    if (url.empty()) return false;
    if (!cfg.shouldUpdateCatalogToday()) return false;

    util::logLine("catalog: daily update started from " + url);

    net::HttpClient http;
    auto res = http.httpGet(url);
    if (res.status_code != 200 || res.body.empty()) {
        util::logLine("catalog: daily update failed, status=" + std::to_string(res.status_code));
        return false;
    }

    if (!writeTextFile(kCatalogPath, res.body)) {
        util::logLine("catalog: daily update failed to write file");
        return false;
    }

    cfg.setLastCatalogUpdateDate(config::ConfigManager::currentDateString());
    util::logLine("catalog: daily update completed");
    return true;
}

static void showDataSourceSettings(download::DownloadManager& dm) {
    auto& ds = dm.dataSourceManager();
    auto& cfg = config::ConfigManager::instance();
    bool running = true;

    while (running && appletMainLoop()) {
        dm.trackProgress();
        consoleClear();
        printf("=== Download Source ===\n\n");
        printf("Current Mode: %s\n\n", ds.modeDescription().c_str());
        printf("Remote URL: %s\n", ds.remoteUrl().c_str());
        printf("Applet Mode: %s\n", ds.isAppletMode() ? "YES (RAM restricted!)" : "No");
        printf("Local Client: %s\n", ds.isLocalServerAvailable() ? "Available" : "Unavailable");
        printf("RAM for Local: %s\n\n", ds.hasEnoughMemoryForLocal() ? "Enough" : "NOT ENOUGH");

        if (ds.isAppletMode()) {
            printf("[!] Launch via title override (hold R on any game)\n");
            printf("[!] to access the local server mode.\n\n");
        }

        printf("Up: TorrServer mode\n");
        printf("Down: Local client mode\n");
        printf("Right: Change URL\n");
        printf("\nB - Back\n");

        consoleUpdate(NULL);

        u64 keys = inputDown();
        u64 nav = keys | inputRepeat(HidNpadButton_Up | HidNpadButton_Down);
        if (keys & HidNpadButton_B) {
            running = false;
        }

        if (nav & HidNpadButton_Up) {
            ds.setMode(datasource::DataSourceMode::Remote);
            cfg.setDataMode("torrserver");
        } else if (nav & HidNpadButton_Down) {
            ds.setMode(datasource::DataSourceMode::LocalClient);
            cfg.setDataMode("local_client");
        } else if (keys & HidNpadButton_Right) {
            std::string url = showKeyboard("Remote TorrServer URL (http://IP:8090)");
            if (!url.empty()) {
                cfg.setTorrServerUrl(url);
                ds.setRemoteUrl(url);
            }
        }

        consoleUpdate(NULL);
    }
}

static void showCatalogSourceSettings(download::DownloadManager& dm, catalog::CatalogManager& catalog_mgr) {
    auto& cfg = config::ConfigManager::instance();
    bool running = true;

    while (running && appletMainLoop()) {
        dm.trackProgress();
        consoleClear();
        printf("=== Catalog Source ===\n\n");
        printf("Catalog file: %s\n\n", kCatalogPath);
        printf("Source URL: %s\n", cfg.getCatalogSourceUrl().empty() ? "(not set)" : cfg.getCatalogSourceUrl().c_str());
        printf("Last update: %s\n\n", cfg.getLastCatalogUpdateDate().empty() ? "(never)" : cfg.getLastCatalogUpdateDate().c_str());

        printf("A: Set source URL\n");
        printf("X: Force update now\n");
        printf("Y: Reload local catalog\n");
        printf("B: Back\n");

        consoleUpdate(NULL);

        u64 keys = inputDown();
        if (keys & HidNpadButton_B) {
            running = false;
        } else if (keys & HidNpadButton_A) {
            std::string url = showKeyboard("Catalog JSON URL (switch_games.json)");
            if (!url.empty()) {
                cfg.setCatalogSourceUrl(url);
            }
        } else if (keys & HidNpadButton_X) {
            cfg.setLastCatalogUpdateDate("");
            updateCatalogFromUrlIfNeeded(cfg);
            refreshCatalog(catalog_mgr);
        } else if (keys & HidNpadButton_Y) {
            refreshCatalog(catalog_mgr);
        }
    }
}

static void showSettings(download::DownloadManager& dm, catalog::CatalogManager& catalog_mgr) {
    bool running = true;
    int selected = 0;

    while (running && appletMainLoop()) {
        dm.trackProgress();
        auto& cfg = config::ConfigManager::instance();
        std::vector<std::string> options = {
            "Download Source (Mode / URL)",
            "Catalog Source URL",
            std::string("Prevent Sleep While Downloading: ") +
                (cfg.getKeepAwakeDuringDownloads() ? "ON" : "OFF"),
            "Back"
        };
        consoleClear();
        printf("=== Settings ===\n\n");

        for (size_t i = 0; i < options.size(); ++i) {
            if (static_cast<int>(i) == selected) printf(" > %s\n", options[i].c_str());
            else printf("   %s\n", options[i].c_str());
        }

        printf("\nA: Select/Toggle  B: Back\n");
        consoleUpdate(NULL);

        u64 keys = inputDown();
        u64 nav = keys | inputRepeat(HidNpadButton_Up | HidNpadButton_Down);
        if (keys & HidNpadButton_B) {
            running = false;
        } else if (nav & HidNpadButton_Down) {
            selected = (selected + 1) % static_cast<int>(options.size());
        } else if (nav & HidNpadButton_Up) {
            selected = (selected - 1 + static_cast<int>(options.size())) % static_cast<int>(options.size());
        } else if (keys & HidNpadButton_A) {
            if (selected == 0) {
                showDataSourceSettings(dm);
            } else if (selected == 1) {
                showCatalogSourceSettings(dm, catalog_mgr);
            } else if (selected == 2) {
                cfg.setKeepAwakeDuringDownloads(!cfg.getKeepAwakeDuringDownloads());
            } else {
                running = false;
            }
        }
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    util::logInit();
    util::logLine("main: start");

    consoleInit(NULL);
    util::logLine("main: consoleInit ok");

    Result romfs_rc = romfsInit();
    bool romfs_ok = R_SUCCEEDED(romfs_rc);
    util::logLine(std::string("main: romfsInit rc=") + std::to_string((int)romfs_rc));
    if (romfs_ok) {
        ui::installConsoleFont();
    }

    // Increase BSD socket memory pool to allow many peers with large buffers.
    const SocketInitConfig* defaults = socketGetDefaultInitConfig();
    SocketInitConfig sock_cfg = *defaults;
    sock_cfg.num_bsd_sessions  = 8;
    sock_cfg.sb_efficiency = 32; // from nxTransmission, greatly improves socket buffer allocation on Switch
    sock_cfg.tcp_tx_buf_max_size = 1048576; // 1MB TX
    sock_cfg.tcp_rx_buf_max_size = 1048576; // 1MB RX
    sock_cfg.udp_rx_buf_size = 1048576;      // 1MB UDP RX
    sock_cfg.udp_tx_buf_size = 1048576;      // 1MB UDP TX
    util::logLine("main: socket config sb_efficiency=" + std::to_string(sock_cfg.sb_efficiency) +
                  " num_bsd_sessions=" + std::to_string(sock_cfg.num_bsd_sessions) +
                  " tcp_tx=" + std::to_string(sock_cfg.tcp_tx_buf_size) +
                  " tcp_rx=" + std::to_string(sock_cfg.tcp_rx_buf_size) +
                  " tcp_tx_max=" + std::to_string(sock_cfg.tcp_tx_buf_max_size) +
                  " tcp_rx_max=" + std::to_string(sock_cfg.tcp_rx_buf_max_size) +
                  " udp_rx=" + std::to_string(sock_cfg.udp_rx_buf_size) +
                  " udp_tx=" + std::to_string(sock_cfg.udp_tx_buf_size));
    Result sock_rc = socketInitialize(&sock_cfg);
    util::logLine(std::string("main: socketInitialize rc=") + std::to_string((int)sock_rc));

    inputInit();
    util::logLine("main: inputInit ok");
    setKeepAwake(false);

    AppletType applet_type = appletGetAppletType();
    if (applet_type == AppletType_LibraryApplet || applet_type == AppletType_OverlayApplet) {
        util::logLine("main: warning - applet mode, local TorrServer is unavailable");
    } else {
        util::logLine("main: full application mode");
    }

    catalog::CatalogManager catalog_mgr;
    download::DownloadManager download_mgr;

    auto& cfg = config::ConfigManager::instance();
    download_mgr.dataSourceManager().setRemoteUrl(cfg.getTorrServerUrl());
    if (cfg.getDataMode() == "local_client") {
        download_mgr.dataSourceManager().setMode(datasource::DataSourceMode::LocalClient);
    } else {
        download_mgr.dataSourceManager().setMode(datasource::DataSourceMode::Remote);
    }

    catalog::FavoritesManager::instance().init("sdmc:/switch/TorrentShopNX/favorites.json");

    updateCatalogFromUrlIfNeeded(cfg);

    bool catalog_loaded = refreshCatalog(catalog_mgr);
    if (!catalog_loaded) {
        bool sources_loaded = catalog_mgr.loadSourcesWithFallback("sdmc:/switch/TorrentShopNX/sources.json", "romfs:/sources.json");
        util::logLine(std::string("main: fallback sources_loaded=") + (sources_loaded ? "true" : "false"));
        if (sources_loaded) {
            catalog_mgr.updateCatalogs();
            catalog_mgr.mergeCatalogEntries();
        }
    }
    util::logLine(std::string("main: entries=") + std::to_string((int)catalog_mgr.entries().size()));

    util::logLine("main: entering loop");
    while (appletMainLoop()) {
        auto choice = ui::showMainMenu(&download_mgr);
        if (choice == ui::MainMenuChoice::Browse) {
            ui::showCatalogBrowser(catalog_mgr.entries(), download_mgr);
        } else if (choice == ui::MainMenuChoice::Search) {
            std::string q = showKeyboard("Search catalog");
            auto results = catalog_mgr.searchCatalog(q);
            ui::showCatalogBrowser(results, download_mgr);
        } else if (choice == ui::MainMenuChoice::Favorites) {
            ui::showCatalogBrowser(catalog::FavoritesManager::instance().getFavorites(), download_mgr);
        } else if (choice == ui::MainMenuChoice::Downloads) {
            ui::showDownloads(download_mgr);
        } else if (choice == ui::MainMenuChoice::Sources) {
            consoleClear();
            printf("Sources\n\n");
            for (const auto& s : catalog_mgr.sources()) {
                printf("%s | %s | %s\n", s.name.c_str(), s.type.c_str(), s.url.c_str());
            }
            printf("\nPress B to return.\n");
            while (appletMainLoop()) {
                download_mgr.trackProgress();
                if (inputDown() & HidNpadButton_B) break;
                consoleUpdate(NULL);
            }
        } else if (choice == ui::MainMenuChoice::Settings) {
            showSettings(download_mgr, catalog_mgr);
        } else if (choice == ui::MainMenuChoice::Exit) {
            break;
        }
    }

    setKeepAwake(false);
    socketExit();
    if (romfs_ok) romfsExit();
    consoleExit(NULL);
    return 0;
}

