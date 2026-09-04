#pragma once

#include <borealis.hpp>
#include <string>

namespace ui {

class DashboardHeader : public brls::Box {
public:
    DashboardHeader();

    void updateStats(int game_count, const std::string& catalog_updated_str,
                     const std::string& sd_str, const std::string& nand_str);

    void setOnFileManagerClicked(std::function<void()> cb) { onFileManagerClicked_ = cb; }

private:
    std::function<void()> onFileManagerClicked_;
    brls::Box* file_manager_btn_ = nullptr;
    brls::Label* catalog_info_label_ = nullptr;
    brls::Label* storage_info_label_ = nullptr;
};

} // namespace ui
