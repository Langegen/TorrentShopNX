#include "MainMenu.hpp"
#include "DownloadUiManager.hpp"
#include "CatalogView.hpp"
#include "CollectionsView.hpp"
#include "LibraryView.hpp"
#include "DownloadsView.hpp"
#include "SettingsTab.hpp"
#include "RemoteAddView.hpp"
#include "RetroConsolesView.hpp"
#include "FileManagerView.hpp"
#include "CatalogProgressNotification.hpp"
#include "FavoritesManager.hpp"
#include "../GameData.hpp"
#include "../config/config.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include "../utils/app_paths.h"
#include "../utils/switch_utils.h"
#include "../utils/storage_utils.h"
#include "../catalog/catalog_manager.h"
#include "../catalog/catalog_updater.h"
#include "../catalog/IgnoredUpdatesManager.hpp"
#include "../net/http_client.h"
#include <borealis/views/hint.hpp>
#include <borealis/extern/nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>

using namespace brls::literals;

extern std::vector<Game> g_games;
extern std::recursive_mutex g_switch_service_mutex;

static std::string formatBytesLocal(unsigned long long bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    return std::string(buf);
}

MainMenu::MainMenu() {
    util::logLine("MainMenu: constructor start (Modern Dashboard)");
    current_locale_ = brls::Application::getLocale();

    refreshTimer_ = new brls::RepeatingTimer();
    refreshTimer_->setPeriod(1000);
    refreshTimer_->setCallback([this]() {
        refreshDashboardState();
    });
    refreshTimer_->start();

    util::logLine("MainMenu: constructor end");
}

MainMenu::~MainMenu() {
    if (refreshTimer_) {
        refreshTimer_->stop();
        delete refreshTimer_;
        refreshTimer_ = nullptr;
    }
}

brls::View* MainMenu::createContentView() {
    if (!rootContainer_) {
        rootContainer_ = new brls::Box();
        rootContainer_->setWidthPercentage(100.0f);
        rootContainer_->setHeightPercentage(100.0f);

        // 1. Fullscreen Wallpaper covering entire 1280x720 window
        bgImage_ = new brls::Image();
        bgImage_->setPositionType(brls::PositionType::ABSOLUTE);
        bgImage_->setPositionTop(0.0f);
        bgImage_->setPositionLeft(0.0f);
        bgImage_->setWidthPercentage(100.0f);
        bgImage_->setHeightPercentage(100.0f);
        bgImage_->setScalingType(brls::ImageScalingType::FILL);
        bgImage_->setImageFromFile(std::string(BRLS_RESOURCES) + "img/dashboard_bg.jpg");
        rootContainer_->addView(bgImage_);

        // 2. Setup content view
        setupLayout();

        // 3. AppletFrame with transparent background over the wallpaper
        appletFrame_ = new brls::AppletFrame(rootBox_);
        appletFrame_->setHeaderVisibility(brls::Visibility::GONE);
        appletFrame_->setFooterVisibility(brls::Visibility::VISIBLE);
        appletFrame_->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        if (appletFrame_->getFooter()) {
            appletFrame_->getFooter()->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        }
        appletFrame_->setWidthPercentage(100.0f);
        appletFrame_->setHeightPercentage(100.0f);
        rootContainer_->addView(appletFrame_);
    }
    return rootContainer_;
}

void MainMenu::setupLayout() {
    rootBox_ = new brls::Box();
    rootBox_->setWidthPercentage(100.0f);
    rootBox_->setHeightPercentage(100.0f);
    rootBox_->setAxis(brls::Axis::COLUMN);
    rootBox_->setAlignItems(brls::AlignItems::CENTER);
    rootBox_->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);

    // 1. Top Header Bar
    header_ = new ui::DashboardHeader();
    header_->setOnFileManagerClicked([this]() {
        openFileManager();
    });
    rootBox_->addView(header_);

    // 2. Center 5x Dashboard Tiles Row (Lowered by 30px)
    tilesBox_ = new brls::Box();
    tilesBox_->setAxis(brls::Axis::ROW);
    tilesBox_->setJustifyContent(brls::JustifyContent::CENTER);
    tilesBox_->setAlignItems(brls::AlignItems::CENTER);
    tilesBox_->setWidthPercentage(100.0f);
    tilesBox_->setMarginTop(30.0f);
    tilesBox_->setMarginBottom(18.0f);

    struct TileDef {
        int index;
        std::string icon;
        std::string title_key;
        std::string title_fallback;
    };

    TileDef defs[5] = {
        {0, "img/tile_catalog.png",   "app/menu/catalog",     "Catalog"},
        {1, "img/tile_retro.png",     "app/menu/retro_games", "Retro Games"},
        {2, "img/tile_library.png",   "app/menu/library",     "Library"},
        {3, "img/tile_downloads.png", "app/menu/downloads",   "Downloads"},
        {4, "img/tile_tools.png",     "app/menu/settings",    "Settings"}
    };

    for (int i = 0; i < 5; ++i) {
        std::string title = brls::getStr(defs[i].title_key);
        if (title.empty() || title == defs[i].title_key) title = defs[i].title_fallback;

        ui::DashboardTile* tile = new ui::DashboardTile(
            defs[i].index,
            defs[i].icon,
            title,
            [this](int idx) { onTileFocused(idx); },
            [this](int idx) { onTileClicked(idx); }
        );

        tile->registerAction("app/menu/remote_add"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
            brls::Application::pushActivity(new ui::RemoteAddView());
            return true;
        });
        tile->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_MINUS), [this](brls::View* view) {
            brls::Application::pushActivity(new ui::RemoteAddView());
            return true;
        });

        tile->registerAction("app/file_manager/title"_i18n, brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
            openFileManager();
            return true;
        });
        tile->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_EQUAL), [this](brls::View* view) {
            openFileManager();
            return true;
        });
        tile->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_KP_ADD), [this](brls::View* view) {
            openFileManager();
            return true;
        });
        tile->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_F1), [this](brls::View* view) {
            openFileManager();
            return true;
        });
        tile->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_TAB), [this](brls::View* view) {
            openFileManager();
            return true;
        });
        tile->registerAction("app/actions/refresh"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
            refreshDashboardState();
            return true;
        }, true);

        if (i < 4) {
            tile->setMarginRight(16.0f);
        }

        tiles_.push_back(tile);
        tilesBox_->addView(tile);
    }
    if (!tiles_.empty()) {
        tilesBox_->setDefaultFocusedIndex(0);
    }
    rootBox_->addView(tilesBox_);
    rootBox_->setDefaultFocusedIndex(1);

    // 3. Bottom 1/3 Summary Drawer (Compact 175px, lifted with margin)
    summaryView_ = new ui::DashboardSummaryView();
    summaryView_->setMarginBottom(18.0f);
    summaryView_->setOnOpenSectionCallback([this](int idx) {
        onTileClicked(idx);
    });
    summaryView_->setOnDefocusCallback([this]() {
        if (current_focused_index_ >= 0 && current_focused_index_ < static_cast<int>(tiles_.size()) && tiles_[current_focused_index_]) {
            brls::Application::giveFocus(tiles_[current_focused_index_]);
        }
    });
    summaryView_->setGetActiveTileCallback([this]() -> brls::View* {
        if (current_focused_index_ >= 0 && current_focused_index_ < static_cast<int>(tiles_.size())) {
            return tiles_[current_focused_index_];
        }
        return nullptr;
    });
    summaryView_->registerAction("app/file_manager/title"_i18n, brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
        openFileManager();
        return true;
    });
    summaryView_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_EQUAL), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    summaryView_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_KP_ADD), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    summaryView_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_F1), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    summaryView_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_TAB), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    summaryView_->registerAction("app/menu/remote_add"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        brls::Application::pushActivity(new ui::RemoteAddView());
        return true;
    });
    summaryView_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_MINUS), [this](brls::View* view) {
        brls::Application::pushActivity(new ui::RemoteAddView());
        return true;
    });
    summaryView_->registerAction("app/actions/refresh"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        refreshDashboardState();
        return true;
    }, true);
    rootBox_->addView(summaryView_);

    rootBox_->registerAction("app/menu/remote_add"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        brls::Application::pushActivity(new ui::RemoteAddView());
        return true;
    });
    rootBox_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_MINUS), [this](brls::View* view) {
        brls::Application::pushActivity(new ui::RemoteAddView());
        return true;
    });
    rootBox_->registerAction("app/file_manager/title"_i18n, brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
        openFileManager();
        return true;
    });
    rootBox_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_EQUAL), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    rootBox_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_KP_ADD), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    rootBox_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_F1), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    rootBox_->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_TAB), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    rootBox_->registerAction("app/actions/refresh"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        refreshDashboardState();
        return true;
    }, true);
}

static int s_cachedInstalledCount = 0;
static int s_cachedUpdatesCount = 0;
static bool s_installedCountCalculated = false;
static std::atomic<bool> s_calculatingInstalledCount{false};

static uint64_t s_totalCacheSize = 0;
static uint64_t s_totalLeftoverSize = 0;
static bool s_settingsStatsCalculated = false;
static std::atomic<bool> s_calculatingSettingsStats{false};

void MainMenu::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);

    std::string newLocale = brls::Application::getLocale();
    bool localeChanged = (newLocale != current_locale_);
    current_locale_ = newLocale;

    // Update tile titles to reflect the currently active locale
    if (tiles_.size() >= 5) {
        tiles_[0]->setTitle("app/menu/catalog"_i18n);
        tiles_[1]->setTitle("app/menu/retro_games"_i18n);
        tiles_[2]->setTitle("app/menu/library"_i18n);
        tiles_[3]->setTitle("app/menu/downloads"_i18n);
        tiles_[4]->setTitle("app/menu/settings"_i18n);
    }

    if (localeChanged && summaryView_) {
        summaryView_->refreshLocale();
    }

    s_installedCountCalculated = false;
    s_settingsStatsCalculated = false;
    refreshDashboardState();
    if (resetState) {
        current_focused_index_ = 0;
        if (!tiles_.empty() && tiles_[0]) {
            brls::Application::giveFocus(tiles_[0]);
        }
    }
    if (summaryView_) {
        onTileFocused(current_focused_index_);
    }
}

void MainMenu::onTileFocused(int index) {
    current_focused_index_ = index;
    if (tilesBox_ && index >= 0 && index < static_cast<int>(tiles_.size())) {
        tilesBox_->setDefaultFocusedIndex(index);
    }
    if (summaryView_) {
        if (index == 2) {
            // Instantly display cached installed count and updates count (0ms, 60 FPS)
            summaryView_->setLibraryStats(s_cachedInstalledCount, s_cachedUpdatesCount);

            // Asynchronously query installed count and available updates in background
            if (!s_installedCountCalculated && !s_calculatingInstalledCount.exchange(true)) {
                brls::async([this]() {
                    int installedCount = 0;
                    int updatesCount = 0;
                    std::vector<uint64_t> baseTids;
#ifdef __SWITCH__
                    {
                        std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
                        if (R_SUCCEEDED(nsInitialize())) {
                            s32 offset = 0;
                            s32 entry_count = 0;
                            do {
                                std::vector<NsApplicationRecord> batch(50);
                                if (R_FAILED(nsListApplicationRecord(batch.data(), 50, offset, &entry_count))) break;
                                for (s32 i = 0; i < entry_count; ++i) {
                                    baseTids.push_back(batch[i].application_id);
                                }
                                installedCount += entry_count;
                                offset += entry_count;
                            } while (entry_count == 50);
                            nsExit();
                        }
                    }

                    // Parse available versions database from versions.txt
                    std::unordered_map<uint64_t, uint32_t> availableVersions;
                    std::ifstream in(TSNX_VERSIONS_PATH);
                    if (in.is_open()) {
                        std::string line;
                        while (std::getline(in, line)) {
                            if (line.empty()) continue;
                            size_t firstPipe = line.find('|');
                            size_t lastPipe = line.rfind('|');
                            if (firstPipe != std::string::npos && lastPipe != std::string::npos) {
                                std::string tidStr = line.substr(0, firstPipe);
                                std::string verStr = line.substr(lastPipe + 1);
                                while (!verStr.empty() && (verStr.back() == '\r' || std::isspace(static_cast<unsigned char>(verStr.back())))) {
                                    verStr.pop_back();
                                }
                                try {
                                    uint64_t tid = std::stoull(tidStr, nullptr, 16);
                                    uint32_t ver = static_cast<uint32_t>(std::stoul(verStr));
                                    availableVersions[tid] = ver;
                                } catch (...) {}
                            }
                        }
                    }

                    // Query installed patch versions using ncm
                    std::unordered_map<uint64_t, uint32_t> installedPatchVersions;
                    if (!baseTids.empty()) {
                        std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
                        Result rc = ncmInitialize();
                        if (R_SUCCEEDED(rc)) {
                            NcmContentMetaDatabase db;
                            rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
                            if (R_FAILED(rc)) rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_BuiltInUser);
                            if (R_SUCCEEDED(rc)) {
                                for (uint64_t baseTid : baseTids) {
                                    uint64_t patchTid = baseTid | 0x800ULL;
                                    NcmContentMetaKey key;
                                    if (R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, patchTid))) {
                                        installedPatchVersions[baseTid] = key.version;
                                    }
                                }
                                ncmContentMetaDatabaseClose(&db);
                            }
                            ncmExit();
                        }
                    }

                    // Count updates
                    for (uint64_t baseTid : baseTids) {
                        if (catalog::IgnoredUpdatesManager::instance().isIgnored(baseTid)) {
                            continue;
                        }
                        uint64_t patchTid = baseTid | 0x800ULL;
                        auto it = availableVersions.find(patchTid);
                        if (it != availableVersions.end() && it->second > 0) {
                            uint32_t latestVer = it->second;
                            uint32_t currentVer = 0;
                            auto iv = installedPatchVersions.find(baseTid);
                            if (iv != installedPatchVersions.end()) currentVer = iv->second;
                            if (latestVer > currentVer) {
                                updatesCount++;
                            }
                        }
                    }
#else
                    installedCount = 3;
                    updatesCount = 0;
                    std::vector<uint64_t> mockTids = {0x0100000000010000ULL, 0x01007ef00011e000ULL, 0x0100000000020000ULL};
                    for (uint64_t tid : mockTids) {
                        if (tid != 0x0100000000020000ULL && !catalog::IgnoredUpdatesManager::instance().isIgnored(tid)) {
                            updatesCount++;
                        }
                    }
#endif
                    s_cachedInstalledCount = installedCount;
                    s_cachedUpdatesCount = updatesCount;
                    s_installedCountCalculated = true;
                    s_calculatingInstalledCount = false;

                    brls::sync([this, installedCount, updatesCount]() {
                        if (summaryView_) {
                            summaryView_->setLibraryStats(installedCount, updatesCount);
                        }
                    });
                });
            }
        } else if (index == 4) {
            auto& cfg = config::ConfigManager::instance();
            std::string modeStr = (cfg.getDataMode() == "local_client") ? "Custom Engine" : "TorrServer";

            // Instantly display cached total cache size (0ms, 60 FPS, no freezing)
            summaryView_->setSettingsStats(modeStr, s_totalCacheSize, s_totalLeftoverSize);

            // Asynchronously compute total application cache (sum of all caches) and leftovers in background
            if (!s_settingsStatsCalculated && !s_calculatingSettingsStats.exchange(true)) {
                brls::async([this, modeStr]() {
                    uint64_t totalCache = util::dirSizeRecursive(TSNX_CACHE_DIR) + util::pathSize(TSNX_VERSIONS_PATH);
                    uint64_t tempSize = util::dirSizeRecursive(TSNX_CACHE_STREAM) +
                                        util::dirSizeRecursive(TSNX_CACHE_TMP) +
                                        util::dirSizeRecursive(TSNX_CACHE_LOCALENGINE);
                    int phCountSd = 0, phCountNand = 0;
                    int64_t phSizeSd = 0, phSizeNand = 0;
                    util::getLeftoverPlaceholders(1, phCountSd, phSizeSd);
                    util::getLeftoverPlaceholders(0, phCountNand, phSizeNand);
                    uint64_t placeholderSize = (phSizeSd > 0 ? static_cast<uint64_t>(phSizeSd) : 0) +
                                               (phSizeNand > 0 ? static_cast<uint64_t>(phSizeNand) : 0);
                    uint64_t totalLeftover = placeholderSize + tempSize;

                    s_totalCacheSize = totalCache;
                    s_totalLeftoverSize = totalLeftover;
                    s_settingsStatsCalculated = true;
                    s_calculatingSettingsStats = false;

                    brls::sync([this, modeStr, totalCache, totalLeftover]() {
                        if (summaryView_) {
                            summaryView_->setSettingsStats(modeStr, totalCache, totalLeftover);
                        }
                    });
                });
            }
        }
        summaryView_->setFocusedIndex(index);
    }
}

void MainMenu::onTileClicked(int index) {
    switch (index) {
        case 0:
            brls::Application::pushActivity(new ui::CollectionsView());
            break;
        case 1:
            brls::Application::pushActivity(new ui::RetroConsolesView());
            break;
        case 2:
            brls::Application::pushActivity(new ui::LibraryView());
            break;
        case 3:
            brls::Application::pushActivity(new ui::DownloadsView());
            break;
        case 4:
            brls::Application::pushActivity(new ui::SettingsTab());
            break;
        default:
            break;
    }
}

void MainMenu::updateDownloadsBadge(int count) {
    if (tiles_.size() > 3 && tiles_[3]) {
        tiles_[3]->setBadge(count > 0 ? std::to_string(count) : "");
    }
}

void MainMenu::refreshDashboardState() {
    // 1. Calculate active downloads count (fast, in-memory)
    auto& dlMgr = ui::DownloadManager::instance();
    const auto& items = dlMgr.getImpl().queue();

    int activeCount = 0;
    for (const auto& it : items) {
        if (it.state == download::DownloadState::Downloading || 
            it.state == download::DownloadState::StreamPreparing ||
            it.state == download::DownloadState::StreamInstalling ||
            it.state == download::DownloadState::Installing) {
            activeCount++;
        }
    }

    // 2. Storage Stats (SD & NAND)
    int64_t sdFree = 0, sdTotal = 0;
    bool sdOk = util::getStorageStats(1, sdFree, sdTotal);
    std::string sdStr = sdOk
        ? (formatBytesLocal(static_cast<unsigned long long>(sdFree)) + " / " + formatBytesLocal(static_cast<unsigned long long>(sdTotal)))
        : "45.2 GB / 128.0 GB";

    int64_t nandFree = 0, nandTotal = 0;
    bool nandOk = util::getStorageStats(0, nandFree, nandTotal);
    std::string nandStr = nandOk
        ? (formatBytesLocal(static_cast<unsigned long long>(nandFree)) + " / " + formatBytesLocal(static_cast<unsigned long long>(nandTotal)))
        : "18.5 GB / 32.0 GB";

    // 3. Catalog Count & Update Timestamp (fast in-memory, avoid synchronous SD card stat every second)
    auto catalog = getCatalogSnapshot();
    int gameCount = static_cast<int>(catalog->size());
    if (gameCount == 0) gameCount = 7087;

    std::string dateStr = "28.08.2026";
    std::string lastDate = config::ConfigManager::instance().getLastCatalogUpdateDate();
    if (!lastDate.empty()) {
        if (lastDate.size() == 10 && lastDate[4] == '-' && lastDate[7] == '-') {
            dateStr = lastDate.substr(8, 2) + "." + lastDate.substr(5, 2) + "." + lastDate.substr(0, 4);
        } else {
            dateStr = lastDate;
        }
    }

    if (header_) {
        header_->updateStats(gameCount, dateStr, sdStr, nandStr);
    }

    updateDownloadsBadge(activeCount);

    if (summaryView_) {
        summaryView_->updateDownloads(items);
        if (!catalog->empty()) {
            summaryView_->setCatalogSample(*catalog);
        }
        summaryView_->setRemoteInfo(ui::RemoteAddView::getLocalIpAddress(), 8080);
    }
}

void MainMenu::openFileManager() {
    util::logLine("MainMenu: openFileManager triggered");
    try {
        brls::Application::pushActivity(new ui::FileManagerView());
        util::logLine("MainMenu: FileManagerView pushed successfully");
    } catch (const std::exception& e) {
        util::logLine(std::string("MainMenu: EXCEPTION opening FileManager: ") + e.what());
    } catch (...) {
        util::logLine("MainMenu: UNKNOWN EXCEPTION opening FileManager");
    }
}

void MainMenu::onContentAvailable() {
    util::logLine("MainMenu: onContentAvailable start");
    refreshDashboardState();

    if (!tiles_.empty() && tiles_[0]) {
        current_focused_index_ = 0;
        brls::Application::giveFocus(tiles_[0]);
    }

    this->registerAction("app/file_manager/title"_i18n, brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
        openFileManager();
        return true;
    });

    this->registerAction("app/actions/refresh"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        refreshDashboardState();
        return true;
    }, true);

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_EQUAL), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_KP_ADD), [this](brls::View* view) {
        openFileManager();
        return true;
    });
    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_F1), [this](brls::View* view) {
        openFileManager();
        return true;
    });

    // Trigger scheduled catalog update (48h full or 4h diff)
    catalog::CatalogUpdater::instance().checkAndRunScheduledUpdate();

    // Trigger asynchronous app update check if enabled and due
    auto& cfg = config::ConfigManager::instance();
    if (cfg.getAutoAppUpdate() && cfg.shouldCheckAppUpdateToday()) {
        std::string app_update_url = cfg.getEffectiveAppUpdateUrl();
        brls::async([app_update_url]() {
            util::logLine("main_menu: background app update check started from " + app_update_url);
            net::HttpClient http;
            auto res = http.httpGet(app_update_url);
            if (res.status_code == 200 && !res.body.empty()) {
                try {
                    auto j = nlohmann::json::parse(res.body);
                    std::string version;
                    std::string url;
                    
                    if (j.contains("tag_name")) {
                        version = j.value("tag_name", "");
                        if (j.contains("assets") && j["assets"].is_array()) {
                            for (const auto& asset : j["assets"]) {
                                std::string assetName = asset.value("name", "");
                                if (assetName.size() >= 4 && assetName.rfind(".nro") == assetName.size() - 4) {
                                    url = asset.value("browser_download_url", "");
                                    break;
                                }
                            }
                        }
                    } else {
                        version = j.value("version", "");
                        url = j.value("url", "");
                    }
                    
                    std::string currentVersion = config::ConfigManager::APP_VERSION;
                    std::string cleanVersion = version;
                    if (!cleanVersion.empty() && (cleanVersion[0] == 'v' || cleanVersion[0] == 'V')) cleanVersion = cleanVersion.substr(1);
                    std::string cleanCurrent = currentVersion;
                    if (!cleanCurrent.empty() && (cleanCurrent[0] == 'v' || cleanCurrent[0] == 'V')) cleanCurrent = cleanCurrent.substr(1);
                    
                    if (!version.empty() && !url.empty() && util::compareSemver(cleanVersion, cleanCurrent) > 0) {
                        brls::sync([url, version]() {
                            std::string msg = brls::getStr("app/settings/app_update_prompt", version);
                            brls::Dialog* dialog = new brls::Dialog(msg);
                            dialog->addButton("app/common/yes"_i18n, [url, version]() {
                                ui::downloadAndInstallAppUpdate(url, version);
                            });
                            dialog->addButton("app/common/no"_i18n, []() {});
                            dialog->open();
                        });
                    }
                } catch (const std::exception& e) {
                    util::logLine("main_menu: background app update parse error: " + std::string(e.what()));
                }
            } else {
                util::logLine("main_menu: background app update failed, status=" + std::to_string(res.status_code));
            }
            
            brls::sync([]() {
                auto& main_cfg = config::ConfigManager::instance();
                main_cfg.setLastAppUpdateCheckDate(config::ConfigManager::currentDateString());
            });
        });
    }

    // Delete renamed cache folders iteratively
    if (!g_pathsToDelete.empty()) {
        g_cleanupRunning = true;
        brls::async([]() {
            for (const auto& path : g_pathsToDelete) {
                if (g_cleanupCancelled.load()) break;
                deleteDirectoryIterative(path);
            }
            g_pathsToDelete.clear();
            g_cleanupRunning = false;
        });
    }
}
