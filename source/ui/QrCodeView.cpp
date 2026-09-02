#include "QrCodeView.hpp"
#include "../utils/qrcodegen.hpp"
#include <vector>
#include <algorithm>

using qrcodegen::QrCode;
using qrcodegen::QrSegment;
using namespace brls::literals;

namespace ui {

QrCodeView::QrCodeView() {
    this->setWidth(256);
    this->setHeight(256);
}

QrCodeView::~QrCodeView() {
}

void QrCodeView::setContent(const std::string& text) {
    this->content = text;
    this->needsUpdate = true;
}

void QrCodeView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    if (content.empty()) {
        return;
    }

    try {
        QrCode qr = QrCode::encodeText(content.c_str(), QrCode::Ecc::LOW);

        int size = qr.getSize();
        int margin = 2; // quiet zone
        int fullSize = size + margin * 2;
        
        float scale = std::min(width / fullSize, height / fullSize);
        
        float actualWidth = fullSize * scale;
        float actualHeight = fullSize * scale;
        float offsetX = x + (width - actualWidth) / 2.0f;
        float offsetY = y + (height - actualHeight) / 2.0f;

        // Draw white background with subtle rounded corners
        nvgBeginPath(vg);
        nvgRoundedRect(vg, offsetX, offsetY, actualWidth, actualHeight, 10.0f);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);

        // Draw black squares
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        
        for (int yqr = 0; yqr < size; yqr++) {
            for (int xqr = 0; xqr < size; xqr++) {
                if (qr.getModule(xqr, yqr)) {
                    float rectX = offsetX + (xqr + margin) * scale;
                    float rectY = offsetY + (yqr + margin) * scale;
                    
                    nvgBeginPath(vg);
                    // Draw slightly larger to avoid anti-aliasing gaps between modules
                    nvgRect(vg, rectX, rectY, scale + 0.5f, scale + 0.5f);
                    nvgFill(vg);
                }
            }
        }
    } catch (...) {
        // Fallback or error handling if encoding fails
    }
}

QrDialog::QrDialog(const std::string& title, const std::string& url, const std::string& hint) {
    // Root container covering 100% of the screen with a dimmed backdrop
    this->setWidthPercentage(100.0f);
    this->setHeightPercentage(100.0f);
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setBackgroundColor(nvgRGBA(0, 0, 0, 185));

    // Tap outside to dismiss
    this->addGestureRecognizer(new brls::TapGestureRecognizer(this, [this]() {
        dismissDialog();
    }));

    // Center Modal Card
    brls::Box* card = new brls::Box();
    card->setWidth(480.0f);
    card->setAxis(brls::Axis::COLUMN);
    card->setAlignItems(brls::AlignItems::CENTER);
    card->setJustifyContent(brls::JustifyContent::CENTER);
    card->setBackgroundColor(nvgRGB(34, 36, 42));
    card->setCornerRadius(16.0f);
    card->setPadding(26.0f, 28.0f, 24.0f, 28.0f);

    // Prevent backdrop tap from triggering when tapping inside the card
    card->addGestureRecognizer(new brls::TapGestureRecognizer(card, []() {}));

    // 1. Title Header
    if (!title.empty()) {
        brls::Label* titleLabel = new brls::Label();
        titleLabel->setText(title);
        titleLabel->setFontSize(20.0f);
        titleLabel->setTextColor(nvgRGB(255, 255, 255));
        titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        titleLabel->setMarginBottom(16.0f);
        card->addView(titleLabel);
    }

    // 2. White QR Card Container (Neat rounded square with crisp margins)
    brls::Box* qrCard = new brls::Box();
    qrCard->setWidth(220.0f);
    qrCard->setHeight(220.0f);
    qrCard->setBackgroundColor(nvgRGB(255, 255, 255));
    qrCard->setCornerRadius(12.0f);
    qrCard->setJustifyContent(brls::JustifyContent::CENTER);
    qrCard->setAlignItems(brls::AlignItems::CENTER);
    qrCard->setPadding(10.0f, 10.0f, 10.0f, 10.0f);
    qrCard->setMarginBottom(16.0f);

    QrCodeView* qrView = new QrCodeView();
    qrView->setWidth(200.0f);
    qrView->setHeight(200.0f);
    qrView->setContent(url);
    qrCard->addView(qrView);
    card->addView(qrCard);

    // 3. Hint Text
    if (!hint.empty()) {
        brls::Label* hintLabel = new brls::Label();
        hintLabel->setText(hint);
        hintLabel->setFontSize(14.5f);
        hintLabel->setTextColor(nvgRGB(175, 180, 190));
        hintLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        hintLabel->setWidth(420.0f);
        hintLabel->setIsWrapping(true);
        hintLabel->setMarginBottom(10.0f);
        card->addView(hintLabel);
    }

    // 4. URL badge / container
    if (!url.empty()) {
        brls::Box* urlBox = new brls::Box();
        urlBox->setWidth(420.0f);
        urlBox->setBackgroundColor(nvgRGBA(18, 20, 24, 210));
        urlBox->setCornerRadius(8.0f);
        urlBox->setPadding(6.0f, 12.0f, 6.0f, 12.0f);
        urlBox->setAlignItems(brls::AlignItems::CENTER);
        urlBox->setMarginBottom(20.0f);

        brls::Label* urlLabel = new brls::Label();
        urlLabel->setText(url);
        urlLabel->setFontSize(12.0f);
        urlLabel->setTextColor(nvgRGB(100, 180, 245));
        urlLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        urlLabel->setWidth(396.0f);
        urlLabel->setIsWrapping(true);
        urlBox->addView(urlLabel);
        card->addView(urlBox);
    }

    // 5. OK Button
    btnOk_ = new brls::Button();
    btnOk_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    btnOk_->setText("app/common/ok"_i18n);
    btnOk_->setWidth(180.0f);
    btnOk_->setHeight(42.0f);
    btnOk_->setCornerRadius(8.0f);
    btnOk_->setHighlightCornerRadius(8.0f);
    btnOk_->setFocusable(true);
    btnOk_->registerClickAction([this](brls::View* view) {
        dismissDialog();
        return true;
    });
    card->addView(btnOk_);

    this->addView(card);

    // Make card focusable so button inside gets default focus
    this->setLastFocusedView(btnOk_);

    // Controller actions: A button or B button closes the dialog
    this->registerAction("app/common/ok"_i18n, brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
        dismissDialog();
        return true;
    });

    this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View* view) {
        dismissDialog();
        return true;
    });
}

void QrDialog::dismissDialog() {
    brls::Application::popActivity(brls::TransitionAnimation::FADE);
}

void QrDialog::open(const std::string& title, const std::string& url, const std::string& hint) {
    QrDialog* dialog = new QrDialog(title, url, hint);
    brls::Activity* act = new brls::Activity(dialog);
    brls::Application::pushActivity(act, brls::TransitionAnimation::FADE);
    if (dialog->getOkButton()) {
        brls::Application::giveFocus(dialog->getOkButton());
    }
}

} // namespace ui
