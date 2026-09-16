#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include "../GameData.hpp"
#include "../catalog/retro_catalog_manager.h"
#include "../catalog/filter_manager.hpp"

namespace ui {

class RetroGridRowCell : public brls::RecyclerCell {
public:
    RetroGridRowCell();
    ~RetroGridRowCell() override;

    std::shared_ptr<bool> imageToken;
    static RetroGridRowCell* create();
    void prepareForReuse() override;
    brls::View* getDefaultFocus() override;

    static int s_lastFocusedColumn;

    BRLS_BIND(brls::Box, card0, "card0");
    BRLS_BIND(brls::Image, cover0, "cover0");
    BRLS_BIND(brls::Label, lang0, "lang0");
    BRLS_BIND(brls::Label, title0, "title0");
    BRLS_BIND(brls::Label, size0, "size0");
    BRLS_BIND(brls::Label, romBadge0, "romBadge0");

    BRLS_BIND(brls::Box, card1, "card1");
    BRLS_BIND(brls::Image, cover1, "cover1");
    BRLS_BIND(brls::Label, lang1, "lang1");
    BRLS_BIND(brls::Label, title1, "title1");
    BRLS_BIND(brls::Label, size1, "size1");
    BRLS_BIND(brls::Label, romBadge1, "romBadge1");

    BRLS_BIND(brls::Box, card2, "card2");
    BRLS_BIND(brls::Image, cover2, "cover2");
    BRLS_BIND(brls::Label, lang2, "lang2");
    BRLS_BIND(brls::Label, title2, "title2");
    BRLS_BIND(brls::Label, size2, "size2");
    BRLS_BIND(brls::Label, romBadge2, "romBadge2");

    BRLS_BIND(brls::Box, card3, "card3");
    BRLS_BIND(brls::Image, cover3, "cover3");
    BRLS_BIND(brls::Label, lang3, "lang3");
    BRLS_BIND(brls::Label, title3, "title3");
    BRLS_BIND(brls::Label, size3, "size3");
    BRLS_BIND(brls::Label, romBadge3, "romBadge3");

    BRLS_BIND(brls::Box, card4, "card4");
    BRLS_BIND(brls::Image, cover4, "cover4");
    BRLS_BIND(brls::Label, lang4, "lang4");
    BRLS_BIND(brls::Label, title4, "title4");
    BRLS_BIND(brls::Label, size4, "size4");
    BRLS_BIND(brls::Label, romBadge4, "romBadge4");

    BRLS_BIND(brls::Box, card5, "card5");
    BRLS_BIND(brls::Image, cover5, "cover5");
    BRLS_BIND(brls::Label, lang5, "lang5");
    BRLS_BIND(brls::Label, title5, "title5");
    BRLS_BIND(brls::Label, size5, "size5");
    BRLS_BIND(brls::Label, romBadge5, "romBadge5");
};

class RetroListRowCell : public brls::RecyclerCell {
public:
    RetroListRowCell();
    ~RetroListRowCell() override;

    std::shared_ptr<bool> imageToken;
    static RetroListRowCell* create();
    void prepareForReuse() override;
    brls::View* getDefaultFocus() override;

    BRLS_BIND(brls::Image, cover, "cover");
    BRLS_BIND(brls::Label, title, "title");
    BRLS_BIND(brls::Label, meta, "meta");
    BRLS_BIND(brls::Label, lang, "lang");
    BRLS_BIND(brls::Label, size, "size");
    BRLS_BIND(brls::Box, actionBox, "actionBox");
    BRLS_BIND(brls::Label, actionLabel, "actionLabel");
};

class RetroCatalogView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("retro_catalog_view.xml");

    RetroCatalogView(const catalog::RetroConsoleInfo& consoleInfo);
    ~RetroCatalogView() override;

    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

    void filterCatalog();
    void resetFilters();
    void toggleViewMode();
    void refreshCatalog();

    BRLS_BIND(brls::Label, titleLabel, "titleLabel");
    BRLS_BIND(brls::Box, emuBadge, "emuBadge");
    BRLS_BIND(brls::Label, emuBadgeText, "emuBadgeText");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::Box, loadingBox, "loadingBox");
    BRLS_BIND(brls::Label, loadingLabel, "loadingLabel");
    BRLS_BIND(brls::ProgressSpinner, progressSpinner, "progressSpinner");
    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");

private:
    void updateEmulatorBadge();
    void triggerEmulatorAction();
    catalog::RetroConsoleInfo consoleInfo_;
    catalog::FilterSortState filterState_;
    std::vector<Game> allGames_;
    std::vector<Game> filteredRomsets_;
    std::vector<Game> filteredStandalone_;

    struct CatalogSection {
        std::string title;
        bool isRomset = false;
        std::vector<Game>* games = nullptr;
    };
    std::vector<CatalogSection> sections_;

    std::shared_ptr<std::atomic<bool>> alive_flag_;

    bool isListView_ = false;
    size_t focusedSection_ = 0;
    size_t focusedGameIndex_ = 0;

    class RetroDataSource : public brls::RecyclerDataSource {
    public:
        RetroDataSource(RetroCatalogView* parent) : parent_(parent) {}
        int numberOfSections(brls::RecyclerFrame* recycler) override;
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        std::string titleForHeader(brls::RecyclerFrame* recycler, int section) override;
        float heightForHeader(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForHeader(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
    private:
        RetroCatalogView* parent_;
    };
};

} // namespace ui
