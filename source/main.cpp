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
#include <borealis/core/thread_pool.hpp>
#include <nanovg.h>

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
#include "ui/ThemeManager.hpp"
#include "ui/FavoritesManager.hpp"
#include "catalog/IgnoredUpdatesManager.hpp"
#include "catalog/game_stats_manager.h"
#include "ui/DownloadUiManager.hpp"
#include "ui/AppletWarningView.hpp"
#include "ui/QrCodeView.hpp"
#include "ui/RetroCatalogView.hpp"
#include "catalog/retro_catalog_manager.h"
#include "config/config.h"
#include "utils/log.h"
#include "utils/update_journal.h"
#include "utils/perf_stats.h"
#include "utils/ui_watchdog.h"
#include "engine/engine_log.h"
#include "utils/switch_utils.h"
#include "utils/screen_sleep_manager.h"
#include "net/http_client.h"
#include "net/image_downloader.h"
#include "datasource/custom_engine_client.h"
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
    Result g_socket_init_result = 0xFFFFFFFF;
    u32 g_socket_mem_size_used = 0;   // BSD transfer memory actually handed to bsd:u
    u32 g_socket_sb_efficiency = 0;
    int g_applet_type_detected = -1;

    // Size of the BSD transfer memory libnx allocates for a config. This memory
    // is the WHOLE socket-buffer pool of the process (every TCP/UDP socket,
    // curl included), not a per-socket budget. Same formula as libnx
    // bsdInitialize(): sb_efficiency * page_round(tx_max + rx_max + udp_tx + udp_rx).
    static u32 socketPoolBytes(const SocketInitConfig& c) {
        u32 tx = c.tcp_tx_buf_max_size ? c.tcp_tx_buf_max_size : c.tcp_tx_buf_size;
        u32 rx = c.tcp_rx_buf_max_size ? c.tcp_rx_buf_max_size : c.tcp_rx_buf_size;
        u32 sum = tx + rx + c.udp_tx_buf_size + c.udp_rx_buf_size;
        sum = (sum + 0xFFF) & ~0xFFFu;
        return c.sb_efficiency * sum;
    }

    void userAppInit(void) {
        appletLockExit();
        AppletType applet_type = appletGetAppletType();
        g_applet_type_detected = static_cast<int>(applet_type);
        bool is_applet = (applet_type == AppletType_LibraryApplet || applet_type == AppletType_OverlayApplet);

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
            // The pool is sb_efficiency * (tx_max + rx_max + udp_tx + udp_rx) for the
            // whole process (see socketPoolBytes). The old config (2 * 592 KB) gave
            // ~1.2 MB: socket() failed with ENOBUFS after a few dozen sockets, so the
            // engine held 15-20 peers out of hundreds known, UDP trackers failed with
            // errno 105 and image fetches failed mid-download.
            //
            // 8 * (1.5 MB + 64 KB + 16 KB + 32 KB) ~= 12.9 MB: room for ~64 peers,
            // dials in flight, trackers, DHT and HTTP. sb_efficiency stays within the
            // documented 1..8; the size comes from rx_max, which is only the autotune
            // ceiling of one socket (the engine sets SO_RCVBUF explicitly anyway).
            cfg.num_bsd_sessions = 16;
            cfg.sb_efficiency = 8;
            cfg.tcp_tx_buf_size = 0x4000;        // 16 KB initial
            cfg.tcp_rx_buf_size = 0x8000;        // 32 KB initial
            cfg.tcp_tx_buf_max_size = 0x10000;   // 64 KB max (we also upload pieces)
            cfg.tcp_rx_buf_max_size = 0x180000;  // 1.5 MB max
            cfg.udp_rx_buf_size = 0x8000;        // 32 KB
            cfg.udp_tx_buf_size = 0x4000;        // 16 KB
        }
        g_socket_init_result = socketInitialize(&cfg);
        if (R_FAILED(g_socket_init_result) && !is_applet) {
            // Smaller pool (~6.4 MB) if the service refused the big one.
            cfg.sb_efficiency = 4;
            g_socket_init_result = socketInitialize(&cfg);
        }
        if (R_FAILED(g_socket_init_result)) {
            cfg = *(socketGetDefaultInitConfig());
            g_socket_init_result = socketInitializeDefault();
        }
        g_socket_mem_size_used = socketPoolBytes(cfg);
        g_socket_sb_efficiency = cfg.sb_efficiency;

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
        appletUnlockExit();
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

#ifdef __SWITCH__
extern "C" u32 __nx_applet_exit_mode;
#endif

// How the process leaves after an update was applied.
//   relaunch=true  (start of main, nothing but the main thread is running):
//                  hand the new NRO to hbloader via envSetNextLoad.
//   relaunch=false (end of a normal session): only replace the file and keep
//                  the normal exit to the HOME menu. Returning to hbloader from
//                  a full session is unsafe: engine/curl/image threads may still
//                  be alive in the shared process, and once hbloader maps the
//                  next NRO over their code they crash the whole title.
static void prepareExitAfterUpdate(const std::string& nroPath, bool relaunch) {
#ifdef __SWITCH__
    if (relaunch && envHasNextLoad()) {
        std::string quotedArg = "\"" + nroPath + "\"";
        envSetNextLoad(nroPath.c_str(), quotedArg.c_str());
        __nx_applet_exit_mode = 0;
        util::updateJournal("relaunching updated NRO via envSetNextLoad: " + nroPath);
    } else {
        __nx_applet_exit_mode = 1; // straight to the HOME menu, process is torn down
        util::updateJournal(relaunch ? "loader has no next-load support, exiting to HOME"
                                     : "update applied at exit, closing to HOME");
    }
#else
    (void)nroPath;
    (void)relaunch;
#endif
}

static bool checkAndApplyPendingUpdate(bool relaunch) {
#ifdef __SWITCH__
    // Ensure RomFS is not mounted while we manipulate NRO files
    util::unmountRomfs();
#endif

    // 1. If we are running AS the .update file (e.g. TorrentShopNX.nro.update)
    if (g_nroPath.find(".update") != std::string::npos) {
        std::string mainNroPath = g_nroPath.substr(0, g_nroPath.find(".update"));
        util::updateJournal("running as update NRO (" + g_nroPath + "), overwriting " + mainNroPath);

        bool ok = replaceNroFile(g_nroPath, mainNroPath);
        util::updateJournal(std::string("replace result=") + (ok ? "ok" : "FAILED"));

        std::remove(g_nroPath.c_str());
        prepareExitAfterUpdate(mainNroPath, relaunch);
        return true;
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
            if (util::isValidNroFile(upPath)) {
                // Determine target NRO path for this update file
                std::string targetNro = upPath;
                if (targetNro.size() > 7 && targetNro.rfind(".update") == targetNro.size() - 7) {
                    targetNro = targetNro.substr(0, targetNro.size() - 7);
                } else {
                    targetNro = g_nroPath;
                }

                util::updateJournal(std::string(relaunch ? "start" : "exit") + ": found pending update " +
                                    upPath + " (" + std::to_string(st.st_size) + " bytes), target=" + targetNro);

                bool ok = replaceNroFile(upPath, targetNro);
                util::updateJournal("replaceNroFile to " + targetNro + (ok ? " ok" : " FAILED"));

                if (ok) {
                    g_nroPath = targetNro;
                    prepareExitAfterUpdate(g_nroPath, relaunch);
                    return true;
                }
            } else {
                util::updateJournal("removing invalid update file " + upPath + " (" + std::to_string(st.st_size) +
                                    " bytes, no NRO0 header)");
                std::remove(upPath.c_str());
            }
        }
    }
    return false;
}

int main(int argc, char** argv) {
    std::set_terminate([]() {
        std::string err = "std::terminate called!";
        std::exception_ptr ex = std::current_exception();
        if (ex) {
            try {
                std::rethrow_exception(ex);
            } catch (const std::exception& e) {
                err += " Exception: " + std::string(e.what());
            } catch (...) {
                err += " Unknown exception";
            }
        }
        util::logLine("FATAL: " + err);
        std::abort();
    });

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
    if (checkAndApplyPendingUpdate(/*relaunch=*/true)) {
        // borealis creates its global ThreadPool (3 workers) in a static
        // initializer, before main(). Returning to hbloader with those threads
        // still parked in the process crashed the title as soon as the next NRO
        // was mapped over their code ("closed because of an error", update
        // applied only on the launch after). Join them first.
        brls::ThreadPool::shutdownGlobal();
        util::logLine("main: exiting for update relaunch");
        util::logClose();   // drain the async writer before committing
        fsdevCommitDevice("sdmc");
        _exit(0);
    }
#endif

#ifdef __SWITCH__
    util::logLine("main: socketInit result=" + std::to_string(g_socket_init_result) +
                  " pool=" + std::to_string(g_socket_mem_size_used / 1024) + "KB" +
                  " sb_eff=" + std::to_string(g_socket_sb_efficiency) +
                  " applet_type=" + std::to_string(g_applet_type_detected));
    bool startupBoostActive = false;
    if (!checkAppletMode()) {
        util::cpuBoostBegin();
        startupBoostActive = true;
        util::logLine("main: startup CPU boost enabled (1785 MHz)");
    }
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
    } else if (userLang == "ar") {
        brls::Platform::APP_LOCALE_DEFAULT = brls::LOCALE_AR;
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

    // Snappy focus highlight and smooth scrolling transitions (down from sluggish 200ms default)
    brls::getStyle().addMetric("brls/animations/highlight", 100.0f);

    // Exit directly to Nintendo Switch Home Menu instead of Homebrew Menu (hbmenu)
    brls::Application::getPlatform()->exitToHomeMode(true);

    // Signal background tasks and probes to abort as soon as Borealis begins exiting
    // (before Application::exit() clears activities and waits on ThreadPool::shutdownGlobal()).
    brls::Application::getExitEvent()->subscribe([]() {
        g_appExiting.store(true);
        g_cleanupCancelled = true;
        datasource::CustomEngineClient::instance().cancelProbe();
    });

    brls::Application::createWindow("TorrentShopNX");

    // Ensure Arabic fallback font is registered with NVG if not already registered by font loader
    {
        NVGcontext* vg = brls::Application::getNVGContext();
        int baseFont = brls::Application::getFont(brls::FONT_REGULAR);
        int arFont = brls::Application::getFont("arabic");

        if (vg && baseFont != brls::FONT_INVALID && arFont == brls::FONT_INVALID) {
            std::vector<std::string> candidateArabicPaths = {
#ifdef __SWITCH__
                "romfs:/font/arabic_font.ttf",
#endif
                "sdmc:/switch/TorrentShopNX/font/arabic.ttf",
                "sdmc:/switch/TorrentShopNX/arabic.ttf",
#ifndef __SWITCH__
                "resources/font/arabic_font.ttf",
                "build_pc/resources/font/arabic_font.ttf"
#endif
            };

#if defined(_WIN32)
            const char* winDir = getenv("SystemRoot");
            std::string winFonts = (winDir ? std::string(winDir) : "C:\\Windows") + "\\Fonts\\";
            candidateArabicPaths.push_back(winFonts + "tahoma.ttf");
            candidateArabicPaths.push_back(winFonts + "arial.ttf");
            candidateArabicPaths.push_back(winFonts + "seguiui.ttf");
#endif

            for (const auto& path : candidateArabicPaths) {
                if (access(path.c_str(), F_OK) != -1) {
                    if (brls::Application::loadFontFromFile("arabic", path)) {
                        arFont = brls::Application::getFont("arabic");
                        if (arFont != brls::FONT_INVALID) {
                            util::logLine("main: loaded Arabic font from: " + path);
                            nvgAddFallbackFontId(vg, baseFont, arFont);
                            util::logLine("main: registered Arabic fallback font");
                            break;
                        }
                    }
                }
            }
        }
    }
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::LIGHT);
    brls::Application::setGlobalQuit(false);
    ui::ThemeManager::instance().init();
    brls::Application::setGlobalWallpaper(ui::ThemeManager::instance().getEffectiveWallpaperPath());
    ui::ThemeManager::instance().subscribe([]() {
        brls::Application::setGlobalWallpaper(ui::ThemeManager::instance().getEffectiveWallpaperPath());
    });
    brls::Application::registerXMLView("QrCodeView", ui::QrCodeView::create);

    // Global navigation haptic feedback: subtle, gentle Joy-Con tick on any focus change
    brls::Application::getGlobalFocusChangeEvent()->subscribe([](brls::View* view) {
        if (view && brls::Application::getInputType() == brls::InputType::GAMEPAD) {
            util::triggerHapticFeedback(1);
        }
    });

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
        catalog::GameStatsManager::instance().loadFromDiskAsync();
        ui::DownloadManager::instance().init();

        // Initialize curl first, so background network threads can safely use it.
    #if __has_include(<curl/curl.h>)
        curl_global_init(CURL_GLOBAL_ALL);
    #endif
    #ifdef __SWITCH__
        net::ImageDownloader::instance().init(2);
    #else
        net::ImageDownloader::instance().init(4);
    #endif

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

    // Global safety guard: ensure gamepad focus is automatically recovered whenever it drops to null
    brls::Application::getRunLoopEvent()->subscribe([]() {
        if (brls::Application::getCurrentFocus() == nullptr) {
            const auto& stack = brls::Application::getActivitiesStack();
            if (!stack.empty() && stack.back()) {
                brls::View* defFocus = stack.back()->getDefaultFocus();
                if (defFocus) {
                    brls::Application::giveFocus(defFocus);
                }
            }
        }
    });

    util::logLine("main: initializing ScreenSleepManager");
    util::ScreenSleepManager::instance().init();

    util::logLine("main: entering mainLoop");
    uiwd::start();
    // Execute Borealis main loop
    try {
        int frameCount = 0;
        while (brls::Application::mainLoop()) {
            frameCount++;
            perf::frame();
            uiwd::beat();
            if (frameCount <= 3 || frameCount == 10 || frameCount == 30 || frameCount == 60) {
                util::logLine("main: mainLoop frame=" + std::to_string(frameCount));
                if (frameCount == 60) {
                    util::logLine("main: initial startup stabilized (frame 60)");
#ifdef __SWITCH__
                    if (startupBoostActive) {
                        util::cpuBoostEnd();
                        startupBoostActive = false;
                        util::logLine("main: startup CPU boost returned to normal (1020 MHz)");
                    }
#endif
                }
            }
        }
#ifdef __SWITCH__
        if (startupBoostActive) {
            util::cpuBoostEnd();
            startupBoostActive = false;
        }
#endif
        util::logLine("main: mainLoop exited normally after " + std::to_string(frameCount) + " frames");
    } catch (const std::exception& e) {
#ifdef __SWITCH__
        if (startupBoostActive) {
            util::cpuBoostEnd();
            startupBoostActive = false;
        }
#endif
        util::logLine(std::string("main: EXCEPTION in mainLoop: ") + e.what());
    } catch (...) {
#ifdef __SWITCH__
        if (startupBoostActive) {
            util::cpuBoostEnd();
            startupBoostActive = false;
        }
#endif
        util::logLine("main: UNKNOWN EXCEPTION in mainLoop");
    }

    util::logLine("main: mainLoop exited, starting shutdown sequence");
    uiwd::stop();
    util::ScreenSleepManager::instance().shutdown();

    // Signal background tasks and network transfers to cancel immediately
    g_appExiting.store(true);
    g_cleanupCancelled = true;
    datasource::CustomEngineClient::instance().cancelProbe();

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

        util::logLine("main: calling CustomEngineClient::shutdown");
        datasource::CustomEngineClient::instance().shutdown();

        util::logLine("main: calling ImageDownloader::stop");
        net::ImageDownloader::instance().stop();
        
        util::logLine("main: all threads requested to stop");
    }

#ifdef __SWITCH__
    // Unmount RomFS now that UI and all threads have stopped.
    // This releases the file lock on g_nroPath so any pending update can be applied right now!
    util::unmountRomfs();

    // Apply pending update if one was downloaded during this session.
    // Only the file is replaced here; the process still exits to HOME (see
    // prepareExitAfterUpdate), the next launch runs the new version.
    if (checkAndApplyPendingUpdate(/*relaunch=*/false)) {
        util::logLine("main: pending update applied during shutdown");
    }
#endif

    // Do NOT call curl_global_cleanup() because it might crash if curl threads are alive

    // Use _exit(0) to exit cleanly:
    //   - Skips C++ atexit handlers / global destructors (prevents engine teardown crashes)
    //   - Calls __libnx_exit → __appExit → userAppExit (proper service teardown)
    //   - With exitToHomeMode(true), _appletCleanup() exits directly to the Switch Home Menu
    //     (or chainloads via envSetNextLoad when __nx_applet_exit_mode == 0 for updates).
#ifdef __SWITCH__
    util::logLine("main: closing log and exiting via _exit(0). Goodbye!");
    engine_log_flush();  // applet mode skips the engine shutdown: drain its log ring
    util::logClose();    // drains the async log writer; before __appExit calls fsExit
    fsdevCommitDevice("sdmc");
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
