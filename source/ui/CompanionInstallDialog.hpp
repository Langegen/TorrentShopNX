#pragma once

#include "../catalog/modcd_index.hpp"
#include "../utils/switch_utils.h"

#include <functional>
#include <string>

namespace ui {

// Displays a modal progress dialog and installs a companion homebrew app.
void showCompanionInstallDialog(const std::string& repo,
                                const std::string& appName,
                                const std::string& defaultFileName,
                                const std::string& destRelativePath,
                                std::function<void(bool success)> onComplete = nullptr);

// Shows a dialog for ModCD:
// - If ModCD is not installed: explains mods are available and offers to install ModCD.
// - If ModCD is installed: shows available mods and how to launch ModCD via hbmenu.
void showModCdOfferDialog(uint64_t appId, const modcd::TitleMods& mods, std::function<void()> onDone = nullptr);

// Shows a dialog for ModKitNX when an SDK mismatch is detected:
// - If ModKitNX is not installed: offers to install ModKitNX to fix the SDK incompatibility.
// - If ModKitNX is installed: reminds the user to launch ModKitNX and configure a compatible SDK.
void showModKitOfferDialog(const util::SdkVersion& game_sdk,
                           const util::SdkVersion& console_sdk,
                           std::function<void()> onDone = nullptr);

} // namespace ui
