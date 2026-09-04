#pragma once

#include <borealis.hpp>
#include <vector>
#include <deque>
#include <string>
#include <memory>
#include <functional>
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
    void setOnDefocusCallback(std::function<void()> cb) { on_defocus_ = std::move(cb); }
    void setGetActiveTileCallback(std::function<brls::View*()> cb) { get_active_tile_ = std::move(cb); }

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

    // Dynamic active download widgets (for in-place metrics updates without rebuilding)
    bool dl_active_mode_ = false;
    std::string dl_active_topic_id_;
    std::string dl_active_title_;
    std::string dl_loaded_cover_url_;

    brls::Image* dl_coverImg_ = nullptr;
    brls::Label* dl_titleLbl_ = nullptr;
    brls::Label* dl_stLbl_ = nullptr;
    brls::Box* dl_barFill_ = nullptr;
    brls::Label* dl_pctLbl_ = nullptr;
    brls::Label* dl_spdLbl_ = nullptr;
    brls::Label* dl_szLbl_ = nullptr;
    brls::Label* dl_peersLbl_ = nullptr;
    brls::Label* dl_etaLbl_ = nullptr;
    brls::Label* dl_qCountLbl_ = nullptr;
    class SpeedSparklineView* dl_sparkline_ = nullptr;

    std::function<void()> on_defocus_;
    std::function<brls::View*()> get_active_tile_;
};

} // namespace ui
