#pragma once

// Retro games saved by the downloader: one manifest per release in
// TSNX_DATA_DIR/retro_installed/<key>.json with everything the release left on
// the SD card, so a game can be shown as "on SD", and removed with its files.

#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace catalog {

struct InstalledRetroGame {
    std::string key;          // topic id, or a name derived from the title
    std::string topic_id;
    std::string title;
    std::string console_id;
    std::string launch_path;  // what the emulator lists (.m3u, ROM, archive), may be empty
    std::time_t installed_at = 0;
    std::vector<std::string> paths; // files and folders, absolute (sdmc:/...)
};

class InstalledRetroGames {
public:
    static InstalledRetroGames& instance();

    static std::string keyFor(const std::string& topic_id, const std::string& title);

    bool save(const InstalledRetroGame& game);
    bool load(const std::string& key, InstalledRetroGame& out) const;
    std::vector<InstalledRetroGame> list() const;   // reads every manifest

    // Cheap check for catalog cells: a manifest exists (no disk access).
    bool isInstalled(const std::string& topic_id) const;
    // The manifest exists and what the emulator lists is still on the card.
    bool isPresentOnCard(const std::string& topic_id) const;

    // Deletes the game's files and folders and the manifest.
    bool uninstall(const std::string& key, std::string& out_error);

private:
    InstalledRetroGames();
    std::string manifestPath(const std::string& key) const;
    void loadKeys();

    mutable std::mutex mutex_;
    std::set<std::string> keys_;
};

} // namespace catalog
