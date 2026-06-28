#include "FileSelectView.hpp"
#include "DownloadUiManager.hpp"
#include "MainMenu.hpp"
#include "DownloadsView.hpp"
#include <iomanip>
#include <algorithm>
#include <cctype>

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

// ─────────────────────────────────────────────────────────────────────────────
// FileSelectView
// ─────────────────────────────────────────────────────────────────────────────

FileSelectView::FileSelectView(const Game& game) : game_(game) {}

void FileSelectView::onContentAvailable() {
    util::logLine("FileSelectView: onContentAvailable start");
    title->setText(cleanTitle(game_.title));
    subtitle->setText("Получение списка файлов торрента...");

    // Grab focus to prevent navigation events from reaching the background catalog view
    fileListScroll->setFocusable(true);
    brls::Application::giveFocus(fileListScroll);

    this->registerAction("Выбрать/Снять все", brls::ControllerButton::BUTTON_X,
        [this](brls::View*) { toggleAllSelection(); return true; });

    this->registerAction("Начать загрузку", brls::ControllerButton::BUTTON_START,
        [this](brls::View*) { startDownloadAndGoToDownloads(); return true; });

    brls::async([this]() {
        util::logLine("FileSelectView: probe thread started");
        std::vector<torrent::TorrentFileInfo> probedFiles;
        std::string err;

        util::logLine("FileSelectView: probing magnet " + game_.magnet);
        bool success = ui::DownloadManager::instance().getImpl()
                           .probeTorrentFiles(game_.magnet, probedFiles, &err);
        util::logLine("FileSelectView: probe done, success=" + std::to_string(success) +
                      " count=" + std::to_string(probedFiles.size()));

        brls::sync([this, success, probedFiles, err]() {
            util::logLine("FileSelectView: sync callback, success=" +
                          std::to_string(success) + " count=" +
                          std::to_string(probedFiles.size()));

            if (!this->getContentView()) {
                util::logLine("FileSelectView: view gone, aborting");
                return;
            }

            if (success && !probedFiles.empty()) {
                files_    = probedFiles;
                selected_.assign(files_.size(), true);
                subtitle->setText("Выберите файлы для загрузки");
                updateTotalSize();
                rebuildFileList();
                util::logLine("FileSelectView: rebuildFileList done, rows=" +
                              std::to_string(files_.size()));
                
                // Allow focus to leave the scroll frame now that items are focusable
                fileListScroll->setFocusable(false);

                // Set default focus to the first file item
                if (fileListBox && !fileListBox->getChildren().empty()) {
                    brls::Application::giveFocus(fileListBox->getChildren().front());
                }
            } else {
                subtitle->setText("Ошибка загрузки метаданных");
                brls::Application::notify("Ошибка: " +
                    (err.empty() ? "превышено время ожидания" : err));
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

    for (size_t i = 0; i < files_.size(); ++i) {
        const auto& file = files_[i];
        bool isSel = (i < selected_.size()) && selected_[i];

        // Row container
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

        // File name label
        auto* nameLbl = new brls::Label();
        nameLbl->setGrow(1.0f);
        nameLbl->setHeight(brls::View::AUTO);
        nameLbl->setFontSize(16);
        nameLbl->setText(file.name);
        nameLbl->setMarginLeft(10);
        nameLbl->setMarginRight(10);
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
        size_t idx = i; // capture by value
        row->registerClickAction([this, idx](brls::View*) {
            if (idx < selected_.size()) {
                selected_[idx] = !selected_[idx];
                updateTotalSize();
                updateRowSelectionState(idx);
            }
            return true;
        });

        fileListBox->addView(row);
        util::logLine("FileSelectView: added row " + std::to_string(i) +
                      " name='" + file.name + "'");
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

    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            selectedIndices.push_back(files_[i].index);
            if (isSwitchGameFile(files_[i].name) && files_[i].size > largestGameSize) {
                largestGameSize = files_[i].size;
                forcedIndex     = files_[i].index;
                forcedName      = files_[i].name;
            }
        }
    }

    if (selectedIndices.empty()) {
        brls::Application::notify("Не выбрано ни одного файла для загрузки!");
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

    for (size_t i = 0; i < files_.size(); ++i) {
        if (i < selected_.size() && selected_[i]) {
            std::vector<int> singleSelected = { files_[i].index };
            std::string itemTitle = cleanTitle(game_.title);
            if (selectedIndices.size() > 1) {
                itemTitle += " (" + files_[i].name + ")";
            }
            Game singleGame = game_;
            singleGame.title = itemTitle;
            singleGame.topic_id = game_.topic_id + "_" + std::to_string(files_[i].index);
            
            ui::DownloadManager::instance().addDownload(singleGame, singleSelected, files_[i].index, files_[i].name);
        }
    }

    // Pop back to MainMenu
    while (brls::Application::getActivitiesStack().size() > 1)
        brls::Application::popActivity(brls::TransitionAnimation::NONE);

    // Redirect directly to the Downloads window
    brls::Application::pushActivity(new ui::DownloadsView());
}

void FileSelectView::updateRowSelectionState(size_t idx) {
    if (!fileListBox || idx >= fileListBox->getChildren().size()) return;
    brls::View* rowView = fileListBox->getChildren()[idx];
    brls::Box* row = dynamic_cast<brls::Box*>(rowView);
    if (!row || row->getChildren().empty()) return;

    brls::Label* chk = dynamic_cast<brls::Label*>(row->getChildren().front());
    if (!chk) return;

    bool isSel = (idx < selected_.size()) && selected_[idx];
    chk->setText(isSel ? "[V]" : "[ ]");
    chk->setTextColor(isSel ? nvgRGB(76, 175, 80) : nvgRGB(180, 180, 180));
}

} // namespace ui
