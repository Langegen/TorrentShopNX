#pragma once

#include <borealis.hpp>
#include <string>

namespace ui {

class DashboardHeader : public brls::Box {
public:
    DashboardHeader();

    void updateStats(int game_count, const std::string& catalog_updated_str,
                     const std::string& sd_str, const std::string& nand_str);

private:
    brls::Label* catalog_info_label_ = nullptr;
    brls::Label* storage_info_label_ = nullptr;
};

} // namespace ui
