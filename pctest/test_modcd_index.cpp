// Tests for modcd::Index (source/catalog/modcd_index.*): cached load,
// 12-hour fail cooldown (including when bundled/cached data is already loaded),
// and retry after cooldown expiry.
//
// Build and run from the repo root:  powershell -File pctest\build_homebrew.ps1

#include "../source/catalog/modcd_index.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n"; \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        ++g_checks;                                                                                        \
        const auto va = (a);                                                                               \
        const auto vb = (b);                                                                               \
        if (!(va == vb)) {                                                                                 \
            ++g_failed;                                                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #a " == " #b << "\n     got: " << va \
                      << "\n     expected: " << vb << "\n";                                                \
        }                                                                                                  \
    } while (0)

static void writeFile(const std::string& path, const std::string& body) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
}

int main() {
    const std::string tmp = "pctest/.tmp_modcd_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const std::string sampleJson = R"({
        "format": 1,
        "titles": {
            "0100000000010000": {
                "mods": 2,
                "versions": [65536],
                "types": ["gfx"]
            }
        }
    })";

    // Case 1: Cached/bundled data already loaded (loaded() == true). A failed
    // refresh writes modcd_last_fail, and subsequent refreshes within 12h must
    // NOT hit the network — even across a fresh Index instance (app restart).
    {
        const std::string dir = tmp + "/case1";
        writeFile(dir + "/modcd.json", sampleJson);

        long long clockSec = 1000000;
        int fetches = 0;
        auto failFetch = [&](const std::string&, const std::string&) -> homebrew::FetchResult {
            ++fetches;
            return {404, "", ""};
        };

        modcd::Index idx1(dir, "https://example.invalid/modcd.json", failFetch, [&] { return clockSec; });
        CHECK(idx1.loadCached());
        CHECK(idx1.loaded());
        CHECK_EQ(idx1.size(), size_t(1));

        // First refresh fails and records modcd_last_fail.
        CHECK(idx1.refresh(false));
        CHECK_EQ(fetches, 1);
        CHECK(fs::exists(dir + "/modcd_last_fail"));

        // Second refresh 1 hour later on same instance: cooldown suppresses fetch.
        clockSec += 3600;
        CHECK(idx1.refresh(false));
        CHECK_EQ(fetches, 1);

        // Fresh instance after loadCached() (simulating app restart): still suppresses fetch.
        modcd::Index idx2(dir, "https://example.invalid/modcd.json", failFetch, [&] { return clockSec; });
        CHECK(idx2.loadCached());
        CHECK(idx2.loaded());
        CHECK(idx2.refresh(false));
        CHECK_EQ(fetches, 1);

        // After >= 12 hours, refresh retries the network.
        clockSec += 12 * 3600;
        CHECK(idx2.refresh(false));
        CHECK_EQ(fetches, 2);

        // force=true bypasses cooldown immediately.
        CHECK(idx2.refresh(true));
        CHECK_EQ(fetches, 3);
    }

    // Case 2: Successful refresh clears modcd_last_fail and updates index.
    {
        const std::string dir = tmp + "/case2";
        writeFile(dir + "/modcd.json", sampleJson);
        writeFile(dir + "/modcd_last_fail", "1000000");

        long long clockSec = 1000100;
        int fetches = 0;
        const std::string updatedJson = R"({
            "format": 1,
            "titles": {
                "0100000000010000": { "mods": 2, "versions": [65536] },
                "01006A800016E000": { "mods": 1, "versions": [0] }
            }
        })";
        auto okFetch = [&](const std::string&, const std::string&) -> homebrew::FetchResult {
            ++fetches;
            return {200, updatedJson, "etag-2"};
        };

        modcd::Index idx(dir, "https://example.invalid/modcd.json", okFetch, [&] { return clockSec; });
        CHECK(idx.loadCached());
        CHECK(idx.refresh(true));
        CHECK_EQ(fetches, 1);
        CHECK_EQ(idx.size(), size_t(2));
        CHECK(!fs::exists(dir + "/modcd_last_fail"));

        modcd::TitleMods tm;
        CHECK(idx.find(0x01006A800016E000ULL, tm));
        CHECK_EQ(tm.mods, 1);
    }

    fs::remove_all(tmp);
    std::cout << (g_failed ? "FAILED " : "OK ") << (g_checks - g_failed) << "/" << g_checks << " modcd checks\n";
    return g_failed ? 1 : 0;
}
