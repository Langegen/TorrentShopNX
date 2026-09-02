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
    
    // Navigation actions
    root->registerAction("Next", brls::ControllerButton::BUTTON_RIGHT, [this](brls::View* view) {
        if (currentIndex_ + 1 < urls_.size()) {
            currentIndex_++;
            loadCurrent();
        }
        return true;
    });
    
    root->registerAction("Prev", brls::ControllerButton::BUTTON_LEFT, [this](brls::View* view) {
        if (currentIndex_ > 0) {
            currentIndex_--;
            loadCurrent();
        }
        return true;
    });

    root->registerAction("Next (RB)", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        if (currentIndex_ + 1 < urls_.size()) {
            currentIndex_++;
            loadCurrent();
        }
        return true;
    });
    
    root->registerAction("Prev (LB)", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        if (currentIndex_ > 0) {
            currentIndex_--;
            loadCurrent();
        }
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

    setImageFromHTTPS(image_, url, imageToken_, "romfs:/img/borealis_96.png", true, fallbackUrl, -1, -1, 3000000);
}

} // namespace ui
