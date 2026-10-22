#pragma once

#include <borealis.hpp>

namespace ui {

void downloadAndInstallAppUpdate(const std::string& url, const std::string& version);

class SettingsTab : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("tabs/settings.xml");

    SettingsTab();
    ~SettingsTab() override;
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

private:
    std::shared_ptr<bool> aliveToken_;
    BRLS_BIND(brls::TabFrame, tabFrame, "tabFrame");

    brls::View* buildGeneralTab();
    brls::View* buildUpdatesTab();
    brls::View* buildAppearanceTab();
    brls::View* buildDownloadsTab();
    brls::View* buildRetroTab();
    brls::View* buildStorageTab();
    brls::View* buildAboutTab();
};

} // namespace ui
