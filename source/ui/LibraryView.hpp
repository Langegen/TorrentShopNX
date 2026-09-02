#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"

#include <atomic>

namespace ui {

enum class GameUpdateStatus {
    UpToDate,
    UpdateAvailable,
    UpdateIgnored,
    Unknown
};

struct LibraryItem {
    Game game;
    std::string currentVersion;
    std::string latestVersion;
    GameUpdateStatus status;
    uint64_t titleId;
    std::string rawName;
    bool hasMods = false;
    bool updateIgnored = false;
    std::string modDetails;
};

struct LibrarySection {
    std::string title;
    std::vector<LibraryItem> items;
};

class LibraryRowCell : public brls::RecyclerCell {
public:
    LibraryRowCell();
    ~LibraryRowCell();
    static LibraryRowCell* create();
    
    BRLS_BIND(brls::Image, cover, "cover");
    BRLS_BIND(brls::Label, title, "title");
    BRLS_BIND(brls::Label, titleId, "titleId");
    BRLS_BIND(brls::Box, modBadgeBox, "modBadgeBox");
    BRLS_BIND(brls::Label, modBadge, "modBadge");
    BRLS_BIND(brls::Label, currentVersion, "currentVersion");
    BRLS_BIND(brls::Label, latestVersion, "latestVersion");
    BRLS_BIND(brls::Label, statusBadge, "statusBadge");
    BRLS_BIND(brls::Box, statusBox, "statusBox");
    
    std::shared_ptr<bool> imageToken;
};

class LibraryView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("library_view.xml");

    LibraryView();
    ~LibraryView();
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

    void scanForUpdates();
    void uninstallGame(uint64_t titleId, const std::string& displayName);
    void toggleUpdateIgnored(uint64_t titleId, const std::string& displayName);
    void showModWarningDialog(const LibraryItem& item);
    void rebuildSections();
    void updateStatsAndSpace();
    void updateSpaceHint();

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::Label, spaceHint, "spaceHint");

private:
    std::vector<LibraryItem> rawItems_;
    std::vector<LibrarySection> sections_;
    std::shared_ptr<bool> cancelToken_;
    std::atomic<bool> isScanning_{false};

    class LibraryDataSource : public brls::RecyclerDataSource {
    public:
        LibraryDataSource(LibraryView* parent) : parent_(parent) {}
        
        int numberOfSections(brls::RecyclerFrame* recycler) override;
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 76; }
        float heightForHeader(brls::RecyclerFrame* recycler, int section) override;
        std::string titleForHeader(brls::RecyclerFrame* recycler, int section) override;

    private:
        LibraryView* parent_;
    };
};

} // namespace ui
