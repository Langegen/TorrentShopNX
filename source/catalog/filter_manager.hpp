#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <set>
#include <cstdio>
#include <borealis.hpp>
#include "../GameData.hpp"
#include "genre_taxonomy.hpp"

namespace catalog {

enum class SortOption {
    DEFAULT = 0,
    TITLE_ASC = 1,
    TITLE_DESC = 2,
    SIZE_ASC = 3,
    SIZE_DESC = 4,
    YEAR_DESC = 5,
    YEAR_ASC = 6,
};

inline std::vector<std::string> getSortOptionNames() {
    return {
        "app/filter/sort_default"_i18n,
        "app/filter/sort_title_asc"_i18n,
        "app/filter/sort_title_desc"_i18n,
        "app/filter/sort_size_asc"_i18n,
        "app/filter/sort_size_desc"_i18n,
        "app/filter/sort_year_desc"_i18n,
        "app/filter/sort_year_asc"_i18n
    };
}

enum class LanguageFilter {
    ALL = 0,
    RUSSIAN_ONLY = 1,
    ENGLISH_ONLY = 2,
    SPANISH_ONLY = 3,
    FRENCH_ONLY = 4,
    GERMAN_ONLY = 5,
    ITALIAN_ONLY = 6,
    JAPANESE_ONLY = 7,
    CHINESE_ONLY = 8,
    PORTUGUESE_ONLY = 9,
    MULTI_ONLY = 10,
};

inline std::vector<std::string> getLanguageFilterNames() {
    return {
        "app/filter/lang_all"_i18n,
        "app/filter/lang_rus"_i18n,
        "app/filter/lang_eng"_i18n,
        "app/filter/lang_spa"_i18n,
        "app/filter/lang_fra"_i18n,
        "app/filter/lang_ger"_i18n,
        "app/filter/lang_ita"_i18n,
        "app/filter/lang_jpn"_i18n,
        "app/filter/lang_zho"_i18n,
        "app/filter/lang_por"_i18n,
        "app/filter/lang_multi"_i18n
    };
}

enum class PlayersFilter {
    ALL = 0,
    SINGLE_ONLY = 1,
    TWO_PLAYERS = 2,
    THREE_FOUR = 3,
    FIVE_PLUS = 4,
    ANY_MULTI = 5,
};

inline std::vector<std::string> getPlayerFilterNames() {
    return {
        "app/filter/players_all"_i18n,
        "app/filter/players_1"_i18n,
        "app/filter/players_2"_i18n,
        "app/filter/players_3_4"_i18n,
        "app/filter/players_5_plus"_i18n,
        "app/filter/players_multi"_i18n
    };
}

struct FilterSortState {
    SortOption sort = SortOption::DEFAULT;
    std::string genre = "";       // empty means "All genres"
    std::vector<std::string> genres; // multiple canonical genre IDs
    LanguageFilter lang = LanguageFilter::ALL;
    std::vector<LanguageFilter> langs; // multiple language filters
    bool onlyFavorites = false;
    std::string year = "";        // empty means "All years"
    std::vector<std::string> years; // multiple year strings
    PlayersFilter players = PlayersFilter::ALL;
    std::vector<PlayersFilter> playersList; // multiple player filters
    std::string searchQuery = ""; // text search

    bool isDefault() const {
        return sort == SortOption::DEFAULT &&
               genre.empty() && genres.empty() &&
               lang == LanguageFilter::ALL && langs.empty() &&
               !onlyFavorites &&
               year.empty() && years.empty() &&
               players == PlayersFilter::ALL && playersList.empty() &&
               searchQuery.empty();
    }

    void reset() {
        sort = SortOption::DEFAULT;
        genre.clear();
        genres.clear();
        lang = LanguageFilter::ALL;
        langs.clear();
        onlyFavorites = false;
        year.clear();
        years.clear();
        players = PlayersFilter::ALL;
        playersList.clear();
        searchQuery.clear();
    }
};

// Convert size string (e.g. "14.28 GB", "850 MB", "500 KB", "1.2 TB") to raw bytes
inline uint64_t parseSizeToBytes(const std::string& sizeStr) {
    if (sizeStr.empty()) return 0;
    std::string s = sizeStr;
    // Replace comma with dot
    for (char& ch : s) {
        if (ch == ',') ch = '.';
    }
    double val = 0.0;
    char unit[16] = {0};
    int parsed = std::sscanf(s.c_str(), "%lf %15s", &val, unit);
    if (parsed >= 1) {
        std::string u = unit;
        for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u.find("TB") != std::string::npos) return static_cast<uint64_t>(val * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
        if (u.find("GB") != std::string::npos) return static_cast<uint64_t>(val * 1024ULL * 1024ULL * 1024ULL);
        if (u.find("MB") != std::string::npos) return static_cast<uint64_t>(val * 1024ULL * 1024ULL);
        if (u.find("KB") != std::string::npos) return static_cast<uint64_t>(val * 1024ULL);
        if (u.find("B") != std::string::npos) return static_cast<uint64_t>(val);
        return static_cast<uint64_t>(val);
    }
    return 0;
}

// Extract 4-digit release year as integer
inline int parseYear(const std::string& yearStr) {
    if (yearStr.empty()) return 0;
    int y = 0;
    for (size_t i = 0; i + 3 < yearStr.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(yearStr[i])) &&
            std::isdigit(static_cast<unsigned char>(yearStr[i+1])) &&
            std::isdigit(static_cast<unsigned char>(yearStr[i+2])) &&
            std::isdigit(static_cast<unsigned char>(yearStr[i+3]))) {
            y = std::atoi(yearStr.substr(i, 4).c_str());
            if (y >= 1980 && y <= 2099) return y;
        }
    }
    return 0;
}

// Extract maximum number of players as integer (1 = singleplayer, 2+ = multiplayer)
inline int parseMaxPlayers(const std::string& multiplayer) {
    if (multiplayer.empty()) return 1;
    std::string s = toLowerUtf8(multiplayer);
    if (s == "нет" || s == "no" || s == "1") return 1;

    int maxPlayers = 0;
    int currentNum = 0;
    bool inNum = false;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            currentNum = currentNum * 10 + (c - '0');
            inNum = true;
        } else {
            if (inNum) {
                if (currentNum > maxPlayers) maxPlayers = currentNum;
                currentNum = 0;
                inNum = false;
            }
        }
    }
    if (inNum && currentNum > maxPlayers) {
        maxPlayers = currentNum;
    }

    if (maxPlayers > 0) {
        return maxPlayers;
    }

    if (s.find("мульти") != std::string::npos ||
        s.find("multi") != std::string::npos ||
        s.find("да") != std::string::npos ||
        s.find("yes") != std::string::npos ||
        s.find("кооп") != std::string::npos ||
        s.find("coop") != std::string::npos ||
        s.find("сеть") != std::string::npos) {
        return 2;
    }

    return 1;
}


// Extract unique years from games collection
inline std::vector<std::string> extractYears(const std::vector<Game>& games) {
    std::set<int> yearsSet;
    for (const auto& g : games) {
        int y = parseYear(g.year);
        if (y >= 1980 && y <= 2099) {
            yearsSet.insert(y);
        }
    }

    std::vector<std::string> result;
    result.push_back("app/filter/all_years"_i18n);
    // Sort years descending (newest first)
    for (auto it = yearsSet.rbegin(); it != yearsSet.rend(); ++it) {
        result.push_back(std::to_string(*it));
    }
    return result;
}

inline bool checkLanguageMatch(const std::string& lowerLang, LanguageFilter lang) {
    switch (lang) {
        case LanguageFilter::RUSSIAN_ONLY:
            return (lowerLang.find("rus") != std::string::npos || lowerLang.find("рус") != std::string::npos);
        case LanguageFilter::ENGLISH_ONLY:
            return (lowerLang.find("eng") != std::string::npos || lowerLang.find("англ") != std::string::npos);
        case LanguageFilter::SPANISH_ONLY:
            return (lowerLang.find("spa") != std::string::npos || lowerLang.find("esp") != std::string::npos ||
                    lowerLang.find("исп") != std::string::npos || lowerLang.find("castellano") != std::string::npos);
        case LanguageFilter::FRENCH_ONLY:
            return (lowerLang.find("fra") != std::string::npos || lowerLang.find("fre") != std::string::npos ||
                    lowerLang.find("фран") != std::string::npos || lowerLang.find("french") != std::string::npos);
        case LanguageFilter::GERMAN_ONLY:
            return (lowerLang.find("ger") != std::string::npos || lowerLang.find("deu") != std::string::npos ||
                    lowerLang.find("нем") != std::string::npos || lowerLang.find("deutsch") != std::string::npos ||
                    lowerLang.find("german") != std::string::npos);
        case LanguageFilter::ITALIAN_ONLY:
            return (lowerLang.find("ita") != std::string::npos || lowerLang.find("ита") != std::string::npos ||
                    lowerLang.find("italiano") != std::string::npos || lowerLang.find("italian") != std::string::npos);
        case LanguageFilter::JAPANESE_ONLY:
            return (lowerLang.find("jpn") != std::string::npos || lowerLang.find("jap") != std::string::npos ||
                    lowerLang.find("япон") != std::string::npos || lowerLang.find("japanese") != std::string::npos);
        case LanguageFilter::CHINESE_ONLY:
            return (lowerLang.find("chi") != std::string::npos || lowerLang.find("zho") != std::string::npos ||
                    lowerLang.find("кит") != std::string::npos || lowerLang.find("chinese") != std::string::npos);
        case LanguageFilter::PORTUGUESE_ONLY:
            return (lowerLang.find("por") != std::string::npos || lowerLang.find("порт") != std::string::npos ||
                    lowerLang.find("portug") != std::string::npos);
        case LanguageFilter::MULTI_ONLY:
            return (lowerLang.find("multi") != std::string::npos || lowerLang.find("мульти") != std::string::npos);
        default:
            return true;
    }
}

inline bool checkPlayersMatch(int maxP, PlayersFilter pf) {
    switch (pf) {
        case PlayersFilter::SINGLE_ONLY:
            return (maxP == 1);
        case PlayersFilter::TWO_PLAYERS:
            return (maxP == 2);
        case PlayersFilter::THREE_FOUR:
            return (maxP >= 3 && maxP <= 4);
        case PlayersFilter::FIVE_PLUS:
            return (maxP >= 5);
        case PlayersFilter::ANY_MULTI:
            return (maxP >= 2);
        default:
            return true;
    }
}

// Check if game matches filter state
inline bool matchesGameFilter(const Game& game, const FilterSortState& state, bool isFavorite) {
    // 1. Favorite filter
    if (state.onlyFavorites && !isFavorite) {
        return false;
    }

    // 2. Genre filter (Multi-select OR logic)
    if (!state.genres.empty()) {
        bool matchAny = false;
        for (const auto& gId : state.genres) {
            if (gId.empty() || gId == "Все жанры" || gId == "app/filter/all_genres"_i18n || gameMatchesGenre(game, gId)) {
                matchAny = true;
                break;
            }
        }
        if (!matchAny) return false;
    } else if (!state.genre.empty() && state.genre != "Все жанры" && state.genre != "app/filter/all_genres"_i18n) {
        if (!gameMatchesGenre(game, state.genre)) {
            return false;
        }
    }

    // 3. Language filter (Multi-select OR logic)
    if (!state.langs.empty()) {
        std::string lowerLang = toLowerUtf8(game.interface_lang + " " + game.voice_lang + " " + game.title);
        bool matchAny = false;
        for (auto lf : state.langs) {
            if (lf == LanguageFilter::ALL || checkLanguageMatch(lowerLang, lf)) {
                matchAny = true;
                break;
            }
        }
        if (!matchAny) return false;
    } else if (state.lang != LanguageFilter::ALL) {
        std::string lowerLang = toLowerUtf8(game.interface_lang + " " + game.voice_lang + " " + game.title);
        if (!checkLanguageMatch(lowerLang, state.lang)) {
            return false;
        }
    }

    // 4. Year filter (Multi-select OR logic)
    if (!state.years.empty()) {
        int gameYear = parseYear(game.year);
        bool matchAny = false;
        for (const auto& yStr : state.years) {
            if (yStr.empty() || yStr == "Все года" || yStr == "Все годы" || yStr == "app/filter/all_years"_i18n) {
                matchAny = true;
                break;
            }
            int targetYear = std::atoi(yStr.c_str());
            if (targetYear > 0 && gameYear == targetYear) {
                matchAny = true;
                break;
            }
        }
        if (!matchAny) return false;
    } else if (!state.year.empty() && state.year != "Все годы" && state.year != "Все года" && state.year != "app/filter/all_years"_i18n) {
        int targetYear = std::atoi(state.year.c_str());
        if (targetYear > 0 && parseYear(game.year) != targetYear) {
            return false;
        }
    }

    // 5. Players filter (Multi-select OR logic)
    if (!state.playersList.empty()) {
        int maxP = parseMaxPlayers(game.multiplayer);
        bool matchAny = false;
        for (auto pf : state.playersList) {
            if (pf == PlayersFilter::ALL || checkPlayersMatch(maxP, pf)) {
                matchAny = true;
                break;
            }
        }
        if (!matchAny) return false;
    } else if (state.players != PlayersFilter::ALL) {
        int maxP = parseMaxPlayers(game.multiplayer);
        if (!checkPlayersMatch(maxP, state.players)) {
            return false;
        }
    }

    // 6. Search query filter
    if (!state.searchQuery.empty()) {
        std::string lowerQuery = toLowerUtf8(state.searchQuery);
        bool match = false;

        // Check Title ID (e.g. "01006F8002326000")
        if (!game.title_id.empty()) {
            std::string lowerTid = toLowerUtf8(game.title_id);
            if (lowerTid.find(lowerQuery) != std::string::npos) {
                match = true;
            }
        }

        if (!match) {
            std::string lowerTitle = toLowerUtf8(game.title);
            if (lowerTitle.find(lowerQuery) != std::string::npos) {
                match = true;
            } else {
                auto stripPunct = [](const std::string& str) {
                    std::string res;
                    for (char c : str) {
                        if (c != ':' && c != '-' && c != ',' && c != '.' && c != '\'' && c != '\"' && c != '!' && c != '?') {
                            res.push_back(c);
                        }
                    }
                    return res;
                };
                std::string normQuery = stripPunct(lowerQuery);
                std::string normTitle = stripPunct(lowerTitle);
                if (!normQuery.empty() && normTitle.find(normQuery) != std::string::npos) {
                    match = true;
                }
            }
        }

        if (!match && !game.developer.empty()) {
            std::string lowerDev = toLowerUtf8(game.developer);
            if (lowerDev.find(lowerQuery) != std::string::npos) {
                match = true;
            }
        }

        if (!match && !game.publisher.empty()) {
            std::string lowerPub = toLowerUtf8(game.publisher);
            if (lowerPub.find(lowerQuery) != std::string::npos) {
                match = true;
            }
        }

        if (!match) {
            return false;
        }
    }

    return true;
}

// Normalize title for sorting by skipping leading punctuation and symbols
inline std::string normalizeTitleForSort(const std::string& title) {
    std::string s = cleanTitle(title);
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 32 || c == '[' || c == '(' || c == '"' || c == '\'' ||
            c == '-' || c == '.' || c == '_' || c == '#' || c == '!' ||
            c == '?' || c == '~' || c == ':' || c == '/' || c == '\\' ||
            c == '+' || c == '=' || c == '*' || c == '@' || c == '$' ||
            c == '%' || c == '^' || c == '&') {
            i++;
        } else {
            break;
        }
    }
    return i > 0 ? s.substr(i) : s;
}

// Comparator for Game sorting
inline bool compareGames(const Game& a, const Game& b, SortOption sort) {
    switch (sort) {
        case SortOption::TITLE_ASC: {
            std::string tA = toLowerUtf8(normalizeTitleForSort(a.title));
            std::string tB = toLowerUtf8(normalizeTitleForSort(b.title));
            return tA < tB;
        }
        case SortOption::TITLE_DESC: {
            std::string tA = toLowerUtf8(normalizeTitleForSort(a.title));
            std::string tB = toLowerUtf8(normalizeTitleForSort(b.title));
            return tA > tB;
        }
        case SortOption::SIZE_ASC: {
            uint64_t sA = parseSizeToBytes(a.size);
            uint64_t sB = parseSizeToBytes(b.size);
            if (sA != sB) return sA < sB;
            return toLowerUtf8(normalizeTitleForSort(a.title)) < toLowerUtf8(normalizeTitleForSort(b.title));
        }
        case SortOption::SIZE_DESC: {
            uint64_t sA = parseSizeToBytes(a.size);
            uint64_t sB = parseSizeToBytes(b.size);
            if (sA != sB) return sA > sB;
            return toLowerUtf8(normalizeTitleForSort(a.title)) < toLowerUtf8(normalizeTitleForSort(b.title));
        }
        case SortOption::YEAR_DESC: {
            int yA = parseYear(a.year);
            int yB = parseYear(b.year);
            if (yA != yB) return yA > yB;
            return toLowerUtf8(normalizeTitleForSort(a.title)) < toLowerUtf8(normalizeTitleForSort(b.title));
        }
        case SortOption::YEAR_ASC: {
            int yA = parseYear(a.year);
            int yB = parseYear(b.year);
            // Treat 0 (unknown year) as highest when ascending so known older years appear first
            if (yA == 0) yA = 9999;
            if (yB == 0) yB = 9999;
            if (yA != yB) return yA < yB;
            return toLowerUtf8(normalizeTitleForSort(a.title)) < toLowerUtf8(normalizeTitleForSort(b.title));
        }
        case SortOption::DEFAULT:
        default:
            return false;
    }
}

} // namespace catalog
