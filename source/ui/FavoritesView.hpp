#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"
#include "../catalog/filter_manager.hpp"

namespace ui {

class FavoritesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("favorites_view.xml");

    FavoritesView();
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

    void filterFavorites();
    void resetFilters();

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");

private:
    catalog::FilterSortState filterState_;
    std::vector<Game> allFavorites_;
    std::vector<Game> filteredFavorites_;

    class FavoritesDataSource : public brls::RecyclerDataSource {
    public:
        FavoritesDataSource(FavoritesView* parent) : parent_(parent) {}
        
        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 320; }

    private:
        FavoritesView* parent_;
    };
};

} // namespace ui
