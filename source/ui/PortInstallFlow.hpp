#pragma once

#include "../GameData.hpp"

namespace ui {

// "Automatic installation" of a homebrew port: reads the torrent's file list,
// builds the plan (recipe or heuristics), lets the user pick a version, shows
// where the files will go and how much space is needed, then queues the install.
void startPortInstallFlow(const Game& game);

// True if the auto-installer has a manifest for this release.
bool isPortInstalled(const Game& game);

// Asks for confirmation and removes the installed port (files, folders, shortcut).
void confirmPortUninstall(const Game& game);

} // namespace ui
