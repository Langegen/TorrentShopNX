#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include "CollectionCard.hpp"
#include "../catalog/collections_manager.h"

namespace ui {

class CollectionsView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("collections_view.xml");

    CollectionsView();
    ~CollectionsView() override;
    void onContentAvailable() override;

private:
    void rebuildGrid();

    std::shared_ptr<std::atomic<bool>> alive_flag_;
    std::unordered_map<std::string, CollectionCard*> collection_cards_;
    std::vector<std::vector<CollectionCard*>> grid_;

    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, listBox, "listBox");
};

} // namespace ui
