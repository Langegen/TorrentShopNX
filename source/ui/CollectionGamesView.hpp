#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include "../GameData.hpp"
#include "../catalog/collections_manager.h"
#include "../catalog/filter_manager.hpp"

namespace ui {

class CollectionGameRowCell : public brls::RecyclerCell {
public:
    CollectionGameRowCell();
    ~CollectionGameRowCell();
    std::shared_ptr<bool> imageToken;
    static CollectionGameRowCell* create();
    void prepareForReuse() override;
    brls::View* getDefaultFocus() override;

    static int s_lastFocusedColumn;

    BRLS_BIND(brls::Box, card0, "card0");
    BRLS_BIND(brls::Image, cover0, "cover0");
    BRLS_BIND(brls::Label, meta0, "meta0");
    BRLS_BIND(brls::Label, lang0, "lang0");
    BRLS_BIND(brls::Label, title0, "title0");
    BRLS_BIND(brls::Label, size0, "size0");

    BRLS_BIND(brls::Box, card1, "card1");
    BRLS_BIND(brls::Image, cover1, "cover1");
    BRLS_BIND(brls::Label, meta1, "meta1");
    BRLS_BIND(brls::Label, lang1, "lang1");
    BRLS_BIND(brls::Label, title1, "title1");
    BRLS_BIND(brls::Label, size1, "size1");

    BRLS_BIND(brls::Box, card2, "card2");
    BRLS_BIND(brls::Image, cover2, "cover2");
    BRLS_BIND(brls::Label, meta2, "meta2");
    BRLS_BIND(brls::Label, lang2, "lang2");
    BRLS_BIND(brls::Label, title2, "title2");
    BRLS_BIND(brls::Label, size2, "size2");

    BRLS_BIND(brls::Box, card3, "card3");
    BRLS_BIND(brls::Image, cover3, "cover3");
    BRLS_BIND(brls::Label, meta3, "meta3");
    BRLS_BIND(brls::Label, lang3, "lang3");
    BRLS_BIND(brls::Label, title3, "title3");
    BRLS_BIND(brls::Label, size3, "size3");

    BRLS_BIND(brls::Box, card4, "card4");
    BRLS_BIND(brls::Image, cover4, "cover4");
    BRLS_BIND(brls::Label, meta4, "meta4");
    BRLS_BIND(brls::Label, lang4, "lang4");
    BRLS_BIND(brls::Label, title4, "title4");
    BRLS_BIND(brls::Label, size4, "size4");

    BRLS_BIND(brls::Box, card5, "card5");
    BRLS_BIND(brls::Image, cover5, "cover5");
    BRLS_BIND(brls::Label, meta5, "meta5");
    BRLS_BIND(brls::Label, lang5, "lang5");
    BRLS_BIND(brls::Label, title5, "title5");
    BRLS_BIND(brls::Label, size5, "size5");
};

class CollectionGamesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("collection_games_view.xml");

    CollectionGamesView(const catalog::CollectionInfo& info);
    ~CollectionGamesView() override;
    void onContentAvailable() override;

private:
    void loadAndShow();
    void rebuildDisplay();
    void ensureAllMatches();
    void resetFilters();

    catalog::CollectionInfo info_;
    catalog::FilterSortState filterState_;
    std::vector<catalog::CollectionEntry> entries_;
    std::vector<Game> matchedGames_;
    std::vector<bool> matchedComputed_;
    catalog::CollectionMatchIndex matchIndex_;
    std::vector<int> displayIdx_;
    bool loading_ = false;
    int coverLogBudget_ = 3;
    std::shared_ptr<std::atomic<bool>> alive_flag_;

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Label, headerTitle, "headerTitle");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::Label, loadingLabel, "loadingLabel");

    class GamesDataSource : public brls::RecyclerDataSource {
    public:
        GamesDataSource(CollectionGamesView* parent) : parent_(parent) {}

        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 320; }

    private:
        CollectionGamesView* parent_;
    };
};

} // namespace ui