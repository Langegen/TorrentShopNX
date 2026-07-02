#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"

namespace ui {

class GameRowCell : public brls::RecyclerCell {
public:
    GameRowCell();
    ~GameRowCell();
    std::shared_ptr<bool> imageToken;
    static GameRowCell* create();
    void prepareForReuse() override;

    // Bind cards 0 to 5
    BRLS_BIND(brls::Box, card0, "card0");
    BRLS_BIND(brls::Image, cover0, "cover0");
    BRLS_BIND(brls::Label, lang0, "lang0");
    BRLS_BIND(brls::Label, title0, "title0");
    BRLS_BIND(brls::Label, size0, "size0");

    BRLS_BIND(brls::Box, card1, "card1");
    BRLS_BIND(brls::Image, cover1, "cover1");
    BRLS_BIND(brls::Label, lang1, "lang1");
    BRLS_BIND(brls::Label, title1, "title1");
    BRLS_BIND(brls::Label, size1, "size1");

    BRLS_BIND(brls::Box, card2, "card2");
    BRLS_BIND(brls::Image, cover2, "cover2");
    BRLS_BIND(brls::Label, lang2, "lang2");
    BRLS_BIND(brls::Label, title2, "title2");
    BRLS_BIND(brls::Label, size2, "size2");

    BRLS_BIND(brls::Box, card3, "card3");
    BRLS_BIND(brls::Image, cover3, "cover3");
    BRLS_BIND(brls::Label, lang3, "lang3");
    BRLS_BIND(brls::Label, title3, "title3");
    BRLS_BIND(brls::Label, size3, "size3");

    BRLS_BIND(brls::Box, card4, "card4");
    BRLS_BIND(brls::Image, cover4, "cover4");
    BRLS_BIND(brls::Label, lang4, "lang4");
    BRLS_BIND(brls::Label, title4, "title4");
    BRLS_BIND(brls::Label, size4, "size4");

    BRLS_BIND(brls::Box, card5, "card5");
    BRLS_BIND(brls::Image, cover5, "cover5");
    BRLS_BIND(brls::Label, lang5, "lang5");
    BRLS_BIND(brls::Label, title5, "title5");
    BRLS_BIND(brls::Label, size5, "size5");
};

class CatalogView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("catalog_view.xml");
    
    CatalogView();
    void onContentAvailable() override;
    
    void filterCatalog();

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");

private:
    std::string searchQuery_;
    std::string selectedGenre_;
    std::vector<Game> filteredGames_;

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

} // namespace ui
