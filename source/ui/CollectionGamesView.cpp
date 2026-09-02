#include "CollectionGamesView.hpp"
#include "GameDetailView.hpp"
#include "FavoritesManager.hpp"
#include "FilterSortDialog.hpp"
#include "../catalog/filter_manager.hpp"
#include "../utils/log.h"

#include <cstdio>
#include <algorithm>

#ifdef __SWITCH__
#include <switch.h>
static std::string showCollectionKeyboard(const char* hint) {
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
static std::string showCollectionKeyboard(const char* hint) {
    return "";
}
#endif

extern std::vector<Game> g_games;

namespace ui {

// COLLECTIONGAMEROWCELL IMPLEMENTATION
int CollectionGameRowCell::s_lastFocusedColumn = 0;

CollectionGameRowCell::CollectionGameRowCell() {
    this->inflateFromXMLRes("xml/collection_grid_cell.xml");

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

CollectionGameRowCell::~CollectionGameRowCell() {
    if (imageToken) *imageToken = false;
}

CollectionGameRowCell* CollectionGameRowCell::create() {
    return new CollectionGameRowCell();
}

void CollectionGameRowCell::prepareForReuse() {
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

brls::View* CollectionGameRowCell::getDefaultFocus() {
    brls::Box* cards[] = { card0, card1, card2, card3, card4, card5 };
    if (s_lastFocusedColumn >= 0 && s_lastFocusedColumn < 6) {
        if (cards[s_lastFocusedColumn] &&
            cards[s_lastFocusedColumn]->getVisibility() == brls::Visibility::VISIBLE) {
            return cards[s_lastFocusedColumn];
        }
    }
    for (auto* card : cards) {
        if (card && card->getVisibility() == brls::Visibility::VISIBLE) return card;
    }
    return nullptr;
}

// COLLECTIONGAMESVIEW IMPLEMENTATION
CollectionGamesView::CollectionGamesView(const catalog::CollectionInfo& info)
    : info_(info), alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
}

CollectionGamesView::~CollectionGamesView() {
    *alive_flag_ = false;
}

void CollectionGamesView::onContentAvailable() {
    headerTitle->setText(brls::getStr("app/collections/collection_prefix", info_.getName()));
    statsHint->setText(brls::getStr("app/collections/games_count", "..."));

    this->registerAction("app/actions/search"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        std::string query = showCollectionKeyboard("app/collections/search_hint"_i18n.c_str());
        filterState_.searchQuery = query;
        rebuildDisplay();
        return true;
    });

    this->registerAction("", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        FilterSortDialog::show(filterState_, g_games, [this](const catalog::FilterSortState& newState) {
            filterState_ = newState;
            rebuildDisplay();
        }, [this]() {
            resetFilters();
        });
        return true;
    }, true);

    this->registerAction("", brls::ControllerButton::BUTTON_LB, [this](brls::View* view) {
        resetFilters();
        return true;
    }, true);

    recycler->registerCell("Row", []() { return CollectionGameRowCell::create(); });
    recycler->setDataSource(new GamesDataSource(this));
    recycler->setVisibility(brls::Visibility::GONE);

    loadAndShow();
}

void CollectionGamesView::loadAndShow() {
    if (loading_) return;
    loading_ = true;

    loadingLabel->setText("app/common/loading"_i18n);
    loadingLabel->setVisibility(brls::Visibility::VISIBLE);
    recycler->setVisibility(brls::Visibility::GONE);

    std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
    catalog::CollectionInfo info = info_;

    brls::async([this, flag, info]() {
        std::vector<catalog::CollectionEntry> entries;
        bool from_cache = true;
        bool ok = catalog::CollectionsManager::instance().loadCollection(info, entries, from_cache);

        if (!flag->load()) return;

        std::vector<Game> matchedGames(entries.size());
        catalog::CollectionMatchIndex matchIndex;

        if (ok && !entries.empty()) {
            if (info.id == "ports_homebrew") {
                // Direct O(1) matching from g_games (where isHomebrewGame is true)
                size_t entryIdx = 0;
                for (const auto& g : g_games) {
                    if (isHomebrewGame(g) && entryIdx < entries.size()) {
                        matchedGames[entryIdx] = g;
                        entryIdx++;
                    }
                }
            } else {
                matchIndex = catalog::buildMatchIndex(g_games);
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (!flag->load()) return;
                    const Game* matched = catalog::matchWithIndex(matchIndex, entries[i]);
                    if (matched) {
                        matchedGames[i] = *matched;
                    }
                }
            }
        }

        if (!flag->load()) return;

        brls::sync([this, flag, ok, entries = std::move(entries),
                    matchedGames = std::move(matchedGames),
                    matchIndex = std::move(matchIndex), from_cache]() mutable {
            if (!flag->load()) return;
            loading_ = false;

            if (!ok) {
                loadingLabel->setText("app/collections/load_failed"_i18n);
                loadingLabel->setVisibility(brls::Visibility::VISIBLE);
                recycler->setVisibility(brls::Visibility::GONE);
                return;
            }

            entries_ = std::move(entries);
            matchedGames_ = std::move(matchedGames);
            matchedComputed_.assign(entries_.size(), true);
            matchIndex_ = std::move(matchIndex);

            if (entries_.empty()) {
                loadingLabel->setText("app/collections/empty"_i18n);
                loadingLabel->setVisibility(brls::Visibility::VISIBLE);
                recycler->setVisibility(brls::Visibility::GONE);
                return;
            }

            loadingLabel->setVisibility(brls::Visibility::GONE);
            recycler->setVisibility(brls::Visibility::VISIBLE);

            rebuildDisplay();

            util::logLine("CollectionGamesView: loaded " + std::to_string(entries_.size()) +
                          " entries for " + info_.id +
                          (from_cache ? " (cache)" : " (network)"));
        });
    });
}

void CollectionGamesView::ensureAllMatches() {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (!matchedComputed_[i]) {
            const Game* matched = catalog::matchWithIndex(matchIndex_, entries_[i]);
            if (matched) {
                matchedGames_[i] = *matched;
            }
            matchedComputed_[i] = true;
        }
    }
}

void CollectionGamesView::rebuildDisplay() {
    ensureAllMatches();

    displayIdx_.clear();
    displayIdx_.reserve(entries_.size());

    auto& fm = catalog::FavoritesManager::instance();

    for (size_t i = 0; i < entries_.size(); ++i) {
        Game effectiveGame = matchedGames_[i];
        if (effectiveGame.title.empty()) {
            effectiveGame.title = entries_[i].title;
            effectiveGame.title_id = entries_[i].title_id;
        }

        bool isFav = filterState_.onlyFavorites ? fm.isFavorite(effectiveGame) : false;

        // Check if game passes the filter state
        if (catalog::matchesGameFilter(effectiveGame, filterState_, isFav)) {
            displayIdx_.push_back(static_cast<int>(i));
        }
    }

    if (filterState_.sort != catalog::SortOption::DEFAULT) {
        std::stable_sort(displayIdx_.begin(), displayIdx_.end(), [this](int a, int b) {
            Game gameA = matchedGames_[a];
            if (gameA.title.empty()) {
                gameA.title = entries_[a].title;
                gameA.title_id = entries_[a].title_id;
            }
            Game gameB = matchedGames_[b];
            if (gameB.title.empty()) {
                gameB.title = entries_[b].title;
                gameB.title_id = entries_[b].title_id;
            }
            return catalog::compareGames(gameA, gameB, filterState_.sort);
        });
    }

    std::string countStr;
    if (displayIdx_.size() != entries_.size()) {
        countStr = brls::getStr("app/collections/shown_of", std::to_string(displayIdx_.size()), std::to_string(entries_.size()));
    } else {
        countStr = brls::getStr("app/collections/games_count", std::to_string(displayIdx_.size()));
    }
    statsHint->setText(countStr);

    if (recycler && recycler->getVisibility() == brls::Visibility::VISIBLE) {
        CollectionGameRowCell::s_lastFocusedColumn = 0;
        recycler->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler->resetScrollToTop();
        recycler->reloadData();
        brls::Application::giveFocus(recycler);
    }
}

void CollectionGamesView::resetFilters() {
    filterState_.reset();
    rebuildDisplay();
    brls::Application::notify("app/catalog/filters_reset"_i18n);
}

// DATASOURCE IMPLEMENTATION
int CollectionGamesView::GamesDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    return (static_cast<int>(parent_->displayIdx_.size()) + 5) / 6;
}

brls::RecyclerCell* CollectionGamesView::GamesDataSource::cellForRow(
    brls::RecyclerFrame* recycler, brls::IndexPath index) {
    CollectionGameRowCell* rowCell =
        dynamic_cast<CollectionGameRowCell*>(recycler->dequeueReusableCell("Row"));
    if (!rowCell) return nullptr;

    if (rowCell->imageToken) *(rowCell->imageToken) = false;
    rowCell->imageToken = std::make_shared<bool>(true);

    int row = index.row;

    struct CardRefs {
        brls::Box* card;
        brls::Image* cover;
        brls::Label* meta;
        brls::Label* lang;
        brls::Label* title;
        brls::Label* size;
    } cards[6];

    try {
        cards[0] = { rowCell->card0, rowCell->cover0, rowCell->meta0, rowCell->lang0, rowCell->title0, rowCell->size0 };
        cards[1] = { rowCell->card1, rowCell->cover1, rowCell->meta1, rowCell->lang1, rowCell->title1, rowCell->size1 };
        cards[2] = { rowCell->card2, rowCell->cover2, rowCell->meta2, rowCell->lang2, rowCell->title2, rowCell->size2 };
        cards[3] = { rowCell->card3, rowCell->cover3, rowCell->meta3, rowCell->lang3, rowCell->title3, rowCell->size3 };
        cards[4] = { rowCell->card4, rowCell->cover4, rowCell->meta4, rowCell->lang4, rowCell->title4, rowCell->size4 };
        cards[5] = { rowCell->card5, rowCell->cover5, rowCell->meta5, rowCell->lang5, rowCell->title5, rowCell->size5 };
    } catch (const std::exception& e) {
        brls::Logger::error("CollectionGamesDataSource: EXCEPTION resolving BRLS_BIND variables: {}", e.what());
        return rowCell;
    } catch (...) {
        brls::Logger::error("CollectionGamesDataSource: UNKNOWN EXCEPTION resolving BRLS_BIND variables");
        return rowCell;
    }

    for (int i = 0; i < 6; ++i) {
        size_t slot = static_cast<size_t>(row * 6 + i);
        if (slot < parent_->displayIdx_.size()) {
            try {
                brls::Box* cardBox = cards[i].card;
                if (!cardBox) continue;

                int idx = parent_->displayIdx_[slot];
                const auto& entry = parent_->entries_[idx];

                cardBox->setVisibility(brls::Visibility::VISIBLE);
                cardBox->setFocusable(true);

                std::string fullTitle = entry.title;
                std::string shortTitle = truncateCatalogTitle(fullTitle);
                brls::Label* titleLabel = cards[i].title;

                titleLabel->setText(shortTitle);

                cardBox->getFocusEvent()->clear();
                cardBox->getFocusLostEvent()->clear();

                cardBox->getFocusEvent()->subscribe([titleLabel, fullTitle, row, i](brls::View* v) {
                    if (v->isFocused()) {
                        CollectionGameRowCell::s_lastFocusedColumn = i;
                        net::ImageDownloader::instance().setFocusedPosition(row, i);
                    }
                    titleLabel->setText(fullTitle);
                    titleLabel->setAnimated(true);
                });

                cardBox->getFocusLostEvent()->subscribe([titleLabel, shortTitle](brls::View*) {
                    titleLabel->setAnimated(false);
                    titleLabel->setText(shortTitle);
                });

                // Metacritic badge (top-right of the cover)
                brls::Label* metaLabel = cards[i].meta;
                if (metaLabel) {
                    if (entry.has_metacritic) {
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), " %.0f ", entry.metacritic);
                        metaLabel->setText(buf);
                        NVGcolor color = entry.metacritic >= 75.0 ? nvgRGB(0x66, 0xCC, 0x33)
                                       : entry.metacritic >= 50.0 ? nvgRGB(0xFF, 0xCC, 0x33)
                                                                  : nvgRGB(0xFF, 0x33, 0x00);
                        metaLabel->setBackgroundColor(color);
                        metaLabel->setVisibility(brls::Visibility::VISIBLE);
                    } else {
                        metaLabel->setVisibility(brls::Visibility::GONE);
                    }
                }

                // Fast indexed match (O(1) by title_id or single normalized scan)
                if (!parent_->matchedComputed_[idx]) {
                    const Game* matched = catalog::matchWithIndex(parent_->matchIndex_, entry);
                    if (matched) {
                        parent_->matchedGames_[idx] = *matched;
                    }
                    parent_->matchedComputed_[idx] = true;
                }

                bool hasMatch = parent_->matchedComputed_[idx] &&
                                !parent_->matchedGames_[idx].title.empty();

                brls::Label* sizeLabel = cards[i].size;
                if (sizeLabel) {
                    sizeLabel->setText(hasMatch ? parent_->matchedGames_[idx].size : "");
                }

                brls::Label* langLabel = cards[i].lang;
                if (langLabel) {
                    if (hasMatch) {
                        std::string lang = extractLangBadge(parent_->matchedGames_[idx].interface_lang);
                        if (!lang.empty()) {
                            langLabel->setVisibility(brls::Visibility::VISIBLE);
                            langLabel->setText(" " + lang + " ");
                        } else {
                            langLabel->setVisibility(brls::Visibility::GONE);
                        }
                    } else {
                        langLabel->setVisibility(brls::Visibility::GONE);
                    }
                }

                std::string coverUrl = hasMatch ? parent_->matchedGames_[idx].cover : "";
                if (i == 0 && parent_->coverLogBudget_ > 0) {
                    parent_->coverLogBudget_--;
                    util::logLine("collections: covers row=" + std::to_string(row) +
                                  " url=" + (coverUrl.empty() ? "(none)" : coverUrl));
                }
                setImageFromHTTPS(cards[i].cover, coverUrl,
                                  rowCell->imageToken, "romfs:/img/borealis_96.png",
                                  false, "", row, i);

                Game matchedCopy;
                if (hasMatch) matchedCopy = parent_->matchedGames_[idx];

                if (hasMatch) {
                    cardBox->registerAction("app/actions/toggle_favorite"_i18n, brls::ControllerButton::BUTTON_Y, [matchedCopy](brls::View* view) {
                        bool fav = catalog::FavoritesManager::instance().toggleFavorite(matchedCopy);
                        brls::Application::notify(fav ? "app/favorites/added"_i18n : "app/favorites/removed"_i18n);
                        return true;
                    });
                } else {
                    cardBox->registerAction("", brls::ControllerButton::BUTTON_Y, [](brls::View*) { return false; });
                }

                cardBox->registerClickAction([matchedCopy, hasMatch](brls::View*) {
                    if (hasMatch) {
                        brls::Application::pushActivity(new GameDetailView(matchedCopy));
                    } else {
                        brls::Application::notify("app/collections/not_found_in_catalog"_i18n);
                    }
                    return true;
                });
            } catch (const std::exception& e) {
                brls::Logger::error("CollectionGamesDataSource: EXCEPTION in card setup {}: {}", i, e.what());
            } catch (...) {
                brls::Logger::error("CollectionGamesDataSource: UNKNOWN EXCEPTION in card setup {}", i);
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

} // namespace ui