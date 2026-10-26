#include "RetroConsolesView.hpp"
#include "RetroCatalogView.hpp"
#include "RetroUpdateDialog.hpp"
#include "RetroEmulatorsView.hpp"
#include "ThemeManager.hpp"
#include "../catalog/retro_emulator_manager.h"
#include "../catalog/retro_layout.h"
#include "../catalog/retro_readiness.h"
#include "../config/config.h"
#include "../utils/app_paths.h"
#include "../utils/log.h"
#include <algorithm>
#include <filesystem>

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
        updateHeader();
        statsHint->addGestureRecognizer(new brls::TapGestureRecognizer(statsHint, [this]() {
            showUpdateDialog();
        }));
    }

    // Register emulators manager action (Y)
    this->registerAction("app/retro/emulators_action"_i18n, brls::ControllerButton::BUTTON_Y, [](brls::View*) {
        brls::Application::pushActivity(new RetroEmulatorsView());
        return true;
    });

    // Register update action (-)
    this->registerAction("app/retro/action_update_bases"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        showUpdateDialog();
        return true;
    });

    rebuildGrid();
    refreshGameCounts();
    refreshStatuses();

    // Once per launch: bring folders written by older versions to the current
    // layout, and offer to move PS Vita / Wii U games to their emulators.
    static bool s_checkedLegacyLayout = false;
    if (!s_checkedLegacyLayout) {
        s_checkedLegacyLayout = true;
        std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
        brls::async([flag]() {
            catalog::retro::migrateLegacyTicoLayout();
            if (catalog::retro::wasLegacyStandaloneOfferAnswered()) return;
            catalog::retro::LegacyStandaloneGames legacy = catalog::retro::findLegacyStandaloneGames();
            if (legacy.total() == 0) return;
            brls::sync([flag, legacy]() {
                if (!flag->load()) return;
                std::string msg = "app/retro/legacy_move_title"_i18n + "\n\n" +
                                  brls::getStr("app/retro/legacy_move_desc",
                                               std::to_string(legacy.vita), std::to_string(legacy.wiiu));
                auto* dialog = new brls::Dialog(msg);
                dialog->setCancelable(true);
                dialog->addButton("app/retro/legacy_move_btn"_i18n, []() {
                    brls::async([]() {
                        int moved = catalog::retro::moveLegacyStandaloneGames();
                        catalog::retro::markLegacyStandaloneOfferAnswered();
                        brls::sync([moved]() {
                            brls::Application::notify(brls::getStr("app/retro/legacy_moved_notify", std::to_string(moved)));
                        });
                    });
                });
                dialog->addButton("app/retro/legacy_keep_btn"_i18n, []() {
                    catalog::retro::markLegacyStandaloneOfferAnswered();
                });
                dialog->open();
            });
        });
    }
}

void RetroConsolesView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    rebuildGrid();
    refreshGameCounts();
    refreshStatuses();
    updateHeader();
}

void RetroConsolesView::showUpdateDialog() {
    showRetroCatalogUpdateDialog([this](int updatedCount) {
        if (updatedCount > 0) {
            refreshGameCounts();
            updateHeader();
        }
    });
}

// "Systems: 20 • Games in the base: 8214" from the cached bases, not a fixed text.
void RetroConsolesView::updateHeader() {
    if (!statsHint) return;
    auto& mgr = catalog::RetroCatalogManager::instance();
    std::string systems = std::to_string(mgr.consoles().size());
    int games = mgr.getTotalCachedGamesCount();
    std::string text = games > 0
        ? brls::getStr("app/retro/consoles_stats_fmt", systems, std::to_string(games))
        : brls::getStr("app/retro/consoles_count_fmt", systems);
    statsHint->setText(text + "app/retro/consoles_stats_suffix"_i18n);
}

// What each console still needs, shown under its name. Reads the SD card, so off the UI thread.
void RetroConsolesView::refreshStatuses() {
    std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
    std::vector<std::string> ids;
    for (const auto& c : catalog::RetroCatalogManager::instance().consoles()) ids.push_back(c.id);

    brls::async([this, flag, ids]() {
        struct Status {
            std::string id;
            std::string key;
            int kind; // 0 ready, 1 attention, 2 neutral
        };
        std::vector<Status> statuses;
        for (const auto& id : ids) {
            if (!flag->load()) return;
            catalog::RetroReadiness r = catalog::checkRetroReadiness(id);
            if (!r.systemFilesReady()) {
                statuses.push_back({id, id == "psvita" ? "app/retro/card_need_fw" : "app/retro/card_need_bios", 1});
            } else if (!r.emulatorReady()) {
                statuses.push_back({id, "app/retro/card_auto_install", 2});
            } else {
                statuses.push_back({id, "app/retro/card_ready", 0});
            }
        }
        brls::sync([this, flag, statuses]() {
            if (!flag->load()) return;
            auto& mgr = catalog::RetroCatalogManager::instance();
            bool isLight = ThemeManager::instance().isCurrentThemeLight();
            for (const auto& st : statuses) {
                auto it = console_cards_.find(st.id);
                const auto* info = mgr.findConsole(st.id);
                if (it == console_cards_.end() || !it->second || !info) continue;
                std::string runner = info->route == catalog::RetroRoute::Tico ? std::string("Tico") : mgr.getRunnerLabel(st.id);
                it->second->setDesc(info->release_year + " • " + runner + " • " + brls::getStr(st.key));
                if (st.kind == 0) {
                    it->second->setDescColor(isLight ? nvgRGBA(22, 128, 61, 255) : nvgRGBA(76, 217, 100, 255));
                } else if (st.kind == 1) {
                    it->second->setDescColor(isLight ? nvgRGB(194, 65, 12) : nvgRGB(255, 183, 77));
                } else {
                    it->second->setDescColor(ThemeManager::instance().getTextSecondaryColor());
                }
            }
        });
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
                            card->setCountText(std::to_string(count) + " " + "app/retro/unit_games"_i18n);
                        } else {
                            card->setCountText("app/retro/catalog_badge"_i18n);
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
    if (nintendoTitle.empty() || nintendoTitle == "app/retro/section_nintendo") nintendoTitle = "Nintendo";
    std::string sonyTitle = brls::getStr("app/retro/section_sony");
    if (sonyTitle.empty() || sonyTitle == "app/retro/section_sony") sonyTitle = "Sony PlayStation";
    std::string segaTitle = brls::getStr("app/retro/section_sega");
    if (segaTitle.empty() || segaTitle == "app/retro/section_sega") segaTitle = "Sega";

    bool isLightConsoles = ThemeManager::instance().isCurrentThemeLight();

    std::vector<BrandSection> sections = {
        {"Nintendo", nintendoTitle, isLightConsoles ? nvgRGB(210, 20, 20) : nvgRGBA(230, 0, 18, 240)},
        {"Sony",     sonyTitle,     isLightConsoles ? nvgRGB(2, 132, 199) : nvgRGBA(33, 150, 243, 240)},
        {"Sega",     segaTitle,     isLightConsoles ? nvgRGB(13, 148, 136) : nvgRGBA(0, 224, 165, 240)}
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
            std::string countStr = (cached > 0) ? (std::to_string(cached) + " " + "app/retro/unit_games"_i18n) : "app/retro/catalog_badge"_i18n;

            // The status ("ready", "needs a BIOS"...) is added by refreshStatuses().
            std::string runner = (c.route == catalog::RetroRoute::Tico) ? std::string("Tico") : mgr.getRunnerLabel(c.id);
            std::string desc = c.release_year + " • " + runner;

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
