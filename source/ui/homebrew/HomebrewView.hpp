#pragma once

// Homebrew store: the "Homebrew" tab of the "Homebrew and retro" section.
// Grid of 6 cards per row; ZL/ZR (or a tap) switch the category, X searches,
// Y changes the sort, minus refreshes the catalog, LB/RB go to the retro tab.

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../../catalog/homebrew_installer.hpp"
#include "../../catalog/homebrew_view_model.hpp"

namespace ui {

class EmptyState;

class HomebrewRowCell : public brls::RecyclerCell {
public:
    static constexpr int kColumns = 6;
    HomebrewRowCell();
    static HomebrewRowCell* create();
    void prepareForReuse() override;
    brls::View* getDefaultFocus() override;
    // The recycler gives every cell a bottom separator; a grid of cards has none.
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override;

    struct Card {
        brls::Box* box = nullptr;
        brls::Image* icon = nullptr;
        brls::Label* badge = nullptr;
        brls::Label* title = nullptr;
        brls::Label* sub = nullptr;
    };
    Card cards[kColumns];
    std::shared_ptr<bool> imageToken;
    static int s_lastFocusedColumn;
};

class HomebrewView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("homebrew_view.xml");

    HomebrewView();
    ~HomebrewView() override;
    void onContentAvailable() override;
    void willAppear(bool resetState) override;
    void onResume() override;  // back from a package card (popActivity calls this, not willAppear)

    // Rebuilds the list from the catalog (category, query, sort kept).
    void rebuild(bool keepFocus);

private:
    class DataSource;
    friend class DataSource;

    void buildChips();
    void updateChips();
    void updateHeader();
    void setCategory(homebrew::Category c);
    void showEmpty();
    void loadCatalog(bool force);
    // "+": update everything (or, on the sysmodules tab, open the modules screen).
    void updateStartAction();
    void updateAll();

    BRLS_BIND(brls::Label, titleLabel, "titleLabel");
    BRLS_BIND(brls::Label, otherTabLabel, "otherTabLabel");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::Box, chipsBox, "chipsBox");
    BRLS_BIND(brls::Box, emptyHolder, "emptyHolder");
    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");

    EmptyState* emptyState_ = nullptr;
    std::vector<brls::Label*> chips_;

    std::shared_ptr<const homebrew::PackageList> packages_;
    std::shared_ptr<const homebrew::InstalledMap> installed_;
    std::vector<homebrew::ViewItem> items_;
    std::array<int, homebrew::kCategoryCount> counts_{};

    homebrew::Category category_ = homebrew::Category::All;
    homebrew::SortMode sort_ = homebrew::SortMode::Popular;
    std::string query_;
    std::string focusedName_;  // package under the focus, to come back to it after a rebuild
    uint64_t seenGeneration_ = 0;
    bool loading_ = false;
    bool loadFailed_ = false;
    bool updatingAll_ = false;
    std::string updateAllStatus_;  // "Updating 2 of 5: sys-clk" in the header

    std::shared_ptr<std::atomic<bool>> alive_;
};

}  // namespace ui
