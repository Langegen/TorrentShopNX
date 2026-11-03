#pragma once

#include <functional>
#include <vector>

#include "../GameData.hpp"

namespace ui {

// Shows the releases (best first, marked with a star) with their
// re-registration date, size and seeders; calls onPick with the chosen one
// after the list has closed. Nothing happens when the list is dismissed.
// `appId`: the game the releases are of; its version is shown (a multi-game
// release has one per game). 0 = the release-wide version.
void showReleaseChooser(const std::vector<Game>& releases, std::function<void(const Game&)> onPick,
                        uint64_t appId = 0);

} // namespace ui
