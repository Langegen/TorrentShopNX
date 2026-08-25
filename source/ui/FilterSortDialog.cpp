#include "FilterSortDialog.hpp"
#include <borealis/views/cells/cell_selector.hpp>
#include <borealis/views/cells/cell_bool.hpp>
#include <borealis/views/dialog.hpp>
#include <memory>
#include <algorithm>

namespace ui {

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

    // 1. Sort option
    brls::SelectorCell* sortCell = new brls::SelectorCell();
    const auto& sortNames = catalog::getSortOptionNames();
    int initialSort = static_cast<int>(statePtr->sort);
    if (initialSort < 0 || initialSort >= static_cast<int>(sortNames.size())) initialSort = 0;
    sortCell->init("app/filter/sort_label"_i18n, sortNames, initialSort, [statePtr](int selected) {
        statePtr->sort = static_cast<catalog::SortOption>(selected);
    });
    content->addView(sortCell);

    // 2. Genre option
    brls::SelectorCell* genreCell = new brls::SelectorCell();
    std::vector<std::string> genres = catalog::extractGenres(games);
    int initialGenre = 0;
    if (!statePtr->genre.empty()) {
        for (size_t i = 0; i < genres.size(); ++i) {
            if (genres[i] == statePtr->genre) {
                initialGenre = static_cast<int>(i);
                break;
            }
        }
    }
    genreCell->init("app/filter/genre_label"_i18n, genres, initialGenre, [statePtr, genres](int selected) {
        if (selected <= 0 || selected >= static_cast<int>(genres.size())) {
            statePtr->genre.clear();
        } else {
            statePtr->genre = genres[selected];
        }
    });
    content->addView(genreCell);

    // 3. Language option
    brls::SelectorCell* langCell = new brls::SelectorCell();
    const auto& langNames = catalog::getLanguageFilterNames();
    int initialLang = static_cast<int>(statePtr->lang);
    if (initialLang < 0 || initialLang >= static_cast<int>(langNames.size())) initialLang = 0;
    langCell->init("app/filter/lang_label"_i18n, langNames, initialLang, [statePtr](int selected) {
        statePtr->lang = static_cast<catalog::LanguageFilter>(selected);
    });
    content->addView(langCell);

    // 4. Favorites option
    brls::BooleanCell* favCell = new brls::BooleanCell();
    favCell->init("app/filter/only_favorites"_i18n, statePtr->onlyFavorites, [statePtr](bool value) {
        statePtr->onlyFavorites = value;
    });
    content->addView(favCell);

    // 5. Year option
    brls::SelectorCell* yearCell = new brls::SelectorCell();
    std::vector<std::string> years = catalog::extractYears(games);
    int initialYear = 0;
    if (!statePtr->year.empty()) {
        for (size_t i = 0; i < years.size(); ++i) {
            if (years[i] == statePtr->year) {
                initialYear = static_cast<int>(i);
                break;
            }
        }
    }
    yearCell->init("app/filter/year_label"_i18n, years, initialYear, [statePtr, years](int selected) {
        if (selected <= 0 || selected >= static_cast<int>(years.size())) {
            statePtr->year.clear();
        } else {
            statePtr->year = years[selected];
        }
    });
    content->addView(yearCell);

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
