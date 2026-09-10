#include "RetroConsolesView.hpp"
#include "RetroCatalogView.hpp"
#include "RetroUpdateDialog.hpp"
#include "../utils/log.h"
#include <algorithm>

namespace ui {

namespace {

brls::Box* createSectionDivider(const std::string& title_text, NVGcolor bar_color) {
    brls::Box* headerBox = new brls::Box();
    headerBox->setFocusable(false);
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setWidthPercentage(100.0f);
    headerBox->setMarginTop(14.0f);
    headerBox->setMarginBottom(10.0f);

    brls::Box* bar = new brls::Box();
    bar->setWidth(4.0f);
    bar->setHeight(18.0f);
    bar->setCornerRadius(2.0f);
    bar->setBackgroundColor(bar_color);
    bar->setMarginRight(8.0f);
    headerBox->addView(bar);

    brls::Label* lbl = new brls::Label();
    lbl->setText(title_text);
    lbl->setFontSize(14.0f);
    lbl->setTextColor(bar_color);
    headerBox->addView(lbl);

    return headerBox;
}

} // namespace

RetroConsolesView::RetroConsolesView()
    : alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
}

RetroConsolesView::~RetroConsolesView() {
    *alive_flag_ = false;
}

void RetroConsolesView::onContentAvailable() {
    if (scroll) {
        scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    if (titleLabel) {
        titleLabel->setText(brls::getStr("app/retro/consoles_title"));
    }
    if (statsHint) {
        statsHint->setText(brls::getStr("app/retro/consoles_stats") + "  (-) Обновить базы");
        statsHint->addGestureRecognizer(new brls::TapGestureRecognizer(statsHint, [this]() {
            showUpdateDialog();
        }));
    }

    // Register update action (-)
    this->registerAction("Обновить базы", brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        showUpdateDialog();
        return true;
    });

    rebuildGrid();
    refreshGameCounts();
}

void RetroConsolesView::showUpdateDialog() {
    showRetroCatalogUpdateDialog([this](int updatedCount) {
        if (updatedCount > 0) {
            refreshGameCounts();
        }
    });
}

void RetroConsolesView::refreshGameCounts() {
    auto& mgr = catalog::RetroCatalogManager::instance();
    for (const auto& c : mgr.consoles()) {
        std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
        std::string cid = c.id;
        auto it = console_cards_.find(cid);
        CollectionCard* card = (it != console_cards_.end()) ? it->second : nullptr;

        brls::async([flag, card, cid, &mgr]() {
            int count = mgr.getCachedGameCount(cid);
            if (flag->load() && card) {
                brls::sync([flag, card, count]() {
                    if (flag->load() && card) {
                        if (count > 0) {
                            card->setCountText(std::to_string(count) + " игр");
                        } else {
                            card->setCountText("Каталог");
                        }
                    }
                });
            }
        });
    }
}

void RetroConsolesView::rebuildGrid() {
    if (!listBox) return;
    listBox->clearViews();
    console_cards_.clear();
    grid_.clear();

    auto& mgr = catalog::RetroCatalogManager::instance();
    const auto& consoles = mgr.consoles();

    struct BrandSection {
        std::string brand;
        std::string title;
        NVGcolor color;
    };

    std::string nintendoTitle = brls::getStr("app/retro/section_nintendo");
    if (nintendoTitle.empty() || nintendoTitle == "app/retro/section_nintendo") nintendoTitle = "Nintendo (10 платформ)";
    std::string sonyTitle = brls::getStr("app/retro/section_sony");
    if (sonyTitle.empty() || sonyTitle == "app/retro/section_sony") sonyTitle = "Sony PlayStation (4 платформы)";
    std::string segaTitle = brls::getStr("app/retro/section_sega");
    if (segaTitle.empty() || segaTitle == "app/retro/section_sega") segaTitle = "Sega (6 платформ)";

    std::vector<BrandSection> sections = {
        {"Nintendo", nintendoTitle, nvgRGBA(230, 0, 18, 240)},
        {"Sony",     sonyTitle,     nvgRGBA(33, 150, 243, 240)},
        {"Sega",     segaTitle,     nvgRGBA(0, 224, 165, 240)}
    };

    for (const auto& sec : sections) {
        listBox->addView(createSectionDivider(sec.title, sec.color));

        brls::Box* currentRow = nullptr;
        std::vector<CollectionCard*> currentGridRow;

        for (const auto& c : consoles) {
            if (c.brand != sec.brand) continue;

            if (currentGridRow.empty()) {
                currentRow = new brls::Box();
                currentRow->setAxis(brls::Axis::ROW);
                currentRow->setJustifyContent(brls::JustifyContent::FLEX_START);
                currentRow->setWidthPercentage(100.0f);
                currentRow->setMarginBottom(12.0f);
                listBox->addView(currentRow);
            }

            int cached = mgr.getCachedGameCount(c.id);
            std::string countStr = (cached > 0) ? (std::to_string(cached) + " игр") : "Каталог";

            std::string desc = c.release_year + " • " + c.recommended_emulator;

            catalog::RetroConsoleInfo cInfo = c;
            CollectionCard* card = new CollectionCard(
                c.id,
                c.name,
                desc,
                countStr,
                c.color,
                [cInfo]() {
                    brls::Application::pushActivity(new RetroCatalogView(cInfo));
                }
            );

            if (currentGridRow.size() < 3) {
                card->setMarginRight(16.0f);
            }

            console_cards_[c.id] = card;
            currentRow->addView(card);
            currentGridRow.push_back(card);

            if (currentGridRow.size() == 4) {
                grid_.push_back(currentGridRow);
                currentGridRow.clear();
                currentRow = nullptr;
            }
        }

        if (!currentGridRow.empty() && currentRow) {
            grid_.push_back(currentGridRow);
        }
    }

    // Configure directional navigation routes
    for (size_t r = 0; r < grid_.size(); ++r) {
        for (size_t c = 0; c < grid_[r].size(); ++c) {
            CollectionCard* card = grid_[r][c];
            if (!card) continue;

            if (r > 0) {
                size_t targetCol = std::min(c, grid_[r - 1].size() - 1);
                card->setCustomNavigationRoute(brls::FocusDirection::UP, grid_[r - 1][targetCol]);
            }
            if (r + 1 < grid_.size()) {
                size_t targetCol = std::min(c, grid_[r + 1].size() - 1);
                card->setCustomNavigationRoute(brls::FocusDirection::DOWN, grid_[r + 1][targetCol]);
            }
            if (c > 0) {
                card->setCustomNavigationRoute(brls::FocusDirection::LEFT, grid_[r][c - 1]);
            }
            if (c + 1 < grid_[r].size()) {
                card->setCustomNavigationRoute(brls::FocusDirection::RIGHT, grid_[r][c + 1]);
            }
        }
    }

    util::logLine("RetroConsolesView: successfully built grid with " + std::to_string(grid_.size()) + " rows");
}

} // namespace ui
