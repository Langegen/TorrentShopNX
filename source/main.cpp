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
#include "ui/AppletWarningView.hpp"
#include "ui/QrCodeView.hpp"
#include "config/config.h"
#include "utils/log.h"
#include "net/http_client.h"
#include "net/image_downloader.h"
#include <thread>
#include <borealis/extern/nlohmann/json.hpp>

// Global list of catalog games
std::vector<Game> g_games;

// Helper to check Applet Mode
static bool checkAppletMode() {
#ifdef __SWITCH__
    AppletType type = appletGetAppletType();
    return (type == AppletType_LibraryApplet || type == AppletType_OverlayApplet);
#else
    return false;
#endif
}



extern "C" {
    u32 __nx_socket_mem_size = 0x02000000; // Default to 32MB for Title Mode
    size_t __nx_socket_tcp_tx_buf_size = 0x10000; // Default to 64KB TCP send buffer
    size_t __nx_socket_tcp_rx_buf_size = 0x10000; // Default to 64KB TCP recv buffer

    Result g_socket_init_result = 0xFFFFFFFF;
    u32 g_socket_mem_size_used = 0;
    int g_applet_type_detected = -1;

    void userAppInit(void) {
        AppletType applet_type = appletGetAppletType();
        g_applet_type_detected = static_cast<int>(applet_type);
        bool is_applet = (applet_type == AppletType_LibraryApplet || applet_type == AppletType_OverlayApplet);

        if (is_applet) {
            __nx_socket_mem_size = 0x00100000;        // 1MB minimal socket pool for Applet Mode
            __nx_socket_tcp_tx_buf_size = 0x4000;      // 16KB default send buffer
            __nx_socket_tcp_rx_buf_size = 0x4000;      // 16KB default recv buffer
        } else {
            __nx_socket_mem_size = 0x02000000;        // 32MB socket pool for Title Mode
            __nx_socket_tcp_tx_buf_size = 0x4000;      // 16KB initial send buffer
            __nx_socket_tcp_rx_buf_size = 0x8000;      // 32KB initial recv buffer
        }

        SocketInitConfig cfg = *(socketGetDefaultInitConfig());
        if (is_applet) {
            cfg.num_bsd_sessions = 4;
            cfg.sb_efficiency = 2;
            cfg.tcp_tx_buf_size = 0x4000;
            cfg.tcp_rx_buf_size = 0x4000;
            cfg.tcp_tx_buf_max_size = 0x8000;
            cfg.tcp_rx_buf_max_size = 0x8000;
            cfg.udp_rx_buf_size = 8192;
            cfg.udp_tx_buf_size = 8192;
        } else {
            // Switch BSD buffer pool is fixed; keep per-socket initial cost low
            // so the engine can open many peer sockets without ENOBUFS.
            cfg.num_bsd_sessions = 12;
            cfg.sb_efficiency = 8;
            cfg.tcp_tx_buf_size = 0x4000;       // 16 KB initial
            cfg.tcp_rx_buf_size = 0x8000;       // 32 KB initial
            cfg.tcp_tx_buf_max_size = 0x40000;  // 256 KB stock
            cfg.tcp_rx_buf_max_size = 0x40000;  // 256 KB stock
            cfg.udp_rx_buf_size = 0x8000;       // 32 KB
            cfg.udp_tx_buf_size = 0x4000;       // 16 KB
        }
        g_socket_init_result = socketInitialize(&cfg);
        if (R_FAILED(g_socket_init_result)) {
            g_socket_init_result = socketInitializeDefault();
        }
        g_socket_mem_size_used = __nx_socket_mem_size;

        romfsInit();
        plInitialize(PlServiceType_User);
        setsysInitialize();
        setInitialize();
        psmInitialize();

        // Privileged services (hidsys, inss, lbl, nifm) are only available in Title Mode.
        // Calling them in Applet Mode causes OS service permission failure and system crashes.
        if (!is_applet) {
            hidsysInitialize();
            inssInitialize();
            nifmInitialize(NifmServiceType_User);
            lblInitialize();
        }
    }

    void userAppExit(void) {
        AppletType applet_type = appletGetAppletType();
        bool is_applet = (applet_type == AppletType_LibraryApplet || applet_type == AppletType_OverlayApplet);

        if (!is_applet) {
            net::ImageDownloader::instance().stop();
            lblExit();
            nifmExit();
            inssExit();
            hidsysExit();
        }
        psmExit();
        setExit();
        setsysExit();
        plExit();
        romfsExit();
        socketExit();
    }
}

std::string g_nroPath = "sdmc:/switch/TorrentShopNX/TorrentShopNX.nro";

static bool copyFileOverwrite(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize fileSize = in.tellg();
    in.seekg(0, std::ios::beg);
    if (fileSize < 100 * 1024) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    char buffer[65536];
    std::streamsize copied = 0;
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        std::streamsize bytes = in.gcount();
        if (bytes > 0) {
            out.write(buffer, bytes);
            if (out.fail()) {
                out.close();
                in.close();
                return false;
            }
            copied += bytes;
        }
    }
    out.flush();
    out.close();
    in.close();
    return (copied >= 100 * 1024);
}

static bool checkAndApplyPendingUpdate() {
    // 1. If we are running AS the .update file (e.g. TorrentShopNX.nro.update)
    if (g_nroPath.find(".update") != std::string::npos) {
        std::string mainNroPath = g_nroPath.substr(0, g_nroPath.find(".update"));
        util::logLine("main: running as update NRO (" + g_nroPath + "). Overwriting main NRO: " + mainNroPath);
        
        bool ok = copyFileOverwrite(g_nroPath, mainNroPath);
        util::logLine("main: copy result=" + std::to_string(ok));
        
        // Remove the .update file so we NEVER enter a bootloop
        std::remove(g_nroPath.c_str());
        
#ifdef __SWITCH__
        if (envHasNextLoad()) {
            envSetNextLoad(mainNroPath.c_str(), mainNroPath.c_str());
            util::logLine("main: relaunching main NRO via envSetNextLoad: " + mainNroPath);
            return true; // Signal main to exit so HBL chainloads mainNroPath
        }
#endif
        return false;
    }

    // 2. We are running as regular TorrentShopNX.nro. Check for pending .update files.
    std::string updatePath = g_nroPath + ".update";
    std::vector<std::string> possibleUpdates = {
        updatePath,
        "sdmc:/switch/TorrentShopNX/TorrentShopNX.nro.update",
        "sdmc:/switch/TorrentShopNX.nro.update"
    };

    for (const auto& upPath : possibleUpdates) {
        struct stat st;
        if (stat(upPath.c_str(), &st) == 0) {
            if (st.st_size >= 100 * 1024) {
                util::logLine("main: found pending update at " + upPath + " (" + std::to_string(st.st_size) + " bytes), applying...");
                
                // Copy the update over main NRO. Since no RomFS is mounted yet at top of main(), this succeeds
                bool ok = copyFileOverwrite(upPath, g_nroPath);
                util::logLine("main: copyFileOverwrite to " + g_nroPath + " result=" + std::to_string(ok));
                
                // CRITICAL: ALWAYS delete the .update file after processing it
                std::remove(upPath.c_str());
                
#ifdef __SWITCH__
                if (ok && envHasNextLoad()) {
                    envSetNextLoad(g_nroPath.c_str(), g_nroPath.c_str());
                    util::logLine("main: relaunching updated NRO via envSetNextLoad");
                    return true; // Signal main to exit so HBL chainloads the freshly updated NRO
                }
#endif
            } else {
                util::logLine("main: removing invalid/small update file at " + upPath + " (" + std::to_string(st.st_size) + " bytes)");
                std::remove(upPath.c_str());
            }
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc > 0 && argv[0] && std::string(argv[0]).find(".nro") != std::string::npos) {
        g_nroPath = argv[0];
    }

    util::logInit();
    util::logLine("main: start g_nroPath=" + g_nroPath);

    if (checkAndApplyPendingUpdate()) {
        util::logLine("main: exiting for update relaunch");
        util::logClose();
        return 0;
    }

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

    // Register focus change listener to handle console sleep / wake safely (Title Mode only)
    if (!checkAppletMode()) {
        brls::Application::getWindowFocusChangedEvent()->subscribe([](bool focused) {
            if (!focused) {
                util::logLine("main: focus lost (console going to sleep / minimized)");
            } else {
                util::logLine("main: focus regained (console waking up)");
            }
        });
    }

    // Check if running in Applet Mode (Album launch)
    if (checkAppletMode()) {
        util::logLine("main: Applet Mode detected! Displaying AppletWarningView...");
        brls::Application::pushActivity(new ui::AppletWarningView());
    } else {
        // Initialize managers and load configurations for Title Mode
        auto& cfg = config::ConfigManager::instance();
        cfg.load();

        catalog::FavoritesManager::instance().init("sdmc:/switch/TorrentShopNX/favorites.json");
        ui::DownloadManager::instance().init();

        // Initialize curl first, so background network threads can safely use it.
    #if __has_include(<curl/curl.h>)
        curl_global_init(CURL_GLOBAL_ALL);
    #endif
        net::ImageDownloader::instance().init(4);

        // Load database games instantly on the main thread (takes <50ms)
        g_games = loadGamesFromFile(kCatalogPath);
        util::logLine("main: initially loaded g_games count=" + std::to_string(g_games.size()));

        // Logger configuration
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

        // Bootstrap activity
        util::logLine("main: instantiating and pushing MainMenu...");
        MainMenu* menu = new MainMenu();
        util::logLine("main: MainMenu instantiated, pushing...");
        brls::Application::pushActivity(menu);
    }

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

    // Signal background tasks and network transfers to cancel immediately
    g_appExiting.store(true);
    g_cleanupCancelled = true;

    // Wait for background tasks (cleanup and catalog update) to complete safely before deinitializing systems
    util::logLine("main: waiting for background threads to exit...");
    int waitCount = 0;
    while ((g_cleanupRunning.load() || g_catalogUpdateRunning.load()) && waitCount < 100) {
        usleep(10000); // 10ms
        waitCount++;
    }
    util::logLine("main: background threads exited after " + std::to_string(waitCount * 10) + "ms, proceeding with shutdown");

    // Stop threads (Title Mode only)
    if (!checkAppletMode()) {
        util::logLine("main: calling DownloadManager::shutdown");
        ui::DownloadManager::instance().shutdown();

        util::logLine("main: calling ImageDownloader::stop");
        net::ImageDownloader::instance().stop();
        
        util::logLine("main: all threads requested to stop");
    }

    // Do NOT call curl_global_cleanup() because it might crash if curl threads are alive

    // Use _exit(0) to exit cleanly:
    //   - Skips C++ atexit handlers / global destructors (prevents engine teardown crashes)
    //   - Calls __libnx_exit → __appExit → userAppExit (proper service teardown)
    //   - Calls envGetExitFuncPtr() to return to Homebrew Menu (not svcExitProcess!)
    //
    // svcExitProcess() was killing the ENTIRE HBMenu process because NROs share
    // HBMenu's address space. _exit() is the correct way to return to HBMenu.
    util::logLine("main: closing log and returning 0 to HBMenu. Goodbye!");
    util::logClose();  // close log file before __appExit calls fsExit

    return 0;
}
