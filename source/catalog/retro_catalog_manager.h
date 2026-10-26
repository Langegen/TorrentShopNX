#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <nanovg.h>
#include "../GameData.hpp"

namespace catalog {

// Which program runs the games of a console. Tico is used wherever it has a
// module; PS Vita and Wii U go to their own emulators.
enum class RetroRoute {
    Tico,
    Vita3K,
    Cemu
};

struct RetroConsoleInfo {
    std::string id;
    std::string filename;
    std::string name;
    std::string brand; // "Nintendo", "Sony", "Sega"
    std::string release_year;
    std::string default_rom_subfolder; // legacy RetroArch-style folder (sdmc:/roms/<sub>)
    std::string recommended_emulator;
    std::string recommended_emulator_id;
    std::vector<std::string> known_extensions;
    NVGcolor color;
    std::string tico_slug;      // folder under tico/roms and tico/assets/covers
    bool is_tico_supported = true;
    std::string tico_recommended_core;

    // Filled from the route table (see applyRouteTable).
    RetroRoute route = RetroRoute::Tico;
    std::string tico_module;    // Tico module id (sdmc:/tico/modules/<id>/)
    bool disc_based = false;    // games may span several files (cue/bin, gdi, discs)
    bool supports_m3u = false;  // the emulator reads .m3u playlists
    bool keep_archives = false; // the emulator reads zip/7z/rar directly
    // Extensions the emulator starts a game from (archives excluded).
    std::vector<std::string> launch_extensions;
};

class RetroCatalogManager {
public:
    static RetroCatalogManager& instance();

    const std::vector<RetroConsoleInfo>& consoles() const { return consoles_; }
    const RetroConsoleInfo* findConsole(const std::string& id) const;
    const RetroConsoleInfo* getConsole(const std::string& id) const { return findConsole(id); }

    RetroRoute getRoute(const std::string& console_id) const;
    bool isTicoSupported(const std::string& console_id) const;
    std::string getTicoSlug(const std::string& console_id) const;
    std::string getTicoCoreName(const std::string& console_id) const;

    // Folder the games of a console are saved to: tico/roms/<slug>,
    // switch/vita3k/install, switch/cemu/games, or <custom>/<slug> when the
    // user picked a custom folder (Tico reads extra folders as <folder>/<console>/).
    std::string getTargetDir(const std::string& console_id) const;

    // Text describing what runs the console ("Tico • DuckStation", "Vita3K").
    std::string getRunnerLabel(const std::string& console_id) const;
    // Emulator package that must be installed to play (tico, vita3k, cemu).
    std::string getRunnerPackageId(const std::string& console_id) const;

    std::string getConsoleJsonPath(const std::string& console_id) const;
    std::string getConsoleBinPath(const std::string& console_id) const;
    std::string getConsoleDownloadUrl(const std::string& console_id) const;

    bool isConsoleCatalogCached(const std::string& console_id) const;
    int getCachedGameCount(const std::string& console_id);

    bool loadConsoleGames(const std::string& console_id,
                          std::vector<Game>& out_games,
                          bool allow_download = true,
                          std::function<void(float progress, const std::string& status)> progress_cb = nullptr);

    bool downloadConsoleCatalog(const std::string& console_id,
                                std::function<void(float progress, const std::string& status)> progress_cb = nullptr);

    bool refreshConsoleCatalog(const std::string& console_id,
                               std::vector<Game>& out_games,
                               std::function<void(float progress, const std::string& status)> progress_cb = nullptr);

    bool updateAllConsoleCatalogs(bool only_cached,
                                  std::function<void(int current, int total, const std::string& console_name)> progress_cb = nullptr,
                                  std::atomic<bool>* cancel_flag = nullptr);

    int getTotalCachedGamesCount();

private:
    RetroCatalogManager();
    void initConsoles();
    void applyRouteTable();

    std::vector<RetroConsoleInfo> consoles_;
    std::unordered_map<std::string, int> cached_counts_;
};

} // namespace catalog
