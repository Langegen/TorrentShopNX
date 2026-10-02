#include "DashboardHeader.hpp"
#include "../ThemeManager.hpp"
#include "../StorageTabView.hpp"
#include "../../config/config.h"
#include "../../utils/log.h"

using namespace brls::literals;

namespace ui {

DashboardHeader::DashboardHeader() {
    this->setWidthPercentage(100.0f);
    this->setHeight(100.0f);
    this->setAxis(brls::Axis::ROW);
    this->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(12.0f, 40.0f, 6.0f, 40.0f);

    // ================= LEFT: Extra-Large Logo & Version Only =================
    brls::Box* leftBox = new brls::Box();
    leftBox->setAxis(brls::Axis::ROW);
    leftBox->setAlignItems(brls::AlignItems::CENTER);

    // Equalizer Icon
    NVGcolor textAccent = ThemeManager::instance().getTextAccentColor();
    NVGcolor accent = ThemeManager::instance().getAccentColor();

    eqIcon_ = new brls::Label();
    eqIcon_->setText("i|i");
    eqIcon_->setFontSize(36.0f);
    eqIcon_->setTextColor(textAccent);
    eqIcon_->setMarginRight(12.0f);
    eqIcon_->setSingleLine(true);
    leftBox->addView(eqIcon_);

    // "TorrentShop"
    titleLabel_ = new brls::Label();
    titleLabel_->setText("TorrentShop");
    titleLabel_->setFontSize(32.0f);
    titleLabel_->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLabel_->setSingleLine(true);
    leftBox->addView(titleLabel_);

    // "NX" Badge
    nxBadge_ = new brls::Box();
    nxBadge_->setHeight(26.0f);
    nxBadge_->setPadding(2.0f, 8.0f, 2.0f, 8.0f);
    nxBadge_->setCornerRadius(6.0f);
    nxBadge_->setBackgroundColor(accent);
    nxBadge_->setMarginLeft(12.0f);
    nxBadge_->setAlignItems(brls::AlignItems::CENTER);
    nxBadge_->setJustifyContent(brls::JustifyContent::CENTER);

    nxText_ = new brls::Label();
    nxText_->setText("NX");
    nxText_->setFontSize(15.0f);
    nxText_->setTextColor(ThemeManager::instance().isCurrentThemeLight() ? nvgRGB(255, 255, 255) : nvgRGB(10, 16, 26));
    nxText_->setSingleLine(true);
    nxBadge_->addView(nxText_);
    leftBox->addView(nxBadge_);

    // Version
    verLabel_ = new brls::Label();
    verLabel_->setText(std::string("v") + config::ConfigManager::APP_VERSION);
    verLabel_->setFontSize(15.0f);
    verLabel_->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    verLabel_->setMarginLeft(14.0f);
    verLabel_->setSingleLine(true);
    leftBox->addView(verLabel_);

    this->addView(leftBox);

    // ================= RIGHT: Catalog Info & Storage (SD & NAND) =================
    rightBox_ = new brls::Box();
    rightBox_->setAxis(brls::Axis::COLUMN);
    rightBox_->setAlignItems(brls::AlignItems::FLEX_END);
    rightBox_->setPadding(2.0f, 0.0f, 2.0f, 0.0f);
    rightBox_->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
    rightBox_->setBorderThickness(0.0f);

    // Row 1: Catalog Count & Last Update Date
    catalog_info_label_ = new brls::Label();
    catalog_info_label_->setText("");
    catalog_info_label_->setFontSize(13.0f);
    catalog_info_label_->setTextColor(textAccent);
    catalog_info_label_->setSingleLine(true);
    rightBox_->addView(catalog_info_label_);

    // Row 2: SD & NAND Storage (free / total)
    storage_info_label_ = new brls::Label();
    storage_info_label_->setText("SD: 0 GB / 0 GB  •  NAND: 0 GB / 0 GB");
    storage_info_label_->setFontSize(12.5f);
    storage_info_label_->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    storage_info_label_->setMarginTop(4.0f);
    storage_info_label_->setSingleLine(true);
    rightBox_->addView(storage_info_label_);

    rightBox_->setFocusable(false);
    rightBox_->addGestureRecognizer(new brls::TapGestureRecognizer(rightBox_, []() {
        auto* scroll = new brls::ScrollingFrame();
        scroll->setContentView(new StorageTabView());
        auto* applet = new brls::AppletFrame(scroll);
        applet->setTitle("app/dashboard/header_storage_title"_i18n);
        brls::Application::pushActivity(new brls::Activity(applet));
    }));

    this->addView(rightBox_);

    ThemeManager::instance().subscribe([this]() {
        refreshTheme();
    });
}

void DashboardHeader::refreshTheme() {
    NVGcolor textAccent = ThemeManager::instance().getTextAccentColor();
    NVGcolor accent = ThemeManager::instance().getAccentColor();
    bool isLight = ThemeManager::instance().isCurrentThemeLight();

    if (eqIcon_) {
        eqIcon_->setTextColor(textAccent);
    }
    if (titleLabel_) {
        titleLabel_->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    }
    if (nxBadge_) {
        nxBadge_->setBackgroundColor(accent);
    }
    if (nxText_) {
        nxText_->setTextColor(isLight ? nvgRGB(255, 255, 255) : nvgRGB(10, 16, 26));
    }
    if (catalog_info_label_) {
        catalog_info_label_->setTextColor(textAccent);
    }
    if (storage_info_label_) {
        storage_info_label_->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    }
    if (verLabel_) {
        verLabel_->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    }
}

void DashboardHeader::updateStats(int game_count, const std::string& catalog_updated_str,
                                  const std::string& sd_str, const std::string& nand_str) {
    if (catalog_info_label_) {
        std::string text = "app/dashboard/header_catalog_prefix"_i18n + std::to_string(game_count) + " " + "app/retro/unit_games"_i18n;
        if (!catalog_updated_str.empty()) {
            text += "app/dashboard/header_updated_prefix"_i18n + catalog_updated_str;
        }
        catalog_info_label_->setText(text);
    }
    if (storage_info_label_) {
        storage_info_label_->setText("SD: " + sd_str + "  •  NAND: " + nand_str);
    }
}

} // namespace ui
