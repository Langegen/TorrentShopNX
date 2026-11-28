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
#include "game_stats_manager.h"

namespace catalog {

enum class SortOption {
    DEFAULT = 0,
    TITLE_ASC = 1,
    TITLE_DESC = 2,
    SIZE_ASC = 3,
    SIZE_DESC = 4,
    YEAR_DESC = 5,
    YEAR_ASC = 6,
    SEEDS_DESC = 7,
    SEEDS_ASC = 8,
    DOWNLOADS_DESC = 9,
    DOWNLOADS_ASC = 10,
    DATE_DESC = 11,
    DATE_ASC = 12,
};

inline std::vector<std::string> getSortOptionNames() {
    return {
        "app/filter/sort_default"_i18n,
        "app/filter/sort_title_asc"_i18n,
        "app/filter/sort_title_desc"_i18n,
        "app/filter/sort_size_asc"_i18n,
        "app/filter/sort_size_desc"_i18n,
        "app/filter/sort_year_desc"_i18n,
        "app/filter/sort_year_asc"_i18n,
        "app/filter/sort_seeds_desc"_i18n,
        "app/filter/sort_seeds_asc"_i18n,
        "app/filter/sort_downloads_desc"_i18n,
        "app/filter/sort_downloads_asc"_i18n,
        "app/filter/sort_date_desc"_i18n,
        "app/filter/sort_date_asc"_i18n
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
    ARABIC_ONLY = 10,
    MULTI_ONLY = 11,
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
        "app/filter/lang_ara"_i18n,
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
    bool onlyRomsets = false;     // Retro: only compilations / romsets
    bool onlyPortable = false;    // PC: only Portable releases
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
               !onlyRomsets && !onlyPortable &&
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
        onlyRomsets = false;
        onlyPortable = false;
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
    if (s == "нет" || s == "no" || s == "1" || s == "non" || s == "nein" || s == "não" || s == "ninguno" ||
        s == "无" || s == "否" || s == "不支持" || s == "单人" || s == "单人游戏" ||
        s == "無" || s == "不支援" || s == "單人" || s == "單人遊戲") return 1;
    if (s == "支持" || s == "是" || s == "多人" || s == "双人" || s == "支持多人" || s == "多人游戏" || s == "联机" || s == "合作" ||
        s == "支援" || s == "雙人" || s == "支援多人" || s == "多人遊戲" || s == "連線") return 2;

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

inline bool checkLanguageMask(util::LanguageMask mask, LanguageFilter lang) {
    switch (lang) {
        case LanguageFilter::RUSSIAN_ONLY: return (mask & util::LangRus) != 0;
        case LanguageFilter::ENGLISH_ONLY: return (mask & util::LangEng) != 0;
        case LanguageFilter::SPANISH_ONLY: return (mask & util::LangSpa) != 0;
        case LanguageFilter::FRENCH_ONLY: return (mask & util::LangFra) != 0;
        case LanguageFilter::GERMAN_ONLY: return (mask & util::LangGer) != 0;
        case LanguageFilter::ITALIAN_ONLY: return (mask & util::LangIta) != 0;
        case LanguageFilter::JAPANESE_ONLY: return (mask & util::LangJpn) != 0;
        case LanguageFilter::CHINESE_ONLY: return (mask & util::LangChinese) != 0;
        case LanguageFilter::PORTUGUESE_ONLY: return (mask & util::LangPor) != 0;
        case LanguageFilter::ARABIC_ONLY: return (mask & util::LangAra) != 0;
        case LanguageFilter::MULTI_ONLY: return util::metadataIsMulti(mask);
        default: return true;
    }
}

inline bool checkLanguageMatch(const std::string& lowerLang, LanguageFilter lang) {
    return checkLanguageMask(util::metadataLanguageMask(lowerLang), lang);
}

inline util::LanguageMask gameLanguageMask(const Game& game) {
    auto mask = util::metadataLanguageMask(toLowerUtf8(game.interface_lang))
              | util::metadataLanguageMask(toLowerUtf8(game.voice_lang));
    // Only explicit bracketed tags are a fallback, never arbitrary title words.
    if (!mask && util::isPlaceholderValue(game.interface_lang) && util::isPlaceholderValue(game.voice_lang))
        mask = util::titleLanguageTags(toLowerUtf8(game.title));
    return mask;
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

// Map a game's interface_lang (same priority as getGameLangBadge) to LanguageFilter.
// Returns LanguageFilter::ALL when no dedicated filter exists (e.g. standalone KOR/HUN).
inline LanguageFilter languageFilterForBadge(const std::string& interfaceLang, const std::string& appLocale = "") {
    if (util::isPlaceholderValue(interfaceLang)) return LanguageFilter::ALL;

    const auto mask = util::metadataLanguageMask(util::toLowerUtf8(interfaceLang));
    if (!mask) return LanguageFilter::MULTI_ONLY;

    const std::string locale = util::toLowerUtf8(appLocale);
    const bool chineseUi = locale.rfind("zh", 0) == 0;
    if (chineseUi && (mask & util::LangChinese)) return LanguageFilter::CHINESE_ONLY;
    if (mask & util::LangRus)     return LanguageFilter::RUSSIAN_ONLY;
    if (mask & util::LangEng)     return LanguageFilter::ENGLISH_ONLY;
    if (mask & util::LangChinese) return LanguageFilter::CHINESE_ONLY;
    if (mask & util::LangSpa)     return LanguageFilter::SPANISH_ONLY;
    if (mask & util::LangFra)     return LanguageFilter::FRENCH_ONLY;
    if (mask & util::LangGer)     return LanguageFilter::GERMAN_ONLY;
    if (mask & util::LangIta)     return LanguageFilter::ITALIAN_ONLY;
    if (mask & util::LangJpn)     return LanguageFilter::JAPANESE_ONLY;
    if (mask & util::LangPor)     return LanguageFilter::PORTUGUESE_ONLY;
    if (mask & util::LangAra)     return LanguageFilter::ARABIC_ONLY;
    if (util::metadataIsMulti(mask)) return LanguageFilter::MULTI_ONLY;
    return LanguageFilter::ALL;
}

// Short header label matching the active language badge filter (e.g. "RUS • MULTI" -> "RUS", "MULTI" -> "MULTI").
inline std::string languageFilterHeaderLabel(LanguageFilter filter, const std::string& badgeText) {
    if (filter == LanguageFilter::MULTI_ONLY) {
        const size_t sep = badgeText.find(" \xE2\x80\xA2 ");
        if (sep != std::string::npos) return badgeText.substr(sep + 5);
        return badgeText.empty() ? "MULTI" : badgeText;
    }
    const size_t sep = badgeText.find(" \xE2\x80\xA2 ");
    if (sep != std::string::npos) return badgeText.substr(0, sep);
    return badgeText;
}

// Map multiplayer string to the matching PlayersFilter bucket.
// Non-numeric strings (e.g. "Yes", "Coop") map to ANY_MULTI instead of exact TWO_PLAYERS.
inline PlayersFilter playersFilterForMultiplayer(const std::string& multiplayer) {
    if (util::isPlaceholderValue(multiplayer)) return PlayersFilter::ALL;
    const int maxP = parseMaxPlayers(multiplayer);
    if (maxP <= 1) return PlayersFilter::ALL;
    bool hasDigit = false;
    for (unsigned char c : multiplayer) {
        if (std::isdigit(c)) { hasDigit = true; break; }
    }
    if (!hasDigit) return PlayersFilter::ANY_MULTI;
    if (maxP == 2) return PlayersFilter::TWO_PLAYERS;
    if (maxP <= 4) return PlayersFilter::THREE_FOUR;
    return PlayersFilter::FIVE_PLUS;
}

namespace filter_detail {

// matchesGameFilter() runs once per game (~7000) on every filter pass: the
// i18n lookups and the query lower-casing used to happen per game too.
// Language changes need an app restart, so caching the texts is safe.
inline const std::string& allGenresText() {
    static const std::string s = "app/filter/all_genres"_i18n;
    return s;
}
inline const std::string& allYearsText() {
    static const std::string s = "app/filter/all_years"_i18n;
    return s;
}

struct PreparedQuery {
    std::string raw;
    std::string lower;
    std::string norm;   // lower-case without : - , . ' " ! ?
    bool valid = false;
};

inline const PreparedQuery& preparedQuery(const std::string& q) {
    thread_local PreparedQuery pq;
    if (!pq.valid || pq.raw != q) {
        pq.raw = q;
        pq.lower = toLowerUtf8(q);
        pq.norm.clear();
        for (char c : pq.lower) {
            if (c != ':' && c != '-' && c != ',' && c != '.' && c != '\'' && c != '\"' && c != '!' && c != '?') {
                pq.norm.push_back(c);
            }
        }
        pq.valid = true;
    }
    return pq;
}

} // namespace filter_detail

// Check if game matches filter state
inline bool matchesGameFilter(const Game& game, const FilterSortState& state, bool isFavorite) {
    // 1. Favorite filter
    if (state.onlyFavorites && !isFavorite) {
        return false;
    }
    if (state.onlyRomsets && !isRomsetGame(game)) {
        return false;
    }
    if (state.onlyPortable && !isPortablePcGame(game)) {
        return false;
    }

    // 2. Genre filter (Multi-select OR logic)
    if (!state.genres.empty()) {
        bool matchAny = false;
        for (const auto& gId : state.genres) {
            if (gId.empty() || gId == "Все жанры" || gId == filter_detail::allGenresText() || gameMatchesGenre(game, gId)) {
                matchAny = true;
                break;
            }
        }
        if (!matchAny) return false;
    } else if (!state.genre.empty() && state.genre != "Все жанры" && state.genre != filter_detail::allGenresText()) {
        if (!gameMatchesGenre(game, state.genre)) {
            return false;
        }
    }

    // 3. Language filter (Multi-select OR logic)
    if (!state.langs.empty()) {
        const auto languageMask = gameLanguageMask(game);
        bool matchAny = false;
        for (auto lf : state.langs) {
            if (lf == LanguageFilter::ALL || checkLanguageMask(languageMask, lf)) {
                matchAny = true;
                break;
            }
        }
        if (!matchAny) return false;
    } else if (state.lang != LanguageFilter::ALL) {
        const auto languageMask = gameLanguageMask(game);
        if (!checkLanguageMask(languageMask, state.lang)) {
            return false;
        }
    }

    // 4. Year filter (Multi-select OR logic)
    if (!state.years.empty()) {
        int gameYear = parseYear(game.year);
        bool matchAny = false;
        for (const auto& yStr : state.years) {
            if (yStr.empty() || yStr == "Все года" || yStr == "Все годы" || yStr == filter_detail::allYearsText()) {
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
    } else if (!state.year.empty() && state.year != "Все годы" && state.year != "Все года" && state.year != filter_detail::allYearsText()) {
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
        const auto& query = filter_detail::preparedQuery(state.searchQuery);
        const std::string& lowerQuery = query.lower;
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
                const std::string& normQuery = query.norm;
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

inline uint64_t parseTopicId(const std::string& tid) {
    if (tid.empty()) return 0;
    uint64_t id = 0;
    for (char c : tid) {
        if (c >= '0' && c <= '9') {
            id = id * 10 + (c - '0');
        } else if (c != ' ' && c != '\t') {
            return 0;
        }
    }
    return id;
}

// Comparator for Game sorting (zero heap allocations using precomputed sort_key)
inline bool compareGames(const Game& a, const Game& b, SortOption sort) {
    switch (sort) {
        case SortOption::TITLE_ASC: {
            return a.sort_key < b.sort_key;
        }
        case SortOption::TITLE_DESC: {
            return a.sort_key > b.sort_key;
        }
        case SortOption::SIZE_ASC: {
            uint64_t sA = parseSizeToBytes(a.size);
            uint64_t sB = parseSizeToBytes(b.size);
            if (sA != sB) return sA < sB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::SIZE_DESC: {
            uint64_t sA = parseSizeToBytes(a.size);
            uint64_t sB = parseSizeToBytes(b.size);
            if (sA != sB) return sA > sB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::YEAR_DESC: {
            int yA = parseYear(a.year);
            int yB = parseYear(b.year);
            if (yA != yB) return yA > yB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::YEAR_ASC: {
            int yA = parseYear(a.year);
            int yB = parseYear(b.year);
            // Treat 0 (unknown year) as highest when ascending so known older years appear first
            if (yA == 0) yA = 9999;
            if (yB == 0) yB = 9999;
            if (yA != yB) return yA < yB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::SEEDS_DESC: {
            const auto sA = GameStatsManager::instance().getAnyStats(a.topic_id);
            const auto sB = GameStatsManager::instance().getAnyStats(b.topic_id);
            int32_t valA = sA ? sA->seeds : -1;
            int32_t valB = sB ? sB->seeds : -1;
            if (valA != valB) return valA > valB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::SEEDS_ASC: {
            const auto sA = GameStatsManager::instance().getAnyStats(a.topic_id);
            const auto sB = GameStatsManager::instance().getAnyStats(b.topic_id);
            int32_t valA = (sA && sA->seeds > 0) ? sA->seeds : INT32_MAX;
            int32_t valB = (sB && sB->seeds > 0) ? sB->seeds : INT32_MAX;
            if (valA != valB) return valA < valB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::DOWNLOADS_DESC: {
            const auto sA = GameStatsManager::instance().getAnyStats(a.topic_id);
            const auto sB = GameStatsManager::instance().getAnyStats(b.topic_id);
            int64_t valA = sA ? sA->downloads : -1;
            int64_t valB = sB ? sB->downloads : -1;
            if (valA != valB) return valA > valB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::DOWNLOADS_ASC: {
            const auto sA = GameStatsManager::instance().getAnyStats(a.topic_id);
            const auto sB = GameStatsManager::instance().getAnyStats(b.topic_id);
            int64_t valA = (sA && sA->downloads > 0) ? sA->downloads : INT64_MAX;
            int64_t valB = (sB && sB->downloads > 0) ? sB->downloads : INT64_MAX;
            if (valA != valB) return valA < valB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::DATE_DESC: {
            const auto sA = GameStatsManager::instance().getAnyStats(a.topic_id);
            const auto sB = GameStatsManager::instance().getAnyStats(b.topic_id);
            int64_t valA = (sA && sA->latest_timestamp > 0) ? sA->latest_timestamp : -1;
            int64_t valB = (sB && sB->latest_timestamp > 0) ? sB->latest_timestamp : -1;
            if (valA != valB) return valA > valB;
            uint64_t idA = parseTopicId(a.topic_id);
            uint64_t idB = parseTopicId(b.topic_id);
            if (idA != idB) return idA > idB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::DATE_ASC: {
            const auto sA = GameStatsManager::instance().getAnyStats(a.topic_id);
            const auto sB = GameStatsManager::instance().getAnyStats(b.topic_id);
            int64_t valA = (sA && sA->latest_timestamp > 0) ? sA->latest_timestamp : INT64_MAX;
            int64_t valB = (sB && sB->latest_timestamp > 0) ? sB->latest_timestamp : INT64_MAX;
            if (valA != valB) return valA < valB;
            uint64_t idA = parseTopicId(a.topic_id);
            uint64_t idB = parseTopicId(b.topic_id);
            if (idA == 0) idA = UINT64_MAX;
            if (idB == 0) idB = UINT64_MAX;
            if (idA != idB) return idA < idB;
            return a.sort_key < b.sort_key;
        }
        case SortOption::DEFAULT:
        default:
            return false;
    }
}

inline bool compareGames(const Game* a, const Game* b, SortOption sort) {
    if (!a && !b) return false;
    if (!a) return false;
    if (!b) return true;
    return compareGames(*a, *b, sort);
}

// Numeric sort keys for one game, matching compareGames(). Smaller sorts
// first; ties fall back to sort_key ascending.
struct GameSortKeys {
    int64_t k1 = 0;
    int64_t k2 = 0;
};

inline GameSortKeys makeGameSortKeys(const Game& g, SortOption sort) {
    GameSortKeys k;
    switch (sort) {
        case SortOption::SIZE_ASC:  k.k1 = (int64_t)parseSizeToBytes(g.size); break;
        case SortOption::SIZE_DESC: k.k1 = -(int64_t)parseSizeToBytes(g.size); break;
        case SortOption::YEAR_DESC: k.k1 = -parseYear(g.year); break;
        case SortOption::YEAR_ASC: {
            int y = parseYear(g.year);
            k.k1 = y == 0 ? 9999 : y;
            break;
        }
        case SortOption::SEEDS_DESC:
        case SortOption::SEEDS_ASC:
        case SortOption::DOWNLOADS_DESC:
        case SortOption::DOWNLOADS_ASC:
        case SortOption::DATE_DESC:
        case SortOption::DATE_ASC: {
            const auto st = GameStatsManager::instance().getAnyStats(g.topic_id);
            if (sort == SortOption::SEEDS_DESC) {
                k.k1 = -(int64_t)(st ? st->seeds : -1);
            } else if (sort == SortOption::SEEDS_ASC) {
                k.k1 = (st && st->seeds > 0) ? st->seeds : INT32_MAX;
            } else if (sort == SortOption::DOWNLOADS_DESC) {
                k.k1 = -(st ? st->downloads : -1);
            } else if (sort == SortOption::DOWNLOADS_ASC) {
                k.k1 = (st && st->downloads > 0) ? st->downloads : INT64_MAX;
            } else if (sort == SortOption::DATE_DESC) {
                k.k1 = -((st && st->latest_timestamp > 0) ? st->latest_timestamp : -1);
                k.k2 = -(int64_t)parseTopicId(g.topic_id);
            } else {
                k.k1 = (st && st->latest_timestamp > 0) ? st->latest_timestamp : INT64_MAX;
                uint64_t id = parseTopicId(g.topic_id);
                k.k2 = id == 0 ? INT64_MAX : (int64_t)id;
            }
            break;
        }
        default: break;
    }
    return k;
}

// Stable sort of any container of game handles. Sort keys (sizes, years,
// stats) are computed once per game instead of twice per comparison, and the
// stats lock is taken n times instead of n*log(n).
template <typename T, typename GetGame>
inline void sortGamesBy(std::vector<T>& items, SortOption sort, GetGame getGame) {
    if (sort == SortOption::DEFAULT || items.size() < 2) return;
    struct Entry { GameSortKeys k; const std::string* key; size_t idx; };
    static const std::string kEmpty;
    std::vector<Entry> entries;
    entries.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const Game* g = getGame(items[i]);
        if (g) entries.push_back({makeGameSortKeys(*g, sort), &g->sort_key, i});
        else   entries.push_back({{INT64_MAX, INT64_MAX}, &kEmpty, i});  // missing games last
    }
    const bool titleDesc = sort == SortOption::TITLE_DESC;
    std::stable_sort(entries.begin(), entries.end(), [titleDesc](const Entry& a, const Entry& b) {
        if (titleDesc) return *a.key > *b.key;
        if (a.k.k1 != b.k.k1) return a.k.k1 < b.k.k1;
        if (a.k.k2 != b.k.k2) return a.k.k2 < b.k.k2;
        return *a.key < *b.key;
    });
    std::vector<T> sorted;
    sorted.reserve(items.size());
    for (const auto& e : entries) sorted.push_back(std::move(items[e.idx]));
    items = std::move(sorted);
}

} // namespace catalog
