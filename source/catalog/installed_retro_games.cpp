#include "installed_retro_games.h"

#include "../utils/app_paths.h"
#include "../utils/log.h"

#include <borealis/extern/nlohmann/json.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace catalog {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Folders a game manifest may name but that are never removed.
bool isProtectedDir(const std::string& path) {
    std::string d = lowerAscii(path);
    while (!d.empty() && d.back() == '/') d.pop_back();
    static const char* keep[] = {
        "sdmc:", "sdmc:/switch", "sdmc:/tico", "sdmc:/tico/roms", "sdmc:/switch/vita3k",
        "sdmc:/switch/vita3k/install", "sdmc:/switch/cemu", "sdmc:/switch/cemu/games",
        ".", "./switch", "./tico", "./tico/roms", "./switch/vita3k", "./switch/vita3k/install",
        "./switch/cemu", "./switch/cemu/games"
    };
    for (const char* k : keep) {
        if (d == k) return true;
    }
    // A console folder itself (tico/roms/<slug>) is shared by all its games.
    for (const char* rootDir : {"sdmc:/tico/roms/", "./tico/roms/"}) {
        std::string root = rootDir;
        if (d.rfind(root, 0) == 0 && d.find('/', root.size()) == std::string::npos) return true;
    }
    return false;
}

std::string dirPath() {
    return std::string(TSNX_DATA_DIR) + "/retro_installed";
}

} // namespace

InstalledRetroGames& InstalledRetroGames::instance() {
    static InstalledRetroGames inst;
    return inst;
}

InstalledRetroGames::InstalledRetroGames() {
    loadKeys();
}

std::string InstalledRetroGames::keyFor(const std::string& topic_id, const std::string& title) {
    std::string k;
    for (char c : topic_id) {
        if (std::isalnum(static_cast<unsigned char>(c))) k += c;
    }
    if (!k.empty()) return k;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (k.size() >= 48) break;
    }
    return k.empty() ? "game" : "t_" + k;
}

std::string InstalledRetroGames::manifestPath(const std::string& key) const {
    return dirPath() + "/" + key + ".json";
}

void InstalledRetroGames::loadKeys() {
    std::lock_guard<std::mutex> lk(mutex_);
    keys_.clear();
    std::error_code ec;
    if (!fs::is_directory(dirPath(), ec)) return;
    for (const auto& e : fs::directory_iterator(dirPath(), ec)) {
        if (e.path().extension() == ".json") keys_.insert(e.path().stem().string());
    }
}

bool InstalledRetroGames::save(const InstalledRetroGame& game) {
    nlohmann::json j;
    j["topic_id"] = game.topic_id;
    j["title"] = game.title;
    j["console"] = game.console_id;
    j["launch"] = game.launch_path;
    j["installed_at"] = static_cast<long long>(game.installed_at);
    j["paths"] = game.paths;

    std::string path = manifestPath(game.key);
    tsnx_ensure_parent_dirs(path.c_str());
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        if (!out) return false;
    }
    std::error_code ec;
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    keys_.insert(game.key);
    return true;
}

bool InstalledRetroGames::load(const std::string& key, InstalledRetroGame& out) const {
    std::ifstream in(manifestPath(key), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    out = InstalledRetroGame();
    out.key = key;
    out.topic_id = j.value("topic_id", "");
    out.title = j.value("title", "");
    out.console_id = j.value("console", "");
    out.launch_path = j.value("launch", "");
    out.installed_at = static_cast<std::time_t>(j.value("installed_at", 0LL));
    if (j.contains("paths") && j["paths"].is_array()) {
        for (const auto& p : j["paths"]) {
            if (p.is_string()) out.paths.push_back(p.get<std::string>());
        }
    }
    return true;
}

std::vector<InstalledRetroGame> InstalledRetroGames::list() const {
    std::vector<InstalledRetroGame> out;
    std::set<std::string> keys;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        keys = keys_;
    }
    for (const auto& k : keys) {
        InstalledRetroGame g;
        if (load(k, g)) out.push_back(std::move(g));
    }
    return out;
}

bool InstalledRetroGames::isInstalled(const std::string& topic_id) const {
    if (topic_id.empty()) return false;
    std::string key = keyFor(topic_id, "");
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!keys_.count(key)) return false;
    }
    InstalledRetroGame g;
    if (!load(key, g) || g.paths.empty()) return false;
    std::error_code ec;
    return fs::exists(g.launch_path.empty() ? g.paths.front() : g.launch_path, ec);
}

bool InstalledRetroGames::uninstall(const std::string& key, std::string& out_error) {
    InstalledRetroGame g;
    if (!load(key, g)) {
        out_error = "not installed";
        return false;
    }
    std::error_code ec;
    for (const auto& p : g.paths) {
        if (isProtectedDir(p)) continue;
        if (fs::is_directory(p, ec)) fs::remove_all(p, ec);
        else fs::remove(p, ec);
        if (ec) util::logLine("retro: could not remove " + p + ": " + ec.message());
        ec.clear();
    }
    fs::remove(manifestPath(key), ec);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        keys_.erase(key);
    }
    util::logLine("retro: removed " + g.title + " (" + std::to_string(g.paths.size()) + " path(s))");
    return true;
}

} // namespace catalog
