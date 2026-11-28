#include "PcCatalogView.hpp"
#include "CatalogView.hpp"
#include "EmptyState.hpp"
#include "style/HeaderHints.hpp"
#include "GameDetailView.hpp"
#include "FavoritesManager.hpp"
#include "FilterSortDialog.hpp"
#include "ThemeManager.hpp"
#include "../catalog/pc_catalog_manager.h"
#include "../catalog/game_stats_manager.h"
#include "../catalog/filter_manager.hpp"
#include "../net/image_downloader.h"
#include "../utils/log.h"
#include "../config/config.h"
#include <algorithm>

#ifdef __SWITCH__
#include <switch.h>
static std::string showKeyboard(const char* hint) {
    SwkbdConfig kbd;
    swkbdCreate(&kbd, 0);
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, hint);
    char out[256] = {0};
    Result rc = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return "";
    return std::string(out);
}
#else
static std::string showKeyboard(const char* hint) {
    return "";
}
#endif

namespace ui {

class PcCatalogDataSource : public brls::RecyclerDataSource {
public:
    explicit PcCatalogDataSource(PcCatalogView* parent) : parent_(parent) {}

    int numberOfRows(brls::RecyclerFrame* recycler, int section) override {
        if (!parent_ || parent_->filteredGames_.empty()) return 0;
        return (static_cast<int>(parent_->filteredGames_.size()) + 5) / 6;
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath indexPath) override {
        GameRowCell* rowCell = dynamic_cast<GameRowCell*>(recycler->dequeueReusableCell("Row"));
        if (!rowCell) return nullptr;

        if (rowCell->imageToken) *(rowCell->imageToken) = false;
        rowCell->imageToken = std::make_shared<bool>(true);

        int row = indexPath.row;
        struct CardViews {
            brls::Box* card;
            brls::Image* cover;
            brls::Label* lang;
            brls::Label* stats;
            brls::Label* title;
            brls::Label* size;
            brls::Box* statsBox;
            brls::Label* seeds;
            brls::Label* leeches;
        } cards[6];

        try {
            cards[0] = { rowCell->card0, rowCell->cover0, rowCell->lang0, rowCell->stats0, rowCell->title0, rowCell->size0, rowCell->statsBox0, rowCell->seeds0, rowCell->leeches0 };
            cards[1] = { rowCell->card1, rowCell->cover1, rowCell->lang1, rowCell->stats1, rowCell->title1, rowCell->size1, rowCell->statsBox1, rowCell->seeds1, rowCell->leeches1 };
            cards[2] = { rowCell->card2, rowCell->cover2, rowCell->lang2, rowCell->stats2, rowCell->title2, rowCell->size2, rowCell->statsBox2, rowCell->seeds2, rowCell->leeches2 };
            cards[3] = { rowCell->card3, rowCell->cover3, rowCell->lang3, rowCell->stats3, rowCell->title3, rowCell->size3, rowCell->statsBox3, rowCell->seeds3, rowCell->leeches3 };
            cards[4] = { rowCell->card4, rowCell->cover4, rowCell->lang4, rowCell->stats4, rowCell->title4, rowCell->size4, rowCell->statsBox4, rowCell->seeds4, rowCell->leeches4 };
            cards[5] = { rowCell->card5, rowCell->cover5, rowCell->lang5, rowCell->stats5, rowCell->title5, rowCell->size5, rowCell->statsBox5, rowCell->seeds5, rowCell->leeches5 };
        } catch (...) {
            return rowCell;
        }

        for (int i = 0; i < 6; ++i) {
            size_t gameIdx = static_cast<size_t>(row * 6 + i);
            if (gameIdx < parent_->filteredGames_.size()) {
                brls::Box* cardBox = cards[i].card;
                if (!cardBox) continue;

                const auto& game = *parent_->filteredGames_[gameIdx];

                cardBox->setVisibility(brls::Visibility::VISIBLE);
                cardBox->setFocusable(true);

                brls::Label* titleLabel = cards[i].title;
                titleLabel->setText(cleanTitle(game.title));
                cards[i].size->setText(game.size);

                cardBox->getFocusEvent()->clear();
                cardBox->getFocusLostEvent()->clear();

                cardBox->getFocusEvent()->subscribe([this, titleLabel, row, i](brls::View* v) {
                    if (v->isFocused()) {
                        parent_->focusedRow_ = row;
                        parent_->focusedCol_ = i;
                        GameRowCell::s_lastFocusedColumn = i;
                        net::ImageDownloader::instance().setFocusedPosition(row, i);
                    }
                    titleLabel->setAnimated(true);
                });
                cardBox->getFocusLostEvent()->subscribe([titleLabel](brls::View*) {
                    titleLabel->setAnimated(false);
                });

                GameLangBadge badge = getGameLangBadge(game.interface_lang, brls::Application::getLocale());
                if (!badge.text.empty()) {
                    cards[i].lang->setVisibility(brls::Visibility::VISIBLE);
                    cards[i].lang->setText("  " + badge.text + "  ");
                    cards[i].lang->setBackgroundColor(badge.color);
                } else {
                    cards[i].lang->setVisibility(brls::Visibility::GONE);
                }

                cards[i].stats->setVisibility(brls::Visibility::GONE);

                const auto statsData = catalog::GameStatsManager::instance().getAnyStats(game.topic_id);
                if (statsData) {
                    int seeds = statsData->seeds;
                    int leeches = statsData->leeches;
                    if (seeds >= 0 || leeches >= 0) {
                        cards[i].statsBox->setVisibility(brls::Visibility::VISIBLE);
                        cards[i].seeds->setText(std::to_string(std::max(0, seeds)));
                        cards[i].leeches->setText(std::to_string(std::max(0, leeches)));
                    } else {
                        cards[i].statsBox->setVisibility(brls::Visibility::GONE);
                    }
                } else {
                    cards[i].statsBox->setVisibility(brls::Visibility::GONE);
                }

                setImageFromHTTPS(cards[i].cover, game.cover, rowCell->imageToken, "romfs:/img/cover_blank.png", false, "", row, i);

                cards[i].card->registerAction("app/actions/toggle_favorite"_i18n, brls::ControllerButton::BUTTON_Y, [game](brls::View* view) {
                    bool fav = catalog::FavoritesManager::instance().toggleFavorite(game);
                    brls::Application::notify(fav ? "app/favorites/added"_i18n : "app/favorites/removed"_i18n);
                    return true;
                });

                cards[i].card->registerClickAction([game](brls::View* view) {
                    brls::Application::pushActivity(new GameDetailView(game));
                    return true;
                });
            } else {
                cards[i].card->setVisibility(brls::Visibility::INVISIBLE);
                cards[i].card->setFocusable(false);
            }
        }

        return rowCell;
    }

private:
    PcCatalogView* parent_ = nullptr;
};

PcCatalogView::PcCatalogView(const std::string& subforum_id,
                             const std::string& category_title,
                             const catalog::FilterSortState& initialFilter,
                             const std::string& filterBadgeLabel)
    : subforum_id_(subforum_id),
      category_title_(category_title),
      filterState_(initialFilter),
      filterBadgeLabel_(filterBadgeLabel) {
}

PcCatalogView::~PcCatalogView() = default;

void PcCatalogView::updateHeaderTitle() {
    if (!headerTitle) return;
    std::string base = !category_title_.empty() ? category_title_ : brls::getStr("app/pc/all_catalog");
    headerTitle->setSingleLine(true);
    headerTitle->setText(filterBadgeLabel_.empty() ? base : (base + " \xC2\xB7 " + filterBadgeLabel_));
}

void PcCatalogView::onContentAvailable() {
    if (headerTitle) {
        updateHeaderTitle();
        headerTitle->addGestureRecognizer(new brls::TapGestureRecognizer(headerTitle, [this]() {
            std::string query = showKeyboard("app/catalog/search_hint"_i18n.c_str());
            filterState_.searchQuery = query;
            filterBadgeLabel_.clear();
            updateHeaderTitle();
            filterCatalog();
        }));
    }

    // Populate all games for this category
    if (!catalog::PcCatalogManager::instance().isLoaded()) {
        catalog::PcCatalogManager::instance().loadCatalog(false);
    }
    if (subforum_id_.empty()) {
        const auto& all = catalog::PcCatalogManager::instance().getAllGames();
        allGames_.reserve(all.size());
        for (const auto& g : all) allGames_.push_back(&g);
    } else {
        allGames_ = catalog::PcCatalogManager::instance().getGamesForSubforum(subforum_id_);
    }

    // Special case for "Новинки" (subforum 635): sort by date by default
    if (subforum_id_ == "635" && filterState_.sort == catalog::SortOption::DEFAULT) {
        filterState_.sort = catalog::SortOption::DATE_DESC;
    }

    if (statsHint) {
        statsHint->addGestureRecognizer(new brls::TapGestureRecognizer(statsHint, [this]() {
            std::vector<Game> copy;
            copy.reserve(allGames_.size());
            for (const auto* g : allGames_) copy.push_back(*g);
            FilterSortDialog::show(filterState_, copy, [this](const catalog::FilterSortState& newState) {
                filterState_ = newState;
                filterBadgeLabel_.clear();
                updateHeaderTitle();
                filterCatalog();
            }, [this]() {
                resetFilters();
            });
        }));
    }

    // Register search/filter action keys
    this->registerAction("app/actions/search"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        std::string query = showKeyboard("app/catalog/search_hint"_i18n.c_str());
        filterState_.searchQuery = query;
        filterBadgeLabel_.clear();
        updateHeaderTitle();
        filterCatalog();
        return true;
    });

    this->registerAction("", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        std::vector<Game> copy;
        copy.reserve(allGames_.size());
        for (const auto* g : allGames_) copy.push_back(*g);
        FilterSortDialog::show(filterState_, copy, [this](const catalog::FilterSortState& newState) {
            filterState_ = newState;
            filterBadgeLabel_.clear();
            updateHeaderTitle();
            filterCatalog();
        }, [this]() {
            resetFilters();
        });
        return true;
    }, true);

    this->registerAction("", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        resetFilters();
        return true;
    }, true);

    this->registerAction("", brls::ControllerButton::BUTTON_RT, [this](brls::View* view) {
        jumpToNextLetter(true);
        return true;
    }, true);

    this->registerAction("", brls::ControllerButton::BUTTON_LT, [this](brls::View* view) {
        jumpToNextLetter(false);
        return true;
    }, true);

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_RIGHT_BRACKET, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        jumpToNextLetter(true);
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_LEFT_BRACKET, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        jumpToNextLetter(false);
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_PAGE_DOWN, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        jumpToNextLetter(true);
        return true;
    });

    this->registerAction(brls::BrlsKeyCombination{brls::BRLS_KBD_KEY_PAGE_UP, brls::BRLS_KBD_MODIFIER_NONE}, [this](brls::View* view) {
        jumpToNextLetter(false);
        return true;
    });

    recycler->registerCell("Row", []() { return GameRowCell::create(); });
    recycler->setDataSource(new PcCatalogDataSource(this));

    filterCatalog();
}

void PcCatalogView::willAppear(bool resetState) {
    Activity::willAppear(resetState);
}

void PcCatalogView::willDisappear(bool resetState) {
    Activity::willDisappear(resetState);
}

void PcCatalogView::onResume() {
    Activity::onResume();
}

void PcCatalogView::filterCatalog() {
    filteredGames_.clear();

    const bool hasSearch = !filterState_.searchQuery.empty();
    std::string lowerQuery;
    if (hasSearch) lowerQuery = util::toLowerUtf8(filterState_.searchQuery);

    for (const auto* gamePtr : allGames_) {
        if (!gamePtr) continue;
        const Game& game = *gamePtr;

        bool isFav = catalog::FavoritesManager::instance().isFavorite(game.topic_id);
        if (!catalog::matchesGameFilter(game, filterState_, isFav)) {
            continue;
        }

        if (hasSearch) {
            std::string lowerTitle = util::toLowerUtf8(cleanTitle(game.title));
            if (lowerTitle.find(lowerQuery) == std::string::npos) {
                continue;
            }
        }

        filteredGames_.push_back(gamePtr);
    }

    if (filterState_.sort != catalog::SortOption::DEFAULT) {
        catalog::sortGamesBy(filteredGames_, filterState_.sort, [](const Game* g) { return g; });
    }

    if (statsHint) {
        std::string countStr;
        if (filteredGames_.size() != allGames_.size()) {
            countStr = brls::getStr("app/catalog/shown_of", std::to_string(filteredGames_.size()), std::to_string(allGames_.size()));
        } else {
            countStr = brls::getStr("app/catalog/games_count", std::to_string(filteredGames_.size()));
        }
        statsHint->setText(ui::style::headerMeta(countStr, {
            ui::style::buttonHint(brls::ControllerButton::BUTTON_LT, brls::ControllerButton::BUTTON_RT, "app/header_hint/jump_az"_i18n),
            ui::style::buttonHint(brls::ControllerButton::BUTTON_LB, "app/header_hint/reset"_i18n),
            ui::style::buttonHint(brls::ControllerButton::BUTTON_RB, "app/header_hint/filter"_i18n)
        }));
    }

    if (emptyHolder && recycler) {
        const bool empty = filteredGames_.empty();
        if (empty && !emptyState_) {
            emptyState_ = new EmptyState();
            emptyState_->setContent(DashboardIcon::Kind::Search, "app/empty/search_title"_i18n,
                                    "app/empty/search_sub"_i18n, "app/empty/reset_filters"_i18n,
                                    [this]() { resetFilters(); });
            emptyHolder->addView(emptyState_);
        }
        emptyHolder->setVisibility(empty ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        recycler->setVisibility(empty ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        if (empty) {
            const auto& stack = brls::Application::getActivitiesStack();
            if (!stack.empty() && stack.back() == this && emptyState_->actionView()) {
                brls::Application::giveFocus(emptyState_->actionView());
            }
            return;
        }
    }

    GameRowCell::s_lastFocusedColumn = 0;
    if (recycler) {
        recycler->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler->resetScrollToTop();
        recycler->reloadData();
        const auto& stack = brls::Application::getActivitiesStack();
        if (!stack.empty() && stack.back() == this) {
            brls::Application::giveFocus(this->recycler);
        }
    }
}

void PcCatalogView::resetFilters() {
    filterState_.reset();
    if (subforum_id_ == "635") {
        filterState_.sort = catalog::SortOption::DATE_DESC;
    }
    filterBadgeLabel_.clear();
    updateHeaderTitle();
    filterCatalog();
}

void PcCatalogView::jumpToNextLetter(bool forward) {
    if (filteredGames_.empty() || !recycler) return;

    size_t currentIdx = static_cast<size_t>(std::max(0, focusedRow_ * 6 + focusedCol_));
    if (currentIdx >= filteredGames_.size()) currentIdx = 0;

    auto getFirstChar = [](const std::string& title) -> char32_t {
        std::string cleaned = cleanTitle(title);
        cleaned = normalizeTitleForSort(cleaned);
        if (cleaned.empty()) return 0;
        return static_cast<char32_t>(static_cast<unsigned char>(std::toupper(cleaned[0])));
    };

    char32_t currentChar = getFirstChar(filteredGames_[currentIdx]->title);

    if (forward) {
        for (size_t i = currentIdx + 1; i < filteredGames_.size(); ++i) {
            char32_t c = getFirstChar(filteredGames_[i]->title);
            if (c != currentChar && c != 0) {
                int targetRow = static_cast<int>(i / 6);
                int targetCol = static_cast<int>(i % 6);
                focusedRow_ = targetRow;
                focusedCol_ = targetCol;
                GameRowCell::s_lastFocusedColumn = targetCol;
                recycler->selectRowAt(brls::IndexPath(0, targetRow), true);
                return;
            }
        }
    } else {
        if (currentIdx == 0) return;
        for (int i = static_cast<int>(currentIdx) - 1; i >= 0; --i) {
            char32_t c = getFirstChar(filteredGames_[i]->title);
            if (c != currentChar && c != 0) {
                while (i > 0 && getFirstChar(filteredGames_[i - 1]->title) == c) {
                    --i;
                }
                int targetRow = i / 6;
                int targetCol = i % 6;
                focusedRow_ = targetRow;
                focusedCol_ = targetCol;
                GameRowCell::s_lastFocusedColumn = targetCol;
                recycler->selectRowAt(brls::IndexPath(0, targetRow), true);
                return;
            }
        }
    }
}

} // namespace ui
