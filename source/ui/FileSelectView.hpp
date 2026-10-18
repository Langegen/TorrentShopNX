#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include <set>
#include <map>
#include <string>
#include <vector>
#include "../GameData.hpp"
#include "../torrent/torrent_manager.h"
#include "../utils/switch_utils.h"

namespace ui {

class FileSelectView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("file_select_view.xml");

    FileSelectView(const Game& game, const std::string& retro_console_id = "");
    ~FileSelectView();
    void onContentAvailable() override;
    static brls::View* create(); // XML support stub

    void toggleAllSelection();
    void updateTotalSize();
    void startDownloadAndGoToDownloads();
    void executeDownloads(const std::vector<int>& selectedIndices, int forcedIndex, const std::string& forcedName);

private:
    Game game_;
    std::string retro_console_id_;
    bool isFolderHierarchyMode() const { return !retro_console_id_.empty() || isHomebrewGame(game_); }
    std::vector<torrent::TorrentFileInfo> files_;
    std::vector<bool> selected_;
    std::vector<brls::Label*> checkboxLabels_;
    std::shared_ptr<std::atomic<bool>> alive_flag_;

    BRLS_BIND(brls::Label,          title,          "title");
    BRLS_BIND(brls::Label,          subtitle,       "subtitle");
    BRLS_BIND(brls::Box,            toggleAllBox,   "toggleAllBox");
    BRLS_BIND(brls::Label,          toggleAllBtnIcon, "toggleAllBtnIcon");
    BRLS_BIND(brls::Label,          toggleAllBtnText, "toggleAllBtnText");
    BRLS_BIND(brls::ScrollingFrame, fileListScroll, "fileListScroll");
    BRLS_BIND(brls::Box,            fileListBox,    "fileListBox");
    BRLS_BIND(brls::Label,          totalSizeText,  "totalSizeText");
    BRLS_BIND(brls::Label,          freeSpaceSdText, "freeSpaceSdText");
    BRLS_BIND(brls::Label,          freeSpaceNandText, "freeSpaceNandText");
    BRLS_BIND(brls::Box,            bottomSummaryBox, "bottomSummaryBox");
    BRLS_BIND(brls::Box,            installLocationBox, "installLocationBox");
    BRLS_BIND(brls::Label,          installLocationLabel, "installLocationLabel");
    BRLS_BIND(brls::Label,          installLocationText, "installLocationText");

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

    void renderOtherFilesTree(const OtherFileTreeNode& node, int depth, brls::View*& lastFocusable, std::map<std::string, brls::View*>& focusMap);

    // Rebuild the visible list from files_ / selected_
    void rebuildFileList();
    void rebuildRetroFolderList();
    void rebuildSwitchFileList();
    void navigateToFolder(const std::string& folder);
    void navigateUp();
    void updateRetroSubtitle();
    void updateRowSelectionState(size_t idx);
    void calculateUncompressedSizes();
    void startProbe();
};

} // namespace ui
