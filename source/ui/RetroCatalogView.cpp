#include "RetroCatalogView.hpp"
#include "GameDetailView.hpp"
#include "FilterSortDialog.hpp"
#include "FavoritesManager.hpp"
#include "../utils/log.h"
#include <algorithm>

namespace ui {

namespace {

#ifdef __SWITCH__
#include <switch.h>
static std::string showRetroKeyboard(const char* hint) {
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
static std::string showRetroKeyboard(const char* hint) {
    return "";
}
#endif

} // namespace

int RetroGridRowCell::s_lastFocusedColumn = 0;

// === RETRO GRID ROW CELL (6 Columns) ===
RetroGridRowCell::RetroGridRowCell() {
    this->inflateFromXMLRes("xml/retro_grid_cell.xml");

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

RetroGridRowCell::~RetroGridRowCell() {
    if (imageToken) *imageToken = false;
}

RetroGridRowCell* RetroGridRowCell::create() {
    return new RetroGridRowCell();
}

void RetroGridRowCell::prepareForReuse() {
    brls::RecyclerCell::prepareForReuse();

    if (imageToken) {
        *imageToken = false;
        imageToken.reset();
    }

    brls::Box* cards[] = { card0, card1, card2, card3, card4, card5 };
    brls::Label* titles[] = { title0, title1, title2, title3, title4, title5 };

    for (int i = 0; i < 6; ++i) {
        if (cards[i]) {
            cards[i]->setHighlighted(false);
            cards[i]->setHighlightProgress(0.0f);
        }
        if (titles[i]) {
            titles[i]->setAnimated(false);
            titles[i]->setTextColor(nvgRGBA(255, 255, 255, 255));
        }
    }
}

brls::View* RetroGridRowCell::getDefaultFocus() {
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

// === RETRO LIST ROW CELL (1 Row per Game) ===
RetroListRowCell::RetroListRowCell() {
    this->inflateFromXMLRes("xml/retro_list_cell.xml");
}

RetroListRowCell::~RetroListRowCell() {
    if (imageToken) *imageToken = false;
}

RetroListRowCell* RetroListRowCell::create() {
    return new RetroListRowCell();
}

void RetroListRowCell::prepareForReuse() {
    brls::RecyclerCell::prepareForReuse();

    if (imageToken) {
        *imageToken = false;
        imageToken.reset();
    }

    this->setHighlighted(false);
    this->setHighlightProgress(0.0f);
    this->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
    if (title) {
        title->setTextColor(nvgRGBA(255, 255, 255, 255));
    }
}

brls::View* RetroListRowCell::getDefaultFocus() {
    return this;
}

// === RETRO CATALOG VIEW ===
RetroCatalogView::RetroCatalogView(const catalog::RetroConsoleInfo& consoleInfo)
    : consoleInfo_(consoleInfo), alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
}

RetroCatalogView::~RetroCatalogView() {
    *alive_flag_ = false;
}

void RetroCatalogView::onContentAvailable() {
    if (titleLabel) {
        titleLabel->setText(consoleInfo_.name);
        titleLabel->addGestureRecognizer(new brls::TapGestureRecognizer(titleLabel, [this]() {
            std::string query = showRetroKeyboard("Поиск по названию игры...");
            filterState_.searchQuery = query;
            filterCatalog();
        }));
    }
    if (statsHint) {
        statsHint->setText("Загрузка базы данных...");
        statsHint->addGestureRecognizer(new brls::TapGestureRecognizer(statsHint, [this]() {
            FilterSortDialog::show(filterState_, allGames_, [this](const catalog::FilterSortState& newState) {
                filterState_ = newState;
                filterCatalog();
            }, [this]() {
                resetFilters();
            });
        }));
    }

    // Register search/filter action keys
    this->registerAction("app/actions/search"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View* view) {
        std::string query = showRetroKeyboard("Поиск по названию игры...");
        filterState_.searchQuery = query;
        filterCatalog();
        return true;
    });

    this->registerAction("", brls::ControllerButton::BUTTON_RB, [this](brls::View* view) {
        FilterSortDialog::show(filterState_, allGames_, [this](const catalog::FilterSortState& newState) {
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

    this->registerAction("В избранное / Убрать", brls::ControllerButton::BUTTON_Y, [this](brls::View* view) {
        if (focusedSection_ < sections_.size() && sections_[focusedSection_].games &&
            focusedGameIndex_ < sections_[focusedSection_].games->size()) {
            const auto& game = (*sections_[focusedSection_].games)[focusedGameIndex_];
            bool fav = catalog::FavoritesManager::instance().toggleFavorite(game);
            brls::Application::notify(fav ? "Добавлено в избранное: " + cleanRetroTitle(game.title)
                                          : "Удалено из избранного: " + cleanRetroTitle(game.title));
        }
        return true;
    });

    // L3 Toggle: Grid vs List view (hidden from bottom bar, already in statsHint)
    this->registerAction("", brls::ControllerButton::BUTTON_LSB, [this](brls::View* view) {
        toggleViewMode();
        return true;
    }, true);

    // (-) Refresh catalog from GitHub (hidden from bottom bar, already in statsHint)
    this->registerAction("", brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        refreshCatalog();
        return true;
    }, true);

    // Register recycler cells
    recycler->registerCell("GridRow", []() { return RetroGridRowCell::create(); });
    recycler->registerCell("ListRow", []() { return RetroListRowCell::create(); });
    RetroGridRowCell::s_lastFocusedColumn = 0;
    recycler->setDefaultCellFocus(brls::IndexPath(0, 0));
    recycler->setDataSource(new RetroDataSource(this));

    // Asynchronously load console games from cache / network
    std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
    std::string cid = consoleInfo_.id;

    util::logLine("RetroCatalogView: onContentAvailable for " + consoleInfo_.name + " (" + cid + ")");

    brls::async([this, flag, cid]() {
        util::logLine("RetroCatalogView: starting async loadConsoleGames for " + cid);
        std::vector<Game> loaded;
        bool ok = catalog::RetroCatalogManager::instance().loadConsoleGames(
            cid, loaded, true,
            [this, flag](float prog, const std::string& status) {
                if (!flag->load()) return;
                brls::sync([this, flag, prog, status]() {
                    if (!flag->load()) return;
                    if (loadingLabel) loadingLabel->setText(status);
                    if (statsHint) {
                        if (prog > 0.0f && prog < 1.0f) {
                            statsHint->setText(status + " (" + std::to_string(static_cast<int>(prog * 100)) + "%)");
                        } else {
                            statsHint->setText(status);
                        }
                    }
                });
            }
        );

        if (!flag->load()) return;

        brls::sync([this, flag, ok, games = std::move(loaded)]() mutable {
            if (!flag->load()) return;
            util::logLine("RetroCatalogView: loadConsoleGames returned ok=" + std::to_string(ok) + " count=" + std::to_string(games.size()));

            if (!ok || games.empty()) {
                if (loadingLabel) loadingLabel->setText("Не удалось загрузить базу игр");
                if (statsHint) statsHint->setText("Ошибка загрузки");
                return;
            }

            allGames_ = std::move(games);
            filterCatalog();

            if (loadingBox) loadingBox->setVisibility(brls::Visibility::GONE);
            if (recycler) {
                recycler->setVisibility(brls::Visibility::VISIBLE);
                recycler->resetScrollToTop();
                recycler->reloadData();
                brls::Application::giveFocus(recycler);
            }
        });
    });
}

void RetroCatalogView::toggleViewMode() {
    if (sections_.empty() || !recycler) return;

    isListView_ = !isListView_;

    size_t targetSection = focusedSection_;
    if (targetSection >= sections_.size()) targetSection = 0;

    size_t targetIdx = focusedGameIndex_;
    if (sections_[targetSection].games && targetIdx >= sections_[targetSection].games->size()) targetIdx = 0;

    int targetRow = isListView_ ? static_cast<int>(targetIdx) : static_cast<int>(targetIdx / 6);
    RetroGridRowCell::s_lastFocusedColumn = isListView_ ? 0 : static_cast<int>(targetIdx % 6);

    if (statsHint) {
        std::string modeStr = isListView_ ? "Список" : "Сетка";
        size_t total = filteredRomsets_.size() + filteredStandalone_.size();
        std::string countStr = "Игр: " + std::to_string(total);
        if (!filteredRomsets_.empty()) {
            countStr += " (сборников: " + std::to_string(filteredRomsets_.size()) + ")";
        }
        statsHint->setText(countStr + " | " + modeStr + "  (LS) Вид  (-) Обновить  R Фильтр");
    }

    recycler->setDefaultCellFocus(brls::IndexPath(static_cast<int>(targetSection), targetRow));
    recycler->reloadData();

    brls::sync([this, targetSection, targetRow]() {
        if (recycler) {
            recycler->selectRowAt(brls::IndexPath(static_cast<int>(targetSection), targetRow), false);
            brls::View* defFocus = recycler->getDefaultFocus();
            if (defFocus) {
                brls::Application::giveFocus(defFocus);
            }
        }
    });
}

void RetroCatalogView::refreshCatalog() {
    if (loadingBox && loadingBox->getVisibility() == brls::Visibility::VISIBLE) {
        return; // Already loading
    }

    if (loadingBox) {
        loadingBox->setVisibility(brls::Visibility::VISIBLE);
        if (progressSpinner) progressSpinner->setVisibility(brls::Visibility::VISIBLE);
    }
    if (loadingLabel) loadingLabel->setText("Обновление базы с GitHub...");
    if (statsHint) statsHint->setText("Загрузка обновления базы...");

    std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
    std::string cid = consoleInfo_.id;

    brls::async([this, flag, cid]() {
        std::vector<Game> updated;
        bool ok = catalog::RetroCatalogManager::instance().refreshConsoleCatalog(
            cid, updated,
            [this, flag](float prog, const std::string& status) {
                if (!flag->load()) return;
                brls::sync([this, flag, prog, status]() {
                    if (!flag->load()) return;
                    if (loadingLabel) loadingLabel->setText(status);
                    if (statsHint) {
                        if (prog > 0.0f && prog < 1.0f) {
                            statsHint->setText(status + " (" + std::to_string(static_cast<int>(prog * 100)) + "%)");
                        } else {
                            statsHint->setText(status);
                        }
                    }
                });
            }
        );

        if (!flag->load()) return;

        brls::sync([this, flag, ok, games = std::move(updated)]() mutable {
            if (!flag->load()) return;
            if (loadingBox) loadingBox->setVisibility(brls::Visibility::GONE);

            if (!ok || games.empty()) {
                brls::Application::notify("Не удалось обновить базу (проверьте интернет)");
                filterCatalog();
                return;
            }

            allGames_ = std::move(games);
            filterCatalog();

            if (recycler) {
                recycler->reloadData();
            }
            brls::Application::notify("База обновлена! Игр: " + std::to_string(allGames_.size()));
        });
    });
}

void RetroCatalogView::filterCatalog() {
    filteredRomsets_.clear();
    filteredStandalone_.clear();
    sections_.clear();

    auto& fm = catalog::FavoritesManager::instance();

    for (const auto& game : allGames_) {
        bool isFav = fm.isFavorite(game);
        if (catalog::matchesGameFilter(game, filterState_, isFav)) {
            if (isRomsetGame(game)) {
                filteredRomsets_.push_back(game);
            } else {
                filteredStandalone_.push_back(game);
            }
        }
    }

    if (filterState_.sort != catalog::SortOption::DEFAULT) {
        auto sortFunc = [this](const Game& a, const Game& b) {
            return catalog::compareGames(a, b, filterState_.sort);
        };
        std::sort(filteredRomsets_.begin(), filteredRomsets_.end(), sortFunc);
        std::sort(filteredStandalone_.begin(), filteredStandalone_.end(), sortFunc);
    }

    if (!filteredRomsets_.empty()) {
        sections_.push_back({ "Сборники и ромсеты", true, &filteredRomsets_ });
    }
    if (!filteredStandalone_.empty()) {
        sections_.push_back({ "Отдельные игры", false, &filteredStandalone_ });
    }

    if (statsHint) {
        std::string modeStr = isListView_ ? "Список" : "Сетка";
        size_t totalFiltered = filteredRomsets_.size() + filteredStandalone_.size();
        std::string countStr = "Игр: " + std::to_string(totalFiltered);
        if (totalFiltered != allGames_.size()) {
            countStr += " из " + std::to_string(allGames_.size());
        }
        if (!filteredRomsets_.empty()) {
            countStr += " (сборников: " + std::to_string(filteredRomsets_.size()) + ")";
        }
        statsHint->setText(countStr + " | " + modeStr + "  (LS) Вид  (-) Обновить  R Фильтр");
    }

    if (recycler) {
        focusedSection_ = 0;
        focusedGameIndex_ = 0;
        recycler->setDefaultCellFocus(brls::IndexPath(0, 0));
        recycler->reloadData();
        brls::sync([this]() {
            if (recycler) {
                brls::View* defFocus = recycler->getDefaultFocus();
                if (defFocus) {
                    brls::Application::giveFocus(defFocus);
                }
            }
        });
    }
}

void RetroCatalogView::resetFilters() {
    filterState_.reset();
    filterCatalog();
}

void RetroCatalogView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
}

void RetroCatalogView::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
}

int RetroCatalogView::RetroDataSource::numberOfSections(brls::RecyclerFrame* recycler) {
    return static_cast<int>(parent_->sections_.size());
}

int RetroCatalogView::RetroDataSource::numberOfRows(brls::RecyclerFrame* recycler, int section) {
    if (section < 0 || section >= static_cast<int>(parent_->sections_.size())) return 0;
    const auto* gList = parent_->sections_[section].games;
    if (!gList) return 0;

    if (parent_->isListView_) {
        return static_cast<int>(gList->size());
    } else {
        return static_cast<int>((gList->size() + 5) / 6);
    }
}

std::string RetroCatalogView::RetroDataSource::titleForHeader(brls::RecyclerFrame* recycler, int section) {
    if (section < 0 || section >= static_cast<int>(parent_->sections_.size())) return "";
    if (parent_->sections_.size() == 1 && !parent_->sections_[0].isRomset) {
        return ""; // No header needed if catalog has no romsets
    }
    return parent_->sections_[section].title;
}

float RetroCatalogView::RetroDataSource::heightForHeader(brls::RecyclerFrame* recycler, int section) {
    std::string title = titleForHeader(recycler, section);
    return title.empty() ? 0.0f : 48.0f;
}

brls::RecyclerCell* RetroCatalogView::RetroDataSource::cellForHeader(brls::RecyclerFrame* recycler, int section) {
    brls::RecyclerHeader* header = (brls::RecyclerHeader*)recycler->dequeueReusableCell("brls::Header");
    std::string title = titleForHeader(recycler, section);
    if (!title.empty() && section >= 0 && section < static_cast<int>(parent_->sections_.size())) {
        header->setTitle(title);
        const auto* gList = parent_->sections_[section].games;
        size_t count = gList ? gList->size() : 0;
        std::string countStr = std::to_string(count) + " " + (parent_->sections_[section].isRomset ? "раздач" : "игр");
        header->setSubtitle(countStr);
        header->setVisibility(brls::Visibility::VISIBLE);
        header->setHeight(brls::View::AUTO);
    } else {
        header->setTitle("");
        header->setSubtitle("");
        header->setVisibility(brls::Visibility::GONE);
        header->setHeight(0);
    }
    return header;
}

float RetroCatalogView::RetroDataSource::heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    return parent_->isListView_ ? 82.0f : 300.0f;
}

brls::RecyclerCell* RetroCatalogView::RetroDataSource::cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) {
    if (index.section >= parent_->sections_.size()) return nullptr;
    const auto* gList = parent_->sections_[index.section].games;
    if (!gList) return nullptr;
    const auto& games = *gList;

    // 1. LIST VIEW MODE
    if (parent_->isListView_) {
        RetroListRowCell* cell = dynamic_cast<RetroListRowCell*>(recycler->dequeueReusableCell("ListRow"));
        if (!cell) cell = RetroListRowCell::create();

        if (!cell->imageToken) cell->imageToken = std::make_shared<bool>(true);

        if (index.row < 0 || static_cast<size_t>(index.row) >= games.size()) {
            cell->setVisibility(brls::Visibility::INVISIBLE);
            return cell;
        }

        cell->setVisibility(brls::Visibility::VISIBLE);
        size_t gameIdx = static_cast<size_t>(index.row);
        const auto& game = games[gameIdx];
        std::string cleanName = cleanRetroTitle(game.title);

        cell->title->setText(cleanName);
        cell->size->setText(game.size);

        std::string metaStr = (game.year.empty() ? "" : (game.year + " • ")) +
                              (game.genre.empty() ? parent_->consoleInfo_.name : game.genre);
        cell->meta->setText(metaStr);

        std::string lang = extractLangBadge(game.interface_lang);
        if (!lang.empty()) {
            cell->lang->setVisibility(brls::Visibility::VISIBLE);
            cell->lang->setText(" " + lang + " ");
        } else {
            cell->lang->setVisibility(brls::Visibility::GONE);
        }

        // Action Tag styling: Romset vs Standalone
        bool isRom = isRomsetGame(game);
        if (isRom) {
            cell->actionBox->setBackgroundColor(nvgRGBA(255, 170, 0, 35));
            cell->actionLabel->setTextColor(nvgRGBA(255, 170, 0, 255));
            const auto& cfg = config::ConfigManager::instance();
            if (cfg.getRetroRomsetMode() == "select") {
                cell->actionLabel->setText("(A) Выбрать");
            } else {
                cell->actionLabel->setText("(A) Скачать сет");
            }
        } else {
            cell->actionBox->setBackgroundColor(nvgRGBA(0, 224, 165, 32));
            cell->actionLabel->setTextColor(nvgRGBA(0, 230, 175, 255));
            cell->actionLabel->setText("(A) Скачать РОМ");
        }

        cell->cover->setClipsToBounds(false);
        cell->cover->setScalingType(brls::ImageScalingType::FIT);
        int effectivePriority = (index.section == 1 && parent_->sections_.size() > 1 && parent_->sections_[0].games)
            ? static_cast<int>(parent_->sections_[0].games->size() + gameIdx)
            : static_cast<int>(gameIdx);
        setImageFromHTTPS(cell->cover, game.cover, cell->imageToken, "romfs:/img/retro_cover_placeholder.png", false, "", effectivePriority, 0);

        cell->getFocusEvent()->clear();
        cell->getFocusLostEvent()->clear();

        cell->getFocusEvent()->subscribe([this, cell, section = index.section, gameIdx, effectivePriority](brls::View*) {
            parent_->focusedSection_ = section;
            parent_->focusedGameIndex_ = gameIdx;
            net::ImageDownloader::instance().setFocusedPosition(effectivePriority, 0);
            cell->title->setTextColor(nvgRGBA(0, 230, 175, 255));
            cell->setBackgroundColor(nvgRGBA(0, 224, 165, 30));
        });

        cell->getFocusLostEvent()->subscribe([cell](brls::View*) {
            cell->title->setTextColor(nvgRGBA(255, 255, 255, 255));
            cell->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        });

        std::string cid = parent_->consoleInfo_.id;
        cell->registerClickAction([game, cid](brls::View*) {
            brls::Application::pushActivity(new GameDetailView(game, cid));
            return true;
        });
        cell->registerAction("hints/ok"_i18n, brls::ControllerButton::BUTTON_A, [game, cid](brls::View*) {
            brls::Application::pushActivity(new GameDetailView(game, cid));
            return true;
        });

        return cell;
    }

    // 2. GRID VIEW MODE (6 Columns)
    RetroGridRowCell* rowCell = dynamic_cast<RetroGridRowCell*>(recycler->dequeueReusableCell("GridRow"));
    if (!rowCell) rowCell = RetroGridRowCell::create();

    if (!rowCell->imageToken) rowCell->imageToken = std::make_shared<bool>(true);

    int row = index.row;
    if (row < 0) return rowCell;
    struct CardRefs {
        brls::Box* card;
        brls::Image* cover;
        brls::Label* lang;
        brls::Label* title;
        brls::Label* size;
        brls::Label* romBadge;
    } cards[6] = {
        { rowCell->card0, rowCell->cover0, rowCell->lang0, rowCell->title0, rowCell->size0, rowCell->romBadge0 },
        { rowCell->card1, rowCell->cover1, rowCell->lang1, rowCell->title1, rowCell->size1, rowCell->romBadge1 },
        { rowCell->card2, rowCell->cover2, rowCell->lang2, rowCell->title2, rowCell->size2, rowCell->romBadge2 },
        { rowCell->card3, rowCell->cover3, rowCell->lang3, rowCell->title3, rowCell->size3, rowCell->romBadge3 },
        { rowCell->card4, rowCell->cover4, rowCell->lang4, rowCell->title4, rowCell->size4, rowCell->romBadge4 },
        { rowCell->card5, rowCell->cover5, rowCell->lang5, rowCell->title5, rowCell->size5, rowCell->romBadge5 }
    };

    int effectiveRow = (index.section == 1 && parent_->sections_.size() > 1 && parent_->sections_[0].games)
        ? static_cast<int>((parent_->sections_[0].games->size() + 5) / 6) + row
        : row;

    for (int i = 0; i < 6; ++i) {
        size_t gameIdx = static_cast<size_t>(row * 6 + i);
        if (gameIdx < games.size()) {
            brls::Box* cardBox = cards[i].card;
            if (!cardBox) continue;

            const auto& game = games[gameIdx];
            cardBox->setVisibility(brls::Visibility::VISIBLE);
            cardBox->setFocusable(true);

            std::string fullTitle = cleanRetroTitle(game.title);
            std::string shortTitle = truncateCatalogTitle(fullTitle, 36);
            brls::Label* titleLabel = cards[i].title;

            titleLabel->setText(shortTitle);
            cards[i].size->setText(game.size);

            // Badge styling: ROMSET vs ROM
            bool isRom = isRomsetGame(game);
            if (isRom) {
                cards[i].romBadge->setText("СБОРНИК");
                cards[i].romBadge->setTextColor(nvgRGBA(255, 170, 0, 255));
            } else {
                cards[i].romBadge->setText("ROM");
                cards[i].romBadge->setTextColor(nvgRGBA(0, 230, 175, 255));
            }

            cardBox->getFocusEvent()->clear();
            cardBox->getFocusLostEvent()->clear();

            cardBox->getFocusEvent()->subscribe([this, titleLabel, fullTitle, section = index.section, effectiveRow, i, gameIdx](brls::View* v) {
                if (v->isFocused()) {
                    RetroGridRowCell::s_lastFocusedColumn = i;
                    parent_->focusedSection_ = section;
                    parent_->focusedGameIndex_ = gameIdx;
                    net::ImageDownloader::instance().setFocusedPosition(effectiveRow, i);
                }
                titleLabel->setTextColor(nvgRGBA(0, 230, 175, 255));
                titleLabel->setText(fullTitle);
                titleLabel->setAnimated(true);
            });

            cardBox->getFocusLostEvent()->subscribe([titleLabel, shortTitle](brls::View*) {
                titleLabel->setTextColor(nvgRGBA(255, 255, 255, 255));
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

            cards[i].cover->setClipsToBounds(false);
            cards[i].cover->setScalingType(brls::ImageScalingType::FIT);
            setImageFromHTTPS(cards[i].cover, game.cover, rowCell->imageToken, "romfs:/img/retro_cover_placeholder.png", false, "", effectiveRow, i);

            std::string cid = parent_->consoleInfo_.id;
            cardBox->registerClickAction([game, cid](brls::View*) {
                brls::Application::pushActivity(new GameDetailView(game, cid));
                return true;
            });
            cardBox->registerAction("hints/ok"_i18n, brls::ControllerButton::BUTTON_A, [game, cid](brls::View*) {
                brls::Application::pushActivity(new GameDetailView(game, cid));
                return true;
            });
        } else {
            if (cards[i].card) {
                cards[i].card->setVisibility(brls::Visibility::INVISIBLE);
                cards[i].card->setFocusable(false);
            }
        }
    }

    return rowCell;
}

} // namespace ui
