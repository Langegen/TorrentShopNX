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

// Bottom panel of the main menu. Every section uses the same template:
// a header row (title + optional meta) and three equal cards, each with a
// caption, a large value and a sub line.
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
    void setStorageStats(int64_t sd_free, int64_t sd_total, int64_t nand_free, int64_t nand_total);
    void setOnDefocusCallback(std::function<void()> cb) { on_defocus_ = std::move(cb); }
    void setGetActiveTileCallback(std::function<brls::View*()> cb) { get_active_tile_ = std::move(cb); }
    void setOnOpenSectionCallback(std::function<void(int)> cb) { on_open_section_ = std::move(cb); }
    void refreshLocale() { rebuildContent(); }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    void rebuildContent();
    void buildCatalogSection();
    void buildRetroGamesSection();
    void buildLibrarySection();
    void buildDownloadsSection();
    void buildToolsSection();

    void addHeader(const std::string& title, const std::string& meta);
    brls::Box* addCardsRow();
    void routeUpToTile(brls::View* view);
    void wireCard(brls::View* card);
    void updateActiveDownloadWidgets(const download::DownloadItem& item);
    void updateStorageWidgets();
    std::string idleDownloadsSignature() const;

    int active_index_ = 0;
    std::vector<Game> catalog_sample_;
    size_t catalog_total_ = 0;
    std::vector<download::DownloadItem> cached_downloads_;
    std::string local_ip_ = "192.168.1.100";
    int remote_port_ = 8080;

    int installed_count_ = 0;
    int updates_count_ = 0;

    std::string engine_mode_ = "Custom Engine";
    uint64_t cache_size_bytes_ = 0;
    uint64_t leftover_size_bytes_ = 0;

    int64_t sd_free_ = 0, sd_total_ = 0, nand_free_ = 0, nand_total_ = 0;

    brls::Box* content_container_ = nullptr;
    std::shared_ptr<bool> imageToken_;
    std::deque<float> speed_history_;

    // Library section: storage widgets updated in place
    brls::Label* lib_sdVal_ = nullptr;
    brls::Box* lib_sdFill_ = nullptr;
    brls::Label* lib_nandVal_ = nullptr;
    brls::Box* lib_nandFill_ = nullptr;

    // Downloads section, idle state: rebuilt only when this changes
    std::string dl_idle_signature_;

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
    std::function<void(int)> on_open_section_;
};

} // namespace ui
