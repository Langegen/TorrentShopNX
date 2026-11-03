#pragma once

// Game versions from the catalog against the installed game.
//
// The catalog builder fills two fields per release:
//   version         - game version in the release ("1.0.3"), from the topic
//                     or the file names; sometimes junk ("09.09.2026",
//                     "RC1 1d12337", "NES: 9.1.0 / SNES: 6.0.0");
//   update_version  - newest update package in the torrent, Nintendo's
//                     number N << 16 (65536 = 1st update), 0 = unknown.
// The installed side has the NACP display version and the installed update
// number. One rule decides "newer" on every screen (game card, game manager,
// home tile, file selection):
//   1. both version strings usable -> compare them part by part;
//   2. else the release's update number, when known, against the installed one;
//   3. else unknown.
//
// Header-only and free of app dependencies so the PC tests can include it.

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace catalog {

// Nintendo numbers updates as N << 16: 65536 = 1st update, 1441792 = 22nd.
inline uint32_t updateOrdinal(uint32_t version) { return version >> 16; }

// " v1.0.3 ", "Ver.1.0.3", "Version 1.0.3" -> "1.0.3"
inline std::string trimVersion(std::string v) {
    auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!v.empty() && isSpace(v.front())) v.erase(v.begin());
    while (!v.empty() && isSpace(v.back())) v.pop_back();
    // Longest prefix first
    static const char* kPrefixes[] = {"version", "ver.", "ver", "v.", "v"};
    for (const char* p : kPrefixes) {
        size_t n = std::char_traits<char>::length(p);
        if (v.size() <= n) continue;
        bool match = true;
        for (size_t i = 0; i < n; ++i) {
            if (std::tolower(static_cast<unsigned char>(v[i])) != p[i]) { match = false; break; }
        }
        if (!match) continue;
        size_t j = n;
        while (j < v.size() && isSpace(v[j])) ++j;
        if (j < v.size() && std::isdigit(static_cast<unsigned char>(v[j]))) {
            v.erase(0, j);
            break;
        }
    }
    return v;
}

namespace detail {

// Leading "1.0.10" of a string as numbers; stops at the first character that
// is not a digit or a dot followed by a digit ("1.0.3b", "1.2 (rev)").
inline bool versionParts(const std::string& s, std::vector<unsigned long>& out) {
    out.clear();
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        unsigned long n = 0;
        size_t digits = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            if (++digits > 9) return false;   // a hash or a build stamp, not a version
            n = n * 10 + static_cast<unsigned long>(s[i++] - '0');
        }
        out.push_back(n);
        if (i + 1 < s.size() && s[i] == '.' && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            ++i;
            continue;
        }
        break;
    }
    return !out.empty();
}

} // namespace detail

// A version string worth comparing: starts with a number, is not a date
// ("09.09.2026", "2026.09.09") and the number part is not followed by more
// numbers ("NES: 9.1.0 / SNES: 6.0.0" fails the first check anyway).
inline bool isPlausibleVersion(const std::string& raw) {
    std::string v = trimVersion(raw);
    std::vector<unsigned long> p;
    if (!detail::versionParts(v, p)) return false;
    if (p[0] >= 1000) return false;                                    // 2026.09.09
    if (p.size() == 3 && p[2] >= 1990 && p[2] <= 2100 && p[1] >= 1 && p[1] <= 12 && p[0] <= 31)
        return false;                                                  // 09.09.2026
    return true;
}

// Compares "1.0.10" with "1.0.9" part by part: -1, 0 or 1; 2 when either is
// not a plausible version. Missing parts count as 0 ("1.0" == "1.0.0").
inline int compareVersions(const std::string& a, const std::string& b) {
    if (!isPlausibleVersion(a) || !isPlausibleVersion(b)) return 2;
    std::vector<unsigned long> pa, pb;
    detail::versionParts(trimVersion(a), pa);
    detail::versionParts(trimVersion(b), pb);
    size_t n = pa.size() > pb.size() ? pa.size() : pb.size();
    pa.resize(n, 0);
    pb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

enum class VersionCmp { Newer, Same, Older, Unknown };

// Is the release (version string, update number) newer than the installed
// game (NACP display version, installed update number)? See the rule above.
// Works for two releases as well.
// `releaseUpdateKnown`: the update number is known even when it is 0 (the
// release's files say it carries the base game without an update).
inline VersionCmp compareRelease(const std::string& releaseVersion, uint32_t releaseUpdate,
                                 bool releaseUpdateKnown,
                                 const std::string& installedVersion, uint32_t installedUpdate) {
    int c = compareVersions(releaseVersion, installedVersion);
    if (c != 2) return c > 0 ? VersionCmp::Newer : (c == 0 ? VersionCmp::Same : VersionCmp::Older);
    if (releaseUpdate != 0 || releaseUpdateKnown) {
        if (releaseUpdate > installedUpdate) return VersionCmp::Newer;
        return releaseUpdate == installedUpdate ? VersionCmp::Same : VersionCmp::Older;
    }
    return VersionCmp::Unknown;
}

inline VersionCmp compareRelease(const std::string& releaseVersion, uint32_t releaseUpdate,
                                 const std::string& installedVersion, uint32_t installedUpdate) {
    return compareRelease(releaseVersion, releaseUpdate, false, installedVersion, installedUpdate);
}

// Version of one game inside a release (see catalog::titleVersion).
struct TitleVersion {
    std::string version;         // "1.0.3", "" = unknown
    uint32_t update = 0;         // newest update in the release, N << 16
    bool updateKnown = false;    // `update` is known even when 0
    bool known() const { return isPlausibleVersion(version) || update != 0 || updateKnown; }
};

inline VersionCmp compareRelease(const TitleVersion& release,
                                 const std::string& installedVersion, uint32_t installedUpdate) {
    return compareRelease(release.version, release.update, release.updateKnown, installedVersion, installedUpdate);
}

// Version of game `appId` in release `g` (a Game: `titles`, `version`,
// `update_version`). A release can hold several games, each with its own
// version; the release-wide `version` / `update_version` describe only one
// of them, so they are used only when the release holds a single game.
//   - `titles` lists the game: its own update number and version string;
//     the release-wide fields fill gaps only for a single-game release;
//   - `titles` is empty (older catalog): the release-wide fields;
//   - `titles` lists other games only: unknown.
template <class GameT>
TitleVersion titleVersionOf(const GameT& g, uint64_t appId) {
    TitleVersion out;
    if (g.titles.empty()) {
        out.version = g.version;
        out.update = g.update_version;
        return out;
    }
    const bool single = g.titles.size() == 1;
    for (const auto& t : g.titles) {
        if (t.app_id != appId) continue;
        out.version = t.version;
        out.update = t.update_version;
        out.updateKnown = t.update_known;
        if (single) {
            if (!isPlausibleVersion(out.version)) out.version = g.version;
            if (!out.updateKnown && out.update == 0) out.update = g.update_version;
        }
        return out;
    }
    return out;
}

// "2026-09-01" -> "01.09.2026"; anything else is returned unchanged.
inline std::string formatIsoDate(const std::string& iso) {
    if (iso.size() == 10 && iso[4] == '-' && iso[7] == '-') {
        return iso.substr(8, 2) + "." + iso.substr(5, 2) + "." + iso.substr(0, 4);
    }
    return iso;
}

} // namespace catalog
