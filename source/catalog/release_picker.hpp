#pragma once

// Choosing one catalog release when several share a title ID.
//
// The catalog often holds more than one rutracker release of the same game:
// the official dump that the uploader re-registers after every update, plus
// mods (Mario Kart 8 CTGP), ports or older uploads. The game manager used to
// take whichever came first in the catalog, and that order changes (the diff
// update puts recently touched entries on top), so "Update" could open an
// old release without the latest patch.
//
// Ranking, best first:
//   0. the release title shares a word with the installed game's name (guards
//      against catalog entries carrying a wrong title_id: "Don't Starve" filed
//      under Crash Bandicoot's ID);
//   1. not a mod / port / homebrew build of the game;
//   1b. the newer update package, when the catalog knows it for both
//       (update_version, filled by the catalog builder);
//   2. most recently (re-)registered torrent: rutracker re-registers a release
//      when the uploader replaces it, so a newer date means a newer update;
//   3. more seeders;
//   4. higher topic id (newer topic).
//
// Header-only and free of app dependencies so the PC tests can include it.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

namespace catalog {

struct ReleaseInfo {
    std::string title;
    std::string topic_id;
    bool homebrew = false;          // NRO build (isHomebrewGame)
    int64_t registered = 0;         // YYYYMMDDHHMMSS from game stats, 0 = unknown
    int32_t seeds = 0;
    bool nameMatch = true;          // shares a word with the installed game's name
    uint32_t updateVersion = 0;     // newest update package in the torrent, 0 = unknown
};

namespace detail {

// Lower-cases ASCII and the basic Cyrillic range (А-Я, Ё) of a UTF-8 string.
inline std::string lowerUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
            continue;
        }
        if (c == 0xD0 && i + 1 < s.size()) {
            unsigned char n = static_cast<unsigned char>(s[i + 1]);
            if (n >= 0x90 && n <= 0x9F) {          // А-П -> а-п
                out += static_cast<char>(0xD0); out += static_cast<char>(n + 0x20); ++i; continue;
            }
            if (n >= 0xA0 && n <= 0xAF) {          // Р-Я -> р-я
                out += static_cast<char>(0xD1); out += static_cast<char>(n - 0x20); ++i; continue;
            }
            if (n == 0x81) {                       // Ё -> ё
                out += static_cast<char>(0xD1); out += static_cast<char>(0x91); ++i; continue;
            }
        }
        out += static_cast<char>(c);
    }
    return out;
}

// Splits into words: ASCII letters/digits and any non-ASCII (UTF-8) bytes.
inline std::vector<std::string> words(const std::string& lower) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : lower) {
        bool wordChar = (c >= 0x80) || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (wordChar) {
            cur += static_cast<char>(c);
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace detail

// Drops every "( ... )" group that mentions "mod.": rutracker marks fan
// translations and dubs that way ("RUS (Mod.)", "(Voice Mod.)",
// "(Voice+Text Mod.)", "(+Mod.)"); they are the game itself, not a mod.
inline std::string stripTranslationMarks(const std::string& lower) {
    std::string out;
    out.reserve(lower.size());
    size_t i = 0;
    while (i < lower.size()) {
        if (lower[i] == '(') {
            size_t close = lower.find(')', i);
            if (close != std::string::npos &&
                lower.substr(i, close - i + 1).find("mod.") != std::string::npos) {
                i = close + 1;
                continue;
            }
        }
        out += lower[i++];
    }
    return out;
}

// Significant words (3+ bytes) of a name, lower-cased; tags in [] dropped.
inline std::vector<std::string> nameWords(const std::string& name) {
    std::string t = detail::lowerUtf8(name);
    std::string noTags;
    bool inBracket = false;
    for (char c : t) {
        if (c == '[') inBracket = true;
        else if (c == ']') inBracket = false;
        else if (!inBracket) noTags += c;
    }
    std::vector<std::string> out;
    for (auto& w : detail::words(noTags)) {
        if (w.size() >= 3) out.push_back(w);
    }
    return out;
}

// True when the release title shares at least one significant word with the
// installed game's name.
inline bool releaseNameMatches(const std::string& installedName, const std::string& releaseTitle) {
    auto a = nameWords(installedName);
    auto b = nameWords(releaseTitle);
    for (const auto& x : a) {
        for (const auto& y : b) {
            if (x == y) return true;
        }
    }
    return false;
}

// True for releases that are not the plain game: gameplay mods, ports,
// romhacks. Fan translations and dubs ("(Voice Mod.)") don't count.
inline bool isModdedRelease(const std::string& title) {
    std::string t = stripTranslationMarks(detail::lowerUtf8(title));
    static const char* kMarkers[] = {
        "mod", "mods", "modded", "ctgp", "romhack", "port", "ports",
        "\xD0\xBC\xD0\xBE\xD0\xB4",                         // мод
        "\xD0\xBC\xD0\xBE\xD0\xB4\xD1\x8B",                 // моды
        "\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82",                 // порт
        "\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82\xD1\x8B",         // порты
    };
    for (const auto& w : detail::words(t)) {
        for (const char* m : kMarkers) {
            if (w == m) return true;
        }
    }
    return false;
}

// Strict "a is a better pick than b".
inline bool releaseBetter(const ReleaseInfo& a, const ReleaseInfo& b) {
    if (a.nameMatch != b.nameMatch) return a.nameMatch;
    bool aPlain = !a.homebrew && !isModdedRelease(a.title);
    bool bPlain = !b.homebrew && !isModdedRelease(b.title);
    if (aPlain != bPlain) return aPlain;
    // Both releases say which update they carry: the newer one wins.
    if (a.updateVersion && b.updateVersion && a.updateVersion != b.updateVersion)
        return a.updateVersion > b.updateVersion;
    if (a.registered != b.registered) return a.registered > b.registered;
    if (a.seeds != b.seeds) return a.seeds > b.seeds;
    long long at = std::atoll(a.topic_id.c_str());
    long long bt = std::atoll(b.topic_id.c_str());
    return at > bt;
}

// Indices of `releases` sorted best first.
inline std::vector<size_t> rankReleases(const std::vector<ReleaseInfo>& releases) {
    std::vector<size_t> idx(releases.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) {
        return releaseBetter(releases[x], releases[y]);
    });
    return idx;
}

} // namespace catalog
