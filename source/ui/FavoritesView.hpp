#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"

namespace ui {

class FavoritesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("favorites_view.xml");

    FavoritesView();
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

    void filterFavorites();

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");

private:
    std::vector<Game> favoritedGames_;

    class FavoriteRowCell : public brls::RecyclerCell {
    public:
        FavoriteRowCell();
        ~FavoriteRowCell();
        std::shared_ptr<bool> imageToken;
        static FavoriteRowCell* create();
    };

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
