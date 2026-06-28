#include "CatalogView.hpp"
#include "GameDetailView.hpp"
#include "FavoritesManager.hpp"
#include <sstream>
#include <set>

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

extern std::vector<Game> g_games;

namespace ui {

// GAMEROWCELL IMPLEMENTATION
GameRowCell::GameRowCell() {
    this->inflateFromXMLRes("xml/catalog_cell.xml");
}

GameRowCell::~GameRowCell() {
    if (imageToken) *imageToken = false;
}

GameRowCell* GameRowCell::create() {
    brls::Logger::info("GameRowCell: create() called");
    GameRowCell* cell = new GameRowCell();
    brls::Logger::info("GameRowCell: create() done");
    return cell;
}

// CATALOGVIEW IMPLEMENTATION
CatalogView::CatalogView() {
    // Empty constructor
}

void CatalogView::onContentAvailable() {
    brls::Logger::info("CatalogView: onContentAvailable start");

    // Genre tabs have been removed to save screen space

    // Register search/filter action keys
    this->registerAction("Поиск", brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        std::string query = showKeyboard("Поиск по названию игры");
        searchQuery_ = query;
        filterCatalog();
        return true;
    });

    this->registerAction("Сбросить фильтры", brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
        searchQuery_.clear();
        selectedGenre_.clear();
        filterCatalog();
        return true;
    });

    // Configure recycler
    brls::Logger::info("CatalogView: registering recycler cell");
    recycler->registerCell("Row", []() { return GameRowCell::create(); });
    brls::Logger::info("CatalogView: cell registered");
    // Pre-populate filteredGames_ so data is ready when RecyclerFrame does onLayout()
    brls::Logger::info("CatalogView: about to assign filteredGames_, g_games size=%d", (int)g_games.size());
    filteredGames_.assign(g_games.begin(), g_games.end());
    brls::Logger::info("CatalogView: filteredGames_ populated, count=%d", (int)filteredGames_.size());
    brls::Logger::info("CatalogView: recycler layouted? creating DataSource...");
    auto* ds = new CatalogDataSource(this);
    brls::Logger::info("CatalogView: DataSource created, calling setDataSource");
    recycler->setDataSource(ds);
    brls::Logger::info("CatalogView: data source set, constructor done");
    // RecyclerFrame will call reloadData() on its first onLayout()
}




static std::string toLowerLocal(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower;
}

void CatalogView::filterCatalog() {
    filteredGames_.clear();
    std::string lowerQuery = toLowerLocal(searchQuery_);
    
    for (const auto& game : g_games) {
        // Filter by genre tag
        if (!selectedGenre_.empty()) {
            if (toLowerLocal(game.genre).find(toLowerLocal(selectedGenre_)) == std::string::npos) {
                continue;
            }
        }
        
        // Filter by title search query
        if (!lowerQuery.empty()) {
            if (toLowerLocal(game.title).find(lowerQuery) == std::string::npos) {
                continue;
            }
        }
        
        filteredGames_.push_back(game);
    }
    
    // Reload recycler view
    recycler->reloadData();
}

// DATASOURCE IMPLEMENTATION
int CatalogView::CatalogDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    // 6 columns
    return (parent_->filteredGames_.size() + 5) / 6;
}

brls::RecyclerCell* CatalogView::CatalogDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    brls::RecyclerCell* genericCell = recycler->dequeueReusableCell("Row");
    
    GameRowCell* rowCell = dynamic_cast<GameRowCell*>(genericCell);

    if (!rowCell) {
        return genericCell;
    }

    if (rowCell->imageToken) *(rowCell->imageToken) = false;
    rowCell->imageToken = std::make_shared<bool>(true);

    int row = index.row;
    
    struct CardRefs {
        brls::Box* card;
        brls::Image* cover;
        brls::Label* lang;
        brls::Label* title;
        brls::Label* size;
    } cards[6];
    
    try {
        cards[0] = { rowCell->card0, rowCell->cover0, rowCell->lang0, rowCell->title0, rowCell->size0 };
        cards[1] = { rowCell->card1, rowCell->cover1, rowCell->lang1, rowCell->title1, rowCell->size1 };
        cards[2] = { rowCell->card2, rowCell->cover2, rowCell->lang2, rowCell->title2, rowCell->size2 };
        cards[3] = { rowCell->card3, rowCell->cover3, rowCell->lang3, rowCell->title3, rowCell->size3 };
        cards[4] = { rowCell->card4, rowCell->cover4, rowCell->lang4, rowCell->title4, rowCell->size4 };
        cards[5] = { rowCell->card5, rowCell->cover5, rowCell->lang5, rowCell->title5, rowCell->size5 };
    } catch (const std::exception& e) {
        brls::Logger::error("CatalogDataSource: EXCEPTION resolving BRLS_BIND variables: {}", e.what());
        return rowCell;
    } catch (...) {
        brls::Logger::error("CatalogDataSource: UNKNOWN EXCEPTION resolving BRLS_BIND variables");
        return rowCell;
    }

    for (int i = 0; i < 6; ++i) {
        size_t gameIdx = static_cast<size_t>(row * 6 + i);
        if (gameIdx < parent_->filteredGames_.size()) {
            try {
                brls::Box* cardBox = cards[i].card;
                if (!cardBox) continue;
                
                const auto& game = parent_->filteredGames_[gameIdx];
                
                cardBox->setVisibility(brls::Visibility::VISIBLE);
                cardBox->setFocusable(true);
                
                // Short title (guaranteed ≤150px): safe static display
                std::string fullTitle  = cleanTitle(game.title);
                std::string shortTitle = truncateCatalogTitle(fullTitle);
                brls::Label* titleLabel = cards[i].title;

                titleLabel->setText(shortTitle);
                cards[i].size->setText(game.size);

                // Clear subscriptions from recycled cell before adding new ones
                cardBox->getFocusEvent()->clear();
                cardBox->getFocusLostEvent()->clear();

                // On focus → switch to full title and start scrolling animation (scissor-clipped by Borealis)
                cardBox->getFocusEvent()->subscribe([titleLabel, fullTitle](brls::View*) {
                    titleLabel->setText(fullTitle);
                    titleLabel->setAnimated(true);
                });

                // On focus lost → stop animation and restore short title (no overflow)
                cardBox->getFocusLostEvent()->subscribe([titleLabel, shortTitle](brls::View*) {
                    titleLabel->setAnimated(false);
                    titleLabel->setText(shortTitle);
                });
                
                std::string lang = extractLangBadge(game.interface_lang);
                if (!lang.empty()) {
                    cards[i].lang->setVisibility(brls::Visibility::VISIBLE);
                    cards[i].lang->setText(" " + lang + " ");
                } else {
                    cards[i].lang->setVisibility(brls::Visibility::GONE);
                }
                
                setImageFromHTTPS(cards[i].cover, game.cover, rowCell->imageToken);
                
                cardBox->registerAction("В избранное / Убрать", brls::ControllerButton::BUTTON_Y, [game](brls::View* view) {
                    catalog::FavoritesManager::instance().toggleFavorite(game.topic_id);
                    brls::Application::notify(catalog::FavoritesManager::instance().isFavorite(game.topic_id) ? "Добавлено в избранное" : "Удалено из избранного");
                    return true;
                });

                cardBox->registerClickAction([game](brls::View* view) {
                    brls::Application::pushActivity(new GameDetailView(game));
                    return true;
                });
            } catch (const std::exception& e) {
                brls::Logger::error("CatalogDataSource: EXCEPTION in card setup {}: {}", i, e.what());
            } catch (...) {
                brls::Logger::error("CatalogDataSource: UNKNOWN EXCEPTION in card setup {}", i);
            }
        } else {
            try {
                if (cards[i].card) {
                    cards[i].card->setVisibility(brls::Visibility::INVISIBLE);
                    cards[i].card->setFocusable(false);
                }
            } catch (...) {}
        }
    }
    
    return rowCell;
}

} // namespace ui
