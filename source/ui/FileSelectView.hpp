#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
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
    std::vector<torrent::TorrentFileInfo> files_;
    std::vector<bool> selected_;
    std::vector<brls::Label*> checkboxLabels_;
    std::shared_ptr<std::atomic<bool>> alive_flag_;

    BRLS_BIND(brls::Label,          title,          "title");
    BRLS_BIND(brls::Label,          subtitle,       "subtitle");
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

    // Rebuild the visible list from files_ / selected_
    void rebuildFileList();
    void updateRowSelectionState(size_t idx);
    void calculateUncompressedSizes();
};

} // namespace ui
