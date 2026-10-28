#include "DashboardSummaryView.hpp"
#include "SpeedSparklineView.hpp"
#include "DashboardIcon.hpp"
#include "../ThemeManager.hpp"
#include "../style/UiTokens.hpp"
#include "../../utils/string_utils.h"
#include "../GameDetailView.hpp"
#include "../../utils/switch_utils.h"
#include "../../utils/app_paths.h"
#include "../../config/config.h"
#include "../../catalog/retro_catalog_manager.h"
#include "../../utils/log.h"
#include "../../utils/i18n_plural.h"
#include "../../net/image_downloader.h"
#include <cstdio>
#include <algorithm>
#include <utility>

using namespace brls::literals;

extern std::vector<Game> g_games;

namespace ui {

// -------------------------------------------------------------
// Panel template: sizes and colors shared by every section
// -------------------------------------------------------------
static constexpr float kPanelHeight  = 232.0f;
static constexpr float kCardWidth    = 384.0f;
static constexpr float kCardHeight   = 172.0f;
static constexpr float kCardPadV     = 16.0f;
static constexpr float kCardPadH     = 20.0f;
static constexpr float kCardInnerW   = kCardWidth - 2 * kCardPadH;
static constexpr float kCardRadius   = style::kRadiusCard;
static constexpr float kHeaderFont   = style::kFontSub;
static constexpr float kCaptionFont  = style::kFontCaption;
static constexpr float kValueFont    = style::kFontValue;
static constexpr float kValueFontMid = style::kFontValueMid;
static constexpr float kSubFont      = style::kFontSub;
static constexpr float kCardCaptionFont = 16.0f; // caption at the top of a panel card
static constexpr float kCardSubFont     = 17.0f; // line at the bottom of a panel card
static constexpr float kCoverW       = 88.0f;
static constexpr float kCoverH       = 140.0f;

static inline NVGcolor themeAccent(unsigned char alpha = 255) {
    NVGcolor c = ThemeManager::instance().getTextAccentColor();
    return nvgRGBA(c.r * 255, c.g * 255, c.b * 255, alpha);
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

static bool isActiveState(download::DownloadState st) {
    return st == download::DownloadState::Downloading ||
           st == download::DownloadState::StreamPreparing ||
           st == download::DownloadState::StreamInstalling ||
           st == download::DownloadState::Installing;
}

static const download::DownloadItem* findActiveDownload(const std::vector<download::DownloadItem>& items) {
    for (const auto& it : items) {
        if (isActiveState(it.state)) return &it;
    }
    return nullptr;
}

static const download::DownloadItem* findLastCompleted(const std::vector<download::DownloadItem>& items) {
    const download::DownloadItem* best = nullptr;
    for (const auto& it : items) {
        if (it.state != download::DownloadState::Completed) continue;
        if (!best || it.finish_time >= best->finish_time) best = &it;
    }
    return best;
}

static int countWaiting(const std::vector<download::DownloadItem>& items) {
    int n = 0;
    for (const auto& it : items) {
        if (it.state == download::DownloadState::Queued || it.state == download::DownloadState::Paused) ++n;
    }
    return n;
}

static brls::Label* makeLabel(const std::string& text, float size, NVGcolor color) {
    brls::Label* l = new brls::Label();
    l->setText(text);
    l->setFontSize(size);
    l->setTextColor(color);
    l->setSingleLine(true);
    return l;
}

static brls::Box* makeRow() {
    brls::Box* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    return row;
}

// A focusable card of the panel template: caption / value / sub, top to bottom.
static brls::Box* makeCard(float width, const std::function<bool(brls::View*)>& onClick) {
    brls::Box* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    card->setWidth(width);
    card->setHeight(kCardHeight);
    card->setPadding(kCardPadV, kCardPadH, kCardPadV, kCardPadH);
    card->setCornerRadius(kCardRadius);
    card->setBackgroundColor(style::cardBg());
    card->setBorderThickness(1.0f);
    card->setBorderColor(style::cardBorder());
    card->setFocusable(true);
    if (onClick) card->registerClickAction(onClick);
    return card;
}

static void addCaption(brls::Box* card, const std::string& caption, const std::string& meta = "") {
    brls::Box* row = makeRow();
    row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    row->setWidthPercentage(100.0f);
    brls::Label* c = makeLabel(caption, kCardCaptionFont, style::textSecondary());
    c->setLetterSpacing(0.6f);
    row->addView(c);
    if (!meta.empty()) {
        brls::Label* m = makeLabel(meta, kCaptionFont, style::textSecondary());
        m->setMarginLeft(12.0f);
        row->addView(m);
    }
    card->addView(row);
}

static brls::Label* addSub(brls::Box* card, const std::string& text, NVGcolor color) {
    brls::Label* s = makeLabel(text, kCardSubFont, color);
    s->setWidthPercentage(100.0f);
    card->addView(s);
    return s;
}

using Icon = DashboardIcon::Kind;

static constexpr float kEmblem    = 56.0f;
static constexpr float kEmblemGap = 14.0f;

// Middle row of a panel card: illustrated icon, then the large value.
static brls::Label* addIconValue(brls::Box* card, Icon kind, const std::string& text, NVGcolor color,
                                 float size = kValueFont) {
    brls::Box* row = makeRow();
    row->setWidthPercentage(100.0f);
    DashboardIcon* ic = new DashboardIcon(kind, kEmblem);
    ic->setMarginRight(kEmblemGap);
    row->addView(ic);
    brls::Label* v = makeLabel(text, size, color);
    v->setWidth(kCardInnerW - kEmblem - kEmblemGap);
    row->addView(v);
    card->addView(row);
    return v;
}

// Strips torrent tags ("[NRO][RUS/Multi12]") and tidies "(port )" for display.
static std::string displayTitle(const std::string& title) {
    std::string t = cleanTitle(title);
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == ' ' && i + 1 < t.size() && t[i + 1] == ')') continue;
        if (t[i] == ' ' && !out.empty() && out.back() == ' ') continue;
        out += t[i];
    }
    return out;
}

// Progress / storage bar. Returns the fill box so callers can update it.
static brls::Box* makeBar(brls::Box* parent, float height, float percent) {
    brls::Box* track = new brls::Box();
    track->setGrow(1.0f);
    track->setHeight(height);
    track->setCornerRadius(height / 2.0f);
    track->setBackgroundColor(style::barTrack());
    brls::Box* fill = new brls::Box();
    fill->setWidthPercentage(std::max(2.0f, std::min(100.0f, percent)));
    fill->setHeight(height);
    fill->setCornerRadius(height / 2.0f);
    fill->setBackgroundColor(ThemeManager::instance().getAccentColor());
    track->addView(fill);
    parent->addView(track);
    return fill;
}

static std::string ofStr(const std::string& a, const std::string& b) {
    return brls::getStr("app/dashboard/of_fmt", a, b);
}

// -------------------------------------------------------------
// Catalog card: cover, two-line title, size and year, genre
// -------------------------------------------------------------
class DashboardGameCard : public brls::Box {
public:
    DashboardGameCard(const Game& game, std::function<void()> on_back = nullptr)
        : game_(game), on_back_(on_back) {

        cardImageToken_ = std::make_shared<bool>(true);

        this->setFocusable(true);
        this->setHideHighlight(true);
        this->setAxis(brls::Axis::ROW);
        this->setWidth(kCardWidth);
        this->setHeight(kCardHeight);
        this->setPadding(kCardPadV);
        this->setCornerRadius(kCardRadius);

        brls::Box* imgBox = new brls::Box();
        imgBox->setWidth(kCoverW);
        imgBox->setHeight(kCoverH);
        imgBox->setCornerRadius(6.0f);
        imgBox->setBackgroundColor(style::isLight() ? nvgRGBA(210, 225, 240, 180) : nvgRGBA(15, 25, 38, 140));
        imgBox->setMarginRight(16.0f);
        imgBox->setShrink(0.0f);

        brls::Image* coverImg = new brls::Image();
        coverImg->setWidth(kCoverW);
        coverImg->setHeight(kCoverH);
        coverImg->setCornerRadius(6.0f);
        coverImg->setScalingType(brls::ImageScalingType::FILL);
        if (!game.cover.empty()) {
            setImageFromHTTPS(coverImg, game.cover, cardImageToken_, "romfs:/img/cover_blank.png");
        } else {
            coverImg->setImageFromFile("romfs:/img/cover_blank.png");
        }
        imgBox->addView(coverImg);
        this->addView(imgBox);

        const float infoW = kCardWidth - 2 * kCardPadV - kCoverW - 16.0f;
        brls::Box* infoCol = new brls::Box();
        infoCol->setAxis(brls::Axis::COLUMN);
        infoCol->setWidth(infoW);
        infoCol->setHeight(kCoverH);

        // One line; the full title is on the game page
        titleLbl1_ = makeLabel(displayTitle(game.title), 18.0f, style::textPrimary());
        titleLbl1_->setWidth(infoW);
        titleLbl1_->setAutoAnimate(false);
        infoCol->addView(titleLbl1_);

        std::string metaText = game.size.empty() ? "NSP" : game.size;
        if (!game.year.empty()) metaText += "  ·  " + game.year;
        brls::Label* meta = makeLabel(metaText, kSubFont, style::textPrimary());
        meta->setWidth(infoW);
        meta->setMarginTop(8.0f);
        infoCol->addView(meta);

        if (!util::isPlaceholderValue(game.genre)) {
            brls::Label* genre = makeLabel(game.genre, kSubFont, style::textSecondary());
            genre->setWidth(infoW);
            genre->setMarginTop(4.0f);
            infoCol->addView(genre);
        }

        // Interface language, pinned to the bottom of the column
        GameLangBadge badge = getGameLangBadge(game.interface_lang, brls::Application::getLocale());
        if (!badge.text.empty()) {
            brls::Box* spacer = new brls::Box();
            spacer->setGrow(1.0f);
            infoCol->addView(spacer);
            brls::Box* pill = new brls::Box();
            pill->setPadding(2.0f, 8.0f, 2.0f, 8.0f);
            pill->setCornerRadius(style::kRadiusChip);
            pill->setBackgroundColor(badge.color);
            pill->setAlignSelf(brls::AlignSelf::FLEX_START);
            pill->addView(makeLabel(badge.text, 13.0f, nvgRGB(255, 255, 255)));
            infoCol->addView(pill);
        }
        this->addView(infoCol);

        this->registerAction("hints/ok"_i18n, brls::ControllerButton::BUTTON_A, [this](brls::View*) {
            triggerOpen();
            return true;
        });
        if (on_back_) {
            this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View*) {
                if (on_back_) {
                    on_back_();
                    return true;
                }
                return false;
            });
        }
        this->addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound*) {
            if (status.state == brls::GestureState::END) triggerOpen();
        }));
    }

    void triggerOpen() {
        brls::Application::pushActivity(new ui::GameDetailView(game_));
    }

    void onFocusGained() override {
        Box::onFocusGained();
        if (titleLbl1_) titleLbl1_->setTextColor(themeAccent(255));
        if (titleLbl2_) titleLbl2_->setTextColor(themeAccent(255));
    }

    void onFocusLost() override {
        Box::onFocusLost();
        if (titleLbl1_) titleLbl1_->setTextColor(style::textPrimary());
        if (titleLbl2_) titleLbl2_->setTextColor(style::textPrimary());
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        float targetScale = isFocused() ? 1.03f : 1.0f;
        scale_ += (targetScale - scale_) * 0.25f;
        float targetGlow = isFocused() ? 1.0f : 0.0f;
        glow_ += (targetGlow - glow_) * 0.25f;

        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;

        nvgSave(vg);
        nvgTranslate(vg, cx, cy);
        nvgScale(vg, scale_, scale_);
        nvgTranslate(vg, -cx, -cy);

        if (glow_ > 0.01f) {
            NVGpaint glowPaint = nvgBoxGradient(vg, x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f,
                                                kCardRadius, 6.0f,
                                                themeAccent(static_cast<unsigned char>(90.0f * glow_)),
                                                themeAccent(0));
            nvgBeginPath(vg);
            nvgRect(vg, x - 10.0f, y - 10.0f, width + 20.0f, height + 20.0f);
            nvgFillPaint(vg, glowPaint);
            nvgFill(vg);
        }

        bool isLight = style::isLight();
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, kCardRadius);
        nvgFillColor(vg, style::cardBg());
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, kCardRadius);
        if (glow_ > 0.01f) {
            nvgStrokeColor(vg, themeAccent(static_cast<unsigned char>(255.0f * glow_)));
            nvgStrokeWidth(vg, isLight ? 2.5f : 2.0f);
        } else {
            nvgStrokeColor(vg, style::cardBorder());
            nvgStrokeWidth(vg, 1.0f);
        }
        nvgStroke(vg);

        Box::draw(vg, x, y, width, height, style, ctx);
        nvgRestore(vg);
    }

    ~DashboardGameCard() override {
        if (cardImageToken_) *cardImageToken_ = false;
    }

private:
    Game game_;
    std::shared_ptr<bool> cardImageToken_;
    brls::Label* titleLbl1_ = nullptr;
    brls::Label* titleLbl2_ = nullptr;
    std::function<void()> on_back_;
    float scale_ = 1.0f;
    float glow_ = 0.0f;
};

// -------------------------------------------------------------
// Panel
// -------------------------------------------------------------
DashboardSummaryView::DashboardSummaryView() {
    imageToken_ = std::make_shared<bool>(true);
    this->setWidthPercentage(95.0f);
    this->setHeight(kPanelHeight);
    this->setAxis(brls::Axis::COLUMN);
    this->setPadding(12.0f, 16.0f, 16.0f, 16.0f);
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

    // Fade the new section in instead of swapping it instantly
    if (content_container_) {
        content_container_->alpha.stop();
        content_container_->alpha.reset(0.0f);
        content_container_->alpha.addStep(1.0f, 180, brls::EasingFunction::quadraticOut);
        content_container_->alpha.start();
    }
}

std::string DashboardSummaryView::idleDownloadsSignature() const {
    const download::DownloadItem* last = findLastCompleted(cached_downloads_);
    return std::to_string(countWaiting(cached_downloads_)) + "|" + (last ? last->title : std::string());
}

void DashboardSummaryView::updateActiveDownloadWidgets(const download::DownloadItem& item) {
    if (dl_titleLbl_) dl_titleLbl_->setText(cleanTitle(item.title));

    std::string stText;
    if (!item.install_stage_label.empty()) {
        stText = item.install_stage_label;
    } else if (item.state == download::DownloadState::Installing || item.state == download::DownloadState::StreamInstalling) {
        stText = "app/dashboard/downloads_status_installing"_i18n;
    } else {
        stText = "app/dashboard/downloads_status_downloading"_i18n;
    }
    if (dl_stLbl_) dl_stLbl_->setText(stText);

    if (dl_barFill_) dl_barFill_->setWidthPercentage(std::max(2.0f, item.progress * 100.0f));

    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", item.progress * 100.0f);
    if (dl_pctLbl_) dl_pctLbl_->setText(pctBuf);

    char spdBuf[32];
    std::snprintf(spdBuf, sizeof(spdBuf), "%.1f MB/s", item.download_speed_kbps / 1024.0f);
    if (dl_spdLbl_) dl_spdLbl_->setText(spdBuf);

    unsigned long long written = item.install_written;
    unsigned long long total = item.install_total;
    if (total == 0 && item.hybrid_installer) total = item.hybrid_installer->totalBytes();
    if (dl_szLbl_) dl_szLbl_->setText(ofStr(formatBytes(written), formatBytes(total)));

    if (dl_peersLbl_) {
        dl_peersLbl_->setText("app/dashboard/downloads_peers"_i18n + std::to_string(item.peers) +
                              "app/dashboard/downloads_seeds"_i18n + std::to_string(item.seeds));
    }

    std::string etaStr = "app/dashboard/downloads_in_progress"_i18n;
    if (item.download_speed_kbps > 10.0f && total > written) {
        unsigned long long rate = static_cast<unsigned long long>(item.download_speed_kbps * 1024.0f);
        unsigned long long sec = (total - written) / rate;
        etaStr = brls::getStr("app/dashboard/downloads_eta_min", std::to_string((sec / 60) + 1));
    }
    if (dl_etaLbl_) dl_etaLbl_->setText(etaStr);

    if (dl_qCountLbl_) {
        dl_qCountLbl_->setText("app/dashboard/downloads_queue_prefix"_i18n + std::to_string(countWaiting(cached_downloads_)));
    }
    if (dl_sparkline_) dl_sparkline_->setSamples(speed_history_);

    // The cover may resolve later, once the catalog sample has loaded.
    if (dl_coverImg_ && dl_loaded_cover_url_.empty()) {
        std::string coverUrl = findCoverForDownload(item, catalog_sample_);
        if (!coverUrl.empty()) {
            dl_loaded_cover_url_ = coverUrl;
            setImageFromHTTPS(dl_coverImg_, coverUrl, imageToken_, "romfs:/img/cover_blank.png",
                              false, "", -1, -1, 1000000);
        }
    }
}

void DashboardSummaryView::updateDownloads(const std::vector<download::DownloadItem>& items) {
    const download::DownloadItem* activeItem = findActiveDownload(items);

    if (activeItem) {
        speed_history_.push_back(activeItem->download_speed_kbps * 1024.0f);
        while (speed_history_.size() > 45) speed_history_.pop_front();
    } else {
        speed_history_.clear();
    }

    cached_downloads_ = items;
    if (active_index_ != 3) return;

    activeItem = findActiveDownload(cached_downloads_);
    if (activeItem) {
        if (dl_active_mode_ && dl_coverImg_ != nullptr &&
            activeItem->topic_id == dl_active_topic_id_ && activeItem->title == dl_active_title_) {
            updateActiveDownloadWidgets(*activeItem);
        } else {
            rebuildContent();
        }
    } else if (dl_active_mode_ || idleDownloadsSignature() != dl_idle_signature_) {
        // Active -> idle, or the queue / last completed item changed.
        rebuildContent();
    }
}

void DashboardSummaryView::setCatalogSample(const std::vector<Game>& games) {
    size_t targetCount = std::min<size_t>(games.size(), 3);
    bool changed = catalog_total_ != games.size() || catalog_sample_.size() != targetCount;
    if (!changed) {
        for (size_t i = 0; i < targetCount; ++i) {
            if (catalog_sample_[i].title != games[i].title || catalog_sample_[i].cover != games[i].cover) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;

    catalog_total_ = games.size();
    catalog_sample_.assign(games.begin(), games.begin() + targetCount);

    if (active_index_ == 0) {
        rebuildContent();
    } else if (active_index_ == 3 && dl_active_mode_) {
        if (const download::DownloadItem* activeItem = findActiveDownload(cached_downloads_)) {
            updateActiveDownloadWidgets(*activeItem);
        }
    }
}

void DashboardSummaryView::setRemoteInfo(const std::string& ip_str, int port) {
    local_ip_ = ip_str;
    remote_port_ = port;
}

void DashboardSummaryView::setLibraryStats(int installed_count, int updates_count) {
    if (installed_count_ == installed_count && updates_count_ == updates_count) return;
    installed_count_ = installed_count;
    updates_count_ = updates_count;
    if (active_index_ == 2) rebuildContent();
}

void DashboardSummaryView::setSettingsStats(const std::string& engine_mode, uint64_t cache_size_bytes, uint64_t leftover_size_bytes) {
    if (engine_mode_ == engine_mode && cache_size_bytes_ == cache_size_bytes && leftover_size_bytes_ == leftover_size_bytes) return;
    engine_mode_ = engine_mode;
    cache_size_bytes_ = cache_size_bytes;
    leftover_size_bytes_ = leftover_size_bytes;
    if (active_index_ == 4) rebuildContent();
}

void DashboardSummaryView::setStorageStats(int64_t sd_free, int64_t sd_total, int64_t nand_free, int64_t nand_total) {
    if (sd_free_ == sd_free && sd_total_ == sd_total && nand_free_ == nand_free && nand_total_ == nand_total) return;
    sd_free_ = sd_free;
    sd_total_ = sd_total;
    nand_free_ = nand_free;
    nand_total_ = nand_total;
    updateStorageWidgets();
}

void DashboardSummaryView::updateStorageWidgets() {
    auto apply = [](brls::Label* lbl, brls::Box* fill, int64_t freeB, int64_t totalB) {
        if (!lbl || !fill) return;
        if (totalB <= 0) {
            lbl->setText("—");
            fill->setWidthPercentage(2.0f);
            return;
        }
        lbl->setText(ofStr(formatBytes(static_cast<unsigned long long>(freeB)),
                           formatBytes(static_cast<unsigned long long>(totalB))));
        float used = 100.0f * static_cast<float>(totalB - freeB) / static_cast<float>(totalB);
        fill->setWidthPercentage(std::max(2.0f, std::min(100.0f, used)));
    };
    apply(lib_sdVal_, lib_sdFill_, sd_free_, sd_total_);
    apply(lib_nandVal_, lib_nandFill_, nand_free_, nand_total_);
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
    lib_sdVal_ = nullptr;
    lib_sdFill_ = nullptr;
    lib_nandVal_ = nullptr;
    lib_nandFill_ = nullptr;

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

    if (focusInSummary && brls::Application::getCurrentFocus() == nullptr) {
        if (on_defocus_) {
            on_defocus_();
        }
    }
}

void DashboardSummaryView::addHeader(const std::string& title, const std::string& meta) {
    brls::Box* row = makeRow();
    row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    row->setWidthPercentage(100.0f);
    row->setHeight(22.0f);
    row->setPadding(0.0f, 4.0f, 0.0f, 4.0f);
    row->setMarginBottom(8.0f);
    row->setFocusable(false);
    brls::Label* t = makeLabel(title, kHeaderFont, themeAccent(255));
    t->setLetterSpacing(0.8f);
    row->addView(t);
    if (!meta.empty()) row->addView(makeLabel(meta, kCaptionFont, style::textSecondary()));
    content_container_->addView(row);
}

brls::Box* DashboardSummaryView::addCardsRow() {
    brls::Box* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    row->setWidthPercentage(100.0f);
    row->setHeight(kCardHeight);
    content_container_->addView(row);
    return row;
}

void DashboardSummaryView::routeUpToTile(brls::View* view) {
    if (!get_active_tile_ || !view) return;
    if (brls::View* tile = get_active_tile_()) {
        view->setCustomNavigationRoute(brls::FocusDirection::UP, tile);
    }
}

// Up and B both take focus from a panel card back to the menu tile.
void DashboardSummaryView::wireCard(brls::View* card) {
    routeUpToTile(card);
    if (!card || !on_defocus_) return;
    card->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View*) {
        if (on_defocus_) on_defocus_();
        return true;
    });
}

// -------------------------------------------------------------
// SECTION 0: CATALOG — three newest games
// -------------------------------------------------------------
void DashboardSummaryView::buildCatalogSection() {
    std::string meta;
    if (catalog_total_ > 0) meta = brls::getStr("app/dashboard/catalog_meta_fmt", std::to_string(catalog_total_));
    addHeader("app/dashboard/catalog_title"_i18n, meta);

    brls::Box* cardsRow = addCardsRow();
    if (catalog_sample_.empty()) {
        cardsRow->setAlignItems(brls::AlignItems::CENTER);
        cardsRow->setJustifyContent(brls::JustifyContent::CENTER);
        cardsRow->addView(makeLabel("app/dashboard/catalog_loading"_i18n, kSubFont, style::textSecondary()));
        return;
    }
    for (const auto& g : catalog_sample_) {
        DashboardGameCard* card = new DashboardGameCard(g, on_defocus_);
        routeUpToTile(card);
        cardsRow->addView(card);
    }
}

// -------------------------------------------------------------
// SECTION 1: RETRO GAMES
// -------------------------------------------------------------
void DashboardSummaryView::buildRetroGamesSection() {
    auto openRetro = [this](brls::View*) {
        if (on_open_section_) on_open_section_(1);
        return true;
    };

    auto& retroCatalog = catalog::RetroCatalogManager::instance();
    const int systems = static_cast<int>(retroCatalog.consoles().size());
    const int retroGames = retroCatalog.getTotalCachedGamesCount();
    int ticoSystems = 0;
    for (const auto& c : retroCatalog.consoles()) {
        if (c.route == catalog::RetroRoute::Tico) ++ticoSystems;
    }

    addHeader("app/dashboard/retro_title"_i18n, "app/dashboard/retro_meta"_i18n);
    brls::Box* cardsRow = addCardsRow();

    // Games in the database
    brls::Box* c1 = makeCard(kCardWidth, openRetro);
    brls::Box* b1 = c1;
    addCaption(b1, "app/dashboard/retro_c1_title"_i18n);
    if (retroGames > 0) addIconValue(b1, Icon::Gamepad, std::to_string(retroGames), style::textPrimary());
    else addIconValue(b1, Icon::Gamepad, "app/dashboard/retro_c1_val_empty"_i18n, style::textSecondary(), kValueFontMid);
    addSub(b1, util::pluralize("app/plural/systems", systems), style::textSecondary());
    cardsRow->addView(c1);

    // Platforms handled by Tico
    brls::Box* c2 = makeCard(kCardWidth, openRetro);
    brls::Box* b2 = c2;
    addCaption(b2, "app/dashboard/retro_c2_title"_i18n);
    addIconValue(b2, Icon::Puzzle, ofStr(std::to_string(ticoSystems), std::to_string(systems)), style::textPrimary());
    addSub(b2, "app/dashboard/retro_c2_sub"_i18n, style::textSecondary());
    cardsRow->addView(c2);

    // ROM folder
    auto& cfg = config::ConfigManager::instance();
    std::string modeTag = cfg.isRetroCustomFolder() ? "app/dashboard/retro_mode_custom"_i18n : std::string("Tico");
    std::string effDir = (cfg.isRetroCustomFolder() && !cfg.getRetroCustomPath().empty())
        ? cfg.getRetroCustomPath() : std::string("sdmc:/tico/roms/");
    brls::Box* c3 = makeCard(kCardWidth, openRetro);
    brls::Box* b3 = c3;
    addCaption(b3, "app/dashboard/retro_c3_title"_i18n + "  \u00B7  " + modeTag);
    addIconValue(b3, Icon::Folder, effDir, style::textPrimary(), kValueFontMid);
    addSub(b3, "app/dashboard/retro_c3_sub"_i18n, style::textSecondary());
    cardsRow->addView(c3);

    wireCard(c1);
    wireCard(c2);
    wireCard(c3);
}

// -------------------------------------------------------------
// SECTION 2: GAME MANAGER — installed, updates, free space
// -------------------------------------------------------------
void DashboardSummaryView::buildLibrarySection() {
    auto openLib = [this](brls::View*) {
        if (on_open_section_) on_open_section_(2);
        return true;
    };

    addHeader("app/dashboard/library_title"_i18n, "");
    brls::Box* cardsRow = addCardsRow();

    brls::Box* c1 = makeCard(kCardWidth, openLib);
    brls::Box* b1 = c1;
    addCaption(b1, "app/dashboard/library_inst_title"_i18n);
    addIconValue(b1, Icon::GameCards, util::pluralize("app/plural/games", installed_count_), style::textPrimary());
    addSub(b1, "app/dashboard/library_inst_sub"_i18n, style::textSecondary());
    cardsRow->addView(c1);

    brls::Box* c2 = makeCard(kCardWidth, openLib);
    if (updates_count_ > 0) {
        c2->setBorderColor(style::stateWarnBorder());
        brls::Box* b2 = c2;
        addCaption(b2, "app/dashboard/library_upd_title"_i18n);
        addIconValue(b2, Icon::Update, util::pluralize("app/plural/updates", updates_count_), style::stateWarn());
        addSub(b2, "app/dashboard/library_upd_sub"_i18n, style::textSecondary());
    } else {
        brls::Box* b2 = c2;
        addCaption(b2, "app/dashboard/library_upd_title"_i18n);
        addIconValue(b2, Icon::Done, "app/dashboard/library_upd_all_updated"_i18n, style::stateOk(), kValueFontMid);
        addSub(b2, "app/dashboard/library_upd_sub_ok"_i18n, style::textSecondary());
    }
    cardsRow->addView(c2);

    brls::Box* c3card = makeCard(kCardWidth, openLib);
    addCaption(c3card, "app/dashboard/library_free_title"_i18n);
    brls::Box* c3row = makeRow();
    c3row->setWidthPercentage(100.0f);
    DashboardIcon* c3icon = new DashboardIcon(Icon::SdCard, kEmblem);
    c3icon->setMarginRight(kEmblemGap);
    c3row->addView(c3icon);
    brls::Box* c3 = new brls::Box();
    c3->setAxis(brls::Axis::COLUMN);
    c3->setWidth(kCardInnerW - kEmblem - kEmblemGap);
    c3row->addView(c3);
    c3card->addView(c3row);
    c3card->addView(new brls::Box()); // keeps the bars centered between caption and bottom
    auto addStorageRow = [&](const std::string& name, brls::Label*& valOut, brls::Box*& fillOut) {
        brls::Box* labels = makeRow();
        labels->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        labels->setWidthPercentage(100.0f);

        labels->addView(makeLabel(name, kSubFont, style::textPrimary()));
        valOut = makeLabel("—", kSubFont, style::textSecondary());
        labels->addView(valOut);
        c3->addView(labels);

        brls::Box* barRow = makeRow();
        barRow->setWidthPercentage(100.0f);
        barRow->setMarginTop(6.0f);
        barRow->setMarginBottom(10.0f);
        fillOut = makeBar(barRow, 8.0f, 0.0f);
        c3->addView(barRow);
    };
    addStorageRow("app/dashboard/library_free_sd"_i18n, lib_sdVal_, lib_sdFill_);
    addStorageRow("NAND", lib_nandVal_, lib_nandFill_);
    cardsRow->addView(c3card);
    updateStorageWidgets();

    wireCard(c1);
    wireCard(c2);
    wireCard(c3card);
}

// -------------------------------------------------------------
// SECTION 3: DOWNLOADS — active download, or queue / last / network
// -------------------------------------------------------------
void DashboardSummaryView::buildDownloadsSection() {
    auto openDl = [this](brls::View*) {
        if (on_open_section_) on_open_section_(3);
        return true;
    };

    const std::string queueMeta = "app/dashboard/downloads_queue_prefix"_i18n + std::to_string(countWaiting(cached_downloads_));
    const download::DownloadItem* activeItem = findActiveDownload(cached_downloads_);

    if (activeItem) {
        dl_active_mode_ = true;
        dl_active_topic_id_ = activeItem->topic_id;
        dl_active_title_ = activeItem->title;

        addHeader("app/dashboard/downloads_title"_i18n, "");
        brls::Box* cardsRow = addCardsRow();

        // Wide card: cover, title, progress, three metrics
        const float wideW = kCardWidth * 2 + (1184.0f - 3 * kCardWidth) / 2.0f;
        brls::Box* mainCard = makeCard(wideW, openDl);
        mainCard->setAxis(brls::Axis::ROW);
        mainCard->setJustifyContent(brls::JustifyContent::FLEX_START);
        mainCard->setPadding(kCardPadV, kCardPadH, kCardPadV, kCardPadV);

        brls::Box* coverBox = new brls::Box();
        coverBox->setWidth(kCoverW);
        coverBox->setHeight(kCoverH);
        coverBox->setCornerRadius(6.0f);
        coverBox->setBackgroundColor(style::isLight() ? nvgRGBA(210, 225, 240, 180) : nvgRGBA(15, 25, 38, 140));
        coverBox->setMarginRight(18.0f);
        coverBox->setShrink(0.0f);

        dl_coverImg_ = new brls::Image();
        dl_coverImg_->setWidth(kCoverW);
        dl_coverImg_->setHeight(kCoverH);
        dl_coverImg_->setCornerRadius(6.0f);
        dl_coverImg_->setScalingType(brls::ImageScalingType::FILL);
        dl_coverImg_->setImageFromFile("romfs:/img/cover_blank.png");
        coverBox->addView(dl_coverImg_);
        mainCard->addView(coverBox);

        const float detailsW = wideW - kCardPadV - kCardPadH - kCoverW - 18.0f;
        brls::Box* details = new brls::Box();
        details->setAxis(brls::Axis::COLUMN);
        details->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        details->setWidth(detailsW);
        details->setHeight(kCoverH);

        brls::Box* topRow = makeRow();
        topRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        topRow->setWidthPercentage(100.0f);
        dl_titleLbl_ = makeLabel("", 22.0f, style::textPrimary());
        dl_titleLbl_->setWidth(detailsW * 0.6f);
        topRow->addView(dl_titleLbl_);
        dl_stLbl_ = makeLabel("", kSubFont, style::textSecondary());
        topRow->addView(dl_stLbl_);
        details->addView(topRow);

        brls::Box* barRow = makeRow();
        barRow->setWidthPercentage(100.0f);
        dl_barFill_ = makeBar(barRow, 10.0f, activeItem->progress * 100.0f);
        dl_pctLbl_ = makeLabel("", 22.0f, style::textPrimary());
        dl_pctLbl_->setMarginLeft(14.0f);
        barRow->addView(dl_pctLbl_);
        details->addView(barRow);

        brls::Box* metrics = makeRow();
        metrics->setWidthPercentage(100.0f);
        metrics->setAlignItems(brls::AlignItems::FLEX_START);
        auto addMetric = [&](const std::string& caption) {
            brls::Box* col = new brls::Box();
            col->setAxis(brls::Axis::COLUMN);
            col->setWidth(detailsW / 3.0f);
            col->addView(makeLabel(caption, kCaptionFont, style::textSecondary()));
            brls::Label* v = makeLabel("", 18.0f, style::textPrimary());
            v->setMarginTop(2.0f);
            v->setWidth(detailsW / 3.0f - 8.0f);
            col->addView(v);
            metrics->addView(col);
            return v;
        };
        dl_spdLbl_ = addMetric("app/dashboard/downloads_cap_speed"_i18n);
        dl_szLbl_  = addMetric("app/dashboard/downloads_cap_written"_i18n);
        dl_etaLbl_ = addMetric("app/dashboard/downloads_cap_eta"_i18n);
        details->addView(metrics);
        mainCard->addView(details);
        cardsRow->addView(mainCard);

        // Speed graph card
        brls::Box* graphCard = makeCard(kCardWidth, openDl);
        brls::Box* gTop = makeRow();
        gTop->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        gTop->setWidthPercentage(100.0f);
        gTop->addView(makeLabel("app/dashboard/downloads_speed_graph"_i18n, kCaptionFont, style::textSecondary()));
        dl_qCountLbl_ = makeLabel(queueMeta, kCaptionFont, style::textSecondary());
        gTop->addView(dl_qCountLbl_);
        graphCard->addView(gTop);

        dl_sparkline_ = new SpeedSparklineView();
        dl_sparkline_->setWidthPercentage(100.0f);
        dl_sparkline_->setHeight(64.0f);
        graphCard->addView(dl_sparkline_);

        dl_peersLbl_ = addSub(graphCard, "", style::textSecondary());
        cardsRow->addView(graphCard);

        updateActiveDownloadWidgets(*activeItem);

        wireCard(mainCard);
        wireCard(graphCard);
        return;
    }

    // Idle: queue state, last finished download, network
    dl_active_mode_ = false;
    dl_idle_signature_ = idleDownloadsSignature();

    addHeader("app/dashboard/downloads_title"_i18n, "");
    brls::Box* cardsRow = addCardsRow();

    brls::Box* c1 = makeCard(kCardWidth, openDl);
    brls::Box* b1 = c1;
    addCaption(b1, "app/dashboard/downloads_active_title"_i18n);
    addIconValue(b1, Icon::Download, "app/dashboard/downloads_none"_i18n, style::textPrimary());
    addSub(b1, countWaiting(cached_downloads_) > 0 ? queueMeta : "app/dashboard/downloads_none_sub"_i18n,
           style::textSecondary());
    cardsRow->addView(c1);

    brls::Box* c2 = makeCard(kCardWidth, openDl);
    if (const download::DownloadItem* last = findLastCompleted(cached_downloads_)) {
        brls::Box* b2 = c2;
        addCaption(b2, "app/dashboard/downloads_last_title"_i18n);
        addIconValue(b2, Icon::Done, displayTitle(last->title), style::textPrimary(), kValueFontMid);
        std::string sub = "app/dashboard/downloads_last_done"_i18n;
        if (last->download_total_bytes > 0) sub += "  ·  " + formatBytes(last->download_total_bytes);
        addSub(b2, sub, style::stateOk());
    } else {
        brls::Box* b2 = c2;
        addCaption(b2, "app/dashboard/downloads_last_title"_i18n);
        addIconValue(b2, Icon::Clock, "app/dashboard/downloads_last_none"_i18n, style::textSecondary(), kValueFontMid);
        addSub(b2, "app/dashboard/downloads_last_none_sub"_i18n, style::textSecondary());
    }
    cardsRow->addView(c2);

    auto& cfg = config::ConfigManager::instance();
    const std::string engine = (cfg.getDataMode() == "local_client") ? "Custom Engine" : "TorrServer";
    brls::Box* c3 = makeCard(kCardWidth, openDl);
    brls::Box* b3 = c3;
    addCaption(b3, "app/dashboard/downloads_net_title"_i18n + "  \u00B7  " + engine);
    addIconValue(b3, Icon::Router, brls::getStr("app/dashboard/port_fmt", std::to_string(cfg.getListenPort())), style::textPrimary());
    addSub(b3, "DHT  ·  " + (cfg.getKeepAwakeDuringDownloads() ? "app/dashboard/keep_awake_on"_i18n
                                                               : "app/dashboard/keep_awake_off"_i18n), style::textSecondary());
    cardsRow->addView(c3);

    wireCard(c1);
    wireCard(c2);
    wireCard(c3);
}

// -------------------------------------------------------------
// SECTION 4: SYSTEM — engine, cache, unfinished installs
// -------------------------------------------------------------
void DashboardSummaryView::buildToolsSection() {
    auto openTools = [this](brls::View*) {
        if (on_open_section_) on_open_section_(4);
        return true;
    };

    addHeader("app/dashboard/tools_title"_i18n, "");
    brls::Box* cardsRow = addCardsRow();

    auto& cfg = config::ConfigManager::instance();
    brls::Box* c1 = makeCard(kCardWidth, openTools);
    brls::Box* b1 = c1;
    addCaption(b1, "app/dashboard/tools_c1_title"_i18n);
    addIconValue(b1, Icon::Chip, engine_mode_, style::textPrimary(), kValueFontMid + 4.0f);
    addSub(b1, brls::getStr("app/dashboard/port_fmt", std::to_string(cfg.getListenPort())) + "  ·  " +
               (cfg.getKeepAwakeDuringDownloads() ? "app/dashboard/keep_awake_on"_i18n
                                                  : "app/dashboard/keep_awake_off"_i18n),
           style::textSecondary());
    cardsRow->addView(c1);

    brls::Box* c2 = makeCard(kCardWidth, openTools);
    brls::Box* b2 = c2;
    addCaption(b2, "app/dashboard/tools_c2_title"_i18n);
    addIconValue(b2, Icon::Database, formatBytes(cache_size_bytes_), style::textPrimary());
    addSub(b2, "app/dashboard/tools_c2_sub"_i18n, style::textSecondary());
    cardsRow->addView(c2);

    brls::Box* c3 = makeCard(kCardWidth, openTools);
    if (leftover_size_bytes_ > 0) {
        c3->setBorderColor(style::stateWarnBorder());
        brls::Box* b3 = c3;
        addCaption(b3, "app/dashboard/tools_c3_title"_i18n);
        addIconValue(b3, Icon::Trash, formatBytes(leftover_size_bytes_), style::stateWarn());
        addSub(b3, "app/dashboard/tools_c3_sub"_i18n, style::textSecondary());
    } else {
        brls::Box* b3 = c3;
        addCaption(b3, "app/dashboard/tools_c3_title"_i18n);
        addIconValue(b3, Icon::Done, "app/dashboard/tools_c3_clean"_i18n, style::stateOk());
        addSub(b3, "app/dashboard/tools_c3_sub_clean"_i18n, style::textSecondary());
    }
    cardsRow->addView(c3);

    wireCard(c1);
    wireCard(c2);
    wireCard(c3);
}

void DashboardSummaryView::draw(NVGcontext* vg, float x, float y, float width, float height,
                                brls::Style style, brls::FrameContext* ctx) {
    bool isLight = style::isLight();
    NVGcolor accent = ThemeManager::instance().getAccentColor();

    // 1. Frosted glass base, tinted with the active theme color
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 16.0f);
    NVGpaint bgPaint;
    if (isLight) {
        bgPaint = nvgLinearGradient(vg, x, y, x, y + height,
                                    nvgRGBA(255, 255, 255, 170),
                                    nvgRGBA(242, 246, 252, 150));
    } else {
        bgPaint = nvgLinearGradient(vg, x, y, x, y + height,
                                    nvgRGBA(accent.r * 255 * 0.35f, accent.g * 255 * 0.35f, accent.b * 255 * 0.35f, 40),
                                    nvgRGBA(8, 14, 22, 90));
    }
    nvgFillPaint(vg, bgPaint);
    nvgFill(vg);

    // 2. Border
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 16.0f);
    NVGpaint borderPaint;
    if (isLight) {
        borderPaint = nvgLinearGradient(vg, x, y, x, y + height,
                                        nvgRGBA(210, 220, 232, 200),
                                        nvgRGBA(185, 198, 215, 150));
    } else {
        borderPaint = nvgLinearGradient(
            vg, x, y, x, y + height,
            nvgRGBA(std::min<int>(255, accent.r * 255 * 0.7f + 40),
                    std::min<int>(255, accent.g * 255 * 0.7f + 40),
                    std::min<int>(255, accent.b * 255 * 0.7f + 40), 95),
            nvgRGBA(accent.r * 255 * 0.3f, accent.g * 255 * 0.3f, accent.b * 255 * 0.3f, 30));
    }
    nvgStrokePaint(vg, borderPaint);
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);

    // 3. Cards
    Box::draw(vg, x, y, width, height, style, ctx);
}

} // namespace ui
