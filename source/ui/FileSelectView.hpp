#pragma once

#include <borealis.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../GameData.hpp"
#include "../torrent/torrent_manager.h"
#include "../utils/switch_utils.h"

namespace ui {

class VectorCheckboxView;
class ToggleSwitchView;
class StorageBarView;

// Shared with the game manager's "Update all".
// True for installable Switch packages (.nsp/.nsz/.xci/.xcz).
bool isSwitchPackageFile(const std::string& filename);
// Title ID a torrent file installs: from its name ([0100...] tag), or the
// game's title_id for an unlabelled base-game package. 0 if unknown.
uint64_t torrentFileTitleId(const std::string& filename, const std::string& gameTitleId);
// Queues one package of `game` for download + install (or raw download without install)
void queueSwitchPackage(const Game& game, const torrent::TorrentFileInfo& file, bool no_install = false);

class FileSelectView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("file_select_view.xml");

    FileSelectView(const Game& game, const std::string& retro_console_id = "", bool no_install = false);
    ~FileSelectView();
    void onContentAvailable() override;
    static brls::View* create(); // XML support stub

    void toggleAllSelection();
    void updateTotalSize();
    void startDownloadAndGoToDownloads();
    void promptAndExecuteDownloads(const std::vector<int>& selectedIndices, int forcedIndex, const std::string& forcedName);
    void executeDownloads(const std::vector<int>& selectedIndices, int forcedIndex, const std::string& forcedName);

private:
    enum class LeftState { Loading, Error, List };

    Game game_;
    std::string retro_console_id_;
    bool noInstall_ = false;
    bool isFolderHierarchyMode() const { return !retro_console_id_.empty() || isHomebrewGame(game_); }
    std::vector<torrent::TorrentFileInfo> files_;
    std::vector<bool> selected_;
    // Per file: is its title already installed? Filled once per probe result
    // (rebuildSwitchFileList runs on spoiler toggle / tree expand / mode toggle).
    std::vector<signed char> installedCache_;
    std::shared_ptr<std::atomic<bool>> alive_flag_;

    // Header
    BRLS_BIND(brls::Label,          title,               "title");
    BRLS_BIND(brls::Label,          headerMeta,          "headerMeta");

    // Left column
    BRLS_BIND(brls::Box,            leftPanel,           "leftPanel");
    BRLS_BIND(brls::Box,            breadcrumbBar,       "breadcrumbBar");
    BRLS_BIND(brls::Label,          breadcrumbIcon,      "breadcrumbIcon");
    BRLS_BIND(brls::Label,          breadcrumbPath,      "breadcrumbPath");
    BRLS_BIND(brls::Label,          breadcrumbCount,     "breadcrumbCount");
    BRLS_BIND(brls::Box,            loadingStateBox,     "loadingStateBox");
    BRLS_BIND(brls::Box,            spinnerSlot,         "spinnerSlot");
    BRLS_BIND(brls::Label,          loadingTitle,        "loadingTitle");
    BRLS_BIND(brls::Label,          loadingStatus,       "loadingStatus");
    BRLS_BIND(brls::Box,            errorStateBox,       "errorStateBox");
    BRLS_BIND(brls::Box,            errorIconSlot,       "errorIconSlot");
    BRLS_BIND(brls::Label,          errorTitle,          "errorTitle");
    BRLS_BIND(brls::Label,          errorMessage,        "errorMessage");
    BRLS_BIND(brls::Button,         btnRetry,            "btnRetry");
    BRLS_BIND(brls::Button,         btnDownloadAll,      "btnDownloadAll");
    BRLS_BIND(brls::ScrollingFrame, fileListScroll,      "fileListScroll");
    BRLS_BIND(brls::Box,            fileListBox,         "fileListBox");

    // Right column
    BRLS_BIND(brls::Box,            rightPanel,          "rightPanel");
    BRLS_BIND(brls::Label,          selectedCaption,     "selectedCaption");
    BRLS_BIND(brls::Label,          selectedCountText,   "selectedCountText");
    BRLS_BIND(brls::Label,          totalSizeText,       "totalSizeText");
    BRLS_BIND(brls::Label,          unpackedSizeText,    "unpackedSizeText");
    BRLS_BIND(brls::Box,            summaryDivider,      "summaryDivider");
    BRLS_BIND(brls::Label,          storageCaption,      "storageCaption");
    BRLS_BIND(brls::Box,            storageChipsRow,     "storageChipsRow");
    BRLS_BIND(brls::Box,            chipAuto,            "chipAuto");
    BRLS_BIND(brls::Label,          chipAutoLabel,       "chipAutoLabel");
    BRLS_BIND(brls::Box,            chipSd,              "chipSd");
    BRLS_BIND(brls::Label,          chipSdLabel,         "chipSdLabel");
    BRLS_BIND(brls::Box,            chipNand,            "chipNand");
    BRLS_BIND(brls::Label,          chipNandLabel,       "chipNandLabel");
    BRLS_BIND(brls::Box,            chipUsb,             "chipUsb");
    BRLS_BIND(brls::Label,          chipUsbLabel,        "chipUsbLabel");
    BRLS_BIND(brls::Box,            noInstallToggleRow,  "noInstallToggleRow");
    BRLS_BIND(brls::Label,          noInstallLabel,      "noInstallLabel");
    BRLS_BIND(brls::Box,            noInstallSwitchSlot, "noInstallSwitchSlot");
    BRLS_BIND(brls::Box,            targetPathBox,       "targetPathBox");
    BRLS_BIND(brls::Label,          targetPathIcon,      "targetPathIcon");
    BRLS_BIND(brls::Label,          targetPathText,      "targetPathText");
    BRLS_BIND(brls::Box,            storageBarSlot,      "storageBarSlot");
    BRLS_BIND(brls::Label,          spaceStatusText,     "spaceStatusText");
    BRLS_BIND(brls::Label,          secondarySpaceText,  "secondarySpaceText");
    BRLS_BIND(brls::Button,         btnDownload,         "btnDownload");
    BRLS_BIND(brls::Button,         btnToggleAll,        "btnToggleAll");

    // Embedded NanoVG / Borealis widgets
    brls::ProgressSpinner* spinner_ = nullptr;
    ToggleSwitchView* noInstallSwitch_ = nullptr;
    StorageBarView* storageBar_ = nullptr;

    // Cached storage stats (populated asynchronously by refreshStorageStats)
    bool sdOk_ = false;
    int64_t sdFree_ = 0;
    int64_t sdTotal_ = 0;
    bool nandOk_ = false;
    int64_t nandFree_ = 0;
    int64_t nandTotal_ = 0;
    bool usbOk_ = false;
    int64_t usbFree_ = 0;
    int64_t usbTotal_ = 0;
    bool usbAvailable_ = false;

    LeftState leftState_ = LeftState::Loading;
    bool installedExpanded_ = false;
    brls::View* lastFocusedListRow_ = nullptr;

    struct FileRowBinding {
        brls::Box* row = nullptr;
        VectorCheckboxView* checkbox = nullptr;
        brls::Label* title = nullptr;
        bool installed = false;
        bool tooLarge = false;
    };
    std::vector<FileRowBinding> fileRowBindings_;

    struct FolderRowBinding {
        brls::Box* row = nullptr;
        VectorCheckboxView* checkbox = nullptr;
        std::vector<size_t> fileIndices;
    };
    std::vector<FolderRowBinding> folderRowBindings_;

    struct FileProbeInfo {
        uint64_t uncompressed_size = 0;
        bool is_estimated = false;
    };
    std::vector<FileProbeInfo> probe_info_;

    std::string currentFolder_;
    std::string baseFolder_;

    std::set<std::string> expandedFolders_;
    std::string focusedKey_;

    struct OtherFileTreeNode {
        std::string name;
        std::string fullPath;
        bool isFolder = false;
        size_t fileIndex = 0;
        uint64_t size = 0;
        size_t totalFiles = 0;
        size_t selectedFiles = 0;
        std::vector<size_t> allFileIndices;
        std::map<std::string, OtherFileTreeNode> subfolders;
        std::vector<OtherFileTreeNode> directFiles;
    };

    void renderOtherFilesTree(const OtherFileTreeNode& node, int depth, std::map<std::string, brls::View*>& focusMap);

    // Rebuild the visible list from files_ / selected_
    void rebuildFileList(bool keepRightFocus = false);
    void rebuildRetroFolderList(bool keepRightFocus = false);
    void rebuildSwitchFileList(bool keepRightFocus = false);
    void navigateToFolder(const std::string& folder);
    void navigateUp();
    void updateBreadcrumb(size_t folderCount, size_t fileCount);
    void updateRowSelectionState(size_t idx);
    void refreshFolderCheckboxes();
    void calculateUncompressedSizes();
    void startProbe();
    void scheduleProbeStatusPoll(std::shared_ptr<std::atomic<bool>> alive,
                                 std::shared_ptr<std::atomic<bool>> running);
    bool probeInFlight_ = false;  // Y must not start a second probe and poller

    void setLeftState(LeftState state);
    void refreshStorageStats();
    void updateSummaryUi();
    void updateStorageChipsUi();
    void updateHeaderMeta();
    void updateCrossColumnRoutes();
    void bindRightPanelLeftRoute(brls::View* target);
    brls::View* getRightPanelEntryView();
    brls::View* getLeftPanelFallbackView();
    void selectStorageLocation(const std::string& loc, brls::View* clickedChip);
    void toggleNoInstallMode();
};

} // namespace ui
