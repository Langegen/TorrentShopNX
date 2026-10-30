#include "GameDetailView.hpp"
#include "style/UiTokens.hpp"
#include "../utils/string_utils.h"
#include "ThemeManager.hpp"
#include "FileSelectView.hpp"
#include "DownloadUiManager.hpp"
#include "DownloadsView.hpp"
#include "FavoritesManager.hpp"
#include "ScreenshotViewer.hpp"
#include "QrCodeView.hpp"
#include "PortInstallFlow.hpp"
#include "../catalog/filter_manager.hpp"
#include "../catalog/retro_catalog_manager.h"
#include "../catalog/installed_retro_games.h"
#include "../catalog/release_finder.h"
#include "../catalog/titledb.h"
#include "../config/config.h"
#include "../installer/PortInstallPlanner.hpp"
#include "../utils/file_ops.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace ui {

namespace {

// Download button of a game that is already on the console: green, so the
// card reads "you have this" before the label is read.
const brls::ButtonStyle BUTTONSTYLE_INSTALLED = {
    .shadowType              = brls::ShadowType::GENERIC,
    .hideHighlightBackground = true,

    .highlightPadding = "brls/button/primary_highlight_padding",
    .borderThickness  = "",

    .enabledBackgroundColor = "tsnx/button/installed_background",
    .enabledLabelColor      = "tsnx/button/installed_text",
    .enabledBorderColor     = "",

    .disabledBackgroundColor = "brls/button/primary_disabled_background",
    .disabledLabelColor      = "brls/button/primary_disabled_text",
    .disabledBorderColor     = "",
};

// " v1.0.3 " -> "1.0.3"
std::string trimVersion(std::string v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    if (v.size() > 1 && (v[0] == 'v' || v[0] == 'V') && std::isdigit(static_cast<unsigned char>(v[1]))) v.erase(0, 1);
    return v;
}

// Compares "1.0.10" with "1.0.9" part by part: -1, 0 or 1; 2 when either is
// not a dotted number.
int compareDotted(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s, std::vector<unsigned long>& out) {
        size_t i = 0;
        while (i < s.size()) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
            unsigned long n = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) n = n * 10 + (s[i++] - '0');
            out.push_back(n);
            if (i < s.size() && s[i] != '.') break;   // "1.0.3b", "1.2 (rev)": compare the number part
            ++i;
        }
        return !out.empty();
    };
    std::vector<unsigned long> pa, pb;
    if (!parse(a, pa) || !parse(b, pb)) return 2;
    size_t n = std::max(pa.size(), pb.size());
    pa.resize(n, 0);
    pb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

// Version of the game in the release: "1.0.3", "1.0.3 (update 3)" or
// "update 3"; "" when the catalog does not know it.
std::string releaseVersionText(const Game& g) {
    std::string v = trimVersion(g.version);
    if (g.update_version) {
        std::string upd = brls::getStr("app/library/update_label",
                                       std::to_string(titledb::updateOrdinal(g.update_version)));
        return v.empty() ? upd : v + " (" + upd + ")";
    }
    return v;
}

} // namespace

class ScrollAndFocusController : public brls::View {
public:
    ScrollAndFocusController(brls::ScrollingFrame* targetScroll,
                             brls::Button* btnDownload,
                             brls::Button* btnFavorite,
                             brls::Button* btnQr)
        : targetScroll_(targetScroll),
          btnDownload_(btnDownload),
          btnFavorite_(btnFavorite),
          btnQr_(btnQr) {
        setHeight(0);
        setWidth(0);
    }
    
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override {
        if (!targetScroll_) return;
        
        brls::View* currentFocus = brls::Application::getCurrentFocus();
        if (!currentFocus) return;
        
        // Focus is in right column if it is NOT on the left action buttons
        bool inRightColumn = (currentFocus != btnDownload_ &&
                              currentFocus != btnFavorite_ &&
                              currentFocus != btnQr_);
                              
        if (inRightColumn) {
            auto& state = brls::Application::getControllerState();
            float leftY = state.axes[brls::LEFT_Y];
            float rightY = state.axes[brls::RIGHT_Y];
            
            float currentOffset = targetScroll_->getContentOffsetY();
            float contentHeight = 0;
            if (!targetScroll_->getChildren().empty()) {
                contentHeight = targetScroll_->getChildren().front()->getHeight();
            }
            float viewHeight = targetScroll_->getHeight();
            float maxOffset = contentHeight - viewHeight;
            if (maxOffset < 0) maxOffset = 0;
            
            float delta = 0.0f;
            // Both Left stick Y and Right stick Y scroll smoothly when in right column
            if (std::abs(leftY) > 0.15f) {
                delta += leftY * 14.0f;
            } else if (std::abs(rightY) > 0.15f) {
                delta += rightY * 14.0f;
            }
            
            if (delta != 0.0f) {
                float newOffset = currentOffset + delta;
                if (newOffset < 0) newOffset = 0;
                if (newOffset > maxOffset) newOffset = maxOffset;
                if (newOffset != currentOffset) {
                    targetScroll_->setContentOffsetY(newOffset, false);
                }
            }
        }
    }
    
private:
    brls::ScrollingFrame* targetScroll_;
    brls::Button* btnDownload_;
    brls::Button* btnFavorite_;
    brls::Button* btnQr_;
};

GameDetailView::GameDetailView(const Game& game, const std::string& retro_console_id)
    : game_(game), retro_console_id_(retro_console_id) {
}

GameDetailView::~GameDetailView() {
    if (imageToken) *imageToken = false;
    if (alive_) *alive_ = false;
}

void GameDetailView::onContentAvailable() {
    imageToken = std::make_shared<bool>(true);
    alive_ = std::make_shared<bool>(true);
    // Fill text and images
    title->setText(cleanTitle(game_.title));
    setImageFromHTTPS(cover, game_.cover, imageToken, "romfs:/img/cover_blank.png", false, "", -1, -1, 2000000);
    
    // Characteristics: caption above value, two per row. Fields without a
    // value are left out instead of showing "Unknown".
    struct MetaItem { std::string caption, value; bool wide; };
    std::vector<MetaItem> meta;
    auto addMeta = [&meta](const char* captionKey, const std::string& value, bool wide = false) {
        if (!util::isPlaceholderValue(value)) meta.push_back({brls::getStr(captionKey), value, wide});
    };
    addMeta("app/detail_meta/developer", game_.developer);
    addMeta("app/detail_meta/publisher", game_.publisher);
    addMeta("app/detail_meta/release", game_.year);
    addMeta("app/detail_meta/voice", game_.voice_lang);

    const auto* statsData = catalog::GameStatsManager::instance().getAnyStats(game_.topic_id);
    if (statsData && (statsData->seeds >= 0 || statsData->downloads >= 0)) {
        addMeta("app/detail_meta/torrent",
                brls::getStr("app/detail_meta/torrent_value",
                             std::to_string(std::max(0, statsData->seeds)),
                             std::to_string(std::max(0, statsData->leeches)),
                             std::to_string(std::max<int64_t>(0, statsData->downloads))));
        // "2021-06-11 19:34:29" -> "11.06.2021"
        std::string added = statsData->registered_at;
        if (added.size() >= 10 && added[4] == '-' && added[7] == '-') {
            added = added.substr(8, 2) + "." + added.substr(5, 2) + "." + added.substr(0, 4);
        }
        addMeta("app/detail_meta/added", added);
    }
    addMeta("app/detail_meta/version", releaseVersionText(game_));
    // Long ("NSZ, compressed ~58%, installed 0.08 GB"): last, on a row of its own
    addMeta("app/detail_meta/format", game_.image_format, game_.image_format.size() > 32);

    if (metaBox) {
        metaBox->clearViews();
        if (meta.empty()) {
            metaBox->setVisibility(brls::Visibility::GONE);
        } else {
            metaBox->setVisibility(brls::Visibility::VISIBLE);
            metaBox->setBackgroundColor(style::cardBg());
            metaBox->setBorderThickness(1.0f);
            metaBox->setBorderColor(style::cardBorder());
            brls::Box* row = nullptr;
            int inRow = 2;
            for (size_t i = 0; i < meta.size(); ++i) {
                if (inRow >= 2 || meta[i].wide) {
                    inRow = 0;
                    row = new brls::Box();
                    row->setAxis(brls::Axis::ROW);
                    row->setWidthPercentage(100.0f);
                    row->setMarginBottom(14.0f);
                    metaBox->addView(row);
                }
                brls::Box* cell = new brls::Box();
                cell->setAxis(brls::Axis::COLUMN);
                cell->setWidthPercentage(meta[i].wide ? 100.0f : 50.0f);
                inRow += meta[i].wide ? 2 : 1;
                cell->setPaddingRight(16.0f);

                brls::Label* cap = new brls::Label();
                cap->setText(meta[i].caption);
                cap->setFontSize(style::kFontCaption);
                cap->setTextColor(style::textSecondary());
                cap->setSingleLine(true);
                cell->addView(cap);

                brls::Label* val = new brls::Label();
                val->setText(meta[i].value);
                val->setLineHeight(1.25f);
                val->setFontSize(style::kFontBody + 1.0f);
                val->setTextColor(style::textPrimary());
                val->setMarginTop(3.0f);
                val->setMaxLines(2);
                val->setAutoAnimate(false);
                cell->addView(val);
                row->addView(cell);
            }
        }
    }

    if (coverFrame) {
        coverFrame->setShadowType(brls::ShadowType::GENERIC);
        coverFrame->setShadowVisibility(true);
    }

    // Clean description (remove leading ': ')
    std::string desc = game_.description;
    if (desc.size() >= 2 && desc.substr(0, 2) == ": ") {
        desc = desc.substr(2);
    }
    if (util::isPlaceholderValue(desc)) {
        description->setVisibility(brls::Visibility::GONE);
        if (descriptionHeader) descriptionHeader->setVisibility(brls::Visibility::GONE);
    } else {
        description->setText(desc);
    }
    
    // Dynamic Badges (Console, Genres & Languages)
    if (!retro_console_id_.empty()) {
        const auto* cInfo = catalog::RetroCatalogManager::instance().getConsole(retro_console_id_);
        brls::Box* cBadge = new brls::Box();
        cBadge->setPadding(5, 10, 5, 10);
        cBadge->setMarginRight(10);
        cBadge->setMarginBottom(10);
        NVGcolor col = cInfo ? cInfo->color : nvgRGB(80, 80, 80);
        cBadge->setBackgroundColor(col);
        cBadge->setCornerRadius(6);

        brls::Label* cLabel = new brls::Label();
        cLabel->setText(cInfo ? cInfo->name : retro_console_id_);
        cLabel->setFontSize(14);
        cLabel->setTextColor(nvgRGB(255, 255, 255));
        cBadge->addView(cLabel);

        badgesBox->addView(cBadge);

        bool isTico = catalog::RetroCatalogManager::instance().getRoute(retro_console_id_) == catalog::RetroRoute::Tico;
        std::string emuText = catalog::RetroCatalogManager::instance().getRunnerLabel(retro_console_id_);

        if (!emuText.empty()) {
            brls::Box* emuBadge = new brls::Box();
            emuBadge->setPadding(5, 10, 5, 10);
            emuBadge->setMarginRight(10);
            emuBadge->setMarginBottom(10);
            emuBadge->setBackgroundColor(isTico ? nvgRGB(124, 77, 255) : nvgRGB(50, 120, 200));
            emuBadge->setCornerRadius(6);

            brls::Label* emuLabel = new brls::Label();
            emuLabel->setText(emuText);
            emuLabel->setFontSize(14);
            emuLabel->setTextColor(nvgRGB(255, 255, 255));
            emuBadge->addView(emuLabel);

            badgesBox->addView(emuBadge);
        }

        if (isRomsetGame(game_)) {
            brls::Box* setBadge = new brls::Box();
            setBadge->setPadding(5, 10, 5, 10);
            setBadge->setMarginRight(10);
            setBadge->setMarginBottom(10);
            setBadge->setBackgroundColor(nvgRGB(230, 130, 20)); // Amber
            setBadge->setCornerRadius(6);

            brls::Label* setLabel = new brls::Label();
            setLabel->setText("app/detail/compilation_badge"_i18n);
            setLabel->setFontSize(14);
            setLabel->setTextColor(nvgRGB(255, 255, 255));
            setBadge->addView(setLabel);

            badgesBox->addView(setBadge);

            btnDownload->setText("app/detail/download_romset_btn"_i18n);
        } else {
            btnDownload->setText("app/detail/download_rom"_i18n);
        }
    }

    // Port installed by the auto-installer (has a manifest)
    if ((isHomebrewGame(game_) || isHomebrewPort(game_)) && ui::isPortInstalled(game_)) {
        btnDownload->setText("app/ports/installed_btn"_i18n);
    }

    // Add language badge
    std::string lang = extractLangBadge(game_.interface_lang);
    if (!lang.empty()) {
        brls::Box* langBadge = new brls::Box();
        langBadge->setPadding(5, 10, 5, 10);
        langBadge->setMarginRight(10);
        langBadge->setMarginBottom(10);
        langBadge->setBackgroundColor(nvgRGB(233, 30, 99)); // Pink lang badge
        langBadge->setCornerRadius(6);

        brls::Label* langLabel = new brls::Label();
        langLabel->setText(lang);
        langLabel->setFontSize(14);
        langLabel->setTextColor(nvgRGB(255, 255, 255));
        langBadge->addView(langLabel);

        badgesBox->addView(langBadge);
    }
    
    // Add genre badges
    std::vector<std::string> displayBadges = catalog::getDisplayGenreBadges(game_.genre);
    for (const auto& genreTag : displayBadges) {
        if (util::isPlaceholderValue(genreTag)) continue;
        brls::Box* gBadge = new brls::Box();
        gBadge->setPadding(5, 10, 5, 10);
        gBadge->setMarginRight(10);
        gBadge->setMarginBottom(10);
        gBadge->setBackgroundColor(ThemeManager::instance().getAccentDimColor());
        gBadge->setBorderThickness(1.0f);
        gBadge->setBorderColor(ThemeManager::instance().getCardBorderColor());
        gBadge->setCornerRadius(6);

        brls::Label* gLabel = new brls::Label();
        gLabel->setText(genreTag);
        gLabel->setFontSize(14);
        gLabel->setTextColor(ThemeManager::instance().getAccentColor());
        gBadge->addView(gLabel);

        badgesBox->addView(gBadge);
    }

    // Add multiplayer badge
    if (!game_.multiplayer.empty()) {
        std::string mp = game_.multiplayer;
        std::string lowerMp = catalog::toLowerUtf8(mp);
        if (lowerMp != "\xd0\xbd\xd0\xb5\xd1\x82" && lowerMp != "no" && lowerMp != "1" && !util::isPlaceholderValue(mp)) {
            brls::Box* mpBadge = new brls::Box();
            mpBadge->setPadding(5, 10, 5, 10);
            mpBadge->setMarginRight(10);
            mpBadge->setMarginBottom(10);
            mpBadge->setBackgroundColor(style::isLight() ? nvgRGB(232, 236, 242) : nvgRGBA(255, 255, 255, 20));
            mpBadge->setBorderThickness(1.0f);
            mpBadge->setBorderColor(style::cardBorder());
            mpBadge->setCornerRadius(6);

            brls::Label* mpLabel = new brls::Label();
            mpLabel->setText(std::string("\uE7FB ") + mp); // people icon + player count
            mpLabel->setFontSize(14);
            mpLabel->setTextColor(style::textSecondary());
            mpBadge->addView(mpLabel);

            badgesBox->addView(mpBadge);
        }
    }
    
    // Screenshots Horizontal Scroll
    brls::View* firstScr = nullptr;
    if (game_.screenshots.empty()) {
        screenshotsContainer->setVisibility(brls::Visibility::GONE);
    } else {
        screenshotsContainer->setVisibility(brls::Visibility::VISIBLE);
        for (size_t i = 0; i < game_.screenshots.size(); ++i) {
            const auto& scrUrl = game_.screenshots[i];
            if (scrUrl.empty()) continue;
            brls::Image* scrImg = new brls::Image();
            scrImg->setWidth(240); // 16:9 ratio
            scrImg->setHeight(135);
            scrImg->setMarginRight(15);
            scrImg->setScalingType(brls::ImageScalingType::FILL);
            scrImg->setCornerRadius(6);
            scrImg->setFocusable(true);
            scrImg->registerClickAction([this, i](brls::View* view) {
                brls::Application::pushActivity(new ScreenshotViewer(game_.screenshots, i));
                return true;
            });
            
            // Allow D-pad down/up to scroll the description while on any screenshot
            scrImg->registerAction("", brls::ControllerButton::BUTTON_DOWN, [this](brls::View*) {
                float contentHeight = (!scroll->getChildren().empty()) ? scroll->getChildren().front()->getHeight() : 0;
                float maxOffset = contentHeight - scroll->getHeight();
                if (maxOffset > 0) {
                    float currentOffset = scroll->getContentOffsetY();
                    float newOffset = std::min(maxOffset, currentOffset + 60.0f);
                    scroll->setContentOffsetY(newOffset, true);
                }
                return true;
            }, true, true, brls::SOUND_NONE);

            scrImg->registerAction("", brls::ControllerButton::BUTTON_UP, [this](brls::View*) {
                float currentOffset = scroll->getContentOffsetY();
                if (currentOffset > 0) {
                    float newOffset = std::max(0.0f, currentOffset - 60.0f);
                    scroll->setContentOffsetY(newOffset, true);
                }
                return true;
            }, true, true, brls::SOUND_NONE);

            setImageFromHTTPS(scrImg, scrUrl, imageToken, "romfs:/img/cover_blank.png", false, "", -1, -1, 1900000 - static_cast<int>(i) * 10);
            screenshotsBox->addView(scrImg);
            if (!firstScr) {
                firstScr = scrImg;
            }
        }
    }
    
    // Buttons Actions
    const bool isPortRelease = retro_console_id_.empty() && (isHomebrewGame(game_) || isHomebrewPort(game_));
    isSwitchRelease_ = retro_console_id_.empty() && !isPortRelease && catalog::releaseAppId(game_) != 0;
    if (isPortRelease || !retro_console_id_.empty()) {
        // Ports and retro games: picking files by hand is the advanced path, kept on X.
        btnDownload->registerAction("app/detail/select_files_btn"_i18n, brls::ControllerButton::BUTTON_X,
                                    [this](brls::View*) {
            brls::Application::pushActivity(new FileSelectView(game_, retro_console_id_));
            return true;
        });
    }

    btnDownload->registerClickAction([this, isPortRelease](brls::View* view) {
        // Ports: "Download" starts the installer straight away (file list ->
        // version -> where files go). "Files only" is offered in that preview.
        if (isPortRelease) {
            if (!ui::isPortInstalled(game_)) {
                ui::startPortInstallFlow(game_);
                return true;
            }
            auto* dialog = new brls::Dialog(cleanTitle(game_.title));
            dialog->setCancelable(true);
            dialog->addButton("app/ports/update_btn"_i18n, [this]() { ui::startPortInstallFlow(game_); });
            dialog->addButton("app/ports/uninstall_btn"_i18n, [this]() { ui::confirmPortUninstall(game_); });
            dialog->addButton("app/common/cancel"_i18n, []() {});
            dialog->open();
            return true;
        }

        if (!retro_console_id_.empty()) {
            // Retro games: one button. Folder, format and emulator are chosen by the console.
            auto startDownload = [this]() {
                ui::DownloadManager::instance().addDownload(game_, {}, -1, "", retro_console_id_);
                brls::sync([]() {
                    brls::Application::pushActivity(new ui::DownloadsView());
                });
            };
            if (!catalog::InstalledRetroGames::instance().isPresentOnCard(game_.topic_id)) {
                startDownload();
                return true;
            }
            auto* dialog = new brls::Dialog(cleanTitle(game_.title) + "\n\n" + "app/retro/game_on_sd"_i18n);
            dialog->setCancelable(true);
            dialog->addButton("app/retro/btn_download_again"_i18n, startDownload);
            dialog->addButton("app/retro/btn_remove_from_sd"_i18n, [this]() {
                brls::sync([this]() { confirmRetroRemoval(); });
            });
            dialog->addButton("app/common/cancel"_i18n, []() {});
            dialog->open();
            return true;
        }

        brls::Application::pushActivity(new FileSelectView(game_));
        return true;
    });
    
    btnFavorite->registerClickAction([this](brls::View* view) {
        catalog::FavoritesManager::instance().toggleFavorite(game_);
        updateFavoriteButton();
        return true;
    });
    
    btnQr->registerClickAction([this](brls::View* view) {
        if (game_.url.empty()) {
            brls::Application::notify("app/detail/qr_no_url"_i18n);
            return true;
        }

        QrDialog::open("app/detail/torrent_link_btn"_i18n, game_.url, "app/detail/qr_hint"_i18n);
        return true;
    });
    
    // Gamepad quick-favorites shortcut
    this->registerAction("app/actions/toggle_favorite"_i18n, brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
        catalog::FavoritesManager::instance().toggleFavorite(game_);
        updateFavoriteButton();
        return true;
    });
    
    // Update initial button state
    updateFavoriteButton();

    ScrollAndFocusController* scrollController = new ScrollAndFocusController(scroll, btnDownload, btnFavorite, btnQr);
    contentBox->addView(scrollController);

    // Custom navigation routes for seamless gamepad control
    btnDownload->setCustomNavigationRoute(brls::FocusDirection::DOWN, btnFavorite);
    btnFavorite->setCustomNavigationRoute(brls::FocusDirection::UP, btnDownload);
    btnFavorite->setCustomNavigationRoute(brls::FocusDirection::RIGHT, btnQr);
    btnQr->setCustomNavigationRoute(brls::FocusDirection::LEFT, btnFavorite);
    btnQr->setCustomNavigationRoute(brls::FocusDirection::UP, btnDownload);

    brls::View* rightTarget = firstScr ? firstScr : static_cast<brls::View*>(scroll);
    btnDownload->setCustomNavigationRoute(brls::FocusDirection::RIGHT, rightTarget);
    btnQr->setCustomNavigationRoute(brls::FocusDirection::RIGHT, rightTarget);

    if (firstScr) {
        firstScr->setCustomNavigationRoute(brls::FocusDirection::LEFT, btnDownload);
    }
    scroll->setCustomNavigationRoute(brls::FocusDirection::LEFT, btnDownload);

    // Explicitly set the last focused view on the activity content box to guarantee btnDownload gets default focus
    brls::Box* contentBoxView = dynamic_cast<brls::Box*>(getContentView());
    if (contentBoxView) {
        contentBoxView->setLastFocusedView(btnDownload);
    }

    // Explicitly request focus on the download button to guarantee focus is not lost or dropped
    brls::Application::giveFocus(btnDownload);
}

void GameDetailView::updateFavoriteButton() {
    bool isFav = catalog::FavoritesManager::instance().isFavorite(game_);
    if (isFav) {
        btnFavorite->setText("app/detail/remove_favorite_btn"_i18n);
    } else {
        btnFavorite->setText("app/detail/add_favorite_btn"_i18n);
    }
}

brls::View* GameDetailView::create() {
    return nullptr; // XML constructor stub (should not be used directly)
}

void GameDetailView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    scroll->resetScrollToTop();
    updateRetroDownloadButton();
    checkInstalled();   // again on return: the game may have been installed meanwhile
    brls::Application::giveFocus(btnDownload);
}

void GameDetailView::checkInstalled() {
    if (!isSwitchRelease_ || installCheckRunning_ || !alive_) return;
    installCheckRunning_ = true;
    const uint64_t appId = catalog::releaseAppId(game_);
    auto alive = alive_;
    brls::async([this, alive, appId]() {
        util::InstalledGameInfo info = util::queryInstalledGame(appId);
        brls::sync([this, alive, info]() {
            if (!*alive) return;
            installCheckRunning_ = false;
            applyInstalledState(info);
        });
    });
}

void GameDetailView::applyInstalledState(const util::InstalledGameInfo& info) {
    if (installedBadge_) {
        badgesBox->removeView(installedBadge_);
        installedBadge_ = nullptr;
    }
    if (!info.installed) {
        btnDownload->setText("app/detail/download_btn"_i18n);
        btnDownload->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        return;
    }

    const std::string installedVer = trimVersion(info.displayVersion);

    // Green chip first in the badge row: "✓ Installed · 1.0.2"
    installedBadge_ = new brls::Box();
    installedBadge_->setPadding(5, 10, 5, 10);
    installedBadge_->setMarginRight(10);
    installedBadge_->setMarginBottom(10);
    installedBadge_->setCornerRadius(6);
    installedBadge_->setBackgroundColor(style::isLight() ? nvgRGB(223, 244, 231) : nvgRGBA(110, 220, 150, 38));
    installedBadge_->setBorderThickness(1.0f);
    installedBadge_->setBorderColor(style::stateOk());
    brls::Label* badgeLabel = new brls::Label();
    badgeLabel->setText(std::string("\uE5CA ") +
                        (installedVer.empty() ? "app/detail/installed_badge"_i18n
                                              : brls::getStr("app/detail/installed_badge_ver", installedVer)));
    badgeLabel->setFontSize(14);
    badgeLabel->setTextColor(style::stateOk());
    installedBadge_->addView(badgeLabel);
    badgesBox->addView(installedBadge_, 0);

    // Does this release carry something newer than what is installed?
    // Version strings decide when both are known; the update number only
    // when they are not (it alone said "newer" for an equal 1.2.1 install).
    const std::string releaseVer = trimVersion(game_.version);
    const int cmp = (!releaseVer.empty() && !installedVer.empty()) ? compareDotted(releaseVer, installedVer) : 2;
    const bool releaseNewer = (cmp != 2) ? (cmp == 1)
                                         : (game_.update_version != 0 && game_.update_version > info.patchVersion);
    if (releaseNewer) {
        btnDownload->setText(releaseVer.empty() ? "app/detail/update_btn"_i18n
                                                : brls::getStr("app/detail/update_to_btn", releaseVer));
        btnDownload->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    } else {
        btnDownload->setText("app/detail/check_updates_btn"_i18n);
        btnDownload->setStyle(&BUTTONSTYLE_INSTALLED);
    }
}

void GameDetailView::updateRetroDownloadButton() {
    if (retro_console_id_.empty()) return;
    if (catalog::InstalledRetroGames::instance().isPresentOnCard(game_.topic_id)) {
        btnDownload->setText("app/retro/detail_on_sd_btn"_i18n);
    } else if (isRomsetGame(game_)) {
        btnDownload->setText("app/detail/download_romset_btn"_i18n);
    } else {
        btnDownload->setText("app/detail/download_btn"_i18n);
    }
}

void GameDetailView::confirmRetroRemoval() {
    std::string title = cleanTitle(game_.title);
    auto* confirm = new brls::Dialog(brls::getStr("app/retro/confirm_remove_from_sd", title));
    confirm->setCancelable(true);
    confirm->addButton("app/retro/btn_remove_from_sd"_i18n, [this, title]() {
        std::string key = catalog::InstalledRetroGames::keyFor(game_.topic_id, game_.title);
        std::string err;
        if (catalog::InstalledRetroGames::instance().uninstall(key, err)) {
            brls::Application::notify(brls::getStr("app/retro/removed_from_sd", title));
        } else {
            brls::Application::notify("app/retro/emu_delete_error"_i18n + err);
        }
        updateRetroDownloadButton();
    });
    confirm->addButton("app/common/cancel"_i18n, []() {});
    confirm->open();
}

void GameDetailView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
}

} // namespace ui
