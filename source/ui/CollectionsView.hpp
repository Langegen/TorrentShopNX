#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include "../catalog/collections_manager.h"

namespace ui {

class CollectionsView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("collections_view.xml");

    CollectionsView();
    ~CollectionsView() override;
    void onContentAvailable() override;

private:
    void rebuildList();

    std::vector<brls::Label*> countLabels_;
    std::shared_ptr<std::atomic<bool>> alive_flag_;

    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, listBox, "listBox");
};

} // namespace ui
