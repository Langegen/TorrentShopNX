#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include "../catalog/retro_emulator_manager.h"

namespace ui {

class RetroEmulatorsView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("retro_emulators_view.xml");

    RetroEmulatorsView();
    ~RetroEmulatorsView() override;
    void onContentAvailable() override;

    void rebuildList();
    void refreshManifestOnline();

private:
    std::shared_ptr<std::atomic<bool>> alive_flag_;

    BRLS_BIND(brls::Label, titleLabel, "titleLabel");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, listBox, "listBox");
};

} // namespace ui
