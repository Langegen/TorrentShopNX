#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <nanovg.h>
#include "../GameData.hpp"

namespace catalog {

struct RetroConsoleInfo {
    std::string id;
    std::string filename;
    std::string name;
    std::string brand; // "Nintendo", "Sony", "Sega"
    std::string release_year;
    std::string default_rom_subfolder;
    std::string recommended_emulator;
    std::vector<std::string> known_extensions;
    NVGcolor color;
};

class RetroCatalogManager {
public:
    static RetroCatalogManager& instance();

    const std::vector<RetroConsoleInfo>& consoles() const { return consoles_; }
    const RetroConsoleInfo* findConsole(const std::string& id) const;
    const RetroConsoleInfo* getConsole(const std::string& id) const { return findConsole(id); }

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

    std::vector<RetroConsoleInfo> consoles_;
    std::unordered_map<std::string, int> cached_counts_;
};

} // namespace catalog
