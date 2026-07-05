#include <switch.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <borealis.hpp>

#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#endif

#include "GameData.hpp"
#include "catalog/catalog_manager.h"
#include "ui/MainMenu.hpp"
#include "ui/CatalogView.hpp"
#include "ui/FavoritesView.hpp"
#include "ui/DownloadsView.hpp"
#include "ui/SettingsTab.hpp"
#include "ui/FavoritesManager.hpp"
#include "ui/DownloadUiManager.hpp"
#include "ui/QrCodeView.hpp"
#include "config/config.h"
#include "torrent/torrent_engine.h"
#include "utils/log.h"
#include "net/http_client.h"

// Global list of catalog games
std::vector<Game> g_games;

// Override libnx's socketExit() with a no-op via the --wrap linker flag.
// Rationale: libtorrent's io_service/disk threads are kept alive (we leak the
// session to avoid a blocking join crash on Switch). The normal exit path calls
// __appExit() -> socketExit() -> free(socket_mem_pool). Freeing that pool while
// libtorrent threads still hold socket references crashes the process.
// We use std::quick_exit() in main() which skips C++ static destructors and calls _exit().
extern "C" void __wrap_socketExit(void) {
    util::logLine("__wrap_socketExit: no-op called");
}

extern "C" {
    u32 __nx_socket_mem_size = 0x02000000; // 32MB socket memory pool (saves 224MB RAM!)
    size_t __nx_socket_tcp_tx_buf_size = 0x40000; // 256KB
    size_t __nx_socket_tcp_rx_buf_size = 0x40000; // 256KB

    void userAppInit(void) {
        appletLockExit();

        // Custom network initialization configured for TorrentShopNX
        SocketInitConfig cfg = *(socketGetDefaultInitConfig());
        cfg.num_bsd_sessions  = 12; // Restore to 12 (Switch OS limit)
        cfg.sb_efficiency = 4; // Restored to standard 4 to prevent socket memory exhaustion (32 was taking 16MB per socket!)
        cfg.tcp_tx_buf_max_size = 131072; // 128KB (optimized for 32MB socket pool)
        cfg.tcp_rx_buf_max_size = 131072; // 128KB
        cfg.udp_rx_buf_size = 32768; // 32KB
        cfg.udp_tx_buf_size = 32768; // 32KB
        socketInitialize(&cfg);

        romfsInit();
        hidsysInitialize();
        inssInitialize();
        plInitialize(PlServiceType_User);
        setsysInitialize();
        setInitialize();
        psmInitialize();
        nifmInitialize(NifmServiceType_User);
        lblInitialize();
    }

    void userAppExit(void) {
        lblExit();
        nifmExit();
        psmExit();
        setExit();
        setsysExit();
        plExit();
        inssExit();
        hidsysExit();
        romfsExit();
        socketExit();
        appletUnlockExit();
    }

    void __appExit(void) {
        util::logLine("__appExit: custom no-op called, closing log file");
        util::logClose();
    }
}

static const char* kCatalogPath = "sdmc:/switch/TorrentShopNX/switch_games.json";

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

static std::string extractBtihHashLocal(std::string magnet) {
    std::transform(magnet.begin(), magnet.end(), magnet.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string marker = "xt=urn:btih:";
    size_t pos = magnet.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();
    size_t end = magnet.find('&', pos);
    if (end == std::string::npos) end = magnet.size();
    if (end <= pos) return {};
    return magnet.substr(pos, end - pos);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    util::logInit();
    util::logLine("main: start");
    clearCaches();

    bool romfs_ok = true; // romfs was initialized in userAppInit

    // Initialize Borealis UI
    brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;
    if (!brls::Application::init()) {
        util::logLine("main: failed to initialize Borealis");
        return EXIT_FAILURE;
    }
    brls::Application::createWindow("TorrentShopNX");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);
    brls::Application::registerXMLView("QrCodeView", ui::QrCodeView::create);

    // Initialize managers and load configurations
    auto& cfg = config::ConfigManager::instance();
    cfg.load();
    
    catalog::FavoritesManager::instance().init("sdmc:/switch/TorrentShopNX/favorites.json");
    ui::DownloadManager::instance().init();
    
    // Force eager initialization of TorrentEngine in the main thread to prevent background thread crashes
    util::logLine("main: initializing TorrentEngine eagerly");
    auto& eng = torrent::TorrentEngine::instance();
    util::logLine("main: TorrentEngine eagerly initialized, address=" + std::to_string((uintptr_t)&eng));

    // Check for catalog updates
    updateCatalogFromUrlIfNeeded(cfg);

    // Load database games
    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

#if __has_include(<curl/curl.h>)
    curl_global_init(CURL_GLOBAL_ALL);
#endif

    util::logLine("config: loaded config.ini");
    g_games = loadGamesFromFile(kCatalogPath);
    util::logLine("main: loaded g_games count=" + std::to_string(g_games.size()));

    // Fallback parser if JSON games catalog doesn't exist
    if (g_games.empty()) {
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
                g_games.push_back(g);
            }
        }
    }

    // Sub-views are now Activities and do not need XML registration.

    // Bootstrap activity
    util::logLine("main: instantiating and pushing MainMenu...");
    MainMenu* menu = new MainMenu();
    util::logLine("main: MainMenu instantiated, pushing...");
    brls::Application::pushActivity(menu);

    util::logLine("main: entering mainLoop");
    // Execute Borealis main loop
    try {
        int frameCount = 0;
        while (brls::Application::mainLoop()) {
            frameCount++;
            if (frameCount <= 3) {
                util::logLine("main: mainLoop frame=" + std::to_string(frameCount));
            }
        }
        util::logLine("main: mainLoop exited normally after " + std::to_string(frameCount) + " frames");
    } catch (const std::exception& e) {
        util::logLine(std::string("main: EXCEPTION in mainLoop: ") + e.what());
    } catch (...) {
        util::logLine("main: UNKNOWN EXCEPTION in mainLoop");
    }

    // Clean exit
    ui::DownloadManager::instance().shutdown();
    torrent::TorrentEngine::instance().stop();
    if (romfs_ok) {
        romfsExit();
    }
    
    // Clear all caches right before exiting
    clearCaches();
    
    quick_exit(0);
}
