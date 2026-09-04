#include "DashboardSummaryView.hpp"
#include "SpeedSparklineView.hpp"
#include "../GameDetailView.hpp"
#include "../../utils/switch_utils.h"
#include "../../utils/app_paths.h"
#include "../../config/config.h"
#include "../../net/image_downloader.h"
#include <cstdio>
#include <algorithm>

using namespace brls::literals;

extern std::vector<Game> g_games;

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

// -------------------------------------------------------------
// Interactive Game Card for Dashboard (Clickable & Navigable)
// -------------------------------------------------------------
class DashboardGameCard : public brls::Box {
public:
    DashboardGameCard(const Game& game, std::shared_ptr<bool> imageToken, std::function<void()> on_back = nullptr)
        : game_(game), imageToken_(imageToken), on_back_(on_back) {
        
        this->setFocusable(true);
        this->setHideHighlight(true);
        this->setAxis(brls::Axis::ROW);
        this->setWidth(272.0f);
        this->setHeight(124.0f);
        this->setPadding(8.0f);
        this->setCornerRadius(10.0f);

        // Left: Front Cover Artwork
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

        if (!game.cover.empty()) {
            setImageFromHTTPS(
                coverImg,
                game.cover,
                imageToken_,
                "romfs:/img/borealis_96.png"
            );
        } else {
            coverImg->setImageFromFile("romfs:/img/borealis_96.png");
        }
        imgBox->addView(coverImg);
        this->addView(imgBox);

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

        titleLbl_ = new brls::Label();
        titleLbl_->setText(truncateStr(game.title, 18));
        titleLbl_->setFontSize(13.0f);
        titleLbl_->setTextColor(nvgRGBA(255, 255, 255, 255));
        titleLbl_->setSingleLine(true);
        topDetails->addView(titleLbl_);

        // Metadata string (Size • Year) in Emerald
        std::string metaText = game.size.empty() ? "NSP" : game.size;
        if (!game.year.empty()) metaText += "  •  " + game.year;

        brls::Label* gMeta = new brls::Label();
        gMeta->setText(metaText);
        gMeta->setFontSize(11.5f);
        gMeta->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
        gMeta->setMarginTop(4.0f);
        gMeta->setSingleLine(true);
        topDetails->addView(gMeta);

        if (!game.genre.empty()) {
            brls::Label* gGenre = new brls::Label();
            gGenre->setText(truncateStr(game.genre, 16));
            gGenre->setFontSize(10.5f);
            gGenre->setTextColor(nvgRGBA(130, 155, 185, 180));
            gGenre->setMarginTop(3.0f);
            gGenre->setSingleLine(true);
            topDetails->addView(gGenre);
        }
        infoCol->addView(topDetails);

        // Bottom Action Pill in Emerald
        actPill_ = new brls::Box();
        actPill_->setWidthPercentage(100.0f);
        actPill_->setHeight(22.0f);
        actPill_->setCornerRadius(5.0f);
        actPill_->setBackgroundColor(nvgRGBA(0, 224, 165, 28)); // Emerald tint
        actPill_->setAlignItems(brls::AlignItems::CENTER);
        actPill_->setJustifyContent(brls::JustifyContent::CENTER);

        actLbl_ = new brls::Label();
        actLbl_->setText("Загрузить");
        actLbl_->setFontSize(11.5f);
        actLbl_->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald text
        actPill_->addView(actLbl_);
        infoCol->addView(actPill_);

        this->addView(infoCol);

        // Register action and tap gesture
        this->registerAction("hints/ok"_i18n, brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
            triggerOpen();
            return true;
        });

        if (on_back_) {
            this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
                if (on_back_) {
                    on_back_();
                    return true;
                }
                return false;
            });
        }

        this->addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound* sound) {
            if (status.state == brls::GestureState::END) {
                triggerOpen();
            }
        }));
    }

    void triggerOpen() {
        brls::Application::pushActivity(new ui::GameDetailView(game_));
    }

    void onFocusGained() override {
        Box::onFocusGained();
        if (titleLbl_) titleLbl_->setTextColor(nvgRGBA(0, 245, 195, 255));
        if (actPill_) actPill_->setBackgroundColor(nvgRGBA(0, 224, 165, 80));
        if (actLbl_) actLbl_->setTextColor(nvgRGBA(255, 255, 255, 255));
    }

    void onFocusLost() override {
        Box::onFocusLost();
        if (titleLbl_) titleLbl_->setTextColor(nvgRGBA(255, 255, 255, 255));
        if (actPill_) actPill_->setBackgroundColor(nvgRGBA(0, 224, 165, 28));
        if (actLbl_) actLbl_->setTextColor(nvgRGBA(0, 230, 175, 255));
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        float targetScale = isFocused() ? 1.04f : 1.0f;
        scale_ += (targetScale - scale_) * 0.25f;
        float targetGlow = isFocused() ? 1.0f : 0.0f;
        glow_ += (targetGlow - glow_) * 0.25f;

        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;

        nvgSave(vg);
        nvgTranslate(vg, cx, cy);
        nvgScale(vg, scale_, scale_);
        nvgTranslate(vg, -cx, -cy);

        // 1. Focused Outer Glow
        if (glow_ > 0.01f) {
            NVGpaint glowPaint = nvgBoxGradient(vg, x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f,
                                                10.0f, 6.0f,
                                                nvgRGBA(0, 224, 165, static_cast<unsigned char>(90.0f * glow_)),
                                                nvgRGBA(0, 224, 165, 0));
            nvgBeginPath(vg);
            nvgRect(vg, x - 10.0f, y - 10.0f, width + 20.0f, height + 20.0f);
            nvgFillPaint(vg, glowPaint);
            nvgFill(vg);
        }

        // 2. Base Background
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 10.0f);
        NVGcolor bgTop = isFocused() ? nvgRGBA(0, 180, 140, 65) : nvgRGBA(25, 45, 70, 85);
        NVGcolor bgBot = isFocused() ? nvgRGBA(12, 32, 48, 120) : nvgRGBA(15, 28, 45, 95);
        NVGpaint bgPaint = nvgLinearGradient(vg, x, y, x, y + height, bgTop, bgBot);
        nvgFillPaint(vg, bgPaint);
        nvgFill(vg);

        // 3. Top Sheen
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 1.0f, y + 1.0f, width - 2.0f, height * 0.45f, 9.0f);
        NVGpaint glossPaint = nvgLinearGradient(
            vg, x, y, x, y + height * 0.45f,
            nvgRGBA(255, 255, 255, static_cast<unsigned char>(isFocused() ? 45 : 25)),
            nvgRGBA(255, 255, 255, 0)
        );
        nvgFillPaint(vg, glossPaint);
        nvgFill(vg);

        // 4. Border Stroke
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 10.0f);
        if (glow_ > 0.01f) {
            nvgStrokeColor(vg, nvgRGBA(0, 240, 185, static_cast<unsigned char>(255.0f * glow_)));
            nvgStrokeWidth(vg, 2.0f);
        } else {
            nvgStrokeColor(vg, nvgRGBA(160, 200, 220, 60));
            nvgStrokeWidth(vg, 1.0f);
        }
        nvgStroke(vg);

        nvgRestore(vg);

        // 5. Draw children views
        Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    Game game_;
    std::shared_ptr<bool> imageToken_;
    brls::Label* titleLbl_ = nullptr;
    brls::Box* actPill_ = nullptr;
    brls::Label* actLbl_ = nullptr;
    std::function<void()> on_back_;
    float scale_ = 1.0f;
    float glow_ = 0.0f;
};

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
    }
}

void DashboardSummaryView::setFocusedIndex(int index) {
    if (active_index_ == index) return;
    active_index_ = index;
    rebuildContent();
}

void DashboardSummaryView::updateDownloads(const std::vector<download::DownloadItem>& items) {
    const download::DownloadItem* activeItem = nullptr;
    for (const auto& it : items) {
        if (it.state == download::DownloadState::Downloading || 
            it.state == download::DownloadState::StreamPreparing ||
            it.state == download::DownloadState::StreamInstalling ||
            it.state == download::DownloadState::Installing) {
            activeItem = &it;
            break;
        }
    }

    if (activeItem) {
        speed_history_.push_back(activeItem->download_speed_kbps * 1024.0f);
        while (speed_history_.size() > 45) {
            speed_history_.pop_front();
        }
    } else {
        speed_history_.clear();
    }

    cached_downloads_ = items;

    if (active_index_ == 3) {
        bool hasActive = (activeItem != nullptr);
        if (hasActive) {
            if (dl_active_mode_ && dl_coverImg_ != nullptr &&
                activeItem->topic_id == dl_active_topic_id_ && activeItem->title == dl_active_title_) {

                // In-place dynamic updates without tearing down the UI hierarchy or cancelling imageToken_
                if (dl_titleLbl_) dl_titleLbl_->setText(truncateStr(cleanTitle(activeItem->title), 34));

                std::string stText = (activeItem->state == download::DownloadState::Installing || activeItem->state == download::DownloadState::StreamInstalling)
                                     ? "Установка..." : "Загрузка...";
                if (dl_stLbl_) dl_stLbl_->setText(stText);

                if (dl_barFill_) dl_barFill_->setWidthPercentage(std::max(2.0f, activeItem->progress * 100.0f));

                char pctBuf[16];
                std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", activeItem->progress * 100.0f);
                if (dl_pctLbl_) dl_pctLbl_->setText(pctBuf);

                char spdBuf[32];
                std::snprintf(spdBuf, sizeof(spdBuf), "↓ %.1f MB/s", activeItem->download_speed_kbps / 1024.0f);
                if (dl_spdLbl_) dl_spdLbl_->setText(spdBuf);

                unsigned long long inst_written = activeItem->install_written;
                unsigned long long inst_total = activeItem->install_total;
                if (inst_total == 0 && activeItem->hybrid_installer) {
                    inst_total = activeItem->hybrid_installer->totalBytes();
                }
                std::string szStr = formatBytes(inst_written) + " / " + formatBytes(inst_total);
                if (dl_szLbl_) dl_szLbl_->setText(szStr);

                std::string peersStr = "Пиры: " + std::to_string(activeItem->peers) + " / Сиды: " + std::to_string(activeItem->seeds);
                if (dl_peersLbl_) dl_peersLbl_->setText(peersStr);

                std::string etaStr = "В процессе";
                if (activeItem->download_speed_kbps > 10.0f && inst_total > inst_written) {
                    unsigned long long remBytes = inst_total - inst_written;
                    unsigned long long rate = static_cast<unsigned long long>(activeItem->download_speed_kbps * 1024.0f);
                    unsigned long long sec = remBytes / rate;
                    char etaBuf[32];
                    std::snprintf(etaBuf, sizeof(etaBuf), "~%llu мин", (sec / 60) + 1);
                    etaStr = std::string(etaBuf);
                }
                if (dl_etaLbl_) dl_etaLbl_->setText("Осталось: " + etaStr);

                if (dl_qCountLbl_) dl_qCountLbl_->setText("В очереди: " + std::to_string(items.size()));
                if (dl_sparkline_) dl_sparkline_->setSamples(speed_history_);

                // If cover URL was not resolved initially (e.g. catalog loaded asynchronously), try to resolve and set it now
                if (dl_loaded_cover_url_.empty()) {
                    std::string coverUrl = findCoverForDownload(*activeItem, catalog_sample_);
                    if (!coverUrl.empty()) {
                        dl_loaded_cover_url_ = coverUrl;
                        setImageFromHTTPS(
                            dl_coverImg_,
                            coverUrl,
                            imageToken_,
                            "romfs:/img/borealis_96.png",
                            false,
                            "",
                            -1,
                            -1,
                            1000000
                        );
                    }
                }
            } else {
                // Mode changed to active or active download item changed
                rebuildContent();
            }
        } else {
            // Idle mode (no active downloads)
            if (dl_active_mode_) {
                // Transitioned from active to idle: rebuild once
                rebuildContent();
            }
            // If already in idle mode (!dl_active_mode_), DO NOT rebuild every second!
        }
    }
}

void DashboardSummaryView::setCatalogSample(const std::vector<Game>& games) {
    size_t targetCount = std::min<size_t>(games.size(), 4);
    bool changed = false;
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
    if (active_index_ == 0 || (active_index_ == 3 && !dl_active_mode_)) {
        rebuildContent();
    } else if (active_index_ == 3 && dl_active_mode_ && dl_loaded_cover_url_.empty() && dl_coverImg_) {
        // Try to resolve cover now that catalog sample is available
        const download::DownloadItem* activeItem = nullptr;
        for (const auto& it : cached_downloads_) {
            if (it.state == download::DownloadState::Downloading || 
                it.state == download::DownloadState::StreamPreparing ||
                it.state == download::DownloadState::StreamInstalling ||
                it.state == download::DownloadState::Installing) {
                activeItem = &it;
                break;
            }
        }
        if (activeItem) {
            std::string coverUrl = findCoverForDownload(*activeItem, catalog_sample_);
            if (!coverUrl.empty()) {
                dl_loaded_cover_url_ = coverUrl;
                setImageFromHTTPS(
                    dl_coverImg_,
                    coverUrl,
                    imageToken_,
                    "romfs:/img/borealis_96.png",
                    false,
                    "",
                    -1,
                    -1,
                    1000000
                );
            }
        }
    }
}

void DashboardSummaryView::setRemoteInfo(const std::string& ip_str, int port) {
    if (local_ip_ == ip_str && remote_port_ == port) return;
    local_ip_ = ip_str;
    remote_port_ = port;
    if (active_index_ == 1) {
        rebuildContent();
    }
}

void DashboardSummaryView::setLibraryStats(int installed_count, int updates_count) {
    if (installed_count_ == installed_count && updates_count_ == updates_count) return;
    installed_count_ = installed_count;
    updates_count_ = updates_count;
    if (active_index_ == 2) {
        rebuildContent();
    }
}

void DashboardSummaryView::setSettingsStats(const std::string& engine_mode, uint64_t cache_size_bytes, uint64_t leftover_size_bytes) {
    if (engine_mode_ == engine_mode && cache_size_bytes_ == cache_size_bytes && leftover_size_bytes_ == leftover_size_bytes) return;
    engine_mode_ = engine_mode;
    cache_size_bytes_ = cache_size_bytes;
    leftover_size_bytes_ = leftover_size_bytes;
    if (active_index_ == 4) {
        rebuildContent();
    }
}

void DashboardSummaryView::rebuildContent() {
    dl_active_mode_ = false;
    dl_active_topic_id_.clear();
    dl_active_title_.clear();
    dl_loaded_cover_url_.clear();
    dl_coverImg_ = nullptr;
    dl_titleLbl_ = nullptr;
    dl_stLbl_ = nullptr;
    dl_barFill_ = nullptr;
    dl_pctLbl_ = nullptr;
    dl_spdLbl_ = nullptr;
    dl_szLbl_ = nullptr;
    dl_peersLbl_ = nullptr;
    dl_etaLbl_ = nullptr;
    dl_qCountLbl_ = nullptr;
    dl_sparkline_ = nullptr;

    if (imageToken_) {
        *imageToken_ = false;
        imageToken_.reset();
    }
    imageToken_ = std::make_shared<bool>(true);

    if (!content_container_) return;

    brls::View* currentFocus = brls::Application::getCurrentFocus();
    bool focusInSummary = false;
    for (brls::View* v = currentFocus; v != nullptr; v = v->getParent()) {
        if (v == this || v == content_container_) {
            focusInSummary = true;
            break;
        }
    }

    if (focusInSummary && on_defocus_) {
        on_defocus_();
    }

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
            DashboardGameCard* card = new DashboardGameCard(g, imageToken_, on_defocus_);
            if (get_active_tile_) {
                brls::View* tile = get_active_tile_();
                if (tile) card->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
            }
            cardsRow->addView(card);
        }
    }
    content_container_->addView(cardsRow);
}

// -------------------------------------------------------------
// SECTION 1: REMOTE ADD (QR / Web) - Enhanced Fonts & Proportions
// -------------------------------------------------------------
void DashboardSummaryView::buildRemoteAddSection() {
    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);

    brls::Label* title = new brls::Label();
    title->setText("БЕСПРОВОДНОЕ ДОБАВЛЕНИЕ ТОРРЕНТОВ И MAGNET-ССЫЛОК");
    title->setFontSize(13.0f);
    title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
    headerRow->addView(title);

    brls::Label* hint = new brls::Label();
    hint->setText("Нажмите (A) для полного QR-кода на экране");
    hint->setFontSize(11.5f);
    hint->setTextColor(nvgRGBA(150, 175, 205, 200));
    headerRow->addView(hint);
    content_container_->addView(headerRow);

    brls::Box* bodyRow = new brls::Box();
    bodyRow->setAxis(brls::Axis::ROW);
    bodyRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    bodyRow->setWidthPercentage(100.0f);

    // Left URL Box (Translucent glass with prominent large URL)
    brls::Box* urlBox = new brls::Box();
    urlBox->setAxis(brls::Axis::COLUMN);
    urlBox->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    urlBox->setWidth(445.0f);
    urlBox->setHeight(124.0f);
    urlBox->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    urlBox->setCornerRadius(10.0f);
    urlBox->setBackgroundColor(nvgRGBA(25, 45, 70, 85));

    brls::Label* urlHeader = new brls::Label();
    urlHeader->setText("АДРЕС ВЕБ-ИНТЕРФЕЙСА В ЛОКАЛЬНОЙ СЕТИ:");
    urlHeader->setFontSize(12.0f);
    urlHeader->setTextColor(nvgRGBA(160, 185, 215, 220));
    urlBox->addView(urlHeader);

    brls::Label* urlLabel = new brls::Label();
    urlLabel->setText("http://" + local_ip_ + ":" + std::to_string(remote_port_) + "/");
    urlLabel->setFontSize(21.0f);
    urlLabel->setTextColor(nvgRGBA(0, 235, 180, 255)); // Emerald High-Visibility
    urlBox->addView(urlLabel);

    brls::Box* badgesRow = new brls::Box();
    badgesRow->setAxis(brls::Axis::ROW);
    badgesRow->setAlignItems(brls::AlignItems::CENTER);

    brls::Box* b1 = new brls::Box();
    b1->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
    b1->setCornerRadius(4.0f);
    b1->setBackgroundColor(nvgRGBA(0, 224, 165, 32));
    b1->setMarginRight(8.0f);
    brls::Label* l1 = new brls::Label();
    l1->setText("Порт 8080 (HTTP)");
    l1->setFontSize(11.5f);
    l1->setTextColor(nvgRGBA(0, 230, 175, 255));
    b1->addView(l1);
    badgesRow->addView(b1);

    brls::Box* b2 = new brls::Box();
    b2->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
    b2->setCornerRadius(4.0f);
    b2->setBackgroundColor(nvgRGBA(255, 255, 255, 18));
    brls::Label* l2 = new brls::Label();
    l2->setText("Wi-Fi / Ethernet");
    l2->setFontSize(11.5f);
    l2->setTextColor(nvgRGBA(200, 220, 245, 230));
    b2->addView(l2);
    badgesRow->addView(b2);

    urlBox->addView(badgesRow);
    bodyRow->addView(urlBox);

    // Right Steps Box (Translucent glass with clear readable steps)
    brls::Box* stepsBox = new brls::Box();
    stepsBox->setAxis(brls::Axis::COLUMN);
    stepsBox->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    stepsBox->setWidth(670.0f);
    stepsBox->setHeight(124.0f);
    stepsBox->setPadding(10.0f, 16.0f, 10.0f, 16.0f);
    stepsBox->setCornerRadius(10.0f);
    stepsBox->setBackgroundColor(nvgRGBA(25, 45, 70, 70));

    brls::Label* stepsHeader = new brls::Label();
    stepsHeader->setText("КАК ПЕРЕДАТЬ ФАЙЛЫ НА КОНСОЛЬ:");
    stepsHeader->setFontSize(12.0f);
    stepsHeader->setTextColor(nvgRGBA(160, 185, 215, 220));
    stepsBox->addView(stepsHeader);

    struct StepInfo {
        std::string num;
        std::string text;
        bool highlight;
    };
    StepInfo steps[3] = {
        {"1", "Откройте указанный адрес в любом браузере на телефоне или ПК", false},
        {"2", "Вставьте Magnet-ссылку или перетащите .torrent файл раздачи", false},
        {"3", "Выберите нужные файлы, и загрузка начнётся прямо на Switch!", true}
    };

    for (int i = 0; i < 3; ++i) {
        brls::Box* stepRow = new brls::Box();
        stepRow->setAxis(brls::Axis::ROW);
        stepRow->setAlignItems(brls::AlignItems::CENTER);

        brls::Box* numBadge = new brls::Box();
        numBadge->setWidth(22.0f);
        numBadge->setHeight(22.0f);
        numBadge->setCornerRadius(11.0f);
        numBadge->setBackgroundColor(steps[i].highlight ? nvgRGBA(0, 224, 165, 40) : nvgRGBA(255, 255, 255, 22));
        numBadge->setAlignItems(brls::AlignItems::CENTER);
        numBadge->setJustifyContent(brls::JustifyContent::CENTER);
        numBadge->setMarginRight(10.0f);

        brls::Label* numLbl = new brls::Label();
        numLbl->setText(steps[i].num);
        numLbl->setFontSize(12.0f);
        numLbl->setTextColor(steps[i].highlight ? nvgRGBA(0, 235, 180, 255) : nvgRGBA(220, 235, 255, 240));
        numBadge->addView(numLbl);
        stepRow->addView(numBadge);

        brls::Label* sText = new brls::Label();
        sText->setText(steps[i].text);
        sText->setFontSize(13.5f);
        sText->setTextColor(steps[i].highlight ? nvgRGBA(0, 235, 180, 255) : nvgRGBA(225, 238, 255, 235));
        stepRow->addView(sText);

        stepsBox->addView(stepRow);
    }
    bodyRow->addView(stepsBox);

    content_container_->addView(bodyRow);
}

// -------------------------------------------------------------
// SECTION 2: GAME LIBRARY (2 Clean Sections: Installed & Updates)
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

    brls::Label* hint = new brls::Label();
    hint->setText("Нажмите (A) для перехода в библиотеку");
    hint->setFontSize(11.5f);
    hint->setTextColor(nvgRGBA(150, 175, 205, 200));
    headerRow->addView(hint);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    // Section 1: Installed Games
    brls::Box* instCard = new brls::Box();
    instCard->setAxis(brls::Axis::COLUMN);
    instCard->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    instCard->setWidth(555.0f);
    instCard->setHeight(124.0f);
    instCard->setPadding(12.0f, 18.0f, 12.0f, 18.0f);
    instCard->setCornerRadius(10.0f);
    instCard->setBackgroundColor(nvgRGBA(25, 45, 70, 80));

    brls::Box* instTop = new brls::Box();
    instTop->setAxis(brls::Axis::ROW);
    instTop->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    instTop->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* instTitle = new brls::Label();
    instTitle->setText("УСТАНОВЛЕННЫЕ ИГРЫ");
    instTitle->setFontSize(12.0f);
    instTitle->setTextColor(nvgRGBA(160, 185, 215, 220));
    instTop->addView(instTitle);

    brls::Label* instStorage = new brls::Label();
    instStorage->setText("Память SD + NAND");
    instStorage->setFontSize(11.5f);
    instStorage->setTextColor(nvgRGBA(130, 160, 195, 200));
    instTop->addView(instStorage);
    instCard->addView(instTop);

    brls::Box* instMid = new brls::Box();
    instMid->setAxis(brls::Axis::ROW);
    instMid->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* instCountLbl = new brls::Label();
    instCountLbl->setText(std::to_string(installed_count_) + " игр");
    instCountLbl->setFontSize(26.0f);
    instCountLbl->setTextColor(nvgRGBA(255, 255, 255, 255));
    instMid->addView(instCountLbl);

    brls::Box* instBadge = new brls::Box();
    instBadge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
    instBadge->setCornerRadius(4.0f);
    instBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 28));
    instBadge->setMarginLeft(14.0f);
    brls::Label* instBadgeLbl = new brls::Label();
    instBadgeLbl->setText("Готовы к запуску");
    instBadgeLbl->setFontSize(12.0f);
    instBadgeLbl->setTextColor(nvgRGBA(0, 230, 175, 255));
    instBadge->addView(instBadgeLbl);
    instMid->addView(instBadge);
    instCard->addView(instMid);

    brls::Label* instSub = new brls::Label();
    instSub->setText("(A) Управление установленными играми, DLC и удаление");
    instSub->setFontSize(11.5f);
    instSub->setTextColor(nvgRGBA(140, 170, 200, 200));
    instCard->addView(instSub);
    cardsRow->addView(instCard);

    // Section 2: Available Updates
    brls::Box* updCard = new brls::Box();
    updCard->setAxis(brls::Axis::COLUMN);
    updCard->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    updCard->setWidth(555.0f);
    updCard->setHeight(124.0f);
    updCard->setPadding(12.0f, 18.0f, 12.0f, 18.0f);
    updCard->setCornerRadius(10.0f);
    updCard->setBackgroundColor(nvgRGBA(25, 45, 70, 80));

    brls::Box* updTop = new brls::Box();
    updTop->setAxis(brls::Axis::ROW);
    updTop->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    updTop->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* updTitle = new brls::Label();
    updTitle->setText("ДОСТУПНЫЕ ОБНОВЛЕНИЯ");
    updTitle->setFontSize(12.0f);
    updTitle->setTextColor(nvgRGBA(160, 185, 215, 220));
    updTop->addView(updTitle);

    brls::Label* updSync = new brls::Label();
    updSync->setText("Сверка с каталогом");
    updSync->setFontSize(11.5f);
    updSync->setTextColor(nvgRGBA(130, 160, 195, 200));
    updTop->addView(updSync);
    updCard->addView(updTop);

    brls::Box* updMid = new brls::Box();
    updMid->setAxis(brls::Axis::ROW);
    updMid->setAlignItems(brls::AlignItems::CENTER);

    if (updates_count_ > 0) {
        brls::Label* updCountLbl = new brls::Label();
        updCountLbl->setText(std::to_string(updates_count_) + " обновлений");
        updCountLbl->setFontSize(26.0f);
        updCountLbl->setTextColor(nvgRGBA(255, 185, 70, 255)); // Amber
        updMid->addView(updCountLbl);

        brls::Box* updBadge = new brls::Box();
        updBadge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
        updBadge->setCornerRadius(4.0f);
        updBadge->setBackgroundColor(nvgRGBA(255, 180, 50, 32));
        updBadge->setMarginLeft(14.0f);
        brls::Label* updBadgeLbl = new brls::Label();
        updBadgeLbl->setText("Доступны новые патчи");
        updBadgeLbl->setFontSize(12.0f);
        updBadgeLbl->setTextColor(nvgRGBA(255, 200, 80, 255));
        updBadge->addView(updBadgeLbl);
        updMid->addView(updBadge);
    } else {
        brls::Label* updCountLbl = new brls::Label();
        updCountLbl->setText("Все игры обновлены");
        updCountLbl->setFontSize(22.0f);
        updCountLbl->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald
        updMid->addView(updCountLbl);

        brls::Box* updBadge = new brls::Box();
        updBadge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
        updBadge->setCornerRadius(4.0f);
        updBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 28));
        updBadge->setMarginLeft(14.0f);
        brls::Label* updBadgeLbl = new brls::Label();
        updBadgeLbl->setText("Все версии актуальны");
        updBadgeLbl->setFontSize(12.0f);
        updBadgeLbl->setTextColor(nvgRGBA(0, 230, 175, 255));
        updBadge->addView(updBadgeLbl);
        updMid->addView(updBadge);
    }
    updCard->addView(updMid);

    brls::Label* updSub = new brls::Label();
    updSub->setText("(A) Просмотр списка обновлений и быстрая загрузка");
    updSub->setFontSize(11.5f);
    updSub->setTextColor(nvgRGBA(140, 170, 200, 200));
    updCard->addView(updSub);

    cardsRow->addView(updCard);
    content_container_->addView(cardsRow);
}

// -------------------------------------------------------------
// SECTION 3: DOWNLOADS (Active Download with Metrics & Sparkline OR Idle Recommendations)
// -------------------------------------------------------------
void DashboardSummaryView::buildDownloadsSection() {
    const download::DownloadItem* activeItem = nullptr;
    for (const auto& it : cached_downloads_) {
        if (it.state == download::DownloadState::Downloading ||
            it.state == download::DownloadState::StreamPreparing ||
            it.state == download::DownloadState::StreamInstalling ||
            it.state == download::DownloadState::Installing) {
            activeItem = &it;
            break;
        }
    }

    if (activeItem) {
        dl_active_mode_ = true;
        dl_active_topic_id_ = activeItem->topic_id;
        dl_active_title_ = activeItem->title;

        // Active Download Display
        brls::Box* headerRow = new brls::Box();
        headerRow->setAxis(brls::Axis::ROW);
        headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        headerRow->setAlignItems(brls::AlignItems::CENTER);
        headerRow->setMarginBottom(6.0f);

        brls::Label* title = new brls::Label();
        title->setText("АКТИВНАЯ ЗАГРУЗКА И УСТАНОВКА");
        title->setFontSize(13.0f);
        title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
        headerRow->addView(title);

        brls::Label* hint = new brls::Label();
        hint->setText("Нажмите (A) для управления очередью загрузок");
        hint->setFontSize(11.5f);
        hint->setTextColor(nvgRGBA(150, 175, 205, 200));
        headerRow->addView(hint);
        content_container_->addView(headerRow);

        brls::Box* bodyRow = new brls::Box();
        bodyRow->setAxis(brls::Axis::ROW);
        bodyRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        bodyRow->setWidthPercentage(100.0f);

        // Left Card: Cover + Progress + Metrics
        brls::Box* mainCard = new brls::Box();
        mainCard->setAxis(brls::Axis::ROW);
        mainCard->setAlignItems(brls::AlignItems::CENTER);
        mainCard->setWidth(755.0f);
        mainCard->setHeight(124.0f);
        mainCard->setPadding(8.0f, 14.0f, 8.0f, 14.0f);
        mainCard->setCornerRadius(10.0f);
        mainCard->setBackgroundColor(nvgRGBA(25, 45, 70, 85));

        // Cover Box
        brls::Box* coverBox = new brls::Box();
        coverBox->setWidth(68.0f);
        coverBox->setHeight(108.0f);
        coverBox->setCornerRadius(6.0f);
        coverBox->setBackgroundColor(nvgRGBA(15, 25, 38, 140));
        coverBox->setAlignItems(brls::AlignItems::CENTER);
        coverBox->setJustifyContent(brls::JustifyContent::CENTER);
        coverBox->setMarginRight(14.0f);

        dl_coverImg_ = new brls::Image();
        dl_coverImg_->setWidth(68.0f);
        dl_coverImg_->setHeight(108.0f);
        dl_coverImg_->setCornerRadius(6.0f);
        dl_coverImg_->setScalingType(brls::ImageScalingType::FILL);

        std::string coverUrl = findCoverForDownload(*activeItem, catalog_sample_);
        dl_loaded_cover_url_ = coverUrl;

        if (!coverUrl.empty()) {
            setImageFromHTTPS(
                dl_coverImg_,
                coverUrl,
                imageToken_,
                "romfs:/img/borealis_96.png",
                false,
                "",
                -1,
                -1,
                1000000
            );
        } else {
            dl_coverImg_->setImageFromFile("romfs:/img/borealis_96.png");
        }
        coverBox->addView(dl_coverImg_);
        mainCard->addView(coverBox);

        // Details column
        brls::Box* detailsCol = new brls::Box();
        detailsCol->setAxis(brls::Axis::COLUMN);
        detailsCol->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        detailsCol->setGrow(1.0f);
        detailsCol->setHeightPercentage(100.0f);

        // Title + status badge
        brls::Box* topRow = new brls::Box();
        topRow->setAxis(brls::Axis::ROW);
        topRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        topRow->setAlignItems(brls::AlignItems::CENTER);

        dl_titleLbl_ = new brls::Label();
        dl_titleLbl_->setText(truncateStr(cleanTitle(activeItem->title), 34));
        dl_titleLbl_->setFontSize(15.0f);
        dl_titleLbl_->setTextColor(nvgRGBA(255, 255, 255, 255));
        dl_titleLbl_->setSingleLine(true);
        topRow->addView(dl_titleLbl_);

        std::string stText = (activeItem->state == download::DownloadState::Installing || activeItem->state == download::DownloadState::StreamInstalling)
                             ? "Установка..." : "Загрузка...";
        dl_stLbl_ = new brls::Label();
        dl_stLbl_->setText(stText);
        dl_stLbl_->setFontSize(12.0f);
        dl_stLbl_->setTextColor(nvgRGBA(0, 230, 175, 255));
        topRow->addView(dl_stLbl_);
        detailsCol->addView(topRow);

        // Progress Bar with Percentage
        brls::Box* barRow = new brls::Box();
        barRow->setAxis(brls::Axis::ROW);
        barRow->setAlignItems(brls::AlignItems::CENTER);

        brls::Box* barBg = new brls::Box();
        barBg->setGrow(1.0f);
        barBg->setHeight(6.0f);
        barBg->setCornerRadius(3.0f);
        barBg->setBackgroundColor(nvgRGBA(18, 32, 50, 180));
        barBg->setMarginRight(10.0f);

        dl_barFill_ = new brls::Box();
        dl_barFill_->setWidthPercentage(std::max(2.0f, activeItem->progress * 100.0f));
        dl_barFill_->setHeight(6.0f);
        dl_barFill_->setCornerRadius(3.0f);
        dl_barFill_->setBackgroundColor(nvgRGBA(0, 224, 165, 255));
        barBg->addView(dl_barFill_);
        barRow->addView(barBg);

        char pctBuf[16];
        std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", activeItem->progress * 100.0f);
        dl_pctLbl_ = new brls::Label();
        dl_pctLbl_->setText(pctBuf);
        dl_pctLbl_->setFontSize(13.0f);
        dl_pctLbl_->setTextColor(nvgRGBA(0, 230, 175, 255));
        barRow->addView(dl_pctLbl_);
        detailsCol->addView(barRow);

        // Metrics row
        brls::Box* metricsRow = new brls::Box();
        metricsRow->setAxis(brls::Axis::ROW);
        metricsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);

        char spdBuf[32];
        std::snprintf(spdBuf, sizeof(spdBuf), "↓ %.1f MB/s", activeItem->download_speed_kbps / 1024.0f);
        dl_spdLbl_ = new brls::Label();
        dl_spdLbl_->setText(spdBuf);
        dl_spdLbl_->setFontSize(12.0f);
        dl_spdLbl_->setTextColor(nvgRGBA(0, 230, 175, 255));
        metricsRow->addView(dl_spdLbl_);

        unsigned long long inst_written = activeItem->install_written;
        unsigned long long inst_total = activeItem->install_total;
        if (inst_total == 0 && activeItem->hybrid_installer) {
            inst_total = activeItem->hybrid_installer->totalBytes();
        }

        std::string szStr = formatBytes(inst_written) + " / " + formatBytes(inst_total);
        dl_szLbl_ = new brls::Label();
        dl_szLbl_->setText(szStr);
        dl_szLbl_->setFontSize(11.5f);
        dl_szLbl_->setTextColor(nvgRGBA(180, 205, 230, 220));
        metricsRow->addView(dl_szLbl_);

        std::string peersStr = "Пиры: " + std::to_string(activeItem->peers) + " / Сиды: " + std::to_string(activeItem->seeds);
        dl_peersLbl_ = new brls::Label();
        dl_peersLbl_->setText(peersStr);
        dl_peersLbl_->setFontSize(11.5f);
        dl_peersLbl_->setTextColor(nvgRGBA(150, 175, 205, 200));
        metricsRow->addView(dl_peersLbl_);

        std::string etaStr = "В процессе";
        if (activeItem->download_speed_kbps > 10.0f && inst_total > inst_written) {
            unsigned long long remBytes = inst_total - inst_written;
            unsigned long long rate = static_cast<unsigned long long>(activeItem->download_speed_kbps * 1024.0f);
            unsigned long long sec = remBytes / rate;
            char etaBuf[32];
            std::snprintf(etaBuf, sizeof(etaBuf), "~%llu мин", (sec / 60) + 1);
            etaStr = std::string(etaBuf);
        }
        dl_etaLbl_ = new brls::Label();
        dl_etaLbl_->setText("Осталось: " + etaStr);
        dl_etaLbl_->setFontSize(11.5f);
        dl_etaLbl_->setTextColor(nvgRGBA(150, 175, 205, 200));
        metricsRow->addView(dl_etaLbl_);

        detailsCol->addView(metricsRow);
        mainCard->addView(detailsCol);
        bodyRow->addView(mainCard);

        // Right Card: Sparkline Speed Graph
        brls::Box* graphCard = new brls::Box();
        graphCard->setAxis(brls::Axis::COLUMN);
        graphCard->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        graphCard->setWidth(360.0f);
        graphCard->setHeight(124.0f);
        graphCard->setPadding(8.0f, 12.0f, 8.0f, 12.0f);
        graphCard->setCornerRadius(10.0f);
        graphCard->setBackgroundColor(nvgRGBA(25, 45, 70, 75));

        brls::Box* gHeader = new brls::Box();
        gHeader->setAxis(brls::Axis::ROW);
        gHeader->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);

        brls::Label* gLbl = new brls::Label();
        gLbl->setText("ГРАФИК СКОРОСТИ");
        gLbl->setFontSize(11.5f);
        gLbl->setTextColor(nvgRGBA(160, 185, 215, 220));
        gHeader->addView(gLbl);

        dl_qCountLbl_ = new brls::Label();
        dl_qCountLbl_->setText("В очереди: " + std::to_string(cached_downloads_.size()));
        dl_qCountLbl_->setFontSize(11.5f);
        dl_qCountLbl_->setTextColor(nvgRGBA(0, 230, 175, 255));
        gHeader->addView(dl_qCountLbl_);
        graphCard->addView(gHeader);

        dl_sparkline_ = new SpeedSparklineView();
        dl_sparkline_->setWidthPercentage(100.0f);
        dl_sparkline_->setHeight(56.0f);
        dl_sparkline_->setSamples(speed_history_);
        graphCard->addView(dl_sparkline_);

        brls::Label* gFooter = new brls::Label();
        gFooter->setText("Прямая запись на носитель NAND / SD");
        gFooter->setFontSize(11.0f);
        gFooter->setTextColor(nvgRGBA(130, 160, 190, 200));
        graphCard->addView(gFooter);

        bodyRow->addView(graphCard);
        content_container_->addView(bodyRow);
    } else {
        dl_active_mode_ = false;
        // Idle State: Recommendations from Catalog / Top 100
        brls::Box* headerRow = new brls::Box();
        headerRow->setAxis(brls::Axis::ROW);
        headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        headerRow->setAlignItems(brls::AlignItems::CENTER);
        headerRow->setMarginBottom(6.0f);

        brls::Label* title = new brls::Label();
        title->setText("ОЧЕРЕДЬ ЗАГРУЗОК ПУСТА  •  НЕТ АКТИВНЫХ ЗАДАЧ");
        title->setFontSize(13.0f);
        title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
        headerRow->addView(title);

        brls::Label* hint = new brls::Label();
        hint->setText("Рекомендуемые игры для загрузки из «Топ 100»:");
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
                DashboardGameCard* card = new DashboardGameCard(g, imageToken_, on_defocus_);
                if (get_active_tile_) {
                    brls::View* tile = get_active_tile_();
                    if (tile) card->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
                }
                cardsRow->addView(card);
            }
        }
        content_container_->addView(cardsRow);
    }
}

// -------------------------------------------------------------
// SECTION 4: TOOLS & SYSTEM PARAMETERS
// -------------------------------------------------------------
void DashboardSummaryView::buildToolsSection() {
    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);

    brls::Label* title = new brls::Label();
    title->setText("СИСТЕМНАЯ ИНФОРМАЦИЯ И НАСТРОЙКИ ХРАНИЛИЩА");
    title->setFontSize(13.0f);
    title->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
    headerRow->addView(title);

    brls::Label* hint = new brls::Label();
    hint->setText("Нажмите (A) для открытия подробных настроек");
    hint->setFontSize(11.5f);
    hint->setTextColor(nvgRGBA(150, 175, 205, 200));
    headerRow->addView(hint);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    // Card 1: Torrent Engine Mode
    brls::Box* c1 = new brls::Box();
    c1->setAxis(brls::Axis::COLUMN);
    c1->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c1->setWidth(360.0f);
    c1->setHeight(124.0f);
    c1->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    c1->setCornerRadius(10.0f);
    c1->setBackgroundColor(nvgRGBA(25, 45, 70, 80));

    brls::Label* c1Title = new brls::Label();
    c1Title->setText("РЕЖИМ ЗАГРУЗКИ");
    c1Title->setFontSize(11.5f);
    c1Title->setTextColor(nvgRGBA(160, 185, 215, 220));
    c1->addView(c1Title);

    brls::Label* c1Val = new brls::Label();
    c1Val->setText(engine_mode_);
    c1Val->setFontSize(21.0f);
    c1Val->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald
    c1->addView(c1Val);

    brls::Label* c1Sub = new brls::Label();
    c1Sub->setText("Порт: 6881 (DHT)  •  Keep-Awake");
    c1Sub->setFontSize(11.0f);
    c1Sub->setTextColor(nvgRGBA(130, 160, 190, 200));
    c1->addView(c1Sub);
    cardsRow->addView(c1);

    // Card 2: Total Application Cache
    brls::Box* c2 = new brls::Box();
    c2->setAxis(brls::Axis::COLUMN);
    c2->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c2->setWidth(360.0f);
    c2->setHeight(124.0f);
    c2->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    c2->setCornerRadius(10.0f);
    c2->setBackgroundColor(nvgRGBA(25, 45, 70, 80));

    brls::Label* c2Title = new brls::Label();
    c2Title->setText("ОБЩИЙ КЭШ ПРИЛОЖЕНИЯ");
    c2Title->setFontSize(11.5f);
    c2Title->setTextColor(nvgRGBA(160, 185, 215, 220));
    c2->addView(c2Title);

    brls::Label* c2Val = new brls::Label();
    c2Val->setText(formatBytes(cache_size_bytes_));
    c2Val->setFontSize(21.0f);
    c2Val->setTextColor(nvgRGBA(255, 255, 255, 255));
    c2->addView(c2Val);

    brls::Label* c2Sub = new brls::Label();
    c2Sub->setText("Обложки, торренты, метаданные, DHT");
    c2Sub->setFontSize(11.0f);
    c2Sub->setTextColor(nvgRGBA(130, 160, 190, 200));
    c2->addView(c2Sub);
    cardsRow->addView(c2);

    // Card 3: Unfinished/Leftover Files
    brls::Box* c3 = new brls::Box();
    c3->setAxis(brls::Axis::COLUMN);
    c3->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c3->setWidth(360.0f);
    c3->setHeight(124.0f);
    c3->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    c3->setCornerRadius(10.0f);
    c3->setBackgroundColor(nvgRGBA(25, 45, 70, 80));

    brls::Label* c3Title = new brls::Label();
    c3Title->setText("НЕЗАВЕРШЕННЫЕ УСТАНОВКИ");
    c3Title->setFontSize(11.5f);
    c3Title->setTextColor(nvgRGBA(160, 185, 215, 220));
    c3->addView(c3Title);

    brls::Label* c3Val = new brls::Label();
    if (leftover_size_bytes_ > 0) {
        c3Val->setText(formatBytes(leftover_size_bytes_));
        c3Val->setTextColor(nvgRGBA(255, 185, 70, 255)); // Amber
    } else {
        c3Val->setText("0 B (Чисто)");
        c3Val->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald
    }
    c3Val->setFontSize(21.0f);
    c3->addView(c3Val);

    brls::Label* c3Sub = new brls::Label();
    c3Sub->setText("Плейсхолдеры NCM и временные файлы");
    c3Sub->setFontSize(11.0f);
    c3Sub->setTextColor(nvgRGBA(130, 160, 190, 200));
    c3->addView(c3Sub);
    cardsRow->addView(c3);

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
