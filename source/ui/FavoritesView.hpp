#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include "../GameData.hpp"
#include "../catalog/filter_manager.hpp"

namespace ui {

class FavoritesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("favorites_view.xml");

    FavoritesView();
    ~FavoritesView() override { if (alive_) *alive_ = false; }
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

    void filterFavorites();
    void resetFilters();

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Box, emptyHolder, "emptyHolder");
    class EmptyState* emptyState_ = nullptr;

private:
    catalog::FilterSortState filterState_;
    std::vector<Game> allFavorites_;
    std::vector<Game> filteredFavorites_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

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
