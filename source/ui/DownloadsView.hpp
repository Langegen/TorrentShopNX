#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"
#include "../download/download_manager.h"

namespace ui {

class DownloadCell : public brls::RecyclerCell {
public:
    DownloadCell();
    ~DownloadCell();
    std::shared_ptr<bool> imageToken;
    static DownloadCell* create();

    void clearRegisteredActions() {
        while (!this->getActions().empty()) {
            this->unregisterAction(this->getActions().front()->getIdentifier());
        }
    }

    BRLS_BIND(brls::Image, cover, "cover");
    BRLS_BIND(brls::Label, title, "title");
    BRLS_BIND(brls::Box, progressBar, "progressBar");
    BRLS_BIND(brls::Box, progressBarBg, "progressBarBg");
    BRLS_BIND(brls::Label, progressText, "progressText");
    BRLS_BIND(brls::Box, installBar, "installBar");
    BRLS_BIND(brls::Box, installBarBg, "installBarBg");
    BRLS_BIND(brls::Label, installText, "installText");
    BRLS_BIND(brls::Label, statsText, "statsText");
    BRLS_BIND(brls::Label, elapsedText, "elapsedText");
    BRLS_BIND(brls::Label, peersText, "peersText");
    BRLS_BIND(brls::Label, statusText, "statusText");
    BRLS_BIND(brls::Box, installProgressRow, "installProgressRow");
};

class DownloadsView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("downloads_view.xml");

    DownloadsView();
    ~DownloadsView() override;
    void onContentAvailable() override;
    void willAppear(bool resetState) override;
    void willDisappear(bool resetState) override;

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Label, emptyLabel, "emptyLabel");
    BRLS_BIND(brls::Label, backlightHint, "backlightHint");

    void updateCell(DownloadCell* cell, const download::DownloadItem& item);
    size_t lastRows_ = 0;
    int focusedRow_ = 0;

private:
    void checkBacklightState();
    void toggleBacklight();

    std::chrono::steady_clock::time_point lastInputTime_;
    std::chrono::steady_clock::time_point backlightToggleTime_;
    brls::RepeatingTimer* backlightTimer_ = nullptr;
    brls::ControllerState prevControllerState_{};
    bool isFirstStateCheck_ = true;

    class DownloadsDataSource : public brls::RecyclerDataSource {
    public:
        DownloadsDataSource(DownloadsView* parent) : parent_(parent) {}
        
        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        void didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 130; }

    private:
        DownloadsView* parent_;
    };
};

} // namespace ui
