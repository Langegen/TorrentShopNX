#pragma once

#include <borealis.hpp>

class MainMenu : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("main_menu.xml");
    
    MainMenu();
    void onContentAvailable() override;
    void updateDownloadsBadge(int count);

private:
    BRLS_BIND(brls::Box, btnCatalog, "btnCatalog");
    BRLS_BIND(brls::Box, btnRemoteAdd, "btnRemoteAdd");
    BRLS_BIND(brls::Box, btnFavorites, "btnFavorites");
    BRLS_BIND(brls::Box, btnDownloads, "btnDownloads");
    BRLS_BIND(brls::Box, btnSettings, "btnSettings");
    BRLS_BIND(brls::Label, lblDownloads, "lblDownloads");
};
