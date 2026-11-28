#include "VirtualKeyboard.hpp"
#include "ThemeManager.hpp"
#include "style/HeaderHints.hpp"
#include "style/UiTokens.hpp"
#include "../config/config.h"

#include <utility>

namespace ui {

util::kbd::Layout VirtualKeyboard::s_lastAlphaLayout = util::kbd::Layout::EN;

VirtualKeyboard::VirtualKeyboard(std::function<void(std::string)> onSubmit,
                                 const std::string& headerText,
                                 const std::string& subText,
                                 int maxStringLength,
                                 const std::string& initialText,
                                 int kbdDisableBitmask)
    : brls::Box(brls::Axis::COLUMN),
      onSubmit_(std::move(onSubmit)),
      headerText_(headerText),
      subText_(subText),
      maxLen_(maxStringLength > 0 ? maxStringLength : 256),
      kbdDisableBitmask_(kbdDisableBitmask),
      text_(util::kbd::clampUtf8Codepoints(initialText, maxStringLength > 0 ? maxStringLength : 256)) {
    layout_ = util::kbd::prefersLatinLayout(initialText) ? util::kbd::Layout::EN : s_lastAlphaLayout;
    setupUi();
    setupNavigationRoutes();
    setupShortcuts();
    updateKeyGrid();
    updateActionButtons();
    updateDisplay();
}

void VirtualKeyboard::open(std::function<void(std::string)> onSubmit,
                           const std::string& headerText,
                           const std::string& subText,
                           int maxStringLength,
                           const std::string& initialText,
                           int kbdDisableBitmask) {
    auto* kbd = new VirtualKeyboard(std::move(onSubmit), headerText, subText,
                                    maxStringLength, initialText, kbdDisableBitmask);
    brls::Application::pushActivity(new brls::Activity(kbd), brls::TransitionAnimation::FADE);
}

bool openTextInput(std::function<void(std::string)> onSubmit,
                   const std::string& headerText,
                   const std::string& subText,
                   int maxStringLength,
                   const std::string& initialText,
                   int kbdDisableBitmask) {
    if (config::ConfigManager::instance().getUseBuiltinKeyboard()) {
        VirtualKeyboard::open(std::move(onSubmit), headerText, subText,
                              maxStringLength, initialText, kbdDisableBitmask);
        return true;
    }

    auto* ime = brls::Application::getImeManager();
    if (ime) {
        return ime->openForText(std::move(onSubmit), headerText, subText,
                                maxStringLength, initialText, kbdDisableBitmask);
    }

    VirtualKeyboard::open(std::move(onSubmit), headerText, subText,
                          maxStringLength, initialText, kbdDisableBitmask);
    return true;
}

float VirtualKeyboard::getShowAnimationDuration(brls::TransitionAnimation animation) {
    return brls::View::getShowAnimationDuration(animation) / 2.0f;
}

brls::View* VirtualKeyboard::getDefaultFocus() {
    if (charKeys_[1][0]) return charKeys_[1][0];
    return brls::Box::getDefaultFocus();
}

void VirtualKeyboard::setupUi() {
    const bool isLight = ui::style::isLight();
    const NVGcolor panelBg = isLight ? nvgRGB(244, 246, 250) : nvgRGB(24, 29, 40);
    const NVGcolor panelBorder = isLight ? nvgRGB(205, 214, 226) : nvgRGBA(255, 255, 255, 42);
    const NVGcolor inputBg = isLight ? nvgRGB(255, 255, 255) : nvgRGB(14, 18, 27);
    const NVGcolor keyBg = isLight ? nvgRGB(255, 255, 255) : nvgRGB(42, 49, 66);
    const NVGcolor keyBorder = isLight ? nvgRGB(213, 221, 232) : nvgRGBA(255, 255, 255, 28);
    const NVGcolor actionBg = isLight ? nvgRGB(228, 234, 243) : nvgRGB(54, 63, 84);
    const NVGcolor accent = ThemeManager::instance().getAccentColor();

    setWidth(1280.0f);
    setHeight(720.0f);
    setJustifyContent(brls::JustifyContent::FLEX_END);
    setAlignItems(brls::AlignItems::CENTER);
    setBackgroundColor(nvgRGBA(0, 0, 0, 145));

    // Tap outside the keyboard panel dismisses without submitting.
    addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound*) {
        if (status.state == brls::GestureState::END) {
            if (panel_ && !panel_->getFrame().pointInside(status.position)) {
                cancelAndDismiss();
            }
        }
    }));

    panel_ = new brls::Box(brls::Axis::COLUMN);
    panel_->setWidth(1216.0f);
    panel_->setCornerRadius(18.0f);
    panel_->setPadding(18.0f, 22.0f, 14.0f, 22.0f);
    panel_->setMarginBottom(16.0f);
    panel_->setBackgroundColor(panelBg);
    panel_->setBorderColor(panelBorder);
    panel_->setBorderThickness(1.5f);
    addView(panel_);

    // Header row: title + active layout badge + character counter.
    auto* headerRow = new brls::Box(brls::Axis::ROW);
    headerRow->setAlignItems(brls::AlignItems::CENTER);
    headerRow->setMarginBottom(10.0f);
    panel_->addView(headerRow);

    titleLabel_ = new brls::Label();
    titleLabel_->setGrow(1.0f);
    titleLabel_->setSingleLine(true);
    titleLabel_->setFontSize(20.0f);
    titleLabel_->setTextColor(ui::style::textPrimary());
    titleLabel_->setText(headerText_.empty() ? "app/keyboard/placeholder"_i18n : headerText_);
    headerRow->addView(titleLabel_);

    auto* modePill = new brls::Box(brls::Axis::ROW);
    modePill->setPadding(4.0f, 12.0f, 4.0f, 12.0f);
    modePill->setCornerRadius(ui::style::kRadiusChip);
    modePill->setBackgroundColor(ThemeManager::instance().getAccentDimColor());
    modePill->setAlignItems(brls::AlignItems::CENTER);
    modePill->setJustifyContent(brls::JustifyContent::CENTER);
    headerRow->addView(modePill);

    modeBadge_ = new brls::Label();
    modeBadge_->setFontSize(ui::style::kFontCaption);
    modeBadge_->setSingleLine(true);
    modeBadge_->setTextColor(ui::style::textAccent());
    modePill->addView(modeBadge_);

    counterLabel_ = new brls::Label();
    counterLabel_->setFontSize(ui::style::kFontSub);
    counterLabel_->setSingleLine(true);
    counterLabel_->setMarginLeft(14.0f);
    counterLabel_->setTextColor(ui::style::textSecondary());
    headerRow->addView(counterLabel_);

    // Text preview box.
    auto* inputBox = new brls::Box(brls::Axis::ROW);
    inputBox->setHeight(56.0f);
    inputBox->setCornerRadius(10.0f);
    inputBox->setPadding(0.0f, 16.0f, 0.0f, 16.0f);
    inputBox->setMarginBottom(14.0f);
    inputBox->setAlignItems(brls::AlignItems::CENTER);
    inputBox->setBackgroundColor(inputBg);
    inputBox->setBorderColor(accent);
    inputBox->setBorderThickness(2.0f);
    panel_->addView(inputBox);

    inputLabel_ = new brls::Label();
    inputLabel_->setGrow(1.0f);
    inputLabel_->setSingleLine(true);
    inputLabel_->setAutoAnimate(false);
    inputLabel_->setFontSize(22.0f);
    inputLabel_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    inputLabel_->setVerticalAlign(brls::VerticalAlign::CENTER);
    inputBox->addView(inputLabel_);

    // 4x12 character rows.
    for (int r = 0; r < util::kbd::kRows; ++r) {
        auto* rowBox = new brls::Box(brls::Axis::ROW);
        rowBox->setHeight(58.0f);
        rowBox->setMarginBottom(6.0f);
        panel_->addView(rowBox);

        for (int c = 0; c < util::kbd::kCols; ++c) {
            auto* keyBox = new brls::Box(brls::Axis::ROW);
            keyBox->setGrow(1.0f);
            keyBox->setHeight(58.0f);
            keyBox->setMarginLeft(3.0f);
            keyBox->setMarginRight(3.0f);
            keyBox->setCornerRadius(8.0f);
            keyBox->setJustifyContent(brls::JustifyContent::CENTER);
            keyBox->setAlignItems(brls::AlignItems::CENTER);
            keyBox->setBackgroundColor(keyBg);
            keyBox->setBorderColor(keyBorder);
            keyBox->setBorderThickness(1.0f);
            keyBox->setFocusable(true);
            keyBox->setHighlightCornerRadius(8.0f);
            keyBox->setHighlightPadding(1.0f);

            auto* lbl = new brls::Label();
            lbl->setFontSize(20.0f);
            lbl->setSingleLine(true);
            lbl->setAutoAnimate(false);
            lbl->setHorizontalAlign(brls::HorizontalAlign::CENTER);
            lbl->setVerticalAlign(brls::VerticalAlign::CENTER);
            lbl->setTextColor(ui::style::textPrimary());
            keyBox->addView(lbl);

            // BUTTON_A automatically binds TapGestureRecognizer in Borealis.
            keyBox->registerAction("", brls::BUTTON_A, [this, r, c](brls::View*) {
                onCharKeyPressed(r, c);
                return true;
            }, true, true, brls::SOUND_CLICK);

            charKeys_[r][c] = keyBox;
            charLabels_[r][c] = lbl;
            rowBox->addView(keyBox);
        }
    }

    // 5th row: action buttons (Shift, EN/RU, #+=, Space, Clear, Backspace, Done).
    auto* actionRow = new brls::Box(brls::Axis::ROW);
    actionRow->setHeight(58.0f);
    actionRow->setMarginTop(2.0f);
    panel_->addView(actionRow);

    const float actionGrow[kActionCount] = { 1.6f, 1.4f, 1.4f, 4.2f, 1.4f, 1.6f, 2.0f };
    for (int i = 0; i < kActionCount; ++i) {
        auto* btn = new brls::Box(brls::Axis::ROW);
        btn->setGrow(actionGrow[i]);
        btn->setHeight(58.0f);
        btn->setMarginLeft(3.0f);
        btn->setMarginRight(3.0f);
        btn->setCornerRadius(8.0f);
        btn->setJustifyContent(brls::JustifyContent::CENTER);
        btn->setAlignItems(brls::AlignItems::CENTER);
        btn->setBackgroundColor(i == 6 ? accent : actionBg);
        btn->setBorderColor(keyBorder);
        btn->setBorderThickness(i == 6 ? 0.0f : 1.0f);
        btn->setFocusable(true);
        btn->setHighlightCornerRadius(8.0f);
        btn->setHighlightPadding(1.0f);

        auto* lbl = new brls::Label();
        lbl->setFontSize(17.0f);
        lbl->setSingleLine(true);
        lbl->setAutoAnimate(false);
        lbl->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        lbl->setVerticalAlign(brls::VerticalAlign::CENTER);
        lbl->setTextColor(i == 6 ? nvgRGB(255, 255, 255) : ui::style::textPrimary());
        btn->addView(lbl);

        actionKeys_[i] = btn;
        actionLabels_[i] = lbl;
        actionRow->addView(btn);
    }

    actionKeys_[0]->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        onShiftPressed();
        return true;
    }, true, false, brls::SOUND_CLICK);
    actionKeys_[1]->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        onLangTogglePressed();
        return true;
    }, true, false, brls::SOUND_CLICK);
    actionKeys_[2]->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        onSymbolsTogglePressed();
        return true;
    }, true, false, brls::SOUND_CLICK);
    actionKeys_[3]->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        onSpacePressed();
        return true;
    }, true, true, brls::SOUND_CLICK);
    actionKeys_[4]->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        onClearPressed();
        return true;
    }, true, false, brls::SOUND_CLICK);
    actionKeys_[5]->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        onBackspacePressed();
        return true;
    }, true, true, brls::SOUND_CLICK);
    actionKeys_[6]->registerAction("", brls::BUTTON_A, [this](brls::View*) {
        submitAndDismiss();
        return true;
    }, true, false, brls::SOUND_CLICK);

    actionLabels_[1]->setText("EN / RU");
    actionLabels_[3]->setText("app/keyboard/space"_i18n);
    actionLabels_[4]->setText("app/keyboard/clear"_i18n);
    actionLabels_[5]->setText("app/keyboard/hint_delete"_i18n);
    actionLabels_[6]->setText("app/keyboard/ok"_i18n);

    // Compact gamepad shortcut strip at the bottom of the panel.
    auto* hintStrip = new brls::Box(brls::Axis::ROW);
    hintStrip->setHeight(26.0f);
    hintStrip->setMarginTop(10.0f);
    hintStrip->setJustifyContent(brls::JustifyContent::CENTER);
    hintStrip->setAlignItems(brls::AlignItems::CENTER);
    panel_->addView(hintStrip);

    auto* hintLabel = new brls::Label();
    hintLabel->setFontSize(ui::style::kFontCaption);
    hintLabel->setSingleLine(true);
    hintLabel->setAutoAnimate(false);
    hintLabel->setTextColor(ui::style::textSecondary());
    hintLabel->setText(ui::style::headerMeta("", {
        ui::style::buttonHint(brls::BUTTON_A, "app/keyboard/hint_type"_i18n),
        ui::style::buttonHint(brls::BUTTON_Y, "app/keyboard/hint_delete"_i18n),
        ui::style::buttonHint(brls::BUTTON_X, "app/keyboard/hint_space"_i18n),
        ui::style::buttonHint(brls::BUTTON_LB, "app/keyboard/hint_shift"_i18n),
        ui::style::buttonHint(brls::BUTTON_RB, "app/keyboard/hint_layout"_i18n),
        ui::style::buttonHint(brls::BUTTON_START, "app/keyboard/hint_done"_i18n),
        ui::style::buttonHint(brls::BUTTON_B, "app/keyboard/hint_cancel"_i18n),
    }));
    hintStrip->addView(hintLabel);
}

void VirtualKeyboard::setupNavigationRoutes() {
    static constexpr int kBottomIdxForCol[util::kbd::kCols] = {
        0, 0, 1, 2, 3, 3, 3, 3, 4, 5, 6, 6
    };
    static constexpr int kColForAction[kActionCount] = {
        0, 2, 3, 5, 8, 9, 11
    };

    for (int r = 0; r < util::kbd::kRows; ++r) {
        for (int c = 0; c < util::kbd::kCols; ++c) {
            brls::Box* key = charKeys_[r][c];
            key->setCustomNavigationRoute(brls::FocusDirection::LEFT,
                charKeys_[r][(c + util::kbd::kCols - 1) % util::kbd::kCols]);
            key->setCustomNavigationRoute(brls::FocusDirection::RIGHT,
                charKeys_[r][(c + 1) % util::kbd::kCols]);
            key->setCustomNavigationRoute(brls::FocusDirection::UP,
                r > 0 ? charKeys_[r - 1][c] : actionKeys_[kBottomIdxForCol[c]]);
            key->setCustomNavigationRoute(brls::FocusDirection::DOWN,
                r + 1 < util::kbd::kRows ? charKeys_[r + 1][c] : actionKeys_[kBottomIdxForCol[c]]);
        }
    }

    for (int i = 0; i < kActionCount; ++i) {
        brls::Box* btn = actionKeys_[i];
        btn->setCustomNavigationRoute(brls::FocusDirection::LEFT,
            actionKeys_[(i + kActionCount - 1) % kActionCount]);
        btn->setCustomNavigationRoute(brls::FocusDirection::RIGHT,
            actionKeys_[(i + 1) % kActionCount]);
        btn->setCustomNavigationRoute(brls::FocusDirection::UP,
            charKeys_[util::kbd::kRows - 1][kColForAction[i]]);
        btn->setCustomNavigationRoute(brls::FocusDirection::DOWN,
            charKeys_[0][kColForAction[i]]);
    }
}

void VirtualKeyboard::setupShortcuts() {
    registerAction("", brls::BUTTON_Y, [this](brls::View*) {
        onBackspacePressed();
        return true;
    }, true, true, brls::SOUND_CLICK);

    registerAction("", brls::BUTTON_X, [this](brls::View*) {
        onSpacePressed();
        return true;
    }, true, true, brls::SOUND_CLICK);

    registerAction("", brls::BUTTON_LB, [this](brls::View*) {
        onShiftPressed();
        return true;
    }, true, false, brls::SOUND_CLICK);

    registerAction("", brls::BUTTON_RB, [this](brls::View*) {
        onCycleLayoutPressed();
        return true;
    }, true, false, brls::SOUND_CLICK);

    registerAction("", brls::BUTTON_BACK, [this](brls::View*) {
        onClearPressed();
        return true;
    }, true, false, brls::SOUND_CLICK);

    registerAction("", brls::BUTTON_START, [this](brls::View*) {
        submitAndDismiss();
        return true;
    }, true, false, brls::SOUND_CLICK);

    registerAction("", brls::BUTTON_B, [this](brls::View*) {
        cancelAndDismiss();
        return true;
    }, true, false, brls::SOUND_BACK);

    registerAction({ brls::BRLS_KBD_KEY_BACKSPACE }, [this](brls::View*) {
        onBackspacePressed();
        return true;
    }, true);

    registerAction({ brls::BRLS_KBD_KEY_ENTER }, [this](brls::View*) {
        submitAndDismiss();
        return true;
    }, false);

    registerAction({ brls::BRLS_KBD_KEY_ESCAPE }, [this](brls::View*) {
        cancelAndDismiss();
        return true;
    }, false);
}

void VirtualKeyboard::onCharKeyPressed(int row, int col) {
    const std::string token = util::kbd::keyText(layout_, isShifted(), row, col);
    if (!util::kbd::appendToken(text_, token, maxLen_, kbdDisableBitmask_)) {
        brls::Application::getAudioPlayer()->play(brls::SOUND_FOCUS_ERROR);
        return;
    }

    if (shiftMode_ == util::kbd::ShiftMode::ONCE) {
        shiftMode_ = util::kbd::ShiftMode::OFF;
        updateKeyGrid();
        updateActionButtons();
    }
    updateDisplay();
}

void VirtualKeyboard::onShiftPressed() {
    switch (shiftMode_) {
        case util::kbd::ShiftMode::OFF:
            shiftMode_ = util::kbd::ShiftMode::ONCE;
            break;
        case util::kbd::ShiftMode::ONCE:
            shiftMode_ = util::kbd::ShiftMode::LOCK;
            break;
        case util::kbd::ShiftMode::LOCK:
            shiftMode_ = util::kbd::ShiftMode::OFF;
            break;
    }
    updateKeyGrid();
    updateActionButtons();
    updateDisplay();
}

void VirtualKeyboard::onLangTogglePressed() {
    if (layout_ == util::kbd::Layout::SYM) {
        layout_ = s_lastAlphaLayout;
    } else {
        layout_ = (layout_ == util::kbd::Layout::EN) ? util::kbd::Layout::RU : util::kbd::Layout::EN;
        s_lastAlphaLayout = layout_;
    }
    updateKeyGrid();
    updateActionButtons();
    updateDisplay();
}

void VirtualKeyboard::onSymbolsTogglePressed() {
    if (layout_ == util::kbd::Layout::SYM) {
        layout_ = s_lastAlphaLayout;
    } else {
        layout_ = util::kbd::Layout::SYM;
    }
    updateKeyGrid();
    updateActionButtons();
    updateDisplay();
}

void VirtualKeyboard::onCycleLayoutPressed() {
    if (layout_ == util::kbd::Layout::EN) {
        layout_ = util::kbd::Layout::RU;
        s_lastAlphaLayout = util::kbd::Layout::RU;
    } else if (layout_ == util::kbd::Layout::RU) {
        layout_ = util::kbd::Layout::SYM;
    } else {
        layout_ = util::kbd::Layout::EN;
        s_lastAlphaLayout = util::kbd::Layout::EN;
    }
    updateKeyGrid();
    updateActionButtons();
    updateDisplay();
}

void VirtualKeyboard::onSpacePressed() {
    if (!util::kbd::appendToken(text_, " ", maxLen_, kbdDisableBitmask_)) {
        brls::Application::getAudioPlayer()->play(brls::SOUND_FOCUS_ERROR);
        return;
    }
    updateDisplay();
}

void VirtualKeyboard::onClearPressed() {
    if (text_.empty()) return;
    text_.clear();
    updateDisplay();
}

void VirtualKeyboard::onBackspacePressed() {
    if (text_.empty()) return;
    util::kbd::utf8PopBack(text_);
    updateDisplay();
}

void VirtualKeyboard::updateKeyGrid() {
    const NVGcolor normalText = ui::style::textPrimary();
    NVGcolor disabledText = ui::style::textSecondary();
    disabledText.a *= 0.45f;

    for (int r = 0; r < util::kbd::kRows; ++r) {
        for (int c = 0; c < util::kbd::kCols; ++c) {
            const char* tok = util::kbd::keyText(layout_, isShifted(), r, c);
            brls::Label* lbl = charLabels_[r][c];
            if (!lbl) continue;

            const size_t cpCount = util::kbd::utf8CodepointCount(tok);
            lbl->setFontSize(cpCount > 2 ? 14.0f : 20.0f);
            lbl->setText(tok);

            const bool disabled = util::kbd::isTokenDisabled(tok, kbdDisableBitmask_);
            lbl->setTextColor(disabled ? disabledText : normalText);
        }
    }
}

void VirtualKeyboard::updateActionButtons() {
    const bool isLight = ui::style::isLight();
    const NVGcolor actionBg = isLight ? nvgRGB(228, 234, 243) : nvgRGB(54, 63, 84);
    const NVGcolor accent = ThemeManager::instance().getAccentColor();
    const NVGcolor accentDim = ThemeManager::instance().getAccentMediumColor();

    if (actionKeys_[0] && actionLabels_[0]) {
        if (shiftMode_ == util::kbd::ShiftMode::LOCK) {
            actionKeys_[0]->setBackgroundColor(accent);
            actionLabels_[0]->setTextColor(nvgRGB(255, 255, 255));
            actionLabels_[0]->setText("CAPS");
        } else if (shiftMode_ == util::kbd::ShiftMode::ONCE) {
            actionKeys_[0]->setBackgroundColor(accentDim);
            actionLabels_[0]->setTextColor(ui::style::textPrimary());
            actionLabels_[0]->setText("SHIFT");
        } else {
            actionKeys_[0]->setBackgroundColor(actionBg);
            actionLabels_[0]->setTextColor(ui::style::textPrimary());
            actionLabels_[0]->setText("Shift");
        }
    }

    if (actionLabels_[2]) {
        actionLabels_[2]->setText(layout_ == util::kbd::Layout::SYM ? "ABC" : "#+=");
    }

    if (actionLabels_[3]) {
        NVGcolor spaceColor = ui::style::textPrimary();
        if (util::kbd::isTokenDisabled(" ", kbdDisableBitmask_)) {
            spaceColor = ui::style::textSecondary();
            spaceColor.a *= 0.45f;
        }
        actionLabels_[3]->setTextColor(spaceColor);
    }
}

void VirtualKeyboard::updateDisplay() {
    if (modeBadge_) {
        std::string badge = "EN";
        if (layout_ == util::kbd::Layout::RU) badge = "RU";
        else if (layout_ == util::kbd::Layout::SYM) badge = "#+=";

        if (shiftMode_ == util::kbd::ShiftMode::ONCE) badge += " · SHIFT";
        else if (shiftMode_ == util::kbd::ShiftMode::LOCK) badge += " · CAPS";
        modeBadge_->setText(badge);
    }

    const size_t len = util::kbd::utf8CodepointCount(text_);
    if (counterLabel_) {
        counterLabel_->setText(std::to_string(len) + " / " + std::to_string(maxLen_));
    }

    if (inputLabel_) {
        if (text_.empty()) {
            inputLabel_->setTextColor(ui::style::textSecondary());
            inputLabel_->setText(subText_.empty() ? "app/keyboard/placeholder"_i18n : subText_);
            inputLabel_->setCursor(static_cast<int>(brls::CursorPosition::START));
        } else {
            inputLabel_->setTextColor(ui::style::textPrimary());
            inputLabel_->setText(util::kbd::tailDisplaySlice(text_, 54));
            inputLabel_->setCursor(static_cast<int>(brls::CursorPosition::END));
        }
    }
}

void VirtualKeyboard::submitAndDismiss() {
    if (dismissed_) return;
    dismissed_ = true;

    std::string finalText = text_;
    auto cb = std::move(onSubmit_);
    onSubmit_ = nullptr;

    brls::Application::popActivity(brls::TransitionAnimation::FADE,
        [cb = std::move(cb), finalText = std::move(finalText)]() mutable {
            if (cb) {
                brls::sync([cb = std::move(cb), finalText = std::move(finalText)]() mutable {
                    cb(finalText);
                });
            }
        });
}

void VirtualKeyboard::cancelAndDismiss() {
    if (dismissed_) return;
    dismissed_ = true;
    onSubmit_ = nullptr;
    brls::Application::popActivity(brls::TransitionAnimation::FADE);
}

} // namespace ui
