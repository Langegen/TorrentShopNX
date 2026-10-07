#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <borealis.hpp>
#include "../GameData.hpp"

namespace catalog {

// Structure for items in the Genre filter dropdown
struct GenreFilterItem {
    std::string id;          // canonical ID ("adventure", "action", etc.) or empty for "All"
    std::string displayName; // e.g. "Приключения (2655)"
    int count = 0;
};

// UTF-8 lowercase helper handling both ASCII and Cyrillic (CP1251 / UTF-8)
inline std::string toLowerUtf8(const std::string& s) {
    return util::toLowerUtf8(s);
}

// Helper to trim string
inline std::string trimString(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

// List of all 23 canonical gameplay genres
inline const std::vector<std::string>& getAllCanonicalGenreIds() {
    static const std::vector<std::string> kGenres = {
        "action",
        "adventure",
        "platformer",
        "arcade",
        "puzzle",
        "rpg",
        "simulation",
        "strategy",
        "visual_novel",
        "horror",
        "shooter",
        "party",
        "metroidvania",
        "card_board",
        "roguelike",
        "point_and_click",
        "racing",
        "music",
        "fighting",
        "hidden_objects",
        "stealth",
        "education",
        "sports"
    };
    return kGenres;
}

// Get localized genre display name with fallback
inline std::string getLocalizedGenreName(const std::string& id) {
    if (id.empty()) {
        return "app/filter/all_genres"_i18n;
    }
    std::string key = "app/genres/" + id;
    std::string val = brls::getStr(key);
    if (val != key) {
        return val;
    }

    // Built-in Russian fallback in case translations are not loaded or missing
    static const std::unordered_map<std::string, std::string> kFallbackRu = {
        {"action", "Экшен"},
        {"adventure", "Приключения"},
        {"platformer", "Платформеры"},
        {"arcade", "Аркады"},
        {"puzzle", "Головоломки"},
        {"rpg", "Ролевые (RPG)"},
        {"simulation", "Симуляторы"},
        {"strategy", "Стратегии и тактика"},
        {"visual_novel", "Визуальные новеллы"},
        {"horror", "Хорроры"},
        {"shooter", "Шутеры"},
        {"party", "Вечеринки и мультиплеер"},
        {"fighting", "Файтинги"},
        {"metroidvania", "Метроидвании"},
        {"card_board", "Настольные и карточные"},
        {"roguelike", "Рогалики (Roguelike)"},
        {"point_and_click", "Квесты / Point & Click"},
        {"racing", "Гонки"},
        {"music", "Музыка и ритм"},
        {"sports", "Спорт"},
        {"hidden_objects", "Поиск предметов"},
        {"stealth", "Стелс"},
        {"education", "Обучающие"}
    };

    auto it = kFallbackRu.find(id);
    if (it != kFallbackRu.end()) return it->second;
    return id;
}

// Map a single raw genre tag (cleaned & lowercased) to 0, 1 or more canonical genre IDs
inline std::vector<std::string> mapRawTokenToCanonical(const std::string& raw) {
    std::string token = toLowerUtf8(trimString(raw));
    if (token.empty()) return {};

    static const std::unordered_map<std::string, std::vector<std::string>> kTagMap = {
        // Action & Melee
        {"action", {"action"}},
        {"actions", {"action"}},
        {"actionь", {"action"}},
        {"экшен", {"action"}},
        {"экшн", {"action"}},
        {"action-adventure", {"action", "adventure"}},
        {"action adventure", {"action", "adventure"}},
        {"action adventure > survival", {"action", "adventure"}},
        {"hack and slash", {"action"}},
        {"hack-and-slash", {"action"}},
        {"hack-n-slash", {"action"}},
        {"hack'n'slash", {"action"}},
        {"slasher", {"action"}},
        {"musou", {"action"}},
        {"beat 'em up", {"action"}},
        {"beatemup", {"action"}},
        {"beatmeup", {"action"}},
        {"beat em up", {"action"}},
        {"brawler", {"action"}},

        // Adventure
        {"adventure", {"adventure"}},
        {"adventur", {"adventure"}},
        {"aventure", {"adventure"}},
        {"advenutre", {"adventure"}},
        {"приключение", {"adventure"}},
        {"приключения", {"adventure"}},
        {"quest", {"adventure"}},

        // Point & Click
        {"point & click", {"point_and_click"}},
        {"point and click", {"point_and_click"}},
        {"point-and-click", {"point_and_click"}},
        {"квест", {"point_and_click"}},
        {"квесты", {"point_and_click"}},

        // Platformer
        {"platformer", {"platformer"}},
        {"platforer", {"platformer"}},
        {"platfomer", {"platformer"}},
        {"platfromer", {"platformer"}},
        {"платформер", {"platformer"}},
        {"3d platformer", {"platformer"}},
        {"3d platformerm", {"platformer"}},
        {"platformer action", {"platformer", "action"}},
        {"runner", {"platformer"}},

        // Arcade
        {"arcade", {"arcade"}},
        {"аркада", {"arcade"}},
        {"arkanoid", {"arcade"}},
        {"pinball", {"arcade"}},
        {"clicker", {"arcade"}},

        // Puzzle
        {"puzzle", {"puzzle"}},
        {"puzze", {"puzzle"}},
        {"пазл", {"puzzle"}},
        {"sokoban", {"puzzle"}},

        // Hidden Objects
        {"hidden objects", {"hidden_objects"}},
        {"hidden-objects", {"hidden_objects"}},
        {"поиск предметов", {"hidden_objects"}},

        // Role-Playing / RPG
        {"role-playing", {"rpg"}},
        {"role playing", {"rpg"}},
        {"role-play", {"rpg"}},
        {"role play", {"rpg"}},
        {"rpg", {"rpg"}},
        {"action rpg", {"rpg", "action"}},
        {"jrpg", {"rpg"}},
        {"jrole-playing", {"rpg"}},
        {"ролевая", {"rpg"}},
        {"ролевая игра", {"rpg"}},
        {"dungeon crawler", {"rpg"}},

        // Strategy & Tactics
        {"strategy", {"strategy"}},
        {"stategy", {"strategy"}},
        {"стратегия", {"strategy"}},
        {"стратегии", {"strategy"}},
        {"strategy arcade", {"strategy", "arcade"}},
        {"tactical strategy", {"strategy"}},
        {"real-time strategy", {"strategy"}},
        {"rts", {"strategy"}},
        {"city-building", {"strategy"}},
        {"tactics", {"strategy"}},
        {"tactic", {"strategy"}},
        {"tower defense", {"strategy"}},
        {"turn based", {"strategy"}},
        {"turn-based", {"strategy"}},
        {"пошаговая", {"strategy"}},

        // Simulation
        {"simulator", {"simulation"}},
        {"simulation", {"simulation"}},
        {"sim", {"simulation"}},
        {"симулятор", {"simulation"}},
        {"economical simulator", {"simulation"}},
        {"flight", {"simulation"}},
        {"farm", {"simulation"}},
        {"sandbox", {"simulation"}},

        // Shooter & FPS
        {"shooter", {"shooter"}},
        {"шутер", {"shooter"}},
        {"шутер от первого лица", {"shooter"}},
        {"first-person shooter", {"shooter"}},
        {"first person shooter", {"shooter"}},
        {"first-person", {"shooter"}},
        {"first person", {"shooter"}},
        {"fps", {"shooter"}},
        {"shoot'em up", {"shooter"}},
        {"shootemup", {"shooter"}},
        {"shoot 'em up", {"shooter"}},
        {"bullet hell", {"shooter"}},
        {"run & gun", {"shooter"}},
        {"run'n'gun", {"shooter"}},
        {"run&gun", {"shooter"}},
        {"twin-stick", {"shooter"}},
        {"battle royale", {"shooter", "action"}},

        // Fighting
        {"fighting", {"fighting"}},
        {"файтинг", {"fighting"}},

        // Horror
        {"horror", {"horror"}},
        {"хоррор", {"horror"}},
        {"survival horror", {"horror"}},
        {"survival-horror", {"horror"}},
        {"survival", {"horror"}},

        // Metroidvania
        {"metroidvania", {"metroidvania"}},
        {"metrodivania", {"metroidvania"}},
        {"метроидвания", {"metroidvania"}},

        // Visual Novel & Interactive Fiction
        {"visual novel", {"visual_novel"}},
        {"visual nove", {"visual_novel"}},
        {"viusal novel", {"visual_novel"}},
        {"визуальная новелла", {"visual_novel"}},
        {"визуальные новеллы", {"visual_novel"}},
        {"interactive fiction", {"visual_novel"}},
        {"interactive fiction (if)", {"visual_novel"}},
        {"if", {"visual_novel"}},
        {"interactive movie", {"visual_novel"}},
        {"otome", {"visual_novel"}},
        {"eroge", {"visual_novel"}},
        {"hentai", {"visual_novel"}},
        {"fmv", {"visual_novel"}},

        // Roguelike & Roguelite
        {"roguelike", {"roguelike"}},
        {"roguelite", {"roguelike"}},
        {"rogulite", {"roguelike"}},
        {"rogutelite", {"roguelike"}},
        {"rougelite", {"roguelike"}},
        {"roguelute", {"roguelike"}},
        {"рогалик", {"roguelike"}},

        // Board & Card Games
        {"board game", {"card_board"}},
        {"board games", {"card_board"}},
        {"настольная", {"card_board"}},
        {"настольная игра", {"card_board"}},
        {"настольные игры", {"card_board"}},
        {"tcg", {"card_board"}},
        {"tgc", {"card_board"}},
        {"карточная", {"card_board"}},
        {"карточная пошаговая", {"card_board", "strategy"}},
        {"card game", {"card_board"}},

        // Racing
        {"racing", {"racing"}},
        {"racnig", {"racing"}},
        {"гонки", {"racing"}},
        {"гонка", {"racing"}},
        {"moto", {"racing"}},
        {"bike", {"racing"}},
        {"driving", {"racing"}},

        // Music & Rhythm
        {"music", {"music"}},
        {"musical", {"music"}},
        {"музыка", {"music"}},
        {"rhythm", {"music"}},
        {"ритм-игра", {"music"}},
        {"karaoke", {"music"}},

        // Party & Multiplayer
        {"party", {"party"}},
        {"вечеринка", {"party"}},
        {"multiplayer", {"party"}},
        {"online multiplayer", {"party"}},
        {"кооп", {"party"}},
        {"coop", {"party"}},

        // Sports
        {"sport", {"sports"}},
        {"sports", {"sports"}},
        {"спорт", {"sports"}},
        {"fitness", {"sports"}},
        {"training", {"sports"}},

        // Stealth
        {"stealth", {"stealth", "action"}},
        {"stealh", {"stealth", "action"}},
        {"stealth action", {"stealth", "action"}},
        {"стелс", {"stealth", "action"}},

        // Education
        {"education", {"education"}},
        {"learning", {"education"}},
        {"study", {"education"}},
        {"обучение", {"education"}}
    };

    auto it = kTagMap.find(token);
    if (it != kTagMap.end()) {
        return it->second;
    }
    return {};
}

// Extract unique canonical genre IDs from game's raw genre string
inline std::vector<std::string> extractGameCanonicalGenres(const std::string& rawGenre) {
    if (rawGenre.empty()) return {};

    std::vector<std::string> result;
    std::unordered_set<std::string> seen;

    std::string cur;
    auto addToken = [&](const std::string& t) {
        auto canonList = mapRawTokenToCanonical(t);
        for (const auto& c : canonList) {
            if (seen.insert(c).second) {
                result.push_back(c);
            }
        }
    };

    for (char c : rawGenre) {
        if (c == ',' || c == '/' || c == ';' || c == '|') {
            addToken(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    addToken(cur);

    return result;
}

// Checks if a game matches a selected genre filter
inline bool gameMatchesGenre(const Game& game, const std::string& filterGenre) {
    if (filterGenre.empty() ||
        filterGenre == "Все жанры" ||
        filterGenre == "All genres" ||
        filterGenre == "app/filter/all_genres"_i18n) {
        return true;
    }

    std::string lowerFilter = toLowerUtf8(trimString(filterGenre));

    // 1. Is filterGenre directly a canonical ID (e.g. "adventure", "rpg")?
    std::string targetCanonicalId;
    for (const auto& canonId : getAllCanonicalGenreIds()) {
        if (lowerFilter == canonId) {
            targetCanonicalId = canonId;
            break;
        }
    }

    // 2. If not a canonical ID, check if it maps to canonical IDs (e.g. legacy "Экшен" or "Action")
    if (targetCanonicalId.empty()) {
        auto mapped = mapRawTokenToCanonical(lowerFilter);
        if (!mapped.empty()) {
            targetCanonicalId = mapped.front();
        }
    }

    // 3. If target is canonical, check game's canonical genres
    if (!targetCanonicalId.empty()) {
        auto gameGenres = extractGameCanonicalGenres(game.genre);
        for (const auto& g : gameGenres) {
            if (g == targetCanonicalId) return true;
        }
        return false;
    }

    // 4. Fallback for custom or unmapped tags: substring search in game's raw genre
    std::string lowerGameGenre = toLowerUtf8(game.genre);
    return (lowerGameGenre.find(lowerFilter) != std::string::npos);
}

// Extract canonical genre filter items with dynamic counting and thresholding
inline std::vector<GenreFilterItem> getGenreFilterItems(const std::vector<Game>& games, int minCount = 5) {
    std::unordered_map<std::string, int> counts;
    counts.reserve(32);

    for (const auto& g : games) {
        auto genres = extractGameCanonicalGenres(g.genre);
        for (const auto& gen : genres) {
            counts[gen]++;
        }
    }

    std::vector<GenreFilterItem> items;
    items.reserve(counts.size() + 1);

    // Option 0: All genres
    GenreFilterItem allItem;
    allItem.id = "";
    allItem.displayName = "app/filter/all_genres"_i18n;
    allItem.count = static_cast<int>(games.size());
    items.push_back(allItem);

    // Filter items meeting the threshold (minCount)
    std::vector<GenreFilterItem> validItems;
    for (const auto& [id, count] : counts) {
        if (count >= minCount) {
            GenreFilterItem item;
            item.id = id;
            item.count = count;
            item.displayName = getLocalizedGenreName(id) + " (" + std::to_string(count) + ")";
            validItems.push_back(item);
        }
    }

    // Sort descending by game count, then alphabetically by name
    std::sort(validItems.begin(), validItems.end(), [](const GenreFilterItem& a, const GenreFilterItem& b) {
        if (a.count != b.count) {
            return a.count > b.count; // Popular first
        }
        return a.displayName < b.displayName;
    });

    items.insert(items.end(), validItems.begin(), validItems.end());
    return items;
}

// Format genre badges for GameDetailView
inline std::vector<std::string> getDisplayGenreBadges(const std::string& rawGenre) {
    if (rawGenre.empty()) return {};

    std::vector<std::string> badges;
    std::unordered_set<std::string> seenBadges;

    std::string cur;
    auto processToken = [&](const std::string& t) {
        std::string trimmed = trimString(t);
        if (trimmed.empty()) return;

        auto canonicals = mapRawTokenToCanonical(trimmed);
        if (!canonicals.empty()) {
            for (const auto& cId : canonicals) {
                std::string locName = getLocalizedGenreName(cId);
                if (seenBadges.insert(locName).second) {
                    badges.push_back(locName);
                }
            }
        } else {
            // Keep clean original tag (e.g. "Cyberpunk", "B&W")
            if (seenBadges.insert(trimmed).second) {
                badges.push_back(trimmed);
            }
        }
    };

    for (char c : rawGenre) {
        if (c == ',' || c == '/' || c == ';' || c == '|') {
            processToken(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    processToken(cur);

    return badges;
}

// Backwards compatibility for callers expecting std::vector<std::string>
inline std::vector<std::string> extractGenres(const std::vector<Game>& games) {
    auto items = getGenreFilterItems(games, 5);
    std::vector<std::string> result;
    result.reserve(items.size());
    for (const auto& item : items) {
        result.push_back(item.displayName);
    }
    return result;
}

} // namespace catalog
