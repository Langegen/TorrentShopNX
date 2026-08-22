#pragma once

#include <borealis.hpp>
#include <functional>
#include <vector>
#include "../catalog/filter_manager.hpp"
#include "../GameData.hpp"

namespace ui {

class FilterSortDialog {
public:
    static void show(const catalog::FilterSortState& currentState,
                     const std::vector<Game>& games,
                     std::function<void(const catalog::FilterSortState&)> onApply,
                     std::function<void()> onReset);
};

} // namespace ui
