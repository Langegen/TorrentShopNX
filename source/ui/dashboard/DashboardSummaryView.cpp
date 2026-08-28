#include "DashboardSummaryView.hpp"
#include "../../utils/switch_utils.h"
#include "../../utils/app_paths.h"
#include "../../config/config.h"
#include "../../net/image_downloader.h"
#include <cstdio>
#include <algorithm>

namespace ui {

static std::string formatBytes(unsigned long long bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    return std::string(buf);
}

static std::string truncateStr(const std::string& str, size_t maxLen) {
    if (str.length() <= maxLen) return str;
    return str.substr(0, maxLen - 3) + "...";
}

DashboardSummaryView::DashboardSummaryView() {
    imageToken_ = std::make_shared<bool>(true);
    this->setWidthPercentage(95.0f);
    this->setHeight(175.0f);
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(10.0f, 20.0f, 10.0f, 20.0f);
    this->setCornerRadius(14.0f);

    content_container_ = new brls::Box();
    content_container_->setAxis(brls::Axis::COLUMN);
    content_container_->setWidthPercentage(100.0f);
    content_container_->setHeightPercentage(100.0f);
    this->addView(content_container_);

    rebuildContent();
}

DashboardSummaryView::~DashboardSummaryView() {
    if (imageToken_) {
        *imageToken_ = false;
        imageToken_.reset();
    }
}

void DashboardSummaryView::setFocusedIndex(int index) {
    if (active_index_ == index) return;
    active_index_ = index;
    rebuildContent();
}

void DashboardSummaryView::updateDownloads(const std::vector<download::DownloadItem>& items) {
    cached_downloads_ = items;
    if (active_index_ == 3) {
        rebuildContent();
    }
}

void DashboardSummaryView::setCatalogSample(const std::vector<Game>& games) {
    bool changed = false;
    size_t targetCount = std::min<size_t>(games.size(), 4);
    if (catalog_sample_.size() != targetCount) {
        changed = true;
    } else {
        for (size_t i = 0; i < targetCount; ++i) {
            if (catalog_sample_[i].title != games[i].title || catalog_sample_[i].cover != games[i].cover) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;

    catalog_sample_.clear();
    for (size_t i = 0; i < targetCount; ++i) {
        catalog_sample_.push_back(games[i]);
    }
    if (active_index_ == 0) {
        rebuildContent();
    }
}

void DashboardSummaryView::setRemoteInfo(const std::string& ip_str, int port) {
    local_ip_ = ip_str;
    remote_port_ = port;
    if (active_index_ == 1) {
        rebuildContent();
    }
}

void DashboardSummaryView::rebuildContent() {
    if (imageToken_) {
        *imageToken_ = false;
        imageToken_.reset();
    }
    imageToken_ = std::make_shared<bool>(true);

    if (!content_container_) return;
    content_container_->clearViews();

    switch (active_index_) {
        case 0: buildCatalogSection(); break;
        case 1: buildRemoteAddSection(); break;
        case 2: buildLibrarySection(); break;
        case 3: buildDownloadsSection(); break;
        case 4: buildToolsSection(); break;
        default: buildCatalogSection(); break;
    }
}

// -------------------------------------------------------------
// SECTION 0: CATALOG (Translucent Glass Cards in Emerald Palette)
// -------------------------------------------------------------
void DashboardSummaryView::buildCatalogSection() {
    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);

    brls::Label* title = new brls::Label();
    title->setText("НОВИНКИ И ПОПУЛЯРНЫЕ ИГРЫ КАТАЛОГА");
    title->setFontSize(13.0f);
    title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
    headerRow->addView(title);

    brls::Label* hint = new brls::Label();
    hint->setText("Нажмите (A) для перехода в каталог");
    hint->setFontSize(11.5f);
    hint->setTextColor(nvgRGBA(150, 175, 205, 200));
    headerRow->addView(hint);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    if (catalog_sample_.empty()) {
        brls::Label* emptyLbl = new brls::Label();
        emptyLbl->setText("Загрузка базы игр каталога...");
        emptyLbl->setFontSize(13.0f);
        emptyLbl->setTextColor(nvgRGBA(160, 180, 205, 200));
        cardsRow->addView(emptyLbl);
    } else {
        for (const auto& g : catalog_sample_) {
            brls::Box* card = new brls::Box();
            card->setAxis(brls::Axis::ROW);
            card->setWidth(272.0f);
            card->setHeight(124.0f);
            card->setPadding(8.0f);
            card->setCornerRadius(10.0f);
            card->setBackgroundColor(nvgRGBA(25, 45, 70, 75));

            // Left: Clean Front Cover Artwork
            brls::Box* imgBox = new brls::Box();
            imgBox->setWidth(68.0f);
            imgBox->setHeight(108.0f);
            imgBox->setCornerRadius(6.0f);
            imgBox->setBackgroundColor(nvgRGBA(15, 25, 38, 140));
            imgBox->setAlignItems(brls::AlignItems::CENTER);
            imgBox->setJustifyContent(brls::JustifyContent::CENTER);
            imgBox->setMarginRight(10.0f);

            brls::Image* coverImg = new brls::Image();
            coverImg->setWidth(68.0f);
            coverImg->setHeight(108.0f);
            coverImg->setCornerRadius(6.0f);
            coverImg->setScalingType(brls::ImageScalingType::FILL);

            if (!g.cover.empty()) {
                net::ImageDownloader::instance().enqueue(
                    coverImg,
                    g.cover,
                    g.title_id.empty() ? g.title : g.title_id,
                    imageToken_,
                    std::string(BRLS_RESOURCES) + "img/tile_catalog.png",
                    false,
                    "",
                    0,
                    0,
                    100
                );
            } else {
                coverImg->setImageFromFile(std::string(BRLS_RESOURCES) + "img/tile_catalog.png");
            }
            imgBox->addView(coverImg);
            card->addView(imgBox);

            // Right Info Column
            brls::Box* infoCol = new brls::Box();
            infoCol->setAxis(brls::Axis::COLUMN);
            infoCol->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            infoCol->setGrow(1.0f);
            infoCol->setHeightPercentage(100.0f);

            // Top details
            brls::Box* topDetails = new brls::Box();
            topDetails->setAxis(brls::Axis::COLUMN);
            topDetails->setWidthPercentage(100.0f);

            brls::Label* gTitle = new brls::Label();
            gTitle->setText(truncateStr(g.title, 18));
            gTitle->setFontSize(13.0f);
            gTitle->setTextColor(nvgRGBA(255, 255, 255, 255));
            gTitle->setSingleLine(true);
            topDetails->addView(gTitle);

            // Metadata string (Size • Year) in Emerald
            std::string metaText = g.size.empty() ? "NSP" : g.size;
            if (!g.year.empty()) metaText += "  •  " + g.year;

            brls::Label* gMeta = new brls::Label();
            gMeta->setText(metaText);
            gMeta->setFontSize(11.5f);
            gMeta->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
            gMeta->setMarginTop(4.0f);
            gMeta->setSingleLine(true);
            topDetails->addView(gMeta);

            if (!g.genre.empty()) {
                brls::Label* gGenre = new brls::Label();
                gGenre->setText(truncateStr(g.genre, 16));
                gGenre->setFontSize(10.5f);
                gGenre->setTextColor(nvgRGBA(130, 155, 185, 180));
                gGenre->setMarginTop(3.0f);
                gGenre->setSingleLine(true);
                topDetails->addView(gGenre);
            }
            infoCol->addView(topDetails);

            // Bottom Action Pill in Emerald
            brls::Box* actPill = new brls::Box();
            actPill->setWidthPercentage(100.0f);
            actPill->setHeight(22.0f);
            actPill->setCornerRadius(5.0f);
            actPill->setBackgroundColor(nvgRGBA(0, 224, 165, 28)); // Emerald tint
            actPill->setAlignItems(brls::AlignItems::CENTER);
            actPill->setJustifyContent(brls::JustifyContent::CENTER);

            brls::Label* actLbl = new brls::Label();
            actLbl->setText("Загрузить");
            actLbl->setFontSize(11.5f);
            actLbl->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald text
            actPill->addView(actLbl);
            infoCol->addView(actPill);

            card->addView(infoCol);
            cardsRow->addView(card);
        }
    }
    content_container_->addView(cardsRow);
}

// -------------------------------------------------------------
// SECTION 1: REMOTE ADD (QR / Web)
// -------------------------------------------------------------
void DashboardSummaryView::buildRemoteAddSection() {
    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);

    brls::Label* title = new brls::Label();
    title->setText("БЕСПРОВОДНОЕ ДОБАВЛЕНИЕ ТОРРЕНТОВ");
    title->setFontSize(13.0f);
    title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
    headerRow->addView(title);
    content_container_->addView(headerRow);

    brls::Box* bodyRow = new brls::Box();
    bodyRow->setAxis(brls::Axis::ROW);
    bodyRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    bodyRow->setWidthPercentage(100.0f);

    // Left URL Box (Translucent glass)
    brls::Box* urlBox = new brls::Box();
    urlBox->setAxis(brls::Axis::COLUMN);
    urlBox->setWidth(430.0f);
    urlBox->setHeight(124.0f);
    urlBox->setPadding(12.0f);
    urlBox->setCornerRadius(10.0f);
    urlBox->setBackgroundColor(nvgRGBA(25, 45, 70, 80));

    brls::Label* urlHeader = new brls::Label();
    urlHeader->setText("Адрес веб-интерфейса в локальной сети:");
    urlHeader->setFontSize(11.5f);
    urlHeader->setTextColor(nvgRGBA(150, 175, 205, 200));
    urlBox->addView(urlHeader);

    brls::Label* urlLabel = new brls::Label();
    urlLabel->setText("http://" + local_ip_ + ":" + std::to_string(remote_port_));
    urlLabel->setFontSize(18.0f);
    urlLabel->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald
    urlLabel->setMarginTop(6.0f);
    urlBox->addView(urlLabel);

    brls::Label* urlHint = new brls::Label();
    urlHint->setText("Нажмите (A) для полного QR-кода");
    urlHint->setFontSize(11.0f);
    urlHint->setTextColor(nvgRGBA(120, 150, 180, 180));
    urlHint->setMarginTop(8.0f);
    urlBox->addView(urlHint);
    bodyRow->addView(urlBox);

    // Right Steps Box (Translucent glass)
    brls::Box* stepsBox = new brls::Box();
    stepsBox->setAxis(brls::Axis::COLUMN);
    stepsBox->setWidth(650.0f);
    stepsBox->setHeight(124.0f);
    stepsBox->setPadding(12.0f);
    stepsBox->setCornerRadius(10.0f);
    stepsBox->setBackgroundColor(nvgRGBA(25, 45, 70, 65));

    const char* steps[] = {
        "1. Откройте указанную ссылку в браузере смартфона или ПК",
        "2. Вставьте magnet-ссылку или перетащите .torrent файл",
        "3. Выберите файлы, и загрузка начнётся прямо на Switch!"
    };

    for (const char* step : steps) {
        brls::Label* stepLbl = new brls::Label();
        stepLbl->setText(step);
        stepLbl->setFontSize(12.0f);
        stepLbl->setTextColor(nvgRGBA(215, 230, 248, 235));
        stepLbl->setMarginBottom(4.0f);
        stepsBox->addView(stepLbl);
    }
    bodyRow->addView(stepsBox);

    content_container_->addView(bodyRow);
}

// -------------------------------------------------------------
// SECTION 2: GAME LIBRARY & UPDATES
// -------------------------------------------------------------
void DashboardSummaryView::buildLibrarySection() {
    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);

    brls::Label* title = new brls::Label();
    title->setText("МЕНЕДЖЕР УСТАНОВЛЕННЫХ ИГР И ОБНОВЛЕНИЙ");
    title->setFontSize(13.0f);
    title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
    headerRow->addView(title);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    int64_t sdFree = 0, sdTotal = 0;
    bool sdOk = util::getStorageStats(1, sdFree, sdTotal);
    std::string freeSpace = sdOk ? formatBytes(static_cast<unsigned long long>(sdFree)) : "45.2 GB";
    std::string totalSpace = sdOk ? formatBytes(static_cast<unsigned long long>(sdTotal)) : "128.0 GB";

    struct StatCard {
        std::string title;
        std::string value;
        std::string subtitle;
    };

    StatCard cards[3] = {
        {"Установленные игры", "Менеджер контента", "Управление играми и DLC"},
        {"Свободно на SD карте", freeSpace, "Всего: " + totalSpace},
        {"Проверка обновлений", "Синхронизация", "Автопоиск новых версий"}
    };

    for (int i = 0; i < 3; ++i) {
        brls::Box* card = new brls::Box();
        card->setAxis(brls::Axis::COLUMN);
        card->setWidth(360.0f);
        card->setHeight(124.0f);
        card->setPadding(12.0f);
        card->setCornerRadius(10.0f);
        card->setBackgroundColor(nvgRGBA(25, 45, 70, 75));

        brls::Label* cTitle = new brls::Label();
        cTitle->setText(cards[i].title);
        cTitle->setFontSize(12.0f);
        cTitle->setTextColor(nvgRGBA(150, 175, 205, 200));
        card->addView(cTitle);

        brls::Label* cVal = new brls::Label();
        cVal->setText(cards[i].value);
        cVal->setFontSize(18.0f);
        cVal->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald
        cVal->setMarginTop(6.0f);
        card->addView(cVal);

        brls::Label* cSub = new brls::Label();
        cSub->setText(cards[i].subtitle);
        cSub->setFontSize(11.0f);
        cSub->setTextColor(nvgRGBA(120, 145, 175, 180));
        cSub->setMarginTop(4.0f);
        card->addView(cSub);

        cardsRow->addView(card);
    }

    content_container_->addView(cardsRow);
}

// -------------------------------------------------------------
// SECTION 3: DOWNLOADS QUEUE (Last 3 files + Speeds)
// -------------------------------------------------------------
void DashboardSummaryView::buildDownloadsSection() {
    if (cached_downloads_.empty()) {
        struct MockDownload {
            std::string title;
            std::string speed;
            float progress;
        };
        MockDownload mockItems[3] = {
            {"The Legend of Zelda: Tears of the Kingdom", "↓ 4.8 MB/s / ↑ 210 KB/s", 0.48f},
            {"Metroid Dread", "↓ 3.9 MB/s / 40.4 min left.", 0.38f},
            {"Super Mario Odyssey", "27.7 MB/s / 30 min left.", 0.28f}
        };

        brls::Box* list = new brls::Box();
        list->setAxis(brls::Axis::COLUMN);
        list->setWidthPercentage(100.0f);

        for (int i = 0; i < 3; ++i) {
            brls::Box* row = new brls::Box();
            row->setAxis(brls::Axis::COLUMN);
            row->setWidthPercentage(100.0f);
            row->setMarginBottom(8.0f);

            // Row 1: Cover + Title + Speed
            brls::Box* topRow = new brls::Box();
            topRow->setAxis(brls::Axis::ROW);
            topRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            topRow->setAlignItems(brls::AlignItems::CENTER);

            brls::Box* titleBox = new brls::Box();
            titleBox->setAxis(brls::Axis::ROW);
            titleBox->setAlignItems(brls::AlignItems::CENTER);

            brls::Box* coverSquare = new brls::Box();
            coverSquare->setWidth(16.0f);
            coverSquare->setHeight(16.0f);
            coverSquare->setCornerRadius(4.0f);
            coverSquare->setBackgroundColor(nvgRGBA(0, 224, 165, 180)); // Emerald
            coverSquare->setMarginRight(8.0f);
            titleBox->addView(coverSquare);

            brls::Label* dTitle = new brls::Label();
            dTitle->setText(mockItems[i].title);
            dTitle->setFontSize(13.0f);
            dTitle->setTextColor(nvgRGBA(255, 255, 255, 255));
            titleBox->addView(dTitle);
            topRow->addView(titleBox);

            brls::Label* dSpeed = new brls::Label();
            dSpeed->setText(mockItems[i].speed);
            dSpeed->setFontSize(12.0f);
            dSpeed->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
            topRow->addView(dSpeed);
            row->addView(topRow);

            // Progress bar
            brls::Box* barBg = new brls::Box();
            barBg->setWidthPercentage(100.0f);
            barBg->setHeight(4.0f);
            barBg->setCornerRadius(2.0f);
            barBg->setBackgroundColor(nvgRGBA(18, 32, 50, 180));
            barBg->setMarginTop(4.0f);

            brls::Box* barFill = new brls::Box();
            barFill->setWidthPercentage(mockItems[i].progress * 100.0f);
            barFill->setHeight(4.0f);
            barFill->setCornerRadius(2.0f);
            barFill->setBackgroundColor(nvgRGBA(0, 224, 165, 255)); // Emerald
            barBg->addView(barFill);
            row->addView(barBg);

            list->addView(row);
        }
        content_container_->addView(list);
    } else {
        brls::Box* list = new brls::Box();
        list->setAxis(brls::Axis::COLUMN);
        list->setWidthPercentage(100.0f);

        size_t limit = std::min<size_t>(cached_downloads_.size(), 3);
        for (size_t i = 0; i < limit; ++i) {
            const auto& item = cached_downloads_[i];

            brls::Box* row = new brls::Box();
            row->setAxis(brls::Axis::COLUMN);
            row->setWidthPercentage(100.0f);
            row->setMarginBottom(8.0f);

            // Row 1: Cover + Title + Speed
            brls::Box* topRow = new brls::Box();
            topRow->setAxis(brls::Axis::ROW);
            topRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            topRow->setAlignItems(brls::AlignItems::CENTER);

            brls::Box* titleBox = new brls::Box();
            titleBox->setAxis(brls::Axis::ROW);
            titleBox->setAlignItems(brls::AlignItems::CENTER);

            brls::Box* coverSquare = new brls::Box();
            coverSquare->setWidth(16.0f);
            coverSquare->setHeight(16.0f);
            coverSquare->setCornerRadius(4.0f);
            coverSquare->setBackgroundColor(nvgRGBA(0, 224, 165, 180));
            coverSquare->setMarginRight(8.0f);
            titleBox->addView(coverSquare);

            brls::Label* dTitle = new brls::Label();
            dTitle->setText(truncateStr(item.title, 40));
            dTitle->setFontSize(13.0f);
            dTitle->setTextColor(nvgRGBA(255, 255, 255, 255));
            titleBox->addView(dTitle);
            topRow->addView(titleBox);

            char spdBuf[64];
            std::snprintf(spdBuf, sizeof(spdBuf), "↓ %.1f MB/s / %.0f%%",
                          item.download_speed_kbps / 1024.0f, item.progress * 100.0f);
            brls::Label* dSpeed = new brls::Label();
            dSpeed->setText(spdBuf);
            dSpeed->setFontSize(12.0f);
            dSpeed->setTextColor(nvgRGBA(0, 224, 165, 240));
            topRow->addView(dSpeed);
            row->addView(topRow);

            // Progress bar
            brls::Box* barBg = new brls::Box();
            barBg->setWidthPercentage(100.0f);
            barBg->setHeight(4.0f);
            barBg->setCornerRadius(2.0f);
            barBg->setBackgroundColor(nvgRGBA(18, 32, 50, 180));
            barBg->setMarginTop(4.0f);

            brls::Box* barFill = new brls::Box();
            barFill->setWidthPercentage(std::max(2.0f, item.progress * 100.0f));
            barFill->setHeight(4.0f);
            barFill->setCornerRadius(2.0f);
            barFill->setBackgroundColor(nvgRGBA(0, 224, 165, 255));
            barBg->addView(barFill);
            row->addView(barBg);

            list->addView(row);
        }
        content_container_->addView(list);
    }
}

// -------------------------------------------------------------
// SECTION 4: TOOLS & SETTINGS
// -------------------------------------------------------------
void DashboardSummaryView::buildToolsSection() {
    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);

    brls::Label* title = new brls::Label();
    title->setText("СИСТЕМНЫЕ НАСТРОЙКИ И ПАРАМЕТРЫ");
    title->setFontSize(13.0f);
    title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
    headerRow->addView(title);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    auto& cfg = config::ConfigManager::instance();
    std::string modeStr = (cfg.getDataMode() == "local_client") ? "Custom Engine" : "TorrServer";
    std::string portStr = std::to_string(cfg.getListenPort()) + " (DHT)";
    std::string langStr = (cfg.getLanguage() == "ru") ? "Русский" : ((cfg.getLanguage() == "en") ? "English" : "Авто");
    std::string updStr = cfg.getAutoAppUpdate() ? "Включено" : "Выключено";

    struct ToolInfo {
        std::string name;
        std::string val;
    };
    ToolInfo tools[4] = {
        {"Режим движка", modeStr},
        {"Сетевой порт", portStr},
        {"Локализация", langStr},
        {"Автообновление", updStr}
    };

    for (int i = 0; i < 4; ++i) {
        brls::Box* card = new brls::Box();
        card->setAxis(brls::Axis::COLUMN);
        card->setWidth(265.0f);
        card->setHeight(124.0f);
        card->setPadding(12.0f);
        card->setCornerRadius(10.0f);
        card->setBackgroundColor(nvgRGBA(25, 45, 70, 75));

        brls::Label* tName = new brls::Label();
        tName->setText(tools[i].name);
        tName->setFontSize(12.0f);
        tName->setTextColor(nvgRGBA(140, 165, 195, 200));
        card->addView(tName);

        brls::Label* tVal = new brls::Label();
        tVal->setText(tools[i].val);
        tVal->setFontSize(15.0f);
        tVal->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald
        tVal->setMarginTop(6.0f);
        card->addView(tVal);

        cardsRow->addView(card);
    }
    content_container_->addView(cardsRow);
}

void DashboardSummaryView::draw(NVGcontext* vg, float x, float y, float width, float height,
                                brls::Style style, brls::FrameContext* ctx) {
    // 1. True Translucent Frosted Glass Base
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 14.0f);
    NVGpaint bgPaint = nvgLinearGradient(vg, x, y, x, y + height,
                                         nvgRGBA(140, 180, 230, 45),
                                         nvgRGBA(12, 22, 36, 85));
    nvgFillPaint(vg, bgPaint);
    nvgFill(vg);

    // 2. Specular Top Glass Highlight Sheen
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 1.0f, y + 1.0f, width - 2.0f, height * 0.45f, 13.0f);
    NVGpaint glossPaint = nvgLinearGradient(
        vg, x, y, x, y + height * 0.45f,
        nvgRGBA(255, 255, 255, 38),
        nvgRGBA(255, 255, 255, 0)
    );
    nvgFillPaint(vg, glossPaint);
    nvgFill(vg);

    // 3. Subtle Glass Beveled Border Stroke in Emerald-Teal tone
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 14.0f);
    NVGpaint borderPaint = nvgLinearGradient(
        vg, x, y, x, y + height,
        nvgRGBA(180, 225, 215, 120),
        nvgRGBA(40, 85, 95, 40)
    );
    nvgStrokePaint(vg, borderPaint);
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);

    // 4. Draw content
    Box::draw(vg, x, y, width, height, style, ctx);
}

} // namespace ui
