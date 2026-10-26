#include "retro_catalog_manager.h"
#include "game_stats_manager.h"
#include "../utils/app_paths.h"
#include "../utils/log.h"
#include "../net/http_client.h"
#include "../config/config.h"
#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <chrono>

namespace catalog {

RetroCatalogManager& RetroCatalogManager::instance() {
    static RetroCatalogManager inst;
    return inst;
}

RetroCatalogManager::RetroCatalogManager() {
    initConsoles();
}

void RetroCatalogManager::initConsoles() {
    consoles_ = {
        // Nintendo Family
        {
            "nes", "nes_games.json", "NES / Famicom", "Nintendo", "1983",
            "nes", "pNES", "pnes",
            {".nes", ".fds", ".zip", ".7z"},
            nvgRGBA(230, 0, 18, 255),
            "nes", true, "FCEUmm"
        },
        {
            "snes", "snes_games.json", "Super Nintendo", "Nintendo", "1990",
            "snes", "pSNES", "psnes",
            {".smc", ".sfc", ".fig", ".zip", ".7z"},
            nvgRGBA(124, 77, 255, 255),
            "snes", true, "Snes9x"
        },
        {
            "n64", "n64_games.json", "Nintendo 64", "Nintendo", "1996",
            "n64", "Mupen64Plus-Next", "mupen64plus_next",
            {".z64", ".n64", ".v64", ".zip", ".7z"},
            nvgRGBA(0, 160, 75, 255),
            "n64", true, "Mupen64Plus"
        },
        {
            "gbc", "gbc_games.json", "Game Boy / Color", "Nintendo", "1998",
            "gbc", "mGBA", "mgba",
            {".gb", ".gbc", ".zip", ".7z"},
            nvgRGBA(156, 39, 176, 255),
            "gbc", true, "Gambatte"
        },
        {
            "gba", "gba_games.json", "Game Boy Advance", "Nintendo", "2001",
            "gba", "mGBA / pGBA", "mgba",
            {".gba", ".zip", ".7z"},
            nvgRGBA(63, 81, 181, 255),
            "gba", true, "mGBA"
        },
        {
            "nds", "nds_games.json", "Nintendo DS", "Nintendo", "2004",
            "nds", "DraStic DS", "drasticds",
            {".nds", ".zip", ".7z"},
            nvgRGBA(0, 188, 212, 255),
            "nds", true, "DraStic DS"
        },
        {
            "3ds", "3ds_games.json", "Nintendo 3DS", "Nintendo", "2011",
            "3ds", "Dekopon (Citra)", "dekopon",
            {".3ds", ".cia", ".cxi", ".zip"},
            nvgRGBA(244, 67, 54, 255),
            "3ds", true, "Azahar (Citra)"
        },
        {
            "gamecube", "gamecube_games.json", "Nintendo GameCube", "Nintendo", "2001",
            "gamecube", "Dolphin", "dolphin",
            {".iso", ".gcm", ".rvz", ".ciso"},
            nvgRGBA(103, 58, 183, 255),
            "gc", true, "Dolphin"
        },
        {
            "wii", "wii_games.json", "Nintendo Wii", "Nintendo", "2006",
            "wii", "Dolphin", "dolphin",
            {".wbfs", ".iso", ".rvz"},
            nvgRGBA(33, 150, 243, 255),
            "wii", true, "Dolphin"
        },
        {
            "wiiu", "wiiu_games.json", "Nintendo Wii U", "Nintendo", "2012",
            "wiiu", "Cemu", "cemu",
            {".wup", ".rpx", ".zip"},
            nvgRGBA(0, 172, 237, 255),
            "wiiu", false, ""
        },

        // Sony PlayStation Family
        {
            "ps1", "ps1_games.json", "PlayStation 1", "Sony", "1994",
            "psx", "DuckStation", "duckstation",
            {".chd", ".bin", ".cue", ".pbp", ".iso", ".img"},
            nvgRGBA(0, 55, 145, 255),
            "psx", true, "DuckStation"
        },
        {
            "ps2", "ps2_games.json", "PlayStation 2", "Sony", "2000",
            "ps2", "NetherSX2", "nethersx2",
            {".iso", ".chd", ".bin", ".cso"},
            nvgRGBA(0, 36, 100, 255),
            "ps2", false, ""
        },
        {
            "psp", "psp_games.json", "PlayStation Portable", "Sony", "2004",
            "psp", "PPSSPP", "ppsspp",
            {".iso", ".cso", ".pbp"},
            nvgRGBA(30, 136, 229, 255),
            "psp", true, "PPSSPP"
        },
        {
            "psvita", "psvita_games.json", "PlayStation Vita", "Sony", "2011",
            "psvita", "Vita3K", "vita3k",
            {".vpk", ".zip"},
            nvgRGBA(0, 150, 214, 255),
            "psvita", false, ""
        },

        // Sega Ecosystem
        {
            "sega_ms", "sega_ms_games.json", "Master System", "Sega", "1985",
            "mastersystem", "pGEN", "pgen",
            {".sms", ".zip", ".7z"},
            nvgRGBA(233, 30, 99, 255),
            "sms", true, "Genesis Plus GX"
        },
        {
            "sega_gg", "sega_gg_games.json", "Game Gear", "Sega", "1990",
            "gamegear", "pGEN", "pgen",
            {".gg", ".zip", ".7z"},
            nvgRGBA(0, 150, 136, 255),
            "gg", true, "Genesis Plus GX"
        },
        {
            "sega_md", "sega_md_games.json", "Mega Drive / Genesis", "Sega", "1988",
            "megadrive", "pGEN", "pgen",
            {".md", ".bin", ".gen", ".smd", ".zip", ".7z"},
            nvgRGBA(55, 71, 79, 255),
            "megadrive", true, "Genesis Plus GX"
        },
        {
            "sega_cd", "sega_cd_games.json", "Sega CD / Mega CD", "Sega", "1991",
            "segacd", "pGEN", "pgen",
            {".bin", ".cue", ".chd", ".iso"},
            nvgRGBA(76, 175, 80, 255),
            "segacd", true, "Genesis Plus GX"
        },
        {
            "sega_32x", "sega_32x_games.json", "Sega 32X", "Sega", "1994",
            "32x", "Picodrive", "picodrive",
            {".32x", ".bin", ".zip", ".7z"},
            nvgRGBA(255, 152, 0, 255),
            "32x", true, "Picodrive"
        },
        {
            "dreamcast", "dreamcast_games.json", "Sega Dreamcast", "Sega", "1998",
            "dreamcast", "Flycast", "flycast",
            {".cdi", ".gdi", ".chd"},
            nvgRGBA(255, 87, 34, 255),
            "dc", true, "Flycast"
        }
    };
    applyRouteTable();
}

// Where each console's games go and what format its emulator wants.
// Folder names, modules and extensions follow the module.json files of the
// Tico modules (github.com/ticohq/tico-<module>) as of Tico 0.8.0.
void RetroCatalogManager::applyRouteTable() {
    struct RouteDef {
        const char* id;
        RetroRoute route;
        const char* slug;
        const char* module;
        const char* core_label;
        bool disc_based;
        bool supports_m3u;
        bool keep_archives;
        std::vector<std::string> launch_ext;
    };
    static const std::vector<RouteDef> table = {
        {"nes",       RetroRoute::Tico, "nes",           "fceumm",          "FCEUmm",          false, false, true,  {".nes", ".fds", ".unf", ".unif"}},
        {"snes",      RetroRoute::Tico, "snes",          "snes9x",          "Snes9x",          false, false, true,  {".sfc", ".smc", ".fig", ".swc", ".bs", ".st"}},
        {"n64",       RetroRoute::Tico, "n64",           "mupen64plus",     "Mupen64Plus",     false, false, true,  {".z64", ".n64", ".v64"}},
        {"gbc",       RetroRoute::Tico, "gbc",           "gambatte",        "Gambatte",        false, false, true,  {".gb", ".gbc"}},
        {"gba",       RetroRoute::Tico, "gba",           "mgba",            "mGBA",            false, false, true,  {".gba", ".gb", ".gbc", ".sgb"}},
        {"nds",       RetroRoute::Tico, "nds",           "drastic",         "DraStic",         false, false, true,  {".nds"}},
        {"3ds",       RetroRoute::Tico, "3ds",           "dekopon",         "Dekopon",         false, false, false, {".3ds", ".cci", ".cxi", ".app", ".3dsx", ".zcci", ".zcxi", ".z3dsx"}},
        {"gamecube",  RetroRoute::Tico, "gc",            "dolphin",         "Dolphin",         true,  true,  false, {".rvz", ".iso", ".gcm", ".gcz", ".ciso", ".wia", ".wbfs"}},
        {"wii",       RetroRoute::Tico, "wii",           "dolphin",         "Dolphin",         true,  true,  false, {".rvz", ".wbfs", ".iso", ".gcz", ".ciso", ".wia", ".gcm", ".wad"}},
        {"wiiu",      RetroRoute::Cemu, "",              "",                "",                true,  false, false, {".wua", ".wux", ".wud", ".rpx"}},
        {"ps1",       RetroRoute::Tico, "psx",           "duckstation",     "DuckStation",     true,  true,  false, {".chd", ".cue", ".iso", ".pbp", ".ecm", ".bin"}},
        {"ps2",       RetroRoute::Tico, "ps2",           "nethersx2",       "NetherSX2",       true,  false, false, {".chd", ".iso", ".cso", ".zso", ".gz", ".bin", ".mdf", ".img", ".nrg"}},
        {"psp",       RetroRoute::Tico, "psp",           "ppsspp",          "PPSSPP",          true,  true,  false, {".iso", ".cso", ".pbp", ".chd"}},
        {"psvita",    RetroRoute::Vita3K, "",            "",                "",                true,  false, true,  {".vpk", ".pkg"}},
        {"sega_ms",   RetroRoute::Tico, "master-system", "genesis_plus_gx", "Genesis Plus GX", false, false, true,  {".sms", ".bin"}},
        {"sega_gg",   RetroRoute::Tico, "game-gear",     "genesis_plus_gx", "Genesis Plus GX", false, false, true,  {".gg", ".bin"}},
        {"sega_md",   RetroRoute::Tico, "genesis",       "genesis_plus_gx", "Genesis Plus GX", false, false, true,  {".md", ".bin", ".gen", ".smd"}},
        {"sega_cd",   RetroRoute::Tico, "sega-cd",       "genesis_plus_gx", "Genesis Plus GX", true,  true,  false, {".chd", ".cue", ".iso", ".bin"}},
        {"sega_32x",  RetroRoute::Tico, "sega-32x",      "picodrive",       "PicoDrive",       false, false, true,  {".32x"}},
        {"dreamcast", RetroRoute::Tico, "dc",            "flycast",         "Flycast",         true,  true,  false, {".chd", ".gdi", ".cdi", ".cue"}},
    };

    for (auto& c : consoles_) {
        for (const auto& r : table) {
            if (c.id != r.id) continue;
            c.route = r.route;
            c.is_tico_supported = (r.route == RetroRoute::Tico);
            c.tico_slug = r.slug;
            c.tico_module = r.module;
            c.tico_recommended_core = r.core_label;
            c.disc_based = r.disc_based;
            c.supports_m3u = r.supports_m3u;
            c.keep_archives = r.keep_archives;
            c.launch_extensions = r.launch_ext;
            break;
        }
    }
}

const RetroConsoleInfo* RetroCatalogManager::findConsole(const std::string& id) const {
    for (const auto& c : consoles_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

RetroRoute RetroCatalogManager::getRoute(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    return info ? info->route : RetroRoute::Tico;
}

std::string RetroCatalogManager::getTargetDir(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    RetroRoute route = info ? info->route : RetroRoute::Tico;
    if (route == RetroRoute::Vita3K) return TSNX_VITA3K_INSTALL_DIR;
    if (route == RetroRoute::Cemu) return TSNX_CEMU_GAMES_DIR;

    std::string slug = getTicoSlug(console_id);
    auto& cfg = config::ConfigManager::instance();
    if (cfg.isRetroCustomFolder() && !cfg.getRetroCustomPath().empty()) {
        std::string base = cfg.getRetroCustomPath();
        if (base.back() != '/') base += '/';
        return base + slug;
    }
    return std::string(TSNX_TICO_ROMS_DIR) + "/" + slug;
}

std::string RetroCatalogManager::getRunnerLabel(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    if (!info) return "";
    switch (info->route) {
        case RetroRoute::Vita3K: return "Vita3K";
        case RetroRoute::Cemu:   return "Cemu";
        case RetroRoute::Tico:   break;
    }
    std::string core = getTicoCoreName(console_id);
    return core.empty() ? "Tico" : ("Tico • " + core);
}

std::string RetroCatalogManager::getRunnerPackageId(const std::string& console_id) const {
    switch (getRoute(console_id)) {
        case RetroRoute::Vita3K: return "vita3k";
        case RetroRoute::Cemu:   return "cemu";
        case RetroRoute::Tico:   break;
    }
    return "tico";
}

bool RetroCatalogManager::isTicoSupported(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    return info ? info->is_tico_supported : false;
}

std::string RetroCatalogManager::getTicoSlug(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    if (info && !info->tico_slug.empty()) return info->tico_slug;
    return console_id;
}

std::string RetroCatalogManager::getTicoCoreName(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    if (info && !info->tico_recommended_core.empty()) return info->tico_recommended_core;
    if (info && !info->recommended_emulator.empty()) return info->recommended_emulator;
    return "";
}

std::string RetroCatalogManager::getConsoleJsonPath(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    std::string fn = (info ? info->filename : (console_id + "_games.json"));
    return std::string(TSNX_RETRO_DATA_DIR) + "/" + fn;
}

std::string RetroCatalogManager::getConsoleBinPath(const std::string& console_id) const {
    return std::string(TSNX_RETRO_DATA_DIR) + "/" + console_id + "_games.bin";
}

std::string RetroCatalogManager::getConsoleDownloadUrl(const std::string& console_id) const {
    const auto* info = findConsole(console_id);
    std::string fn = (info ? info->filename : (console_id + "_games.json"));
    return "https://raw.githubusercontent.com/Langegen/console-games/main/data/" + fn;
}

bool RetroCatalogManager::isConsoleCatalogCached(const std::string& console_id) const {
    struct stat st;
    if (stat(getConsoleBinPath(console_id).c_str(), &st) == 0 && st.st_size > 16) return true;
    if (stat(getConsoleJsonPath(console_id).c_str(), &st) == 0 && st.st_size > 0) return true;
    return false;
}

int RetroCatalogManager::getCachedGameCount(const std::string& console_id) {
    auto it = cached_counts_.find(console_id);
    if (it != cached_counts_.end()) return it->second;

    std::string binPath = getConsoleBinPath(console_id);
    std::ifstream in(binPath, std::ios::binary);
    if (in.is_open()) {
        char magic[8];
        in.read(magic, 8);
        if (in.gcount() == 8 && std::memcmp(magic, "TSNXBIN2", 8) == 0) {
            uint32_t ver = 0;
            in.read(reinterpret_cast<char*>(&ver), 4);
            uint32_t count = 0;
            in.read(reinterpret_cast<char*>(&count), 4);
            if (in.gcount() == 4) {
                cached_counts_[console_id] = static_cast<int>(count);
                return static_cast<int>(count);
            }
        }
    }

    return -1;
}

int RetroCatalogManager::getTotalCachedGamesCount() {
    int total = 0;
    for (const auto& c : consoles_) {
        int cnt = getCachedGameCount(c.id);
        if (cnt > 0) total += cnt;
    }
    return total;
}

bool RetroCatalogManager::downloadConsoleCatalog(const std::string& console_id,
                                                std::function<void(float, const std::string&)> progress_cb) {
    std::string url = getConsoleDownloadUrl(console_id);
    std::string dest = getConsoleJsonPath(console_id);
    std::string tmpDest = dest + ".tmp";

    std::filesystem::path p(tmpDest);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    util::logLine("RetroCatalog: downloading catalog from " + url + " to " + tmpDest);
    if (progress_cb) progress_cb(0.05f, "Загрузка базы с GitHub...");

    net::HttpClient client;
    client.setTimeout(120);
    auto lastUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    client.setProgressCallback([progress_cb, lastUpdate](int64_t dltotal, int64_t dlnow) {
        if (!progress_cb || dltotal <= 0) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count();
        if (elapsed < 150 && dlnow < dltotal) {
            return; // Throttle to ~6-7 Hz
        }
        *lastUpdate = now;
        float frac = static_cast<float>(dlnow) / static_cast<float>(dltotal);
        progress_cb(0.05f + frac * 0.70f, "Загрузка базы с GitHub...");
    });

    bool ok = client.downloadToFile(url, tmpDest, &g_appExiting, 120);
    if (!ok || g_appExiting.load()) {
        util::logLine("RetroCatalog: failed to download catalog for " + console_id);
        std::error_code ec;
        std::filesystem::remove(tmpDest, ec);
        if (progress_cb) progress_cb(0.0f, "Ошибка загрузки с GitHub");
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(dest, ec);
    std::filesystem::rename(tmpDest, dest, ec);
    if (ec) {
        util::logLine("RetroCatalog: rename failed for " + console_id + ": " + ec.message());
        std::filesystem::remove(tmpDest, ec);
        if (progress_cb) progress_cb(0.0f, "Ошибка сохранения файла базы");
        return false;
    }

    util::logLine("RetroCatalog: downloaded catalog for " + console_id);
    return true;
}

bool RetroCatalogManager::loadConsoleGames(const std::string& console_id,
                                          std::vector<Game>& out_games,
                                          bool allow_download,
                                          std::function<void(float, const std::string&)> progress_cb) {
    out_games.clear();
    std::string binPath = getConsoleBinPath(console_id);
    std::string jsonPath = getConsoleJsonPath(console_id);

    struct stat stBin, stJson;
    bool hasBin = (stat(binPath.c_str(), &stBin) == 0 && stBin.st_size > 16);
    bool hasJson = (stat(jsonPath.c_str(), &stJson) == 0 && stJson.st_size > 0);

    // 1) Fast path: binary cache up-to-date
    if (hasBin && (!hasJson || stBin.st_mtime >= stJson.st_mtime)) {
        if (progress_cb) progress_cb(0.3f, "Загрузка из бинарного кэша...");
        if (loadGamesFromBinaryFile(binPath, out_games) && !out_games.empty()) {
            cached_counts_[console_id] = static_cast<int>(out_games.size());
            if (progress_cb) progress_cb(1.0f, "Готово");
            util::logLine("RetroCatalog: loaded " + std::to_string(out_games.size()) + " games for " + console_id + " from bin");
            return true;
        }
    }

    // 2) Parse JSON if present
    if (!hasJson && allow_download) {
        if (!downloadConsoleCatalog(console_id, progress_cb)) {
            return false;
        }
        hasJson = (stat(jsonPath.c_str(), &stJson) == 0 && stJson.st_size > 0);
    }

    if (hasJson) {
        if (progress_cb) progress_cb(0.80f, "Индексация и кэширование базы...");
        out_games = parseGamesFromFileStream(jsonPath);
        if (!out_games.empty()) {
            if (progress_cb) progress_cb(0.95f, "Сохранение быстрого кэша...");
            saveGamesToBinaryFile(binPath, out_games);
            cached_counts_[console_id] = static_cast<int>(out_games.size());
            if (progress_cb) progress_cb(1.0f, "Готово");
            util::logLine("RetroCatalog: parsed and cached " + std::to_string(out_games.size()) + " games for " + console_id);
            return true;
        }
    }

    return false;
}

bool RetroCatalogManager::refreshConsoleCatalog(const std::string& console_id,
                                                std::vector<Game>& out_games,
                                                std::function<void(float progress, const std::string& status)> progress_cb) {
    std::string url = getConsoleDownloadUrl(console_id);
    std::string finalJson = getConsoleJsonPath(console_id);
    std::string tmpJson = finalJson + ".tmp";
    std::string finalBin = getConsoleBinPath(console_id);

    std::filesystem::path p(tmpJson);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    util::logLine("RetroCatalog: refreshing catalog for " + console_id + " from " + url);
    if (progress_cb) progress_cb(0.05f, "Загрузка обновления с GitHub...");

    net::HttpClient client;
    client.setTimeout(120);
    auto lastUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    client.setProgressCallback([progress_cb, lastUpdate](int64_t dltotal, int64_t dlnow) {
        if (!progress_cb || dltotal <= 0) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count();
        if (elapsed < 150 && dlnow < dltotal) {
            return; // Throttle to ~6-7 Hz
        }
        *lastUpdate = now;
        float frac = static_cast<float>(dlnow) / static_cast<float>(dltotal);
        progress_cb(0.05f + frac * 0.65f, "Загрузка обновления с GitHub...");
    });

    bool ok = client.downloadToFile(url, tmpJson, &g_appExiting, 120);
    if (!ok || g_appExiting.load()) {
        util::logLine("RetroCatalog: failed to download catalog update for " + console_id);
        std::error_code ec;
        std::filesystem::remove(tmpJson, ec);
        if (progress_cb) progress_cb(0.0f, "Ошибка загрузки с GitHub");
        return false;
    }

    if (progress_cb) progress_cb(0.75f, "Разбор и проверка каталога...");
    std::vector<Game> parsedGames = parseGamesFromFileStream(tmpJson);
    if (parsedGames.empty()) {
        util::logLine("RetroCatalog: parsed games empty for " + console_id + " update");
        std::error_code ec;
        std::filesystem::remove(tmpJson, ec);
        if (progress_cb) progress_cb(0.0f, "Ошибка разбора каталога");
        return false;
    }

    // Atomic replace JSON (remove destination first for FAT32 / Horizon OS compatibility)
    std::error_code ec;
    std::filesystem::remove(finalJson, ec);
    std::filesystem::rename(tmpJson, finalJson, ec);

    // Save binary cache
    if (progress_cb) progress_cb(0.90f, "Обновление быстрого кэша...");
    saveGamesToBinaryFile(finalBin, parsedGames);
    cached_counts_[console_id] = static_cast<int>(parsedGames.size());

    out_games = std::move(parsedGames);
    if (progress_cb) progress_cb(1.0f, "Каталог успешно обновлен");
    util::logLine("RetroCatalog: successfully refreshed " + std::to_string(out_games.size()) + " games for " + console_id);
    return true;
}

bool RetroCatalogManager::updateAllConsoleCatalogs(bool only_cached,
                                                  std::function<void(int current, int total, const std::string& console_name)> progress_cb,
                                                  std::atomic<bool>* cancel_flag) {
    std::vector<RetroConsoleInfo> targets;
    for (const auto& c : consoles_) {
        if (!only_cached || isConsoleCatalogCached(c.id)) {
            targets.push_back(c);
        }
    }

    if (targets.empty()) {
        return true;
    }

    int total = static_cast<int>(targets.size());
    int current = 0;
    int successCount = 0;

    for (const auto& c : targets) {
        if (cancel_flag && cancel_flag->load()) {
            util::logLine("RetroCatalog: updateAllConsoleCatalogs cancelled by user");
            break;
        }

        current++;
        if (progress_cb) {
            progress_cb(current, total, c.name);
        }

        std::vector<Game> dummy;
        if (refreshConsoleCatalog(c.id, dummy, nullptr)) {
            successCount++;
        }
    }

    if (successCount > 0) {
        GameStatsManager::instance().updateStatsInBackground();
    }

    return successCount > 0;
}

} // namespace catalog

