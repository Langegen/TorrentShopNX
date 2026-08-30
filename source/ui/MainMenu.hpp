#pragma once

#include <borealis.hpp>
#include <vector>
#include "dashboard/DashboardHeader.hpp"
#include "dashboard/DashboardTile.hpp"
#include "dashboard/DashboardSummaryView.hpp"

class MainMenu : public brls::Activity {
public:
    MainMenu();
    ~MainMenu() override;

    brls::View* createContentView() override;
    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;
    void updateDownloadsBadge(int count);
    void refreshDashboardState();

private:
    void setupLayout();
    void onTileFocused(int index);
    void onTileClicked(int index);

    brls::Box* rootContainer_ = nullptr;
    brls::AppletFrame* appletFrame_ = nullptr;
    brls::Box* rootBox_ = nullptr;
    brls::Image* bgImage_ = nullptr;
    ui::DashboardHeader* header_ = nullptr;
    brls::Box* tilesBox_ = nullptr;
    std::vector<ui::DashboardTile*> tiles_;
    ui::DashboardSummaryView* summaryView_ = nullptr;
    brls::RepeatingTimer* refreshTimer_ = nullptr;
    int current_focused_index_ = 0;
};
