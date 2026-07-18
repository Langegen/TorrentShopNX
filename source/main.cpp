#include <switch.h>
#include <unistd.h>  // for _exit()
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
#include <thread>
#include <borealis/extern/nlohmann/json.hpp>

// Global list of catalog games
std::vector<Game> g_games;



extern "C" {
    u32 __nx_socket_mem_size = 0x02000000; // Default to 32MB for Title Mode
    size_t __nx_socket_tcp_tx_buf_size = 0x10000; // Default to 64KB TCP send buffer
    size_t __nx_socket_tcp_rx_buf_size = 0x10000; // Default to 64KB TCP recv buffer

    Result g_socket_init_result = 0xFFFFFFFF;
    u32 g_socket_mem_size_used = 0;
    int g_applet_type_detected = -1;

    void userAppInit(void) {
        // We do NOT call appletLockExit() because we want the OS to be able to cleanly exit the app.
        // Calling it and failing to unlock it perfectly before process teardown causes the OS "An error occurred" dialog.

        // Dynamically adjust socket pool and buffer sizes for Applet vs Title mode to prevent socket memory exhaustion
        AppletType applet_type = appletGetAppletType();
        g_applet_type_detected = static_cast<int>(applet_type);
        if (applet_type == AppletType_LibraryApplet || applet_type == AppletType_OverlayApplet) {
            __nx_socket_mem_size = 0x00400000;        // 4MB socket pool for Applet Mode
            __nx_socket_tcp_tx_buf_size = 0x8000;      // 32KB default send buffer
            __nx_socket_tcp_rx_buf_size = 0x8000;      // 32KB default recv buffer
        } else {
            __nx_socket_mem_size = 0x02000000;        // 32MB socket pool for Title Mode
            __nx_socket_tcp_tx_buf_size = 0x10000;     // 64KB default send buffer
            __nx_socket_tcp_rx_buf_size = 0x10000;     // 64KB default recv buffer
        }

        // Custom network initialization configured for TorrentShopNX
        SocketInitConfig cfg = *(socketGetDefaultInitConfig());
        cfg.num_bsd_sessions  = 12; // Allow up to 12 sessions for multiple connections
        if (applet_type == AppletType_LibraryApplet || applet_type == AppletType_OverlayApplet) {
            cfg.sb_efficiency = 4;
            cfg.tcp_tx_buf_max_size = 32768; // 32KB max for Applet Mode
            cfg.tcp_rx_buf_max_size = 32768;
            cfg.udp_rx_buf_size = 32768;
            cfg.udp_tx_buf_size = 32768;
        } else {
            cfg.sb_efficiency = 8; // Optimal efficiency to prevent fragmentation while saving memory
            cfg.tcp_tx_buf_max_size = 65536; // 64KB max for Title Mode
            cfg.tcp_rx_buf_max_size = 65536;
            cfg.udp_rx_buf_size = 32768; // 32KB UDP buffer is plenty for DHT/trackers
            cfg.udp_tx_buf_size = 32768;
        }
        g_socket_init_result = socketInitialize(&cfg);
        g_socket_mem_size_used = __nx_socket_mem_size;

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
}

static const char* kCatalogPath = "sdmc:/switch/TorrentShopNX/switch_games.json";

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    util::logInit();
    util::logLine("main: start");
    util::logLine("main: socketInit result=" + std::to_string(g_socket_init_result) +
                  " mem_size=" + std::to_string(g_socket_mem_size_used) +
                  " applet_type=" + std::to_string(g_applet_type_detected));
    clearCaches();

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

    // Initialize curl first, so background network threads can safely use it.
#if __has_include(<curl/curl.h>)
    curl_global_init(CURL_GLOBAL_ALL);
#endif

    // Load database games instantly on the main thread (takes <50ms)
    g_games = loadGamesFromFile(kCatalogPath);
    util::logLine("main: initially loaded g_games count=" + std::to_string(g_games.size()));

    // Logger configuration
    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

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

    util::logLine("main: mainLoop exited, starting shutdown sequence");

    // Signal background cleanup to cancel immediately
    g_cleanupCancelled = true;

    // Wait for background tasks (cleanup and catalog update) to complete safely before deinitializing systems
    util::logLine("main: waiting for background threads to exit...");
    int waitCount = 0;
    while ((g_cleanupRunning.load() || g_catalogUpdateRunning.load()) && waitCount < 100) {
        usleep(10000); // 10ms
        waitCount++;
    }
    util::logLine("main: background threads exited after " + std::to_string(waitCount * 10) + "ms, proceeding with shutdown");

    // Stop threads
    util::logLine("main: calling DownloadManager::shutdown");
    ui::DownloadManager::instance().shutdown();
    
    util::logLine("main: calling TorrentEngine::stop");
    torrent::TorrentEngine::instance().stop();
    
    util::logLine("main: all threads requested to stop");

    // Do NOT call curl_global_cleanup() because it might crash if curl threads are alive

    // Use _exit(0) to exit cleanly:
    //   - Skips C++ atexit handlers / global destructors (prevents libtorrent crash)
    //   - Calls __libnx_exit → __appExit → userAppExit (proper service teardown)
    //   - Calls envGetExitFuncPtr() to return to Homebrew Menu (not svcExitProcess!)
    //
    // svcExitProcess() was killing the ENTIRE HBMenu process because NROs share
    // HBMenu's address space. _exit() is the correct way to return to HBMenu.
    util::logLine("main: closing log and returning 0 to HBMenu. Goodbye!");
    util::logClose();  // close log file before __appExit calls fsExit

    return 0;
}
