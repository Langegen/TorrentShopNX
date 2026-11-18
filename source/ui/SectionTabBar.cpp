#include "SectionTabBar.hpp"
#include "ThemeManager.hpp"
#include "homebrew/HomebrewGlue.hpp"
#include "style/UiTokens.hpp"

namespace ui {

SectionTabBar::SectionTabBar(SectionTab activeTab) {
    this->setAxis(brls::Axis::ROW);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setHeight(38.0f);
    this->setFocusable(false);

    buildTabs(activeTab);
}

void SectionTabBar::buildTabs(SectionTab activeTab) {
    const auto accent = ThemeManager::instance().getAccentColor();
    const auto cardBg = ThemeManager::instance().getCardBgColor();
    const auto cardBorder = ThemeManager::instance().getCardBorderColor();
    const auto textSec = style::textSecondary();

    // 1. [L] bumper glyph
    auto* hintL = new brls::Label();
    hintL->setText(brls::Hint::getKeyIcon(brls::ControllerButton::BUTTON_LB));
    hintL->setFontSize(18.0f);
    hintL->setMarginRight(8.0f);
    hintL->setFocusable(false);
    hintL->setTextColor(activeTab != SectionTab::Homebrew ? style::textPrimary() : textSec);
    if (activeTab == SectionTab::Retro) {
        hintL->addGestureRecognizer(new brls::TapGestureRecognizer(hintL, [] {
            hb::showHomebrewSection();
        }));
    } else if (activeTab == SectionTab::PcGames) {
        hintL->addGestureRecognizer(new brls::TapGestureRecognizer(hintL, [] {
            hb::showRetroSection();
        }));
    }
    this->addView(hintL);

    // 2. Pill container
    auto* pillBox = new brls::Box();
    pillBox->setAxis(brls::Axis::ROW);
    pillBox->setAlignItems(brls::AlignItems::CENTER);
    pillBox->setHeight(38.0f);
    pillBox->setCornerRadius(19.0f);
    pillBox->setBackgroundColor(cardBg);
    pillBox->setBorderThickness(1.0f);
    pillBox->setBorderColor(cardBorder);
    pillBox->setPadding(3.0f, 3.0f, 3.0f, 3.0f);
    pillBox->setFocusable(false);

    // Tab 1: Homebrew
    auto* tabHb = new brls::Box();
    tabHb->setHeight(32.0f);
    tabHb->setCornerRadius(16.0f);
    tabHb->setPadding(0.0f, 18.0f, 0.0f, 18.0f);
    tabHb->setAlignItems(brls::AlignItems::CENTER);
    tabHb->setJustifyContent(brls::JustifyContent::CENTER);
    tabHb->setFocusable(false);

    auto* lblHb = new brls::Label();
    lblHb->setText(brls::getStr("app/homebrew/title"));
    lblHb->setFontSize(17.0f);
    lblHb->setSingleLine(true);
    tabHb->addView(lblHb);

    if (activeTab == SectionTab::Homebrew) {
        tabHb->setBackgroundColor(accent);
        lblHb->setTextColor(hb::textOn(accent));
    } else {
        tabHb->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        lblHb->setTextColor(textSec);
        tabHb->addGestureRecognizer(new brls::TapGestureRecognizer(tabHb, [] {
            hb::showHomebrewSection();
        }));
    }
    pillBox->addView(tabHb);

    // Tab 2: Retro
    auto* tabRetro = new brls::Box();
    tabRetro->setHeight(32.0f);
    tabRetro->setCornerRadius(16.0f);
    tabRetro->setPadding(0.0f, 18.0f, 0.0f, 18.0f);
    tabRetro->setAlignItems(brls::AlignItems::CENTER);
    tabRetro->setJustifyContent(brls::JustifyContent::CENTER);
    tabRetro->setFocusable(false);

    auto* lblRetro = new brls::Label();
    lblRetro->setText(brls::getStr("app/homebrew/tab_retro"));
    lblRetro->setFontSize(17.0f);
    lblRetro->setSingleLine(true);
    tabRetro->addView(lblRetro);

    if (activeTab == SectionTab::Retro) {
        tabRetro->setBackgroundColor(accent);
        lblRetro->setTextColor(hb::textOn(accent));
    } else {
        tabRetro->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        lblRetro->setTextColor(textSec);
        tabRetro->addGestureRecognizer(new brls::TapGestureRecognizer(tabRetro, [] {
            hb::showRetroSection();
        }));
    }
    pillBox->addView(tabRetro);

    // Tab 3: PC Games
    auto* tabPc = new brls::Box();
    tabPc->setHeight(32.0f);
    tabPc->setCornerRadius(16.0f);
    tabPc->setPadding(0.0f, 18.0f, 0.0f, 18.0f);
    tabPc->setAlignItems(brls::AlignItems::CENTER);
    tabPc->setJustifyContent(brls::JustifyContent::CENTER);
    tabPc->setFocusable(false);

    auto* lblPc = new brls::Label();
    lblPc->setText(brls::getStr("app/homebrew/tab_pc"));
    lblPc->setFontSize(17.0f);
    lblPc->setSingleLine(true);
    tabPc->addView(lblPc);

    if (activeTab == SectionTab::PcGames) {
        tabPc->setBackgroundColor(accent);
        lblPc->setTextColor(hb::textOn(accent));
    } else {
        tabPc->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        lblPc->setTextColor(textSec);
        tabPc->addGestureRecognizer(new brls::TapGestureRecognizer(tabPc, [] {
            hb::showPcSection();
        }));
    }
    pillBox->addView(tabPc);

    this->addView(pillBox);

    // 3. [R] bumper glyph
    auto* hintR = new brls::Label();
    hintR->setText(brls::Hint::getKeyIcon(brls::ControllerButton::BUTTON_RB));
    hintR->setFontSize(18.0f);
    hintR->setMarginLeft(8.0f);
    hintR->setFocusable(false);
    hintR->setTextColor(activeTab != SectionTab::PcGames ? style::textPrimary() : textSec);
    if (activeTab == SectionTab::Homebrew) {
        hintR->addGestureRecognizer(new brls::TapGestureRecognizer(hintR, [] {
            hb::showRetroSection();
        }));
    } else if (activeTab == SectionTab::Retro) {
        hintR->addGestureRecognizer(new brls::TapGestureRecognizer(hintR, [] {
            hb::showPcSection();
        }));
    }
    this->addView(hintR);
}

} // namespace ui
