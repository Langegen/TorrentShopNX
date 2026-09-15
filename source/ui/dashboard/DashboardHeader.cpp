#include "DashboardHeader.hpp"
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

    // Equalizer Emerald Icon
    brls::Label* eqIcon = new brls::Label();
    eqIcon->setText("i|i");
    eqIcon->setFontSize(36.0f);
    eqIcon->setTextColor(nvgRGBA(0, 224, 165, 255)); // Emerald Turquoise
    eqIcon->setMarginRight(12.0f);
    eqIcon->setSingleLine(true);
    leftBox->addView(eqIcon);

    // "TorrentShop"
    brls::Label* titleLabel = new brls::Label();
    titleLabel->setText("TorrentShop");
    titleLabel->setFontSize(32.0f);
    titleLabel->setTextColor(nvgRGBA(255, 255, 255, 255));
    titleLabel->setSingleLine(true);
    leftBox->addView(titleLabel);

    // "NX" Badge
    brls::Box* nxBadge = new brls::Box();
    nxBadge->setHeight(26.0f);
    nxBadge->setPadding(2.0f, 8.0f, 2.0f, 8.0f);
    nxBadge->setCornerRadius(6.0f);
    nxBadge->setBackgroundColor(nvgRGBA(0, 224, 165, 230)); // Emerald Turquoise
    nxBadge->setMarginLeft(12.0f);
    nxBadge->setAlignItems(brls::AlignItems::CENTER);
    nxBadge->setJustifyContent(brls::JustifyContent::CENTER);

    brls::Label* nxText = new brls::Label();
    nxText->setText("NX");
    nxText->setFontSize(15.0f);
    nxText->setTextColor(nvgRGBA(10, 16, 26, 255));
    nxText->setSingleLine(true);
    nxBadge->addView(nxText);
    leftBox->addView(nxBadge);

    // Version
    brls::Label* verLabel = new brls::Label();
    verLabel->setText(std::string("v") + config::ConfigManager::APP_VERSION);
    verLabel->setFontSize(15.0f);
    verLabel->setTextColor(nvgRGBA(140, 165, 190, 200));
    verLabel->setMarginLeft(14.0f);
    verLabel->setSingleLine(true);
    leftBox->addView(verLabel);

    this->addView(leftBox);

    // ================= RIGHT: Catalog Info & Storage (SD & NAND) =================
    rightBox_ = new brls::Box();
    rightBox_->setAxis(brls::Axis::COLUMN);
    rightBox_->setAlignItems(brls::AlignItems::FLEX_END);
    rightBox_->setPadding(8.0f, 16.0f, 8.0f, 16.0f);
    rightBox_->setCornerRadius(10.0f);
    rightBox_->setBackgroundColor(nvgRGBA(20, 38, 55, 110)); // Translucent glass

    // Row 1: Catalog Count & Last Update Date
    catalog_info_label_ = new brls::Label();
    catalog_info_label_->setText("");
    catalog_info_label_->setFontSize(13.0f);
    catalog_info_label_->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald Turquoise
    catalog_info_label_->setSingleLine(true);
    rightBox_->addView(catalog_info_label_);

    // Row 2: SD & NAND Storage (free / total)
    storage_info_label_ = new brls::Label();
    storage_info_label_->setText("SD: 0 GB / 0 GB  •  NAND: 0 GB / 0 GB");
    storage_info_label_->setFontSize(12.5f);
    storage_info_label_->setTextColor(nvgRGBA(180, 205, 225, 220));
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
