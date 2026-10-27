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
#include "../catalog/game_stats_manager.h"
#include "../net/http_client.h"
#include <borealis/views/hint.hpp>
#include <borealis/extern/nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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
    current_font_scale_ = config::ConfigManager::instance().getFontScale();

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

        // 1. Setup content view
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

    // 3. Bottom 1/3 Summary Drawer (Compact 178px, lifted with margin)
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

    updateDashboardVisibility();
}

void MainMenu::updateDashboardVisibility() {
    bool show = config::ConfigManager::instance().getShowBottomDashboard();
    if (summaryView_) {
        summaryView_->setVisibility(show ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (tilesBox_) {
        if (show) {
            tilesBox_->setGrow(0.0f);
            tilesBox_->setMarginTop(30.0f);
            tilesBox_->setMarginBottom(18.0f);
        } else {
            tilesBox_->setGrow(1.0f);
            tilesBox_->setMarginTop(0.0f);
            tilesBox_->setMarginBottom(0.0f);
        }
    }
    if (rootBox_) {
        rootBox_->invalidate();
    }
    if (rootContainer_) {
        rootContainer_->invalidate();
    }
    if (!show && current_focused_index_ >= 0 && current_focused_index_ < static_cast<int>(tiles_.size())) {
        if (tiles_[current_focused_index_]) {
            brls::Application::giveFocus(tiles_[current_focused_index_]);
        }
    }
}

void MainMenu::onResume() {
    brls::Activity::onResume();
    updateDashboardVisibility();
    refreshDashboardState();
    for (auto* tile : tiles_) {
        if (tile) tile->refreshTheme();
    }
    if (header_) header_->refreshTheme();

    if (brls::Application::getCurrentFocus() == nullptr) {
        int idx = (current_focused_index_ >= 0 && current_focused_index_ < static_cast<int>(tiles_.size()))
            ? current_focused_index_ : 0;
        if (idx < static_cast<int>(tiles_.size()) && tiles_[idx]) {
            brls::Application::giveFocus(tiles_[idx]);
        }
    }
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

    updateDashboardVisibility();

    std::string newLocale = brls::Application::getLocale();
    bool localeChanged = (newLocale != current_locale_);
    current_locale_ = newLocale;

    int newFontScale = config::ConfigManager::instance().getFontScale();
    bool fontScaleChanged = (newFontScale != current_font_scale_);
    current_font_scale_ = newFontScale;

    // Update tile titles to reflect the currently active locale
    if (tiles_.size() >= 5) {
        tiles_[0]->setTitle("app/menu/catalog"_i18n);
        tiles_[1]->setTitle("app/menu/retro_games"_i18n);
        tiles_[2]->setTitle("app/menu/library"_i18n);
        tiles_[3]->setTitle("app/menu/downloads"_i18n);
        tiles_[4]->setTitle("app/menu/settings"_i18n);
    }

    if ((localeChanged || fontScaleChanged) && summaryView_) {
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
    if (summaryView_ && config::ConfigManager::instance().getShowBottomDashboard()) {
        onTileFocused(current_focused_index_);
    }
}

void MainMenu::onTileFocused(int index) {
    current_focused_index_ = index;
    if (tilesBox_ && index >= 0 && index < static_cast<int>(tiles_.size())) {
        tilesBox_->setDefaultFocusedIndex(index);
    }
    if (!config::ConfigManager::instance().getShowBottomDashboard()) {
        return;
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

                    // Patch title IDs of the installed games: the only rows of
                    // versions.txt and of the ncm database we care about.
                    std::unordered_set<uint64_t> wantedPatchTids;
                    for (uint64_t baseTid : baseTids) wantedPatchTids.insert(baseTid | 0x800ULL);

                    // Installed patch versions (patch tid -> highest version).
                    // One List call per storage instead of one
                    // GetLatestContentMetaKey per game: per-title queries flood
                    // ncm, which shares the system core with the compositor,
                    // and the UI thread then stalls in eglSwapBuffers (0.5 s
                    // [uistall] samples on hovering this tile). Both SD and NAND
                    // are read; the old code skipped NAND whenever SD opened.
                    std::unordered_map<uint64_t, uint32_t> installedPatchVersions;
                    if (!baseTids.empty()) {
                        std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
                        if (R_SUCCEEDED(ncmInitialize())) {
                            for (NcmStorageId sid : {NcmStorageId_SdCard, NcmStorageId_BuiltInUser}) {
                                NcmContentMetaDatabase db;
                                if (R_FAILED(ncmOpenContentMetaDatabase(&db, sid))) continue;
                                std::vector<NcmContentMetaKey> keys(256);
                                s32 total = 0, written = 0;
                                Result rc = ncmContentMetaDatabaseList(&db, &total, &written, keys.data(),
                                                                       static_cast<s32>(keys.size()), NcmContentMetaType_Patch,
                                                                       0, 0, UINT64_MAX, NcmContentInstallType_Full);
                                if (R_SUCCEEDED(rc) && total > written) {
                                    keys.resize(static_cast<size_t>(total));
                                    rc = ncmContentMetaDatabaseList(&db, &total, &written, keys.data(),
                                                                    static_cast<s32>(keys.size()), NcmContentMetaType_Patch,
                                                                    0, 0, UINT64_MAX, NcmContentInstallType_Full);
                                }
                                if (R_SUCCEEDED(rc)) {
                                    for (s32 i = 0; i < written; ++i) {
                                        const NcmContentMetaKey& k = keys[static_cast<size_t>(i)];
                                        if (!wantedPatchTids.count(k.id)) continue;
                                        uint32_t& v = installedPatchVersions[k.id];
                                        if (k.version > v) v = k.version;
                                    }
                                }
                                ncmContentMetaDatabaseClose(&db);
                            }
                            ncmExit();
                        }
                    }

                    // Latest versions from versions.txt (~3 MB, "tid|...|version"
                    // per line). Read in one go and scanned in place, keeping only
                    // the installed games: no per-line strings, so this thread
                    // doesn't hog the global malloc lock the UI thread needs too.
                    std::unordered_map<uint64_t, uint32_t> availableVersions;
                    if (!wantedPatchTids.empty()) {
                        std::string buf;
                        if (FILE* vf = std::fopen(TSNX_VERSIONS_PATH, "rb")) {
                            std::fseek(vf, 0, SEEK_END);
                            long len = std::ftell(vf);
                            std::fseek(vf, 0, SEEK_SET);
                            if (len > 0) {
                                buf.resize(static_cast<size_t>(len));
                                buf.resize(std::fread(&buf[0], 1, buf.size(), vf));
                            }
                            std::fclose(vf);
                        }
                        const char* p = buf.data();
                        const char* end = p + buf.size();
                        while (p < end) {
                            const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
                            if (!eol) eol = end;
                            char* tidEnd = nullptr;
                            uint64_t tid = std::strtoull(p, &tidEnd, 16);
                            if (tidEnd != p && tidEnd < eol && *tidEnd == '|' && wantedPatchTids.count(tid)) {
                                const char* lastPipe = eol;
                                while (lastPipe > tidEnd && *(lastPipe - 1) != '|') --lastPipe;
                                // lastPipe now points just after the last '|'
                                availableVersions[tid] = static_cast<uint32_t>(std::strtoul(lastPipe, nullptr, 10));
                            }
                            p = eol + 1;
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
                            auto iv = installedPatchVersions.find(patchTid);
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

namespace {
// Dashboard values that need a service call (ncm free space, nifm IP). They are
// refreshed on a borealis worker so the 1 s dashboard timer never makes the UI
// thread wait on ncm/nifm or on g_switch_service_mutex (measured: one dropped
// frame every second, on every screen, before this).
struct DashboardSysInfo {
    std::mutex mtx;
    bool have = false;
    std::string sd, nand, ip;
    std::atomic<bool> inFlight{false};
    std::chrono::steady_clock::time_point last{};
};
DashboardSysInfo g_dashInfo;

void refreshDashboardSysInfoAsync() {
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_dashInfo.mtx);
        if (g_dashInfo.have && now - g_dashInfo.last < std::chrono::seconds(5)) return;
    }
    if (g_dashInfo.inFlight.exchange(true)) return;
    brls::async([]() {
        int64_t sdFree = 0, sdTotal = 0, nandFree = 0, nandTotal = 0;
        bool sdOk = util::getStorageStats(1, sdFree, sdTotal);
        bool nandOk = util::getStorageStats(0, nandFree, nandTotal);
        std::string ip = ui::RemoteAddView::getLocalIpAddress();
        {
            std::lock_guard<std::mutex> lock(g_dashInfo.mtx);
            g_dashInfo.sd = sdOk
                ? formatBytesLocal(static_cast<unsigned long long>(sdFree)) + " / " + formatBytesLocal(static_cast<unsigned long long>(sdTotal))
                : "-";
            g_dashInfo.nand = nandOk
                ? formatBytesLocal(static_cast<unsigned long long>(nandFree)) + " / " + formatBytesLocal(static_cast<unsigned long long>(nandTotal))
                : "-";
            g_dashInfo.ip = ip;
            g_dashInfo.have = true;
            g_dashInfo.last = std::chrono::steady_clock::now();
        }
        g_dashInfo.inFlight.store(false);
    });
}
} // namespace

void MainMenu::refreshDashboardState() {
    // The timer keeps firing while other screens are on top: skip the work then.
    const auto& stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && stack.back() != this) return;

    refreshDashboardSysInfoAsync();

    // 1. Active downloads: a snapshot taken under the queue lock (the progress
    // thread mutates the queue). try_lock: never stall a frame for it.
    auto& dlMgr = ui::DownloadManager::instance();
    std::vector<download::DownloadItem> items;
    {
        std::unique_lock<std::recursive_mutex> lock(dlMgr.getImpl().queueMutex(), std::try_to_lock);
        if (!lock.owns_lock()) return;   // next tick
        items = dlMgr.getImpl().queue();
    }

    int activeCount = 0;
    for (const auto& it : items) {
        if (it.state == download::DownloadState::Downloading || 
            it.state == download::DownloadState::StreamPreparing ||
            it.state == download::DownloadState::StreamInstalling ||
            it.state == download::DownloadState::Installing) {
            activeCount++;
        }
    }

    // 2. Storage Stats (SD & NAND) and IP: last values from the worker
    std::string sdStr = "...", nandStr = "...", ipStr;
    {
        std::lock_guard<std::mutex> lock(g_dashInfo.mtx);
        if (g_dashInfo.have) {
            sdStr = g_dashInfo.sd;
            nandStr = g_dashInfo.nand;
            ipStr = g_dashInfo.ip;
        }
    }

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

    if (summaryView_ && config::ConfigManager::instance().getShowBottomDashboard()) {
        summaryView_->updateDownloads(items);
        if (!catalog->empty()) {
            summaryView_->setCatalogSample(*catalog);
        }
        if (!ipStr.empty()) summaryView_->setRemoteInfo(ipStr, 8080);
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

    // Background cache cleanup (iterative folder deletion and stale cache sweep)
    g_cleanupRunning = true;
    brls::async([]() {
        try {
            if (!g_pathsToDelete.empty()) {
                for (const auto& path : g_pathsToDelete) {
                    if (g_cleanupCancelled.load()) break;
                    deleteDirectoryIterative(path);
                }
                g_pathsToDelete.clear();
            }

            // Delay stale cache sweep by 15s to allow smooth initial UI & thumbnail loading without SD contention
            for (int s = 0; s < 30; ++s) {
                if (g_cleanupCancelled.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (!g_cleanupCancelled.load()) {
                sweepStaleCaches();
            }
        } catch (const std::exception& e) {
            util::logLine(std::string("MainMenu: exception in background cleanup: ") + e.what());
        } catch (...) {
            util::logLine("MainMenu: unknown exception in background cleanup");
        }
        g_cleanupRunning = false;
    });
}
