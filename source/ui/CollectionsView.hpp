#pragma once

#include <borealis.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../GameData.hpp"
#include "../catalog/collections_manager.h"

namespace ui {

class CollectionSidebarRow;

struct PreviewGameItem {
    Game game;
    double metacritic = 0.0;
    bool has_metacritic = false;
};

struct CollectionPreviewData {
    bool loaded = false;
    bool failed = false;
    int totalCount = -1; // -1 = unknown/loading
    std::vector<PreviewGameItem> topGames; // up to 5 deduplicated games
};

struct SidebarEntryMeta {
    std::string id;          // "all_catalog", "favorites", "new_release", "top_100", or collection id
    std::string title;
    std::string description;
    bool isMain = false;
    bool isCollection = false;
    catalog::CollectionInfo collectionInfo;
};

class CollectionsView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("collections_view.xml");

    CollectionsView();
    ~CollectionsView() override;

    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;

private:
    void applyPanelTheme();
    void buildEntriesList();
    void rebuildSidebar();
    void rebuildPreviewPanel(bool animateFade);
    void updateHeaderStats();
    void updateNavigationRoutes();

    void selectItem(const std::string& id, bool immediate);
    void openSection(const std::string& id);
    void returnFocusToSidebar();
    void refreshFavoritesPreview(bool rebuildIfSelected);
    void startBackgroundPreload();
    void openQuickSearch();

    const SidebarEntryMeta* findEntry(const std::string& id) const;

    std::shared_ptr<std::atomic<bool>> alive_flag_;
    std::shared_ptr<bool> previewImageToken_;
    int themeSub_ = 0;

    std::string selectedId_ = "all_catalog";
    uint64_t previewDebounceGen_ = 0;
    std::chrono::steady_clock::time_point lastPreviewRebuild_{};

    std::mutex priorityMutex_;
    std::string priorityCollectionId_;

    std::vector<SidebarEntryMeta> entries_;
    std::unordered_map<std::string, CollectionSidebarRow*> sidebarRows_;
    std::vector<CollectionSidebarRow*> sidebarRowOrder_;
    std::unordered_map<std::string, CollectionPreviewData> previewCache_;

    brls::Box* previewRoot_ = nullptr;
    brls::Box* openAllBtn_ = nullptr;
    std::vector<brls::Box*> previewCards_;

    BRLS_BIND(brls::Label, headerTitle, "headerTitle");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::Box, leftPanel, "leftPanel");
    BRLS_BIND(brls::ScrollingFrame, sidebarScroll, "sidebarScroll");
    BRLS_BIND(brls::Box, sidebarList, "sidebarList");
    BRLS_BIND(brls::Box, rightPanel, "rightPanel");
};

} // namespace ui
