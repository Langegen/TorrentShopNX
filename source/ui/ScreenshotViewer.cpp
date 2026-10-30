#include "ScreenshotViewer.hpp"
#include "../GameData.hpp"
#include <borealis.hpp>

using namespace brls::literals;

namespace ui {

namespace {

// How long a new screenshot may load before "Loading…" replaces the
// previous one. Short enough to feel responsive, long enough that a picture
// from a fast host appears without anything flashing in between.
constexpr auto kLoadingDelay = std::chrono::milliseconds(350);

const char* const kPlaceholder = "romfs:/img/cover_blank.png";

// Image that counts texture changes: the downloader sets the placeholder at
// once and calls innerSetImage again when the picture (or, on failure, the
// placeholder) arrives.
class ShotImage : public brls::Image {
public:
    void innerSetImage(int tex) override {
        brls::Image::innerSetImage(tex);
        ++sets;
    }
    int sets = 0;
};

// Root box that gives the viewer a per-frame hook.
class ViewerRoot : public brls::Box {
public:
    explicit ViewerRoot(std::function<void()> onFrame) : onFrame_(std::move(onFrame)) {}
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        if (onFrame_) onFrame_();
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    std::function<void()> onFrame_;
};

} // namespace

struct ScreenshotViewer::Slot {
    ShotImage* img = nullptr;
    std::shared_ptr<bool> token;
    int setsAtStart = 0;     // texture changes made by the enqueue call itself
    int placeholderTex = 0;  // placeholder texture, to tell a failure from a picture

    bool done() const { return img && img->sets > setsAtStart; }
    bool failed() const { return done() && img->getTexture() == placeholderTex; }
};

ScreenshotViewer::ScreenshotViewer(const std::vector<std::string>& urls, size_t startIndex)
    : currentIndex_(0) {
    for (const auto& u : urls) {
        if (!u.empty()) urls_.push_back(u);
    }
    if (!urls_.empty()) {
        currentIndex_ = (startIndex < urls_.size()) ? startIndex : 0;
    }
}

brls::View* ScreenshotViewer::createContentView() {
    brls::Box* root = new ViewerRoot([this]() { refresh(); });
    root_ = root;
    root->setWidthPercentage(100.0f);
    root->setHeightPercentage(100.0f);
    root->setBackgroundColor(nvgRGB(0, 0, 0));
    root->setAlignItems(brls::AlignItems::CENTER);
    root->setJustifyContent(brls::JustifyContent::CENTER);

    // We want the view itself to be focusable to capture gamepad events
    root->setFocusable(true);

    // Screenshot layers are inserted at the bottom later (ensureSlot).
    // Status text in the middle, hidden (alpha 0) until needed.
    auto makeStatus = [root](const std::string& text) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(18.0f);
        label->setTextColor(nvgRGB(200, 205, 215));
        label->setAlpha(0.0f);
        root->addView(label);
        return label;
    };
    loadingLabel_ = makeStatus("app/detail/screenshot_loading"_i18n);
    failedLabel_ = makeStatus("app/detail/screenshot_load_failed"_i18n);
    failedLabel_->setPositionType(brls::PositionType::ABSOLUTE);

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
    closeBtn->setFocusable(false); // Touch-only, don't steal D-pad focus from root

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
    prevBtn_->setPositionTopPercentage(45.0f);
    prevBtn_->setWidth(46.0f);
    prevBtn_->setHeight(64.0f);
    prevBtn_->setCornerRadius(10.0f);
    prevBtn_->setBackgroundColor(nvgRGBA(0, 0, 0, 150));
    prevBtn_->setJustifyContent(brls::JustifyContent::CENTER);
    prevBtn_->setAlignItems(brls::AlignItems::CENTER);
    prevBtn_->setFocusable(false); // Touch-only, don't hijack controller navigation

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
    nextBtn_->setPositionTopPercentage(45.0f);
    nextBtn_->setWidth(46.0f);
    nextBtn_->setHeight(64.0f);
    nextBtn_->setCornerRadius(10.0f);
    nextBtn_->setBackgroundColor(nvgRGBA(0, 0, 0, 150));
    nextBtn_->setJustifyContent(brls::JustifyContent::CENTER);
    nextBtn_->setAlignItems(brls::AlignItems::CENTER);
    nextBtn_->setFocusable(false); // Touch-only, don't hijack controller navigation

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

    // Navigation: NAV_LEFT / NAV_RIGHT cover the D-pad and both sticks. The
    // D-pad also sends BUTTON_LEFT / BUTTON_RIGHT, so binding those too made
    // one press skip two screenshots.
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
    root->registerAction("Close (Start)", brls::ControllerButton::BUTTON_START, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    }, true /* hidden */);

    return root;
}

ScreenshotViewer::~ScreenshotViewer() {
    // Late downloads must not touch the deleted layers
    for (auto& kv : slots_) {
        if (kv.second->token) *kv.second->token = false;
    }
}

void ScreenshotViewer::onContentAvailable() {
    if (urls_.empty()) {
        brls::Application::popActivity();
        return;
    }
    goTo(currentIndex_);
    brls::Application::giveFocus(this->getContentView());
}

void ScreenshotViewer::ensureSlot(size_t index, int priority) {
    if (index >= urls_.size() || slots_.count(index)) return;

    auto slot = std::make_unique<Slot>();
    slot->token = std::make_shared<bool>(true);
    slot->img = new ShotImage();
    slot->img->setPositionType(brls::PositionType::ABSOLUTE);
    slot->img->setPositionTop(0);
    slot->img->setPositionLeft(0);
    slot->img->setWidthPercentage(100.0f);
    slot->img->setHeightPercentage(100.0f);
    slot->img->setScalingType(brls::ImageScalingType::FIT);
    slot->img->setAlpha(0.0f);
    root_->addView(slot->img, 0);   // under the status text, top bar and arrows

    std::string fallbackUrl = normalizeImageUrl(urls_[index]);
    std::string url = getOriginalImageUrl(fallbackUrl);
    setImageFromHTTPS(slot->img, url, slot->token, kPlaceholder, true, fallbackUrl, -1, -1, priority);
    slot->setsAtStart = slot->img->sets;
    slot->placeholderTex = slot->img->getTexture();

    slots_[index] = std::move(slot);
}

void ScreenshotViewer::dropFarSlots() {
    for (auto it = slots_.begin(); it != slots_.end();) {
        size_t idx = it->first;
        size_t dist = idx > currentIndex_ ? idx - currentIndex_ : currentIndex_ - idx;
        // Keep the neighbours, and the picture on screen until the new one replaces it
        if (dist <= 1 || idx == shownIndex_) {
            ++it;
            continue;
        }
        *it->second->token = false;
        root_->removeView(it->second->img, true);
        it = slots_.erase(it);
    }
}

void ScreenshotViewer::goTo(size_t index) {
    if (urls_.empty() || index >= urls_.size()) return;
    currentIndex_ = index;
    waitSince_ = std::chrono::steady_clock::now();

    // Current first, then the neighbours
    ensureSlot(index, 3000000);
    ensureSlot(index + 1, 2990000);
    if (index > 0) ensureSlot(index - 1, 2980000);
    dropFarSlots();
    updateChrome();
    refresh();
}

void ScreenshotViewer::refresh() {
    if (slots_.empty() || !loadingLabel_ || !failedLabel_) return;
    auto it = slots_.find(currentIndex_);
    const Slot* cur = (it != slots_.end()) ? it->second.get() : nullptr;
    const bool ready = cur && cur->done();
    const bool failed = ready && cur->failed();
    const bool waitedLong = std::chrono::steady_clock::now() - waitSince_ >= kLoadingDelay;

    // The new picture as soon as it is there; until then the previous one,
    // for kLoadingDelay at most.
    size_t visible = kNone;
    if (ready && !failed) {
        visible = currentIndex_;
    } else if (!ready && !waitedLong && shownIndex_ != kNone) {
        auto old = slots_.find(shownIndex_);
        if (old != slots_.end() && old->second->done() && !old->second->failed()) visible = shownIndex_;
    }
    shownIndex_ = visible;

    for (auto& kv : slots_) {
        kv.second->img->setAlpha(kv.first == visible ? 1.0f : 0.0f);
    }
    loadingLabel_->setAlpha(visible == kNone && !ready && waitedLong ? 1.0f : 0.0f);
    failedLabel_->setAlpha(failed ? 1.0f : 0.0f);
}

void ScreenshotViewer::updateChrome() {
    if (counterLabel_) {
        counterLabel_->setText(std::to_string(currentIndex_ + 1) + " / " + std::to_string(urls_.size()));
    }
    if (prevBtn_) {
        prevBtn_->setVisibility(currentIndex_ > 0 ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
    if (nextBtn_) {
        nextBtn_->setVisibility(currentIndex_ + 1 < urls_.size() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
}

void ScreenshotViewer::nextImage() {
    if (currentIndex_ + 1 < urls_.size()) goTo(currentIndex_ + 1);
}

void ScreenshotViewer::prevImage() {
    if (currentIndex_ > 0) goTo(currentIndex_ - 1);
}

} // namespace ui
