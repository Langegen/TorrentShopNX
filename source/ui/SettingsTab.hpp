#pragma once

#include <borealis.hpp>

namespace ui {

class SettingsTab : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("tabs/settings.xml");

    SettingsTab();
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

private:
    BRLS_BIND(brls::BooleanCell, keepAwakeCell, "keepAwakeCell");
    BRLS_BIND(brls::SelectorCell, modeCell, "modeCell");
    BRLS_BIND(brls::DetailCell, remoteUrlCell, "remoteUrlCell");
    BRLS_BIND(brls::DetailCell, catalogUrlCell, "catalogUrlCell");
};

} // namespace ui
