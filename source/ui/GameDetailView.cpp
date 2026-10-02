#include "GameDetailView.hpp"
#include "ThemeManager.hpp"
#include "FileSelectView.hpp"
#include "DownloadUiManager.hpp"
#include "DownloadsView.hpp"
#include "FavoritesManager.hpp"
#include "ScreenshotViewer.hpp"
#include "QrCodeView.hpp"
#include "../catalog/filter_manager.hpp"
#include "../catalog/retro_catalog_manager.h"
#include "../config/config.h"
#include <sstream>

namespace ui {

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
}

void GameDetailView::onContentAvailable() {
    imageToken = std::make_shared<bool>(true);
    // Fill text and images
    title->setText(cleanTitle(game_.title));
    setImageFromHTTPS(cover, game_.cover, imageToken, "romfs:/img/borealis_96.png", false, "", -1, -1, 2000000);
    
    // Metadata
    std::string unknownStr = "app/detail/unknown"_i18n;
    metaDeveloper->setText(brls::getStr("app/detail/developer", game_.developer.empty() ? unknownStr : game_.developer));
    metaPublisher->setText(brls::getStr("app/detail/publisher", game_.publisher.empty() ? unknownStr : game_.publisher));
    metaYear->setText(brls::getStr("app/detail/release_date", game_.year.empty() ? unknownStr : game_.year));
    metaFormat->setText(brls::getStr("app/detail/image_format", game_.image_format.empty() ? unknownStr : game_.image_format));
    metaVoice->setText(brls::getStr("app/detail/voice_lang", game_.voice_lang.empty() ? unknownStr : game_.voice_lang));
    
    if (metaBox) {
        metaBox->setBackgroundColor(ThemeManager::instance().getCardBgColor());
        metaBox->setBorderThickness(1.0f);
        metaBox->setBorderColor(ThemeManager::instance().getCardBorderColor());
        metaBox->setCornerRadius(10.0f);
    }

    // Clean description (remove leading ': ')
    std::string desc = game_.description;
    if (desc.size() >= 2 && desc.substr(0, 2) == ": ") {
        desc = desc.substr(2);
    }
    description->setText(desc);
    
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

            const auto& cfg = config::ConfigManager::instance();
            if (cfg.getRetroRomsetMode() == "select") {
                btnDownload->setText("app/detail/select_files_btn"_i18n);
            } else {
                btnDownload->setText("app/detail/download_romset_btn"_i18n);
            }
        } else {
            btnDownload->setText("app/detail/download_rom"_i18n);
        }
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
        if (lowerMp != "\xd0\xbd\xd0\xb5\xd1\x82" && lowerMp != "no" && lowerMp != "1") {
            brls::Box* mpBadge = new brls::Box();
            mpBadge->setPadding(5, 10, 5, 10);
            mpBadge->setMarginRight(10);
            mpBadge->setMarginBottom(10);
            mpBadge->setBackgroundColor(nvgRGBA(255, 255, 255, 14));
            mpBadge->setBorderThickness(1.0f);
            mpBadge->setBorderColor(nvgRGBA(255, 255, 255, 22));
            mpBadge->setCornerRadius(6);

            brls::Label* mpLabel = new brls::Label();
            mpLabel->setText(mp);
            mpLabel->setFontSize(14);
            mpLabel->setTextColor(ThemeManager::instance().getCardTitleColor());
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

            setImageFromHTTPS(scrImg, scrUrl, imageToken, "romfs:/img/borealis_96.png", false, "", -1, -1, 1900000 - static_cast<int>(i) * 10);
            screenshotsBox->addView(scrImg);
            if (!firstScr) {
                firstScr = scrImg;
            }
        }
    }
    
    // Buttons Actions
    btnDownload->registerClickAction([this](brls::View* view) {
        if (!retro_console_id_.empty()) {
            const auto& cfg = config::ConfigManager::instance();
            if (isRomsetGame(game_) && cfg.getRetroRomsetMode() == "select") {
                brls::Application::pushActivity(new FileSelectView(game_, retro_console_id_));
                return true;
            } else {
                ui::DownloadManager::instance().addDownload(game_, {}, -1, "", retro_console_id_);
                brls::sync([]() {
                    brls::Application::pushActivity(new ui::DownloadsView());
                });
                return true;
            }
        }
        if (isHomebrewGame(game_)) {
            ui::DownloadManager::instance().addDownload(game_, {}, -1, "");
            brls::sync([]() {
                brls::Application::pushActivity(new ui::DownloadsView());
            });
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
    brls::Application::giveFocus(btnDownload);
}

void GameDetailView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    brls::Application::giveFocus(nullptr);
}

} // namespace ui
