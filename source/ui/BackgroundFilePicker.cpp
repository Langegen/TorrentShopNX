#include "BackgroundFilePicker.hpp"
#include "ThemeManager.hpp"
#include "../utils/log.h"
#include "../utils/string_utils.h"
#include <filesystem>
#include <algorithm>

using namespace brls::literals;

namespace ui {

static std::string formatBytesLocal(uintmax_t bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    return std::string(buf);
}

static bool isImageExtension(const std::string& ext) {
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return (lower == ".jpg" || lower == ".jpeg" || lower == ".png");
}

BackgroundFilePicker::BackgroundFilePicker(const std::string& initialDir) {
    if (!initialDir.empty() && std::filesystem::exists(initialDir)) {
        currentDir_ = initialDir;
    } else {
#ifdef __SWITCH__
        std::string bgDir = "sdmc:/switch/TorrentShopNX/backgrounds";
        if (std::filesystem::exists(bgDir)) {
            currentDir_ = bgDir;
        } else {
            currentDir_ = "sdmc:/";
        }
#else
        currentDir_ = "./";
#endif
    }
}

brls::View* BackgroundFilePicker::createContentView() {
    rootBox_ = new brls::Box(brls::Axis::COLUMN);
    rootBox_->setWidthPercentage(100.0f);
    rootBox_->setHeightPercentage(100.0f);
    rootBox_->setPadding(16.0f, 24.0f, 16.0f, 24.0f);

    // Current Path Bar
    auto* pathBar = new brls::Box(brls::Axis::ROW);
    pathBar->setAlignItems(brls::AlignItems::CENTER);
    pathBar->setMarginBottom(12.0f);
    pathBar->setPadding(8.0f, 16.0f, 8.0f, 16.0f);
    pathBar->setBackgroundColor(nvgRGBA(18, 28, 42, 160));
    pathBar->setCornerRadius(8.0f);

    auto* folderIcon = new brls::Label();
    folderIcon->setText("\uE2C7"); // Material folder icon
    folderIcon->setFontSize(20.0f);
    folderIcon->setTextColor(ThemeManager::instance().getAccentColor());
    folderIcon->setMarginRight(10.0f);
    pathBar->addView(folderIcon);

    pathLabel_ = new brls::Label();
    pathLabel_->setText(currentDir_);
    pathLabel_->setFontSize(16.0f);
    pathLabel_->setTextColor(nvgRGB(220, 225, 230));
    pathLabel_->setGrow(1.0f);
    pathLabel_->setSingleLine(true);
    pathBar->addView(pathLabel_);

    rootBox_->addView(pathBar);

    // Scrollable File List Container
    scroll_ = new brls::ScrollingFrame();
    scroll_->setWidthPercentage(100.0f);
    scroll_->setGrow(1.0f);

    listContainer_ = new brls::Box(brls::Axis::COLUMN);
    listContainer_->setWidthPercentage(100.0f);
    listContainer_->setGrow(1.0f);
    scroll_->setContentView(listContainer_);

    rootBox_->addView(scroll_);

    auto* applet = new brls::AppletFrame(rootBox_);
    applet->setTitle("app/appearance/file_picker_title"_i18n);
    applet->setIcon(std::string(BRLS_RESOURCES) + "img/icon_settings.png");
    applet->setFooterVisibility(brls::Visibility::VISIBLE);

    // Register B button action to navigate up or exit
    registerAction("app/common/back"_i18n, brls::BUTTON_B, [this](brls::View* view) {
        std::filesystem::path p(currentDir_);
        std::filesystem::path parent = p.parent_path();
        if (!parent.empty() && parent != p && parent.generic_string() != currentDir_) {
            navigateTo(parent.generic_string());
            return true;
        }
        brls::Application::popActivity();
        return true;
    });

    return applet;
}

void BackgroundFilePicker::onContentAvailable() {
    refreshList();
}

void BackgroundFilePicker::navigateTo(const std::string& path) {
    currentDir_ = path;
    if (pathLabel_) {
        pathLabel_->setText(currentDir_);
    }
    refreshList();
}

void BackgroundFilePicker::refreshList() {
    if (!listContainer_) return;
    listContainer_->clearViews();

    std::error_code ec;
    std::filesystem::path cur(currentDir_);

    // Parent directory row
    if (cur.has_parent_path() && cur.parent_path() != cur) {
        auto* parentCell = new brls::DetailCell();
        parentCell->setText("..");
        parentCell->setDetailText("app/appearance/folder_parent"_i18n);
        parentCell->registerClickAction([this, cur](brls::View* v) {
            navigateTo(cur.parent_path().generic_string());
            return true;
        });
        listContainer_->addView(parentCell);
    }

    struct ItemInfo {
        std::string name;
        std::string fullPath;
        bool isDir;
        uintmax_t size;
    };

    std::vector<ItemInfo> dirs;
    std::vector<ItemInfo> images;

    for (const auto& entry : std::filesystem::directory_iterator(cur, ec)) {
        if (ec) break;
        std::string name = entry.path().filename().generic_string();
        if (name.empty() || name[0] == '.') continue; // Skip hidden

        bool isDir = entry.is_directory(ec);
        if (isDir) {
            dirs.push_back({ name, entry.path().generic_string(), true, 0 });
        } else if (entry.is_regular_file(ec)) {
            std::string ext = entry.path().extension().generic_string();
            if (isImageExtension(ext)) {
                uintmax_t sz = entry.file_size(ec);
                images.push_back({ name, entry.path().generic_string(), false, sz });
            }
        }
    }

    std::sort(dirs.begin(), dirs.end(), [](const ItemInfo& a, const ItemInfo& b) {
        return a.name < b.name;
    });
    std::sort(images.begin(), images.end(), [](const ItemInfo& a, const ItemInfo& b) {
        return a.name < b.name;
    });

    // Add Directories
    for (const auto& d : dirs) {
        auto* cell = new brls::DetailCell();
        cell->setText(d.name + "/");
        cell->setDetailText("app/appearance/folder_type"_i18n);
        cell->registerClickAction([this, path = d.fullPath](brls::View* v) {
            navigateTo(path);
            return true;
        });
        listContainer_->addView(cell);
    }

    // Add Image Files
    for (const auto& img : images) {
        auto* cell = new brls::DetailCell();
        cell->setText(img.name);
        cell->setDetailText(formatBytesLocal(img.size));
        cell->registerClickAction([this, fullPath = img.fullPath](brls::View* v) {
            onItemSelected(fullPath, false);
            return true;
        });
        listContainer_->addView(cell);
    }

    if (dirs.empty() && images.empty()) {
        auto* emptyBox = new brls::Box();
        emptyBox->setPadding(40.0f, 20.0f, 40.0f, 20.0f);
        emptyBox->setJustifyContent(brls::JustifyContent::CENTER);
        emptyBox->setAlignItems(brls::AlignItems::CENTER);

        auto* emptyLabel = new brls::Label();
        emptyLabel->setText("app/appearance/folder_empty"_i18n);
        emptyLabel->setFontSize(18.0f);
        emptyLabel->setTextColor(nvgRGB(140, 150, 160));
        emptyBox->addView(emptyLabel);
        listContainer_->addView(emptyBox);
    }
}

void BackgroundFilePicker::onItemSelected(const std::string& path, bool isDir) {
    if (isDir) {
        navigateTo(path);
    } else {
        util::logLine("BackgroundFilePicker: selected custom wallpaper: " + path);
        ThemeManager::instance().setCustomBackgroundPath(path);
        brls::Application::notify("app/appearance/bg_applied"_i18n);
        brls::Application::popActivity();
    }
}

} // namespace ui
