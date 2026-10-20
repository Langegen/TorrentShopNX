#include "RetroTargetPrompt.hpp"
#include "ThemeManager.hpp"
#include "../config/config.h"
#include "../catalog/retro_catalog_manager.h"
#include "../utils/app_paths.h"
#include <borealis.hpp>

namespace ui {

void promptRetroTargetSelection(const std::string& retro_console_id,
                                std::function<void(const std::string& target_mode)> onChosen) {
    if (retro_console_id.empty()) {
        if (onChosen) onChosen("");
        return;
    }

    auto& cfg = config::ConfigManager::instance();
    std::string mode = cfg.getRetroRomsMode();

    if (mode == "tico") {
        if (onChosen) onChosen("tico");
        return;
    }
    if (mode == "retroarch") {
        if (onChosen) onChosen("standalone");
        return;
    }
    if (mode == "downloads") {
        if (onChosen) onChosen("downloads");
        return;
    }
    if (mode == "custom") {
        if (onChosen) onChosen("custom");
        return;
    }

    // Default: "prompt" mode - Build interactive card dialog
    bool isTicoSupported = catalog::RetroCatalogManager::instance().isTicoSupported(retro_console_id);
    std::string ticoSlug = catalog::RetroCatalogManager::instance().getTicoSlug(retro_console_id);
    std::string ticoDisplayPath = "sdmc:/tico/roms/" + ticoSlug + "/";

    std::string stdSubfolder = catalog::RetroCatalogManager::instance().getEffectiveConsoleSubfolder(retro_console_id, false);
    std::string stdDisplayPath;
    if (cfg.getRetroRomsMode() == "custom" && !cfg.getRetroCustomPath().empty()) {
        stdDisplayPath = cfg.getRetroCustomPath();
        if (!stdDisplayPath.empty() && stdDisplayPath.back() != '/') stdDisplayPath += '/';
        stdDisplayPath += stdSubfolder + "/";
    } else {
        stdDisplayPath = "sdmc:/roms/" + stdSubfolder + "/";
    }

    bool isLight = ThemeManager::instance().isCurrentThemeLight();
    NVGcolor primaryText = ThemeManager::instance().getTextPrimaryColor();
    NVGcolor secondaryText = ThemeManager::instance().getTextSecondaryColor();

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(560.0f);
    content->setPadding(24.0f, 20.0f, 16.0f, 20.0f);
    content->setAlignItems(brls::AlignItems::STRETCH);

    // Header Title
    auto* titleLabel = new brls::Label();
    titleLabel->setText("app/retro/target_dialog_title"_i18n);
    titleLabel->setFontSize(20.0f);
    titleLabel->setTextColor(primaryText);
    titleLabel->setMarginBottom(4.0f);
    content->addView(titleLabel);

    // Subtitle
    auto* subtitleLabel = new brls::Label();
    subtitleLabel->setText("app/retro/target_dialog_subtitle"_i18n);
    subtitleLabel->setFontSize(13.5f);
    subtitleLabel->setTextColor(secondaryText);
    subtitleLabel->setMarginBottom(16.0f);
    content->addView(subtitleLabel);

    // Card 1: Tico Frontend
    auto* ticoCard = new brls::Box();
    ticoCard->setAxis(brls::Axis::COLUMN);
    ticoCard->setCornerRadius(10.0f);
    ticoCard->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    ticoCard->setMarginBottom(12.0f);
    ticoCard->setBackgroundColor(isLight ? nvgRGBA(124, 77, 255, 20) : nvgRGBA(124, 77, 255, 32));
    ticoCard->setBorderThickness(1.0f);
    ticoCard->setBorderColor(isLight ? nvgRGBA(124, 77, 255, 75) : nvgRGBA(124, 77, 255, 110));

    auto* ticoHeaderRow = new brls::Box();
    ticoHeaderRow->setAxis(brls::Axis::ROW);
    ticoHeaderRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    ticoHeaderRow->setAlignItems(brls::AlignItems::CENTER);
    ticoHeaderRow->setMarginBottom(4.0f);

    auto* ticoNameLabel = new brls::Label();
    ticoNameLabel->setText("Tico Frontend");
    ticoNameLabel->setFontSize(15.5f);
    ticoNameLabel->setTextColor(isLight ? nvgRGB(100, 45, 220) : nvgRGB(180, 150, 255));
    ticoHeaderRow->addView(ticoNameLabel);

    auto* ticoBadge = new brls::Box();
    ticoBadge->setPadding(2.0f, 8.0f, 2.0f, 8.0f);
    ticoBadge->setCornerRadius(4.0f);
    auto* ticoBadgeLabel = new brls::Label();
    ticoBadgeLabel->setFontSize(11.0f);

    if (isTicoSupported) {
        ticoBadge->setBackgroundColor(isLight ? nvgRGBA(56, 142, 60, 30) : nvgRGBA(76, 175, 80, 40));
        ticoBadgeLabel->setText("app/retro/tico_badge_ready"_i18n);
        ticoBadgeLabel->setTextColor(isLight ? nvgRGB(46, 125, 50) : nvgRGB(139, 195, 74));
    } else {
        ticoBadge->setBackgroundColor(isLight ? nvgRGBA(211, 47, 47, 30) : nvgRGBA(244, 67, 54, 40));
        ticoBadgeLabel->setText("app/retro/tico_badge_unsupported"_i18n);
        ticoBadgeLabel->setTextColor(isLight ? nvgRGB(211, 47, 47) : nvgRGB(244, 67, 54));
    }
    ticoBadge->addView(ticoBadgeLabel);
    ticoHeaderRow->addView(ticoBadge);
    ticoCard->addView(ticoHeaderRow);

    auto* ticoPathLabel = new brls::Label();
    ticoPathLabel->setText(ticoDisplayPath);
    ticoPathLabel->setFontSize(13.0f);
    ticoPathLabel->setTextColor(primaryText);
    ticoPathLabel->setMarginBottom(4.0f);
    ticoCard->addView(ticoPathLabel);

    auto* ticoHintLabel = new brls::Label();
    ticoHintLabel->setText(isTicoSupported ? "app/retro/tico_card_hint"_i18n : "app/retro/tico_card_hint_unsupported"_i18n);
    ticoHintLabel->setFontSize(12.0f);
    ticoHintLabel->setTextColor(secondaryText);
    ticoCard->addView(ticoHintLabel);

    content->addView(ticoCard);

    // Card 2: Standalone / RetroArch
    auto* stdCard = new brls::Box();
    stdCard->setAxis(brls::Axis::COLUMN);
    stdCard->setCornerRadius(10.0f);
    stdCard->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    stdCard->setMarginBottom(16.0f);
    stdCard->setBackgroundColor(isLight ? nvgRGBA(0, 0, 0, 14) : nvgRGBA(255, 255, 255, 12));
    stdCard->setBorderThickness(1.0f);
    stdCard->setBorderColor(isLight ? nvgRGBA(0, 0, 0, 35) : nvgRGBA(255, 255, 255, 25));

    auto* stdHeaderRow = new brls::Box();
    stdHeaderRow->setAxis(brls::Axis::ROW);
    stdHeaderRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    stdHeaderRow->setAlignItems(brls::AlignItems::CENTER);
    stdHeaderRow->setMarginBottom(4.0f);

    auto* stdNameLabel = new brls::Label();
    stdNameLabel->setText("Standalone / RetroArch");
    stdNameLabel->setFontSize(15.5f);
    stdNameLabel->setTextColor(primaryText);
    stdHeaderRow->addView(stdNameLabel);

    if (!isTicoSupported) {
        auto* recBadge = new brls::Box();
        recBadge->setPadding(2.0f, 8.0f, 2.0f, 8.0f);
        recBadge->setCornerRadius(4.0f);
        recBadge->setBackgroundColor(isLight ? nvgRGBA(25, 118, 210, 30) : nvgRGBA(33, 150, 243, 40));
        auto* recLabel = new brls::Label();
        recLabel->setText("app/retro/standalone_badge_recommended"_i18n);
        recLabel->setFontSize(11.0f);
        recLabel->setTextColor(isLight ? nvgRGB(25, 118, 210) : nvgRGB(33, 150, 243));
        recBadge->addView(recLabel);
        stdHeaderRow->addView(recBadge);
    }
    stdCard->addView(stdHeaderRow);

    auto* stdPathLabel = new brls::Label();
    stdPathLabel->setText(stdDisplayPath);
    stdPathLabel->setFontSize(13.0f);
    stdPathLabel->setTextColor(primaryText);
    stdPathLabel->setMarginBottom(4.0f);
    stdCard->addView(stdPathLabel);

    auto* stdHintLabel = new brls::Label();
    stdHintLabel->setText("app/retro/standalone_card_hint"_i18n);
    stdHintLabel->setFontSize(12.0f);
    stdHintLabel->setTextColor(secondaryText);
    stdCard->addView(stdHintLabel);

    content->addView(stdCard);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    auto* applet = dialog->getAppletFrame();
    if (applet) {
        applet->setWidth(600.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(28, 30, 38, 252));
    }

    dialog->addButton("app/retro/target_btn_tico"_i18n, [onChosen]() {
        if (onChosen) onChosen("tico");
    });

    dialog->addButton("app/retro/target_btn_standalone"_i18n, [onChosen]() {
        if (onChosen) onChosen("standalone");
    });

    dialog->addButton("app/common/cancel"_i18n, []() {});

    dialog->open();
}

} // namespace ui
