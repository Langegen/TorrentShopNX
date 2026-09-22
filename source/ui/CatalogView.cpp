#include "CatalogView.hpp"
#include "GameDetailView.hpp"
#include "FavoritesManager.hpp"
#include "FilterSortDialog.hpp"
#include "../catalog/filter_manager.hpp"
#include "../utils/log.h"
#include <sstream>
#include <set>
#include <algorithm>
#include "../config/config.h"

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

CatalogView* g_activeCatalogView = nullptr;

int GameRowCell::s_lastFocusedColumn = 0;

// GAMEROWCELL IMPLEMENTATION
GameRowCell::GameRowCell() {
    this->inflateFromXMLRes("xml/catalog_cell.xml");
    
    brls::Box* cards[] = { card0, card1, card2, card3, card4, card5 };
    for (int i = 0; i < 6; ++i) {
        if (cards[i]) {
            cards[i]->getFocusEvent()->subscribe([i](brls::View* v) {
                if (v->isFocused()) {
                    s_lastFocusedColumn = i;
                }
            });
        }
    }
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

void GameRowCell::prepareForReuse() {
    brls::RecyclerCell::prepareForReuse();

    if (imageToken) {
        *imageToken = false;
        imageToken.reset();
    }

    brls::Box* cards[] = { card0, card1, card2, card3, card4, card5 };
    brls::Label* titles[] = { title0, title1, title2, title3, title4, title5 };

    for (int i = 0; i < 6; ++i) {
        if (cards[i]) {
            if (cards[i]->isFocused()) {
                cards[i]->onFocusLost();
                if (brls::Application::getCurrentFocus() == cards[i]) {
                    brls::Application::giveFocus(nullptr);
                }
            }
            cards[i]->setHighlighted(false);
            cards[i]->setHighlightProgress(0.0f);
        }
        if (titles[i]) {
            titles[i]->setAnimated(false);
        }
    }
}

brls::View* GameRowCell::getDefaultFocus() {
    brls::Box* cards[] = { card0, card1, card2, card3, card4, card5 };
    if (s_lastFocusedColumn >= 0 && s_lastFocusedColumn < 6) {
        if (cards[s_lastFocusedColumn] && cards[s_lastFocusedColumn]->getVisibility() == brls::Visibility::VISIBLE) {
            return cards[s_lastFocusedColumn];
        }
    }
    for (auto* card : cards) {
        if (card && card->getVisibility() == brls::Visibility::VISIBLE) return card;
    }
    return nullptr;
}

// CATALOGVIEW IMPLEMENTATION
CatalogView::CatalogView(const std::string& searchQuery) {
    filterState_.sort = catalog::SortOption::TITLE_ASC;
    if (!searchQuery.empty()) {
        filterState_.searchQuery = searchQuery;
    }
}

void CatalogView::onContentAvailable() {
    brls::Logger::info("CatalogView: onContentAvailable start");

    auto catalog = getCatalogSnapshot();

    // Pre-populate filteredGames_ with non-homebrew games, sorted alphabetically by default
    filteredGames_.clear();
    filteredGames_.reserve(catalog->size());
    for (const auto& g : *catalog) {
        if (!isHomebrewGame(g)) {
            filteredGames_.push_back(g);
        }
    }

    catalog::SortOption sortOpt = (filterState_.sort == catalog::SortOption::DEFAULT)
        ? catalog::SortOption::TITLE_ASC
        : filterState_.sort;

    std::stable_sort(filteredGames_.begin(), filteredGames_.end(), [sortOpt](const Game& a, const Game& b) {
        return catalog::compareGames(a, b, sortOpt);
    });

    std::string countStr = brls::getStr("app/catalog/games_count", std::to_string(filteredGames_.size()));
    statsHint->setText(countStr + "app/catalog/stats_hint_suffix"_i18n);

    if (headerTitle) {
        headerTitle->addGestureRecognizer(new brls::TapGestureRecognizer(headerTitle, [this]() {
            std::string query = showKeyboard("app/catalog/search_hint"_i18n.c_str());
            filterState_.searchQuery = query;
            filterCatalog();
        }));
    }

    if (statsHint) {
        statsHint->addGestureRecognizer(new brls::TapGestureRecognizer(statsHint, [this]() {
            auto catalog = getCatalogSnapshot();
            std::vector<Game> officialGames;
            officialGames.reserve(catalog->size());
            for (const auto& g : *catalog) {
                if (!isHomebrewGame(g)) officialGames.push_back(g);
            }
            FilterSortDialog::show(filterState_, officialGames, [this](const catalog::FilterSortState& newState) {
                filterState_ = newState;
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
        filterCatalog();
        return true;
    });

    this->registerAction("", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        auto catalog = getCatalogSnapshot();
        std::vector<Game> officialGames;
        officialGames.reserve(catalog->size());
        for (const auto& g : *catalog) {
            if (!isHomebrewGame(g)) officialGames.push_back(g);
        }
        FilterSortDialog::show(filterState_, officialGames, [this](const catalog::FilterSortState& newState) {
            filterState_ = newState;
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
    }, true /* hidden */);

    this->registerAction("", brls::ControllerButton::BUTTON_LT, [this](brls::View* view) {
        jumpToNextLetter(false);
        return true;
    }, true /* hidden */);

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

    // Configure recycler
    brls::Logger::info("CatalogView: registering recycler cell");
    recycler->registerCell("Row", []() { return GameRowCell::create(); });
    brls::Logger::info("CatalogView: cell registered");
    brls::Logger::info("CatalogView: filteredGames_ populated, count=%d", (int)filteredGames_.size());
    brls::Logger::info("CatalogView: recycler layouted? creating DataSource...");
    auto* ds = new CatalogDataSource(this);
    brls::Logger::info("CatalogView: DataSource created, calling setDataSource");
    GameRowCell::s_lastFocusedColumn = 0;
    recycler->setDefaultCellFocus(brls::IndexPath(0, 0));
    recycler->setDataSource(ds);
    brls::Logger::info("CatalogView: data source set, constructor done");
    // RecyclerFrame will call reloadData() on its first onLayout()
    bool hasFilters = !filterState_.searchQuery.empty() ||
                      !filterState_.genre.empty() ||
                      filterState_.lang != catalog::LanguageFilter::ALL ||
                      filterState_.onlyFavorites ||
                      !filterState_.year.empty() ||
                      filterState_.players != catalog::PlayersFilter::ALL;
    if (hasFilters) {
        filterCatalog();
    }
}

void CatalogView::filterCatalog() {
    filteredGames_.clear();
    auto& fm = catalog::FavoritesManager::instance();
    size_t totalNonHomebrew = 0;
    auto catalog = getCatalogSnapshot();

    for (const auto& game : *catalog) {
        if (isHomebrewGame(game)) continue;
        ++totalNonHomebrew;
        bool isFav = filterState_.onlyFavorites ? fm.isFavorite(game) : false;
        if (catalog::matchesGameFilter(game, filterState_, isFav)) {
            filteredGames_.push_back(game);
        }
    }

    catalog::SortOption sortOpt = (filterState_.sort == catalog::SortOption::DEFAULT)
        ? catalog::SortOption::TITLE_ASC
        : filterState_.sort;

    std::stable_sort(filteredGames_.begin(), filteredGames_.end(), [sortOpt](const Game& a, const Game& b) {
        return catalog::compareGames(a, b, sortOpt);
    });

    if (statsHint) {
        std::string countStr;
        if (filteredGames_.size() != totalNonHomebrew) {
            countStr = brls::getStr("app/catalog/shown_of", std::to_string(filteredGames_.size()), std::to_string(totalNonHomebrew));
        } else {
            countStr = brls::getStr("app/catalog/games_count", std::to_string(filteredGames_.size()));
        }
        statsHint->setText(countStr + "app/catalog/stats_hint_suffix"_i18n);
    }

    GameRowCell::s_lastFocusedColumn = 0;
    // Safely shift focus to recycler before reloading cells
    if (recycler) {
        recycler->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler->resetScrollToTop();
        recycler->reloadData();
        brls::Application::giveFocus(this->recycler);
    }
}

void CatalogView::resetFilters() {
    filterState_.reset();
    filterState_.sort = catalog::SortOption::TITLE_ASC;
    filterCatalog();
    brls::Application::notify("app/catalog/filters_reset"_i18n);
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
                cardBox->getFocusEvent()->subscribe([this, titleLabel, fullTitle, row, i](brls::View* v) {
                    if (v->isFocused()) {
                        parent_->focusedRow_ = row;
                        parent_->focusedCol_ = i;
                        GameRowCell::s_lastFocusedColumn = i;
                        net::ImageDownloader::instance().setFocusedPosition(row, i);
                    }
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
                
                setImageFromHTTPS(cards[i].cover, game.cover, rowCell->imageToken, "romfs:/img/borealis_96.png", false, "", row, i);
                
                cardBox->registerAction("app/actions/toggle_favorite"_i18n, brls::ControllerButton::BUTTON_Y, [game](brls::View* view) {
                    bool fav = catalog::FavoritesManager::instance().toggleFavorite(game);
                    brls::Application::notify(fav ? "app/favorites/added"_i18n : "app/favorites/removed"_i18n);
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
                    cards[i].card->getFocusEvent()->clear();
                    cards[i].card->getFocusLostEvent()->clear();
                    cards[i].card->registerClickAction([](brls::View*) { return false; });
                    cards[i].card->registerAction("", brls::ControllerButton::BUTTON_Y, [](brls::View*) { return false; });
                }
            } catch (...) {}
        }
    }
    
    return rowCell;
}

static std::string getGameInitial(const std::string& title) {
    std::string s = cleanTitle(title);
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 32 || c == '[' || c == '(' || c == '"' || c == '\'' ||
            c == '-' || c == '.' || c == '_' || c == '#' || c == '!' ||
            c == '?' || c == '~' || c == ':' || c == '/' || c == '\\' ||
            c == '+' || c == '=' || c == '*' || c == '@' || c == '$' ||
            c == '%' || c == '^' || c == '&') {
            i++;
            continue;
        }
        if (std::isalpha(c)) {
            return std::string(1, static_cast<char>(std::toupper(c)));
        }
        if (std::isdigit(c)) {
            return "#";
        }
        if (c == 0xD0 && i + 1 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            if (c2 == 0x81) { // Ё -> Ё
                return "\xD0\x81";
            } else if (c2 >= 0x90 && c2 <= 0xAF) { // А..Я
                std::string res;
                res.push_back(static_cast<char>(c));
                res.push_back(static_cast<char>(c2));
                return res;
            } else if (c2 >= 0xB0 && c2 <= 0xBF) { // а..п -> А..П
                std::string res;
                res.push_back(static_cast<char>(0xD0));
                res.push_back(static_cast<char>(c2 - 0x20));
                return res;
            }
            return "#";
        }
        if (c == 0xD1 && i + 1 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            if (c2 == 0x91) { // ё -> Ё
                return "\xD0\x81";
            } else if (c2 >= 0x80 && c2 <= 0x8F) { // р..я -> Р..Я
                std::string res;
                res.push_back(static_cast<char>(0xD0));
                res.push_back(static_cast<char>(c2 + 0x20));
                return res;
            }
            return "#";
        }
        return "#";
    }
    return "#";
}

void CatalogView::jumpToNextLetter(bool forward) {
    if (filteredGames_.empty()) return;

    int currentIdx = std::clamp(focusedRow_ * 6 + focusedCol_, 0, static_cast<int>(filteredGames_.size()) - 1);
    std::string currentInitial = getGameInitial(filteredGames_[currentIdx].title);
    int targetIdx = -1;

    if (forward) {
        for (size_t i = currentIdx + 1; i < filteredGames_.size(); ++i) {
            std::string init = getGameInitial(filteredGames_[i].title);
            if (init != currentInitial) {
                targetIdx = static_cast<int>(i);
                break;
            }
        }
        if (targetIdx == -1 && currentIdx > 0) {
            targetIdx = 0;
        }
    } else {
        size_t firstOfCurrent = currentIdx;
        while (firstOfCurrent > 0 && getGameInitial(filteredGames_[firstOfCurrent - 1].title) == currentInitial) {
            firstOfCurrent--;
        }
        if (currentIdx > static_cast<int>(firstOfCurrent) + 5) {
            targetIdx = static_cast<int>(firstOfCurrent);
        } else if (firstOfCurrent > 0) {
            std::string prevInitial = getGameInitial(filteredGames_[firstOfCurrent - 1].title);
            size_t firstOfPrev = firstOfCurrent - 1;
            while (firstOfPrev > 0 && getGameInitial(filteredGames_[firstOfPrev - 1].title) == prevInitial) {
                firstOfPrev--;
            }
            targetIdx = static_cast<int>(firstOfPrev);
        } else {
            targetIdx = static_cast<int>(filteredGames_.size() - 1);
            std::string lastInitial = getGameInitial(filteredGames_[targetIdx].title);
            while (targetIdx > 0 && getGameInitial(filteredGames_[targetIdx - 1].title) == lastInitial) {
                targetIdx--;
            }
        }
    }

    if (targetIdx >= 0 && targetIdx < static_cast<int>(filteredGames_.size())) {
        int targetRow = targetIdx / 6;
        int targetCol = targetIdx % 6;
        focusedRow_ = targetRow;
        focusedCol_ = targetCol;
        GameRowCell::s_lastFocusedColumn = targetCol;

        if (recycler) {
            recycler->setDefaultCellFocus(brls::IndexPath(0, targetRow));
            recycler->selectRowAt(brls::IndexPath(0, targetRow), false);
            brls::Application::giveFocus(recycler);
        }

        std::string targetInit = getGameInitial(filteredGames_[targetIdx].title);
        std::string toast = "[" + targetInit + "]";
        brls::Application::clearNotifications();
        brls::Application::notify(toast);
    }
}

void CatalogView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    g_activeCatalogView = this;
}

void CatalogView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    if (g_activeCatalogView == this) {
        g_activeCatalogView = nullptr;
    }
}

} // namespace ui
