#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"

#include <atomic>

namespace ui {

struct UpdateItem {
    Game game;
    std::string currentVersion;
    std::string latestVersion;
    bool needsUpdate;
    uint64_t titleId;
    std::string rawName;
};

class UpdateRowCell : public brls::RecyclerCell {
public:
    UpdateRowCell();
    ~UpdateRowCell();
    static UpdateRowCell* create();
    
    BRLS_BIND(brls::Image, cover, "cover");
    BRLS_BIND(brls::Label, title, "title");
    BRLS_BIND(brls::Label, titleId, "titleId");
    BRLS_BIND(brls::Label, currentVersion, "currentVersion");
    BRLS_BIND(brls::Label, latestVersion, "latestVersion");
    BRLS_BIND(brls::Label, statusBadge, "statusBadge");
    BRLS_BIND(brls::Box, statusBox, "statusBox");
    
    std::shared_ptr<bool> imageToken;
};

class UpdatesView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("updates_view.xml");

    UpdatesView();
    ~UpdatesView();
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

    void scanForUpdates();

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Label, statsHint, "statsHint");

private:
    std::vector<UpdateItem> displayItems_;
    std::shared_ptr<bool> cancelToken_;
    std::atomic<bool> isScanning_{false};

    class UpdatesDataSource : public brls::RecyclerDataSource {
    public:
        UpdatesDataSource(UpdatesView* parent) : parent_(parent) {}
        
        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 90; }

    private:
        UpdatesView* parent_;
    };
};

} // namespace ui
