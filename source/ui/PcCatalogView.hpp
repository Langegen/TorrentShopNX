#pragma once

#include <borealis.hpp>
#include <vector>
#include <string>
#include <memory>
#include "../GameData.hpp"
#include "../catalog/filter_manager.hpp"

namespace ui {

class PcCatalogView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("catalog_view.xml");

    PcCatalogView(const std::string& subforum_id = "",
                  const std::string& category_title = "",
                  const catalog::FilterSortState& initialFilter = {},
                  const std::string& filterBadgeLabel = "");
    ~PcCatalogView() override;

    void onContentAvailable() override;
    void willAppear(bool resetState) override;
    void willDisappear(bool resetState) override;
    void onResume() override;

    void filterCatalog();
    void resetFilters();
    void jumpToNextLetter(bool forward);

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Box, emptyHolder, "emptyHolder");
    class EmptyState* emptyState_ = nullptr;
    BRLS_BIND(brls::Label, headerTitle, "headerTitle");
    BRLS_BIND(brls::Label, statsHint, "statsHint");

    int focusedRow_ = 0;
    int focusedCol_ = 0;
    std::vector<const Game*> filteredGames_;

private:
    void updateHeaderTitle();

    std::string subforum_id_;
    std::string category_title_;
    std::vector<const Game*> allGames_;
    catalog::FilterSortState filterState_;
    std::string filterBadgeLabel_;
};

} // namespace ui
