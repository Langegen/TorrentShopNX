#pragma once

#include <functional>
#include <vector>

#include "../GameData.hpp"

namespace ui {

// Shows the releases (best first, marked with a star) with their
// re-registration date, size and seeders; calls onPick with the chosen one
// after the list has closed. Nothing happens when the list is dismissed.
void showReleaseChooser(const std::vector<Game>& releases, std::function<void(const Game&)> onPick);

} // namespace ui
