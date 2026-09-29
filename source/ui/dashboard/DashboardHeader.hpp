#pragma once

#include <borealis.hpp>
#include <string>

namespace ui {

class DashboardHeader : public brls::Box {
public:
    DashboardHeader();

    void updateStats(int game_count, const std::string& catalog_updated_str,
                     const std::string& sd_str, const std::string& nand_str);

    void refreshTheme();

    void setOnFileManagerClicked(std::function<void()> cb) { onFileManagerClicked_ = cb; }

private:
    std::function<void()> onFileManagerClicked_;
    brls::Label* eqIcon_ = nullptr;
    brls::Label* titleLabel_ = nullptr;
    brls::Box* nxBadge_ = nullptr;
    brls::Label* nxText_ = nullptr;
    brls::Box* rightBox_ = nullptr;
    brls::Label* catalog_info_label_ = nullptr;
    brls::Label* storage_info_label_ = nullptr;
    brls::Label* verLabel_ = nullptr;
};

} // namespace ui
