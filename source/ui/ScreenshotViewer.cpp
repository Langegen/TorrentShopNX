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
}

void ScreenshotViewer::onContentAvailable() {
    loadCurrent();
    brls::Application::giveFocus(this->getContentView());
}

void ScreenshotViewer::loadCurrent() {
    if (urls_.empty() || currentIndex_ >= urls_.size()) return;
    
    // Cancel previous image loading if any
    *imageToken_ = false;
    imageToken_ = std::make_shared<bool>(true);
    
    // Using a clear transparent image temporarily while loading
    image_->setImageFromRes("img/borealis_96.png"); 
    
    // Attempt to get the high-res version of the screenshot
    std::string fallbackUrl = urls_[currentIndex_];
    std::string url = fallbackUrl;
    size_t pos = url.find("/thumb/");
    if (pos != std::string::npos) {
        url.replace(pos, 7, "/big/");
    }
    // fastpic thumbnails often have .jpeg while originals have .jpg
    pos = url.rfind(".jpeg");
    if (pos != std::string::npos && pos == url.length() - 5) {
        url.replace(pos, 5, ".jpg");
    }

    setImageFromHTTPS(image_, url, imageToken_, "romfs:/img/borealis_96.png", false, fallbackUrl);
}

} // namespace ui
