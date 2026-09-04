#include "FileManagerView.hpp"
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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// FileManagerCell
// ─────────────────────────────────────────────────────────────────────────────

FileManagerCell::FileManagerCell() {
    this->inflateFromXMLRes("xml/file_manager_cell.xml");
}

FileManagerCell* FileManagerCell::create() {
    return new FileManagerCell();
}

void FileManagerCell::setSelectedVisual(bool selected) {
    if (selected) {
        if (accentBar) accentBar->setBackgroundColor(nvgRGB(0, 224, 165));
        this->setBackgroundColor(nvgRGBA(0, 224, 165, 38));
        if (name) name->setTextColor(nvgRGB(0, 240, 180));
    } else {
        if (accentBar) accentBar->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        this->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        if (name) name->setTextColor(nvgRGB(230, 230, 230));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FileManagerView
// ─────────────────────────────────────────────────────────────────────────────

static std::string normalizeDir(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
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
    // If it's a root path, it has no parent
    if (p == "sdmc:/" || p == "sdmc:" || p == "/" || p == "." || p.empty()) {
        return "";
    }
    // Check for Windows drive root like "C:/" or "C:"
    if (p.size() <= 3 && p.find(':') != std::string::npos) {
        return "";
    }

    // Find the last slash
    size_t lastSlash = p.rfind('/');
    if (lastSlash == std::string::npos) {
        return "";
    }

    // Check if the slash is right after the scheme, e.g. "sdmc:/"
    size_t colon = p.find(':');
    if (colon != std::string::npos && lastSlash == colon + 1) {
        // Parent is root, e.g. "sdmc:/folder" -> "sdmc:/"
        return p.substr(0, lastSlash + 1);
    }

    if (lastSlash == 0) {
        // Root slash, e.g. "/folder" -> "/"
        return "/";
    }

    // e.g. "sdmc:/folder/sub" -> "sdmc:/folder"
    return p.substr(0, lastSlash);
}

FileManagerView::FileManagerView(const std::string& initialPath) {
    if (!initialPath.empty()) {
        currentDir_ = initialPath;
    } else {
        currentDir_ = util::getDefaultRootPath();
    }
    currentDir_ = normalizeDir(currentDir_);
    rootDir_ = currentDir_;
}

void FileManagerView::onContentAvailable() {
    util::logLine("FileManagerView: onContentAvailable start");
    if (recycler) {
        util::logLine("FileManagerView: registering Cell and setting DataSource");
        recycler->estimatedRowHeight = 56.0f;
        recycler->setPaddingRight(30.0f);
        recycler->registerCell("Cell", []() { return FileManagerCell::create(); });
        recycler->setDataSource(new FileManagerDataSource(this));
    }

    // Register Activity Level Actions for Borealis Hints (compact text to avoid wrapping the clock)
    this->registerAction("Действия", brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        showActionsMenu();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_X), [this](brls::View* view) {
        showActionsMenu();
        return true;
    });

    this->registerAction("app/file_manager/select_all"_i18n, brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        selectAll();
        return true;
    }, true);

    this->registerAction("app/file_manager/deselect_all"_i18n, brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        clearSelection();
        return true;
    }, true);

    // Custom B button handling: navigate up if inside subfolder, else exit activity
    this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
        if (hasParentDir_) {
            brls::sync([this]() {
                navigateUp();
            });
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_BACKSPACE), [this](brls::View* view) {
        if (hasParentDir_) {
            brls::sync([this]() {
                navigateUp();
            });
            return true;
        }
        return false;
    });

    util::logLine("FileManagerView: calling initial refresh");
    refresh();
    util::logLine("FileManagerView: onContentAvailable end");
}

void FileManagerView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (resetState && recycler) {
        int targetRow = (currentFocusedRow_ >= 0) ? currentFocusedRow_ : (hasParentDir_ && !items_.empty() ? 1 : 0);
        recycler->setDefaultCellFocus(brls::IndexPath(0, targetRow));
        recycler->selectRowAt(brls::IndexPath(0, targetRow), false);
        brls::Application::giveFocus(recycler);
    }
}

void FileManagerView::navigateTo(const std::string& path, const std::string& focusChild) {
    currentDir_ = normalizeDir(path);
    selectedPaths_.clear();
    refresh(focusChild);
}

void FileManagerView::navigateUp() {
    std::string parentDir = getParentDir(currentDir_);
    if (parentDir.empty() || parentDir == currentDir_) {
        return;
    }
    std::filesystem::path p(currentDir_);
    std::string childName = p.filename().generic_string();
    navigateTo(parentDir, childName);
}

void FileManagerView::refresh(const std::string& focusChild) {
    currentDir_ = normalizeDir(currentDir_);

    // Check if we have a parent directory
    std::string parentDir = getParentDir(currentDir_);
    hasParentDir_ = (!parentDir.empty() && parentDir != currentDir_);

    std::string err;
    items_ = util::listFolder(currentDir_, err);

    if (currentPath) {
        currentPath->setText(currentDir_);
    }

    if (emptyLabel) {
        emptyLabel->setVisibility((items_.empty() && !hasParentDir_) ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    updateSpaceInfo();
    updateSelectionBar();

    // Determine target focus row
    int targetRow = 0;
    if (!focusChild.empty()) {
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].name == focusChild) {
                targetRow = static_cast<int>(i) + (hasParentDir_ ? 1 : 0);
                break;
            }
        }
    } else if (hasParentDir_ && !items_.empty()) {
        targetRow = 1;
    }

    currentFocusedRow_ = targetRow;

    if (recycler) {
        recycler->reloadData();
        brls::sync([this, targetRow]() {
            if (recycler) {
                recycler->setDefaultCellFocus(brls::IndexPath(0, targetRow));
                recycler->selectRowAt(brls::IndexPath(0, targetRow), false);
                brls::Application::giveFocus(recycler);
            }
        });
    }
}

void FileManagerView::updateSpaceInfo() {
    if (!spaceInfo) return;
    uint64_t freeB = 0, totalB = 0;
    if (util::getStorageSpace(currentDir_, freeB, totalB) && totalB > 0) {
        char buf[128];
        std::string freeStr = util::formatFileSize(freeB);
        std::string totalStr = util::formatFileSize(totalB);
        std::snprintf(buf, sizeof(buf), "%s / %s", freeStr.c_str(), totalStr.c_str());
        spaceInfo->setText(buf);
    } else {
        spaceInfo->setText("");
    }
}

void FileManagerView::updateSelectionBar() {
    if (!selectionBar || !selectionText) return;

    if (selectedPaths_.empty()) {
        selectionBar->setVisibility(brls::Visibility::GONE);
    } else {
        selectionBar->setVisibility(brls::Visibility::VISIBLE);
        size_t filesCount = 0;
        size_t dirsCount = 0;
        uint64_t totalSize = 0;

        for (const auto& it : items_) {
            if (selectedPaths_.count(it.path)) {
                if (it.isDir) dirsCount++;
                else {
                    filesCount++;
                    totalSize += it.size;
                }
            }
        }

        char buf[128];
        if (filesCount == 0) {
            std::snprintf(buf, sizeof(buf), "%zu %s", dirsCount, dirsCount == 1 ? "папка" : (dirsCount < 5 ? "папки" : "папок"));
        } else if (dirsCount == 0) {
            std::string sizeStr = util::formatFileSize(totalSize);
            std::snprintf(buf, sizeof(buf), "%zu %s · %s", filesCount, filesCount == 1 ? "файл" : (filesCount < 5 ? "файла" : "файлов"), sizeStr.c_str());
        } else {
            std::string sizeStr = util::formatFileSize(totalSize);
            std::snprintf(buf, sizeof(buf), "%zu (файлов: %zu, папок: %zu) · %s",
                          selectedPaths_.size(), filesCount, dirsCount, sizeStr.c_str());
        }

        std::string selPrefix = brls::getStr("app/file_manager/selected_count");
        if (selPrefix.empty() || selPrefix == "app/file_manager/selected_count") selPrefix = "Выбрано: ";
        selectionText->setText(selPrefix + buf);
    }
}

void FileManagerView::toggleSelectionOnCell(size_t index, FileManagerCell* cell) {
    if (hasParentDir_ && index == 0) {
        return; // Cannot select ".."
    }
    size_t itemIdx = hasParentDir_ ? (index - 1) : index;
    if (itemIdx >= items_.size()) return;

    const std::string& path = items_[itemIdx].path;
    bool isNowSelected = false;
    if (selectedPaths_.count(path)) {
        selectedPaths_.erase(path);
        isNowSelected = false;
    } else {
        selectedPaths_.insert(path);
        isNowSelected = true;
    }

    if (cell) {
        cell->setSelectedVisual(isNowSelected);
    }

    updateSelectionBar();
}

void FileManagerView::toggleSelection(size_t index) {
    toggleSelectionOnCell(index, nullptr);
    brls::sync([this]() {
        if (recycler) {
            recycler->reloadData();
        }
    });
}

void FileManagerView::selectAll() {
    selectedPaths_.clear();
    for (const auto& item : items_) {
        selectedPaths_.insert(item.path);
    }
    updateSelectionBar();
    brls::sync([this]() {
        if (recycler) {
            recycler->reloadData();
        }
    });
}

void FileManagerView::clearSelection() {
    selectedPaths_.clear();
    updateSelectionBar();
    brls::sync([this]() {
        if (recycler) {
            recycler->reloadData();
        }
    });
}

void FileManagerView::openTextViewer(const std::string& path, const std::string& name) {
    brls::Application::pushActivity(new TextViewerActivity(path, name));
}

void FileManagerView::showArchiveDialog(const util::FileItem& item) {
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
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    auto* subLbl = new brls::Label();
    subLbl->setText("Архив · " + util::formatFileSize(item.size));
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(nvgRGBA(255, 110, 64, 210));
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    auto* applet = dynamic_cast<brls::AppletFrame*>(dialog->getView("brls/dialog/applet"));
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(nvgRGBA(24, 26, 32, 252));
    }

    int restoreRow = currentFocusedRow_;
    brls::View* firstOption = nullptr;

    auto addOption = [&firstOption, content, dialog, this, restoreRow](const std::string& iconGlyph, NVGcolor iconCol, const std::string& labelText, std::function<void()> action) {
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
        row->setBackgroundColor(nvgRGBA(36, 39, 46, 190));

        auto* ic = new brls::Label();
        ic->setText(iconGlyph);
        ic->setFontSize(20.0f);
        ic->setTextColor(iconCol);
        ic->setMarginRight(14.0f);
        row->addView(ic);

        auto* lb = new brls::Label();
        lb->setText(labelText);
        lb->setFontSize(15.0f);
        lb->setTextColor(nvgRGB(240, 245, 255));
        lb->setGrow(1.0f);
        row->addView(lb);

        if (!firstOption) firstOption = row;

        row->registerClickAction([dialog, action, this, restoreRow](brls::View* v) {
            brls::sync([dialog, action, this, restoreRow]() {
                dialog->dismiss([action, this, restoreRow]() {
                    if (action) {
                        action();
                    } else {
                        // Cancelled - restore focus to recycler
                        brls::sync([this, restoreRow]() {
                            if (recycler) {
                                int maxRow = static_cast<int>(items_.size()) + (hasParentDir_ ? 1 : 0) - 1;
                                int validRow = std::clamp(restoreRow, 0, std::max(0, maxRow));
                                currentFocusedRow_ = validRow;
                                recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
                                recycler->selectRowAt(brls::IndexPath(0, validRow), false);
                                brls::Application::giveFocus(recycler);
                            }
                        });
                    }
                });
            });
            return true;
        });

        content->addView(row);
    };

    // Option 1: Extract here
    addOption("\uE2C6", nvgRGB(255, 110, 64), "Распаковать в текущую папку", [this, item]() {
        brls::sync([this, item]() {
            auto* progressDlg = new ArchiveProgressDialog(item.path, currentDir_, [this, item](bool ok, const std::string& msg) {
                if (ok) {
                    brls::Application::notify("Распаковка завершена!");
                    refresh(item.name);
                } else {
                    brls::Application::notify(msg.empty() ? "Ошибка распаковки" : msg);
                }
            });
            progressDlg->startExtraction();
        });
    });

    // Option 2: Extract to subfolder named after archive
    std::filesystem::path p(item.path);
    std::string folderName = p.stem().generic_string();
    std::string targetDir = currentDir_ + "/" + folderName;
    addOption("\uE2CC", nvgRGB(0, 224, 165), "Распаковать в папку /" + folderName, [this, item, targetDir, folderName]() {
        brls::sync([this, item, targetDir, folderName]() {
            auto* progressDlg = new ArchiveProgressDialog(item.path, targetDir, [this, folderName](bool ok, const std::string& msg) {
                if (ok) {
                    brls::Application::notify("Распаковка завершена!");
                    refresh(folderName);
                } else {
                    brls::Application::notify(msg.empty() ? "Ошибка распаковки" : msg);
                }
            });
            progressDlg->startExtraction();
        });
    });

    // Separator
    auto* cancelSep = new brls::Box();
    cancelSep->setHeight(1.0f);
    cancelSep->setMarginTop(6.0f);
    cancelSep->setMarginBottom(6.0f);
    cancelSep->setBackgroundColor(nvgRGBA(255, 255, 255, 20));
    content->addView(cancelSep);

    // Cancel option
    addOption("\uE5CD", nvgRGB(239, 83, 80), "Отмена", nullptr);

    if (firstOption) {
        dialog->setLastFocusedView(firstOption);
        content->setLastFocusedView(firstOption);
    }

    dialog->open();

    if (firstOption) {
        brls::Application::giveFocus(firstOption);
        brls::sync([firstOption]() {
            brls::Application::giveFocus(firstOption);
        });
    }
}

void FileManagerView::showInstallDialog(const util::FileItem& item) {
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100.0f);
    content->setPadding(20.0f, 22.0f, 16.0f, 22.0f);

    // Header Box (Emerald / Turquoise theme)
    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(nvgRGBA(0, 224, 165, 80)); // Emerald line

    // Icon Badge
    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);
    iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 35)); // Emerald tint

    auto* badgeIcon = new brls::Label();
    badgeIcon->setText("\uE0E0"); // Gamepad
    badgeIcon->setFontSize(22.0f);
    badgeIcon->setTextColor(nvgRGB(0, 224, 165)); // Emerald
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText(item.name);
    titleLbl->setFontSize(18.0f);
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    auto* subLbl = new brls::Label();
    subLbl->setText("Пакет установки · " + util::formatFileSize(item.size));
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(nvgRGBA(0, 224, 165, 220)); // Emerald subtitle
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    auto* applet = dynamic_cast<brls::AppletFrame*>(dialog->getView("brls/dialog/applet"));
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(nvgRGBA(24, 26, 32, 252));
    }

    int restoreRow = currentFocusedRow_;
    brls::View* firstOption = nullptr;

    auto addOption = [&firstOption, content, dialog, this, restoreRow](const std::string& iconGlyph, NVGcolor iconCol, const std::string& labelText, const std::string& subText, std::function<void()> action) {
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
        row->setBackgroundColor(nvgRGBA(36, 39, 46, 190));

        auto* ic = new brls::Label();
        ic->setText(iconGlyph);
        ic->setFontSize(20.0f);
        ic->setTextColor(iconCol);
        ic->setMarginRight(14.0f);
        row->addView(ic);

        auto* textCol = new brls::Box();
        textCol->setAxis(brls::Axis::COLUMN);
        textCol->setGrow(1.0f);

        auto* lb = new brls::Label();
        lb->setText(labelText);
        lb->setFontSize(15.0f);
        lb->setTextColor(nvgRGB(240, 245, 255));
        lb->setSingleLine(true);
        textCol->addView(lb);

        if (!subText.empty()) {
            auto* sb = new brls::Label();
            sb->setText(subText);
            sb->setFontSize(12.0f);
            sb->setTextColor(nvgRGBA(0, 224, 165, 200)); // Emerald detail
            sb->setSingleLine(true);
            textCol->addView(sb);
        }

        row->addView(textCol);

        if (!firstOption) firstOption = row;

        row->registerClickAction([dialog, action, this, restoreRow](brls::View* v) {
            brls::sync([dialog, action, this, restoreRow]() {
                dialog->dismiss([action, this, restoreRow]() {
                    if (action) {
                        action();
                    } else {
                        // Cancelled - restore focus to recycler
                        brls::sync([this, restoreRow]() {
                            if (recycler) {
                                int maxRow = static_cast<int>(items_.size()) + (hasParentDir_ ? 1 : 0) - 1;
                                int validRow = std::clamp(restoreRow, 0, std::max(0, maxRow));
                                currentFocusedRow_ = validRow;
                                recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
                                recycler->selectRowAt(brls::IndexPath(0, validRow), false);
                                brls::Application::giveFocus(recycler);
                            }
                        });
                    }
                });
            });
            return true;
        });

        content->addView(row);
    };

    // Query storage free space
    int64_t sdFree = 0, nandFree = 0;
    util::getStorageFreeSpace(1, sdFree);
    util::getStorageFreeSpace(0, nandFree);

    std::string sdFreeStr = "Свободно: " + util::formatFileSize(sdFree > 0 ? static_cast<uint64_t>(sdFree) : 0);
    std::string nandFreeStr = "Свободно: " + util::formatFileSize(nandFree > 0 ? static_cast<uint64_t>(nandFree) : 0);

    // Option 1: SD Card (Emerald)
    addOption("\uE1DB", nvgRGB(0, 224, 165), "Установить на SD-карту", sdFreeStr, [this, item]() {
        brls::sync([this, item]() {
            auto* progressDlg = new InstallProgressDialog(item.path, 1, [this, item](bool ok, const std::string& msg) {
                if (ok) {
                    promptDeleteSourceFile(item.path, item.name);
                } else {
                    brls::Application::notify(msg.empty() ? "Ошибка установки" : msg);
                    refresh(item.name);
                }
            });
            progressDlg->startInstallation();
        });
    });

    // Option 2: NAND System Storage (Emerald)
    addOption("\uE318", nvgRGB(0, 224, 165), "Установить в память консоли (NAND)", nandFreeStr, [this, item]() {
        brls::sync([this, item]() {
            auto* progressDlg = new InstallProgressDialog(item.path, 0, [this, item](bool ok, const std::string& msg) {
                if (ok) {
                    promptDeleteSourceFile(item.path, item.name);
                } else {
                    brls::Application::notify(msg.empty() ? "Ошибка установки" : msg);
                    refresh(item.name);
                }
            });
            progressDlg->startInstallation();
        });
    });

    // Separator
    auto* cancelSep = new brls::Box();
    cancelSep->setHeight(1.0f);
    cancelSep->setMarginTop(6.0f);
    cancelSep->setMarginBottom(6.0f);
    cancelSep->setBackgroundColor(nvgRGBA(255, 255, 255, 20));
    content->addView(cancelSep);

    // Option 3: Cancel
    addOption("\uE5CD", nvgRGB(239, 83, 80), "Отмена", "", nullptr);

    if (firstOption) {
        dialog->setLastFocusedView(firstOption);
        content->setLastFocusedView(firstOption);
    }

    dialog->open();

    if (firstOption) {
        brls::Application::giveFocus(firstOption);
        brls::sync([firstOption]() {
            brls::Application::giveFocus(firstOption);
        });
    }
}

void FileManagerView::promptDeleteSourceFile(const std::string& filePath, const std::string& fileName) {
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100.0f);
    content->setPadding(20.0f, 22.0f, 16.0f, 22.0f);

    // Header Box (Emerald theme)
    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(nvgRGBA(0, 224, 165, 80));

    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);
    iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 35));

    auto* badgeIcon = new brls::Label();
    badgeIcon->setText("\uE876"); // Check mark
    badgeIcon->setFontSize(22.0f);
    badgeIcon->setTextColor(nvgRGB(0, 224, 165));
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText("Установка завершена!");
    titleLbl->setFontSize(18.0f);
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    auto* subLbl = new brls::Label();
    subLbl->setText("Освободить место на накопителе?");
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(nvgRGBA(0, 224, 165, 220));
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* descLbl = new brls::Label();
    descLbl->setText("Удалить исходный файл:\n" + fileName + "?");
    descLbl->setFontSize(14.0f);
    descLbl->setTextColor(nvgRGB(200, 205, 215));
    descLbl->setMarginBottom(16.0f);
    content->addView(descLbl);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    auto* applet = dynamic_cast<brls::AppletFrame*>(dialog->getView("brls/dialog/applet"));
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(nvgRGBA(24, 26, 32, 252));
    }

    dialog->addButton("Удалить файл", [this, filePath]() {
        std::string err;
        if (util::deletePathRecursive(filePath, err)) {
            brls::Application::notify("Исходный файл удален!");
        } else {
            brls::Application::notify(err.empty() ? "Не удалось удалить файл" : err);
        }
        refresh();
    });

    dialog->addButton("Оставить", [this, fileName]() {
        refresh(fileName);
    });

    dialog->open();
}

void FileManagerView::showDeleteConfirmDialog() {
    if (selectedPaths_.empty()) return;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "Удалить выбранные элементы (%zu) навсегда?", selectedPaths_.size());

    auto* dialog = new brls::Dialog(std::string(msg));
    dialog->setCancelable(true);

    dialog->addButton("app/common/yes"_i18n, [this]() {
        std::vector<std::string> toDelete(selectedPaths_.begin(), selectedPaths_.end());
        std::string err;
        if (util::deleteMultiplePaths(toDelete, err)) {
            brls::Application::notify("app/file_manager/deleted_success"_i18n);
        } else {
            brls::Application::notify(err.empty() ? "app/common/error"_i18n : err);
        }
        selectedPaths_.clear();
        refresh();
    });

    dialog->addButton("app/common/cancel"_i18n, []() {});
    dialog->open();
}

void FileManagerView::showNewFolderDialog() {
    brls::Application::getImeManager()->openForText([this](std::string text) {
        if (text.empty()) return;
        std::string newPath = currentDir_ + "/" + text;
        std::string err;
        if (util::createFolder(newPath, err)) {
            brls::Application::notify("app/file_manager/folder_created"_i18n);
            refresh();
        } else {
            brls::Application::notify(err.empty() ? "app/common/error"_i18n : err);
        }
    }, brls::getStr("app/file_manager/new_folder"), "", 64, "New_Folder");
}

void FileManagerView::showRenameDialog(const util::FileItem& item) {
    brls::Application::getImeManager()->openForText([this, item](std::string text) {
        if (text.empty() || text == item.name) return;
        std::string err;
        if (util::renameItem(item.path, text, err)) {
            brls::Application::notify("app/file_manager/renamed_success"_i18n);
            selectedPaths_.clear();
            refresh();
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

    brls::async([this, paths, isCut]() {
        bool allOk = true;
        std::string lastErr;

        for (const auto& src : paths) {
            std::filesystem::path sp(src);
            std::string dest = currentDir_ + "/" + sp.filename().generic_string();
            std::string err;

            if (isCut) {
                if (!util::movePath(src, dest, err)) {
                    allOk = false;
                    lastErr = err;
                }
            } else {
                if (!util::copyPathRecursive(src, dest, nullptr, nullptr, err)) {
                    allOk = false;
                    lastErr = err;
                }
            }
        }

        if (isCut) {
            util::clearClipboard();
        }

        brls::sync([this, allOk, lastErr]() {
            if (allOk) {
                brls::Application::notify("app/file_manager/paste_success"_i18n);
            } else {
                brls::Application::notify(lastErr.empty() ? "app/common/error"_i18n : lastErr);
            }
            refresh();
        });
    });
}

void FileManagerView::showActionsMenu() {
    int restoreRow = currentFocusedRow_;

    auto& clip = util::getClipboard();
    bool hasSelection = !selectedPaths_.empty();
    bool hasClipboard = (!clip.paths.empty() && clip.op != util::ClipboardOp::None);

    // Identify target items
    const util::FileItem* targetSingleItem = nullptr;
    if (selectedPaths_.size() == 1) {
        std::string sel = *selectedPaths_.begin();
        for (const auto& it : items_) {
            if (it.path == sel) { targetSingleItem = &it; break; }
        }
    } else if (!hasSelection) {
        if (currentFocusedRow_ >= 0) {
            size_t idx = hasParentDir_ ? (currentFocusedRow_ - 1) : currentFocusedRow_;
            if ((currentFocusedRow_ > 0 || !hasParentDir_) && idx < items_.size()) {
                targetSingleItem = &items_[idx];
            }
        }
    }

    // Outer content inside dialog
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100.0f);
    content->setPadding(20.0f, 22.0f, 16.0f, 22.0f);

    // --- 1. Header Card ---
    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(14.0f);
    headerBox->setPaddingBottom(12.0f);
    headerBox->setLineBottom(1.0f);
    headerBox->setLineColor(nvgRGBA(0, 224, 165, 80));

    // Header Icon Badge
    auto* iconBadge = new brls::Box();
    iconBadge->setWidth(42.0f);
    iconBadge->setHeight(42.0f);
    iconBadge->setCornerRadius(8.0f);
    iconBadge->setJustifyContent(brls::JustifyContent::CENTER);
    iconBadge->setAlignItems(brls::AlignItems::CENTER);
    iconBadge->setMarginRight(14.0f);

    auto* badgeIcon = new brls::Label();
    badgeIcon->setFontSize(22.0f);

    std::string titleText, subtitleText;
    if (hasSelection && selectedPaths_.size() > 1) {
        iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 40));
        badgeIcon->setText("\uE834"); // Multiple select icon
        badgeIcon->setTextColor(nvgRGB(0, 224, 165));
        titleText = "Выбрано элементов: " + std::to_string(selectedPaths_.size());
        subtitleText = "Групповые операции";
    } else if (targetSingleItem) {
        if (targetSingleItem->isDir) {
            iconBadge->setBackgroundColor(nvgRGBA(255, 193, 7, 40));
            badgeIcon->setText("\uE2C7"); // Folder
            badgeIcon->setTextColor(nvgRGB(255, 193, 7));
            titleText = targetSingleItem->name;
            subtitleText = "Папка";
        } else if (util::isArchiveFile(targetSingleItem->path)) {
            iconBadge->setBackgroundColor(nvgRGBA(255, 110, 64, 40));
            badgeIcon->setText("\uE2C6"); // Archive
            badgeIcon->setTextColor(nvgRGB(255, 110, 64));
            titleText = targetSingleItem->name;
            subtitleText = "Архив · " + util::formatFileSize(targetSingleItem->size);
        } else if (util::isGamePackage(targetSingleItem->path)) {
            iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 40));
            badgeIcon->setText("\uE0E0"); // Gamepad
            badgeIcon->setTextColor(nvgRGB(0, 224, 165)); // Emerald
            titleText = targetSingleItem->name;
            subtitleText = "Пакет игры · " + util::formatFileSize(targetSingleItem->size);
        } else if (isTextFile(targetSingleItem->path)) {
            iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 40));
            badgeIcon->setText("\uE873"); // Document
            badgeIcon->setTextColor(nvgRGB(0, 224, 165));
            titleText = targetSingleItem->name;
            subtitleText = "Текстовый файл · " + util::formatFileSize(targetSingleItem->size);
        } else {
            iconBadge->setBackgroundColor(nvgRGBA(33, 150, 243, 40));
            badgeIcon->setText("\uE24D"); // File
            badgeIcon->setTextColor(nvgRGB(33, 150, 243));
            titleText = targetSingleItem->name;
            subtitleText = util::formatFileSize(targetSingleItem->size);
        }
    } else {
        iconBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 40));
        badgeIcon->setText("\uE2C7");
        badgeIcon->setTextColor(nvgRGB(0, 224, 165));
        titleText = "Действия с файлами";
        subtitleText = currentDir_;
    }
    iconBadge->addView(badgeIcon);
    headerBox->addView(iconBadge);

    auto* headerTextCol = new brls::Box();
    headerTextCol->setAxis(brls::Axis::COLUMN);
    headerTextCol->setGrow(1.0f);

    auto* titleLbl = new brls::Label();
    titleLbl->setText(titleText);
    titleLbl->setFontSize(18.0f);
    titleLbl->setTextColor(nvgRGB(255, 255, 255));
    titleLbl->setSingleLine(true);
    headerTextCol->addView(titleLbl);

    auto* subLbl = new brls::Label();
    subLbl->setText(subtitleText);
    subLbl->setFontSize(13.0f);
    subLbl->setTextColor(nvgRGBA(0, 224, 165, 210));
    subLbl->setSingleLine(true);
    headerTextCol->addView(subLbl);

    headerBox->addView(headerTextCol);
    content->addView(headerBox);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    auto* applet = dynamic_cast<brls::AppletFrame*>(dialog->getView("brls/dialog/applet"));
    if (applet) {
        applet->setWidth(540.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(nvgRGBA(24, 26, 32, 252));
    }

    brls::View* firstOption = nullptr;

    // Safe option adder
    auto addOption = [&firstOption, content, dialog, this, restoreRow](const std::string& iconGlyph, NVGcolor iconCol, const std::string& labelText, std::function<void()> action, bool opensSubDialog = false) {
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
        row->setBackgroundColor(nvgRGBA(36, 39, 46, 190));

        auto* ic = new brls::Label();
        ic->setText(iconGlyph);
        ic->setFontSize(20.0f);
        ic->setTextColor(iconCol);
        ic->setMarginRight(14.0f);
        row->addView(ic);

        auto* lb = new brls::Label();
        lb->setText(labelText);
        lb->setFontSize(15.0f);
        lb->setTextColor(nvgRGB(240, 245, 255));
        lb->setGrow(1.0f);
        row->addView(lb);

        if (!firstOption) firstOption = row;

        row->registerClickAction([dialog, action, this, restoreRow, opensSubDialog](brls::View* v) {
            brls::sync([dialog, action, this, restoreRow, opensSubDialog]() {
                dialog->dismiss([action, this, restoreRow, opensSubDialog]() {
                    if (action) {
                        action();
                    }
                    if (!opensSubDialog) {
                        // Restore focus safely on next tick ONLY if not opening another modal dialog
                        brls::sync([this, restoreRow]() {
                            if (recycler) {
                                int maxRow = static_cast<int>(items_.size()) + (hasParentDir_ ? 1 : 0) - 1;
                                int validRow = std::clamp(restoreRow, 0, std::max(0, maxRow));
                                currentFocusedRow_ = validRow;
                                recycler->setDefaultCellFocus(brls::IndexPath(0, validRow));
                                recycler->selectRowAt(brls::IndexPath(0, validRow), false);
                                brls::Application::giveFocus(recycler);
                            }
                        });
                    }
                });
            });
            return true;
        });

        content->addView(row);
    };

    // Install Game (if target is game package)
    if (targetSingleItem && util::isGamePackage(targetSingleItem->path)) {
        addOption("\uE0E0", nvgRGB(0, 224, 165), "Установить игру", [this, item = *targetSingleItem]() {
            showInstallDialog(item);
        }, true);
    }

    // 1. Unarchive (if target is archive)
    if (targetSingleItem && util::isArchiveFile(targetSingleItem->path)) {
        addOption("\uE2C6", nvgRGB(255, 110, 64), "Распаковать архив", [this, item = *targetSingleItem]() {
            showArchiveDialog(item);
        }, true);
    }

    // View as text (if single file)
    if (targetSingleItem && !targetSingleItem->isDir) {
        addOption("\uE873", nvgRGB(0, 224, 165), "Просмотреть как текст", [this, item = *targetSingleItem]() {
            openTextViewer(item.path, item.name);
        }, true);
    }

    // 2. Paste (if clipboard active)
    if (hasClipboard) {
        std::string pasteText = (clip.op == util::ClipboardOp::Cut ? "Вставить перемещенное (" : "Вставить копию (") +
                                std::to_string(clip.paths.size()) + ")";
        addOption("\uE14F", nvgRGB(0, 224, 165), pasteText, [this]() {
            pasteClipboard();
        });
    }

    // 3. Copy
    if (hasSelection) {
        addOption("\uE14D", nvgRGB(64, 196, 255), "Копировать (" + std::to_string(selectedPaths_.size()) + ")", [this]() {
            auto& c = util::getClipboard();
            c.op = util::ClipboardOp::Copy;
            c.paths.assign(selectedPaths_.begin(), selectedPaths_.end());
            brls::Application::notify("Скопировано (" + std::to_string(c.paths.size()) + ")");
        });
    } else if (targetSingleItem) {
        addOption("\uE14D", nvgRGB(64, 196, 255), "Копировать", [this, item = *targetSingleItem]() {
            auto& c = util::getClipboard();
            c.op = util::ClipboardOp::Copy;
            c.paths = { item.path };
            brls::Application::notify("Скопировано: " + item.name);
        });
    }

    // 4. Cut
    if (hasSelection) {
        addOption("\uE14E", nvgRGB(255, 179, 0), "Вырезать (" + std::to_string(selectedPaths_.size()) + ")", [this]() {
            auto& c = util::getClipboard();
            c.op = util::ClipboardOp::Cut;
            c.paths.assign(selectedPaths_.begin(), selectedPaths_.end());
            brls::Application::notify("Вырезано (" + std::to_string(c.paths.size()) + ")");
        });
    } else if (targetSingleItem) {
        addOption("\uE14E", nvgRGB(255, 179, 0), "Вырезать", [this, item = *targetSingleItem]() {
            auto& c = util::getClipboard();
            c.op = util::ClipboardOp::Cut;
            c.paths = { item.path };
            brls::Application::notify("Вырезано: " + item.name);
        });
    }

    // 5. Rename
    if (targetSingleItem) {
        addOption("\uE254", nvgRGB(179, 136, 255), "Переименовать", [this, item = *targetSingleItem]() {
            showRenameDialog(item);
        }, true);
    }

    // 6. Delete
    if (hasSelection) {
        addOption("\uE872", nvgRGB(255, 82, 82), "Удалить (" + std::to_string(selectedPaths_.size()) + ")", [this]() {
            showDeleteConfirmDialog();
        }, true);
    } else if (targetSingleItem) {
        addOption("\uE872", nvgRGB(255, 82, 82), "Удалить", [this, item = *targetSingleItem]() {
            selectedPaths_.clear();
            selectedPaths_.insert(item.path);
            showDeleteConfirmDialog();
        }, true);
    }

    // 7. New Folder (always available)
    addOption("\uE2CC", nvgRGB(38, 198, 218), "Новая папка", [this]() {
        showNewFolderDialog();
    }, true);

    // 8. Selection helpers
    if (hasSelection) {
        addOption("\uE835", nvgRGB(180, 190, 200), "Снять всё выделение", [this]() {
            clearSelection();
        });
    } else if (targetSingleItem && currentFocusedRow_ >= 0) {
        addOption("\uE834", nvgRGB(0, 224, 165), "Выделить этот элемент", [this, row = currentFocusedRow_]() {
            toggleSelection(row);
        });
    }

    if (!items_.empty()) {
        addOption("\uE834", nvgRGB(100, 181, 246), "Выбрать всё (" + std::to_string(items_.size()) + ")", [this]() {
            selectAll();
        });
    }

    // Separator before cancel
    auto* cancelSep = new brls::Box();
    cancelSep->setHeight(1.0f);
    cancelSep->setMarginTop(6.0f);
    cancelSep->setMarginBottom(6.0f);
    cancelSep->setBackgroundColor(nvgRGBA(255, 255, 255, 20));
    content->addView(cancelSep);

    // Clean Cancel option for mouse / touch users
    addOption("\uE5CD", nvgRGB(239, 83, 80), "Отмена", nullptr);

    if (firstOption) {
        dialog->setLastFocusedView(firstOption);
        content->setLastFocusedView(firstOption);
    }

    dialog->open();

    if (firstOption) {
        brls::Application::giveFocus(firstOption);
        brls::sync([firstOption]() {
            brls::Application::giveFocus(firstOption);
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FileManagerDataSource
// ─────────────────────────────────────────────────────────────────────────────

int FileManagerView::FileManagerDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return static_cast<int>(parent_->items_.size()) + (parent_->hasParentDir_ ? 1 : 0);
}

brls::RecyclerCell* FileManagerView::FileManagerDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    util::logLine("FileManagerView: cellForRow " + std::to_string(index.row));
    FileManagerCell* cell = dynamic_cast<FileManagerCell*>(recycler->dequeueReusableCell("Cell"));
    if (!cell) {
        cell = FileManagerCell::create();
        cell->reuseIdentifier = "Cell";
    }

    bool isParentRow = (parent_->hasParentDir_ && index.row == 0);

    if (isParentRow) {
        if (cell->accentBar) cell->accentBar->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        cell->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        cell->icon->setText("\uE5D8"); // Material arrow up
        cell->icon->setTextColor(nvgRGB(150, 150, 160));
        cell->name->setText("..");
        cell->name->setTextColor(nvgRGB(220, 220, 220));
        cell->size->setText(brls::getStr("app/file_manager/parent_folder"));
        cell->date->setText("");

        cell->registerClickAction([parent = parent_](brls::View* view) {
            brls::sync([parent]() {
                parent->navigateUp();
            });
            return true;
        });
        return cell;
    }

    size_t itemIdx = parent_->hasParentDir_ ? (index.row - 1) : index.row;
    if (itemIdx >= parent_->items_.size()) return cell;

    const auto& item = parent_->items_[itemIdx];
    bool isSelected = parent_->selectedPaths_.count(item.path) > 0;

    // Apply visual selection style (emerald glow & accent bar, no ugly checkboxes)
    cell->setSelectedVisual(isSelected);

    // Icon & Name coloring
    if (item.isDir) {
        cell->icon->setText("\uE2C7"); // Material folder
        cell->icon->setTextColor(nvgRGB(255, 193, 7)); // Amber/Yellow
        cell->name->setTextColor(nvgRGB(255, 255, 255));
        cell->size->setText(brls::getStr("app/file_manager/folder_type"));
    } else if (util::isArchiveFile(item.path)) {
        cell->icon->setText("\uE2C6"); // Material archive
        cell->icon->setTextColor(nvgRGB(255, 87, 34)); // Orange
        cell->name->setTextColor(nvgRGB(255, 240, 230));
        cell->size->setText(util::formatFileSize(item.size));
    } else {
        bool isGame = util::isGamePackage(item.path);
        if (isGame) {
            cell->icon->setText("\uE0E0"); // Gamepad
            cell->icon->setTextColor(nvgRGB(0, 224, 165)); // Emerald icon for game packages
        } else if (isTextFile(item.path)) {
            cell->icon->setText("\uE873"); // Material document / article icon
            cell->icon->setTextColor(nvgRGB(0, 224, 165)); // Emerald green for text!
        } else {
            cell->icon->setText("\uE24D"); // Generic file
            cell->icon->setTextColor(nvgRGB(140, 150, 160));
        }
        cell->name->setTextColor(nvgRGB(230, 230, 230));
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
    cell->registerClickAction([parent = parent_, item](brls::View* view) {
        if (item.isDir) {
            brls::sync([parent, target = item.path]() {
                parent->navigateTo(target);
            });
        } else if (util::isGamePackage(item.path)) {
            parent->showInstallDialog(item);
        } else if (util::isArchiveFile(item.path)) {
            parent->showArchiveDialog(item);
        } else if (isTextFile(item.path)) {
            parent->openTextViewer(item.path, item.name);
        } else {
            // Give brief info notification
            brls::Application::notify(item.name + " (" + util::formatFileSize(item.size) + ")");
        }
        return true;
    });

    cell->getFocusEvent()->subscribe([parent = parent_, rowIndex = index.row](bool focused) {
        if (focused) parent->currentFocusedRow_ = static_cast<int>(rowIndex);
    });

    // Selection toggle action (Y button on gamepad, Space / Y on keyboard)
    cell->registerAction("app/file_manager/action_toggle_select"_i18n, brls::ControllerButton::BUTTON_Y,
        [parent = parent_, rowIndex = index.row, cell](brls::View* view) {
            parent->toggleSelectionOnCell(rowIndex, cell);
            return true;
        });

    cell->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_SPACE),
        [parent = parent_, rowIndex = index.row, cell](brls::View* view) {
            parent->toggleSelectionOnCell(rowIndex, cell);
            return true;
        });

    cell->registerAction(brls::BrlsKeyCombination(brls::BRLS_KBD_KEY_Y),
        [parent = parent_, rowIndex = index.row, cell](brls::View* view) {
            parent->toggleSelectionOnCell(rowIndex, cell);
            return true;
        });

    return cell;
}

} // namespace ui
