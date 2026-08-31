#include "collections_manager.h"

#include "../GameData.hpp"
#include "../net/http_client.h"
#include "../utils/log.h"
#include "../utils/app_paths.h"

#include <borealis.hpp>
#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

extern std::vector<Game> g_games;

namespace catalog {

std::string CollectionInfo::getName() const {
    std::string key = "app/collections/item_" + id + "_name";
    std::string val = brls::getStr(key);
    return (val == key) ? name : val;
}

std::string CollectionInfo::getDescription() const {
    std::string key = "app/collections/item_" + id + "_desc";
    std::string val = brls::getStr(key);
    return (val == key) ? description : val;
}

namespace {

const std::vector<CollectionInfo> kCollections = {
    {"new_release",         "Новые релизы",          "Автоматически обновляемые свежие релизы"},
    {"top_100",             "Топ-100",               "Топ-100 лучших игр всех времён по версии Metacritic"},
    {"action_adventure",    "Экшены и приключения",  "Приключенческие экшены (Action & Adventure)"},
    {"arcade",              "Аркады",                "Классические и современные аркады"},
    {"horror",              "Хорроры",               "Сурвайвал и психологические хорроры"},
    {"metroidvania",        "Метроидвании",          "Метроидвании (Metroidvania)"},
    {"party_multiplayer",   "Вечеринки и кооп",      "Игры для вечеринок и локальный кооператив"},
    {"platformers",         "Платформеры",           "2D и 3D платформеры"},
    {"puzzles",             "Головоломки",           "Головоломки и логические игры"},
    {"roguelike_roguelite", "Рогалики",              "Рогалики, роглайты и колодостроительные игры"},
    {"rpg_jrpg",            "RPG / JRPG",            "Ролевые игры и JRPG"},
    {"shooters",            "Шутеры",                "Шутеры от 1-го/3-го лица и Shmup"},
    {"simulation_cozy",     "Симуляторы и уютные",   "Симуляторы и уютные/фермерские игры"},
    {"strategy_tactics",    "Стратегии и тактика",   "Пошаговая тактика и стратегии"},
    {"visual_novels",       "Визуальные новеллы",    "Визуальные новеллы и сюжетные адвенчуры"},
    {"ports_homebrew",      "Порты и Homebrew",      "Портированные и homebrew игры"},
};

bool readWholeFileLocal(const std::string& path, std::string& out) {
    return readFileFast(path, out);
}

std::string normalizeForMatch(const std::string& s) {
    std::string out;
    bool prevSpace = false;
    for (unsigned char c : s) {
        char ch = static_cast<char>(std::tolower(c));
        bool keep = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                    (static_cast<unsigned char>(ch) >= 0x80);
        if (keep) {
            out.push_back(ch);
            prevSpace = false;
        } else if (!prevSpace && !out.empty()) {
            out.push_back(' ');
            prevSpace = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> words;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) words.push_back(w);
    return words;
}

} // namespace

CollectionsManager& CollectionsManager::instance() {
    static CollectionsManager* inst = new CollectionsManager();
    return *inst;
}

CollectionsManager::CollectionsManager() : collections_(kCollections) {
}

std::string CollectionsManager::cachePathFor(const std::string& id) {
    return std::string(TSNX_CACHE_COLLECTIONS) + "/" + id + ".json";
}

bool CollectionsManager::isCacheFresh(const std::string& path, int max_age_seconds) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    std::time_t now = std::time(nullptr);
    if (now <= 0) return true;
    return (now - st.st_mtime) < max_age_seconds;
}

bool CollectionsManager::loadCollection(const CollectionInfo& info,
                                        std::vector<CollectionEntry>& out_entries,
                                        bool& from_cache) {
    if (info.id == "ports_homebrew") {
        out_entries.clear();
        for (const auto& g : g_games) {
            if (isHomebrewGame(g)) {
                CollectionEntry e;
                e.title = g.title;
                e.title_id = g.title_id;
                out_entries.push_back(std::move(e));
            }
        }
        from_cache = true;
        util::logLine("collections: ports_homebrew loaded " +
                      std::to_string(out_entries.size()) + " entries from catalog");
        return true;
    }

    const std::string cache_path = cachePathFor(info.id);
    const std::string url = std::string(kCollectionsBaseUrl) + info.id + ".json";

    std::string body;
    from_cache = true;

    if (!readWholeFileLocal(cache_path, body) || !isCacheFresh(cache_path)) {
        util::logLine("collections: fetching " + info.id + " from " + url);
        net::HttpClient http;
        http.setTimeout(30);
        auto res = http.httpGet(url);
        if (res.status_code == 200 && !res.body.empty()) {
            writeTextFile(cache_path, res.body);
            body = res.body;
            from_cache = false;
            util::logLine("collections: fetched and cached " + info.id +
                          " (" + std::to_string(body.size()) + " bytes)");
        } else {
            util::logLine("collections: fetch failed for " + info.id +
                          ", status=" + std::to_string(res.status_code));
        }
    }

    if (body.empty()) {
        util::logLine("collections: no data for " + info.id);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(body);
        if (!j.is_array()) {
            util::logLine("collections: " + info.id + " root is not an array");
            return false;
        }
        out_entries.clear();
        out_entries.reserve(j.size());
        for (const auto& item : j) {
            CollectionEntry e;
            e.title = safeGetStr(item, "title");
            e.title_id = safeGetStr(item, "title_id");
            if (item.contains("metacritic") && item["metacritic"].is_number()) {
                e.metacritic = item["metacritic"].get<double>();
                e.has_metacritic = true;
            }
            if (!e.title.empty()) out_entries.push_back(std::move(e));
        }
        util::logLine("collections: " + info.id + " parsed " +
                      std::to_string(out_entries.size()) + " entries");
        return true;
    } catch (const std::exception& e) {
        util::logLine(std::string("collections: parse error for ") + info.id + ": " + e.what());
        return false;
    }
}

const Game* matchCollectionEntry(const std::vector<Game>& games, const CollectionEntry& entry) {
    if (games.empty()) return nullptr;    // 1. Точное совпадение по title_id (16-значный hex)
    std::string tid = entry.title_id;
    std::transform(tid.begin(), tid.end(), tid.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (tid.size() == 16) {
        bool tidHex = true;
        for (char c : tid) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) { tidHex = false; break; }
        }
        if (tidHex) {
            for (const auto& g : games) {
                std::string gid = g.title_id;
                std::transform(gid.begin(), gid.end(), gid.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (gid.size() == 16 && gid == tid) {
                    util::logLine("collections: matched by title_id field: " + g.title);
                    return &g;
                }
            }
            for (const auto& g : games) {
                uint64_t parsed = parseTitleIdFromString(g.title);
                if (parsed != 0) {
                    char buf[17];
                    std::snprintf(buf, sizeof(buf), "%016llx",
                                  static_cast<unsigned long long>(parsed));
                    if (tid == std::string(buf)) {
                        util::logLine("collections: matched by title_id in title: " + g.title);
                        return &g;
                    }
                }
            }
        }
    }

    // 2. По нормализованному названию
    std::string nt = normalizeForMatch(entry.title);
    if (nt.empty()) return nullptr;

    const Game* best = nullptr;
    size_t bestScore = 0;

    for (const auto& g : games) {
        std::string nc = normalizeForMatch(cleanTitle(g.title));
        if (nc.empty()) continue;

        size_t score = 0;
        if (nc == nt) {
            score = 1000 + nc.size();
        } else if (nt.size() >= 6 && nc.find(nt) != std::string::npos) {
            score = 500 + nt.size();
        } else if (nc.size() >= 6 && nt.find(nc) != std::string::npos) {
            score = 300 + nc.size();
        } else {
            const auto tokens = splitWords(nt);
            if (tokens.size() >= 2) {
                size_t matched = 0;
                for (const auto& t : tokens) {
                    if (t.size() >= 3 && nc.find(t) != std::string::npos) ++matched;
                }
                if (matched >= 2 && matched * 2 >= tokens.size()) {
                    score = matched * 100;
                }
            }
        }

        if (score > bestScore) {
            bestScore = score;
            best = &g;
        }
    }

    if (best) {
        util::logLine("collections: matched by title: '" + entry.title + "' -> '" +
                      best->title + "' (score=" + std::to_string(bestScore) + ")");
    } else {
        util::logLine("collections: no match for '" + entry.title + "'");
    }
    return best;
}

CollectionMatchIndex buildMatchIndex(const std::vector<Game>& games) {
    CollectionMatchIndex index;
    index.by_title_id.reserve(games.size() * 2);
    index.by_norm_title.reserve(games.size() * 2);
    index.norm_titles.reserve(games.size());

    for (const auto& g : games) {
        std::string gid = g.title_id;
        std::transform(gid.begin(), gid.end(), gid.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (gid.size() == 16) {
            bool hex = true;
            for (char c : gid) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) { hex = false; break; }
            }
            if (hex) {
                try {
                    uint64_t tid = std::stoull(gid, nullptr, 16);
                    if (index.by_title_id.find(tid) == index.by_title_id.end()) {
                        index.by_title_id.emplace(tid, &g);
                    }
                } catch (...) {}
            }
        }

        uint64_t parsed = parseTitleIdFromString(g.title);
        if (parsed != 0 && index.by_title_id.find(parsed) == index.by_title_id.end()) {
            index.by_title_id.emplace(parsed, &g);
        }

        std::string nc = normalizeForMatch(cleanTitle(g.title));
        if (!nc.empty()) {
            if (index.by_norm_title.find(nc) == index.by_norm_title.end()) {
                index.by_norm_title.emplace(nc, &g);
            }
            index.norm_titles.emplace_back(std::move(nc), &g);
        }
    }

    util::logLine("collections: match index built: title_ids=" +
                  std::to_string(index.by_title_id.size()) +
                  " titles=" + std::to_string(index.norm_titles.size()));
    return index;
}

const Game* matchWithIndex(const CollectionMatchIndex& index, const CollectionEntry& entry) {
    // 1. Точное совпадение по title_id
    std::string tid = entry.title_id;
    std::transform(tid.begin(), tid.end(), tid.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (tid.size() == 16) {
        try {
            uint64_t tidNum = std::stoull(tid, nullptr, 16);
            auto it = index.by_title_id.find(tidNum);
            if (it != index.by_title_id.end()) {
                return it->second;
            }
        } catch (...) {}
    }

    // 2. Быстрое точное совпадение по нормализованному названию O(1)
    std::string nt = normalizeForMatch(entry.title);
    if (nt.empty()) return nullptr;

    auto itNorm = index.by_norm_title.find(nt);
    if (itNorm != index.by_norm_title.end()) {
        return itNorm->second;
    }

    // Также пробуем нормализовать очищенное название
    std::string ntClean = normalizeForMatch(cleanTitle(entry.title));
    if (!ntClean.empty() && ntClean != nt) {
        auto itClean = index.by_norm_title.find(ntClean);
        if (itClean != index.by_norm_title.end()) {
            return itClean->second;
        }
    }

    // 3. Нечёткое совпадение по токенам (только если точное не найдено)
    const Game* best = nullptr;
    size_t bestScore = 0;
    const auto tokens = splitWords(nt);

    for (const auto& [nc, game] : index.norm_titles) {
        size_t score = 0;
        if (nt.size() >= 6 && nc.find(nt) != std::string::npos) {
            score = 500 + nt.size();
        } else if (nc.size() >= 6 && nt.find(nc) != std::string::npos) {
            score = 300 + nc.size();
        } else if (tokens.size() >= 2) {
            size_t matched = 0;
            for (const auto& t : tokens) {
                if (t.size() >= 3 && nc.find(t) != std::string::npos) ++matched;
            }
            if (matched >= 2 && matched * 2 >= tokens.size()) {
                score = matched * 100;
            }
        }

        if (score > bestScore) {
            bestScore = score;
            best = game;
        }
    }

    return best;
}

} // namespace catalog
