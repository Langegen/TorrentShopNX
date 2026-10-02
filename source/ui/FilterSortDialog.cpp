#include "FilterSortDialog.hpp"
#include "MultiSelectDialog.hpp"
#include <borealis/views/cells/cell_selector.hpp>
#include <borealis/views/cells/cell_detail.hpp>
#include <borealis/views/cells/cell_bool.hpp>
#include <borealis/views/dialog.hpp>
#include <memory>
#include <algorithm>

namespace ui {

static std::string formatMultiSummary(const std::vector<std::string>& names, const std::string& allLabel) {
    if (names.empty()) {
        return allLabel;
    }
    if (names.size() == 1) {
        return names[0];
    }
    if (names.size() == 2) {
        return names[0] + ", " + names[1];
    }
    return brls::getStr("app/filter/selected_count", std::to_string(names.size()));
}

void FilterSortDialog::show(const catalog::FilterSortState& currentState,
                            const std::vector<Game>& games,
                            std::function<void(const catalog::FilterSortState&)> onApply,
                            std::function<void()> onReset) {
    auto statePtr = std::make_shared<catalog::FilterSortState>(currentState);

    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(20, 20, 10, 20);
    content->setWidth(720);

    brls::Label* title = new brls::Label();
    title->setText("app/filter/title"_i18n);
    title->setFontSize(22);
    title->setMarginBottom(12);
    content->addView(title);

    // 1. Sort option (Single choice)
    brls::SelectorCell* sortCell = new brls::SelectorCell();
    const auto& sortNames = catalog::getSortOptionNames();
    int initialSort = static_cast<int>(statePtr->sort);
    if (initialSort < 0 || initialSort >= static_cast<int>(sortNames.size())) initialSort = 0;
    sortCell->init("app/filter/sort_label"_i18n, sortNames, initialSort, [statePtr](int selected) {
        statePtr->sort = static_cast<catalog::SortOption>(selected);
    });
    content->addView(sortCell);

    // 2. Genre option (Multi-select)
    brls::DetailCell* genreCell = new brls::DetailCell();
    std::vector<catalog::GenreFilterItem> genreItems = catalog::getGenreFilterItems(games, 5);
    std::vector<ui::MultiSelectItem> genreOptions;
    genreOptions.reserve(genreItems.size());
    for (const auto& item : genreItems) {
        genreOptions.push_back({item.id, item.displayName});
    }

    if (statePtr->genres.empty() && !statePtr->genre.empty()) {
        statePtr->genres.push_back(statePtr->genre);
    }

    auto updateGenreDetail = [genreCell, statePtr, genreItems]() {
        std::vector<std::string> names;
        for (const auto& id : statePtr->genres) {
            for (const auto& item : genreItems) {
                if (!item.id.empty() && item.id == id) {
                    names.push_back(catalog::getLocalizedGenreName(item.id));
                    break;
                }
            }
        }
        genreCell->setDetailText(formatMultiSummary(names, "app/filter/all_genres"_i18n));
    };
    genreCell->setText("app/filter/genre_label"_i18n);
    updateGenreDetail();

    genreCell->registerClickAction([genreOptions, statePtr, updateGenreDetail](brls::View* view) {
        MultiSelectDialog::open("app/filter/genre_label"_i18n, genreOptions, statePtr->genres,
            [statePtr, updateGenreDetail](const std::vector<std::string>& selected) {
                statePtr->genres = selected;
                statePtr->genre = selected.empty() ? "" : selected[0];
                updateGenreDetail();
            });
        return true;
    });
    content->addView(genreCell);

    // 3. Language option (Multi-select)
    brls::DetailCell* langCell = new brls::DetailCell();
    const auto& langNames = catalog::getLanguageFilterNames();
    std::vector<ui::MultiSelectItem> langOptions;
    langOptions.reserve(langNames.size());
    langOptions.push_back({"", langNames[0]});
    for (size_t i = 1; i < langNames.size(); ++i) {
        langOptions.push_back({std::to_string(i), langNames[i]});
    }

    if (statePtr->langs.empty() && statePtr->lang != catalog::LanguageFilter::ALL) {
        statePtr->langs.push_back(statePtr->lang);
    }

    auto updateLangDetail = [langCell, statePtr, langNames]() {
        std::vector<std::string> names;
        for (auto lf : statePtr->langs) {
            int idx = static_cast<int>(lf);
            if (idx > 0 && idx < static_cast<int>(langNames.size())) {
                names.push_back(langNames[idx]);
            }
        }
        langCell->setDetailText(formatMultiSummary(names, "app/filter/lang_all"_i18n));
    };
    langCell->setText("app/filter/lang_label"_i18n);
    updateLangDetail();

    langCell->registerClickAction([langOptions, statePtr, updateLangDetail](brls::View* view) {
        std::vector<std::string> currentSelected;
        for (auto lf : statePtr->langs) {
            int idx = static_cast<int>(lf);
            if (idx > 0) {
                currentSelected.push_back(std::to_string(idx));
            }
        }
        MultiSelectDialog::open("app/filter/lang_label"_i18n, langOptions, currentSelected,
            [statePtr, updateLangDetail](const std::vector<std::string>& selected) {
                statePtr->langs.clear();
                for (const auto& s : selected) {
                    int val = std::atoi(s.c_str());
                    if (val > 0) {
                        statePtr->langs.push_back(static_cast<catalog::LanguageFilter>(val));
                    }
                }
                statePtr->lang = statePtr->langs.empty() ? catalog::LanguageFilter::ALL : statePtr->langs[0];
                updateLangDetail();
            });
        return true;
    });
    content->addView(langCell);

    // 4. Favorites option (Boolean)
    brls::BooleanCell* favCell = new brls::BooleanCell();
    favCell->init("app/filter/only_favorites"_i18n, statePtr->onlyFavorites, [statePtr](bool value) {
        statePtr->onlyFavorites = value;
    });
    content->addView(favCell);

    // 5. Year option (Multi-select)
    brls::DetailCell* yearCell = new brls::DetailCell();
    std::vector<std::string> years = catalog::extractYears(games);
    std::vector<ui::MultiSelectItem> yearOptions;
    yearOptions.reserve(years.size());
    yearOptions.push_back({"", years.empty() ? "app/filter/all_years"_i18n : years[0]});
    for (size_t i = 1; i < years.size(); ++i) {
        yearOptions.push_back({years[i], years[i]});
    }

    if (statePtr->years.empty() && !statePtr->year.empty()) {
        statePtr->years.push_back(statePtr->year);
    }

    auto updateYearDetail = [yearCell, statePtr]() {
        yearCell->setDetailText(formatMultiSummary(statePtr->years, "app/filter/all_years"_i18n));
    };
    yearCell->setText("app/filter/year_label"_i18n);
    updateYearDetail();

    yearCell->registerClickAction([yearOptions, statePtr, updateYearDetail](brls::View* view) {
        MultiSelectDialog::open("app/filter/year_label"_i18n, yearOptions, statePtr->years,
            [statePtr, updateYearDetail](const std::vector<std::string>& selected) {
                statePtr->years = selected;
                statePtr->year = selected.empty() ? "" : selected[0];
                updateYearDetail();
            });
        return true;
    });
    content->addView(yearCell);

    // 6. Players option (Multi-select)
    brls::DetailCell* playersCell = new brls::DetailCell();
    const auto& playerFilterNames = catalog::getPlayerFilterNames();
    std::vector<ui::MultiSelectItem> playerOptions;
    playerOptions.reserve(playerFilterNames.size());
    playerOptions.push_back({"", playerFilterNames[0]});
    for (size_t i = 1; i < playerFilterNames.size(); ++i) {
        playerOptions.push_back({std::to_string(i), playerFilterNames[i]});
    }

    if (statePtr->playersList.empty() && statePtr->players != catalog::PlayersFilter::ALL) {
        statePtr->playersList.push_back(statePtr->players);
    }

    auto updatePlayersDetail = [playersCell, statePtr, playerFilterNames]() {
        std::vector<std::string> names;
        for (auto pf : statePtr->playersList) {
            int idx = static_cast<int>(pf);
            if (idx > 0 && idx < static_cast<int>(playerFilterNames.size())) {
                names.push_back(playerFilterNames[idx]);
            }
        }
        playersCell->setDetailText(formatMultiSummary(names, "app/filter/players_all"_i18n));
    };
    playersCell->setText("app/filter/players_label"_i18n);
    updatePlayersDetail();

    playersCell->registerClickAction([playerOptions, statePtr, updatePlayersDetail](brls::View* view) {
        std::vector<std::string> currentSelected;
        for (auto pf : statePtr->playersList) {
            int idx = static_cast<int>(pf);
            if (idx > 0) {
                currentSelected.push_back(std::to_string(idx));
            }
        }
        MultiSelectDialog::open("app/filter/players_label"_i18n, playerOptions, currentSelected,
            [statePtr, updatePlayersDetail](const std::vector<std::string>& selected) {
                statePtr->playersList.clear();
                for (const auto& s : selected) {
                    int val = std::atoi(s.c_str());
                    if (val > 0) {
                        statePtr->playersList.push_back(static_cast<catalog::PlayersFilter>(val));
                    }
                }
                statePtr->players = statePtr->playersList.empty() ? catalog::PlayersFilter::ALL : statePtr->playersList[0];
                updatePlayersDetail();
            });
        return true;
    });
    content->addView(playersCell);

    brls::Dialog* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);

    dialog->addButton("app/common/apply"_i18n, [statePtr, onApply]() {
        if (onApply) onApply(*statePtr);
    });

    dialog->addButton("app/actions/reset_all"_i18n, [onReset]() {
        if (onReset) onReset();
    });

    dialog->open();
}

} // namespace ui
