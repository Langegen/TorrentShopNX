// titledb parsers (source/catalog/titledb.cpp) against real files:
// versions.txt from 16BitWonder/nx-versions, versions.json from blawar/titledb.
//
//   g++ -std=c++17 -DTITLEDB_NO_APP -I source pctest/test_titledb.cpp source/catalog/titledb.cpp -o test_titledb.exe
//   test_titledb.exe <versions.txt> <versions.json>
//
// Exit code = number of failed checks.
#include "catalog/titledb.h"

#include <cstdio>
#include <fstream>
#include <sstream>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    // Inline samples
    auto v = titledb::parseVersionsTxt("id|rightsId|version\n0100152000022800|0000|1441792\r\n0100000000000816|0000|\n");
    check(v.size() == 1 && v[0x0100152000022800ULL] == 1441792, "versions.txt: CRLF, header and empty version");
    auto nx = titledb::parseVersionsTxt("id|version\n0100152000022800|1441792\n0100152000022001|65536\n");
    check(nx.size() == 2 && nx[0x0100152000022800ULL] == 1441792, "nx-versions: id|version");

    auto d = titledb::parseVersionsJson("{\n \"0100152000022000\": {\"65536\": \"2017-05-01\", \"1441792\": \"2026-09-01\"},\n \"010000000eef0000\": {}\n}");
    check(d.size() == 2, "versions.json: two titles");
    check(d[0x0100152000022000ULL].size() == 2 && d[0x0100152000022000ULL][1].first == 1441792 &&
          d[0x0100152000022000ULL][1].second == 20260901, "versions.json: version and date");
    check(titledb::formatDate("2026-09-01") == "01.09.2026", "formatDate");
    check(titledb::updateOrdinal(1441792) == 22, "update ordinal");

    if (argc >= 3) {
        auto all = titledb::parseVersionsTxt(slurp(argv[1]));
        std::printf("      versions.txt: %zu entries\n", all.size());
        check(all.size() > 20000, "real versions.txt parsed");
        check(all[0x0100152000022800ULL] >= 1441792, "real: MK8D patch >= 1441792");
        auto dates = titledb::parseVersionsJson(slurp(argv[2]));
        std::printf("      versions.json: %zu titles\n", dates.size());
        check(dates.size() > 5000, "real versions.json parsed");
        bool found = false;
        for (auto& vd : dates[0x0100152000022000ULL]) {
            if (vd.first == 1441792 && vd.second == 20260901) found = true;
        }
        check(found, "real: MK8D update 22 dated 2026-09-01");
    }
    std::printf("%s: %d failed\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail;
}
