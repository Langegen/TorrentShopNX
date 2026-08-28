#pragma once

#include <borealis.hpp>
#include <vector>
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

    brls::Box* content_container_ = nullptr;
    std::shared_ptr<bool> imageToken_;
};

} // namespace ui
