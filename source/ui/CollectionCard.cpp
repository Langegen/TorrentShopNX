#include "CollectionCard.hpp"
#include <cmath>

using namespace brls::literals;

namespace ui {

CollectionCard::CollectionCard(const std::string& icon_type,
                               const std::string& title,
                               const std::string& desc,
                               const std::string& count_text,
                               NVGcolor icon_color,
                               std::function<void()> on_click)
    : icon_type_(icon_type), title_(title), count_text_(count_text),
      icon_color_(icon_color), on_click_(on_click) {
    
    this->setFocusable(true);
    this->setHideHighlight(true); // Custom glass border
    this->setWidth(274.0f);
    this->setHeight(104.0f);
    this->setAxis(brls::Axis::ROW);
    this->setPadding(10.0f, 12.0f, 10.0f, 12.0f);
    this->setCornerRadius(14.0f);
    this->setAlignItems(brls::AlignItems::CENTER);

    // 1. Left placeholder spacer for 76x76 large emblem icon
    brls::Box* iconSpacer = new brls::Box();
    iconSpacer->setWidth(76.0f);
    iconSpacer->setHeight(76.0f);
    iconSpacer->setMarginRight(12.0f);
    this->addView(iconSpacer);

    // 2. Right: Content info column (Fixed 76px height to match emblem precisely)
    brls::Box* infoCol = new brls::Box();
    infoCol->setAxis(brls::Axis::COLUMN);
    infoCol->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    infoCol->setGrow(1.0f);
    infoCol->setHeight(76.0f);

    // Top block: Title + Desc
    brls::Box* topBox = new brls::Box();
    topBox->setAxis(brls::Axis::COLUMN);
    topBox->setWidthPercentage(100.0f);

    title_label_ = new brls::Label();
    title_label_->setText(title);
    title_label_->setFontSize(14.5f);
    title_label_->setTextColor(nvgRGBA(255, 255, 255, 255));
    title_label_->setSingleLine(true);
    topBox->addView(title_label_);

    desc_label_ = new brls::Label();
    desc_label_->setText(desc);
    desc_label_->setFontSize(10.5f);
    desc_label_->setLineHeight(1.24f);
    desc_label_->setTextColor(nvgRGBA(150, 175, 200, 210));
    desc_label_->setMarginTop(5.0f);
    desc_label_->setSingleLine(false);
    topBox->addView(desc_label_);
    infoCol->addView(topBox);

    // Bottom block: Badge (Aligned exactly with bottom of the emblem)
    brls::Box* badgeRow = new brls::Box();
    badgeRow->setAxis(brls::Axis::ROW);
    badgeRow->setAlignItems(brls::AlignItems::CENTER);

    brls::Box* pill = new brls::Box();
    pill->setPadding(2.0f, 8.0f, 2.0f, 8.0f);
    pill->setCornerRadius(4.0f);
    pill->setBackgroundColor(nvgRGBA(0, 224, 165, 26)); // Emerald tint
    pill->setAlignItems(brls::AlignItems::CENTER);
    pill->setJustifyContent(brls::JustifyContent::CENTER);

    count_label_ = new brls::Label();
    count_label_->setText(count_text);
    count_label_->setFontSize(10.5f);
    count_label_->setTextColor(nvgRGBA(0, 230, 175, 255)); // Emerald text
    count_label_->setSingleLine(true);
    pill->addView(count_label_);
    badgeRow->addView(pill);

    infoCol->addView(badgeRow);
    this->addView(infoCol);

    this->registerAction("hints/ok"_i18n, brls::ControllerButton::BUTTON_A, [this](brls::View* view) {
        if (on_click_) on_click_();
        return true;
    });

    this->registerClickAction([this](brls::View* view) {
        if (on_click_) on_click_();
        return true;
    });
}

void CollectionCard::setCountText(const std::string& text) {
    count_text_ = text;
    if (count_label_) count_label_->setText(text);
}

void CollectionCard::onFocusGained() {
    Box::onFocusGained();
    if (title_label_) title_label_->setTextColor(nvgRGBA(0, 230, 175, 255));
}

void CollectionCard::onFocusLost() {
    Box::onFocusLost();
    if (title_label_) title_label_->setTextColor(nvgRGBA(255, 255, 255, 255));
}

void CollectionCard::drawIconGlyph(NVGcontext* vg, float bx, float by, float bw, float bh) {
    // 1. Icon Box Background Plate with Gradient Depth
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bx, by, bw, bh, 14.0f);
    NVGcolor bgTop = nvgRGBA(
        static_cast<unsigned char>(icon_color_.r * 255.0f),
        static_cast<unsigned char>(icon_color_.g * 255.0f),
        static_cast<unsigned char>(icon_color_.b * 255.0f),
        60
    );
    NVGcolor bgBot = nvgRGBA(
        static_cast<unsigned char>(icon_color_.r * 255.0f * 0.6f),
        static_cast<unsigned char>(icon_color_.g * 255.0f * 0.6f),
        static_cast<unsigned char>(icon_color_.b * 255.0f * 0.6f),
        30
    );
    nvgFillPaint(vg, nvgLinearGradient(vg, bx, by, bx, by + bh, bgTop, bgBot));
    nvgFill(vg);

    // Inner Gloss Sheen on Emblem
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bx + 1.0f, by + 1.0f, bw - 2.0f, bh * 0.45f, 13.0f);
    nvgFillPaint(vg, nvgLinearGradient(vg, bx, by, bx, by + bh * 0.45f, nvgRGBA(255, 255, 255, 45), nvgRGBA(255, 255, 255, 0)));
    nvgFill(vg);

    // Border
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bx, by, bw, bh, 14.0f);
    nvgStrokeColor(vg, nvgRGBA(
        static_cast<unsigned char>(icon_color_.r * 255.0f),
        static_cast<unsigned char>(icon_color_.g * 255.0f),
        static_cast<unsigned char>(icon_color_.b * 255.0f),
        110
    ));
    nvgStrokeWidth(vg, 1.2f);
    nvgStroke(vg);

    // 2. Large Vector Shapes Centered (76x76 container)
    float cx = bx + bw * 0.5f;
    float cy = by + bh * 0.5f;

    nvgStrokeColor(vg, icon_color_);
    nvgFillColor(vg, icon_color_);

    if (icon_type_ == "all_catalog") {
        // 2x2 Large Grid Squares
        float s = 11.0f, g = 5.0f;
        for (int r = 0; r < 2; ++r) {
            for (int c = 0; c < 2; ++c) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, cx - s - g * 0.5f + c * (s + g),
                                  cy - s - g * 0.5f + r * (s + g),
                                  s, s, 2.5f);
                nvgFill(vg);
            }
        }
    } else if (icon_type_ == "favorites") {
        // 5-Point Large Star
        nvgBeginPath(vg);
        for (int i = 0; i < 5; ++i) {
            float a1 = i * (M_PI * 2.0f / 5.0f) - M_PI * 0.5f;
            float a2 = a1 + M_PI / 5.0f;
            float r1 = 18.0f, r2 = 8.0f;
            float x1 = cx + std::cos(a1) * r1, y1 = cy + std::sin(a1) * r1;
            float x2 = cx + std::cos(a2) * r2, y2 = cy + std::sin(a2) * r2;
            if (i == 0) nvgMoveTo(vg, x1, y1);
            else nvgLineTo(vg, x1, y1);
            nvgLineTo(vg, x2, y2);
        }
        nvgClosePath(vg);
        nvgFill(vg);
    } else if (icon_type_ == "new_release") {
        // 4-Point Large Sparkle Diamond
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, cy - 20.0f);
        nvgQuadTo(vg, cx + 3.5f, cy - 3.5f, cx + 20.0f, cy);
        nvgQuadTo(vg, cx + 3.5f, cy + 3.5f, cx, cy + 20.0f);
        nvgQuadTo(vg, cx - 3.5f, cy + 3.5f, cx - 20.0f, cy);
        nvgQuadTo(vg, cx - 3.5f, cy - 3.5f, cx, cy - 20.0f);
        nvgClosePath(vg);
        nvgFill(vg);
    } else if (icon_type_ == "top_100") {
        // Large Golden Trophy
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 12.0f, cy - 18.0f, 24.0f, 16.0f, 3.0f);
        nvgFill(vg);
        // Base
        nvgBeginPath(vg);
        nvgRect(vg, cx - 3.5f, cy - 2.0f, 7.0f, 11.0f);
        nvgRoundedRect(vg, cx - 14.0f, cy + 9.0f, 28.0f, 5.0f, 1.5f);
        nvgFill(vg);
        // Handles
        nvgBeginPath(vg);
        nvgCircle(vg, cx - 12.0f, cy - 11.0f, 6.0f);
        nvgCircle(vg, cx + 12.0f, cy - 11.0f, 6.0f);
        nvgStrokeWidth(vg, 2.5f);
        nvgStroke(vg);
    } else if (icon_type_ == "action_adventure") {
        // Crossed Broadswords
        nvgStrokeWidth(vg, 3.2f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 16.0f, cy - 16.0f); nvgLineTo(vg, cx + 16.0f, cy + 16.0f);
        nvgMoveTo(vg, cx + 16.0f, cy - 16.0f); nvgLineTo(vg, cx - 16.0f, cy + 16.0f);
        nvgStroke(vg);
        // Crossguards
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 11.0f, cy - 4.0f); nvgLineTo(vg, cx - 4.0f, cy - 11.0f);
        nvgMoveTo(vg, cx + 11.0f, cy - 4.0f); nvgLineTo(vg, cx + 4.0f, cy - 11.0f);
        nvgStroke(vg);
    } else if (icon_type_ == "arcade") {
        // Large Arcade Joystick
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 16.0f, cy + 3.0f, 32.0f, 15.0f, 3.5f);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, cy + 3.0f);
        nvgLineTo(vg, cx, cy - 11.0f);
        nvgStrokeWidth(vg, 3.8f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy - 12.0f, 8.0f);
        nvgFill(vg);
    } else if (icon_type_ == "horror") {
        // Large Skull
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy - 3.0f, 14.0f);
        nvgRoundedRect(vg, cx - 8.0f, cy + 4.0f, 16.0f, 12.0f, 2.0f);
        nvgFill(vg);
        // Hollow Eyes
        nvgBeginPath(vg);
        nvgCircle(vg, cx - 5.5f, cy - 3.0f, 3.5f);
        nvgCircle(vg, cx + 5.5f, cy - 3.0f, 3.5f);
        nvgFillColor(vg, nvgRGBA(18, 18, 22, 255));
        nvgFill(vg);
    } else if (icon_type_ == "metroidvania") {
        // Large Swirling Portal Rings
        nvgStrokeWidth(vg, 3.5f);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 16.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 8.0f);
        nvgStroke(vg);
    } else if (icon_type_ == "party_multiplayer") {
        // Dual Gamepads
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 16.0f, cy - 11.0f, 20.0f, 13.0f, 3.0f);
        nvgRoundedRect(vg, cx - 4.0f, cy + 1.0f, 20.0f, 13.0f, 3.0f);
        nvgFill(vg);
    } else if (icon_type_ == "platformers") {
        // High Jump Arc over Platform
        nvgBeginPath(vg);
        nvgRect(vg, cx - 16.0f, cy + 10.0f, 32.0f, 5.0f);
        nvgFill(vg);
        nvgStrokeWidth(vg, 3.2f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 13.0f, cy + 3.0f);
        nvgQuadTo(vg, cx, cy - 20.0f, cx + 13.0f, cy + 3.0f);
        nvgStroke(vg);
    } else if (icon_type_ == "puzzles") {
        // Jigsaw Puzzle Piece
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 12.0f, cy - 12.0f, 24.0f, 24.0f, 3.0f);
        nvgCircle(vg, cx, cy - 12.0f, 5.0f);
        nvgCircle(vg, cx + 12.0f, cy, 5.0f);
        nvgFill(vg);
    } else if (icon_type_ == "roguelike_roguelite") {
        // Large D6 Die with Dots
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 13.0f, cy - 13.0f, 26.0f, 26.0f, 5.0f);
        nvgFill(vg);
        nvgFillColor(vg, nvgRGBA(18, 18, 22, 255));
        nvgBeginPath(vg);
        nvgCircle(vg, cx - 6.5f, cy - 6.5f, 2.4f);
        nvgCircle(vg, cx + 6.5f, cy + 6.5f, 2.4f);
        nvgCircle(vg, cx, cy, 2.4f);
        nvgCircle(vg, cx + 6.5f, cy - 6.5f, 2.4f);
        nvgCircle(vg, cx - 6.5f, cy + 6.5f, 2.4f);
        nvgFill(vg);
    } else if (icon_type_ == "rpg_jrpg") {
        // Magic Crystal Gem
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, cy - 18.0f);
        nvgLineTo(vg, cx + 15.0f, cy - 5.5f);
        nvgLineTo(vg, cx, cy + 18.0f);
        nvgLineTo(vg, cx - 15.0f, cy - 5.5f);
        nvgClosePath(vg);
        nvgFill(vg);
    } else if (icon_type_ == "shooters") {
        // Large Crosshair Target
        nvgStrokeWidth(vg, 3.0f);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 14.0f);
        nvgMoveTo(vg, cx - 20.0f, cy); nvgLineTo(vg, cx + 20.0f, cy);
        nvgMoveTo(vg, cx, cy - 20.0f); nvgLineTo(vg, cx, cy + 20.0f);
        nvgStroke(vg);
    } else if (icon_type_ == "simulation_cozy") {
        // Sprouting Leaf
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, cy + 15.0f);
        nvgQuadTo(vg, cx - 17.0f, cy - 3.5f, cx, cy - 17.0f);
        nvgQuadTo(vg, cx + 17.0f, cy - 3.5f, cx, cy + 15.0f);
        nvgFill(vg);
    } else if (icon_type_ == "strategy_tactics") {
        // Castle Battlements / Rook
        nvgBeginPath(vg);
        nvgRect(vg, cx - 12.0f, cy - 14.0f, 24.0f, 7.0f);
        nvgRect(vg, cx - 9.0f, cy - 7.0f, 18.0f, 14.0f);
        nvgRect(vg, cx - 13.0f, cy + 7.0f, 26.0f, 8.0f);
        nvgFill(vg);
    } else if (icon_type_ == "visual_novels") {
        // Open Book Pages
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 16.0f, cy - 11.0f, 15.0f, 22.0f, 3.0f);
        nvgRoundedRect(vg, cx + 1.0f, cy - 11.0f, 15.0f, 22.0f, 3.0f);
        nvgFill(vg);
    } else {
        // Generic Pro Gamepad
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 16.0f, cy - 9.0f, 32.0f, 18.0f, 4.5f);
        nvgFill(vg);
    }
}

void CollectionCard::draw(NVGcontext* vg, float x, float y, float width, float height,
                          brls::Style style, brls::FrameContext* ctx) {
    float targetScale = isFocused() ? 1.04f : 1.0f;
    scale_ += (targetScale - scale_) * 0.22f;
    float targetGlow = isFocused() ? 1.0f : 0.0f;
    glow_ += (targetGlow - glow_) * 0.22f;

    float cx = x + width * 0.5f;
    float cy = y + height * 0.5f;

    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgScale(vg, scale_, scale_);
    nvgTranslate(vg, -cx, -cy);

    // 1. Subtle Emerald Glow on focus
    if (glow_ > 0.01f) {
        NVGpaint glowPaint = nvgBoxGradient(vg, x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f,
                                            14.0f, 6.0f,
                                            nvgRGBA(0, 224, 165, static_cast<unsigned char>(65.0f * glow_)),
                                            nvgRGBA(0, 224, 165, 0));
        nvgBeginPath(vg);
        nvgRect(vg, x - 10.0f, y - 10.0f, width + 20.0f, height + 20.0f);
        nvgFillPaint(vg, glowPaint);
        nvgFill(vg);
    }

    // 2. Frosted Glass Base
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 14.0f);
    NVGcolor topColor = isFocused() ? nvgRGBA(0, 180, 140, 55) : nvgRGBA(28, 45, 66, 140);
    NVGcolor botColor = isFocused() ? nvgRGBA(10, 28, 45, 210) : nvgRGBA(12, 20, 32, 195);
    NVGpaint bgPaint = nvgLinearGradient(vg, x, y, x, y + height, topColor, botColor);
    nvgFillPaint(vg, bgPaint);
    nvgFill(vg);

    // 3. Top Gloss Sheen
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 1.0f, y + 1.0f, width - 2.0f, height * 0.45f, 13.0f);
    NVGpaint glossPaint = nvgLinearGradient(
        vg, x, y, x, y + height * 0.45f,
        nvgRGBA(255, 255, 255, static_cast<unsigned char>(isFocused() ? 45 : 30)),
        nvgRGBA(255, 255, 255, 0)
    );
    nvgFillPaint(vg, glossPaint);
    nvgFill(vg);

    // 4. Beveled Glass Border Stroke
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 14.0f);
    if (glow_ > 0.01f) {
        nvgStrokeColor(vg, nvgRGBA(0, 230, 175, static_cast<unsigned char>(240.0f * glow_)));
        nvgStrokeWidth(vg, 1.8f);
    } else {
        NVGpaint borderPaint = nvgLinearGradient(
            vg, x, y, x, y + height,
            nvgRGBA(180, 220, 225, 90),
            nvgRGBA(45, 80, 100, 35)
        );
        nvgStrokePaint(vg, borderPaint);
        nvgStrokeWidth(vg, 1.1f);
    }
    nvgStroke(vg);

    // 5. Draw the 76x76 Vector Emblem on the left
    float iconX = x + 12.0f;
    float iconY = y + (height - 76.0f) * 0.5f;
    drawIconGlyph(vg, iconX, iconY, 76.0f, 76.0f);

    nvgRestore(vg);

    // Draw children (labels)
    Box::draw(vg, x, y, width, height, style, ctx);
}

} // namespace ui
