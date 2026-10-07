#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"
#include "../catalog/filter_manager.hpp"

namespace ui {

class GameRowCell : public brls::RecyclerCell {
public:
    GameRowCell();
    ~GameRowCell();
    std::shared_ptr<bool> imageToken;
    static GameRowCell* create();
    void prepareForReuse() override;
    brls::View* getDefaultFocus() override;

    static int s_lastFocusedColumn;

    // Bind cards 0 to 5
    BRLS_BIND(brls::Box, card0, "card0");
    BRLS_BIND(brls::Image, cover0, "cover0");
    BRLS_BIND(brls::Label, lang0, "lang0");
    BRLS_BIND(brls::Label, stats0, "stats0");
    BRLS_BIND(brls::Label, title0, "title0");
    BRLS_BIND(brls::Label, size0, "size0");
    BRLS_BIND(brls::Box, statsBox0, "statsBox0");
    BRLS_BIND(brls::Label, seeds0, "seeds0");
    BRLS_BIND(brls::Label, leeches0, "leeches0");

    BRLS_BIND(brls::Box, card1, "card1");
    BRLS_BIND(brls::Image, cover1, "cover1");
    BRLS_BIND(brls::Label, lang1, "lang1");
    BRLS_BIND(brls::Label, stats1, "stats1");
    BRLS_BIND(brls::Label, title1, "title1");
    BRLS_BIND(brls::Label, size1, "size1");
    BRLS_BIND(brls::Box, statsBox1, "statsBox1");
    BRLS_BIND(brls::Label, seeds1, "seeds1");
    BRLS_BIND(brls::Label, leeches1, "leeches1");

    BRLS_BIND(brls::Box, card2, "card2");
    BRLS_BIND(brls::Image, cover2, "cover2");
    BRLS_BIND(brls::Label, lang2, "lang2");
    BRLS_BIND(brls::Label, stats2, "stats2");
    BRLS_BIND(brls::Label, title2, "title2");
    BRLS_BIND(brls::Label, size2, "size2");
    BRLS_BIND(brls::Box, statsBox2, "statsBox2");
    BRLS_BIND(brls::Label, seeds2, "seeds2");
    BRLS_BIND(brls::Label, leeches2, "leeches2");

    BRLS_BIND(brls::Box, card3, "card3");
    BRLS_BIND(brls::Image, cover3, "cover3");
    BRLS_BIND(brls::Label, lang3, "lang3");
    BRLS_BIND(brls::Label, stats3, "stats3");
    BRLS_BIND(brls::Label, title3, "title3");
    BRLS_BIND(brls::Label, size3, "size3");
    BRLS_BIND(brls::Box, statsBox3, "statsBox3");
    BRLS_BIND(brls::Label, seeds3, "seeds3");
    BRLS_BIND(brls::Label, leeches3, "leeches3");

    BRLS_BIND(brls::Box, card4, "card4");
    BRLS_BIND(brls::Image, cover4, "cover4");
    BRLS_BIND(brls::Label, lang4, "lang4");
    BRLS_BIND(brls::Label, stats4, "stats4");
    BRLS_BIND(brls::Label, title4, "title4");
    BRLS_BIND(brls::Label, size4, "size4");
    BRLS_BIND(brls::Box, statsBox4, "statsBox4");
    BRLS_BIND(brls::Label, seeds4, "seeds4");
    BRLS_BIND(brls::Label, leeches4, "leeches4");

    BRLS_BIND(brls::Box, card5, "card5");
    BRLS_BIND(brls::Image, cover5, "cover5");
    BRLS_BIND(brls::Label, lang5, "lang5");
    BRLS_BIND(brls::Label, stats5, "stats5");
    BRLS_BIND(brls::Label, title5, "title5");
    BRLS_BIND(brls::Label, size5, "size5");
    BRLS_BIND(brls::Box, statsBox5, "statsBox5");
    BRLS_BIND(brls::Label, seeds5, "seeds5");
    BRLS_BIND(brls::Label, leeches5, "leeches5");
};

class CatalogView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("catalog_view.xml");
    
    CatalogView(const std::string& searchQuery = "");
    void onContentAvailable() override;
    void willAppear(bool resetState) override;
    void willDisappear(bool resetState) override;
    void onResume() override;
    
    void filterCatalog();
    void resetFilters();
    void jumpToNextLetter(bool forward);

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Label, headerTitle, "headerTitle");
    BRLS_BIND(brls::Label, statsHint, "statsHint");

private:
    catalog::FilterSortState filterState_;
    std::vector<Game> filteredGames_;
    int focusedRow_ = 0;
    int focusedCol_ = 0;

    // Inner DataSource class
    class CatalogDataSource : public brls::RecyclerDataSource {
    public:
        CatalogDataSource(CatalogView* parent) : parent_(parent) {}
        
        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 320; }

    private:
        CatalogView* parent_;
    };
};

extern CatalogView* g_activeCatalogView;
// Alive token: set in willAppear, invalidated (*token = false) in willDisappear.
// Background threads capture a copy before brls::sync so they can check
// whether the view is still alive before calling filterCatalog().
extern std::shared_ptr<bool> g_catalogViewAliveToken;

} // namespace ui
