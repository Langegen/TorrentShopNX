#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include "CollectionCard.hpp"
#include "../catalog/pc_catalog_manager.h"

namespace ui {

class PcCategoriesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("pc_categories_view.xml");

    PcCategoriesView();
    ~PcCategoriesView() override;

    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;

private:
    void rebuildGrid();
    void showUpdateDialog();
    void updateHeader();
    void loadCatalogWithProgress();

    std::shared_ptr<std::atomic<bool>> alive_flag_;
    std::unordered_map<std::string, CollectionCard*> category_cards_;
    std::vector<std::vector<CollectionCard*>> grid_;

    BRLS_BIND(brls::Box, tabsBox, "tabsBox");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, listBox, "listBox");
};

} // namespace ui
