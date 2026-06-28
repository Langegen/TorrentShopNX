#pragma once

#include <borealis.hpp>

class MainMenu : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("main_menu.xml");
    
    MainMenu();
    void onContentAvailable() override;
    void updateDownloadsBadge(int count);

private:
    BRLS_BIND(brls::Button, btnCatalog, "btnCatalog");
    BRLS_BIND(brls::Button, btnFavorites, "btnFavorites");
    BRLS_BIND(brls::Button, btnDownloads, "btnDownloads");
    BRLS_BIND(brls::Button, btnSettings, "btnSettings");
};
