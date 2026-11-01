// Version comparison shared by the game card, game manager, home tile and
// file selection (source/catalog/game_version.hpp), plus the version step of
// release ranking (source/catalog/release_picker.hpp).
//
//   g++ -std=c++17 -I source pctest/test_game_version.cpp -o pctest/test_game_version.exe
//
// Exit code = number of failed checks.
#include "catalog/game_version.hpp"
#include "catalog/release_picker.hpp"

#include <cstdio>

using catalog::VersionCmp;

static int g_fail = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    using namespace catalog;

    // trimVersion
    check(trimVersion(" v1.0.3 ") == "1.0.3", "trim: spaces and v");
    check(trimVersion("Ver.1.0.3") == "1.0.3", "trim: Ver.");
    check(trimVersion("Version 2.1") == "2.1", "trim: Version + space");
    check(trimVersion("vibe") == "vibe", "trim: word starting with v kept");

    // isPlausibleVersion: values seen in the RU catalog
    check(isPlausibleVersion("1.0.3"), "plausible: 1.0.3");
    check(isPlausibleVersion("1.0.3b"), "plausible: 1.0.3b");
    check(isPlausibleVersion("1.0"), "plausible: 1.0");
    check(isPlausibleVersion("v1.2.0"), "plausible: v1.2.0");
    check(!isPlausibleVersion(""), "junk: empty");
    check(!isPlausibleVersion("09.09.2026"), "junk: date dd.mm.yyyy");
    check(!isPlausibleVersion("2026.09.09"), "junk: date yyyy.mm.dd");
    check(!isPlausibleVersion("RC1 1d12337"), "junk: RC + hash");
    check(!isPlausibleVersion("NES: 9.1.0 / SNES: 6.0.0"), "junk: compound");
    check(!isPlausibleVersion("core v37.02"), "junk: leading word");
    check(!isPlausibleVersion("Prerelease 2"), "junk: Prerelease");
    check(!isPlausibleVersion("1234567890"), "junk: long number");

    // compareVersions
    check(compareVersions("1.0.10", "1.0.9") == 1, "cmp: 1.0.10 > 1.0.9");
    check(compareVersions("1.0", "1.0.0") == 0, "cmp: 1.0 == 1.0.0");
    check(compareVersions("1.0.3b", "1.0.3") == 0, "cmp: suffix ignored");
    check(compareVersions("1.2.0", "1.10.0") == -1, "cmp: 1.2 < 1.10");
    check(compareVersions("09.09.2026", "1.0.0") == 2, "cmp: date is unknown");

    // compareRelease: release vs installed
    check(compareRelease("1.0.3", 196608, "1.0.2", 131072) == VersionCmp::Newer, "rel: newer string");
    check(compareRelease("1.2.1", 65536, "1.2.1", 0) == VersionCmp::Same,
          "rel: equal strings win over update numbers (was a false 'newer')");
    check(compareRelease("1.0.0", 0, "1.0.2", 131072) == VersionCmp::Older, "rel: installed is newer");
    check(compareRelease("", 196608, "", 131072) == VersionCmp::Newer, "rel: no strings, update numbers");
    check(compareRelease("09.09.2026", 196608, "1.0.2", 196608) == VersionCmp::Same, "rel: junk string -> numbers");
    check(compareRelease("", 0, "1.0.2", 0) == VersionCmp::Unknown, "rel: nothing known");
    check(compareRelease("", 65536, "1.0.0", 0) == VersionCmp::Newer, "rel: update, nothing installed");

    check(formatIsoDate("2026-09-01") == "01.09.2026", "date: iso");
    check(formatIsoDate("bad") == "bad", "date: passthrough");

    // Ranking: the newer version string beats a newer update number and the
    // registration date.
    {
        ReleaseInfo a; a.title = "Game [NSP]"; a.topic_id = "1"; a.version = "1.0.3"; a.updateVersion = 65536; a.registered = 1;
        ReleaseInfo b; b.title = "Game [NSP]"; b.topic_id = "2"; b.version = "1.0";   a.registered = 1; b.registered = 9;
        check(rankReleases({b, a}).front() == 1, "rank: 1.0.3 before 1.0 registered later");
        ReleaseInfo c; c.title = "Game [NSP]"; c.topic_id = "3"; c.version = "09.09.2026"; c.registered = 99;
        check(rankReleases({a, c}).front() == 1, "rank: junk version falls back to date");
        ReleaseInfo m; m.title = "Game (mod) [NSP]"; m.topic_id = "4"; m.version = "9.0.0";
        check(rankReleases({m, a}).front() == 1, "rank: mod with a higher version stays below the game");
    }

    std::printf("\n%d failed\n", g_fail);
    return g_fail;
}
