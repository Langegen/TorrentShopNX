#pragma once

#include <borealis.hpp>
#include <vector>
#include <deque>
#include <string>
#include <memory>
#include "../../GameData.hpp"
#include "../../download/download_manager.h"

namespace ui {

class DashboardSummaryView : public brls::Box {
public:
    DashboardSummaryView();
    ~DashboardSummaryView() override;

    void setFocusedIndex(int index);
    void updateDownloads(const std::vector<download::DownloadItem>& items);
    void setCatalogSample(const std::vector<Game>& games);
    void setRemoteInfo(const std::string& ip_str, int port);
    void setLibraryStats(int installed_count, int updates_count);
    void setSettingsStats(const std::string& engine_mode, uint64_t cache_size_bytes, uint64_t leftover_size_bytes);

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    void rebuildContent();
    void buildCatalogSection();
    void buildRemoteAddSection();
    void buildLibrarySection();
    void buildDownloadsSection();
    void buildToolsSection();

    int active_index_ = 0;
    std::vector<Game> catalog_sample_;
    std::vector<download::DownloadItem> cached_downloads_;
    std::string local_ip_ = "192.168.1.100";
    int remote_port_ = 8080;

    int installed_count_ = 0;
    int updates_count_ = 0;

    std::string engine_mode_ = "Custom Engine";
    uint64_t cache_size_bytes_ = 0;
    uint64_t leftover_size_bytes_ = 0;

    brls::Box* content_container_ = nullptr;
    std::shared_ptr<bool> imageToken_;
    std::deque<float> speed_history_;
};

} // namespace ui
