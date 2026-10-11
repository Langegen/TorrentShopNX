#include "FileManagerView.hpp"
#include "ui/ThemeManager.hpp"
#include "ArchiveProgressDialog.hpp"
#include "InstallProgressDialog.hpp"
#include "TextViewerActivity.hpp"
#include "../utils/log.h"
#include "../utils/archive_utils.h"
#include "../utils/switch_utils.h"
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <unordered_set>

using namespace brls::literals;

namespace ui {

namespace {

bool isTextFile(const std::string& path) {
    static const std::unordered_set<std::string> textExts = {
        ".txt", ".log", ".ini", ".cfg", ".conf", ".json", ".xml",
        ".nfo", ".md", ".csv", ".tsv", ".yaml", ".yml", ".toml",
        ".properties", ".py", ".lua", ".sh", ".bat", ".cpp", ".hpp",
        ".c", ".h", ".js", ".html", ".css", ".sql", ".patch", ".diff"
    };
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return textExts.count(ext) > 0;
}

static std::string joinPath(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '/' || dir.back() == '\\') return dir + file;
    return dir + "/" + file;
}

static std::string normalizeDir(const std::string& path) {
    std::string p = util::normalizeFsPath(path);
    if (p.size() == 2 && p[1] == ':') {
        p += "/";
    }
    if (p == "sdmc:" || p == "sdmc") {
        p = "sdmc:/";
    }
    while (p.size() > 1 && p.back() == '/') {
        if (p.size() == 3 && p[1] == ':') break;
        if (p == "sdmc:/") break;
        p.pop_back();
    }
    return p;
}

static std::string getParentDir(const std::string& path) {
    std::string p = normalizeDir(path);
    if (p == "sdmc:/" || p == "sdmc:" || p == "/" || p == "." || p.empty()) {
        return "";
    }
    if (p.size() <= 3 && p.find(':') != std::string::npos) {
        return "";
    }

    size_t lastSlash = p.find_last_of('/');
    if (lastSlash == std::string::npos) {
        return "";
    }
    if (lastSlash == 0) {
        return "/";
    }
    if (p.size() >= 5 && p.substr(0, 5) == "sdmc:" && lastSlash == 5) {
        return "sdmc:/";
    }
    if (lastSlash == 2 && p[1] == ':') {
        return p.substr(0, 3);
    }
    return p.substr(0, lastSlash);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// FileManagerCell
// ─────────────────────────────────────────────────────────────────────────────

FileManagerCell::FileManagerCell() {
    this->inflateFromXMLRes("xml/file_manager_cell.xml");
    if (cellRoot) {
        cellRoot->getFocusEvent()->subscribe([this](bool focused) {
            if (focused && parentView) {
                parentView->setFocusedRow(panelIndex, static_cast<int>(rowIndex));
                if (parentView->getActivePanel() != panelIndex) {
                    parentView->setActivePanel(panelIndex);
                }
            }
        });
    }
}

FileManagerCell* FileManagerCell::create() {
    return new FileManagerCell();
}

void FileManagerCell::setSelectedVisual(bool selected) {
    if (selected) {
        if (accentBar) accentBar->setBackgroundColor(ThemeManager::instance().getAccentColor());
        this->setBackgroundColor(ThemeManager::instance().getDimAccentColor());
        if (name) name->setTextColor(ThemeManager::instance().getAccentColor());
    } else {
        if (accentBar) accentBar->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        this->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        if (name) name->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    }
}

void FileManagerCell::setCompactMode(bool compact) {
    if (isCompact_ == compact && compactConfigured_) return;
    isCompact_ = compact;
    compactConfigured_ = true;

    if (date) {
        date->setVisibility(compact ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    if (size) {
        size->setWidth(compact ? 80.0f : 130.0f);
        size->setMarginRight(compact ? 6.0f : 12.0f);
    }
    if (cellRoot) {
        cellRoot->setPaddingLeft(compact ? 10.0f : 16.0f);
        cellRoot->setPaddingRight(compact ? 12.0f : 16.0f);
    }
    if (name) {
        name->setMarginRight(compact ? 8.0f : 16.0f);
        name->setShrink(1.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FileManagerView
// ─────────────────────────────────────────────────────────────────────────────

FileManagerView::FileManagerView(const std::string& initialPath, const std::string& focusChild, const std::string& rootDir) {
    panels_[0].index = 0;
    panels_[0].rootDir = rootDir.empty() ? "" : normalizeDir(rootDir);
    if (!initialPath.empty()) {
        panels_[0].currentDir = normalizeDir(initialPath);
    } else {
        std::string defRoot = util::getDefaultRootPath();
        panels_[0].currentDir = defRoot.empty() ? "sdmc:/" : normalizeDir(defRoot);
    }

    panels_[1].index = 1;
    panels_[1].rootDir = panels_[0].rootDir;
    panels_[1].currentDir = panels_[0].currentDir;

    initialFocusChild_ = focusChild;
}

void FileManagerView::initPanelBindings() {
    // Panel 0 (Left)
    panels_[0].container = leftPanelBox;
    panels_[0].panelIcon = leftPanelIcon;
    panels_[0].currentPath = currentPath;
    panels_[0].spaceInfo = spaceInfo;
    panels_[0].selectionBar = selectionBar;
    panels_[0].selectionText = selectionText;
    panels_[0].selectionHint = selectionHint;
    panels_[0].colHeaders = leftColHeaders;
    panels_[0].colSize = leftColSize;
    panels_[0].colDate = leftColDate;
    panels_[0].recycler = recycler;
    panels_[0].emptyLabel = emptyLabel;

    // Panel 1 (Right)
    panels_[1].container = rightPanelBox;
    panels_[1].panelIcon = rightPanelIcon;
    panels_[1].currentPath = currentPathRight;
    panels_[1].spaceInfo = spaceInfoRight;
    panels_[1].selectionBar = selectionBarRight;
    panels_[1].selectionText = selectionTextRight;
    panels_[1].selectionHint = selectionHintRight;
    panels_[1].colHeaders = rightColHeaders;
    panels_[1].colSize = rightColSize;
    panels_[1].colDate = rightColDate;
    panels_[1].recycler = recyclerRight;
    panels_[1].emptyLabel = emptyLabelRight;

    if (leftPanelIcon) leftPanelIcon->setText("\uE2C7");
    if (rightPanelIcon) rightPanelIcon->setText("\uE2C7");
}

void FileManagerView::onContentAvailable() {
    util::logLine("FileManagerView: onContentAvailable start");
    initPanelBindings();

    if (recycler) {
        util::logLine("FileManagerView: registering Cell and setting DataSource for Left Recycler");
        recycler->estimatedRowHeight = 56.0f;
        recycler->setPaddingRight(26.0f);
        recycler->registerCell("Cell", []() { return FileManagerCell::create(); });
        recycler->setDataSource(new FileManagerDataSource(this, 0));
    }

    if (recyclerRight) {
        util::logLine("FileManagerView: registering Cell and setting DataSource for Right Recycler");
        recyclerRight->estimatedRowHeight = 56.0f;
        recyclerRight->setPaddingRight(26.0f);
        recyclerRight->registerCell("Cell", []() { return FileManagerCell::create(); });
        recyclerRight->setDataSource(new FileManagerDataSource(this, 1));
    }

    if (leftPanelBox) {
        leftPanelBox->addGestureRecognizer(new brls::TapGestureRecognizer(leftPanelBox, [this]() {
            if (activePanel_ != 0) switchActivePanel(0);
        }));
    }

    if (rightPanelBox) {
        rightPanelBox->addGestureRecognizer(new brls::TapGestureRecognizer(rightPanelBox, [this]() {
            if (activePanel_ != 1) switchActivePanel(1);
        }));
    }

    if (selectionBar) {
        selectionBar->addGestureRecognizer(new brls::TapGestureRecognizer(selectionBar, [this]() {
            setActivePanel(0);
            showActionsMenu();
        }));
    }

    if (selectionBarRight) {
        selectionBarRight->addGestureRecognizer(new brls::TapGestureRecognizer(selectionBarRight, [this]() {
            setActivePanel(1);
            showActionsMenu();
        }));
    }

    if (currentPath) {
        currentPath->addGestureRecognizer(new brls::TapGestureRecognizer(currentPath, [this]() {
            if (panels_[0].hasParentDir) navigateUp(0);
        }));
    }

    if (currentPathRight) {
        currentPathRight->addGestureRecognizer(new brls::TapGestureRecognizer(currentPathRight, [this]() {
            if (panels_[1].hasParentDir) navigateUp(1);
        }));
    }

    if (splitToggleHint) {
        splitToggleHint->addGestureRecognizer(new brls::TapGestureRecognizer(splitToggleHint, [this]() {
            toggleSplitMode();
        }));
    }

    if (splitNavHint) {
        splitNavHint->addGestureRecognizer(new brls::TapGestureRecognizer(splitNavHint, [this]() {
            if (isSplitMode_) {
                switchActivePanel(1 - activePanel_);
            }
        }));
    }

    // Register Activity Level Actions for Borealis Hints
    this->registerAction("app/file_manager/action_btn"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        showActionsMenu();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_X), [this](brls::View* view) {
        showActionsMenu();
        return true;
    });

    this->registerAction("app/file_manager/select_all"_i18n, brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        selectAll(activePanel_);
        return true;
    }, true);

    this->registerAction("app/file_manager/deselect_all"_i18n, brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        clearSelection(activePanel_);
        return true;
    }, true);

    // Toggle split mode: [-] button (BUTTON_BACK) - hidden from footer (shown in top-right header)
    this->registerAction("app/file_manager/split_toggle"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        toggleSplitMode();
        return true;
    }, true);

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_MINUS), [this](brls::View* view) {
        toggleSplitMode();
        return true;
    });

    // Panel switching with Stick, D-Pad, and Arrow Keys (Left / Right)
    auto handleNavRight = [this](brls::View* view) {
        if (isSplitMode_ && activePanel_ == 0) {
            switchActivePanel(1);
            return true;
        }
        return false;
    };

    auto handleNavLeft = [this](brls::View* view) {
        if (isSplitMode_ && activePanel_ == 1) {
            switchActivePanel(0);
            return true;
        }
        return false;
    };

    this->registerAction("", brls::ControllerButton::BUTTON_NAV_RIGHT, handleNavRight, true);
    this->registerAction("", brls::ControllerButton::BUTTON_RIGHT, handleNavRight, true);
    this->registerAction("", brls::ControllerButton::BUTTON_NAV_LEFT, handleNavLeft, true);
    this->registerAction("", brls::ControllerButton::BUTTON_LEFT, handleNavLeft, true);
    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_RIGHT), handleNavRight);
    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_LEFT), handleNavLeft);

    // Also support ZL and ZR / Tab for panel switching
    this->registerAction("", brls::ControllerButton::BUTTON_LT, [this](brls::View* view) {
        if (isSplitMode_) {
            switchActivePanel(0);
            return true;
        }
        return false;
    }, true);

    this->registerAction("", brls::ControllerButton::BUTTON_RT, [this](brls::View* view) {
        if (isSplitMode_) {
            switchActivePanel(1);
            return true;
        }
        return false;
    }, true);

    // Keyboard bindings for panel switching and split toggling
    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_TAB), [this](brls::View* view) {
        if (isSplitMode_) {
            switchActivePanel(1 - activePanel_);
            return true;
        }
        return false;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_F6), [this](brls::View* view) {
        toggleSplitMode();
        return true;
    });

    // Custom B button handling: navigate up in active panel if inside subfolder, else exit activity
    this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
        auto& cur = panels_[activePanel_];
        if (cur.currentDir != cur.rootDir && cur.hasParentDir) {
            brls::sync([this]() {
                navigateUp(activePanel_);
            });
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_BACKSPACE), [this](brls::View* view) {
        auto& cur = panels_[activePanel_];
        if (cur.currentDir != cur.rootDir && cur.hasParentDir) {
            brls::sync([this]() {
                navigateUp(activePanel_);
            });
            return true;
        }
        return false;
    });

    // Start (+) button allows immediately exiting back to previous screen
    this->registerAction("hints/exit"_i18n, brls::ControllerButton::BUTTON_START, [this](brls::View* view) {
        brls::Application::popActivity();
        return true;
    }, true);

    util::logLine("FileManagerView: calling initial refresh");
    std::string focus = initialFocusChild_;
    initialFocusChild_.clear();
    refresh(0, focus);
    updateSplitHints();
    updateCompactMode();
    updateActivePanelVisuals();
    util::logLine("FileManagerView: onContentAvailable end");
}

void FileManagerView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (resetState) {
        auto* rec = panels_[activePanel_].recycler;
        if (rec) {
            int targetRow = (panels_[activePanel_].currentFocusedRow >= 0) ? panels_[activePanel_].currentFocusedRow : (panels_[activePanel_].hasParentDir && !panels_[activePanel_].items.empty() ? 1 : 0);
            rec->setDefaultCellFocus(brls::IndexPath(0, targetRow));
            rec->selectRowAt(brls::IndexPath(0, targetRow), false);
            brls::Application::giveFocus(rec);
        }
    }
}

void FileManagerView::navigateTo(int panelIdx, const std::string& path, const std::string& focusChild) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    panels_[panelIdx].currentDir = normalizeDir(path);
    panels_[panelIdx].selectedPaths.clear();
    refresh(panelIdx, focusChild);
    updateActivePanelVisuals();
}

void FileManagerView::navigateUp(int panelIdx) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    auto& cur = panels_[panelIdx];
    if (!cur.rootDir.empty() && cur.currentDir == cur.rootDir) {
        return;
    }
    std::string parentDir = getParentDir(cur.currentDir);
    if (parentDir.empty() || parentDir == cur.currentDir) {
        return;
    }
    std::filesystem::path p(cur.currentDir);
    std::string childName = p.filename().generic_string();
    navigateTo(panelIdx, parentDir, childName);
}

void FileManagerView::refresh(int panelIdx, const std::string& focusChild) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    auto& p = panels_[panelIdx];
    p.currentDir = normalizeDir(p.currentDir);

    // Check if we have a parent directory
    std::string parentDir = getParentDir(p.currentDir);
    bool atRoot = (!p.rootDir.empty() && p.currentDir == p.rootDir);
    p.hasParentDir = !atRoot && (!parentDir.empty() && parentDir != p.currentDir);

    std::string err;
    std::string archPath, innerPath;
    if (util::parseArchiveVirtualPath(p.currentDir, archPath, innerPath)) {
        p.hasParentDir = true;
        p.items.clear();
        if (!util::listArchiveFolder(archPath, innerPath, p.items, err)) {
            util::logLine("FileManagerView: listArchiveFolder failed: " + err);
            brls::Application::notify(err.empty() ? "Failed to read archive" : err);
        }
    } else {
        p.items = util::listFolder(p.currentDir, err);
    }

    if (p.currentPath) {
        p.currentPath->setText(p.currentDir);
    }

    if (p.emptyLabel) {
        p.emptyLabel->setVisibility((p.items.empty() && !p.hasParentDir) ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    updateSpaceInfo(panelIdx);
    updateSelectionBar(panelIdx);

    // Determine target focus row
    int targetRow = 0;
    if (!focusChild.empty()) {
        for (size_t i = 0; i < p.items.size(); ++i) {
            if (p.items[i].name == focusChild) {
                targetRow = static_cast<int>(i) + (p.hasParentDir ? 1 : 0);
                break;
            }
        }
    } else if (p.hasParentDir && !p.items.empty()) {
        targetRow = 1;
    }

    p.currentFocusedRow = targetRow;

    if (p.recycler) {
        p.recycler->reloadData();
        if (panelIdx == activePanel_) {
            brls::sync([this, panelIdx, targetRow]() {
                auto* rec = panels_[panelIdx].recycler;
                if (rec) {
                    rec->setDefaultCellFocus(brls::IndexPath(0, targetRow));
                    rec->selectRowAt(brls::IndexPath(0, targetRow), false);
                    auto stack = brls::Application::getActivitiesStack();
                    bool topIsDialog = !stack.empty() && stack.back() &&
                                       dynamic_cast<brls::Dialog*>(stack.back()->getContentView()) != nullptr;
                    if (!topIsDialog) {
                        brls::Application::giveFocus(rec);
                    }
                }
            });
        }
    }
}

void FileManagerView::setFocusedRow(int panelIdx, int row) {
    if (panelIdx >= 0 && panelIdx <= 1) {
        panels_[panelIdx].currentFocusedRow = row;
    }
}

void FileManagerView::setActivePanel(int panelIdx) {
    if (panelIdx < 0 || panelIdx > 1) return;
    if (activePanel_ == panelIdx) return;
    activePanel_ = panelIdx;
    updateActivePanelVisuals();
}

void FileManagerView::switchActivePanel(int panelIdx) {
    if (!isSplitMode_ || panelIdx < 0 || panelIdx > 1) return;
    setActivePanel(panelIdx);
    auto* rec = panels_[activePanel_].recycler;
    if (rec) {
        int targetRow = panels_[activePanel_].currentFocusedRow;
        if (targetRow < 0) {
            targetRow = (panels_[activePanel_].hasParentDir && !panels_[activePanel_].items.empty()) ? 1 : 0;
        }
        int maxRow = static_cast<int>(panels_[activePanel_].items.size()) + (panels_[activePanel_].hasParentDir ? 1 : 0) - 1;
        targetRow = std::clamp(targetRow, 0, std::max(0, maxRow));
        panels_[activePanel_].currentFocusedRow = targetRow;
        rec->focusRow(targetRow);
    }
}

void FileManagerView::toggleSplitMode() {
    isSplitMode_ = !isSplitMode_;

    if (isSplitMode_) {
        if (panels_[1].currentDir.empty()) {
            panels_[1].currentDir = panels_[0].currentDir;
            panels_[1].rootDir = panels_[0].rootDir;
        }
        if (rightPanelBox) rightPanelBox->setVisibility(brls::Visibility::VISIBLE);
        if (panelDivider) panelDivider->setVisibility(brls::Visibility::VISIBLE);
        refresh(1);
    } else {
        if (activePanel_ == 1) {
            panels_[0].currentDir = panels_[1].currentDir;
            panels_[0].selectedPaths = panels_[1].selectedPaths;
            activePanel_ = 0;
            refresh(0);
        }
        if (rightPanelBox) rightPanelBox->setVisibility(brls::Visibility::GONE);
        if (panelDivider) panelDivider->setVisibility(brls::Visibility::GONE);
    }

    updateSplitHints();
    updateCompactMode();
    updateActivePanelVisuals();

    brls::Application::getGlobalHintsUpdateEvent()->fire();

    auto* rec = panels_[activePanel_].recycler;
    if (rec) {
        int targetRow = panels_[activePanel_].currentFocusedRow;
        if (targetRow < 0) {
            targetRow = (panels_[activePanel_].hasParentDir && !panels_[activePanel_].items.empty()) ? 1 : 0;
        }
        rec->focusRow(targetRow);
    }
}

void FileManagerView::updateSplitHints() {
    if (splitToggleHint) {
        if (isSplitMode_) {
            splitToggleHint->setText("[-] " + brls::getStr("app/file_manager/split_mode_disable"));
        } else {
            splitToggleHint->setText("[-] " + brls::getStr("app/file_manager/split_toggle"));
        }
    }
    if (splitNavHint) {
        splitNavHint->setVisibility(isSplitMode_ ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
}

void FileManagerView::updateCompactMode() {
    // Strictly equal widths: flex-basis 0 with equal grow & shrink
    if (leftPanelBox) {
        leftPanelBox->setWidth(0.0f);
        leftPanelBox->setGrow(1.0f);
        leftPanelBox->setShrink(1.0f);
    }
    if (rightPanelBox) {
        rightPanelBox->setWidth(0.0f);
        rightPanelBox->setGrow(1.0f);
        rightPanelBox->setShrink(1.0f);
    }

    if (leftColDate) {
        leftColDate->setVisibility(isSplitMode_ ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    }
    if (leftColSize) {
        leftColSize->setWidth(isSplitMode_ ? 80.0f : 130.0f);
    }
    if (leftColHeaders) {
        leftColHeaders->setPaddingLeft(isSplitMode_ ? 56.0f : 72.0f);
        leftColHeaders->setPaddingRight(36.0f);
    }
    if (rightColHeaders) {
        rightColHeaders->setPaddingLeft(56.0f);
        rightColHeaders->setPaddingRight(36.0f);
    }
    if (rightColDate) {
        rightColDate->setVisibility(brls::Visibility::GONE);
    }
    if (rightColSize) {
        rightColSize->setWidth(80.0f);
    }

    if (recycler) recycler->reloadData();
    if (recyclerRight && isSplitMode_) recyclerRight->reloadData();
}

void FileManagerView::updateActivePanelVisuals() {
    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    if (isSplitMode_) {
        for (int i = 0; i < 2; ++i) {
            bool isActive = (i == activePanel_);
            if (panels_[i].container) {
                panels_[i].container->setBorderThickness(1.0f);
                panels_[i].container->setBorderColor(isActive ? ThemeManager::instance().getMediumAccentColor() : (isLight ? nvgRGBA(0, 0, 0, 25) : nvgRGBA(255, 255, 255, 18)));
                panels_[i].container->setBackgroundColor(isActive ? (isLight ? nvgRGBA(0, 0, 0, 10) : nvgRGBA(255, 255, 255, 8)) : (isLight ? nvgRGBA(0, 0, 0, 18) : nvgRGBA(0, 0, 0, 40)));
            }
            if (panels_[i].panelIcon) {
                panels_[i].panelIcon->setTextColor(isActive ? ThemeManager::instance().getAccentColor() : ThemeManager::instance().getTextSecondaryColor());
            }
            if (panels_[i].currentPath) {
                panels_[i].currentPath->setTextColor(isActive ? ThemeManager::instance().getTextPrimaryColor() : ThemeManager::instance().getTextSecondaryColor());
            }
            if (panels_[i].spaceInfo) {
                panels_[i].spaceInfo->setTextColor(isActive ? ThemeManager::instance().getAccentColor() : ThemeManager::instance().getTextSecondaryColor());
            }
        }
    } else {
        if (panels_[0].container) {
            panels_[0].container->setBorderThickness(0.0f);
            panels_[0].container->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        }
        if (panels_[0].panelIcon) {
            panels_[0].panelIcon->setTextColor(ThemeManager::instance().getAccentColor());
        }
        if (panels_[0].currentPath) {
            panels_[0].currentPath->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        }
        if (panels_[0].spaceInfo) {
            panels_[0].spaceInfo->setTextColor(ThemeManager::instance().getAccentColor());
        }
    }
}

void FileManagerView::updateSpaceInfo(int panelIdx) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    auto& p = panels_[panelIdx];
    if (!p.spaceInfo) return;

    uint64_t freeB = 0, totalB = 0;
    std::string checkPath = p.currentDir;
    std::string archPath, innerPath;
    if (util::parseArchiveVirtualPath(p.currentDir, archPath, innerPath)) {
        checkPath = archPath;
    }
    if (util::getStorageSpace(checkPath, freeB, totalB) && totalB > 0) {
        char buf[128];
        std::string freeStr = util::formatFileSize(freeB);
        std::string totalStr = util::formatFileSize(totalB);
        std::snprintf(buf, sizeof(buf), "%s / %s", freeStr.c_str(), totalStr.c_str());
        p.spaceInfo->setText(buf);
    } else {
        p.spaceInfo->setText("");
    }
}

void FileManagerView::updateSelectionBar(int panelIdx) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    auto& p = panels_[panelIdx];
    if (!p.selectionBar || !p.selectionText) return;

    if (p.selectedPaths.empty()) {
        p.selectionBar->setVisibility(brls::Visibility::GONE);
    } else {
        p.selectionBar->setVisibility(brls::Visibility::VISIBLE);
        size_t filesCount = 0;
        size_t dirsCount = 0;
        uint64_t totalSize = 0;

        for (const auto& it : p.items) {
            if (p.selectedPaths.count(it.path)) {
                if (it.isDir) dirsCount++;
                else {
                    filesCount++;
                    totalSize += it.size;
                }
            }
        }

        char buf[128];
        if (filesCount == 0) {
            std::string word = dirsCount == 1 ? "app/file_manager/item_folder_1"_i18n : (dirsCount < 5 ? "app/file_manager/item_folder_few"_i18n : "app/file_manager/item_folder_many"_i18n);
            std::snprintf(buf, sizeof(buf), "%zu %s", dirsCount, word.c_str());
        } else if (dirsCount == 0) {
            std::string sizeStr = util::formatFileSize(totalSize);
            std::string word = filesCount == 1 ? "app/file_manager/item_file_1"_i18n : (filesCount < 5 ? "app/file_manager/item_file_few"_i18n : "app/file_manager/item_file_many"_i18n);
            std::snprintf(buf, sizeof(buf), "%zu %s · %s", filesCount, word.c_str(), sizeStr.c_str());
        } else {
            std::string sizeStr = util::formatFileSize(totalSize);
            std::string formatted = brls::getStr("app/file_manager/selected_summary_format",
                                                 std::to_string(p.selectedPaths.size()),
                                                 std::to_string(filesCount),
                                                 std::to_string(dirsCount),
                                                 sizeStr);
            std::snprintf(buf, sizeof(buf), "%s", formatted.c_str());
        }

        std::string selPrefix = "app/file_manager/selected_count"_i18n;
        p.selectionText->setText(selPrefix + buf);
    }
}

void FileManagerView::toggleSelectionOnCell(int panelIdx, size_t index, FileManagerCell* cell) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    auto& p = panels_[panelIdx];
    if (p.hasParentDir && index == 0) {
        return; // Cannot select ".."
    }
    size_t itemIdx = p.hasParentDir ? (index - 1) : index;
    if (itemIdx >= p.items.size()) return;

    const std::string& path = p.items[itemIdx].path;
    bool isNowSelected = false;
    if (p.selectedPaths.count(path)) {
        p.selectedPaths.erase(path);
        isNowSelected = false;
    } else {
        p.selectedPaths.insert(path);
        isNowSelected = true;
    }

    if (cell) {
        cell->setSelectedVisual(isNowSelected);
    }

    updateSelectionBar(panelIdx);
}

void FileManagerView::toggleSelection(int panelIdx, size_t index) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    toggleSelectionOnCell(panelIdx, index, nullptr);
    brls::sync([this, panelIdx]() {
        if (panels_[panelIdx].recycler) {
            panels_[panelIdx].recycler->reloadData();
        }
    });
}

void FileManagerView::selectAll(int panelIdx) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    auto& p = panels_[panelIdx];
    p.selectedPaths.clear();
    for (const auto& item : p.items) {
        p.selectedPaths.insert(item.path);
    }
    updateSelectionBar(panelIdx);
    brls::sync([this, panelIdx]() {
        if (panels_[panelIdx].recycler) {
            panels_[panelIdx].recycler->reloadData();
        }
    });
}

void FileManagerView::clearSelection(int panelIdx) {
    if (panelIdx < 0 || panelIdx > 1) panelIdx = activePanel_;
    panels_[panelIdx].selectedPaths.clear();
    updateSelectionBar(panelIdx);
    brls::sync([this, panelIdx]() {
        if (panels_[panelIdx].recycler) {
            panels_[panelIdx].recycler->reloadData();
        }
    });
}

void FileManagerView::openTextViewer(const std::string& path, const std::string& name) {
    brls::Application::pushActivity(new TextViewerActivity(path, name));
}

void FileManagerView::showArchiveDialog(const util::FileItem& item) {
    util::logLine("FileManagerView: showArchiveDialog for " + item.path);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100.0f);
    content->setPadding(20.0f, 22.0f, 16.0f, 22.0f);

    // Header Box
    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(nvgRGBA(255, 110, 64, 80));

    // Icon Badge
    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);
    iconBadge->setBackgroundColor(nvgRGBA(255, 110, 64, 40));

    auto* badgeIcon = new brls::Label();
    badgeIcon->setText("\uE2C6"); // Archive
    badgeIcon->setFontSize(22.0f);
    badgeIcon->setTextColor(nvgRGB(255, 110, 64));
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText(item.name);
    titleLbl->setFontSize(18.0f);
    titleLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    auto* subLbl = new brls::Label();
    subLbl->setText("app/file_manager/type_archive"_i18n + util::formatFileSize(item.size));
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(nvgRGBA(255, 110, 64, 210));
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    auto* applet = dialog->getAppletFrame();
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(24, 26, 32, 252));
    }

    int restoreRow = panels_[activePanel_].currentFocusedRow;
    brls::View* firstOption = nullptr;

    auto addOption = [&firstOption, content, dialog, this, restoreRow, isLight](const std::string& iconGlyph, NVGcolor iconCol, const std::string& labelText, std::function<void()> action) {
        auto* row = new brls::Box();
        row->setHeight(42.0f);
        row->setWidthPercentage(100.0f);
        row->setFocusable(true);
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPaddingLeft(14.0f);
        row->setPaddingRight(14.0f);
        row->setMarginBottom(4.0f);
        row->setCornerRadius(8.0f);
        row->setBackgroundColor(isLight ? nvgRGBA(235, 240, 248, 200) : nvgRGBA(36, 39, 46, 190));

        auto* ic = new brls::Label();
        ic->setText(iconGlyph);
        ic->setFontSize(20.0f);
        ic->setTextColor(iconCol);
        ic->setMarginRight(14.0f);
        row->addView(ic);

        auto* lb = new brls::Label();
        lb->setText(labelText);
        lb->setFontSize(15.0f);
        lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        lb->setGrow(1.0f);
        row->addView(lb);

        row->getFocusEvent()->subscribe([lb, isLight](bool focused) {
            if (focused) {
                lb->setTextColor(isLight ? ThemeManager::instance().getAccentColor() : nvgRGB(255, 255, 255));
            } else {
                lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
            }
        });

        if (!firstOption) firstOption = row;

        row->registerClickAction([dialog, action, this, restoreRow, labelText](brls::View* v) {
            util::logLine("FileManagerView: showArchiveDialog option clicked: " + labelText);
            brls::sync([dialog, action, this, restoreRow]() {
                util::logLine("FileManagerView: closing archive option dialog");
                dialog->close([action, this, restoreRow]() {
                    util::logLine("FileManagerView: archive option dialog closed");
                    if (action) {
                        brls::sync([action]() {
                            util::logLine("FileManagerView: executing archive action");
                            action();
                        });
                    } else {
                        brls::sync([this, restoreRow]() {
                            auto* rec = panels_[activePanel_].recycler;
                            if (rec) {
                                int maxRow = static_cast<int>(panels_[activePanel_].items.size()) + (panels_[activePanel_].hasParentDir ? 1 : 0) - 1;
                                int validRow = std::clamp(restoreRow, 0, std::max(0, maxRow));
                                panels_[activePanel_].currentFocusedRow = validRow;
                                rec->setDefaultCellFocus(brls::IndexPath(0, validRow));
                                rec->selectRowAt(brls::IndexPath(0, validRow), false);
                                brls::Application::giveFocus(rec);
                            }
                        });
                    }
                });
            });
            return true;
        });

        content->addView(row);
    };

    auto executeExtract = [this, item](const std::string& targetDir) {
        std::string baseDir = targetDir;
        if (baseDir.empty()) baseDir = panels_[activePanel_].currentDir;

        std::string tmpDir = joinPath(baseDir, ".extract_tmp_" + std::to_string(std::time(nullptr)));
        std::string err;
        util::createFolder(tmpDir, err);

        auto* prog = new ArchiveProgressDialog(item.path, tmpDir, [this, item, tmpDir, targetDir, baseDir](bool ok, const std::string& msg) {
            if (ok) {
                if (targetDir == baseDir) {
                    std::string listErr;
                    std::vector<util::FileItem> extracted = util::listFolder(tmpDir, listErr);
                    bool allMoved = true;
                    for (const auto& e : extracted) {
                        std::string dst = joinPath(targetDir, e.name);
                        std::string moveErr;
                        if (!util::movePath(e.path, dst, moveErr)) {
                            allMoved = false;
                            util::logLine("FileManagerView: merge move failed: " + e.path + " -> " + dst + " (" + moveErr + ")");
                        }
                    }
                    std::string rmErr;
                    util::deletePathRecursive(tmpDir, rmErr);
                    if (allMoved) {
                        brls::Application::notify("app/file_manager/extract_success"_i18n);
                        refresh(activePanel_);
                        if (isSplitMode_) refresh(1 - activePanel_);
                        promptDeleteSourceFile(item.path, item.name);
                    } else {
                        brls::Application::notify("app/file_manager/extract_partial_fail"_i18n);
                        refresh(activePanel_);
                        if (isSplitMode_) refresh(1 - activePanel_);
                    }
                } else {
                    std::string err;
                    if (util::movePath(tmpDir, targetDir, err)) {
                        brls::Application::notify("app/file_manager/extract_success"_i18n);
                        refresh(activePanel_);
                        if (isSplitMode_) refresh(1 - activePanel_);
                        promptDeleteSourceFile(item.path, item.name);
                    } else {
                        util::logLine("FileManagerView: finalize move failed: " + tmpDir + " -> " + targetDir + " (" + err + ")");
                        brls::Application::notify("app/file_manager/extract_temp_fail"_i18n);
                        refresh(activePanel_);
                        if (isSplitMode_) refresh(1 - activePanel_);
                    }
                }
            } else {
                std::string rmErr;
                util::deletePathRecursive(tmpDir, rmErr);
                brls::Application::notify(msg.empty() ? "app/file_manager/extract_error"_i18n : msg);
                refresh(activePanel_);
                if (isSplitMode_) refresh(1 - activePanel_);
            }
        });
        prog->startExtraction();
    };

    // Option 1: Extract Here
    addOption("\uE2C6", nvgRGB(255, 110, 64), "app/file_manager/extract_here"_i18n, [this, executeExtract]() {
        executeExtract(panels_[activePanel_].currentDir);
    });

    // Option 2: Extract to Subfolder
    std::filesystem::path p(item.path);
    std::string stem = p.stem().generic_string();
    std::string subfolderDir = joinPath(panels_[activePanel_].currentDir, stem);
    std::string toFolderText = brls::getStr("app/file_manager/extract_to_folder_named", stem);
    addOption("\uE2CC", ThemeManager::instance().getAccentColor(), toFolderText, [this, executeExtract, subfolderDir]() {
        executeExtract(subfolderDir);
    });

    // Option 3: Extract to Opposite Panel (if split mode active)
    if (isSplitMode_) {
        int oppIdx = 1 - activePanel_;
        const std::string& oppDir = panels_[oppIdx].currentDir;
        std::filesystem::path op(oppDir);
        std::string oppDirName = op.filename().generic_string();
        if (oppDirName.empty()) oppDirName = oppDir;
        addOption("\uE14D", nvgRGB(64, 196, 255), brls::getStr("app/file_manager/extract_to_other_panel", oppDirName), [this, executeExtract, oppDir]() {
            executeExtract(oppDir);
        });
    }

    // Cancel Option
    addOption("\uE5CD", nvgRGB(239, 83, 80), "hints/cancel"_i18n, nullptr);

    if (firstOption) {
        dialog->setLastFocusedView(firstOption);
        content->setLastFocusedView(firstOption);
    }

    util::logLine("FileManagerView: showArchiveDialog opening dialog");
    dialog->open();
    util::logLine("FileManagerView: showArchiveDialog dialog opened successfully");

    if (firstOption) {
        brls::sync([dialog, firstOption]() {
            dialog->setLastFocusedView(firstOption);
            brls::Application::giveFocus(firstOption);
        });
    }
}

void FileManagerView::showCreateArchiveDialog(const std::vector<std::string>& targets) {
    if (targets.empty()) return;

    std::string defaultName;
    if (targets.size() == 1) {
        std::filesystem::path p(targets[0]);
        defaultName = p.stem().generic_string() + ".zip";
    } else {
        std::filesystem::path cur(panels_[activePanel_].currentDir);
        std::string folderName = cur.filename().generic_string();
        if (folderName.empty() || folderName == "." || folderName == "/") {
            folderName = "Archive";
        }
        defaultName = folderName + ".zip";
    }

    std::string promptTitle = brls::getStr("app/file_manager/create_archive_title");
    int actIdx = activePanel_;

    brls::Application::getImeManager()->openForText([this, actIdx, targets](std::string fileName) {
        if (fileName.empty()) return;

        if (fileName.size() < 4 || fileName.substr(fileName.size() - 4) != ".zip") {
            fileName += ".zip";
        }

        std::string targetArchivePath = joinPath(panels_[actIdx].currentDir, fileName);

        if (std::filesystem::exists(targetArchivePath)) {
            std::string confirmMsg = brls::getStr("app/file_manager/archive_exists_overwrite", fileName);
            auto* confirmDialog = new brls::Dialog(confirmMsg);
            confirmDialog->setCancelable(true);
            confirmDialog->addButton("app/common/yes"_i18n, [this, actIdx, targetArchivePath, targets, fileName]() {
                auto* dlg = new ArchiveProgressDialog(targetArchivePath, targets, panels_[actIdx].currentDir, [this, fileName, actIdx](bool ok, const std::string& msg) {
                    if (ok) {
                        panels_[actIdx].selectedPaths.clear();
                        brls::Application::notify("app/file_manager/create_archive_success"_i18n);
                        refresh(actIdx, fileName);
                        if (isSplitMode_) refresh(1 - actIdx);
                    } else {
                        brls::Application::notify(msg.empty() ? "app/file_manager/create_archive_error"_i18n : msg);
                        refresh(actIdx);
                        if (isSplitMode_) refresh(1 - actIdx);
                    }
                });
                dlg->startCreation();
            });
            confirmDialog->addButton("app/common/cancel"_i18n, []() {});
            confirmDialog->open();
            return;
        }

        auto* dlg = new ArchiveProgressDialog(targetArchivePath, targets, panels_[actIdx].currentDir, [this, fileName, actIdx](bool ok, const std::string& msg) {
            if (ok) {
                panels_[actIdx].selectedPaths.clear();
                brls::Application::notify("app/file_manager/create_archive_success"_i18n);
                refresh(actIdx, fileName);
                if (isSplitMode_) refresh(1 - actIdx);
            } else {
                brls::Application::notify(msg.empty() ? "app/file_manager/create_archive_error"_i18n : msg);
                refresh(actIdx);
                if (isSplitMode_) refresh(1 - actIdx);
            }
        });
        dlg->startCreation();
    }, promptTitle, "", 64, defaultName);
}

void FileManagerView::showInstallDialog(const util::FileItem& item) {
    util::logLine("FileManagerView: showInstallDialog for " + item.path);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100.0f);
    content->setPadding(20.0f, 22.0f, 16.0f, 22.0f);

    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(ThemeManager::instance().getMediumAccentColor());

    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);
    iconBadge->setBackgroundColor(ThemeManager::instance().getDimAccentColor());

    auto* badgeIcon = new brls::Label();
    badgeIcon->setText("\uE0E0"); // Gamepad
    badgeIcon->setFontSize(22.0f);
    badgeIcon->setTextColor(ThemeManager::instance().getAccentColor());
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText(item.name);
    titleLbl->setFontSize(18.0f);
    titleLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    auto* subLbl = new brls::Label();
    subLbl->setText("app/file_manager/type_package"_i18n + util::formatFileSize(item.size));
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(ThemeManager::instance().getAccentColor());
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    auto* applet = dialog->getAppletFrame();
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(24, 26, 32, 252));
    }

    int restoreRow = panels_[activePanel_].currentFocusedRow;
    brls::View* firstOption = nullptr;

    auto addOption = [&firstOption, content, dialog, this, restoreRow, isLight](const std::string& iconGlyph, NVGcolor iconCol, const std::string& labelText, const std::string& spaceText, std::function<void()> action) {
        auto* row = new brls::Box();
        row->setHeight(48.0f);
        row->setWidthPercentage(100.0f);
        row->setFocusable(true);
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPaddingLeft(14.0f);
        row->setPaddingRight(14.0f);
        row->setMarginBottom(4.0f);
        row->setCornerRadius(8.0f);
        row->setBackgroundColor(isLight ? nvgRGBA(235, 240, 248, 200) : nvgRGBA(36, 39, 46, 190));

        auto* ic = new brls::Label();
        ic->setText(iconGlyph);
        ic->setFontSize(22.0f);
        ic->setTextColor(iconCol);
        ic->setMarginRight(14.0f);
        row->addView(ic);

        auto* colText = new brls::Box();
        colText->setAxis(brls::Axis::COLUMN);
        colText->setGrow(1.0f);

        auto* lb = new brls::Label();
        lb->setText(labelText);
        lb->setFontSize(15.0f);
        lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        colText->addView(lb);

        auto* sp = spaceText.empty() ? nullptr : new brls::Label();
        if (sp) {
            sp->setText(spaceText);
            sp->setFontSize(12.0f);
            sp->setTextColor(ThemeManager::instance().getTextSecondaryColor());
            colText->addView(sp);
        }

        row->addView(colText);

        row->getFocusEvent()->subscribe([lb, sp, isLight](bool focused) {
            if (focused) {
                lb->setTextColor(isLight ? ThemeManager::instance().getAccentColor() : nvgRGB(255, 255, 255));
                if (sp) sp->setTextColor(isLight ? ThemeManager::instance().getTextPrimaryColor() : nvgRGB(200, 210, 220));
            } else {
                lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
                if (sp) sp->setTextColor(ThemeManager::instance().getTextSecondaryColor());
            }
        });

        if (!firstOption) firstOption = row;

        row->registerClickAction([dialog, action, this, restoreRow, labelText](brls::View* v) {
            util::logLine("FileManagerView: showInstallDialog option clicked: " + labelText);
            brls::sync([dialog, action, this, restoreRow]() {
                util::logLine("FileManagerView: closing install option dialog");
                dialog->close([action, this, restoreRow]() {
                    util::logLine("FileManagerView: install option dialog closed");
                    if (action) {
                        brls::sync([action]() {
                            util::logLine("FileManagerView: executing install action");
                            action();
                        });
                    } else {
                        brls::sync([this, restoreRow]() {
                            auto* rec = panels_[activePanel_].recycler;
                            if (rec) {
                                int maxRow = static_cast<int>(panels_[activePanel_].items.size()) + (panels_[activePanel_].hasParentDir ? 1 : 0) - 1;
                                int validRow = std::clamp(restoreRow, 0, std::max(0, maxRow));
                                panels_[activePanel_].currentFocusedRow = validRow;
                                rec->setDefaultCellFocus(brls::IndexPath(0, validRow));
                                rec->selectRowAt(brls::IndexPath(0, validRow), false);
                                brls::Application::giveFocus(rec);
                            }
                        });
                    }
                });
            });
            return true;
        });

        content->addView(row);
    };

    uint64_t sdFree = 0, sdTotal = 0;
    util::getStorageSpace("sdmc:/", sdFree, sdTotal);
    std::string sdSpaceInfo = sdTotal > 0 ? ("app/file_manager/free_prefix"_i18n + util::formatFileSize(sdFree)) : "";

    addOption("\uE2C7", ThemeManager::instance().getAccentColor(), "app/file_manager/install_to_sd"_i18n, sdSpaceInfo, [this, item]() {
        auto* prog = new InstallProgressDialog(item.path, 1, [this, item](bool ok, const std::string& err) {
            if (ok) {
                brls::Application::notify("app/file_manager/install_success"_i18n);
                promptDeleteSourceFile(item.path, item.name);
            } else {
                brls::Application::notify(err.empty() ? "app/file_manager/install_error"_i18n : err);
                refresh(activePanel_);
            }
        });
        prog->startInstallation();
    });

#if defined(__SWITCH__)
    uint64_t nandFree = 0, nandTotal = 0;
    util::getStorageSpace("user:/", nandFree, nandTotal);
    std::string nandSpaceInfo = nandTotal > 0 ? ("app/file_manager/free_prefix"_i18n + util::formatFileSize(nandFree)) : "";

    addOption("\uE318", nvgRGB(255, 179, 0), "app/file_manager/install_to_nand"_i18n, nandSpaceInfo, [this, item]() {
        auto* prog = new InstallProgressDialog(item.path, 0, [this, item](bool ok, const std::string& err) {
            if (ok) {
                brls::Application::notify("app/file_manager/install_success"_i18n);
                promptDeleteSourceFile(item.path, item.name);
            } else {
                brls::Application::notify(err.empty() ? "app/file_manager/install_error"_i18n : err);
                refresh(activePanel_);
            }
        });
        prog->startInstallation();
    });
#endif

    addOption("\uE5CD", nvgRGB(239, 83, 80), "hints/cancel"_i18n, "", nullptr);

    if (firstOption) {
        dialog->setLastFocusedView(firstOption);
        content->setLastFocusedView(firstOption);
    }

    util::logLine("FileManagerView: showInstallDialog opening dialog");
    dialog->open();
    util::logLine("FileManagerView: showInstallDialog dialog opened successfully");

    if (firstOption) {
        brls::sync([dialog, firstOption]() {
            dialog->setLastFocusedView(firstOption);
            brls::Application::giveFocus(firstOption);
        });
    }
}

void FileManagerView::promptDeleteSourceFile(const std::string& filePath, const std::string& fileName) {
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100.0f);
    content->setPadding(20.0f, 22.0f, 16.0f, 22.0f);

    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(nvgRGBA(255, 179, 0, 80));

    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);
    iconBadge->setBackgroundColor(nvgRGBA(255, 179, 0, 40));

    auto* badgeIcon = new brls::Label();
    badgeIcon->setText("\uE872"); // Trash icon
    badgeIcon->setFontSize(22.0f);
    badgeIcon->setTextColor(nvgRGB(255, 179, 0));
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText("app/file_manager/free_space_prompt"_i18n);
    titleLbl->setFontSize(18.0f);
    titleLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    auto* subLbl = new brls::Label();
    subLbl->setText("app/file_manager/delete_source_prompt"_i18n + fileName);
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(isLight ? nvgRGB(180, 100, 0) : nvgRGBA(255, 179, 0, 210));
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    auto* applet = dialog->getAppletFrame();
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(24, 26, 32, 252));
    }

    int restoreRow = panels_[activePanel_].currentFocusedRow;
    brls::View* firstOption = nullptr;

    auto addOption = [&firstOption, content, dialog, this, restoreRow, isLight](const std::string& iconGlyph, NVGcolor iconCol, const std::string& labelText, std::function<void()> action) {
        auto* row = new brls::Box();
        row->setHeight(42.0f);
        row->setWidthPercentage(100.0f);
        row->setFocusable(true);
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPaddingLeft(14.0f);
        row->setPaddingRight(14.0f);
        row->setMarginBottom(4.0f);
        row->setCornerRadius(8.0f);
        row->setBackgroundColor(isLight ? nvgRGBA(235, 240, 248, 200) : nvgRGBA(36, 39, 46, 190));

        auto* ic = new brls::Label();
        ic->setText(iconGlyph);
        ic->setFontSize(20.0f);
        ic->setTextColor(iconCol);
        ic->setMarginRight(14.0f);
        row->addView(ic);

        auto* lb = new brls::Label();
        lb->setText(labelText);
        lb->setFontSize(15.0f);
        lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        lb->setGrow(1.0f);
        row->addView(lb);

        row->getFocusEvent()->subscribe([lb, isLight](bool focused) {
            if (focused) {
                lb->setTextColor(isLight ? ThemeManager::instance().getAccentColor() : nvgRGB(255, 255, 255));
            } else {
                lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
            }
        });

        if (!firstOption) firstOption = row;

        row->registerClickAction([dialog, action, this, restoreRow](brls::View* v) {
            brls::sync([dialog, action, this, restoreRow]() {
                dialog->close([action, this, restoreRow]() {
                    if (action) {
                        brls::sync([action]() {
                            action();
                        });
                    } else {
                        brls::sync([this, restoreRow]() {
                            auto* rec = panels_[activePanel_].recycler;
                            if (rec) {
                                int maxRow = static_cast<int>(panels_[activePanel_].items.size()) + (panels_[activePanel_].hasParentDir ? 1 : 0) - 1;
                                int validRow = std::clamp(restoreRow, 0, std::max(0, maxRow));
                                panels_[activePanel_].currentFocusedRow = validRow;
                                rec->setDefaultCellFocus(brls::IndexPath(0, validRow));
                                rec->selectRowAt(brls::IndexPath(0, validRow), false);
                                brls::Application::giveFocus(rec);
                            }
                        });
                    }
                });
            });
            return true;
        });

        content->addView(row);
    };

    addOption("\uE872", nvgRGB(255, 82, 82), "app/file_manager/delete_file_btn"_i18n, [this, filePath]() {
        std::string err;
        if (util::deletePathRecursive(filePath, err)) {
            brls::Application::notify("app/file_manager/source_deleted"_i18n);
        } else {
            brls::Application::notify("app/file_manager/delete_failed"_i18n);
        }
        refresh(activePanel_);
        if (isSplitMode_) refresh(1 - activePanel_);
    });

    addOption("\uE5CD", nvgRGB(140, 150, 160), "app/file_manager/keep_file_btn"_i18n, nullptr);

    if (firstOption) {
        dialog->setLastFocusedView(firstOption);
        content->setLastFocusedView(firstOption);
    }

    dialog->open();

    if (firstOption) {
        brls::sync([dialog, firstOption]() {
            dialog->setLastFocusedView(firstOption);
            brls::Application::giveFocus(firstOption);
        });
    }
}

void FileManagerView::showDeleteConfirmDialog() {
    auto& cur = panels_[activePanel_];
    if (cur.selectedPaths.empty()) return;

    std::string msg = brls::getStr("app/file_manager/confirm_delete_selected", std::to_string(cur.selectedPaths.size()));

    auto* dialog = new brls::Dialog(msg);
    dialog->setCancelable(true);

    int actIdx = activePanel_;
    dialog->addButton("app/common/yes"_i18n, [this, actIdx]() {
        std::vector<std::string> toDelete(panels_[actIdx].selectedPaths.begin(), panels_[actIdx].selectedPaths.end());
        std::string err;
        if (util::deleteMultiplePaths(toDelete, err)) {
            brls::Application::notify("app/file_manager/deleted_success"_i18n);
        } else {
            brls::Application::notify(err.empty() ? "app/common/error"_i18n : err);
        }
        panels_[actIdx].selectedPaths.clear();
        refresh(actIdx);
        if (isSplitMode_) refresh(1 - actIdx);
    });

    dialog->addButton("app/common/cancel"_i18n, []() {});
    dialog->open();
}

void FileManagerView::showNewFolderDialog() {
    int actIdx = activePanel_;
    brls::Application::getImeManager()->openForText([this, actIdx](std::string text) {
        if (text.empty()) return;
        std::string newPath = joinPath(panels_[actIdx].currentDir, text);
        std::string err;
        if (util::createFolder(newPath, err)) {
            brls::Application::notify("app/file_manager/folder_created"_i18n);
            refresh(actIdx);
            if (isSplitMode_) refresh(1 - actIdx);
        } else {
            brls::Application::notify(err.empty() ? "app/common/error"_i18n : err);
        }
    }, brls::getStr("app/file_manager/new_folder"), "", 64, "New_Folder");
}

void FileManagerView::showRenameDialog(const util::FileItem& item) {
    int actIdx = activePanel_;
    brls::Application::getImeManager()->openForText([this, actIdx, item](std::string text) {
        if (text.empty() || text == item.name) return;
        std::string err;
        if (util::renameItem(item.path, text, err)) {
            brls::Application::notify("app/file_manager/renamed_success"_i18n);
            panels_[actIdx].selectedPaths.clear();
            refresh(actIdx);
            if (isSplitMode_) refresh(1 - actIdx);
        } else {
            brls::Application::notify(err.empty() ? "app/common/error"_i18n : err);
        }
    }, brls::getStr("app/file_manager/rename"), "", 64, item.name);
}

void FileManagerView::pasteClipboard() {
    auto& clip = util::getClipboard();
    if (clip.paths.empty() || clip.op == util::ClipboardOp::None) return;

    bool isCut = (clip.op == util::ClipboardOp::Cut);
    std::vector<std::string> paths = clip.paths;
    int actIdx = activePanel_;
    std::string targetDir = panels_[actIdx].currentDir;

    brls::async([this, paths, isCut, actIdx, targetDir]() {
        bool allOk = true;
        std::string lastErr;

        for (const auto& src : paths) {
            std::string cleanSrc = src;
            while (cleanSrc.size() > 1 && (cleanSrc.back() == '/' || cleanSrc.back() == '\\')) {
                cleanSrc.pop_back();
            }
            std::filesystem::path sp(cleanSrc);
            std::string dest = joinPath(targetDir, sp.filename().generic_string());
            std::string err;

            util::logLine("FileManagerView: pasteClipboard src=" + cleanSrc + " dest=" + dest + " isCut=" + std::to_string(isCut));

            if (isCut) {
                if (!util::movePath(cleanSrc, dest, err)) {
                    allOk = false;
                    lastErr = err;
                    util::logLine("FileManagerView: pasteClipboard movePath failed: " + err);
                }
            } else {
                if (!util::copyPathRecursive(cleanSrc, dest, nullptr, nullptr, err)) {
                    allOk = false;
                    lastErr = err;
                    util::logLine("FileManagerView: pasteClipboard copyPathRecursive failed: " + err);
                }
            }
        }

        if (isCut) {
            util::clearClipboard();
        }

        brls::sync([this, allOk, lastErr, actIdx]() {
            if (allOk) {
                brls::Application::notify("app/file_manager/paste_success"_i18n);
            } else {
                brls::Application::notify(lastErr.empty() ? "app/common/error"_i18n : lastErr);
            }
            refresh(actIdx);
            if (isSplitMode_) refresh(1 - actIdx);
        });
    });
}

void FileManagerView::copyToOppositePanel() {
    if (!isSplitMode_) return;
    int oppIdx = 1 - activePanel_;
    const std::string& oppDir = panels_[oppIdx].currentDir;
    if (oppDir.empty()) return;

    std::vector<std::string> targets;
    auto& cur = panels_[activePanel_];
    if (!cur.selectedPaths.empty()) {
        targets.assign(cur.selectedPaths.begin(), cur.selectedPaths.end());
    } else if (cur.currentFocusedRow >= 0) {
        size_t idx = cur.hasParentDir ? (cur.currentFocusedRow - 1) : cur.currentFocusedRow;
        if ((cur.currentFocusedRow > 0 || !cur.hasParentDir) && idx < cur.items.size()) {
            targets.push_back(cur.items[idx].path);
        }
    }
    if (targets.empty()) return;

    std::string cleanOpp = normalizeDir(oppDir);
    while (cleanOpp.size() > 1 && cleanOpp.back() == '/') cleanOpp.pop_back();

    for (const auto& src : targets) {
        std::string cleanSrc = normalizeDir(src);
        while (cleanSrc.size() > 1 && cleanSrc.back() == '/') cleanSrc.pop_back();

        bool isIntoItself = false;
#if defined(_WIN32) || defined(PLATFORM_DESKTOP)
        std::string s1 = cleanOpp, s2 = cleanSrc;
        std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower);
        std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
        if (s1 == s2 || (s1.size() > s2.size() && s1.rfind(s2 + "/", 0) == 0)) {
            isIntoItself = true;
        }
#else
        if (cleanOpp == cleanSrc || (cleanOpp.size() > cleanSrc.size() && cleanOpp.rfind(cleanSrc + "/", 0) == 0)) {
            isIntoItself = true;
        }
#endif
        if (isIntoItself) {
            brls::Application::notify("app/file_manager/cannot_copy_into_itself"_i18n);
            return;
        }
    }

    int actIdx = activePanel_;
    brls::async([this, targets, cleanOpp, actIdx, oppIdx]() {
        bool allOk = true;
        std::string lastErr;
        for (const auto& src : targets) {
            std::string cleanSrc = normalizeDir(src);
            while (cleanSrc.size() > 1 && (cleanSrc.back() == '/' || cleanSrc.back() == '\\')) {
                cleanSrc.pop_back();
            }
            std::filesystem::path sp(cleanSrc);
            std::string dest = joinPath(cleanOpp, sp.filename().generic_string());

            if (cleanSrc == dest) {
                std::string stem = sp.stem().generic_string();
                std::string ext = sp.extension().generic_string();
                dest = joinPath(cleanOpp, stem + "_copy" + ext);
                int copyIdx = 2;
                std::error_code ec;
                while (std::filesystem::exists(dest, ec)) {
                    dest = joinPath(cleanOpp, stem + "_copy" + std::to_string(copyIdx++) + ext);
                }
            }

            std::string err;
            if (!util::copyPathRecursive(cleanSrc, dest, nullptr, nullptr, err)) {
                allOk = false;
                lastErr = err;
            }
        }
        brls::sync([this, allOk, lastErr, actIdx, oppIdx]() {
            if (allOk) {
                brls::Application::notify("app/file_manager/copied_success"_i18n);
            } else {
                brls::Application::notify(lastErr.empty() ? "app/common/error"_i18n : lastErr);
            }
            refresh(actIdx);
            refresh(oppIdx);
        });
    });
}

void FileManagerView::moveToOppositePanel() {
    if (!isSplitMode_) return;
    int oppIdx = 1 - activePanel_;
    const std::string& oppDir = panels_[oppIdx].currentDir;
    if (oppDir.empty()) return;

    std::vector<std::string> targets;
    auto& cur = panels_[activePanel_];
    if (!cur.selectedPaths.empty()) {
        targets.assign(cur.selectedPaths.begin(), cur.selectedPaths.end());
    } else if (cur.currentFocusedRow >= 0) {
        size_t idx = cur.hasParentDir ? (cur.currentFocusedRow - 1) : cur.currentFocusedRow;
        if ((cur.currentFocusedRow > 0 || !cur.hasParentDir) && idx < cur.items.size()) {
            targets.push_back(cur.items[idx].path);
        }
    }
    if (targets.empty()) return;

    std::string cleanOpp = normalizeDir(oppDir);
    while (cleanOpp.size() > 1 && cleanOpp.back() == '/') cleanOpp.pop_back();

    std::string cleanAct = normalizeDir(cur.currentDir);
    while (cleanAct.size() > 1 && cleanAct.back() == '/') cleanAct.pop_back();

    if (cleanOpp == cleanAct) {
        brls::Application::notify("app/file_manager/cannot_copy_into_itself"_i18n);
        return;
    }

    for (const auto& src : targets) {
        std::string cleanSrc = normalizeDir(src);
        while (cleanSrc.size() > 1 && cleanSrc.back() == '/') cleanSrc.pop_back();

        bool isIntoItself = false;
#if defined(_WIN32) || defined(PLATFORM_DESKTOP)
        std::string s1 = cleanOpp, s2 = cleanSrc;
        std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower);
        std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
        if (s1 == s2 || (s1.size() > s2.size() && s1.rfind(s2 + "/", 0) == 0)) {
            isIntoItself = true;
        }
#else
        if (cleanOpp == cleanSrc || (cleanOpp.size() > cleanSrc.size() && cleanOpp.rfind(cleanSrc + "/", 0) == 0)) {
            isIntoItself = true;
        }
#endif
        if (isIntoItself) {
            brls::Application::notify("app/file_manager/cannot_copy_into_itself"_i18n);
            return;
        }
    }

    int actIdx = activePanel_;
    brls::async([this, targets, cleanOpp, actIdx, oppIdx]() {
        bool allOk = true;
        std::string lastErr;
        for (const auto& src : targets) {
            std::string cleanSrc = normalizeDir(src);
            while (cleanSrc.size() > 1 && (cleanSrc.back() == '/' || cleanSrc.back() == '\\')) {
                cleanSrc.pop_back();
            }
            std::filesystem::path sp(cleanSrc);
            std::string dest = joinPath(cleanOpp, sp.filename().generic_string());
            std::string err;
            if (!util::movePath(cleanSrc, dest, err)) {
                allOk = false;
                lastErr = err;
            }
        }
        brls::sync([this, allOk, lastErr, actIdx, oppIdx]() {
            if (allOk) {
                panels_[actIdx].selectedPaths.clear();
                brls::Application::notify("app/file_manager/move_success"_i18n);
            } else {
                brls::Application::notify(lastErr.empty() ? "app/common/error"_i18n : lastErr);
            }
            refresh(actIdx);
            refresh(oppIdx);
        });
    });
}

void FileManagerView::showActionsMenu() {
    auto& cur = panels_[activePanel_];
    int restoreRow = cur.currentFocusedRow;

    auto& clip = util::getClipboard();
    bool hasSelection = !cur.selectedPaths.empty();
    bool hasClipboard = (!clip.paths.empty() && clip.op != util::ClipboardOp::None);

    // Identify target items
    const util::FileItem* targetSingleItem = nullptr;
    if (cur.selectedPaths.size() == 1) {
        std::string sel = *cur.selectedPaths.begin();
        for (const auto& it : cur.items) {
            if (it.path == sel) { targetSingleItem = &it; break; }
        }
    } else if (!hasSelection) {
        if (cur.currentFocusedRow >= 0) {
            size_t idx = cur.hasParentDir ? (cur.currentFocusedRow - 1) : cur.currentFocusedRow;
            if ((cur.currentFocusedRow > 0 || !cur.hasParentDir) && idx < cur.items.size()) {
                targetSingleItem = &cur.items[idx];
            }
        }
    }

    // Outer content inside dialog
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100.0f);
    content->setPadding(20.0f, 22.0f, 16.0f, 22.0f);

    // Header Card
    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(ThemeManager::instance().getMediumAccentColor());

    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);

    auto* badgeIcon = new brls::Label();
    badgeIcon->setFontSize(22.0f);

    std::string archPath, innerPath;
    bool inArchive = util::parseArchiveVirtualPath(cur.currentDir, archPath, innerPath);

    std::string titleText, subtitleText;
    if (inArchive) {
        if (targetSingleItem) {
            if (targetSingleItem->isDir) {
                iconBadge->setBackgroundColor(nvgRGBA(255, 193, 7, 40));
                badgeIcon->setText("\uE2C7"); // Folder
                badgeIcon->setTextColor(nvgRGB(255, 193, 7));
                titleText = targetSingleItem->name;
                subtitleText = "app/archive/folder_type"_i18n;
            } else {
                iconBadge->setBackgroundColor(ThemeManager::instance().getDimAccentColor());
                badgeIcon->setText("\uE24D"); // File
                badgeIcon->setTextColor(ThemeManager::instance().getAccentColor());
                titleText = targetSingleItem->name;
                subtitleText = util::formatFileSize(targetSingleItem->size);
            }
        } else {
            iconBadge->setBackgroundColor(nvgRGBA(255, 110, 64, 40));
            badgeIcon->setText("\uE2C6"); // Archive
            badgeIcon->setTextColor(nvgRGB(255, 110, 64));
            std::filesystem::path ap(archPath);
            titleText = ap.filename().generic_string();
            subtitleText = innerPath.empty() ? "/" : ("/" + innerPath);
        }
    } else if (hasSelection && cur.selectedPaths.size() > 1) {
        iconBadge->setBackgroundColor(ThemeManager::instance().getDimAccentColor());
        badgeIcon->setText("\uE834"); // Multiple select icon
        badgeIcon->setTextColor(ThemeManager::instance().getAccentColor());
        titleText = "app/file_manager/selected_items_count"_i18n + std::to_string(cur.selectedPaths.size());
        subtitleText = "app/file_manager/bulk_operations"_i18n;
    } else if (targetSingleItem) {
        if (targetSingleItem->isDir) {
            iconBadge->setBackgroundColor(nvgRGBA(255, 193, 7, 40));
            badgeIcon->setText("\uE2C7");
            badgeIcon->setTextColor(nvgRGB(255, 193, 7));
            titleText = targetSingleItem->name;
            subtitleText = "app/file_manager/folder_type"_i18n;
        } else if (util::isArchiveFile(targetSingleItem->path)) {
            iconBadge->setBackgroundColor(nvgRGBA(255, 110, 64, 40));
            badgeIcon->setText("\uE2C6");
            badgeIcon->setTextColor(nvgRGB(255, 110, 64));
            titleText = targetSingleItem->name;
            subtitleText = "app/file_manager/type_archive"_i18n + util::formatFileSize(targetSingleItem->size);
        } else if (util::isGamePackage(targetSingleItem->path)) {
            iconBadge->setBackgroundColor(ThemeManager::instance().getDimAccentColor());
            badgeIcon->setText("\uE0E0");
            badgeIcon->setTextColor(ThemeManager::instance().getAccentColor());
            titleText = targetSingleItem->name;
            subtitleText = "app/file_manager/type_game_pkg"_i18n + util::formatFileSize(targetSingleItem->size);
        } else if (isTextFile(targetSingleItem->path)) {
            iconBadge->setBackgroundColor(ThemeManager::instance().getDimAccentColor());
            badgeIcon->setText("\uE873");
            badgeIcon->setTextColor(ThemeManager::instance().getAccentColor());
            titleText = targetSingleItem->name;
            subtitleText = "app/file_manager/type_text"_i18n + util::formatFileSize(targetSingleItem->size);
        } else {
            iconBadge->setBackgroundColor(nvgRGBA(33, 150, 243, 40));
            badgeIcon->setText("\uE24D");
            badgeIcon->setTextColor(nvgRGB(33, 150, 243));
            titleText = targetSingleItem->name;
            subtitleText = util::formatFileSize(targetSingleItem->size);
        }
    } else {
        iconBadge->setBackgroundColor(ThemeManager::instance().getDimAccentColor());
        badgeIcon->setText("\uE2C7");
        badgeIcon->setTextColor(ThemeManager::instance().getAccentColor());
        titleText = "app/file_manager/action_menu"_i18n;
        subtitleText = cur.currentDir;
    }
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText(titleText);
    titleLbl->setFontSize(18.0f);
    titleLbl->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    auto* subLbl = new brls::Label();
    subLbl->setText(subtitleText);
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(ThemeManager::instance().getAccentColor());
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    auto* applet = dynamic_cast<brls::AppletFrame*>(dialog->getView("brls/dialog/applet"));
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(24, 26, 32, 252));
    }

    brls::View* firstOption = nullptr;

    auto addOption = [&firstOption, content, dialog, this, restoreRow, isLight](const std::string& iconGlyph, NVGcolor iconCol, const std::string& labelText, std::function<void()> action, bool opensSubDialog = false) {
        auto* row = new brls::Box();
        row->setHeight(42.0f);
        row->setWidthPercentage(100.0f);
        row->setFocusable(true);
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPaddingLeft(14.0f);
        row->setPaddingRight(14.0f);
        row->setMarginBottom(3.0f);
        row->setCornerRadius(8.0f);
        row->setBackgroundColor(isLight ? nvgRGBA(235, 240, 248, 200) : nvgRGBA(36, 39, 46, 190));

        auto* ic = new brls::Label();
        ic->setText(iconGlyph);
        ic->setFontSize(20.0f);
        ic->setTextColor(iconCol);
        ic->setMarginRight(14.0f);
        row->addView(ic);

        auto* lb = new brls::Label();
        lb->setText(labelText);
        lb->setFontSize(15.0f);
        lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        lb->setGrow(1.0f);
        row->addView(lb);

        row->getFocusEvent()->subscribe([lb, isLight](bool focused) {
            if (focused) {
                lb->setTextColor(isLight ? ThemeManager::instance().getAccentColor() : nvgRGB(255, 255, 255));
            } else {
                lb->setTextColor(ThemeManager::instance().getTextPrimaryColor());
            }
        });

        if (!firstOption) firstOption = row;

        row->registerClickAction([dialog, action, this, restoreRow, opensSubDialog, labelText](brls::View* v) {
            util::logLine("FileManagerView: showActionsMenu option clicked: " + labelText);
            brls::sync([dialog, action, this, restoreRow, opensSubDialog]() {
                dialog->close([action, this, restoreRow, opensSubDialog]() {
                    if (action) {
                        brls::sync([action]() {
                            action();
                        });
                    }
                    if (!opensSubDialog) {
                        brls::sync([this, restoreRow]() {
                            auto* rec = panels_[activePanel_].recycler;
                            if (rec) {
                                int maxRow = static_cast<int>(panels_[activePanel_].items.size()) + (panels_[activePanel_].hasParentDir ? 1 : 0) - 1;
                                int validRow = std::clamp(restoreRow, 0, std::max(0, maxRow));
                                panels_[activePanel_].currentFocusedRow = validRow;
                                rec->setDefaultCellFocus(brls::IndexPath(0, validRow));
                                rec->selectRowAt(brls::IndexPath(0, validRow), false);
                                brls::Application::giveFocus(rec);
                            }
                        });
                    }
                });
            });
            return true;
        });

        content->addView(row);
    };

    // Calculate opposite panel details for dual-pane mode
    std::string oppDirName;
    int oppIdx = 1 - activePanel_;
    if (isSplitMode_) {
        const std::string& oppDir = panels_[oppIdx].currentDir;
        std::filesystem::path op(oppDir);
        oppDirName = op.filename().generic_string();
        if (oppDirName.empty() || oppDirName == "/" || oppDirName == ".") oppDirName = oppDir;
    }

    if (inArchive) {
        // Option 1: Extract entire archive
        addOption("\uE2C6", nvgRGB(255, 110, 64), "app/archive/extract_entire_archive"_i18n, [this, archPath]() {
            util::FileItem archItem;
            archItem.path = archPath;
            std::filesystem::path ap(archPath);
            archItem.name = ap.filename().generic_string();
            archItem.isDir = false;
            showArchiveDialog(archItem);
        }, true);

        // Option 2: Extract selected file
        if (targetSingleItem && !targetSingleItem->isDir) {
            addOption("\uE2C6", ThemeManager::instance().getAccentColor(), "app/archive/extract_this_file"_i18n, [this, archPath, item = *targetSingleItem]() {
                std::filesystem::path ap(archPath);
                std::string baseDir = ap.parent_path().generic_string();
                std::string relInner = item.path.substr(archPath.size());
                while (!relInner.empty() && (relInner.front() == '/' || relInner.front() == '\\')) relInner.erase(relInner.begin());
                std::string err;
                bool ok = util::extractSingleFileFromArchive(archPath, relInner, baseDir, nullptr, nullptr, err);
                if (ok) {
                    brls::Application::notify("app/file_manager/archive_complete"_i18n);
                } else {
                    brls::Application::notify(err.empty() ? "app/file_manager/extract_error"_i18n : err);
                }
            }, true);

            // Option 3: Extract this file to opposite panel (if split mode active)
            if (isSplitMode_) {
                addOption("\uE14D", nvgRGB(64, 196, 255), brls::getStr("app/file_manager/extract_to_other_panel", oppDirName), [this, archPath, oppIdx, item = *targetSingleItem]() {
                    const std::string& oppDir = panels_[oppIdx].currentDir;
                    std::string relInner = item.path.substr(archPath.size());
                    while (!relInner.empty() && (relInner.front() == '/' || relInner.front() == '\\')) relInner.erase(relInner.begin());
                    std::string err;
                    bool ok = util::extractSingleFileFromArchive(archPath, relInner, oppDir, nullptr, nullptr, err);
                    if (ok) {
                        brls::Application::notify("app/file_manager/archive_complete"_i18n);
                        refresh(oppIdx);
                    } else {
                        brls::Application::notify(err.empty() ? "app/file_manager/extract_error"_i18n : err);
                    }
                }, true);
            }
        }
    } else {
        // Inter-panel Copy & Move (Split Mode priority actions)
        if (isSplitMode_) {
            if (hasSelection) {
                addOption("\uE14D", ThemeManager::instance().getAccentColor(), brls::getStr("app/file_manager/copy_to_other_panel", oppDirName), [this]() {
                    copyToOppositePanel();
                });
                addOption("\uE14E", nvgRGB(255, 179, 0), brls::getStr("app/file_manager/move_to_other_panel", oppDirName), [this]() {
                    moveToOppositePanel();
                });
            } else if (targetSingleItem) {
                addOption("\uE14D", ThemeManager::instance().getAccentColor(), brls::getStr("app/file_manager/copy_to_other_panel", oppDirName), [this]() {
                    copyToOppositePanel();
                });
                addOption("\uE14E", nvgRGB(255, 179, 0), brls::getStr("app/file_manager/move_to_other_panel", oppDirName), [this]() {
                    moveToOppositePanel();
                });
            }
        }

        // Install Game (if target is game package)
        if (targetSingleItem && util::isGamePackage(targetSingleItem->path)) {
            addOption("\uE0E0", ThemeManager::instance().getAccentColor(), "app/file_manager/install_game_btn"_i18n, [this, item = *targetSingleItem]() {
                showInstallDialog(item);
            }, true);
        }

        // Browse / Extract archive (if target is archive)
        if (targetSingleItem && util::isArchiveFile(targetSingleItem->path)) {
            addOption("\uE2C7", ThemeManager::instance().getAccentColor(), "app/archive/browse_archive"_i18n, [this, item = *targetSingleItem]() {
                navigateTo(activePanel_, item.path);
            });

            addOption("\uE2C6", nvgRGB(255, 110, 64), "app/file_manager/extract_archive_btn"_i18n, [this, item = *targetSingleItem]() {
                showArchiveDialog(item);
            }, true);
        }

        // View as text (if single file)
        if (targetSingleItem && !targetSingleItem->isDir) {
            addOption("\uE873", ThemeManager::instance().getAccentColor(), "app/file_manager/view_as_text_btn"_i18n, [this, item = *targetSingleItem]() {
                openTextViewer(item.path, item.name);
            }, true);
        }

        // Create Archive (if selection or target single item)
        if (hasSelection) {
            addOption("\uE2C6", nvgRGB(255, 110, 64), brls::getStr("app/file_manager/create_archive_count", std::to_string(cur.selectedPaths.size())), [this]() {
                std::vector<std::string> targets(panels_[activePanel_].selectedPaths.begin(), panels_[activePanel_].selectedPaths.end());
                showCreateArchiveDialog(targets);
            }, true);
        } else if (targetSingleItem) {
            addOption("\uE2C6", nvgRGB(255, 110, 64), "app/file_manager/create_archive_single"_i18n, [this, item = *targetSingleItem]() {
                showCreateArchiveDialog({ item.path });
            }, true);
        }

        // Paste (if clipboard active)
        if (hasClipboard) {
            std::string pasteText = (clip.op == util::ClipboardOp::Cut ?
                                    brls::getStr("app/file_manager/paste_cut", std::to_string(clip.paths.size())) :
                                    brls::getStr("app/file_manager/paste_copy", std::to_string(clip.paths.size())));
            addOption("\uE14F", ThemeManager::instance().getAccentColor(), pasteText, [this]() {
                pasteClipboard();
            });
        }

        // Copy (to clipboard)
        if (hasSelection) {
            addOption("\uE14D", nvgRGB(64, 196, 255), brls::getStr("app/file_manager/copy_count", std::to_string(cur.selectedPaths.size())), [this]() {
                auto& c = util::getClipboard();
                c.op = util::ClipboardOp::Copy;
                c.paths.assign(panels_[activePanel_].selectedPaths.begin(), panels_[activePanel_].selectedPaths.end());
                brls::Application::notify(brls::getStr("app/file_manager/copied_count", std::to_string(c.paths.size())));
            });
        } else if (targetSingleItem) {
            addOption("\uE14D", nvgRGB(64, 196, 255), "app/file_manager/copy_single"_i18n, [this, item = *targetSingleItem]() {
                auto& c = util::getClipboard();
                c.op = util::ClipboardOp::Copy;
                c.paths = { item.path };
                brls::Application::notify("app/file_manager/copied_single"_i18n + item.name);
            });
        }

        // Cut (to clipboard)
        if (hasSelection) {
            addOption("\uE14E", nvgRGB(255, 179, 0), brls::getStr("app/file_manager/cut_count", std::to_string(cur.selectedPaths.size())), [this]() {
                auto& c = util::getClipboard();
                c.op = util::ClipboardOp::Cut;
                c.paths.assign(panels_[activePanel_].selectedPaths.begin(), panels_[activePanel_].selectedPaths.end());
                brls::Application::notify(brls::getStr("app/file_manager/cut_done_count", std::to_string(c.paths.size())));
            });
        } else if (targetSingleItem) {
            addOption("\uE14E", nvgRGB(255, 179, 0), "app/file_manager/cut_single"_i18n, [this, item = *targetSingleItem]() {
                auto& c = util::getClipboard();
                c.op = util::ClipboardOp::Cut;
                c.paths = { item.path };
                brls::Application::notify("app/file_manager/cut_done_single"_i18n + item.name);
            });
        }

        // Rename
        if (targetSingleItem) {
            addOption("\uE254", nvgRGB(179, 136, 255), "app/file_manager/rename"_i18n, [this, item = *targetSingleItem]() {
                showRenameDialog(item);
            }, true);
        }

        // Delete
        if (hasSelection) {
            addOption("\uE872", nvgRGB(255, 82, 82), brls::getStr("app/file_manager/delete_count", std::to_string(cur.selectedPaths.size())), [this]() {
                showDeleteConfirmDialog();
            }, true);
        } else if (targetSingleItem) {
            addOption("\uE872", nvgRGB(255, 82, 82), "app/file_manager/delete_single"_i18n, [this, item = *targetSingleItem]() {
                panels_[activePanel_].selectedPaths.clear();
                panels_[activePanel_].selectedPaths.insert(item.path);
                showDeleteConfirmDialog();
            }, true);
        }

        // New Folder (always available)
        addOption("\uE2CC", nvgRGB(38, 198, 218), "app/file_manager/new_folder"_i18n, [this]() {
            showNewFolderDialog();
        }, true);
    }

    // Split Screen Toggle option
    if (isSplitMode_) {
        addOption("\uE8F2", nvgRGB(100, 181, 246), "app/file_manager/split_mode_disable"_i18n, [this]() {
            toggleSplitMode();
        });
    } else {
        addOption("\uE8F1", nvgRGB(100, 181, 246), "app/file_manager/split_mode_enable"_i18n, [this]() {
            toggleSplitMode();
        });
    }

    // Selection helpers
    if (hasSelection) {
        addOption("\uE835", nvgRGB(180, 190, 200), "app/file_manager/deselect_all"_i18n, [this]() {
            clearSelection(activePanel_);
        });
    } else if (targetSingleItem && cur.currentFocusedRow >= 0) {
        addOption("\uE834", ThemeManager::instance().getAccentColor(), "app/file_manager/select_this_item"_i18n, [this, row = cur.currentFocusedRow]() {
            toggleSelection(activePanel_, row);
        });
    }

    if (!cur.items.empty()) {
        addOption("\uE834", nvgRGB(100, 181, 246), brls::getStr("app/file_manager/select_all_count", std::to_string(cur.items.size())), [this]() {
            selectAll(activePanel_);
        });
    }

    // Separator before cancel
    auto* cancelSep = new brls::Box();
    cancelSep->setHeight(1.0f);
    cancelSep->setMarginTop(6.0f);
    cancelSep->setMarginBottom(6.0f);
    cancelSep->setBackgroundColor(isLight ? nvgRGBA(0, 0, 0, 25) : nvgRGBA(255, 255, 255, 20));
    content->addView(cancelSep);

    // Cancel option
    addOption("\uE5CD", nvgRGB(239, 83, 80), "hints/cancel"_i18n, nullptr);

    if (firstOption) {
        dialog->setLastFocusedView(firstOption);
        content->setLastFocusedView(firstOption);
    }

    dialog->open();

    if (firstOption) {
        brls::sync([dialog, firstOption]() {
            dialog->setLastFocusedView(firstOption);
            brls::Application::giveFocus(firstOption);
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FileManagerDataSource
// ─────────────────────────────────────────────────────────────────────────────

int FileManagerView::FileManagerDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    if (panelIndex_ < 0 || panelIndex_ > 1) return 0;
    const auto& cur = parent_->panels_[panelIndex_];
    return static_cast<int>(cur.items.size()) + (cur.hasParentDir ? 1 : 0);
}

brls::RecyclerCell* FileManagerView::FileManagerDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    FileManagerCell* cell = dynamic_cast<FileManagerCell*>(recycler->dequeueReusableCell("Cell"));
    if (!cell) {
        cell = FileManagerCell::create();
        cell->reuseIdentifier = "Cell";
    }

    cell->clearRegisteredActions();
    cell->rowIndex = index.row;
    cell->panelIndex = panelIndex_;
    cell->parentView = parent_;
    cell->setCompactMode(parent_->isSplitMode_);

    const auto& curPanel = parent_->panels_[panelIndex_];
    bool isParentRow = (curPanel.hasParentDir && index.row == 0);

    if (isParentRow) {
        if (cell->accentBar) cell->accentBar->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        cell->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        cell->icon->setText("\uE5D8"); // Material arrow up
        cell->icon->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        cell->name->setText("..");
        cell->name->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        cell->size->setText(brls::getStr("app/file_manager/parent_folder"));
        cell->date->setText("");

        cell->registerClickAction([parent = parent_, pIdx = panelIndex_](brls::View* view) {
            brls::sync([parent, pIdx]() {
                parent->setActivePanel(pIdx);
                parent->navigateUp(pIdx);
            });
            return true;
        });
        return cell;
    }

    size_t itemIdx = curPanel.hasParentDir ? (index.row - 1) : index.row;
    if (itemIdx >= curPanel.items.size()) return cell;

    const auto& item = curPanel.items[itemIdx];
    bool isSelected = curPanel.selectedPaths.count(item.path) > 0;

    // Apply visual selection style
    cell->setSelectedVisual(isSelected);

    // Icon & Name coloring
    if (item.isDir) {
        cell->icon->setText("\uE2C7"); // Material folder
        cell->icon->setTextColor(nvgRGB(255, 193, 7)); // Amber/Yellow
        cell->name->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        cell->size->setText(brls::getStr("app/file_manager/folder_type"));
    } else if (util::isArchiveFile(item.path)) {
        cell->icon->setText("\uE2C6"); // Material archive
        cell->icon->setTextColor(nvgRGB(255, 87, 34)); // Orange
        cell->name->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        cell->size->setText(util::formatFileSize(item.size));
    } else {
        bool isGame = util::isGamePackage(item.path);
        if (isGame) {
            cell->icon->setText("\uE0E0"); // Gamepad
            cell->icon->setTextColor(ThemeManager::instance().getAccentColor());
        } else if (isTextFile(item.path)) {
            cell->icon->setText("\uE873"); // Material document
            cell->icon->setTextColor(ThemeManager::instance().getAccentColor());
        } else {
            cell->icon->setText("\uE24D"); // Generic file
            cell->icon->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        }
        cell->name->setTextColor(ThemeManager::instance().getTextPrimaryColor());
        cell->size->setText(util::formatFileSize(item.size));
    }

    cell->name->setText(item.name);

    // Format modified date safely
    if (item.modifiedTime > 0 && item.modifiedTime < 253402300799LL) {
        char dateBuf[32];
        std::tm tm{};
#if defined(_WIN32)
        if (localtime_s(&tm, &item.modifiedTime) == 0) {
            std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M", &tm);
            cell->date->setText(dateBuf);
        } else {
            cell->date->setText("");
        }
#else
        if (localtime_r(&item.modifiedTime, &tm) != nullptr) {
            std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M", &tm);
            cell->date->setText(dateBuf);
        } else {
            cell->date->setText("");
        }
#endif
    } else {
        cell->date->setText("");
    }

    // Click action (A button)
    cell->registerClickAction([parent = parent_, pIdx = panelIndex_, item](brls::View* view) {
        util::logLine("FileManagerView: cell clicked on " + item.name + " on panel " + std::to_string(pIdx));
        parent->setActivePanel(pIdx);
        std::string archPath, innerPath;
        bool inArchive = util::parseArchiveVirtualPath(parent->panels_[pIdx].currentDir, archPath, innerPath);

        if (item.isDir) {
            brls::sync([parent, pIdx, target = item.path]() {
                parent->navigateTo(pIdx, target);
            });
        } else if (util::isArchiveFile(item.path) && !inArchive) {
            brls::sync([parent, pIdx, target = item.path]() {
                parent->navigateTo(pIdx, target);
            });
        } else if (inArchive) {
            brls::sync([parent]() {
                parent->showActionsMenu();
            });
        } else if (util::isGamePackage(item.path)) {
            brls::sync([parent, item]() {
                parent->showInstallDialog(item);
            });
        } else if (isTextFile(item.path)) {
            brls::sync([parent, item]() {
                parent->openTextViewer(item.path, item.name);
            });
        } else {
            brls::Application::notify(item.name + " (" + util::formatFileSize(item.size) + ")");
        }
        return true;
    });

    // Selection toggle action (Y button on gamepad, Space / Y on keyboard)
    cell->registerAction("app/file_manager/action_toggle_select"_i18n, brls::ControllerButton::BUTTON_Y,
        [parent = parent_, pIdx = panelIndex_, rowIndex = index.row, cell](brls::View* view) {
            parent->setActivePanel(pIdx);
            parent->toggleSelectionOnCell(pIdx, rowIndex, cell);
            return true;
        });

    cell->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_SPACE),
        [parent = parent_, pIdx = panelIndex_, rowIndex = index.row, cell](brls::View* view) {
            parent->setActivePanel(pIdx);
            parent->toggleSelectionOnCell(pIdx, rowIndex, cell);
            return true;
        });

    cell->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_Y),
        [parent = parent_, pIdx = panelIndex_, rowIndex = index.row, cell](brls::View* view) {
            parent->setActivePanel(pIdx);
            parent->toggleSelectionOnCell(pIdx, rowIndex, cell);
            return true;
        });

    return cell;
}

} // namespace ui
