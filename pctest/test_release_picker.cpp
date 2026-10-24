// Release choice for the game manager (source/catalog/release_picker.hpp).
//
//   g++ -std=c++17 -I source pctest/test_release_picker.cpp -o pctest/test_release_picker.exe
//
// Exit code = number of failed checks.
#include "catalog/release_picker.hpp"

#include <cstdio>

using catalog::ReleaseInfo;

static int g_fail = 0;

static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string pick(const std::vector<ReleaseInfo>& r) {
    return r[catalog::rankReleases(r).front()].topic_id;
}

int main() {
    // Real catalog data, 2026-10: both releases carry title_id 0100152000022000.
    ReleaseInfo official{"Mario Kart 8 Deluxe (MK8D) + DLC Booster Course Pass, \xD0\xB2\xD0\xBE\xD0\xBB\xD0\xBD\xD0\xB0 6 [NSZ][RUS/Multi11]",
                         "5757587", false, 20260902062256LL, 413};
    ReleaseInfo ctgp{"Mario Kart 8 CTGP Deluxe Mod [NSZ+MSP][RUS/Multi11]",
                     "6761551", false, 20251022183541LL, 65};
    check(pick({official, ctgp}) == "5757587", "MK8D: official over CTGP mod");
    check(pick({ctgp, official}) == "5757587", "MK8D: catalog order does not matter");

    // A mod re-registered later still loses to the plain game.
    ReleaseInfo ctgpNew = ctgp;
    ctgpNew.registered = 20261001000000LL;
    check(pick({ctgpNew, official}) == "5757587", "plain game beats a newer mod");

    // Two plain uploads: the more recently registered torrent wins.
    ReleaseInfo oldUp{"Game X [NSP][ENG]", "6900000", false, 20250101000000LL, 900};
    ReleaseInfo newUp{"Game X [NSZ][ENG]", "6100000", false, 20260501000000LL, 3};
    check(pick({oldUp, newUp}) == "6100000", "newer registration beats more seeds and topic id");

    // Known update versions beat the registration date.
    {
        ReleaseInfo older{"Game U [NSP]", "300", false, 20260101000000LL, 5};
        ReleaseInfo newer{"Game U [NSZ]", "301", false, 20250101000000LL, 5};
        older.updateVersion = 655360;
        newer.updateVersion = 1114112;
        check(pick({older, newer}) == "301", "newer update package beats newer registration");
        newer.updateVersion = 0;   // unknown on one side: date decides
        check(pick({older, newer}) == "300", "unknown update version falls back to date");
    }

    // No stats: seeds, then topic id.
    ReleaseInfo a{"Game Y [NSP]", "100", false, 0, 5};
    ReleaseInfo b{"Game Y [NSZ]", "200", false, 0, 5};
    check(pick({a, b}) == "200", "no stats: higher topic id");
    b.seeds = 1;
    check(pick({a, b}) == "100", "no stats: more seeds first");

    // Homebrew (NRO) build loses to the game.
    ReleaseInfo nro{"Game Z [NRO][ENG]", "999", true, 20261010000000LL, 50};
    ReleaseInfo nsp{"Game Z [NSP][ENG]", "111", false, 20240101000000LL, 1};
    check(pick({nro, nsp}) == "111", "homebrew build loses");

    // Mod detection: words only, fan translations are not mods.
    using catalog::isModdedRelease;
    check(isModdedRelease("Mario Kart 8 CTGP Deluxe Mod [NSZ+MSP]"), "CTGP Mod is a mod");
    check(isModdedRelease("Mario Kart 64 (\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82 SpaghettiKart) [NSP][ENG]"), "(порт ...) is a port");
    check(isModdedRelease("Sonic R (\xD0\x9F\xD0\xBE\xD1\x80\xD1\x82) [NRO]"), "(Порт) upper-case Cyrillic");
    check(!isModdedRelease("Captain Tsubasa 2 + 5 DLC [NSZ][RUS (Mod.)/ENG]"), "RUS (Mod.) is a fan translation");
    check(!isModdedRelease("Call of Duty Modern Warfare [NSP]"), "Modern is not mod");
    check(!isModdedRelease("Portal Companion Collection [NSP]"), "Portal is not port");
    check(!isModdedRelease("\xD0\xA1\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82 2026 [NSZ]"), "Спорт is not порт");
    check(!isModdedRelease("Mario Kart 8 Deluxe [NSZ]"), "plain title");
    check(!isModdedRelease("Ghostrunner + 7 DLC [NSP][RUS (Voice Mod.)]"), "(Voice Mod.) is a dub");
    check(!isModdedRelease("Resident Evil 4 [NSP][RUS (Voice+Text Mod.)]"), "(Voice+Text Mod.) is a dub");
    check(!isModdedRelease("Don't Starve Together [NSZ][RUS (+Mod.)/Multi7]"), "(+Mod.) is a translation");

    // Wrong title_id in the catalog: "Don't Starve" filed under Crash's ID and
    // re-registered more recently. The installed name decides.
    {
        const std::string installed = "Crash Bandicoot N. Sane Trilogy";
        ReleaseInfo crash{"Crash Bandicoot N. Sane Trilogy [NSP][RUS (Voice)/ENG]", "5622623", false, 20181011205928LL, 40};
        ReleaseInfo starve{"Don't Starve Together / Dont Starve: Nintendo Switch Edition [NSZ][RUS (+Mod.)/Multi7]",
                           "5677357", false, 20260715095830LL, 90};
        crash.nameMatch = catalog::releaseNameMatches(installed, crash.title);
        starve.nameMatch = catalog::releaseNameMatches(installed, starve.title);
        check(crash.nameMatch && !starve.nameMatch, "name match: Crash yes, Don't Starve no");
        check(pick({starve, crash}) == "5622623", "wrong title_id entry loses to the matching name");
    }
    {
        const std::string installed = "Mario Kart 8 Deluxe";
        check(catalog::releaseNameMatches(installed, official.title) &&
              catalog::releaseNameMatches(installed, ctgp.title), "MK8D: both releases match the name");
    }

    std::printf("%s: %d failed\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
