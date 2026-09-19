#include "FileSelectView.hpp"
#include "DownloadUiManager.hpp"
#include "MainMenu.hpp"
#include "DownloadsView.hpp"
#include "../datasource/custom_engine_client.h"
#include "../config/config.h"
#include "../catalog/retro_catalog_manager.h"
#include "../utils/switch_utils.h"
#include "../net/image_downloader.h"
#include <iomanip>
#include <algorithm>
#include <cctype>

#include <mutex>

extern std::recursive_mutex g_switch_service_mutex;

namespace ui {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string formatBytes(unsigned long long bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    return std::string(buf);
}

static bool isRomFile(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::vector<std::string> romExts = {
        ".zip", ".7z", ".rar", ".iso", ".chd", ".cso", ".bin", ".cue", ".img",
        ".nes", ".fds", ".unf", ".smc", ".sfc", ".fig", ".swc", ".z64", ".n64", ".v64",
        ".gba", ".gbc", ".gb", ".nds", ".dsi", ".3ds", ".3dsx", ".cia",
        ".gcm", ".gcz", ".wbfs", ".wad", ".rpx", ".wud", ".wux",
        ".pbp", ".vpk", ".smd", ".gen", ".md", ".gg", ".sg", ".sms", ".cdi", ".gdi"
    };
    for (const auto& ext : romExts) {
        if (lower.size() >= ext.size() && lower.rfind(ext) == lower.size() - ext.size()) {
            return true;
        }
    }
    return false;
}

static bool isSwitchGameFile(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 4 &&
           (lower.rfind(".nsp") == lower.size() - 4 ||
            lower.rfind(".nsz") == lower.size() - 4 ||
            lower.rfind(".xci") == lower.size() - 4 ||
            lower.rfind(".xcz") == lower.size() - 4);
}
#ifdef __SWITCH__
#include <switch.h>

struct SwitchServiceGuard {
    bool ns_ok = false;
    bool ncm_ok = false;
    NcmContentMetaDatabase db;
    NcmStorageId db_storage = NcmStorageId_SdCard;
    bool db_open = false;

    SwitchServiceGuard() {
        g_switch_service_mutex.lock();
        ns_ok = R_SUCCEEDED(nsInitialize());
        ncm_ok = R_SUCCEEDED(ncmInitialize());
        if (ncm_ok) {
            Result rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
            db_storage = NcmStorageId_SdCard;
            if (R_FAILED(rc)) {
                rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_BuiltInUser);
                db_storage = NcmStorageId_BuiltInUser;
            }
            db_open = R_SUCCEEDED(rc);
        }
    }

    ~SwitchServiceGuard() {
        if (db_open) {
            ncmContentMetaDatabaseClose(&db);
        }
        if (ncm_ok) {
            ncmExit();
        }
        if (ns_ok) {
            nsExit();
        }
        g_switch_service_mutex.unlock();
    }
};

// Returns true if any of the real content (Program/Data) referenced by the
// content meta is actually present in the ContentStorage. Deleting a game via
// the Switch home menu can leave a stale content meta / record behind, so a
// bare meta lookup (or application record) is NOT enough — we must verify the
// content file still exists on disk.
static bool contentPresentForMeta(NcmContentMetaDatabase& db, NcmContentStorage& cs, const NcmContentMetaKey& key) {
    NcmContentType types[] = { NcmContentType_Program, NcmContentType_Data };
    for (NcmContentType ct : types) {
        NcmContentId cid;
        if (R_FAILED(ncmContentMetaDatabaseGetContentIdByType(&db, &cid, &key, ct))) continue;
        bool has = false;
        if (R_SUCCEEDED(ncmContentStorageHas(&cs, &has, &cid)) && has) return true;
    }
    return false;
}

static bool titleHasContentOnStorage(NcmContentMetaDatabase& db, NcmStorageId storage, uint64_t tid) {
    NcmContentMetaKey key;
    if (R_FAILED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, tid))) return false;

    NcmContentStorage cs;
    if (R_FAILED(ncmOpenContentStorage(&cs, storage))) return false;
    bool present = contentPresentForMeta(db, cs, key);
    ncmContentStorageClose(&cs);
    return present;
}

static bool isTitleIdInstalled(uint64_t tid, SwitchServiceGuard& guard) {
    if (tid == 0) return false;

    // 1) The storage already opened by the guard (fast path)
    if (guard.db_open && titleHasContentOnStorage(guard.db, guard.db_storage, tid)) return true;

    // 2) The other storage (game may live on the other medium)
    NcmStorageId other = (guard.db_storage == NcmStorageId_SdCard) ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
    NcmContentMetaDatabase db;
    if (R_SUCCEEDED(ncmOpenContentMetaDatabase(&db, other))) {
        bool found = titleHasContentOnStorage(db, other, tid);
        ncmContentMetaDatabaseClose(&db);
        if (found) return true;
    }
    return false;
}
#else
struct SwitchServiceGuard {};
static bool isTitleIdInstalled(uint64_t tid, SwitchServiceGuard& guard) {
    (void)guard;
    if (tid == 0x01000BF0152FB131ULL) return true; // DLC Left 4 Dead Bundle (mock)
    return false;
}
#endif

static uint64_t parseTitleIdFromFilename(const std::string& name) {
    size_t start = name.find('[');
    while (start != std::string::npos) {
        size_t end = name.find(']', start);
        if (end != std::string::npos && (end - start) == 17) {
            std::string tid_str = name.substr(start + 1, 16);
            try {
                return std::stoull(tid_str, nullptr, 16);
            } catch (...) {}
        }
        start = name.find('[', start + 1);
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// FileSelectView
// ─────────────────────────────────────────────────────────────────────────────

std::atomic<bool> g_file_select_view_active{false};

FileSelectView::FileSelectView(const Game& game, const std::string& retro_console_id)
    : game_(game),
      retro_console_id_(retro_console_id),
      alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
    g_file_select_view_active = true;
    // Background cover downloads compete for BSD sockets/sessions with the
    // custom engine probe. Pause them while this view is open.
    net::ImageDownloader::instance().pause();
}

FileSelectView::~FileSelectView() {
    // Signal both async threads that this object is being destroyed.
    // They must NOT touch any member after this flag is false.
    alive_flag_->store(false);
    g_file_select_view_active = false;
    net::ImageDownloader::instance().resume();
    // Abort a still-running probe and drop its torrent (unless a download has
    // adopted it) so the engine does not keep it around forever.
    datasource::CustomEngineClient::instance().cancelProbe();
    datasource::CustomEngineClient::instance().releaseProbeTorrent();
}

void FileSelectView::onContentAvailable() {
    util::logLine("FileSelectView: onContentAvailable start");
    title->setText(cleanTitle(game_.title));
    subtitle->setText("app/fileselect/subtitle"_i18n);

    // Configure install location selector
    auto& cfg = config::ConfigManager::instance();
    if (!retro_console_id_.empty()) {
        if (freeSpaceNandText) freeSpaceNandText->setVisibility(brls::Visibility::GONE);
        if (installLocationText) {
            std::string sub = retro_console_id_;
            const auto* cInfo = catalog::RetroCatalogManager::instance().getConsole(retro_console_id_);
            if (cInfo && !cInfo->default_rom_subfolder.empty()) sub = cInfo->default_rom_subfolder;
            installLocationText->setText(cfg.getEffectiveRetroRomsDir(sub));
            installLocationText->setTextColor(nvgRGB(52, 152, 219));
        }
    } else {
        auto updateInstallLocationDisplay = [this, &cfg]() {
            if (!installLocationText) return;
            std::string loc = cfg.getInstallLocation();
            if (loc == "sd") {
                installLocationText->setText("app/fileselect/loc_sd"_i18n);
                installLocationText->setTextColor(nvgRGB(46, 204, 113)); // green
            } else if (loc == "nand") {
                installLocationText->setText("app/fileselect/loc_nand"_i18n);
                installLocationText->setTextColor(nvgRGB(231, 76, 60)); // red/orange
            } else {
                installLocationText->setText("app/fileselect/loc_auto"_i18n);
                installLocationText->setTextColor(nvgRGB(52, 152, 219)); // blue
            }
        };
        updateInstallLocationDisplay();

        if (installLocationBox) {
            installLocationBox->registerClickAction([this, updateInstallLocationDisplay, &cfg](brls::View* view) {
                std::string loc = cfg.getInstallLocation();
                if (loc == "auto") {
                    cfg.setInstallLocation("sd");
                } else if (loc == "sd") {
                    cfg.setInstallLocation("nand");
                } else {
                    cfg.setInstallLocation("auto");
                }
                cfg.save();
                updateInstallLocationDisplay();
                updateTotalSize(); // Recheck space if we changed storage
                return true;
            });
        }
    }

    // Grab focus to prevent navigation events from reaching the background catalog view
    fileListScroll->setFocusable(true);
    brls::Application::giveFocus(fileListScroll);

    this->registerAction("app/actions/toggle_all"_i18n, brls::ControllerButton::BUTTON_X,
        [this](brls::View*) { toggleAllSelection(); return true; });

    this->registerAction("app/actions/start_download"_i18n, brls::ControllerButton::BUTTON_START,
        [this](brls::View*) { startDownloadAndGoToDownloads(); return true; });

    auto alive = alive_flag_;
    auto status_running = std::make_shared<std::atomic<bool>>(true);
    brls::async([this, status_running, alive]() {
        while (status_running->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (!status_running->load()) break;
            
            auto status = datasource::CustomEngineClient::instance().probeStatus();
            brls::sync([this, status, status_running, alive]() {
                if (!alive->load() || !status_running->load()) return;
                std::string text = "app/fileselect/probing"_i18n;
                if (status.active && status.meta_peers_total > 0) {
                    text += brls::getStr("app/fileselect/meta_peers", std::to_string(status.meta_peers_tried), std::to_string(status.meta_peers_total));
                } else if (status.active) {
                    text += brls::getStr("app/fileselect/peer_search", status.phase);
                } else {
                    text += brls::getStr("app/fileselect/swarm_stats", std::to_string(status.seeds), std::to_string(status.peers), std::to_string(status.dht_nodes));
                }
                subtitle->setText(text);
            });
        }
    });

    brls::async([this, status_running, alive]() {
        util::logLine("FileSelectView: probe thread started");
        std::vector<torrent::TorrentFileInfo> probedFiles;
        std::string err;

        util::logLine("FileSelectView: probing magnet " + game_.magnet);
        bool success = ui::DownloadManager::instance().getImpl()
                           .probeTorrentFiles(game_.magnet, probedFiles, &err);
        util::logLine("FileSelectView: probe done, success=" + std::to_string(success) +
                      " count=" + std::to_string(probedFiles.size()));

        status_running->store(false);

        brls::sync([this, success, probedFiles, err, alive]() {
            if (!alive->load()) {
                util::logLine("FileSelectView: view already destroyed, aborting sync");
                return;
            }
            util::logLine("FileSelectView: sync callback, success=" +
                          std::to_string(success) + " count=" +
                          std::to_string(probedFiles.size()));

            if (success && !probedFiles.empty()) {
                files_    = probedFiles;
                selected_.assign(files_.size(), false);
                if (!retro_console_id_.empty()) {
                    bool anyRom = false;
                    for (size_t i = 0; i < files_.size(); ++i) {
                        if (isRomFile(files_[i].name)) {
                            selected_[i] = true;
                            anyRom = true;
                        }
                    }
                    if (!anyRom) {
                        selected_.assign(files_.size(), true);
                    }
                } else {
                    SwitchServiceGuard guard;
                    for (size_t i = 0; i < files_.size(); ++i) {
                        uint64_t tid = parseTitleIdFromFilename(files_[i].name);
                        bool isGame = isSwitchGameFile(files_[i].name);
                        bool installed = isTitleIdInstalled(tid, guard);
                        if (isGame && !installed) {
                            selected_[i] = true;
                        }
                    }
                }
                subtitle->setText("app/fileselect/select_prompt"_i18n);
                updateTotalSize();
                rebuildFileList();
                util::logLine("FileSelectView: rebuildFileList done, rows=" +
                              std::to_string(files_.size()));
                
                // Allow focus to leave the scroll frame now that items are focusable
                fileListScroll->setFocusable(false);

                // Set default focus to the first focusable file item
                if (fileListBox) {
                    brls::View* firstFocusable = nullptr;
                    for (auto* child : fileListBox->getChildren()) {
                        if (child->isFocusable()) {
                            firstFocusable = child;
                            break;
                        }
                    }
                    if (firstFocusable) {
                        brls::Application::giveFocus(firstFocusable);
                    }
                }
            } else {
                subtitle->setText("app/fileselect/meta_error"_i18n);
                brls::Application::notify("app/common/error"_i18n + ": " +
                    (err.empty() ? "app/fileselect/timeout_error"_i18n : err));
                util::logLine("FileSelectView: probe error: " + err);
            }
        });
    });

    updateTotalSize();
    util::logLine("FileSelectView: onContentAvailable end");
}

brls::View* FileSelectView::create() { return nullptr; }

// ─────────────────────────────────────────────────────────────────────────────
// rebuildFileList — build the Box children from files_/selected_
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::rebuildFileList() {
    // Remove all existing child rows
    fileListBox->clearViews();
    checkboxLabels_.assign(files_.size(), nullptr);

    SwitchServiceGuard guard;

    struct DisplayItem {
        size_t originalIndex;
        bool isHeader;
        std::string headerTitle;
    };
    std::vector<DisplayItem> displayItems;

    if (!retro_console_id_.empty()) {
        displayItems.push_back({0, true, "app/fileselect/group_roms"_i18n});
        bool hasRoms = false;
        for (size_t i = 0; i < files_.size(); ++i) {
            if (isRomFile(files_[i].name)) {
                displayItems.push_back({i, false, ""});
                hasRoms = true;
            }
        }
        if (!hasRoms) {
            for (size_t i = 0; i < files_.size(); ++i) {
                displayItems.push_back({i, false, ""});
            }
        } else {
            bool hasOther = false;
            for (size_t i = 0; i < files_.size(); ++i) {
                if (!isRomFile(files_[i].name)) {
                    if (!hasOther) {
                        displayItems.push_back({0, true, "app/fileselect/group_other"_i18n});
                        hasOther = true;
                    }
                    displayItems.push_back({i, false, ""});
                }
            }
        }
    } else {
    // 1. Group: NOT INSTALLED
    displayItems.push_back({0, true, "app/fileselect/group_uninstalled"_i18n});
    bool hasUninstalled = false;
    for (size_t i = 0; i < files_.size(); ++i) {
        uint64_t tid = parseTitleIdFromFilename(files_[i].name);
        if (isSwitchGameFile(files_[i].name) && !isTitleIdInstalled(tid, guard)) {
            displayItems.push_back({i, false, ""});
            hasUninstalled = true;
        }
    }
    if (!hasUninstalled) {
        displayItems.push_back({0, true, "app/fileselect/no_files"_i18n});
    }

    // 2. Group: OTHER FILES
    displayItems.push_back({0, true, "app/fileselect/group_other"_i18n});
    bool hasOther = false;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (!isSwitchGameFile(files_[i].name)) {
            displayItems.push_back({i, false, ""});
            hasOther = true;
        }
    }
    if (!hasOther) {
        displayItems.push_back({0, true, "app/fileselect/no_files"_i18n});
    }

    // 3. Group: INSTALLED
    displayItems.push_back({0, true, "app/fileselect/group_installed"_i18n});
    bool hasInstalled = false;
    for (size_t i = 0; i < files_.size(); ++i) {
        uint64_t tid = parseTitleIdFromFilename(files_[i].name);
        if (isSwitchGameFile(files_[i].name) && isTitleIdInstalled(tid, guard)) {
            displayItems.push_back({i, false, ""});
            hasInstalled = true;
        }
    }
    if (!hasInstalled) {
        displayItems.push_back({0, true, "app/fileselect/no_files"_i18n});
    }

    }
    // Now render them
    brls::View* lastFocusable = nullptr;
    for (const auto& item : displayItems) {
        if (item.isHeader) {
            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setHeight(50);
            row->setWidth(brls::View::AUTO);
            row->setPaddingLeft(10);
            row->setPaddingRight(10);
            row->setAlignItems(brls::AlignItems::CENTER);
            
            auto* label = new brls::Label();
            label->setFontSize(16);
            label->setText(item.headerTitle);
            
            if (item.headerTitle == "app/fileselect/no_files"_i18n) {
                label->setTextColor(nvgRGB(150, 150, 150));
            } else {
                label->setTextColor(nvgRGB(255, 87, 34)); // Orange/red accent for headers
            }
            row->addView(label);
            fileListBox->addView(row);
        } else {
            size_t idx = item.originalIndex;
            const auto& file = files_[idx];
            bool isSel = (idx < selected_.size()) && selected_[idx];

            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setHeight(60);
            row->setWidth(brls::View::AUTO);
            row->setPaddingLeft(10);
            row->setPaddingRight(10);
            row->setFocusable(true);

            // Checkbox label
            auto* chk = new brls::Label();
            chk->setWidth(40);
            chk->setHeight(brls::View::AUTO);
            chk->setFontSize(20);
            chk->setText(isSel ? "[V]" : "[ ]");
            chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : nvgRGB(180, 180, 180));
            row->addView(chk);
            checkboxLabels_[idx] = chk;

            // File name label
            auto* nameLbl = new brls::Label();
            nameLbl->setGrow(1.0f);
            nameLbl->setHeight(brls::View::AUTO);
            nameLbl->setFontSize(16);
            nameLbl->setText(file.name);
            nameLbl->setMarginLeft(10);
            nameLbl->setMarginRight(10);
            
            uint64_t tid = parseTitleIdFromFilename(file.name);
            if (isSwitchGameFile(file.name) && isTitleIdInstalled(tid, guard)) {
                nameLbl->setTextColor(nvgRGB(120, 120, 120)); // Gray out slightly
            }
            row->addView(nameLbl);

            // File size label
            auto* sizeLbl = new brls::Label();
            sizeLbl->setWidth(140);
            sizeLbl->setHeight(brls::View::AUTO);
            sizeLbl->setFontSize(14);
            sizeLbl->setText(formatBytes(file.size));
            sizeLbl->setTextColor(nvgRGB(136, 136, 136));
            row->addView(sizeLbl);

            // Click to toggle
            row->registerClickAction([this, idx](brls::View*) {
                if (idx < selected_.size()) {
                    selected_[idx] = !selected_[idx];
                    updateTotalSize();
                    updateRowSelectionState(idx);
                }
                return true;
            });

            fileListBox->addView(row);
            lastFocusable = row;
            util::logLine("FileSelectView: added row " + std::to_string(idx) +
                          " name='" + file.name + "'");
        }
    }

    if (lastFocusable && installLocationBox) {
        lastFocusable->setCustomNavigationRoute(brls::FocusDirection::DOWN, installLocationBox);
        installLocationBox->setCustomNavigationRoute(brls::FocusDirection::UP, lastFocusable);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// updateTotalSize
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::updateTotalSize() {
    unsigned long long total = 0;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i])
            total += files_[i].size;
    }
    totalSizeText->setText(formatBytes(total));

    if (!retro_console_id_.empty()) {
        int64_t sdFree = 0;
        if (freeSpaceSdText) {
            if (util::getStorageFreeSpace(1, sdFree)) {
                freeSpaceSdText->setText(brls::getStr("app/fileselect/free_sd", formatBytes(sdFree)));
            }
        }
        if (freeSpaceNandText) freeSpaceNandText->setVisibility(brls::Visibility::GONE);
        return;
    }

    // Update free space displays
    int64_t sdFree = 0;
    int64_t nandFree = 0;
    if (freeSpaceSdText) {
        if (util::getStorageFreeSpace(1, sdFree)) {
            freeSpaceSdText->setText(brls::getStr("app/fileselect/free_sd", formatBytes(sdFree)));
        } else {
            freeSpaceSdText->setText(brls::getStr("app/fileselect/free_sd", "app/fileselect/free_unknown"_i18n));
        }
    }

    if (freeSpaceNandText) {
        if (util::getStorageFreeSpace(0, nandFree)) {
            freeSpaceNandText->setText(brls::getStr("app/fileselect/free_nand", formatBytes(nandFree)));
        } else {
            freeSpaceNandText->setText(brls::getStr("app/fileselect/free_nand", "app/fileselect/free_unknown"_i18n));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// toggleAllSelection
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::toggleAllSelection() {
    if (selected_.empty()) return;
    bool anySelected = false;
    for (bool s : selected_) { if (s) { anySelected = true; break; } }
    for (size_t i = 0; i < selected_.size(); ++i) selected_[i] = !anySelected;
    updateTotalSize();
    for (size_t i = 0; i < files_.size(); ++i) {
        updateRowSelectionState(i);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// startDownloadAndGoToDownloads
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::startDownloadAndGoToDownloads() {
    std::vector<int> selectedIndices;
    int forcedIndex = -1;
    std::string forcedName;
    unsigned long long largestGameSize = 0;
    unsigned long long totalNeededSize = 0;

    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            selectedIndices.push_back(files_[i].index);
            totalNeededSize += files_[i].size;
            if (isSwitchGameFile(files_[i].name) && files_[i].size > largestGameSize) {
                largestGameSize = files_[i].size;
                forcedIndex     = files_[i].index;
                forcedName      = files_[i].name;
            }
        }
    }

    if (selectedIndices.empty()) {
        brls::Application::notify("app/fileselect/no_files_selected"_i18n);
        return;
    }

    // Fallback: largest file overall
    if (forcedIndex < 0) {
        unsigned long long maxOverall = 0;
        for (size_t i = 0; i < files_.size(); ++i) {
            if (i < selected_.size() && selected_[i] && files_[i].size > maxOverall) {
                maxOverall  = files_[i].size;
                forcedIndex = files_[i].index;
                forcedName  = files_[i].name;
            }
        }
    }

    if (!retro_console_id_.empty()) {
        int64_t freeSpace = 0;
        util::getStorageFreeSpace(1, freeSpace);
        if (static_cast<int64_t>(totalNeededSize) > freeSpace) {
            std::string msg = brls::getStr("app/fileselect/low_space_prompt", "SD", formatBytes(totalNeededSize), formatBytes(freeSpace));
            brls::Dialog* dialog = new brls::Dialog(msg);
            dialog->addButton("app/common/continue"_i18n, [this, selectedIndices, forcedIndex, forcedName]() {
                this->executeDownloads(selectedIndices, forcedIndex, forcedName);
            });
            dialog->addButton("app/common/cancel"_i18n, []() {});
            dialog->open();
            return;
        }
        executeDownloads(selectedIndices, forcedIndex, forcedName);
        return;
    }

    // Check free space depending on install_location setting
    auto& cfg = config::ConfigManager::instance();
    std::string loc = cfg.getInstallLocation();
    
    int64_t freeSpace = 0;
    std::string targetStorageName;
    bool checkPassed = true;

    if (loc == "sd") {
        targetStorageName = "app/fileselect/storage_sd"_i18n;
        util::getStorageFreeSpace(1, freeSpace);
        if (static_cast<int64_t>(totalNeededSize) > freeSpace) {
            checkPassed = false;
        }
    } else if (loc == "nand") {
        targetStorageName = "app/fileselect/storage_nand"_i18n;
        util::getStorageFreeSpace(0, freeSpace);
        if (static_cast<int64_t>(totalNeededSize) > freeSpace) {
            checkPassed = false;
        }
    } else { // "auto"
        targetStorageName = "app/fileselect/storage_auto"_i18n;
        int64_t sdFree = 0;
        int64_t nandFree = 0;
        util::getStorageFreeSpace(1, sdFree);
        util::getStorageFreeSpace(0, nandFree);

        if (static_cast<int64_t>(totalNeededSize) <= sdFree) {
            freeSpace = sdFree;
        } else if (static_cast<int64_t>(totalNeededSize) <= nandFree) {
            freeSpace = nandFree;
        } else {
            freeSpace = sdFree; // default display
            checkPassed = false;
        }
    }

    if (!checkPassed) {
        std::string msg = brls::getStr("app/fileselect/low_space_prompt", targetStorageName, formatBytes(totalNeededSize), formatBytes(freeSpace));
        
        brls::Dialog* dialog = new brls::Dialog(msg);
        // Dialog closes itself via Dialog::buttonClick; do NOT call close() here
        // (it would pop this FileSelectView too).
        dialog->addButton("app/common/continue"_i18n, [this, selectedIndices, forcedIndex, forcedName]() {
            this->executeDownloads(selectedIndices, forcedIndex, forcedName);
        });
        dialog->addButton("app/common/cancel"_i18n, []() {});
        dialog->open();
        return;
    }

    executeDownloads(selectedIndices, forcedIndex, forcedName);
}

void FileSelectView::executeDownloads(const std::vector<int>& selectedIndices, int forcedIndex, const std::string& forcedName) {
    (void)forcedIndex;
    (void)forcedName;

    std::vector<size_t> chosen;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            chosen.push_back(i);
        }
    }

    if (!retro_console_id_.empty()) {
        for (size_t i : chosen) {
            std::vector<int> singleSelected = { files_[i].index };
            std::string itemTitle = cleanTitle(game_.title);
            if (selectedIndices.size() > 1) {
                itemTitle += " (" + files_[i].name + ")";
            }
            Game singleGame = game_;
            singleGame.title = itemTitle;
            singleGame.topic_id = game_.topic_id + "_" + std::to_string(files_[i].index);
            ui::DownloadManager::instance().addDownload(singleGame, singleSelected, files_[i].index, files_[i].name, retro_console_id_);
        }
    } else {
        std::vector<size_t> packages;
        std::vector<size_t> extraFiles;

        for (size_t idx : chosen) {
            if (isSwitchGameFile(files_[idx].name)) {
                packages.push_back(idx);
            } else {
                extraFiles.push_back(idx);
            }
        }

        // Сортируем выбранные установочные пакеты по приоритету установки:
        // Базовая игра (v0) ВСЕГДА ставится первой, затем обновления, затем DLC!
        std::stable_sort(packages.begin(), packages.end(), [this](size_t a, size_t b) {
            int prio_a = download::installFilePriority(files_[a].name);
            int prio_b = download::installFilePriority(files_[b].name);
            if (prio_a != prio_b) {
                return prio_a > prio_b;
            }
            return files_[a].size > files_[b].size;
        });

        size_t totalTasks = packages.size() + (!extraFiles.empty() ? 1 : 0);

        // 1. Добавляем установочные пакеты (каждый устанавливается отдельно в NCM)
        for (size_t idx : packages) {
            std::vector<int> singleSelected = { files_[idx].index };
            std::string itemTitle = cleanTitle(game_.title);
            if (totalTasks > 1) {
                itemTitle += " (" + files_[idx].name + ")";
            }
            Game singleGame = game_;
            singleGame.title = itemTitle;
            singleGame.topic_id = game_.topic_id + "_" + std::to_string(files_[idx].index);
            ui::DownloadManager::instance().addDownload(singleGame, singleSelected, files_[idx].index, files_[idx].name, retro_console_id_);
        }

        // 2. Все выбранные доп. файлы (русификаторы, патчи, моды) объединяем в 1 загрузку
        if (!extraFiles.empty()) {
            std::vector<int> extraIndices;
            extraIndices.reserve(extraFiles.size());
            for (size_t idx : extraFiles) {
                extraIndices.push_back(files_[idx].index);
            }

            std::string extraTitle = cleanTitle(game_.title);
            if (extraFiles.size() == 1) {
                extraTitle += " (" + files_[extraFiles[0]].name + ")";
            } else {
                std::string countStr = std::to_string(extraFiles.size());
                extraTitle += " (" + brls::getStr("app/fileselect/extra_files_bundle", countStr) + ")";
            }

            Game extraGame = game_;
            extraGame.title = extraTitle;
            extraGame.topic_id = game_.topic_id + "_extras";
            int firstIndex = files_[extraFiles[0]].index;
            std::string forcedName = files_[extraFiles[0]].name;

            ui::DownloadManager::instance().addDownload(extraGame, extraIndices, firstIndex, forcedName, retro_console_id_);
        }
    }

    brls::sync([]() {
        while (brls::Application::getActivitiesStack().size() > 1)
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
        brls::Application::pushActivity(new ui::DownloadsView());
    });
}

void FileSelectView::updateRowSelectionState(size_t idx) {
    if (idx >= checkboxLabels_.size()) return;
    brls::Label* chk = checkboxLabels_[idx];
    if (!chk) return;

    bool isSel = (idx < selected_.size()) && selected_[idx];
    chk->setText(isSel ? "[V]" : "[ ]");
    chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : nvgRGB(180, 180, 180));
}

} // namespace ui
