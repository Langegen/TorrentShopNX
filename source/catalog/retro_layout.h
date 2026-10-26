#pragma once

// Where the files of a retro release go on the SD card and what is done with
// them after downloading, so the game shows up in Tico / Vita3K / Cemu with no
// extra steps:
//   - a single launchable file goes straight into the console folder;
//   - a game made of several files (cue/bin, gdi, several discs) goes into its
//     own subfolder and gets an .m3u playlist next to it (Tico lists the
//     playlist and hides the discs);
//   - cartridge consoles keep single-game archives (Tico reads zip/7z/rar),
//     other archives are unpacked;
//   - text and picture files that no emulator reads are not downloaded.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "retro_catalog_manager.h"

namespace catalog {
namespace retro {

// Naming helpers shared with the download manager, so file and folder names
// follow the same rules as the rest of the app (FAT-safe, transliterated).
struct PathNaming {
    std::function<std::string(const std::string&)> component;      // one file or folder name
    std::function<std::string(const std::string&)> release_folder; // release title -> folder name
};

struct TorrentFileRef {
    int index = -1;
    std::string path;   // path inside the torrent
    uint64_t size = 0;
};

struct LayoutPlan {
    std::string base_dir;   // console folder (tico/roms/psx, switch/vita3k/install, ...)
    std::string game_dir;   // subfolder of base_dir for this release; empty when files lie flat
    std::string release_folder; // folder name for this release, used when an archive needs unpacking
    std::vector<int> indices;                    // torrent files to download
    std::map<std::string, std::string> dest_rel; // torrent path -> path relative to base_dir
};

LayoutPlan planLayout(const RetroConsoleInfo& console,
                      const std::string& release_title,
                      const std::vector<TorrentFileRef>& files,
                      const PathNaming& naming);

struct FinalizeCallbacks {
    std::function<void(const std::string& i18n_key)> on_stage;
    std::function<void(const std::string& archive_name, int percent)> on_extract;
};

struct FinalizeResult {
    // Entries the emulator lists at the top of the console folder for this
    // release (.m3u, ROM, kept archive), full paths.
    std::vector<std::string> launch_entries;
    // Files were saved but none of them is a format the emulator starts.
    bool unsupported_format = false;
};

// Runs after every planned file is on the SD card.
FinalizeResult finalizeRelease(const RetroConsoleInfo& console,
                               const LayoutPlan& plan,
                               const PathNaming& naming,
                               const FinalizeCallbacks& callbacks,
                               const std::shared_ptr<std::atomic<bool>>& cancel);

// Saves the release cover where Tico looks for it:
// tico/assets/covers/<slug>/<name>.jpg, 512x512. Existing covers are kept.
bool saveTicoCover(const std::string& tico_slug, const std::string& name, const std::string& cover_url);

// One-time fixes of what older versions wrote: Sega folders renamed to the
// names Tico uses and .png covers converted to .jpg. Safe to call repeatedly.
void migrateLegacyTicoLayout();

// PS Vita / Wii U games saved by older versions under roms/ or downloads/retro/,
// where Vita3K and Cemu do not look.
struct LegacyStandaloneGames {
    int vita = 0;
    int wiiu = 0;
    int total() const { return vita + wiiu; }
};
LegacyStandaloneGames findLegacyStandaloneGames();
// Moves them to switch/vita3k/install and switch/cemu/games. Returns how many moved.
int moveLegacyStandaloneGames();
bool wasLegacyStandaloneOfferAnswered();
void markLegacyStandaloneOfferAnswered();

} // namespace retro
} // namespace catalog
