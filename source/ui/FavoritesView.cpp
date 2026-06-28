#include "FavoritesView.hpp"
#include "CatalogView.hpp"
#include "FavoritesManager.hpp"
#include "GameDetailView.hpp"

extern std::vector<Game> g_games;

namespace ui {

FavoritesView::FavoritesView() {
}

void FavoritesView::onContentAvailable() {
    recycler->registerCell("Row", []() { return GameRowCell::create(); });
    recycler->setDataSource(new FavoritesDataSource(this));

    filterFavorites();
}



void FavoritesView::filterFavorites() {
    favoritedGames_.clear();
    auto& fm = catalog::FavoritesManager::instance();
    for (const auto& game : g_games) {
        if (fm.isFavorite(game.topic_id)) {
            favoritedGames_.push_back(game);
        }
    }
    recycler->reloadData();
}

int FavoritesView::FavoritesDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return (parent_->favoritedGames_.size() + 5) / 6;
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
        brls::Label* title;
        brls::Label* size;
    } cards[6] = {
        { rowCell->card0, rowCell->cover0, rowCell->lang0, rowCell->title0, rowCell->size0 },
        { rowCell->card1, rowCell->cover1, rowCell->lang1, rowCell->title1, rowCell->size1 },
        { rowCell->card2, rowCell->cover2, rowCell->lang2, rowCell->title2, rowCell->size2 },
        { rowCell->card3, rowCell->cover3, rowCell->lang3, rowCell->title3, rowCell->size3 },
        { rowCell->card4, rowCell->cover4, rowCell->lang4, rowCell->title4, rowCell->size4 },
        { rowCell->card5, rowCell->cover5, rowCell->lang5, rowCell->title5, rowCell->size5 }
    };

    for (int i = 0; i < 6; ++i) {
        size_t gameIdx = static_cast<size_t>(row * 6 + i);
        if (gameIdx < parent_->favoritedGames_.size()) {
            const auto& game = parent_->favoritedGames_[gameIdx];
            
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

            cards[i].card->getFocusEvent()->subscribe([titleLabel, fullTitle](brls::View*) {
                titleLabel->setText(fullTitle);
                titleLabel->setAnimated(true);
            });
            cards[i].card->getFocusLostEvent()->subscribe([titleLabel, shortTitle](brls::View*) {
                titleLabel->setAnimated(false);
                titleLabel->setText(shortTitle);
            });
            
            std::string lang = extractLangBadge(game.interface_lang);
            if (!lang.empty()) {
                cards[i].lang->setVisibility(brls::Visibility::VISIBLE);
                cards[i].lang->setText(lang);
            } else {
                cards[i].lang->setVisibility(brls::Visibility::GONE);
            }
            
            setImageFromHTTPS(cards[i].cover, game.cover, rowCell->imageToken);
            
            // Toggle favorite on Y button press inside card
            cards[i].card->registerAction("Убрать из избранного", brls::ControllerButton::BUTTON_Y, [this, game](brls::View* view) {
                catalog::FavoritesManager::instance().toggleFavorite(game.topic_id);
                brls::Application::notify("Удалено из избранного");
                // Refresh list
                parent_->filterFavorites();
                return true;
            });

            // Card click action opens details
            cards[i].card->registerClickAction([game](brls::View* view) {
                brls::Application::pushActivity(new GameDetailView(game));
                return true;
            });
            
        } else {
            cards[i].card->setVisibility(brls::Visibility::GONE);
            cards[i].card->setFocusable(false);
        }
    }

    return rowCell;
}

} // namespace ui
