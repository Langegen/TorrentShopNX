#pragma once

// System modules on the card (atmosphere/contents/*/exefs.nsp): running or
// not, autostart. A starts / stops a module now, Y turns autostart on / off.

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <vector>

#include "../../catalog/sysmodules.hpp"

namespace ui {

class EmptyState;

class SysmodulesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("sysmodules_view.xml");

    SysmodulesView() : alive_(std::make_shared<std::atomic<bool>>(true)) {}
    ~SysmodulesView() override { alive_->store(false); }
    void onContentAvailable() override;

private:
    void rebuild(const std::string& focusTid);
    // From a row's own action: the rebuild deletes that row, so wait a frame.
    void rebuildLater(const std::string& focusTid);
    brls::Box* makeRow(const homebrew::SysmoduleInfo& m);

    BRLS_BIND(brls::Label, titleLabel, "titleLabel");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::Label, noteLabel, "noteLabel");
    BRLS_BIND(brls::Box, emptyHolder, "emptyHolder");
    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, listBox, "listBox");

    EmptyState* emptyState_ = nullptr;
    std::vector<homebrew::SysmoduleInfo> modules_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

}  // namespace ui
