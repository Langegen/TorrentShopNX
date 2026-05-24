#include "menu.h"

#include <switch.h>

#include "input.h"
#include <vector>
#include <string>

namespace ui {

MainMenuChoice showMainMenu(download::DownloadManager* downloads) {
    std::vector<std::string> items = {
        "Browse Catalog",
        "Search",
        "Favorites",
        "Downloads",
        "Sources",
        "Settings",
        "Exit"
    };
    int selected = 0;

    while (appletMainLoop()) {
        if (downloads) {
            downloads->trackProgress();
        }
        consoleClear();
        printf("TorrentShopNX\n\n");
        for (size_t i = 0; i < items.size(); ++i) {
            if ((int)i == selected) {
                printf(" > %s\n", items[i].c_str());
            } else {
                printf("   %s\n", items[i].c_str());
            }
        }
        printf("\nA: Select  B: Exit\n");

        u64 kDown = inputDown();
        u64 kNav = kDown | inputRepeat(HidNpadButton_Up | HidNpadButton_Down);
        if (kNav & HidNpadButton_Down) {
            selected = (selected + 1) % items.size();
        }
        if (kNav & HidNpadButton_Up) {
            selected = (selected - 1 + items.size()) % items.size();
        }
        if (kDown & HidNpadButton_A) {
            return static_cast<MainMenuChoice>(selected);
        }
        if (kDown & HidNpadButton_B) {
            return MainMenuChoice::Exit;
        }
        consoleUpdate(NULL);
    }
    return MainMenuChoice::Exit;
}

} // namespace ui
