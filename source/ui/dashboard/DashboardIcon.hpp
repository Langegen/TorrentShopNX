#pragma once

#include <borealis.hpp>

namespace ui {

// Small illustrated icons for the main menu panel, drawn with NanoVG in the
// style of the menu tiles: flat colors, dark outline, a light highlight.
// Vector, so they stay sharp at any size and need no image files.
class DashboardIcon : public brls::View {
public:
    enum class Kind {
        Gamepad,     // retro games in the database
        Puzzle,      // emulator modules / platforms ready
        Folder,      // ROM folder
        GameCards,   // installed games
        Update,      // updates available
        SdCard,      // free space
        Download,    // active downloads
        Done,        // all good / finished
        Clock,       // last download (none yet)
        Router,      // network
        Chip,        // download engine
        Database,    // app cache
        Trash,       // unfinished installs to clean
        Star,        // favorites
        Search,      // nothing found
        CloudError,  // could not load
    };

    explicit DashboardIcon(Kind kind, float size = 56.0f);

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    Kind kind_;
};

} // namespace ui
