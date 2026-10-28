#include "EmptyState.hpp"
#include "style/UiTokens.hpp"

namespace ui {

EmptyState::EmptyState() {
    this->setAxis(brls::Axis::COLUMN);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setJustifyContent(brls::JustifyContent::CENTER);
    this->setGrow(1.0f);
    this->setPaddingBottom(40.0f);

    iconHolder_ = new brls::Box();
    iconHolder_->setWidth(112.0f);
    iconHolder_->setHeight(112.0f);
    this->addView(iconHolder_);

    title_ = new brls::Label();
    title_->setFontSize(24.0f);
    title_->setTextColor(style::textPrimary());
    title_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title_->setSingleLine(true);
    title_->setMarginTop(20.0f);
    this->addView(title_);

    sub_ = new brls::Label();
    sub_->setFontSize(17.0f);
    sub_->setLineHeight(1.35f);
    sub_->setTextColor(style::textSecondary());
    sub_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    sub_->setWidth(560.0f);
    sub_->setMaxLines(2);
    sub_->setAutoAnimate(false);
    sub_->setMarginTop(8.0f);
    this->addView(sub_);

    button_ = new brls::Button();
    button_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    button_->setWidth(300.0f);
    button_->setHeight(48.0f);
    button_->setMarginTop(28.0f);
    button_->setVisibility(brls::Visibility::GONE);
    button_->registerClickAction([this](brls::View*) {
        if (action_) action_();
        return true;
    });
    this->addView(button_);
}

void EmptyState::show(DashboardIcon::Kind kind, const std::string& title, const std::string& sub,
                      const std::string& actionLabel, std::function<void()> action) {
    iconHolder_->clearViews();
    iconHolder_->addView(new DashboardIcon(kind, 112.0f));

    title_->setText(title);
    sub_->setText(sub);
    sub_->setVisibility(sub.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

    action_ = std::move(action);
    const bool hasAction = !actionLabel.empty() && action_;
    if (hasAction) {
        button_->setText(actionLabel);
        button_->setVisibility(brls::Visibility::VISIBLE);
    } else {
        button_->setVisibility(brls::Visibility::GONE);
    }
    this->setFocusable(!hasAction);
    this->setHideHighlight(true);
}

brls::View* EmptyState::actionView() {
    if (button_ && button_->getVisibility() == brls::Visibility::VISIBLE) return button_;
    return this;
}

} // namespace ui
