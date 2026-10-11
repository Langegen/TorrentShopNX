#include "DashboardTile.hpp"
#include "../ThemeManager.hpp"

using namespace brls::literals;

namespace ui {

DashboardTile::DashboardTile(int index, const std::string& icon_res, const std::string& title,
                             std::function<void(int)> on_focus, std::function<void(int)> on_click)
    : index_(index), title_(title), on_focus_(on_focus), on_click_(on_click) {
    
    this->setFocusable(true);
    this->setHideHighlight(true); // Disable Borealis default highlight outline so only our custom glass border is visible!
    this->setWidth(208.0f);
    this->setHeight(230.0f);
    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(0.0f);

    // 1. Card Box (Containing the full icon)
    card_box_ = new brls::Box();
    card_box_->setWidth(208.0f);
    card_box_->setHeight(196.0f);
    card_box_->setAxis(brls::Axis::COLUMN);
    card_box_->setJustifyContent(brls::JustifyContent::CENTER);
    card_box_->setAlignItems(brls::AlignItems::CENTER);
    card_box_->setCornerRadius(18.0f);
    card_box_->setPadding(6.0f);

    // Centered Tile Icon (Fills the card)
    icon_view_ = new brls::Image();
    icon_view_->setWidth(196.0f);
    icon_view_->setHeight(184.0f);
    icon_view_->setScalingType(brls::ImageScalingType::FIT);
    icon_view_->setImageFromFile(std::string(BRLS_RESOURCES) + icon_res);
    card_box_->addView(icon_view_);

    // Optional top badge (e.g. "2 Active")
    NVGcolor accent = ThemeManager::instance().getTextAccentColor();
    sub_badge_ = new brls::Label();
    sub_badge_->setText("");
    sub_badge_->setFontSize(12.0f);
    sub_badge_->setTextColor(nvgRGBA(accent.r * 255, accent.g * 255, accent.b * 255, 230));
    sub_badge_->setPositionType(brls::PositionType::ABSOLUTE);
    sub_badge_->setPositionTop(10.0f);
    sub_badge_->setPositionRight(12.0f);
    sub_badge_->setVisibility(brls::Visibility::GONE);
    card_box_->addView(sub_badge_);

    this->addView(card_box_);

    // 2. Clean Bottom Title
    title_label_ = new brls::Label();
    title_label_->setText(title);
    title_label_->setFontSize(16.0f);
    title_label_->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    title_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title_label_->setHeight(26.0f);
    title_label_->setMarginTop(6.0f);
    this->addView(title_label_);

    // Register native Borealis Action for button A with valid i18n key "hints/ok"
    this->registerAction("hints/ok"_i18n, brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
        if (on_click_) on_click_(index_);
        return true;
    });

    ThemeManager::instance().subscribe([this]() {
        refreshTheme();
    });
}

void DashboardTile::setBadge(const std::string& text) {
    if (sub_badge_) {
        if (text.empty() || text == "0") {
            sub_badge_->setText("");
            sub_badge_->setVisibility(brls::Visibility::GONE);
        } else {
            sub_badge_->setText("↓ " + text + " Active");
            sub_badge_->setVisibility(brls::Visibility::VISIBLE);
        }
    }
}

void DashboardTile::setTitle(const std::string& title) {
    title_ = title;
    if (title_label_) title_label_->setText(title);
}

void DashboardTile::refreshTheme() {
    if (title_label_) {
        title_label_->setTextColor(isFocused() ? ThemeManager::instance().getTextAccentColor()
                                              : ThemeManager::instance().getTextPrimaryColor());
    }
    if (sub_badge_) {
        NVGcolor accent = ThemeManager::instance().getTextAccentColor();
        sub_badge_->setTextColor(nvgRGBA(accent.r * 255, accent.g * 255, accent.b * 255, 230));
    }
}

void DashboardTile::onFocusGained() {
    Box::onFocusGained();
    if (title_label_) title_label_->setTextColor(ThemeManager::instance().getTextAccentColor());
    if (on_focus_) on_focus_(index_);
}

void DashboardTile::onFocusLost() {
    Box::onFocusLost();
    if (title_label_) title_label_->setTextColor(ThemeManager::instance().getTextPrimaryColor());
}

void DashboardTile::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style style, brls::FrameContext* ctx) {
    float targetScale = isFocused() ? 1.06f : 1.0f;
    scale_ += (targetScale - scale_) * 0.22f;
    float targetGlow = isFocused() ? 1.0f : 0.0f;
    glow_ += (targetGlow - glow_) * 0.22f;

    // Card frame bounds
    float cardW = width;
    float cardH = 196.0f;
    float cardX = x;
    float cardY = y;

    float cx = cardX + cardW * 0.5f;
    float cy = cardY + cardH * 0.5f;

    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgScale(vg, scale_, scale_);
    nvgTranslate(vg, -cx, -cy);

    // 1. Subtle, Delicate Accent Outer Glow (No large harsh blooms!)
    if (glow_ > 0.01f) {
        NVGcolor acc = ThemeManager::instance().getAccentColor();
        NVGpaint glowPaint = nvgBoxGradient(vg, cardX - 2.0f, cardY - 2.0f, cardW + 4.0f, cardH + 4.0f,
                                            18.0f, 8.0f,
                                            nvgRGBA(acc.r * 255, acc.g * 255, acc.b * 255, static_cast<unsigned char>(70.0f * glow_)),
                                            nvgRGBA(acc.r * 255, acc.g * 255, acc.b * 255, 0));
        nvgBeginPath(vg);
        nvgRect(vg, cardX - 12.0f, cardY - 12.0f, cardW + 24.0f, cardH + 24.0f);
        nvgFillPaint(vg, glowPaint);
        nvgFill(vg);
    }

    // 2. Translucent Glassmorphism Base
    nvgBeginPath(vg);
    nvgRoundedRect(vg, cardX, cardY, cardW, cardH, 18.0f);

    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    NVGcolor glassTop;
    NVGcolor glassBottom;
    if (isLight) {
        glassTop = isFocused()
            ? nvgRGBA(255, 255, 255, 255)
            : nvgRGBA(255, 255, 255, 235);
        glassBottom = isFocused()
            ? nvgRGBA(242, 248, 255, 255)
            : nvgRGBA(242, 246, 250, 225);
    } else {
        NVGcolor acc = ThemeManager::instance().getAccentColor();
        glassTop = isFocused()
            ? nvgRGBA(acc.r * 255 * 0.85f, acc.g * 255 * 0.85f, acc.b * 255 * 0.85f, 65)
            : nvgRGBA(140, 180, 210, 40);
        glassBottom = isFocused()
            ? nvgRGBA(8, 28, 42, 120)
            : nvgRGBA(15, 25, 38, 75);
    }

    NVGpaint glassPaint = nvgLinearGradient(vg, cardX, cardY, cardX, cardY + cardH, glassTop, glassBottom);
    nvgFillPaint(vg, glassPaint);
    nvgFill(vg);

    // 3. Specular Glass Reflection Sheen (Top Half Gloss)
    nvgBeginPath(vg);
    nvgRoundedRect(vg, cardX + 1.0f, cardY + 1.0f, cardW - 2.0f, cardH * 0.45f, 17.0f);
    NVGpaint glossPaint = nvgLinearGradient(
        vg, cardX, cardY, cardX, cardY + cardH * 0.45f,
        nvgRGBA(255, 255, 255, static_cast<unsigned char>(isFocused() ? (isLight ? 70 : 50) : (isLight ? 50 : 35))),
        nvgRGBA(255, 255, 255, 0)
    );
    nvgFillPaint(vg, glossPaint);
    nvgFill(vg);

    // 4. Refractive Glass Border Stroke
    nvgBeginPath(vg);
    nvgRoundedRect(vg, cardX, cardY, cardW, cardH, 18.0f);
    if (glow_ > 0.01f) {
        // Delicate Accent Border on Focus (clean Nintendo-style 2.5px line in light mode)
        NVGcolor acc = ThemeManager::instance().getAccentColor();
        nvgStrokeColor(vg, nvgRGBA(acc.r * 255, acc.g * 255, acc.b * 255, static_cast<unsigned char>(255.0f * glow_)));
        nvgStrokeWidth(vg, isLight ? 2.5f : 1.8f);
    } else {
        // Subtle ambient glass edge
        NVGpaint borderPaint;
        if (isLight) {
            borderPaint = nvgLinearGradient(
                vg, cardX, cardY, cardX, cardY + cardH,
                nvgRGBA(210, 220, 232, 200),
                nvgRGBA(190, 202, 218, 150)
            );
        } else {
            borderPaint = nvgLinearGradient(
                vg, cardX, cardY, cardX, cardY + cardH,
                nvgRGBA(180, 220, 225, 110),
                nvgRGBA(50, 85, 110, 40)
            );
        }
        nvgStrokePaint(vg, borderPaint);
        nvgStrokeWidth(vg, 1.2f);
    }
    nvgStroke(vg);

    nvgRestore(vg);

    // 5. Draw children views (card box with icon + clean bottom title)
    Box::draw(vg, x, y, width, height, style, ctx);
}

} // namespace ui
