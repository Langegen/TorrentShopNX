#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>
#include "CollectionCard.hpp"
#include "../catalog/retro_catalog_manager.h"

namespace ui {

class RetroConsolesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("retro_consoles_view.xml");

    RetroConsolesView();
    ~RetroConsolesView() override;
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;

private:
    void rebuildGrid();
    void showUpdateDialog();
    void refreshGameCounts();

    std::shared_ptr<std::atomic<bool>> alive_flag_;
    std::unordered_map<std::string, CollectionCard*> console_cards_;
    std::vector<std::vector<CollectionCard*>> grid_;

    BRLS_BIND(brls::Label, titleLabel, "titleLabel");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, listBox, "listBox");
};

} // namespace ui
