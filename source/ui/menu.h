#pragma once
#include "../download/download_manager.h"

namespace ui {

enum class MainMenuChoice {
    Browse,
    Search,
    Favorites,
    Downloads,
    Sources,
    Settings,
    Exit
};

MainMenuChoice showMainMenu(download::DownloadManager* downloads = nullptr);

} // namespace ui
