#ifdef __SWITCH__
#include <switch.h>
#include <unistd.h>  // for _exit()
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#define usleep(us) Sleep((us) / 1000)
#else
#include <unistd.h>
#endif

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
#include "catalog/IgnoredUpdatesManager.hpp"
#include "ui/DownloadUiManager.hpp"
#include "ui/AppletWarningView.hpp"
#include "ui/QrCodeView.hpp"
#include "ui/RetroCatalogView.hpp"
#include "catalog/retro_catalog_manager.h"
#include "config/config.h"
#include "utils/log.h"
#include "utils/switch_utils.h"
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

#ifdef __SWITCH__
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
            cfg.tcp_tx_buf_max_size = 0x60000;  // 384 KB max
            cfg.tcp_rx_buf_max_size = 0x60000;  // 384 KB max
            cfg.udp_rx_buf_size = 0x8000;       // 32 KB
            cfg.udp_tx_buf_size = 0x4000;       // 16 KB
        }
        g_socket_init_result = socketInitialize(&cfg);
        if (R_FAILED(g_socket_init_result)) {
            g_socket_init_result = socketInitializeDefault();
        }
        g_socket_mem_size_used = __nx_socket_mem_size;

        // Note: romfsInit() is deliberately NOT called here in userAppInit().
        // Calling romfsInit() here locks the running .nro file on SD card before main()
        // can execute and replace it during auto-updates. romfsInit() is called in main()
        // right before Borealis UI initialization.
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

    bool g_romfs_mounted = false;

    void userAppExit(void) {
        AppletType applet_type = appletGetAppletType();
        bool is_applet = (applet_type == AppletType_LibraryApplet || applet_type == AppletType_OverlayApplet);

        if (!is_applet) {
            util::setBacklightOff(false);
            lblExit();
            nifmExit();
            inssExit();
            hidsysExit();
        }
        psmExit();
        setExit();
        setsysExit();
        plExit();
        if (g_romfs_mounted) {
            romfsExit();
            g_romfs_mounted = false;
        }
        socketExit();
    }
}

namespace util {
void unmountRomfs() {
    if (g_romfs_mounted) {
        romfsExit();
        g_romfs_mounted = false;
        util::logLine("romfs: unmounted successfully");
    }
}
}
#else
namespace util {
void unmountRomfs() {}
}
#endif


std::string g_nroPath = "sdmc:/switch/TorrentShopNX/TorrentShopNX.nro";

static void normalizeNroPath() {
    if (g_nroPath.empty()) {
        g_nroPath = "sdmc:/switch/TorrentShopNX/TorrentShopNX.nro";
    }
    if (g_nroPath.rfind("sdmc:/", 0) != 0) {
        if (g_nroPath.rfind("sdmc:", 0) == 0) {
            std::string sub = g_nroPath.substr(5);
            if (!sub.empty() && sub.front() == '/') sub = sub.substr(1);
            g_nroPath = "sdmc:/" + sub;
        } else if (g_nroPath.front() == '/') {
            g_nroPath = "sdmc:" + g_nroPath;
        } else {
            g_nroPath = "sdmc:/" + g_nroPath;
        }
    }
}

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

static bool replaceNroFile(const std::string& srcPath, const std::string& dstPath) {
    struct stat st;
    if (stat(srcPath.c_str(), &st) != 0 || st.st_size < 100 * 1024) {
        util::logLine("replaceNroFile: src invalid or too small (" + srcPath + ")");
        return false;
    }

    std::string oldPath = dstPath + ".old";
    std::remove(oldPath.c_str());

    // Rename dst -> old first to vacate destination slot on FAT32
    int r1 = ::rename(dstPath.c_str(), oldPath.c_str());
    util::logLine("replaceNroFile: rename dst -> old (" + dstPath + " -> " + oldPath + ") res=" + std::to_string(r1));

    // Rename src -> dst
    int r2 = ::rename(srcPath.c_str(), dstPath.c_str());
    util::logLine("replaceNroFile: rename src -> dst (" + srcPath + " -> " + dstPath + ") res=" + std::to_string(r2));

    if (r2 == 0) {
        std::remove(oldPath.c_str());
#ifdef __SWITCH__
        fsdevCommitDevice("sdmc");
#endif
        util::logLine("replaceNroFile: successfully replaced NRO via rename");
        return true;
    }

    // If rename src -> dst failed, try restoring oldPath back to dstPath
    if (r1 == 0 && stat(dstPath.c_str(), &st) != 0) {
        ::rename(oldPath.c_str(), dstPath.c_str());
    }

    // Fallback: copyFileOverwrite
    util::logLine("replaceNroFile: rename failed, falling back to copyFileOverwrite");
    std::remove(dstPath.c_str());
    bool ok = copyFileOverwrite(srcPath, dstPath);
    if (ok) {
        std::remove(srcPath.c_str());
        std::remove(oldPath.c_str());
#ifdef __SWITCH__
        fsdevCommitDevice("sdmc");
#endif
        util::logLine("replaceNroFile: successfully replaced NRO via copy");
        return true;
    }

    util::logLine("replaceNroFile: copyFileOverwrite also failed!");
    return false;
}

static bool checkAndApplyPendingUpdate() {
#ifdef __SWITCH__
    // Ensure RomFS is not mounted while we manipulate NRO files
    util::unmountRomfs();
#endif

    // 1. If we are running AS the .update file (e.g. TorrentShopNX.nro.update)
    if (g_nroPath.find(".update") != std::string::npos) {
        std::string mainNroPath = g_nroPath.substr(0, g_nroPath.find(".update"));
        util::logLine("main: running as update NRO (" + g_nroPath + "). Overwriting main NRO: " + mainNroPath);
        
        bool ok = replaceNroFile(g_nroPath, mainNroPath);
        util::logLine("main: replace result=" + std::to_string(ok));
        
        std::remove(g_nroPath.c_str());
        
#ifdef __SWITCH__
        if (envHasNextLoad()) {
            std::string quotedArg = "\"" + mainNroPath + "\"";
            envSetNextLoad(mainNroPath.c_str(), quotedArg.c_str());
            util::logLine("main: relaunching main NRO via envSetNextLoad: " + mainNroPath);
            return true; // Signal main to exit so HBL chainloads mainNroPath
        }
#endif
        return ok;
    }

    // 2. We are running as regular TorrentShopNX.nro. Check for pending .update files.
    std::vector<std::string> possibleUpdates = {
        g_nroPath + ".update",
        "sdmc:/switch/TorrentShopNX/TorrentShopNX.nro.update",
        "sdmc:/switch/TorrentShopNX.nro.update"
    };

    for (const auto& upPath : possibleUpdates) {
        struct stat st;
        if (stat(upPath.c_str(), &st) == 0) {
            if (st.st_size >= 100 * 1024) {
                // Determine target NRO path for this update file
                std::string targetNro = upPath;
                if (targetNro.size() > 7 && targetNro.rfind(".update") == targetNro.size() - 7) {
                    targetNro = targetNro.substr(0, targetNro.size() - 7);
                } else {
                    targetNro = g_nroPath;
                }

                util::logLine("main: found pending update at " + upPath + " (" + std::to_string(st.st_size) + " bytes), target=" + targetNro);
                
                bool ok = replaceNroFile(upPath, targetNro);
                util::logLine("main: replaceNroFile to " + targetNro + " result=" + std::to_string(ok));
                
                if (ok) {
                    g_nroPath = targetNro;
#ifdef __SWITCH__
                    if (envHasNextLoad()) {
                        std::string quotedArg = "\"" + g_nroPath + "\"";
                        envSetNextLoad(g_nroPath.c_str(), quotedArg.c_str());
                        util::logLine("main: relaunching updated NRO via envSetNextLoad: " + g_nroPath);
                    }
#endif
                    return true; // Signal main to exit so HBL chainloads the freshly updated NRO
                } else {
                    util::logLine("main: failed to replace NRO with update from " + upPath);
                }
            } else {
                util::logLine("main: removing invalid/small update file at " + upPath + " (" + std::to_string(st.st_size) + " bytes)");
                std::remove(upPath.c_str());
            }
        }
    }
    return false;
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    if (argc > 0 && argv[0] && std::string(argv[0]).find(".nro") != std::string::npos) {
        g_nroPath = argv[0];
    }
    normalizeNroPath();

    util::logInit();
    util::logLine("main: start g_nroPath=" + g_nroPath);

#ifdef __SWITCH__
    if (checkAndApplyPendingUpdate()) {
        util::logLine("main: exiting for update relaunch");
        fsdevCommitDevice("sdmc");
        util::logClose();
        _exit(0);
    }
#endif

#ifdef __SWITCH__
    util::logLine("main: socketInit result=" + std::to_string(g_socket_init_result) +
                  " mem_size=" + std::to_string(g_socket_mem_size_used) +
                  " applet_type=" + std::to_string(g_applet_type_detected));
#endif
    migrateStorageLayout();
    clearCaches();

    // Load configurations before UI init to set desired locale
    auto& cfg = config::ConfigManager::instance();

    std::string userLang = cfg.getLanguage();
    if (userLang == "ru") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_RU;
    } else if (userLang == "en-US" || userLang == "en") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_EN_US;
    } else if (userLang == "es") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_ES;
    } else if (userLang == "fr") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_FR;
    } else if (userLang == "de") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_DE;
    } else if (userLang == "it") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_IT;
    } else if (userLang == "pt-BR" || userLang == "pt") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_PT_BR;
    } else if (userLang == "zh-Hans" || userLang == "zh-CN" || userLang == "zh") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_ZH_HANS;
    } else if (userLang == "ja") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_JA;
    } else {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AUTO;
    }

    // Initialize RomFS before Borealis UI loads resources
#ifdef __SWITCH__
    if (R_SUCCEEDED(romfsInit())) {
        g_romfs_mounted = true;
        util::logLine("main: romfsInit succeeded");
    } else {
        util::logLine("main: romfsInit failed");
    }
#endif

    // Initialize Borealis UI
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
        // Initialize managers for Title Mode
        catalog::FavoritesManager::instance().init(TSNX_FAVORITES_PATH);
        catalog::IgnoredUpdatesManager::instance().init(TSNX_IGNORED_UPDATES_PATH);
        ui::DownloadManager::instance().init();

        // Initialize curl first, so background network threads can safely use it.
    #if __has_include(<curl/curl.h>)
        curl_global_init(CURL_GLOBAL_ALL);
    #endif
        net::ImageDownloader::instance().init(4);

        // Load database games via fast binary cache (or fallback to JSON)
        auto loadedGames = loadGamesCached(getCatalogPath(), getCatalogBinPath());
        setCatalogSnapshot(std::move(loadedGames));
        util::logLine("main: initially loaded g_games count=" + std::to_string(getCatalogSnapshot()->size()) + " (path=" + getCatalogPath() + ")");

        // Logger configuration
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

        // Bootstrap activity
#ifndef __SWITCH__
        if (argc > 1 && std::string(argv[1]) == "--retro") {
            auto& mgr = catalog::RetroCatalogManager::instance();
            const auto* info = mgr.findConsole("ps1");
            if (info) {
                util::logLine("main: launching directly into RetroCatalogView for ps1");
                brls::Application::pushActivity(new ui::RetroCatalogView(*info));
            } else {
                util::logLine("main: instantiating and pushing MainMenu...");
                MainMenu* menu = new MainMenu();
                util::logLine("main: MainMenu instantiated, pushing...");
                brls::Application::pushActivity(menu);
            }
        } else {
            util::logLine("main: instantiating and pushing MainMenu...");
            MainMenu* menu = new MainMenu();
            util::logLine("main: MainMenu instantiated, pushing...");
            brls::Application::pushActivity(menu);
        }
#else
        util::logLine("main: instantiating and pushing MainMenu...");
        MainMenu* menu = new MainMenu();
        util::logLine("main: MainMenu instantiated, pushing...");
        brls::Application::pushActivity(menu);
#endif
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
    util::setBacklightOff(false);

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

#ifdef __SWITCH__
    // Unmount RomFS now that UI and all threads have stopped.
    // This releases the file lock on g_nroPath so any pending update can be applied right now!
    util::unmountRomfs();

    // Apply pending update if one was downloaded during this session
    if (checkAndApplyPendingUpdate()) {
        util::logLine("main: pending update applied during shutdown");
    }
#endif

    // Do NOT call curl_global_cleanup() because it might crash if curl threads are alive

    // Use _exit(0) to exit cleanly:
    //   - Skips C++ atexit handlers / global destructors (prevents engine teardown crashes)
    //   - Calls __libnx_exit → __appExit → userAppExit (proper service teardown)
    //   - Calls envGetExitFuncPtr() to return to Homebrew Menu (not svcExitProcess!)
    //
    // svcExitProcess() was killing the ENTIRE HBMenu process because NROs share
    // HBMenu's address space. _exit() is the correct way to return to HBMenu.
#ifdef __SWITCH__
    fsdevCommitDevice("sdmc");
    util::logLine("main: closing log and returning to HBMenu via _exit(0). Goodbye!");
    util::logClose();  // close log file before __appExit calls fsExit
    _exit(0);
#else
    util::logLine("main: closing log and exiting. Goodbye!");
    util::logClose();
#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
#endif
}
