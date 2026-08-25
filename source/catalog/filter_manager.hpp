#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <set>
#include <cstdio>
#include <borealis.hpp>
#include "../GameData.hpp"

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
};

inline std::vector<std::string> getLanguageFilterNames() {
    return {
        "app/filter/lang_all"_i18n,
        "app/filter/lang_rus"_i18n,
        "app/filter/lang_eng"_i18n
    };
}

struct FilterSortState {
    SortOption sort = SortOption::DEFAULT;
    std::string genre = "";       // empty means "All genres"
    LanguageFilter lang = LanguageFilter::ALL;
    bool onlyFavorites = false;
    std::string year = "";        // empty means "All years"
    std::string searchQuery = ""; // text search

    bool isDefault() const {
        return sort == SortOption::DEFAULT &&
               genre.empty() &&
               lang == LanguageFilter::ALL &&
               !onlyFavorites &&
               year.empty() &&
               searchQuery.empty();
    }

    void reset() {
        sort = SortOption::DEFAULT;
        genre.clear();
        lang = LanguageFilter::ALL;
        onlyFavorites = false;
        year.clear();
        searchQuery.clear();
    }
};

// UTF-8 lowercase helper handling both ASCII and Cyrillic (CP1251 / UTF-8)
inline std::string toLowerUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(std::tolower(c)));
            i += 1;
        } else if (c == 0xD0 && i + 1 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            if (c2 == 0x81) { // Ё -> ё (0xD1 0x91)
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(0x91));
            } else if (c2 >= 0x90 && c2 <= 0x9F) { // А..П -> а..п (0xD0 0xB0..0xBF)
                out.push_back(static_cast<char>(0xD0));
                out.push_back(static_cast<char>(c2 + 0x20));
            } else if (c2 >= 0xA0 && c2 <= 0xAF) { // Р..Я -> р..я (0xD1 0x80..0x8F)
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(c2 - 0x20));
            } else {
                out.push_back(s[i]);
                out.push_back(s[i + 1]);
            }
            i += 2;
        } else if (c == 0xD1 && i + 1 < s.size()) {
            out.push_back(s[i]);
            out.push_back(s[i + 1]);
            i += 2;
        } else {
            out.push_back(s[i]);
            i += 1;
        }
    }
    return out;
}

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

// Helper to trim string
inline std::string trimString(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

// Extract unique genres from games collection
inline std::vector<std::string> extractGenres(const std::vector<Game>& games) {
    std::set<std::string> genresSet;
    for (const auto& g : games) {
        if (g.genre.empty()) continue;
        std::string cur;
        for (char c : g.genre) {
            if (c == ',' || c == '/' || c == ';' || c == '|') {
                std::string trimmed = trimString(cur);
                if (!trimmed.empty() && trimmed.size() > 1) {
                    genresSet.insert(trimmed);
                }
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        std::string trimmed = trimString(cur);
        if (!trimmed.empty() && trimmed.size() > 1) {
            genresSet.insert(trimmed);
        }
    }

    std::vector<std::string> result;
    result.push_back("app/filter/all_genres"_i18n);
    for (const auto& gen : genresSet) {
        result.push_back(gen);
    }
    return result;
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

// Check if game matches filter state
inline bool matchesGameFilter(const Game& game, const FilterSortState& state, bool isFavorite) {
    // 1. Favorite filter
    if (state.onlyFavorites && !isFavorite) {
        return false;
    }

    // 2. Genre filter
    if (!state.genre.empty() && state.genre != "Все жанры") {
        std::string lowerGameGenre = toLowerUtf8(game.genre);
        std::string lowerFilterGenre = toLowerUtf8(state.genre);
        if (lowerGameGenre.find(lowerFilterGenre) == std::string::npos) {
            return false;
        }
    }

    // 3. Language filter
    if (state.lang != LanguageFilter::ALL) {
        std::string lowerLang = toLowerUtf8(game.interface_lang + " " + game.voice_lang + " " + game.title);
        bool hasRussian = (lowerLang.find("rus") != std::string::npos ||
                           lowerLang.find("рус") != std::string::npos);
        if (state.lang == LanguageFilter::RUSSIAN_ONLY && !hasRussian) {
            return false;
        }
        if (state.lang == LanguageFilter::ENGLISH_ONLY) {
            bool hasEnglish = (lowerLang.find("eng") != std::string::npos ||
                               lowerLang.find("англ") != std::string::npos);
            if (!hasEnglish) return false;
        }
    }

    // 4. Year filter
    if (!state.year.empty() && state.year != "Все годы") {
        int targetYear = std::atoi(state.year.c_str());
        if (targetYear > 0 && parseYear(game.year) != targetYear) {
            return false;
        }
    }

    // 5. Search query filter
    if (!state.searchQuery.empty()) {
        std::string lowerTitle = toLowerUtf8(game.title);
        std::string lowerQuery = toLowerUtf8(state.searchQuery);
        if (lowerTitle.find(lowerQuery) == std::string::npos) {
            return false;
        }
    }

    return true;
}

// Comparator for Game sorting
inline bool compareGames(const Game& a, const Game& b, SortOption sort) {
    switch (sort) {
        case SortOption::TITLE_ASC: {
            std::string tA = toLowerUtf8(cleanTitle(a.title));
            std::string tB = toLowerUtf8(cleanTitle(b.title));
            return tA < tB;
        }
        case SortOption::TITLE_DESC: {
            std::string tA = toLowerUtf8(cleanTitle(a.title));
            std::string tB = toLowerUtf8(cleanTitle(b.title));
            return tA > tB;
        }
        case SortOption::SIZE_ASC: {
            uint64_t sA = parseSizeToBytes(a.size);
            uint64_t sB = parseSizeToBytes(b.size);
            if (sA != sB) return sA < sB;
            return toLowerUtf8(cleanTitle(a.title)) < toLowerUtf8(cleanTitle(b.title));
        }
        case SortOption::SIZE_DESC: {
            uint64_t sA = parseSizeToBytes(a.size);
            uint64_t sB = parseSizeToBytes(b.size);
            if (sA != sB) return sA > sB;
            return toLowerUtf8(cleanTitle(a.title)) < toLowerUtf8(cleanTitle(b.title));
        }
        case SortOption::YEAR_DESC: {
            int yA = parseYear(a.year);
            int yB = parseYear(b.year);
            if (yA != yB) return yA > yB;
            return toLowerUtf8(cleanTitle(a.title)) < toLowerUtf8(cleanTitle(b.title));
        }
        case SortOption::YEAR_ASC: {
            int yA = parseYear(a.year);
            int yB = parseYear(b.year);
            // Treat 0 (unknown year) as highest when ascending so known older years appear first
            if (yA == 0) yA = 9999;
            if (yB == 0) yB = 9999;
            if (yA != yB) return yA < yB;
            return toLowerUtf8(cleanTitle(a.title)) < toLowerUtf8(cleanTitle(b.title));
        }
        case SortOption::DEFAULT:
        default:
            return false;
    }
}

} // namespace catalog
