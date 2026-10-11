#include "DashboardSummaryView.hpp"
#include "SpeedSparklineView.hpp"
#include "../ThemeManager.hpp"
#include "../GameDetailView.hpp"
#include "../../utils/switch_utils.h"
#include "../../utils/app_paths.h"
#include "../../config/config.h"
#include "../../utils/log.h"
#include "../../net/image_downloader.h"
#include <cstdio>
#include <algorithm>

using namespace brls::literals;

extern std::vector<Game> g_games;

namespace ui {

static inline NVGcolor themeAccent(unsigned char alpha = 255) {
    NVGcolor c = ThemeManager::instance().getTextAccentColor();
    return nvgRGBA(c.r * 255, c.g * 255, c.b * 255, alpha);
}

static inline NVGcolor themeTextPrimary() {
    return ThemeManager::instance().getTextPrimaryColor();
}

static inline NVGcolor themeTextSecondary() {
    return ThemeManager::instance().getTextSecondaryColor();
}

static inline NVGcolor themeDim() {
    return ThemeManager::instance().getAccentDimColor();
}

static inline NVGcolor themeMedium() {
    return ThemeManager::instance().getAccentMediumColor();
}

static inline NVGcolor themeCardBg() {
    return ThemeManager::instance().getCardBgColor();
}

static inline NVGcolor themeCardBorder() {
    return ThemeManager::instance().getCardBorderColor();
}

static inline NVGcolor themeCardTitle() {
    return ThemeManager::instance().getCardTitleColor();
}

static inline NVGcolor themeCardSub() {
    return ThemeManager::instance().getCardSubColor();
}

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
    DashboardGameCard(const Game& game, std::function<void()> on_back = nullptr)
        : game_(game), on_back_(on_back) {
        
        cardImageToken_ = std::make_shared<bool>(true);

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
        imgBox->setBackgroundColor(ThemeManager::instance().isCurrentThemeLight() ? nvgRGBA(210, 225, 240, 180) : nvgRGBA(15, 25, 38, 140));
        imgBox->setAlignItems(brls::AlignItems::CENTER);
        imgBox->setJustifyContent(brls::JustifyContent::CENTER);
        imgBox->setMarginRight(10.0f);
        imgBox->setGrow(0.0f);
        imgBox->setShrink(0.0f);

        brls::Image* coverImg = new brls::Image();
        coverImg->setWidth(68.0f);
        coverImg->setHeight(108.0f);
        coverImg->setCornerRadius(6.0f);
        coverImg->setScalingType(brls::ImageScalingType::FILL);
        coverImg->setGrow(0.0f);
        coverImg->setShrink(0.0f);

        if (!game.cover.empty()) {
            setImageFromHTTPS(
                coverImg,
                game.cover,
                cardImageToken_,
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
        titleLbl_->setFontSize(13.5f);
        titleLbl_->setLineHeight(1.3f);
        titleLbl_->setTextColor(themeTextPrimary());
        titleLbl_->setSingleLine(true);
        topDetails->addView(titleLbl_);

        // Metadata string (Size • Year) in Emerald
        std::string metaText = game.size.empty() ? "NSP" : game.size;
        if (!game.year.empty()) metaText += "  •  " + game.year;

        brls::Label* gMeta = new brls::Label();
        gMeta->setText(metaText);
        gMeta->setFontSize(11.5f);
        gMeta->setLineHeight(1.3f);
        gMeta->setTextColor(themeAccent(240));
        gMeta->setMarginTop(2.0f);
        gMeta->setSingleLine(true);
        topDetails->addView(gMeta);

        if (!game.genre.empty()) {
            brls::Label* gGenre = new brls::Label();
            gGenre->setText(truncateStr(game.genre, 24));
            gGenre->setFontSize(11.5f);
            gGenre->setLineHeight(1.3f);
            gGenre->setTextColor(themeTextSecondary());
            gGenre->setMarginTop(2.0f);
            gGenre->setSingleLine(true);
            topDetails->addView(gGenre);
        }
        infoCol->addView(topDetails);

        // Bottom Action Pill
        actPill_ = new brls::Box();
        actPill_->setWidthPercentage(100.0f);
        actPill_->setHeight(24.0f);
        actPill_->setCornerRadius(6.0f);
        actPill_->setBackgroundColor(themeDim());
        actPill_->setAlignItems(brls::AlignItems::CENTER);
        actPill_->setJustifyContent(brls::JustifyContent::CENTER);

        actLbl_ = new brls::Label();
        actLbl_->setText("app/dashboard/btn_download"_i18n);
        actLbl_->setFontSize(11.5f);
        actLbl_->setTextColor(themeAccent(255));
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
        if (titleLbl_) titleLbl_->setTextColor(themeAccent(255));
        if (actPill_) actPill_->setBackgroundColor(themeAccent(230));
        if (actLbl_) actLbl_->setTextColor(ThemeManager::instance().isCurrentThemeLight() ? nvgRGB(255, 255, 255) : nvgRGB(10, 18, 26));
    }

    void onFocusLost() override {
        Box::onFocusLost();
        if (titleLbl_) titleLbl_->setTextColor(themeTextPrimary());
        if (actPill_) actPill_->setBackgroundColor(themeDim());
        if (actLbl_) actLbl_->setTextColor(themeAccent(255));
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
                                                themeAccent(static_cast<unsigned char>(90.0f * glow_)),
                                                themeAccent(0));
            nvgBeginPath(vg);
            nvgRect(vg, x - 10.0f, y - 10.0f, width + 20.0f, height + 20.0f);
            nvgFillPaint(vg, glowPaint);
            nvgFill(vg);
        }

        // 2. Base Background: Light delicate frosted glass (no heavy opaque slabs)
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 10.0f);
        bool isLight = ThemeManager::instance().isCurrentThemeLight();
        NVGcolor bgTop, bgBot;
        if (isLight) {
            bgTop = isFocused() ? nvgRGBA(255, 255, 255, 255) : nvgRGBA(255, 255, 255, 235);
            bgBot = isFocused() ? nvgRGBA(240, 248, 255, 250) : nvgRGBA(242, 246, 250, 220);
        } else {
            NVGcolor acc = ThemeManager::instance().getAccentColor();
            bgTop = isFocused()
                ? nvgRGBA(acc.r * 255 * 0.85f,
                          acc.g * 255 * 0.85f,
                          acc.b * 255 * 0.85f, 60)
                : nvgRGBA(255, 255, 255, 14);
            bgBot = isFocused()
                ? nvgRGBA(12, 22, 34, 155)
                : nvgRGBA(8, 16, 26, 50);
        }
        NVGpaint bgPaint = nvgLinearGradient(vg, x, y, x, y + height, bgTop, bgBot);
        nvgFillPaint(vg, bgPaint);
        nvgFill(vg);

        // 3. Top Sheen
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 1.0f, y + 1.0f, width - 2.0f, height * 0.45f, 9.0f);
        NVGpaint glossPaint = nvgLinearGradient(
            vg, x, y, x, y + height * 0.45f,
            nvgRGBA(255, 255, 255, static_cast<unsigned char>(isFocused() ? (isLight ? 90 : 38) : (isLight ? 60 : 16))),
            nvgRGBA(255, 255, 255, 0)
        );
        nvgFillPaint(vg, glossPaint);
        nvgFill(vg);

        // 4. Border Stroke
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 10.0f);
        if (glow_ > 0.01f) {
            nvgStrokeColor(vg, themeAccent(static_cast<unsigned char>(255.0f * glow_)));
            nvgStrokeWidth(vg, isLight ? 2.5f : 2.0f);
        } else {
            nvgStrokeColor(vg, themeCardBorder());
            nvgStrokeWidth(vg, 1.0f);
        }
        nvgStroke(vg);

        // 5. Draw children views
        Box::draw(vg, x, y, width, height, style, ctx);

        nvgRestore(vg);
    }

    ~DashboardGameCard() override {
        if (cardImageToken_) {
            *cardImageToken_ = false;
        }
    }

private:
    Game game_;
    std::shared_ptr<bool> cardImageToken_;
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
    this->setHeight(178.0f);
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(10.0f, 18.0f, 10.0f, 18.0f);
    this->setCornerRadius(16.0f);

    content_container_ = new brls::Box();
    content_container_->setAxis(brls::Axis::COLUMN);
    content_container_->setWidthPercentage(100.0f);
    content_container_->setHeightPercentage(100.0f);
    this->addView(content_container_);

    rebuildContent();

    ThemeManager::instance().subscribe([this]() {
        rebuildContent();
    });
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
                                     ? "app/dashboard/downloads_status_installing"_i18n : "app/dashboard/downloads_status_downloading"_i18n;
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

                std::string peersStr = "app/dashboard/downloads_peers"_i18n + std::to_string(activeItem->peers) + "app/dashboard/downloads_seeds"_i18n + std::to_string(activeItem->seeds);
                if (dl_peersLbl_) dl_peersLbl_->setText(peersStr);

                std::string etaStr = "app/dashboard/downloads_in_progress"_i18n;
                if (activeItem->download_speed_kbps > 10.0f && inst_total > inst_written) {
                    unsigned long long remBytes = inst_total - inst_written;
                    unsigned long long rate = static_cast<unsigned long long>(activeItem->download_speed_kbps * 1024.0f);
                    unsigned long long sec = remBytes / rate;
                    etaStr = brls::getStr("app/dashboard/downloads_eta_min", std::to_string((sec / 60) + 1));
                }
                if (dl_etaLbl_) dl_etaLbl_->setText("app/dashboard/downloads_eta_prefix"_i18n + etaStr);

                if (dl_qCountLbl_) dl_qCountLbl_->setText("app/dashboard/downloads_queue_prefix"_i18n + std::to_string(items.size()));
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
        case 1: buildRetroGamesSection(); break;
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
    headerRow->setFocusable(false);

    brls::Label* title = new brls::Label();
    title->setText("app/dashboard/catalog_title"_i18n);
    title->setFontSize(13.0f);
    title->setTextColor(themeAccent(240));
    headerRow->addView(title);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    if (catalog_sample_.empty()) {
        brls::Label* emptyLbl = new brls::Label();
        emptyLbl->setText("app/dashboard/catalog_loading"_i18n);
        emptyLbl->setFontSize(13.0f);
        emptyLbl->setTextColor(themeTextSecondary());
        cardsRow->addView(emptyLbl);
    } else {
        for (const auto& g : catalog_sample_) {
            DashboardGameCard* card = new DashboardGameCard(g, on_defocus_);
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
// -------------------------------------------------------------
// SECTION 1: RETRO GAMES (Consoles & ROMs Overview)
// -------------------------------------------------------------
void DashboardSummaryView::buildRetroGamesSection() {
    auto openRetro = [this](brls::View*) {
        if (on_open_section_) on_open_section_(1);
        return true;
    };

    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);
    headerRow->setFocusable(false);

    brls::Label* title = new brls::Label();
    title->setText("app/dashboard/retro_title"_i18n);
    title->setFontSize(13.0f);
    title->setTextColor(themeAccent(240));
    headerRow->addView(title);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    // Card 1: Retro Library & Total Games
    brls::Box* c1 = new brls::Box();
    c1->setAxis(brls::Axis::COLUMN);
    c1->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c1->setWidth(360.0f);
    c1->setHeight(124.0f);
    c1->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    c1->setCornerRadius(10.0f);
    c1->setBackgroundColor(themeCardBg());
    c1->setBorderThickness(1.0f);
    c1->setBorderColor(themeCardBorder());
    c1->setFocusable(true);
    c1->registerClickAction(openRetro);

    brls::Box* c1Top = new brls::Box();
    c1Top->setAxis(brls::Axis::ROW);
    c1Top->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c1Top->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* c1Title = new brls::Label();
    c1Title->setText("app/dashboard/retro_c1_title"_i18n);
    c1Title->setFontSize(11.5f);
    c1Title->setTextColor(themeCardTitle());
    c1Top->addView(c1Title);

    brls::Label* c1TopTag = new brls::Label();
    c1TopTag->setText("app/dashboard/retro_c1_tag"_i18n);
    c1TopTag->setFontSize(11.5f);
    c1TopTag->setTextColor(themeCardSub());
    c1Top->addView(c1TopTag);
    c1->addView(c1Top);

    brls::Box* c1Mid = new brls::Box();
    c1Mid->setAxis(brls::Axis::ROW);
    c1Mid->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* c1Val = new brls::Label();
    c1Val->setText("app/dashboard/retro_c1_val"_i18n);
    c1Val->setFontSize(22.0f);
    c1Val->setTextColor(themeTextPrimary());
    c1Mid->addView(c1Val);

    brls::Box* c1Badge = new brls::Box();
    c1Badge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
    c1Badge->setCornerRadius(4.0f);
    c1Badge->setBackgroundColor(themeDim());
    c1Badge->setMarginLeft(12.0f);
    brls::Label* c1BadgeLbl = new brls::Label();
    c1BadgeLbl->setText("app/dashboard/retro_c1_badge"_i18n);
    c1BadgeLbl->setFontSize(12.0f);
    c1BadgeLbl->setTextColor(themeAccent(255));
    c1Badge->addView(c1BadgeLbl);
    c1Mid->addView(c1Badge);
    c1->addView(c1Mid);

    brls::Label* c1Sub = new brls::Label();
    c1Sub->setText("NES • SNES • GBA • N64 • PS1 • PS2 • PSP • MD...");
    c1Sub->setFontSize(11.0f);
    c1Sub->setTextColor(themeCardSub());
    c1->addView(c1Sub);
    cardsRow->addView(c1);

    // Card 2: Ecosystems & Platforms
    brls::Box* c2 = new brls::Box();
    c2->setAxis(brls::Axis::COLUMN);
    c2->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c2->setWidth(360.0f);
    c2->setHeight(124.0f);
    c2->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    c2->setCornerRadius(10.0f);
    c2->setBackgroundColor(themeCardBg());
    c2->setBorderThickness(1.0f);
    c2->setBorderColor(themeCardBorder());
    c2->setFocusable(true);
    c2->registerClickAction(openRetro);

    brls::Box* c2Top = new brls::Box();
    c2Top->setAxis(brls::Axis::ROW);
    c2Top->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c2Top->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* c2Title = new brls::Label();
    c2Title->setText("app/dashboard/retro_c2_title"_i18n);
    c2Title->setFontSize(11.5f);
    c2Title->setTextColor(themeCardTitle());
    c2Top->addView(c2Title);

    brls::Label* c2TopTag = new brls::Label();
    c2TopTag->setText("RetroArch / Core");
    c2TopTag->setFontSize(11.5f);
    c2TopTag->setTextColor(themeCardSub());
    c2Top->addView(c2TopTag);
    c2->addView(c2Top);

    brls::Box* c2Mid = new brls::Box();
    c2Mid->setAxis(brls::Axis::ROW);
    c2Mid->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* c2Val = new brls::Label();
    c2Val->setText("app/dashboard/retro_c2_val"_i18n);
    c2Val->setFontSize(22.0f);
    c2Val->setTextColor(themeTextPrimary());
    c2Mid->addView(c2Val);

    brls::Box* c2Badge = new brls::Box();
    c2Badge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
    c2Badge->setCornerRadius(4.0f);
    c2Badge->setBackgroundColor(themeDim());
    c2Badge->setMarginLeft(12.0f);
    brls::Label* c2BadgeLbl = new brls::Label();
    c2BadgeLbl->setText("app/dashboard/retro_c2_badge"_i18n);
    c2BadgeLbl->setFontSize(12.0f);
    c2BadgeLbl->setTextColor(themeAccent(255));
    c2Badge->addView(c2BadgeLbl);
    c2Mid->addView(c2Badge);
    c2->addView(c2Mid);

    brls::Label* c2Sub = new brls::Label();
    c2Sub->setText("app/dashboard/retro_c2_sub"_i18n);
    c2Sub->setFontSize(11.0f);
    c2Sub->setTextColor(themeCardSub());
    c2->addView(c2Sub);
    cardsRow->addView(c2);

    // Card 3: Storage & Extraction Settings
    brls::Box* c3 = new brls::Box();
    c3->setAxis(brls::Axis::COLUMN);
    c3->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c3->setWidth(360.0f);
    c3->setHeight(124.0f);
    c3->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    c3->setCornerRadius(10.0f);
    c3->setBackgroundColor(themeCardBg());
    c3->setBorderThickness(1.0f);
    c3->setBorderColor(themeCardBorder());
    c3->setFocusable(true);
    c3->registerClickAction(openRetro);

    auto& cfg = config::ConfigManager::instance();
    std::string romsMode = cfg.getRetroRomsMode();
    std::string modeTag = "RetroArch";
    if (romsMode == "downloads") modeTag = "app/dashboard/retro_mode_downloads"_i18n;
    else if (romsMode == "custom") modeTag = "app/dashboard/retro_mode_custom"_i18n;

    brls::Box* c3Top = new brls::Box();
    c3Top->setAxis(brls::Axis::ROW);
    c3Top->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    c3Top->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* c3Title = new brls::Label();
    c3Title->setText("app/dashboard/retro_c3_title"_i18n);
    c3Title->setFontSize(11.5f);
    c3Title->setTextColor(themeCardTitle());
    c3Top->addView(c3Title);

    brls::Label* c3TopTag = new brls::Label();
    c3TopTag->setText(modeTag);
    c3TopTag->setFontSize(11.5f);
    c3TopTag->setTextColor(themeCardSub());
    c3Top->addView(c3TopTag);
    c3->addView(c3Top);

    brls::Box* c3Mid = new brls::Box();
    c3Mid->setAxis(brls::Axis::ROW);
    c3Mid->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* c3Val = new brls::Label();
    std::string effDir = cfg.getEffectiveRetroRomsDir();
    if (effDir.length() > 22) {
        effDir = effDir.substr(0, 19) + "...";
    }
    c3Val->setText(effDir);
    c3Val->setFontSize(18.0f);
    c3Val->setTextColor(themeTextPrimary());
    c3Mid->addView(c3Val);

    if (cfg.getRetroAutoExtract()) {
        brls::Box* c3Badge = new brls::Box();
        c3Badge->setPadding(3.0f, 6.0f, 3.0f, 6.0f);
        c3Badge->setCornerRadius(4.0f);
        c3Badge->setBackgroundColor(nvgRGBA(255, 180, 50, 32));
        c3Badge->setMarginLeft(8.0f);
        brls::Label* c3BadgeLbl = new brls::Label();
        c3BadgeLbl->setText("Auto-ZIP");
        c3BadgeLbl->setFontSize(11.0f);
        c3BadgeLbl->setTextColor(nvgRGBA(255, 200, 80, 255));
        c3Badge->addView(c3BadgeLbl);
        c3Mid->addView(c3Badge);
    }
    c3->addView(c3Mid);

    brls::Label* c3Sub = new brls::Label();
    c3Sub->setText("app/dashboard/retro_c3_sub"_i18n);
    c3Sub->setFontSize(11.0f);
    c3Sub->setTextColor(themeCardSub());
    c3->addView(c3Sub);

    cardsRow->addView(c3);

    if (get_active_tile_) {
        brls::View* tile = get_active_tile_();
        if (tile) {
            c1->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
            c2->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
            c3->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
        }
    }

    content_container_->addView(cardsRow);
}

// -------------------------------------------------------------
// SECTION 2: GAME LIBRARY (2 Clean Sections: Installed & Updates)
// -------------------------------------------------------------
void DashboardSummaryView::buildLibrarySection() {
    auto openLib = [this](brls::View*) {
        if (on_open_section_) on_open_section_(2);
        return true;
    };

    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);
    headerRow->setFocusable(false);

    brls::Label* title = new brls::Label();
    title->setText("app/dashboard/library_title"_i18n);
    title->setFontSize(13.0f);
    title->setTextColor(themeAccent(240));
    headerRow->addView(title);
    content_container_->addView(headerRow);

    brls::Box* cardsRow = new brls::Box();
    cardsRow->setAxis(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    cardsRow->setWidthPercentage(100.0f);

    // Section 1: Installed Games
    brls::Box* instCard = new brls::Box();
    instCard->setAxis(brls::Axis::COLUMN);
    instCard->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    instCard->setWidth(420.0f);
    instCard->setHeight(124.0f);
    instCard->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    instCard->setCornerRadius(10.0f);
    instCard->setBackgroundColor(themeCardBg());
    instCard->setBorderThickness(1.0f);
    instCard->setBorderColor(themeCardBorder());
    instCard->setFocusable(true);
    instCard->registerClickAction(openLib);

    brls::Box* instTop = new brls::Box();
    instTop->setAxis(brls::Axis::ROW);
    instTop->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    instTop->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* instTitle = new brls::Label();
    instTitle->setText("app/dashboard/library_inst_title"_i18n);
    instTitle->setFontSize(12.0f);
    instTitle->setTextColor(themeCardTitle());
    instTop->addView(instTitle);

    brls::Label* instStorage = new brls::Label();
    instStorage->setText("app/dashboard/library_inst_storage"_i18n);
    instStorage->setFontSize(11.5f);
    instStorage->setTextColor(themeCardSub());
    instTop->addView(instStorage);
    instCard->addView(instTop);

    brls::Box* instMid = new brls::Box();
    instMid->setAxis(brls::Axis::ROW);
    instMid->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* instCountLbl = new brls::Label();
    instCountLbl->setText(brls::getStr("app/dashboard/library_inst_count", std::to_string(installed_count_)));
    instCountLbl->setFontSize(26.0f);
    instCountLbl->setTextColor(themeTextPrimary());
    instMid->addView(instCountLbl);

    brls::Box* instBadge = new brls::Box();
    instBadge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
    instBadge->setCornerRadius(4.0f);
    instBadge->setBackgroundColor(themeDim());
    instBadge->setMarginLeft(14.0f);
    brls::Label* instBadgeLbl = new brls::Label();
    instBadgeLbl->setText("app/dashboard/library_inst_badge"_i18n);
    instBadgeLbl->setFontSize(12.0f);
    instBadgeLbl->setTextColor(themeAccent(255));
    instBadge->addView(instBadgeLbl);
    instMid->addView(instBadge);
    instCard->addView(instMid);

    brls::Label* instSub = new brls::Label();
    instSub->setText("app/dashboard/library_inst_sub"_i18n);
    instSub->setFontSize(11.5f);
    instSub->setTextColor(themeCardSub());
    instCard->addView(instSub);
    cardsRow->addView(instCard);

    // Section 2: Available Updates
    brls::Box* updCard = new brls::Box();
    updCard->setAxis(brls::Axis::COLUMN);
    updCard->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    updCard->setWidth(720.0f);
    updCard->setHeight(124.0f);
    updCard->setPadding(12.0f, 18.0f, 12.0f, 18.0f);
    updCard->setCornerRadius(10.0f);
    updCard->setBackgroundColor(themeCardBg());
    updCard->setBorderThickness(1.0f);
    updCard->setBorderColor(themeCardBorder());
    updCard->setFocusable(true);
    updCard->registerClickAction(openLib);

    brls::Box* updTop = new brls::Box();
    updTop->setAxis(brls::Axis::ROW);
    updTop->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    updTop->setAlignItems(brls::AlignItems::CENTER);

    brls::Label* updTitle = new brls::Label();
    updTitle->setText("app/dashboard/library_upd_title"_i18n);
    updTitle->setFontSize(12.0f);
    updTitle->setTextColor(themeCardTitle());
    updTop->addView(updTitle);

    brls::Label* updSync = new brls::Label();
    updSync->setText("app/dashboard/library_upd_sync"_i18n);
    updSync->setFontSize(11.5f);
    updSync->setTextColor(themeCardSub());
    updTop->addView(updSync);
    updCard->addView(updTop);

    brls::Box* updMid = new brls::Box();
    updMid->setAxis(brls::Axis::ROW);
    updMid->setAlignItems(brls::AlignItems::CENTER);

    if (updates_count_ > 0) {
        brls::Label* updCountLbl = new brls::Label();
        updCountLbl->setText(brls::getStr("app/dashboard/library_upd_count", std::to_string(updates_count_)));
        updCountLbl->setFontSize(26.0f);
        updCountLbl->setTextColor(nvgRGBA(255, 185, 70, 255)); // Amber
        updMid->addView(updCountLbl);

        brls::Box* updBadge = new brls::Box();
        updBadge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
        updBadge->setCornerRadius(4.0f);
        updBadge->setBackgroundColor(nvgRGBA(255, 180, 50, 32));
        updBadge->setMarginLeft(14.0f);
        brls::Label* updBadgeLbl = new brls::Label();
        updBadgeLbl->setText("app/dashboard/library_upd_badge_avail"_i18n);
        updBadgeLbl->setFontSize(12.0f);
        updBadgeLbl->setTextColor(nvgRGBA(255, 200, 80, 255));
        updBadge->addView(updBadgeLbl);
        updMid->addView(updBadge);
    } else {
        brls::Label* updCountLbl = new brls::Label();
        updCountLbl->setText("app/dashboard/library_upd_all_updated"_i18n);
        updCountLbl->setFontSize(22.0f);
        updCountLbl->setTextColor(themeAccent(255));
        updMid->addView(updCountLbl);

        brls::Box* updBadge = new brls::Box();
        updBadge->setPadding(3.0f, 8.0f, 3.0f, 8.0f);
        updBadge->setCornerRadius(4.0f);
        updBadge->setBackgroundColor(themeDim());
        updBadge->setMarginLeft(14.0f);
        brls::Label* updBadgeLbl = new brls::Label();
        updBadgeLbl->setText("app/dashboard/library_upd_badge_ok"_i18n);
        updBadgeLbl->setFontSize(12.0f);
        updBadgeLbl->setTextColor(themeAccent(255));
        updBadge->addView(updBadgeLbl);
        updMid->addView(updBadge);
    }
    updCard->addView(updMid);

    brls::Label* updSub = new brls::Label();
    updSub->setText("app/dashboard/library_upd_sub"_i18n);
    updSub->setFontSize(11.5f);
    updSub->setTextColor(themeCardSub());
    updCard->addView(updSub);

    cardsRow->addView(updCard);

    if (get_active_tile_) {
        brls::View* tile = get_active_tile_();
        if (tile) {
            instCard->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
            updCard->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
        }
    }

    content_container_->addView(cardsRow);
}

// -------------------------------------------------------------
// SECTION 3: DOWNLOADS (Active Download with Metrics & Sparkline OR Idle Recommendations)
// -------------------------------------------------------------
void DashboardSummaryView::buildDownloadsSection() {
    auto openDl = [this](brls::View*) {
        if (on_open_section_) on_open_section_(3);
        return true;
    };

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
        headerRow->setFocusable(false);

        brls::Label* title = new brls::Label();
        title->setText("app/dashboard/downloads_title"_i18n);
        title->setFontSize(13.0f);
        title->setTextColor(themeAccent(240));
        headerRow->addView(title);
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
        mainCard->setBackgroundColor(themeCardBg());
        mainCard->setBorderThickness(1.0f);
        mainCard->setBorderColor(themeCardBorder());
        mainCard->setFocusable(true);
        mainCard->registerClickAction(openDl);

        // Cover Box
        brls::Box* coverBox = new brls::Box();
        coverBox->setWidth(68.0f);
        coverBox->setHeight(108.0f);
        coverBox->setCornerRadius(6.0f);
        coverBox->setBackgroundColor(ThemeManager::instance().isCurrentThemeLight() ? nvgRGBA(210, 225, 240, 180) : nvgRGBA(15, 25, 38, 140));
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
        dl_titleLbl_->setTextColor(themeTextPrimary());
        dl_titleLbl_->setSingleLine(true);
        topRow->addView(dl_titleLbl_);

        std::string stText = (activeItem->state == download::DownloadState::Installing || activeItem->state == download::DownloadState::StreamInstalling)
                             ? "app/dashboard/downloads_status_installing"_i18n : "app/dashboard/downloads_status_downloading"_i18n;
        dl_stLbl_ = new brls::Label();
        dl_stLbl_->setText(stText);
        dl_stLbl_->setFontSize(12.0f);
        dl_stLbl_->setTextColor(themeAccent(255));
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
        barBg->setBackgroundColor(ThemeManager::instance().isCurrentThemeLight() ? nvgRGBA(210, 220, 235, 180) : nvgRGBA(18, 32, 50, 180));
        barBg->setMarginRight(10.0f);

        dl_barFill_ = new brls::Box();
        dl_barFill_->setWidthPercentage(std::max(2.0f, activeItem->progress * 100.0f));
        dl_barFill_->setHeight(6.0f);
        dl_barFill_->setCornerRadius(3.0f);
        dl_barFill_->setBackgroundColor(themeAccent(255));
        barBg->addView(dl_barFill_);
        barRow->addView(barBg);

        char pctBuf[16];
        std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", activeItem->progress * 100.0f);
        dl_pctLbl_ = new brls::Label();
        dl_pctLbl_->setText(pctBuf);
        dl_pctLbl_->setFontSize(13.0f);
        dl_pctLbl_->setTextColor(themeAccent(255));
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
        dl_spdLbl_->setTextColor(themeAccent(255));
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
        dl_szLbl_->setTextColor(themeCardTitle());
        metricsRow->addView(dl_szLbl_);

        std::string peersStr = "app/dashboard/downloads_peers"_i18n + std::to_string(activeItem->peers) + "app/dashboard/downloads_seeds"_i18n + std::to_string(activeItem->seeds);
        dl_peersLbl_ = new brls::Label();
        dl_peersLbl_->setText(peersStr);
        dl_peersLbl_->setFontSize(11.5f);
        dl_peersLbl_->setTextColor(themeCardSub());
        metricsRow->addView(dl_peersLbl_);

        std::string etaStr = "app/dashboard/downloads_in_progress"_i18n;
        if (activeItem->download_speed_kbps > 10.0f && inst_total > inst_written) {
            unsigned long long remBytes = inst_total - inst_written;
            unsigned long long rate = static_cast<unsigned long long>(activeItem->download_speed_kbps * 1024.0f);
            unsigned long long sec = remBytes / rate;
            etaStr = brls::getStr("app/dashboard/downloads_eta_min", std::to_string((sec / 60) + 1));
        }
        dl_etaLbl_ = new brls::Label();
        dl_etaLbl_->setText("app/dashboard/downloads_eta_prefix"_i18n + etaStr);
        dl_etaLbl_->setFontSize(11.5f);
        dl_etaLbl_->setTextColor(themeCardSub());
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
        graphCard->setBackgroundColor(themeCardBg());
        graphCard->setBorderThickness(1.0f);
        graphCard->setBorderColor(themeCardBorder());
        graphCard->setFocusable(true);
        graphCard->registerClickAction(openDl);

        brls::Box* gHeader = new brls::Box();
        gHeader->setAxis(brls::Axis::ROW);
        gHeader->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);

        brls::Label* gLbl = new brls::Label();
        gLbl->setText("app/dashboard/downloads_speed_graph"_i18n);
        gLbl->setFontSize(11.5f);
        gLbl->setTextColor(themeCardTitle());
        gHeader->addView(gLbl);

        dl_qCountLbl_ = new brls::Label();
        dl_qCountLbl_->setText("app/dashboard/downloads_queue_prefix"_i18n + std::to_string(cached_downloads_.size()));
        dl_qCountLbl_->setFontSize(11.5f);
        dl_qCountLbl_->setTextColor(themeAccent(255));
        gHeader->addView(dl_qCountLbl_);
        graphCard->addView(gHeader);

        dl_sparkline_ = new SpeedSparklineView();
        dl_sparkline_->setWidthPercentage(100.0f);
        dl_sparkline_->setHeight(56.0f);
        dl_sparkline_->setSamples(speed_history_);
        graphCard->addView(dl_sparkline_);

        brls::Label* gFooter = new brls::Label();
        gFooter->setText("app/dashboard/downloads_storage_dest"_i18n);
        gFooter->setFontSize(11.0f);
        gFooter->setTextColor(themeCardSub());
        graphCard->addView(gFooter);

        bodyRow->addView(graphCard);

        if (get_active_tile_) {
            brls::View* tile = get_active_tile_();
            if (tile) {
                mainCard->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
                graphCard->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
            }
        }

        content_container_->addView(bodyRow);
    } else {
        dl_active_mode_ = false;
        // Idle State: Recommendations from Catalog / Top 100
        brls::Box* headerRow = new brls::Box();
        headerRow->setAxis(brls::Axis::ROW);
        headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        headerRow->setAlignItems(brls::AlignItems::CENTER);
        headerRow->setMarginBottom(6.0f);
        headerRow->setFocusable(false);

        brls::Label* title = new brls::Label();
        title->setText("app/dashboard/downloads_empty_title"_i18n);
        title->setFontSize(13.0f);
        title->setTextColor(themeAccent(240));
        headerRow->addView(title);
        content_container_->addView(headerRow);

        brls::Box* cardsRow = new brls::Box();
        cardsRow->setAxis(brls::Axis::ROW);
        cardsRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        cardsRow->setWidthPercentage(100.0f);

        if (catalog_sample_.empty()) {
            brls::Label* emptyLbl = new brls::Label();
            emptyLbl->setText("app/dashboard/catalog_loading"_i18n);
            emptyLbl->setFontSize(13.0f);
            emptyLbl->setTextColor(themeTextSecondary());
            cardsRow->addView(emptyLbl);
        } else {
            for (const auto& g : catalog_sample_) {
                DashboardGameCard* card = new DashboardGameCard(g, on_defocus_);
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
    auto openTools = [this](brls::View*) {
        if (on_open_section_) on_open_section_(4);
        return true;
    };

    brls::Box* headerRow = new brls::Box();
    headerRow->setAxis(brls::Axis::ROW);
    headerRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(6.0f);
    headerRow->setFocusable(false);

    brls::Label* title = new brls::Label();
    title->setText("app/dashboard/tools_title"_i18n);
    title->setFontSize(13.0f);
    title->setTextColor(themeAccent(240));
    headerRow->addView(title);
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
    c1->setBackgroundColor(themeCardBg());
    c1->setBorderThickness(1.0f);
    c1->setBorderColor(themeCardBorder());
    c1->setFocusable(true);
    c1->registerClickAction(openTools);

    brls::Label* c1Title = new brls::Label();
    c1Title->setText("app/dashboard/tools_c1_title"_i18n);
    c1Title->setFontSize(11.5f);
    c1Title->setTextColor(themeCardTitle());
    c1->addView(c1Title);

    brls::Label* c1Val = new brls::Label();
    c1Val->setText(engine_mode_);
    c1Val->setFontSize(21.0f);
    c1Val->setTextColor(themeAccent(255));
    c1->addView(c1Val);

    brls::Label* c1Sub = new brls::Label();
    c1Sub->setText("app/dashboard/tools_c1_sub"_i18n);
    c1Sub->setFontSize(11.0f);
    c1Sub->setTextColor(themeCardSub());
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
    c2->setBackgroundColor(themeCardBg());
    c2->setBorderThickness(1.0f);
    c2->setBorderColor(themeCardBorder());
    c2->setFocusable(true);
    c2->registerClickAction(openTools);

    brls::Label* c2Title = new brls::Label();
    c2Title->setText("app/dashboard/tools_c2_title"_i18n);
    c2Title->setFontSize(11.5f);
    c2Title->setTextColor(themeCardTitle());
    c2->addView(c2Title);

    brls::Label* c2Val = new brls::Label();
    c2Val->setText(formatBytes(cache_size_bytes_));
    c2Val->setFontSize(21.0f);
    c2Val->setTextColor(themeTextPrimary());
    c2->addView(c2Val);

    brls::Label* c2Sub = new brls::Label();
    c2Sub->setText("app/dashboard/tools_c2_sub"_i18n);
    c2Sub->setFontSize(11.0f);
    c2Sub->setTextColor(themeCardSub());
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
    c3->setBackgroundColor(themeCardBg());
    c3->setBorderThickness(1.0f);
    c3->setBorderColor(themeCardBorder());
    c3->setFocusable(true);
    c3->registerClickAction(openTools);

    brls::Label* c3Title = new brls::Label();
    c3Title->setText("app/dashboard/tools_c3_title"_i18n);
    c3Title->setFontSize(11.5f);
    c3Title->setTextColor(themeCardTitle());
    c3->addView(c3Title);

    brls::Label* c3Val = new brls::Label();
    if (leftover_size_bytes_ > 0) {
        c3Val->setText(formatBytes(leftover_size_bytes_));
        c3Val->setTextColor(nvgRGBA(255, 185, 70, 255)); // Amber
    } else {
        c3Val->setText("app/dashboard/tools_c3_clean"_i18n);
        c3Val->setTextColor(themeAccent(255));
    }
    c3Val->setFontSize(21.0f);
    c3->addView(c3Val);

    brls::Label* c3Sub = new brls::Label();
    c3Sub->setText("app/dashboard/tools_c3_sub"_i18n);
    c3Sub->setFontSize(11.0f);
    c3Sub->setTextColor(themeCardSub());
    c3->addView(c3Sub);
    cardsRow->addView(c3);

    if (get_active_tile_) {
        brls::View* tile = get_active_tile_();
        if (tile) {
            c1->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
            c2->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
            c3->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
        }
    }

    content_container_->addView(cardsRow);
}

void DashboardSummaryView::draw(NVGcontext* vg, float x, float y, float width, float height,
                                brls::Style style, brls::FrameContext* ctx) {
    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    NVGcolor accent = ThemeManager::instance().getAccentColor();

    // 1. Frosted Glass Base: Translucent, tinted with the active theme color
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 16.0f);
    NVGpaint bgPaint;
    if (isLight) {
        bgPaint = nvgLinearGradient(
            vg, x, y, x, y + height,
            nvgRGBA(255, 255, 255, 230),
            nvgRGBA(242, 246, 252, 210)
        );
    } else {
        bgPaint = nvgLinearGradient(
            vg, x, y, x, y + height,
            nvgRGBA(accent.r * 255 * 0.35f, accent.g * 255 * 0.35f, accent.b * 255 * 0.35f, 40),
            nvgRGBA(8, 14, 22, 90)
        );
    }
    nvgFillPaint(vg, bgPaint);
    nvgFill(vg);

    // 2. Specular Top Glass Highlight Sheen
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 1.0f, y + 1.0f, width - 2.0f, height * 0.45f, 15.0f);
    NVGpaint glossPaint = nvgLinearGradient(
        vg, x, y, x, y + height * 0.45f,
        nvgRGBA(255, 255, 255, isLight ? 70 : 30),
        nvgRGBA(255, 255, 255, 0)
    );
    nvgFillPaint(vg, glossPaint);
    nvgFill(vg);

    // 3. Subtle Glass Beveled Border Stroke in Theme Tone
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 16.0f);
    NVGpaint borderPaint;
    if (isLight) {
        borderPaint = nvgLinearGradient(
            vg, x, y, x, y + height,
            nvgRGBA(210, 220, 232, 200),
            nvgRGBA(185, 198, 215, 150)
        );
    } else {
        borderPaint = nvgLinearGradient(
            vg, x, y, x, y + height,
            nvgRGBA(std::min<int>(255, accent.r * 255 * 0.7f + 40),
                    std::min<int>(255, accent.g * 255 * 0.7f + 40),
                    std::min<int>(255, accent.b * 255 * 0.7f + 40), 95),
            nvgRGBA(accent.r * 255 * 0.3f, accent.g * 255 * 0.3f, accent.b * 255 * 0.3f, 30)
        );
    }
    nvgStrokePaint(vg, borderPaint);
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);

    // 4. Draw children views (cards)
    Box::draw(vg, x, y, width, height, style, ctx);
}

} // namespace ui
