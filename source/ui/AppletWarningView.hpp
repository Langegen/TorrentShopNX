#pragma once

#include <borealis.hpp>

namespace ui {

class AppletWarningView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("applet_warning_view.xml");

    AppletWarningView();
    void onContentAvailable() override;

private:
    BRLS_BIND(brls::Button, btnExit, "btnExit");
};

} // namespace ui
