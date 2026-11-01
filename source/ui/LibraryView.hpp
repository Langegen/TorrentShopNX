#pragma once

#include <borealis.hpp>
#include "../GameData.hpp"

#include <atomic>
#include <ctime>
#include <functional>
#include <unordered_map>
#include <vector>
#include <string>

namespace ui {

enum class GameUpdateStatus {
    UpToDate,
    UpdateAvailable,
    UpdateIgnored,
    NotInstalled,   // record only: base game and update are on a missing SD card or archived
    Unknown
};

struct InstalledComponent {
    enum class Type {
        BaseGame,
        Patch,
        Dlc,
        Mods
    };
    Type type = Type::BaseGame;
    uint64_t titleId = 0;
    uint32_t version = 0;
    uint64_t sizeBytes = 0;
    std::string displayName;
    uint8_t storageId = 0; // NcmStorageId
    std::string extraInfo; // path for mods, etc.
};

struct LibraryItem {
    Game game;
    std::string currentVersion;
    std::string latestVersion;
    GameUpdateStatus status = GameUpdateStatus::Unknown;
    uint64_t titleId = 0;
    std::string rawName;
    bool hasMods = false;
    bool updateIgnored = false;
    std::string modDetails;
    std::string modPath;
    std::string sdkVersion;

    // Component and size info
    uint64_t totalSizeBytes = 0;
    uint64_t appSizeBytes = 0;
    uint64_t patchSizeBytes = 0;
    uint64_t dlcSizeBytes = 0;
    uint64_t modsSizeBytes = 0;
    bool hasPatch = false;
    uint32_t installedPatchVersion = 0;
    int dlcCount = 0;
    std::vector<InstalledComponent> components;

    // Installed version from the game's NACP ("1.0.3"), "" if unknown.
    std::string installedDisplayVersion;
    // Every catalog release of this game, best first (releases[0] == game):
    // the newest version of the plain game on top.
    std::vector<Game> releases;
    // Releases were found by title ID. Only then do their versions decide the
    // update status: a name match may be another region or another game.
    bool matchedByTitleId = false;

    // Homebrew port installed by the auto-installer (its own section)
    bool isPort = false;
    std::string portKey;
    std::string portVariant;
    size_t portFiles = 0;
    std::time_t portInstalledAt = 0;
    std::vector<uint64_t> portForwarders;
};

// Name, NACP version and installed update of each game from the game
// manager's scan cache on the SD card; empty until the manager was opened.
struct CachedInstalledGame {
    std::string name;
    std::string displayVersion;
    uint32_t patchVersion = 0;
};
std::unordered_map<uint64_t, CachedInstalledGame> cachedInstalledGames();

// The game manager's rule: the best catalog release of the game (found by
// title ID) has a newer version than the installed one.
bool catalogHasNewerVersion(uint64_t appId, const std::string& installedName,
                            const std::string& displayVersion, uint32_t patchVersion);

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
    BRLS_BIND(brls::Box, patchBadgeBox, "patchBadgeBox");
    BRLS_BIND(brls::Label, patchBadge, "patchBadge");
    BRLS_BIND(brls::Box, dlcBadgeBox, "dlcBadgeBox");
    BRLS_BIND(brls::Label, dlcBadge, "dlcBadge");
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
    void showGameActionMenu(const LibraryItem& item, int currentSection, int currentRow);
    void uninstallGame(uint64_t titleId, const std::string& displayName, int currentSection, int currentRow, bool deleteMods = false);
    // Ports: Y opens this menu; removal deletes files, folders and shortcuts.
    void showPortActionMenu(const LibraryItem& item, int currentSection, int currentRow);
    void uninstallPort(const LibraryItem& item, int currentSection, int currentRow);
    void showComponentManager(const LibraryItem& item, int currentSection, int currentRow);
    void toggleUpdateIgnored(uint64_t titleId, const std::string& displayName, int currentSection = -1, int currentRow = -1);
    void showModWarningDialog(const LibraryItem& item);
    // Opens the game's release: straight away when there is one, otherwise a
    // list of all releases (best first) to pick from.
    void openRelease(const LibraryItem& item);
    // Queues the missing update of every game in "Updates available".
    void updateAllGames();
    void rebuildSections(int targetSection = -1, int targetRow = -1);
    void updateStatsAndSpace();
    void updateSpaceHint();

    BRLS_BIND(brls::RecyclerFrame, recycler, "recycler");
    BRLS_BIND(brls::Label, statsHint, "statsHint");
    BRLS_BIND(brls::Label, spaceHint, "spaceHint");
    BRLS_BIND(brls::Label, consoleSdkHint, "consoleSdkHint");

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
