#include "FileSelectView.hpp"
#include "DownloadUiManager.hpp"
#include "MainMenu.hpp"
#include "DownloadsView.hpp"
#include "ThemeManager.hpp"
#include "../datasource/custom_engine_client.h"
#include "../config/config.h"
#include "../catalog/retro_catalog_manager.h"
#include "../utils/switch_utils.h"
#include "../net/image_downloader.h"
#include "../installer/nsp_header.h"
#include "../installer/cnmt_parser.h"
#include "../installer/ncz_parser.h"
#include "../utils/app_paths.h"
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

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
        ".pbp", ".vpk", ".smd", ".gen", ".md", ".gg", ".sg", ".sms", ".cdi", ".gdi",
        ".pce", ".sgx", ".ngp", ".ngc", ".ws", ".wsc", ".col", ".vec", ".a26", ".a52", ".a78", ".lnx"
    };
    for (const auto& ext : romExts) {
        if (lower.size() >= ext.size() && lower.rfind(ext) == lower.size() - ext.size()) {
            return true;
        }
    }
    return false;
}

static bool isPlayableFile(const std::string& filename) {
    if (isRomFile(filename)) return true;
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 4 &&
           (lower.rfind(".nro") == lower.size() - 4 ||
            lower.rfind(".ovl") == lower.size() - 4 ||
            lower.rfind(".elf") == lower.size() - 4);
}

static bool isMediaOrMetaPath(const std::string& fullpath) {
    std::string lower = fullpath;
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::vector<std::string> mediaPatterns = {
        "/images/", "/image/", "/covers/", "/cover/", "/screenshots/", "/screenshot/",
        "/snaps/", "/titles/", "/boxart/", "/boxarts/", "/manuals/", "/manual/",
        "/cheats/", "/cheat/", "/saves/", "/save/", "/wheel/", "/wheels/",
        "images/", "image/", "covers/", "cover/", "screenshots/", "screenshot/",
        "snaps/", "titles/", "boxart/", "boxarts/", "manuals/", "manual/",
        "cheats/", "cheat/", "saves/", "save/", "wheel/", "wheels/"
    };
    for (const auto& pat : mediaPatterns) {
        if (pat.front() == '/') {
            if (lower.find(pat) != std::string::npos) return true;
        } else {
            if (lower.rfind(pat, 0) == 0) return true;
        }
    }
    return false;
}

static std::string computeBaseFolder(const std::vector<torrent::TorrentFileInfo>& files) {
    if (files.empty()) return "";
    std::string first = files[0].name;
    std::replace(first.begin(), first.end(), '\\', '/');
    size_t slash = first.find('/');
    if (slash == std::string::npos) return "";
    std::string root = first.substr(0, slash);
    if (root.empty()) return "";

    for (size_t i = 1; i < files.size(); ++i) {
        std::string p = files[i].name;
        std::replace(p.begin(), p.end(), '\\', '/');
        if (p.rfind(root + "/", 0) != 0) {
            return "";
        }
    }
    return root;
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

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) {
                parts.push_back(part);
                part.clear();
            }
        } else {
            part += c;
        }
    }
    if (!part.empty()) {
        parts.push_back(part);
    }
    return parts;
}
#ifdef __SWITCH__
#include <switch.h>

struct SwitchServiceGuard {
    bool ns_ok = false;
    bool ncm_ok = false;
    NcmContentMetaDatabase db_sd = {};
    bool db_sd_open = false;
    NcmContentMetaDatabase db_nand = {};
    bool db_nand_open = false;

    NcmContentStorage cs_sd = {};
    bool cs_sd_open = false;
    NcmContentStorage cs_nand = {};
    bool cs_nand_open = false;

    SwitchServiceGuard() {
        g_switch_service_mutex.lock();
        ns_ok = R_SUCCEEDED(nsInitialize());
        ncm_ok = R_SUCCEEDED(ncmInitialize());
        if (ncm_ok) {
            db_sd_open = R_SUCCEEDED(ncmOpenContentMetaDatabase(&db_sd, NcmStorageId_SdCard));
            db_nand_open = R_SUCCEEDED(ncmOpenContentMetaDatabase(&db_nand, NcmStorageId_BuiltInUser));
            cs_sd_open = R_SUCCEEDED(ncmOpenContentStorage(&cs_sd, NcmStorageId_SdCard));
            cs_nand_open = R_SUCCEEDED(ncmOpenContentStorage(&cs_nand, NcmStorageId_BuiltInUser));
        }
        util::logLine("FileSelectView: SwitchServiceGuard init: ns=" + std::to_string(ns_ok) +
                      " ncm=" + std::to_string(ncm_ok) +
                      " db_sd=" + std::to_string(db_sd_open) +
                      " db_nand=" + std::to_string(db_nand_open) +
                      " cs_sd=" + std::to_string(cs_sd_open) +
                      " cs_nand=" + std::to_string(cs_nand_open));
    }

    ~SwitchServiceGuard() {
        if (cs_sd_open) ncmContentStorageClose(&cs_sd);
        if (cs_nand_open) ncmContentStorageClose(&cs_nand);
        if (db_sd_open) ncmContentMetaDatabaseClose(&db_sd);
        if (db_nand_open) ncmContentMetaDatabaseClose(&db_nand);
        if (ncm_ok) ncmExit();
        if (ns_ok) nsExit();
        g_switch_service_mutex.unlock();
    }

    bool hasContentId(const NcmContentId& cid) {
        bool has = false;
        if (cs_sd_open && R_SUCCEEDED(ncmContentStorageHas(&cs_sd, &has, &cid)) && has) return true;
        if (cs_nand_open && R_SUCCEEDED(ncmContentStorageHas(&cs_nand, &has, &cid)) && has) return true;
        return false;
    }

    bool verifyContentPresent(NcmContentMetaDatabase* db, const NcmContentMetaKey& key) {
        if (!db) return false;
        static const NcmContentType types[] = {
            NcmContentType_Program,
            NcmContentType_Data,
            NcmContentType_Control,
            NcmContentType_HtmlDocument,
            NcmContentType_LegalInformation
        };

        bool foundAnyContent = false;
        for (NcmContentType ct : types) {
            NcmContentId cid;
            if (R_SUCCEEDED(ncmContentMetaDatabaseGetContentIdByType(db, &cid, &key, ct))) {
                foundAnyContent = true;
                if (hasContentId(cid)) return true;
            }
        }

        // AddOnContent / DLC can be metadata-only (license / unlock) with 0 content NCAs
        if (!foundAnyContent && key.type == NcmContentMetaType_AddOnContent) {
            return true;
        }

        return false;
    }

    bool isInstalled(uint64_t tid, uint32_t file_version = 0) {
        if (tid == 0) return false;

        NcmContentMetaKey key = {};
        // 1) Try SD card meta database
        if (db_sd_open && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db_sd, &key, tid))) {
            if (verifyContentPresent(&db_sd, key)) {
                if ((tid & 0xFFFULL) == 0x800 && file_version > 0) {
                    return key.version >= file_version;
                }
                return true;
            }
        }

        // 2) Try NAND meta database
        if (db_nand_open && R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db_nand, &key, tid))) {
            if (verifyContentPresent(&db_nand, key)) {
                if ((tid & 0xFFFULL) == 0x800 && file_version > 0) {
                    return key.version >= file_version;
                }
                return true;
            }
        }

        return false;
    }
};

static inline bool isTitleIdInstalled(uint64_t tid, SwitchServiceGuard& guard, uint32_t file_version = 0) {
    return guard.isInstalled(tid, file_version);
}
#else
struct SwitchServiceGuard {
    bool isInstalled(uint64_t tid, uint32_t file_version = 0) {
        (void)file_version;
        if (tid == 0x01000BF0152FB131ULL) return true; // DLC Left 4 Dead Bundle (mock)
        return false;
    }
};

static inline bool isTitleIdInstalled(uint64_t tid, SwitchServiceGuard& guard, uint32_t file_version = 0) {
    return guard.isInstalled(tid, file_version);
}
#endif

static uint64_t resolveFileTitleId(const std::string& filename, const std::string& game_title_id) {
    uint64_t tid = util::parseTitleIdFromFileName(filename);
    if (tid != 0) return tid;

    // Fallback: if filename didn't contain a Title ID in brackets/name, but game_title_id is set
    // and this file is a Switch base game (not marked as update or dlc), use game_title_id.
    if (!game_title_id.empty() && isSwitchGameFile(filename)) {
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool is_update = (lower.find("upd") != std::string::npos || lower.find("patch") != std::string::npos);
        bool is_dlc = (lower.find("dlc") != std::string::npos || lower.find("addon") != std::string::npos);
        if (!is_update && !is_dlc) {
            try {
                uint64_t g_tid = std::stoull(game_title_id, nullptr, 16);
                if (util::isSwitchTitleId(g_tid)) {
                    return g_tid;
                }
            } catch (...) {}
        }
    }
    return 0;
}

static inline uint64_t parseTitleIdFromFilename(const std::string& name) {
    return util::parseTitleIdFromFileName(name);
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

    if (toggleAllBox) {
        toggleAllBox->setBackgroundColor(ThemeManager::instance().getCardBgColor());
        toggleAllBox->setBorderColor(ThemeManager::instance().getCardBorderColor());
        toggleAllBox->setBorderThickness(1.0f);
        toggleAllBox->registerClickAction([this](brls::View*) {
            toggleAllSelection();
            return true;
        });
    }
    if (toggleAllBtnIcon) {
        toggleAllBtnIcon->setText(brls::Hint::getKeyIcon(brls::ControllerButton::BUTTON_X));
        toggleAllBtnIcon->setTextColor(ThemeManager::instance().getAccentColor());
    }
    if (toggleAllBtnText) {
        toggleAllBtnText->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    }

    if (bottomSummaryBox) {
        bottomSummaryBox->setBackgroundColor(ThemeManager::instance().getCardBgColor());
        bottomSummaryBox->setBorderColor(ThemeManager::instance().getCardBorderColor());
        bottomSummaryBox->setBorderThickness(1.0f);
    }
    if (installLocationBox) {
        installLocationBox->setBackgroundColor(ThemeManager::instance().getDimAccentColor());
        installLocationBox->setBorderColor(ThemeManager::instance().getMediumAccentColor());
        installLocationBox->setBorderThickness(1.5f);
        installLocationBox->setCornerRadius(8.0f);
        installLocationBox->getFocusEvent()->subscribe([this](bool focused) {
            if (installLocationBox) {
                installLocationBox->setBorderColor(focused ? ThemeManager::instance().getAccentColor() : ThemeManager::instance().getMediumAccentColor());
                installLocationBox->setBackgroundColor(focused ? ThemeManager::instance().getMediumAccentColor() : ThemeManager::instance().getDimAccentColor());
            }
        });
    }
    if (totalSizeText) {
        totalSizeText->setTextColor(ThemeManager::instance().getAccentColor());
    }

    // Configure install location selector
    auto& cfg = config::ConfigManager::instance();
    if (!retro_console_id_.empty()) {
        if (freeSpaceNandText) freeSpaceNandText->setVisibility(brls::Visibility::GONE);
        if (installLocationText) {
            std::string sub = retro_console_id_;
            const auto* cInfo = catalog::RetroCatalogManager::instance().getConsole(retro_console_id_);
            if (cInfo && !cInfo->default_rom_subfolder.empty()) sub = cInfo->default_rom_subfolder;
            installLocationText->setText(cfg.getEffectiveRetroRomsDir(sub));
            installLocationText->setTextColor(ThemeManager::instance().getAccentColor());
        }
    } else if (isHomebrewGame(game_)) {
        if (freeSpaceNandText) freeSpaceNandText->setVisibility(brls::Visibility::GONE);
        if (installLocationText) {
            installLocationText->setText(TSNX_DOWNLOADS_DIR);
            installLocationText->setTextColor(ThemeManager::instance().getAccentColor());
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
                installLocationText->setTextColor(ThemeManager::instance().getAccentColor());
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

    this->registerAction("", brls::ControllerButton::BUTTON_X,
        [this](brls::View*) { toggleAllSelection(); return true; }, true);

    this->registerAction("app/actions/start_download"_i18n, brls::ControllerButton::BUTTON_START,
        [this](brls::View*) { startDownloadAndGoToDownloads(); return true; });

    this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View*) {
        if (isFolderHierarchyMode() && currentFolder_ != baseFolder_) {
            navigateUp();
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_BACKSPACE), [this](brls::View*) {
        if (isFolderHierarchyMode() && currentFolder_ != baseFolder_) {
            navigateUp();
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    auto alive = alive_flag_;
    auto status_running = std::make_shared<std::atomic<bool>>(true);
    brls::async([this, status_running, alive]() {
        while (status_running->load() && alive->load() && !g_appExiting.load()) {
            for (int i = 0; i < 10; ++i) {
                if (!status_running->load() || !alive->load() || g_appExiting.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!status_running->load() || !alive->load() || g_appExiting.load()) break;

            auto status = datasource::CustomEngineClient::instance().probeStatus();
            brls::sync([this, status, status_running, alive]() {
                if (!alive->load() || !status_running->load() || g_appExiting.load()) return;
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
                if (isFolderHierarchyMode()) {
                    baseFolder_ = computeBaseFolder(files_);
                    currentFolder_ = baseFolder_;
                    bool anySelected = false;
                    for (size_t i = 0; i < files_.size(); ++i) {
                        bool isPrimary = false;
                        if (!retro_console_id_.empty()) {
                            isPrimary = isRomFile(files_[i].name);
                        } else {
                            isPrimary = !isMediaOrMetaPath(files_[i].name);
                        }
                        if (isPrimary && !isMediaOrMetaPath(files_[i].name)) {
                            selected_[i] = true;
                            anySelected = true;
                        }
                    }
                    if (!anySelected) {
                        for (size_t i = 0; i < files_.size(); ++i) {
                            if (!isMediaOrMetaPath(files_[i].name)) {
                                selected_[i] = true;
                                anySelected = true;
                            }
                        }
                    }
                    if (!anySelected) {
                        selected_.assign(files_.size(), true);
                    }
                    updateRetroSubtitle();
                } else {
                    SwitchServiceGuard guard;
                    for (size_t i = 0; i < files_.size(); ++i) {
                        uint64_t tid = resolveFileTitleId(files_[i].name, game_.title_id);
                        uint32_t ver = util::parseVersionFromFileName(files_[i].name);
                        bool isGame = isSwitchGameFile(files_[i].name);
                        bool installed = isTitleIdInstalled(tid, guard, ver);
                        if (isGame && !installed) {
                            selected_[i] = true;
                        }
                    }
                    subtitle->setText("app/fileselect/select_prompt"_i18n);
                }
                calculateUncompressedSizes();
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
    if (isFolderHierarchyMode()) {
        rebuildRetroFolderList();
    } else {
        rebuildSwitchFileList();
    }
}

void FileSelectView::navigateToFolder(const std::string& folder) {
    currentFolder_ = folder;
    rebuildRetroFolderList();
    if (fileListBox) {
        for (auto* child : fileListBox->getChildren()) {
            if (child->isFocusable()) {
                brls::Application::giveFocus(child);
                break;
            }
        }
    }
}

void FileSelectView::navigateUp() {
    if (currentFolder_.empty() || currentFolder_ == baseFolder_) {
        return;
    }
    size_t lastSlash = currentFolder_.rfind('/');
    if (lastSlash == std::string::npos) {
        currentFolder_ = "";
    } else {
        currentFolder_ = currentFolder_.substr(0, lastSlash);
    }
    rebuildRetroFolderList();
    if (fileListBox) {
        for (auto* child : fileListBox->getChildren()) {
            if (child->isFocusable()) {
                brls::Application::giveFocus(child);
                break;
            }
        }
    }
}

void FileSelectView::updateRetroSubtitle() {
    if (!subtitle) return;
    std::string displayPath;
    if (currentFolder_.empty() || currentFolder_ == baseFolder_) {
        displayPath = "/";
    } else if (!baseFolder_.empty() && currentFolder_.rfind(baseFolder_ + "/", 0) == 0) {
        displayPath = "/" + currentFolder_.substr(baseFolder_.length() + 1);
    } else {
        displayPath = "/" + currentFolder_;
    }

    size_t selCount = 0;
    uint64_t selBytes = 0;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            selCount++;
            selBytes += files_[i].size;
        }
    }

    std::string pathText = "app/fileselect/folder_prefix"_i18n + displayPath;
    std::string selText = brls::getStr("app/fileselect/files_selected", std::to_string(selCount)) + " (" + formatBytes(selBytes) + ")";
    subtitle->setText(pathText + "  |  " + selText);
}

void FileSelectView::rebuildRetroFolderList() {
    fileListBox->clearViews();
    checkboxLabels_.assign(files_.size(), nullptr);

    updateRetroSubtitle();

    struct SubfolderInfo {
        std::string name;
        std::string fullPath;
        std::vector<size_t> fileIndices;
        uint64_t totalSize = 0;
        size_t selectedCount = 0;
    };

    struct FileEntryInfo {
        std::string name;
        size_t fileIndex = 0;
    };

    std::map<std::string, SubfolderInfo> subfoldersMap;
    std::vector<FileEntryInfo> directFiles;

    std::string prefix = currentFolder_.empty() ? "" : (currentFolder_ + "/");

    for (size_t i = 0; i < files_.size(); ++i) {
        std::string normPath = files_[i].name;
        std::replace(normPath.begin(), normPath.end(), '\\', '/');

        if (!prefix.empty()) {
            if (normPath.rfind(prefix, 0) != 0) {
                continue;
            }
        }

        std::string relPath = normPath.substr(prefix.size());
        if (relPath.empty()) continue;

        size_t slashPos = relPath.find('/');
        if (slashPos != std::string::npos) {
            std::string subName = relPath.substr(0, slashPos);
            std::string subFullPath = prefix + subName;

            auto& sInfo = subfoldersMap[subName];
            if (sInfo.name.empty()) {
                sInfo.name = subName;
                sInfo.fullPath = subFullPath;
            }
            sInfo.fileIndices.push_back(i);
            sInfo.totalSize += files_[i].size;
            if (i < selected_.size() && selected_[i]) {
                sInfo.selectedCount++;
            }
        } else {
            directFiles.push_back({relPath, i});
        }
    }

    std::sort(directFiles.begin(), directFiles.end(), [](const FileEntryInfo& a, const FileEntryInfo& b) {
        return a.name < b.name;
    });

    brls::View* firstFocusable = nullptr;
    brls::View* lastFocusable = nullptr;

    // 1. Parent folder row ".." if not at baseFolder_
    if (!currentFolder_.empty() && currentFolder_ != baseFolder_) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(60);
        row->setWidth(brls::View::AUTO);
        row->setPaddingLeft(10);
        row->setPaddingRight(10);
        row->setFocusable(true);

        auto* icon = new brls::Label();
        icon->setFontSize(22);
        icon->setText("\uE5D8"); // Arrow up
        icon->setTextColor(ThemeManager::instance().getAccentColor());
        icon->setWidth(40);
        row->addView(icon);

        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16);
        nameLbl->setText("app/fileselect/parent_folder"_i18n);
        nameLbl->setMarginLeft(10);
        nameLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        row->addView(nameLbl);

        row->registerClickAction([this](brls::View*) {
            navigateUp();
            return true;
        });

        fileListBox->addView(row);
        if (!firstFocusable) firstFocusable = row;
        lastFocusable = row;
    }

    // 2. Subfolders
    for (auto& pair : subfoldersMap) {
        auto& sInfo = pair.second;
        bool allSel = (sInfo.selectedCount == sInfo.fileIndices.size());
        bool partialSel = (sInfo.selectedCount > 0 && !allSel);

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
        if (allSel) {
            chk->setText("[V]");
            chk->setTextColor(nvgRGB(76, 175, 80));
        } else if (partialSel) {
            chk->setText("[-]");
            chk->setTextColor(nvgRGB(255, 183, 77));
        } else {
            chk->setText("[ ]");
            chk->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        }
        row->addView(chk);

        // Folder Icon
        auto* icon = new brls::Label();
        icon->setFontSize(22);
        icon->setText("\uE2C7"); // Material folder
        icon->setTextColor(ThemeManager::instance().getAccentColor());
        icon->setMarginRight(10);
        row->addView(icon);

        // Folder Name
        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16);
        nameLbl->setText(sInfo.name);
        nameLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        row->addView(nameLbl);

        // Folder Info (file count + total size)
        auto* infoLbl = new brls::Label();
        infoLbl->setWidth(160);
        infoLbl->setHeight(brls::View::AUTO);
        infoLbl->setFontSize(14);
        infoLbl->setText(std::to_string(sInfo.fileIndices.size()) + " | " + formatBytes(sInfo.totalSize));
        infoLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        row->addView(infoLbl);

        // Clicking row (Button A) opens the folder
        row->registerClickAction([this, path = sInfo.fullPath](brls::View*) {
            navigateToFolder(path);
            return true;
        });

        // Toggle folder action (Button Y) toggles all files in folder
        auto toggleFolder = [this, sInfoCopy = sInfo, chk](brls::View*) {
            bool allSelected = true;
            for (size_t idx : sInfoCopy.fileIndices) {
                if (idx < selected_.size() && !selected_[idx]) {
                    allSelected = false;
                    break;
                }
            }
            bool newState = !allSelected;
            for (size_t idx : sInfoCopy.fileIndices) {
                if (idx < selected_.size()) {
                    selected_[idx] = newState;
                }
            }
            if (newState) {
                chk->setText("[V]");
                chk->setTextColor(nvgRGB(76, 175, 80));
            } else {
                chk->setText("[ ]");
                chk->setTextColor(ThemeManager::instance().getTextSecondaryColor());
            }
            updateTotalSize();
            updateRetroSubtitle();
            return true;
        };

        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFolder);

        fileListBox->addView(row);
        if (!firstFocusable) firstFocusable = row;
        lastFocusable = row;
    }

    // 3. Direct files
    for (const auto& item : directFiles) {
        size_t idx = item.fileIndex;
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
        chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : ThemeManager::instance().getTextSecondaryColor());
        row->addView(chk);
        checkboxLabels_[idx] = chk;

        // File icon
        auto* icon = new brls::Label();
        icon->setFontSize(20);
        bool isPlayable = isPlayableFile(file.name);
        icon->setText(isPlayable ? "\uE0E0" : "\uE24D");
        icon->setTextColor(isPlayable ? ThemeManager::instance().getAccentColor() : ThemeManager::instance().getTextSecondaryColor());
        icon->setMarginRight(10);
        row->addView(icon);

        // File name label
        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16);
        nameLbl->setText(item.name);
        nameLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        row->addView(nameLbl);

        // File size label
        auto* sizeLbl = new brls::Label();
        sizeLbl->setWidth(140);
        sizeLbl->setHeight(brls::View::AUTO);
        sizeLbl->setFontSize(14);
        sizeLbl->setText(formatBytes(file.size));
        sizeLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        row->addView(sizeLbl);

        // Click to toggle file (Button A)
        auto toggleFile = [this, idx](brls::View*) {
            if (idx < selected_.size()) {
                selected_[idx] = !selected_[idx];
                updateRowSelectionState(idx);
                updateTotalSize();
                updateRetroSubtitle();
            }
            return true;
        };
        row->registerClickAction(toggleFile);
        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFile);

        fileListBox->addView(row);
        if (!firstFocusable) firstFocusable = row;
        lastFocusable = row;
    }

    if (subfoldersMap.empty() && directFiles.empty()) {
        auto* row = new brls::Box();
        row->setHeight(50);
        row->setPaddingLeft(10);
        auto* emptyLbl = new brls::Label();
        emptyLbl->setFontSize(16);
        emptyLbl->setText("app/fileselect/no_files"_i18n);
        emptyLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        row->addView(emptyLbl);
        fileListBox->addView(row);
    }

    if (lastFocusable && installLocationBox) {
        lastFocusable->setCustomNavigationRoute(brls::FocusDirection::DOWN, installLocationBox);
        installLocationBox->setCustomNavigationRoute(brls::FocusDirection::UP, lastFocusable);
    }
}

void FileSelectView::renderOtherFilesTree(const OtherFileTreeNode& node, int depth, brls::View*& lastFocusable, std::map<std::string, brls::View*>& focusMap) {
    // 1. Render Subfolders
    for (const auto& pair : node.subfolders) {
        const auto& sub = pair.second;
        bool isExpanded = (expandedFolders_.count(sub.fullPath) > 0);
        bool allSel = (sub.selectedFiles == sub.totalFiles && sub.totalFiles > 0);
        bool partialSel = (sub.selectedFiles > 0 && !allSel);

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(60);
        row->setWidth(brls::View::AUTO);
        row->setPaddingLeft(10 + depth * 24);
        row->setPaddingRight(10);
        row->setFocusable(true);

        // Expand/Collapse arrow
        auto* arrow = new brls::Label();
        arrow->setWidth(24);
        arrow->setHeight(brls::View::AUTO);
        arrow->setFontSize(14);
        arrow->setText(isExpanded ? "▼" : "▶");
        arrow->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        row->addView(arrow);

        // Checkbox label
        auto* chk = new brls::Label();
        chk->setWidth(40);
        chk->setHeight(brls::View::AUTO);
        chk->setFontSize(20);
        if (allSel) {
            chk->setText("[V]");
            chk->setTextColor(nvgRGB(76, 175, 80));
        } else if (partialSel) {
            chk->setText("[-]");
            chk->setTextColor(nvgRGB(255, 183, 77));
        } else {
            chk->setText("[ ]");
            chk->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        }
        row->addView(chk);

        // Folder Icon
        auto* icon = new brls::Label();
        icon->setFontSize(22);
        icon->setText("\uE2C7"); // Material folder
        icon->setTextColor(ThemeManager::instance().getAccentColor());
        icon->setMarginRight(10);
        row->addView(icon);

        // Folder Name
        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16);
        nameLbl->setText(sub.name);
        nameLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        row->addView(nameLbl);

        // Folder Info (file count + total size)
        auto* infoLbl = new brls::Label();
        infoLbl->setWidth(160);
        infoLbl->setHeight(brls::View::AUTO);
        infoLbl->setFontSize(14);
        infoLbl->setText(std::to_string(sub.totalFiles) + " | " + formatBytes(sub.size));
        infoLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        row->addView(infoLbl);

        std::string rowKey = "folder:" + sub.fullPath;
        focusMap[rowKey] = row;

        // Button A: expand / collapse
        row->registerClickAction([this, path = sub.fullPath](brls::View*) {
            if (expandedFolders_.count(path)) {
                expandedFolders_.erase(path);
            } else {
                expandedFolders_.insert(path);
            }
            focusedKey_ = "folder:" + path;
            rebuildSwitchFileList();
            return true;
        });

        // Button Y: toggle folder selection
        auto toggleFolder = [this, indices = sub.allFileIndices, path = sub.fullPath, selCount = sub.selectedFiles, totCount = sub.totalFiles](brls::View*) {
            bool newState = (selCount < totCount);
            for (size_t idx : indices) {
                if (idx < selected_.size()) {
                    selected_[idx] = newState;
                }
            }
            focusedKey_ = "folder:" + path;
            updateTotalSize();
            rebuildSwitchFileList();
            return true;
        };
        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFolder);

        fileListBox->addView(row);
        lastFocusable = row;

        // If expanded, recursively render children
        if (isExpanded) {
            renderOtherFilesTree(sub, depth + 1, lastFocusable, focusMap);
        }
    }

    // 2. Render Direct Files
    auto directFilesSorted = node.directFiles;
    std::sort(directFilesSorted.begin(), directFilesSorted.end(), [](const OtherFileTreeNode& a, const OtherFileTreeNode& b) {
        return a.name < b.name;
    });

    for (const auto& item : directFilesSorted) {
        size_t idx = item.fileIndex;
        bool isSel = (idx < selected_.size()) && selected_[idx];

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(60);
        row->setWidth(brls::View::AUTO);
        row->setPaddingLeft(10 + depth * 24 + 24); // indented past folder arrow
        row->setPaddingRight(10);
        row->setFocusable(true);

        // Checkbox label
        auto* chk = new brls::Label();
        chk->setWidth(40);
        chk->setHeight(brls::View::AUTO);
        chk->setFontSize(20);
        chk->setText(isSel ? "[V]" : "[ ]");
        chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : ThemeManager::instance().getTextSecondaryColor());
        row->addView(chk);
        if (idx < checkboxLabels_.size()) {
            checkboxLabels_[idx] = chk;
        }

        // File icon
        auto* icon = new brls::Label();
        icon->setFontSize(20);
        icon->setText("\uE24D"); // File icon
        icon->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        icon->setMarginRight(10);
        row->addView(icon);

        // File name label
        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16);
        nameLbl->setText(item.name);
        nameLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        row->addView(nameLbl);

        // File size label
        auto* sizeLbl = new brls::Label();
        sizeLbl->setWidth(140);
        sizeLbl->setHeight(brls::View::AUTO);
        sizeLbl->setFontSize(14);
        sizeLbl->setText(formatBytes(item.size));
        sizeLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        row->addView(sizeLbl);

        std::string rowKey = "file:" + std::to_string(idx);
        focusMap[rowKey] = row;

        // Click to toggle file (Button A or Button Y)
        auto toggleFile = [this, idx](brls::View*) {
            if (idx < selected_.size()) {
                selected_[idx] = !selected_[idx];
                focusedKey_ = "file:" + std::to_string(idx);
                updateTotalSize();
                rebuildSwitchFileList();
            }
            return true;
        };
        row->registerClickAction(toggleFile);
        row->registerAction("app/fileselect/toggle_folder"_i18n, brls::ControllerButton::BUTTON_Y, toggleFile);

        fileListBox->addView(row);
        lastFocusable = row;
    }
}

void FileSelectView::rebuildSwitchFileList() {
    fileListBox->clearViews();
    checkboxLabels_.assign(files_.size(), nullptr);

    SwitchServiceGuard guard;
    std::map<std::string, brls::View*> focusMap;
    brls::View* firstFocusable = nullptr;
    brls::View* lastFocusable = nullptr;

    auto addHeaderRow = [this](const std::string& titleText, bool isEmpty) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setHeight(50);
        row->setWidth(brls::View::AUTO);
        row->setPaddingLeft(10);
        row->setPaddingRight(10);
        row->setAlignItems(brls::AlignItems::CENTER);

        auto* label = new brls::Label();
        label->setFontSize(16);
        label->setText(titleText);
        if (isEmpty) {
            label->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        } else {
            label->setTextColor(ThemeManager::instance().getAccentColor());
        }
        row->addView(label);
        fileListBox->addView(row);
    };

    // ─────────────────────────────────────────────────────────────────────────
    // 1. Group: NOT INSTALLED
    // ─────────────────────────────────────────────────────────────────────────
    addHeaderRow("app/fileselect/group_uninstalled"_i18n, false);
    bool hasUninstalled = false;
    for (size_t i = 0; i < files_.size(); ++i) {
        uint64_t tid = resolveFileTitleId(files_[i].name, game_.title_id);
        uint32_t ver = util::parseVersionFromFileName(files_[i].name);
        if (isSwitchGameFile(files_[i].name) && !isTitleIdInstalled(tid, guard, ver)) {
            hasUninstalled = true;
            size_t idx = i;
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

            auto* chk = new brls::Label();
            chk->setWidth(40);
            chk->setHeight(brls::View::AUTO);
            chk->setFontSize(20);
            chk->setText(isSel ? "[V]" : "[ ]");
            chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : ThemeManager::instance().getTextSecondaryColor());
            row->addView(chk);
            checkboxLabels_[idx] = chk;

            auto* nameLbl = new brls::Label();
            nameLbl->setGrow(1.0f);
            nameLbl->setHeight(brls::View::AUTO);
            nameLbl->setFontSize(16);
            nameLbl->setText(file.name);
            nameLbl->setMarginLeft(10);
            nameLbl->setMarginRight(10);
            nameLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
            row->addView(nameLbl);

            auto* sizeLbl = new brls::Label();
            sizeLbl->setWidth(140);
            sizeLbl->setHeight(brls::View::AUTO);
            sizeLbl->setFontSize(14);
            sizeLbl->setText(formatBytes(file.size));
            sizeLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor());
            row->addView(sizeLbl);

            std::string rowKey = "game:" + std::to_string(idx);
            focusMap[rowKey] = row;

            row->registerClickAction([this, idx](brls::View*) {
                if (idx < selected_.size()) {
                    selected_[idx] = !selected_[idx];
                    updateRowSelectionState(idx);
                    updateTotalSize();
                }
                return true;
            });

            fileListBox->addView(row);
            if (!firstFocusable) firstFocusable = row;
            lastFocusable = row;
        }
    }
    if (!hasUninstalled) {
        addHeaderRow("app/fileselect/no_files"_i18n, true);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 2. Group: OTHER FILES (TREE VIEW)
    // ─────────────────────────────────────────────────────────────────────────
    addHeaderRow("app/fileselect/group_other"_i18n, false);
    bool hasOther = false;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (!isSwitchGameFile(files_[i].name)) {
            hasOther = true;
            break;
        }
    }

    if (hasOther) {
        std::string basePrefix = computeBaseFolder(files_);
        if (!basePrefix.empty()) {
            basePrefix += "/";
        }

        OtherFileTreeNode rootNode;
        for (size_t i = 0; i < files_.size(); ++i) {
            if (isSwitchGameFile(files_[i].name)) continue;

            std::string normPath = files_[i].name;
            std::replace(normPath.begin(), normPath.end(), '\\', '/');

            std::string relPath = normPath;
            if (!basePrefix.empty() && relPath.rfind(basePrefix, 0) == 0) {
                relPath = relPath.substr(basePrefix.size());
            }

            std::vector<std::string> parts = splitPath(relPath);
            if (parts.empty()) continue;

            OtherFileTreeNode* curr = &rootNode;
            std::string currPath = "";
            for (size_t p = 0; p + 1 < parts.size(); ++p) {
                const std::string& seg = parts[p];
                if (!currPath.empty()) currPath += "/";
                currPath += seg;

                auto& sub = curr->subfolders[seg];
                if (sub.name.empty()) {
                    sub.name = seg;
                    sub.fullPath = currPath;
                    sub.isFolder = true;
                }
                sub.allFileIndices.push_back(i);
                sub.size += files_[i].size;
                sub.totalFiles++;
                if (i < selected_.size() && selected_[i]) {
                    sub.selectedFiles++;
                }
                curr = &sub;
            }

            OtherFileTreeNode leaf;
            leaf.name = parts.back();
            leaf.fullPath = normPath;
            leaf.isFolder = false;
            leaf.fileIndex = i;
            leaf.size = files_[i].size;
            curr->directFiles.push_back(leaf);
        }

        renderOtherFilesTree(rootNode, 0, lastFocusable, focusMap);
        if (!firstFocusable && lastFocusable) {
            firstFocusable = lastFocusable;
        }
    } else {
        addHeaderRow("app/fileselect/no_files"_i18n, true);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 3. Group: INSTALLED
    // ─────────────────────────────────────────────────────────────────────────
    addHeaderRow("app/fileselect/group_installed"_i18n, false);
    bool hasInstalled = false;
    for (size_t i = 0; i < files_.size(); ++i) {
        uint64_t tid = resolveFileTitleId(files_[i].name, game_.title_id);
        uint32_t ver = util::parseVersionFromFileName(files_[i].name);
        if (isSwitchGameFile(files_[i].name) && isTitleIdInstalled(tid, guard, ver)) {
            hasInstalled = true;
            size_t idx = i;
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

            auto* chk = new brls::Label();
            chk->setWidth(40);
            chk->setHeight(brls::View::AUTO);
            chk->setFontSize(20);
            chk->setText(isSel ? "[V]" : "[ ]");
            chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : ThemeManager::instance().getTextSecondaryColor());
            row->addView(chk);
            checkboxLabels_[idx] = chk;

            auto* nameLbl = new brls::Label();
            nameLbl->setGrow(1.0f);
            nameLbl->setHeight(brls::View::AUTO);
            nameLbl->setFontSize(16);
            nameLbl->setText(file.name);
            nameLbl->setMarginLeft(10);
            nameLbl->setMarginRight(10);
            nameLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor()); // Gray out installed
            row->addView(nameLbl);

            auto* sizeLbl = new brls::Label();
            sizeLbl->setWidth(140);
            sizeLbl->setHeight(brls::View::AUTO);
            sizeLbl->setFontSize(14);
            sizeLbl->setText(formatBytes(file.size));
            sizeLbl->setTextColor(ThemeManager::instance().getTextSecondaryColor());
            row->addView(sizeLbl);

            std::string rowKey = "game_inst:" + std::to_string(idx);
            focusMap[rowKey] = row;

            row->registerClickAction([this, idx](brls::View*) {
                if (idx < selected_.size()) {
                    selected_[idx] = !selected_[idx];
                    updateRowSelectionState(idx);
                    updateTotalSize();
                }
                return true;
            });

            fileListBox->addView(row);
            if (!firstFocusable) firstFocusable = row;
            lastFocusable = row;
        }
    }
    if (!hasInstalled) {
        addHeaderRow("app/fileselect/no_files"_i18n, true);
    }

    if (lastFocusable && installLocationBox) {
        lastFocusable->setCustomNavigationRoute(brls::FocusDirection::DOWN, installLocationBox);
        installLocationBox->setCustomNavigationRoute(brls::FocusDirection::UP, lastFocusable);
    }

    // Focus restoration
    if (!focusedKey_.empty()) {
        if (focusMap.count(focusedKey_)) {
            brls::Application::giveFocus(focusMap[focusedKey_]);
        } else {
            std::string prefix = focusedKey_;
            while (true) {
                size_t slash = prefix.rfind('/');
                if (slash == std::string::npos) break;
                prefix = prefix.substr(0, slash);
                if (focusMap.count(prefix)) {
                    brls::Application::giveFocus(focusMap[prefix]);
                    break;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// calculateUncompressedSizes
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::calculateUncompressedSizes() {
    if (files_.empty()) return;

    uint64_t total_torrent_bytes = 0;
    for (const auto& f : files_) {
        total_torrent_bytes += f.size;
    }

    double nsz_ratio = util::parseNszCompressionRatio(game_.image_format, total_torrent_bytes);
    util::logLine("FileSelectView: calculated NSZ ratio " + std::to_string(nsz_ratio) +
                  " from image_format='" + game_.image_format + "'");

    probe_info_.clear();
    probe_info_.resize(files_.size());
    for (size_t i = 0; i < files_.size(); ++i) {
        if (!isSwitchGameFile(files_[i].name)) {
            probe_info_[i].uncompressed_size = files_[i].size;
            probe_info_[i].is_estimated = false;
        } else {
            std::string lower = files_[i].name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.size() >= 4 && (lower.rfind(".nsz") == lower.size() - 4 || lower.rfind(".xcz") == lower.size() - 4)) {
                probe_info_[i].uncompressed_size = static_cast<uint64_t>(files_[i].size * nsz_ratio);
                probe_info_[i].is_estimated = (std::abs(nsz_ratio - 1.0) > 0.001);
            } else {
                probe_info_[i].uncompressed_size = files_[i].size;
                probe_info_[i].is_estimated = false;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// updateTotalSize
// ─────────────────────────────────────────────────────────────────────────────

void FileSelectView::updateTotalSize() {
    unsigned long long totalCompressed = 0;
    unsigned long long totalUnpacked = 0;
    bool anyEstimated = false;

    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            totalCompressed += files_[i].size;
            if (i < probe_info_.size()) {
                totalUnpacked += probe_info_[i].uncompressed_size;
                if (probe_info_[i].is_estimated) anyEstimated = true;
            } else {
                totalUnpacked += files_[i].size;
            }
        }
    }

    if (totalSizeText) {
        std::string sizeStr = formatBytes(totalCompressed);
        if (totalUnpacked > totalCompressed) {
            std::string prefix = brls::getStr("app/fileselect/unpacked_prefix");
            if (anyEstimated) {
                sizeStr += " (" + prefix + ": ~" + formatBytes(totalUnpacked) + " " + "app/fileselect/estimated"_i18n + ")";
            } else {
                sizeStr += " (" + prefix + ": " + formatBytes(totalUnpacked) + ")";
            }
        }
        totalSizeText->setText(sizeStr);
    }

    if (isFolderHierarchyMode()) {
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

    if (isFolderHierarchyMode() && currentFolder_ != baseFolder_) {
        // Inside a subfolder: toggle selection for files inside currentFolder_
        std::string prefix = currentFolder_ + "/";
        std::vector<size_t> folderIndices;
        bool anySelectedInFolder = false;

        for (size_t i = 0; i < files_.size(); ++i) {
            std::string normPath = files_[i].name;
            std::replace(normPath.begin(), normPath.end(), '\\', '/');
            if (normPath.rfind(prefix, 0) == 0) {
                folderIndices.push_back(i);
                if (i < selected_.size() && selected_[i]) {
                    anySelectedInFolder = true;
                }
            }
        }

        bool newState = !anySelectedInFolder;
        for (size_t idx : folderIndices) {
            if (idx < selected_.size()) {
                selected_[idx] = newState;
            }
        }
        rebuildRetroFolderList();
    } else {
        bool anySelected = false;
        for (bool s : selected_) {
            if (s) { anySelected = true; break; }
        }
        for (size_t i = 0; i < selected_.size(); ++i) {
            selected_[i] = !anySelected;
        }
        if (isFolderHierarchyMode()) {
            rebuildRetroFolderList();
        } else {
            rebuildSwitchFileList();
        }
    }
    updateTotalSize();
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
            uint64_t needed = files_[i].size;
            if (i < probe_info_.size() && probe_info_[i].uncompressed_size > 0) {
                needed = probe_info_[i].uncompressed_size;
            }
            totalNeededSize += needed;
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

    if (isFolderHierarchyMode()) {
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

    if (isFolderHierarchyMode()) {
        if (chosen.size() == 1) {
            size_t i = chosen[0];
            std::vector<int> singleSelected = { files_[i].index };
            std::string itemTitle = cleanTitle(game_.title) + " (" + std::filesystem::path(files_[i].name).filename().string() + ")";
            Game singleGame = game_;
            singleGame.title = itemTitle;
            singleGame.topic_id = game_.topic_id + "_" + std::to_string(files_[i].index);
            ui::DownloadManager::instance().addDownload(singleGame, singleSelected, files_[i].index, files_[i].name, retro_console_id_);
        } else if (!chosen.empty()) {
            std::vector<int> bundleSelected;
            bundleSelected.reserve(chosen.size());
            for (size_t i : chosen) {
                bundleSelected.push_back(files_[i].index);
            }
            std::string countStr = std::to_string(chosen.size());
            std::string bundleTitle = cleanTitle(game_.title) + " (" + brls::getStr("app/fileselect/bundle_files", countStr) + ")";
            Game bundleGame = game_;
            bundleGame.title = bundleTitle;
            bundleGame.topic_id = game_.topic_id + (retro_console_id_.empty() ? "_hb_bundle" : "_romset");
            int firstIndex = files_[chosen[0]].index;
            std::string firstName = files_[chosen[0]].name;
            ui::DownloadManager::instance().addDownload(bundleGame, bundleSelected, firstIndex, firstName, retro_console_id_);
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
        // Внутри одной известной категории (например, несколько DLC) сохраняем порядок файлов в торренте,
        // чтобы чтение кусков шло строго последовательно и работала предвыборка LOOKAHEAD_PIECES.
        std::stable_sort(packages.begin(), packages.end(), [this](size_t a, size_t b) {
            int prio_a = download::installFilePriority(files_[a].name);
            int prio_b = download::installFilePriority(files_[b].name);
            if (prio_a != prio_b) {
                return prio_a > prio_b;
            }
            if (prio_a >= 150 && prio_a < 200) {
                return files_[a].size > files_[b].size;
            }
            return files_[a].index < files_[b].index;
        });

        // 1. Добавляем установочные пакеты (каждый устанавливается отдельно в NCM)
        for (size_t idx : packages) {
            std::vector<int> singleSelected = { files_[idx].index };
            Game singleGame = game_;
            singleGame.title = cleanTitle(game_.title);
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
        brls::Application::popActivity(brls::TransitionAnimation::NONE, []() {
            brls::Application::pushActivity(new ui::DownloadsView());
        });
    });
}

void FileSelectView::updateRowSelectionState(size_t idx) {
    if (idx >= checkboxLabels_.size()) return;
    brls::Label* chk = checkboxLabels_[idx];
    if (!chk) return;

    bool isSel = (idx < selected_.size()) && selected_[idx];
    chk->setText(isSel ? "[V]" : "[ ]");
    chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : ThemeManager::instance().getTextSecondaryColor());
}

} // namespace ui
