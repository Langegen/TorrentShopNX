#include "ScreenshotViewer.hpp"
#include "../GameData.hpp"
#include <borealis.hpp>

namespace ui {

ScreenshotViewer::ScreenshotViewer(const std::vector<std::string>& urls, size_t startIndex)
    : urls_(urls), currentIndex_(startIndex), imageToken_(std::make_shared<bool>(true)) {
}

brls::View* ScreenshotViewer::createContentView() {
    brls::Box* root = new brls::Box();
    root->setWidth(brls::Application::windowWidth);
    root->setHeight(brls::Application::windowHeight);
    root->setBackgroundColor(nvgRGB(0, 0, 0));
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setJustifyContent(brls::JustifyContent::CENTER);
    
    // We want the view itself to be focusable to capture gamepad events
    root->setFocusable(true);

    image_ = new brls::Image();
    image_->setScalingType(brls::ImageScalingType::FIT);
    image_->setWidth(brls::Application::windowWidth);
    image_->setHeight(brls::Application::windowHeight);
    root->addView(image_);

    // ── Floating Top Bar Overlay ─────────────────────────────────────────
    auto* topBar = new brls::Box();
    topBar->setPositionType(brls::PositionType::ABSOLUTE);
    topBar->setPositionTop(0);
    topBar->setPositionLeft(0);
    topBar->setWidthPercentage(100.0f);
    topBar->setHeight(64.0f);
    topBar->setAxis(brls::Axis::ROW);
    topBar->setAlignItems(brls::AlignItems::CENTER);
    topBar->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    topBar->setPadding(10.0f, 24.0f, 10.0f, 24.0f);
    topBar->setBackgroundColor(nvgRGBA(0, 0, 0, 160));

    counterLabel_ = new brls::Label();
    counterLabel_->setText(std::to_string(currentIndex_ + 1) + " / " + std::to_string(urls_.size()));
    counterLabel_->setFontSize(16.0f);
    counterLabel_->setTextColor(nvgRGB(240, 240, 240));
    topBar->addView(counterLabel_);

    auto* closeBtn = new brls::Box();
    closeBtn->setWidth(42.0f);
    closeBtn->setHeight(42.0f);
    closeBtn->setCornerRadius(21.0f);
    closeBtn->setBackgroundColor(nvgRGBA(255, 255, 255, 30));
    closeBtn->setJustifyContent(brls::JustifyContent::CENTER);
    closeBtn->setAlignItems(brls::AlignItems::CENTER);
    closeBtn->setFocusable(true);

    auto* closeIcon = new brls::Label();
    closeIcon->setText("\uE5CD"); // Material close
    closeIcon->setFontSize(22.0f);
    closeIcon->setTextColor(nvgRGB(255, 255, 255));
    closeBtn->addView(closeIcon);

    closeBtn->registerClickAction([](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });
    topBar->addView(closeBtn);
    root->addView(topBar);

    // ── Prev Button (Left Floating Pill) ─────────────────────────────────
    prevBtn_ = new brls::Box();
    prevBtn_->setPositionType(brls::PositionType::ABSOLUTE);
    prevBtn_->setPositionLeft(20.0f);
    prevBtn_->setPositionTop(brls::Application::windowHeight * 0.45f);
    prevBtn_->setWidth(46.0f);
    prevBtn_->setHeight(64.0f);
    prevBtn_->setCornerRadius(10.0f);
    prevBtn_->setBackgroundColor(nvgRGBA(0, 0, 0, 150));
    prevBtn_->setJustifyContent(brls::JustifyContent::CENTER);
    prevBtn_->setAlignItems(brls::AlignItems::CENTER);
    prevBtn_->setFocusable(true);

    auto* prevIcon = new brls::Label();
    prevIcon->setText("\uE5CB"); // Material chevron left
    prevIcon->setFontSize(28.0f);
    prevIcon->setTextColor(nvgRGB(255, 255, 255));
    prevBtn_->addView(prevIcon);

    prevBtn_->registerClickAction([this](brls::View* view) {
        prevImage();
        return true;
    });
    root->addView(prevBtn_);

    // ── Next Button (Right Floating Pill) ────────────────────────────────
    nextBtn_ = new brls::Box();
    nextBtn_->setPositionType(brls::PositionType::ABSOLUTE);
    nextBtn_->setPositionRight(20.0f);
    nextBtn_->setPositionTop(brls::Application::windowHeight * 0.45f);
    nextBtn_->setWidth(46.0f);
    nextBtn_->setHeight(64.0f);
    nextBtn_->setCornerRadius(10.0f);
    nextBtn_->setBackgroundColor(nvgRGBA(0, 0, 0, 150));
    nextBtn_->setJustifyContent(brls::JustifyContent::CENTER);
    nextBtn_->setAlignItems(brls::AlignItems::CENTER);
    nextBtn_->setFocusable(true);

    auto* nextIcon = new brls::Label();
    nextIcon->setText("\uE5CC"); // Material chevron right
    nextIcon->setFontSize(28.0f);
    nextIcon->setTextColor(nvgRGB(255, 255, 255));
    nextBtn_->addView(nextIcon);

    nextBtn_->registerClickAction([this](brls::View* view) {
        nextImage();
        return true;
    });
    root->addView(nextBtn_);

    // ── Horizontal Swipe / Pan Gesture ───────────────────────────────────
    root->addGestureRecognizer(new brls::PanGestureRecognizer([this](brls::PanGestureStatus status, brls::Sound* sound) {
        if (status.state == brls::GestureState::END) {
            float dx = status.position.x - status.startPosition.x;
            if (dx < -40.0f) {
                nextImage();
            } else if (dx > 40.0f) {
                prevImage();
            }
        }
    }, brls::PanAxis::HORIZONTAL));
    
    // Navigation actions (supports both left stick and D-Pad)
    root->registerAction("Next", brls::ControllerButton::BUTTON_NAV_RIGHT, [this](brls::View* view) {
        nextImage();
        return true;
    });
    
    root->registerAction("Prev", brls::ControllerButton::BUTTON_NAV_LEFT, [this](brls::View* view) {
        prevImage();
        return true;
    });

    root->registerAction("Next (RB)", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        nextImage();
        return true;
    });
    
    root->registerAction("Prev (LB)", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        prevImage();
        return true;
    });

    root->registerAction("Close", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });

    return root;
}

ScreenshotViewer::~ScreenshotViewer() {
    if (imageToken_) {
        *imageToken_ = false;
    }
    if (image_) {
        image_->clear();
    }
}

void ScreenshotViewer::onContentAvailable() {
    loadCurrent();
    brls::Application::giveFocus(this->getContentView());
}

void ScreenshotViewer::loadCurrent() {
    if (urls_.empty() || currentIndex_ >= urls_.size()) return;
    
    // Cancel previous image loading if any
    if (imageToken_) {
        *imageToken_ = false;
    }
    imageToken_ = std::make_shared<bool>(true);
    
    // Clear previous texture safely to prevent memory leak
    if (image_) {
        image_->clear();
        image_->setImageFromFile("romfs:/img/borealis_96.png");
        image_->setFreeTexture(true);
    }
    
    std::string rawUrl = urls_[currentIndex_];
    std::string fallbackUrl = normalizeImageUrl(rawUrl);
    std::string url = getOriginalImageUrl(fallbackUrl);

    if (counterLabel_) {
        counterLabel_->setText(std::to_string(currentIndex_ + 1) + " / " + std::to_string(urls_.size()));
    }
    if (prevBtn_) {
        prevBtn_->setVisibility(currentIndex_ > 0 ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (nextBtn_) {
        nextBtn_->setVisibility(currentIndex_ + 1 < urls_.size() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    setImageFromHTTPS(image_, url, imageToken_, "romfs:/img/borealis_96.png", true, fallbackUrl, -1, -1, 3000000);
}

void ScreenshotViewer::nextImage() {
    if (currentIndex_ + 1 < urls_.size()) {
        currentIndex_++;
        loadCurrent();
    }
}

void ScreenshotViewer::prevImage() {
    if (currentIndex_ > 0) {
        currentIndex_--;
        loadCurrent();
    }
}

} // namespace ui
