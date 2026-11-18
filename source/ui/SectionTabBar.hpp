#pragma once

#include <borealis.hpp>

namespace ui {

enum class SectionTab {
    Homebrew,
    Retro,
    PcGames
};

class SectionTabBar : public brls::Box {
public:
    explicit SectionTabBar(SectionTab activeTab);
    ~SectionTabBar() override = default;

private:
    void buildTabs(SectionTab activeTab);
};

} // namespace ui
