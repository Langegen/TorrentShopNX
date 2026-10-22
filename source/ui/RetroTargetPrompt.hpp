#pragma once

#include <string>
#include <functional>

namespace ui {

/**
 * Prompts the user to choose between Tico and Standalone destination if the
 * active retro ROMs setting is "prompt" (the default).
 * If another mode is configured (e.g. "tico", "retroarch", "downloads", "custom"),
 * onChosen is immediately called with that mode without displaying a dialog.
 * If retro_console_id is empty, onChosen is immediately called with "".
 */
void promptRetroTargetSelection(const std::string& retro_console_id,
                                std::function<void(const std::string& target_mode)> onChosen);

} // namespace ui
