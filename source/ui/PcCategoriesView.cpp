#include "PcCategoriesView.hpp"
#include "PcCatalogView.hpp"
#include "SectionTabBar.hpp"
#include "ThemeManager.hpp"
#include "homebrew/HomebrewGlue.hpp"
#include "style/UiTokens.hpp"
#include "../config/config.h"
#include "../utils/log.h"
#include <vector>
#include <string>

namespace ui {

namespace {

brls::Box* createSectionDivider(const std::string& title_text) {
    brls::Box* headerBox = new brls::Box();
    headerBox->setFocusable(false);
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setWidthPercentage(100.0f);
    headerBox->setMarginTop(12.0f);
    headerBox->setMarginBottom(10.0f);

    brls::Box* bar = new brls::Box();
    bar->setWidth(4.0f);
    bar->setHeight(18.0f);
    bar->setCornerRadius(2.0f);
    bar->setBackgroundColor(ThemeManager::instance().getAccentColor());
    bar->setMarginRight(8.0f);
    headerBox->addView(bar);

    brls::Label* lbl = new brls::Label();
    lbl->setText(title_text);
    lbl->setFontSize(14.0f);
    lbl->setLetterSpacing(0.8f);
    lbl->setTextColor(ThemeManager::instance().getAccentColor());
    headerBox->addView(lbl);

    return headerBox;
}

NVGcolor getSubforumColor(const catalog::PcSubforum& sf) {
    if (sf.is_retro) return nvgRGB(212, 110, 48); // warm vintage amber/orange
    if (sf.slug == "racing") return nvgRGB(230, 74, 25);
    if (sf.slug == "arcade") return nvgRGB(156, 39, 176);
    if (sf.slug == "puzzle") return nvgRGB(0, 150, 136);
    if (sf.slug == "adventure") return nvgRGB(30, 136, 229);
    if (sf.slug == "rpg") return nvgRGB(142, 36, 170);
    if (sf.slug == "simulators") return nvgRGB(67, 160, 71);
    if (sf.slug == "horror") return nvgRGB(198, 40, 40);
    if (sf.parent == "Стратегии" || sf.slug.find("strategy") != std::string::npos) return nvgRGB(0, 137, 123);
    if (sf.parent == "Экшены" || sf.slug.find("action") != std::string::npos) return nvgRGB(216, 27, 96);
    if (sf.slug == "collections") return nvgRGB(245, 124, 0);
    return ThemeManager::instance().getAccentColor();
}

} // namespace

PcCategoriesView::PcCategoriesView()
    : alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
}

PcCategoriesView::~PcCategoriesView() {
    if (alive_flag_) *alive_flag_ = false;
}

void PcCategoriesView::onContentAvailable() {
    if (scroll) {
        scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    config::ConfigManager::instance().setHomebrewSection("pc");
    if (tabsBox) {
        tabsBox->clearViews();
        tabsBox->addView(new SectionTabBar(SectionTab::PcGames));
    }

    // Directional navigation: LB switches to Retro, RB is rightmost (none)
    this->registerAction("", brls::ControllerButton::BUTTON_LB, [](brls::View*) {
        hb::showRetroSection();
        return true;
    }, true);
    this->registerAction("", brls::ControllerButton::BUTTON_RB, [](brls::View*) {
        return true;
    }, true);

    // Register update action (-)
    this->registerAction("app/pc/update_catalog_btn"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        showUpdateDialog();
        return true;
    });

    if (statsHint) {
        updateHeader();
        statsHint->addGestureRecognizer(new brls::TapGestureRecognizer(statsHint, [this]() {
            showUpdateDialog();
        }));
    }

    // 1. Load subforums fast (instant from disk or memory)
    catalog::PcCatalogManager::instance().loadSubforums(false);
    rebuildGrid();
    updateHeader();

    // 2. Check catalog load / download status
    if (!catalog::PcCatalogManager::instance().isLoaded()) {
        if (!catalog::PcCatalogManager::instance().hasLocalCache()) {
            loadCatalogWithProgress();
        } else {
            // Load binary/JSON cache in background, NEVER freezing UI thread!
            std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
            brls::async([this, flag]() {
                bool ok = catalog::PcCatalogManager::instance().loadCatalog(false);
                if (ok) {
                    brls::sync([this, flag]() {
                        if (!flag || !flag->load()) return;
                        rebuildGrid();
                        updateHeader();
                    });
                }
            });
        }
    }
}

void PcCategoriesView::willAppear(bool resetState) {
    Activity::willAppear(resetState);

    // Check auto-update once a day
    if (catalog::PcCatalogManager::instance().isLoaded() &&
        catalog::PcCatalogManager::instance().shouldAutoUpdate()) {
        std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
        catalog::PcCatalogManager::instance().updateCatalogInBackground([this, flag](bool ok) {
            if (!flag || !flag->load() || !ok) return;
            rebuildGrid();
            updateHeader();
        });
    }
}

void PcCategoriesView::updateHeader() {
    if (!statsHint) return;
    size_t count = catalog::PcCatalogManager::instance().getGamesCount();
    if (count == 0) {
        count = catalog::PcCatalogManager::instance().getTotalKnownGamesCount();
    }
    if (count == 0) {
        statsHint->setText(brls::getStr("app/pc/stats_hint_update"));
    } else {
        std::string text = brls::getStr("app/pc/stats_hint", std::to_string(count)) +
                           "   " + brls::getStr("app/pc/stats_hint_update");
        statsHint->setText(text);
    }
}

void PcCategoriesView::loadCatalogWithProgress() {
    bool isLight = ThemeManager::instance().isCurrentThemeLight();

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(480.0f);
    content->setPadding(20.0f);
    content->setAlignItems(brls::AlignItems::STRETCH);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("app/pc/updating_catalog"_i18n);
    titleLabel->setFontSize(20.0f);
    titleLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLabel->setMarginBottom(10.0f);
    content->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setText("app/common/loading"_i18n);
    statusLabel->setFontSize(14.0f);
    statusLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    statusLabel->setMarginBottom(14.0f);
    content->addView(statusLabel);

    auto* progressBg = new brls::Box();
    progressBg->setAxis(brls::Axis::ROW);
    progressBg->setWidth(440.0f);
    progressBg->setHeight(10.0f);
    progressBg->setCornerRadius(5.0f);
    progressBg->setBackgroundColor(isLight ? nvgRGBA(0, 0, 0, 25) : nvgRGBA(42, 45, 52, 255));
    progressBg->setMarginBottom(8.0f);

    auto* progressFill = new brls::Box();
    progressFill->setWidth(0.0f);
    progressFill->setHeight(10.0f);
    progressFill->setCornerRadius(5.0f);
    progressFill->setBackgroundColor(ThemeManager::instance().getAccentColor());
    progressBg->addView(progressFill);
    content->addView(progressBg);

    auto* counterLabel = new brls::Label();
    counterLabel->setText("0%");
    counterLabel->setFontSize(13.0f);
    counterLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    counterLabel->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    content->addView(counterLabel);

    auto* progressDialog = new brls::Dialog(content);
    progressDialog->setCancelable(false);

    auto* applet = progressDialog->getAppletFrame();
    if (applet) {
        applet->setWidth(520.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(24, 26, 32, 252));
    }

    progressDialog->open();

    std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
    brls::async([this, flag, progressDialog, progressFill, counterLabel, statusLabel]() {
        bool ok = catalog::PcCatalogManager::instance().loadCatalog(true,
            [progressFill, counterLabel, statusLabel](size_t dl, size_t total, const std::string& step) {
                if (total > 0) {
                    float p = static_cast<float>(dl) / static_cast<float>(total);
                    int pct = static_cast<int>(p * 100.0f);
                    float w = 440.0f * std::clamp(p, 0.0f, 1.0f);
                    brls::sync([progressFill, counterLabel, statusLabel, w, pct, step]() {
                        progressFill->setWidth(w);
                        counterLabel->setText(std::to_string(pct) + "%");
                        if (!step.empty()) {
                            statusLabel->setText(step);
                        }
                    });
                } else if (!step.empty()) {
                    brls::sync([statusLabel, step]() {
                        statusLabel->setText(step);
                    });
                }
            });

        brls::sync([this, flag, progressDialog, ok]() {
            progressDialog->close([this, flag, ok]() {
                if (!flag || !flag->load()) return;
                if (ok) {
                    rebuildGrid();
                    updateHeader();
                    brls::Application::notify("app/pc/catalog_updated"_i18n);
                } else {
                    brls::Application::notify("app/pc/catalog_update_error"_i18n);
                }
            });
        });
    });
}

void PcCategoriesView::showUpdateDialog() {
    auto* dialog = new brls::Dialog("app/pc/update_catalog_btn"_i18n);
    dialog->setCancelable(true);
    dialog->addButton("app/common/confirm"_i18n, [this]() {
        loadCatalogWithProgress();
    });
    dialog->addButton("app/common/cancel"_i18n, []() {});
    dialog->open();
}

void PcCategoriesView::rebuildGrid() {
    if (!listBox) return;
    listBox->clearViews();
    category_cards_.clear();
    grid_.clear();

    const auto& subforums = catalog::PcCatalogManager::instance().getSubforums();
    size_t totalGames = catalog::PcCatalogManager::instance().getGamesCount();
    if (totalGames == 0) {
        totalGames = catalog::PcCatalogManager::instance().getTotalKnownGamesCount();
    }
    size_t newGames = catalog::PcCatalogManager::instance().getGamesCountForSubforum("635");
    if (newGames == 0) {
        const auto* sf635 = catalog::PcCatalogManager::instance().findSubforum("635");
        if (sf635) newGames = sf635->games_count;
    }

    auto addRowCards = [this](const std::vector<CollectionCard*>& rowCards) {
        if (rowCards.empty()) return;
        brls::Box* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::FLEX_START);
        row->setWidthPercentage(100.0f);
        row->setMarginBottom(12.0f);

        for (size_t i = 0; i < rowCards.size(); ++i) {
            if (i + 1 < rowCards.size()) {
                rowCards[i]->setMarginRight(16.0f);
            }
            row->addView(rowCards[i]);
        }
        listBox->addView(row);
        grid_.push_back(rowCards);
    };

    auto makeCategoryCard = [](const catalog::PcSubforum& sf) -> CollectionCard* {
        std::string countStr = brls::getStr("app/pc/games_count", std::to_string(sf.games_count));
        std::string desc = !sf.parent.empty() ? sf.parent : "";
        std::string id = sf.id;
        std::string name = sf.name;

        return new CollectionCard(
            sf.slug,
            name,
            desc,
            countStr,
            getSubforumColor(sf),
            [id, name]() {
                brls::Application::pushActivity(new PcCatalogView(id, name));
            }
        );
    };

    // ============================================================
    // SECTION 1: ГЛАВНОЕ (Полный каталог + Новинки)
    // ============================================================
    listBox->addView(createSectionDivider("app/pc/sec_main"_i18n));

    std::vector<CollectionCard*> mainRow;

    // 1. Полный каталог
    {
        std::string totalStr = brls::getStr("app/pc/games_count", std::to_string(totalGames));
        CollectionCard* cardAll = new CollectionCard(
            "all_catalog",
            "app/pc/all_catalog"_i18n,
            "app/pc/all_catalog_desc"_i18n,
            totalStr,
            ThemeManager::instance().getAccentColor(),
            []() {
                brls::Application::pushActivity(new PcCatalogView("", "app/pc/all_catalog"_i18n));
            }
        );
        mainRow.push_back(cardAll);
    }

    // 2. Новинки
    {
        std::string newStr = brls::getStr("app/pc/games_count", std::to_string(newGames));
        CollectionCard* cardNew = new CollectionCard(
            "new_release",
            "app/pc/new_games"_i18n,
            "app/pc/new_games_desc"_i18n,
            newStr,
            nvgRGB(0, 200, 83), // Vibrant Green
            []() {
                brls::Application::pushActivity(new PcCatalogView("635", "app/pc/new_games"_i18n));
            }
        );
        mainRow.push_back(cardNew);
    }
    addRowCards(mainRow);

    // Filter helper to group subforums
    std::vector<catalog::PcSubforum> genres;
    std::vector<catalog::PcSubforum> strategies;
    std::vector<catalog::PcSubforum> actions;
    std::vector<catalog::PcSubforum> quests;
    std::vector<catalog::PcSubforum> retro;

    for (const auto& sf : subforums) {
        if (sf.games_count == 0) continue; // Skip empty categories
        if (sf.id == "635") continue;      // Already in "Main"

        if (sf.is_retro || sf.parent == "Старые игры") {
            retro.push_back(sf);
        } else if (sf.parent == "Стратегии") {
            strategies.push_back(sf);
        } else if (sf.parent == "Экшены") {
            actions.push_back(sf);
        } else if (sf.parent == "Приключения и квесты") {
            quests.push_back(sf);
        } else if (sf.parent.empty()) {
            genres.push_back(sf);
        }
    }

    auto renderGroup = [&](const std::string& sectionTitle, const std::vector<catalog::PcSubforum>& list) {
        if (list.empty()) return;
        listBox->addView(createSectionDivider(sectionTitle));
        std::vector<CollectionCard*> currentRow;
        for (const auto& sf : list) {
            currentRow.push_back(makeCategoryCard(sf));
            if (currentRow.size() == 4) {
                addRowCards(currentRow);
                currentRow.clear();
            }
        }
        if (!currentRow.empty()) {
            addRowCards(currentRow);
        }
    };

    // ============================================================
    // SECTION 2: ЖАНРЫ
    // ============================================================
    renderGroup("app/pc/sec_genres"_i18n, genres);

    // ============================================================
    // SECTION 3: СТРАТЕГИИ
    // ============================================================
    renderGroup("app/pc/sec_strategies"_i18n, strategies);

    // ============================================================
    // SECTION 4: ЭКШЕНЫ
    // ============================================================
    renderGroup("app/pc/sec_actions"_i18n, actions);

    // ============================================================
    // SECTION 5: ПРИКЛЮЧЕНИЯ И КВЕСТЫ
    // ============================================================
    renderGroup("app/pc/sec_quests"_i18n, quests);

    // ============================================================
    // SECTION 6: СТАРЫЕ ИГРЫ (РЕТРО ПК)
    // ============================================================
    renderGroup("app/pc/sec_retro"_i18n, retro);
}

} // namespace ui
