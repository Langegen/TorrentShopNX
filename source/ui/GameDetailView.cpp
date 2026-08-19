#include "GameDetailView.hpp"
#include "FileSelectView.hpp"
#include "FavoritesManager.hpp"
#include "ScreenshotViewer.hpp"
#include <sstream>

namespace ui {

class ScrollAndFocusController : public brls::View {
public:
    ScrollAndFocusController(brls::ScrollingFrame* targetScroll, brls::Button* btnDownload)
        : targetScroll_(targetScroll), btnDownload_(btnDownload) {
        setHeight(0);
        setWidth(0);
    }
    
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override {
        if (!targetScroll_ || !btnDownload_) return;
        
        float contentHeight = 0;
        if (!targetScroll_->getChildren().empty()) {
            contentHeight = targetScroll_->getChildren().front()->getHeight();
        }
        float viewHeight = targetScroll_->getHeight();
        float maxOffset = contentHeight - viewHeight;
        
        if (contentHeight <= 0 || viewHeight <= 0) return;
        
        float currentOffset = targetScroll_->getContentOffsetY();
        bool atBottom = (maxOffset <= 0) || (currentOffset >= maxOffset - 5.0f);
        
        brls::View* currentFocus = brls::Application::getCurrentFocus();
        if (currentFocus == targetScroll_) {
            auto& state = brls::Application::getControllerState();
            bool dpadDown = state.buttons[brls::BUTTON_DOWN] || state.buttons[brls::BUTTON_NAV_DOWN];
            float leftY = state.axes[brls::LEFT_Y];
            
            // Left stick scrolling implementation
            if (std::abs(leftY) > 0.15f) {
                float speed = leftY * 12.0f;
                targetScroll_->setContentOffsetY(currentOffset + speed, false);
            }
            
            if (atBottom && (dpadDown || leftY > 0.15f)) {
                brls::Logger::info("ScrollAndFocusController: User pressed DOWN at bottom. Transferring focus to btnDownload.");
                brls::Application::giveFocus(btnDownload_);
            }
        }
    }
    
private:
    brls::ScrollingFrame* targetScroll_;
    brls::Button* btnDownload_;
};

GameDetailView::GameDetailView(const Game& game) : game_(game) {
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
    metaDeveloper->setText("Разработчик: " + (game_.developer.empty() ? "(неизвестно)" : game_.developer));
    metaPublisher->setText("Издатель: " + (game_.publisher.empty() ? "(неизвестно)" : game_.publisher));
    metaYear->setText("Дата выпуска: " + (game_.year.empty() ? "(неизвестно)" : game_.year));
    metaFormat->setText("Формат образа: " + (game_.image_format.empty() ? "(неизвестно)" : game_.image_format));
    metaVoice->setText("Язык озвучки: " + (game_.voice_lang.empty() ? "(неизвестно)" : game_.voice_lang));
    
    // Clean description (remove leading ': ')
    std::string desc = game_.description;
    if (desc.size() >= 2 && desc.substr(0, 2) == ": ") {
        desc = desc.substr(2);
    }
    description->setText(desc);
    
    // Dynamic Badges (Genres & Languages)
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
    std::stringstream ss(game_.genre);
    std::string genreTag;
    while (std::getline(ss, genreTag, ',')) {
        while (!genreTag.empty() && std::isspace(genreTag.front())) genreTag.erase(genreTag.begin());
        while (!genreTag.empty() && std::isspace(genreTag.back())) genreTag.pop_back();
        if (!genreTag.empty()) {
            brls::Box* gBadge = new brls::Box();
            gBadge->setPadding(5, 10, 5, 10);
            gBadge->setMarginRight(10);
            gBadge->setMarginBottom(10);
            gBadge->setBackgroundColor(nvgRGB(0, 150, 136)); // Teal genre badge
            gBadge->setCornerRadius(6);

            brls::Label* gLabel = new brls::Label();
            gLabel->setText(genreTag);
            gLabel->setFontSize(14);
            gLabel->setTextColor(nvgRGB(255, 255, 255));
            gBadge->addView(gLabel);

            badgesBox->addView(gBadge);
        }
    }
    
    // Screenshots Horizontal Scroll
    if (game_.screenshots.empty()) {
        screenshotsScroll->setVisibility(brls::Visibility::GONE);
    } else {
        screenshotsScroll->setVisibility(brls::Visibility::VISIBLE);
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
            setImageFromHTTPS(scrImg, scrUrl, imageToken, "romfs:/img/borealis_96.png", false, "", -1, -1, 1900000 - static_cast<int>(i) * 10);
            screenshotsBox->addView(scrImg);
        }
    }
    
    // Buttons Actions
    btnDownload->registerClickAction([this](brls::View* view) {
        brls::Application::pushActivity(new FileSelectView(game_));
        return true;
    });
    
    btnFavorite->registerClickAction([this](brls::View* view) {
        catalog::FavoritesManager::instance().toggleFavorite(game_);
        updateFavoriteButton();
        return true;
    });
    
    // Gamepad quick-favorites shortcut
    this->registerAction("В избранное / Убрать", brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
        catalog::FavoritesManager::instance().toggleFavorite(game_);
        updateFavoriteButton();
        return true;
    });
    
    // Update initial button state
    updateFavoriteButton();

    ScrollAndFocusController* scrollController = new ScrollAndFocusController(scroll, btnDownload);
    contentBox->addView(scrollController);


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
        btnFavorite->setText("Из избранного");
    } else {
        btnFavorite->setText("В избранное");
    }
}

brls::View* GameDetailView::create() {
    return nullptr; // XML constructor stub (should not be used directly)
}

void GameDetailView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    scroll->resetScrollToTop();
    brls::Application::giveFocus(scroll);
}

void GameDetailView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    brls::Application::giveFocus(nullptr);
}

} // namespace ui
