#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"

namespace ui {

class GameDetailView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("game_detail_view.xml");

    GameDetailView(const Game& game);
    ~GameDetailView();
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;
    static brls::View* create(); // XML support stub

    void updateFavoriteButton();

private:
    Game game_;
    std::shared_ptr<bool> imageToken;

    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Box, contentBox, "contentBox");
    BRLS_BIND(brls::Image, cover, "cover");
    BRLS_BIND(brls::Label, title, "title");
    BRLS_BIND(brls::Box, badgesBox, "badgesBox");
    BRLS_BIND(brls::Label, metaDeveloper, "metaDeveloper");
    BRLS_BIND(brls::Label, metaPublisher, "metaPublisher");
    BRLS_BIND(brls::Label, metaYear, "metaYear");
    BRLS_BIND(brls::Label, metaFormat, "metaFormat");
    BRLS_BIND(brls::Label, metaVoice, "metaVoice");
    BRLS_BIND(brls::Label, description, "description");
    BRLS_BIND(brls::Box, screenshotsBox, "screenshotsBox");
    BRLS_BIND(brls::HScrollingFrame, screenshotsScroll, "screenshotsScroll");
    BRLS_BIND(brls::Button, btnDownload, "btnDownload");
    BRLS_BIND(brls::Button, btnFavorite, "btnFavorite");
};

} // namespace ui
