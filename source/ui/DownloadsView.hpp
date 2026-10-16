#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include "../GameData.hpp"
#include "../download/download_manager.h"

namespace ui {

class DownloadCell : public brls::RecyclerCell {
public:
    DownloadCell();
    ~DownloadCell();
    std::shared_ptr<bool> imageToken;
    std::string loadedCoverUrl;
    bool focusSubscribed = false;
    static DownloadCell* create();

    void clearRegisteredActions() {
        while (!this->getActions().empty()) {
            this->unregisterAction(this->getActions().front()->getIdentifier());
        }
    }

    BRLS_BIND(brls::Image, cover, "cover");
    BRLS_BIND(brls::Label, title, "title");
    BRLS_BIND(brls::Label, countBadge, "countBadge");
    BRLS_BIND(brls::Box, progressBar, "progressBar");
    BRLS_BIND(brls::Box, progressBarBg, "progressBarBg");
    BRLS_BIND(brls::Label, progressText, "progressText");
    BRLS_BIND(brls::Box, installBar, "installBar");
    BRLS_BIND(brls::Box, installBarBg, "installBarBg");
    BRLS_BIND(brls::Label, installText, "installText");
    BRLS_BIND(brls::Label, statsText, "statsText");
    BRLS_BIND(brls::Label, elapsedText, "elapsedText");
    BRLS_BIND(brls::Label, peersText, "peersText");
    BRLS_BIND(brls::Label, expandHint, "expandHint");
    BRLS_BIND(brls::Label, statusText, "statusText");
    BRLS_BIND(brls::Box, installProgressRow, "installProgressRow");
    BRLS_BIND(brls::Box, sdkWarningBox, "sdkWarningBox");
    BRLS_BIND(brls::Label, sdkConsoleLabel, "sdkConsoleLabel");
    BRLS_BIND(brls::Label, sdkGameLabel, "sdkGameLabel");
    BRLS_BIND(brls::Box, componentsBox, "componentsBox");
};

struct DownloadGroup {
    std::string group_id;
    std::string title;
    std::string cover_url;
    std::vector<size_t> item_indices;
    bool is_expanded = false;
    bool has_manual_toggle = false;

    unsigned long long dl_written = 0;
    unsigned long long dl_total = 0;
    unsigned long long inst_written = 0;
    unsigned long long inst_total = 0;
    float dl_speed_kbps = 0.0f;
    float inst_speed_kbps = 0.0f;
    std::chrono::steady_clock::time_point start_time{};
    int seeds = 0;
    int peers = 0;
    int known_peers = 0;
    int dht = 0;

    int completed_count = 0;
    int total_count = 0;
    int failed_item_idx = -1;
    int active_item_idx = -1;
    download::DownloadState state = download::DownloadState::Queued;
    std::string failed_name;
    std::string failed_error;
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

    void buildGroups(const std::vector<download::DownloadItem>& queue);
    void updateCell(DownloadCell* cell, const DownloadGroup& group, const std::vector<download::DownloadItem>& queue, int row);
    std::vector<DownloadGroup> groups_;
    size_t lastRows_ = 0;
    int focusedRow_ = 0;

private:
    void toggleBacklight();
    void registerProgressCallback();
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    class DownloadsDataSource : public brls::RecyclerDataSource {
    public:
        DownloadsDataSource(DownloadsView* parent) : parent_(parent) {}
        
        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        void didSelectRowAt(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;

    private:
        DownloadsView* parent_;
    };
};

} // namespace ui
