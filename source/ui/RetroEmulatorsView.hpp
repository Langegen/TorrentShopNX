#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include "../catalog/retro_emulator_manager.h"
#include "../catalog/retro_readiness.h"

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
    brls::Box* buildPackageRow(const catalog::EmulatorPackage& pkg, bool isLight);
    brls::Box* buildSystemFileRow(const catalog::SystemFileCheck& f, bool isLight);
    brls::Box* buildActionRow(const std::string& text, const std::string& detail, bool isLight);
    // Installs the packages one after another, each with its progress dialog.
    void installSequence(std::vector<std::string> package_ids, size_t index);

    std::shared_ptr<std::atomic<bool>> alive_flag_;
    std::string lastFocusedPkgId_;
    bool showOthers_ = false;

    BRLS_BIND(brls::Label, titleLabel, "titleLabel");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, listBox, "listBox");
};

} // namespace ui
