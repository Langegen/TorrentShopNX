#include "FavoritesView.hpp"
#include "CatalogView.hpp"
#include "FavoritesManager.hpp"
#include "FilterSortDialog.hpp"
#include "GameDetailView.hpp"
#include "../utils/log.h"
#include "../catalog/game_stats_manager.h"

#ifdef __SWITCH__
#include <switch.h>
static std::string showFavoritesKeyboard(const char* hint) {
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
static std::string showFavoritesKeyboard(const char* hint) {
    return "";
}
#endif

extern std::vector<Game> g_games;

namespace ui {

FavoritesView::FavoritesView() {
}

void FavoritesView::onContentAvailable() {
    recycler->registerCell("Row", []() { return GameRowCell::create(); });
    recycler->setDataSource(new FavoritesDataSource(this));

    // Register search/filter action keys
    this->registerAction("app/actions/search"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        std::string query = showFavoritesKeyboard("app/catalog/search_hint"_i18n.c_str());
        filterState_.searchQuery = query;
        filterFavorites();
        return true;
    });

    this->registerAction("", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        FilterSortDialog::show(filterState_, allFavorites_, [this](const catalog::FilterSortState& newState) {
            filterState_ = newState;
            filterFavorites();
        }, [this]() {
            resetFilters();
        });
        return true;
    }, true);

    this->registerAction("", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        resetFilters();
        return true;
    }, true);

    filterFavorites();
}

void FavoritesView::willAppear(bool resetState) {
    util::logLine("FavoritesView: willAppear resetState=" + std::to_string(resetState));
    brls::Activity::willAppear(resetState);
    filterFavorites();
    if (resetState) {
        util::logLine("FavoritesView: resetState is true, giving focus to recycler");
        brls::Application::giveFocus(recycler);
    }
}

void FavoritesView::willDisappear(bool resetState) {
    util::logLine("FavoritesView: willDisappear resetState=" + std::to_string(resetState));
    brls::Activity::willDisappear(resetState);
    util::logLine("FavoritesView: clearing focus");
    brls::Application::giveFocus(nullptr);
}

void FavoritesView::resetFilters() {
    filterState_.reset();
    filterFavorites();
    brls::Application::notify("app/catalog/filters_reset"_i18n);
}

void FavoritesView::filterFavorites() {
    auto& fm = catalog::FavoritesManager::instance();
    auto snap = getCatalogSnapshot();
    if (snap && !snap->empty()) {
        fm.syncLegacyFavorites(*snap);
    }
    allFavorites_ = fm.getFavorites();

    std::vector<Game> filtered;
    filtered.reserve(allFavorites_.size());

    for (const auto& g : allFavorites_) {
        if (catalog::matchesGameFilter(g, filterState_, true)) {
            filtered.push_back(g);
        }
    }

    if (filterState_.sort != catalog::SortOption::DEFAULT) {
        std::stable_sort(filtered.begin(), filtered.end(), [this](const Game& a, const Game& b) {
            return catalog::compareGames(a, b, filterState_.sort);
        });
    }

    filteredFavorites_ = std::move(filtered);

    if (recycler) {
        GameRowCell::s_lastFocusedColumn = 0;
        recycler->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler->resetScrollToTop();
        recycler->reloadData();
        brls::Application::giveFocus(this->recycler);
    }
}

int FavoritesView::FavoritesDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return (parent_->filteredFavorites_.size() + 5) / 6;
}

brls::RecyclerCell* FavoritesView::FavoritesDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    GameRowCell* rowCell = dynamic_cast<GameRowCell*>(recycler->dequeueReusableCell("Row"));
    if (!rowCell) return nullptr;

    int row = index.row;
    
    if (rowCell->imageToken) *(rowCell->imageToken) = false;
    rowCell->imageToken = std::make_shared<bool>(true);

    struct CardRefs {
        brls::Box* card;
        brls::Image* cover;
        brls::Label* lang;
        brls::Label* stats;
        brls::Label* title;
        brls::Label* size;
        brls::Box* statsBox;
        brls::Label* seeds;
        brls::Label* leeches;
    } cards[6] = {
        { rowCell->card0, rowCell->cover0, rowCell->lang0, rowCell->stats0, rowCell->title0, rowCell->size0, rowCell->statsBox0, rowCell->seeds0, rowCell->leeches0 },
        { rowCell->card1, rowCell->cover1, rowCell->lang1, rowCell->stats1, rowCell->title1, rowCell->size1, rowCell->statsBox1, rowCell->seeds1, rowCell->leeches1 },
        { rowCell->card2, rowCell->cover2, rowCell->lang2, rowCell->stats2, rowCell->title2, rowCell->size2, rowCell->statsBox2, rowCell->seeds2, rowCell->leeches2 },
        { rowCell->card3, rowCell->cover3, rowCell->lang3, rowCell->stats3, rowCell->title3, rowCell->size3, rowCell->statsBox3, rowCell->seeds3, rowCell->leeches3 },
        { rowCell->card4, rowCell->cover4, rowCell->lang4, rowCell->stats4, rowCell->title4, rowCell->size4, rowCell->statsBox4, rowCell->seeds4, rowCell->leeches4 },
        { rowCell->card5, rowCell->cover5, rowCell->lang5, rowCell->stats5, rowCell->title5, rowCell->size5, rowCell->statsBox5, rowCell->seeds5, rowCell->leeches5 }
    };

    for (int i = 0; i < 6; ++i) {
        size_t gameIdx = static_cast<size_t>(row * 6 + i);
        if (gameIdx < parent_->filteredFavorites_.size()) {
            const auto& game = parent_->filteredFavorites_[gameIdx];
            
            cards[i].card->setVisibility(brls::Visibility::VISIBLE);
            cards[i].card->setFocusable(true);
            
            std::string fullTitle  = cleanTitle(game.title);
            std::string shortTitle = truncateCatalogTitle(fullTitle);
            brls::Label* titleLabel = cards[i].title;

            titleLabel->setText(shortTitle);
            cards[i].size->setText(game.size);

            // Clear subscriptions from recycled cell
            cards[i].card->getFocusEvent()->clear();
            cards[i].card->getFocusLostEvent()->clear();

            cards[i].card->getFocusEvent()->subscribe([titleLabel, fullTitle, row, i](brls::View* v) {
                if (v->isFocused()) {
                    GameRowCell::s_lastFocusedColumn = i;
                    net::ImageDownloader::instance().setFocusedPosition(row, i);
                }
                titleLabel->setText(fullTitle);
                titleLabel->setAnimated(true);
            });
            cards[i].card->getFocusLostEvent()->subscribe([titleLabel, shortTitle](brls::View*) {
                titleLabel->setAnimated(false);
                titleLabel->setText(shortTitle);
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

            cards[i].size->setText(game.size);
            const auto* statsData = catalog::GameStatsManager::instance().getAnyStats(game.topic_id);
            if (cards[i].statsBox) {
                if (statsData && (statsData->seeds > 0 || statsData->leeches > 0)) {
                    cards[i].statsBox->setVisibility(brls::Visibility::VISIBLE);
                    if (cards[i].seeds) cards[i].seeds->setText(std::to_string(statsData->seeds));
                    if (cards[i].leeches) cards[i].leeches->setText(std::to_string(statsData->leeches));
                } else {
                    cards[i].statsBox->setVisibility(brls::Visibility::GONE);
                }
            }
            
            setImageFromHTTPS(cards[i].cover, game.cover, rowCell->imageToken, "romfs:/img/borealis_96.png", false, "", row, i);
            
            cards[i].card->registerAction("app/actions/toggle_favorite"_i18n, brls::ControllerButton::BUTTON_Y, [this, game](brls::View* view) {
                bool fav = catalog::FavoritesManager::instance().toggleFavorite(game);
                brls::Application::notify(fav ? "app/favorites/added"_i18n : "app/favorites/removed"_i18n);
                if (parent_) parent_->filterFavorites();
                return true;
            });

            cards[i].card->registerClickAction([game](brls::View* view) {
                brls::Application::pushActivity(new GameDetailView(game));
                return true;
            });
        } else {
            cards[i].card->setVisibility(brls::Visibility::INVISIBLE);
            cards[i].card->setFocusable(false);
            cards[i].card->getFocusEvent()->clear();
            cards[i].card->getFocusLostEvent()->clear();
            cards[i].card->registerClickAction([](brls::View*) { return false; });
            cards[i].card->registerAction("", brls::ControllerButton::BUTTON_Y, [](brls::View*) { return false; });
        }
    }
    
    return rowCell;
}

} // namespace ui
