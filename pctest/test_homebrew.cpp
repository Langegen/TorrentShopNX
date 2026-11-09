// Tests for homebrew::* (source/catalog/homebrew_package.*): manifests, path
// rules, versions, both sources and their merge, libget-compatible
// install/removal plans.
//
// Fixtures (pctest/homebrew_fixtures):
//   path_rules.txt          <allowed|warn|forbidden><TAB><path>
//   manifests/*.install     real manifests from switch.cdn.fortheusers.org (09.11.2026)
//   fortheusers_repo.json   8 real packages from repo.json (09.11.2026)
//   homebrew_own.json       our index: merges, hidden, broken entries
//
// Build and run from the repo root:  powershell -File pctest\build_homebrew.ps1
// Extra smoke run over a full repo.json:  pctest\test_homebrew.exe pctest\homebrew_fixtures <repo.json>

#include "../source/catalog/homebrew_package.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace homebrew;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            ++g_failed;                                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";       \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        ++g_checks;                                                                          \
        const auto va = (a);                                                                 \
        const auto vb = (b);                                                                 \
        if (!(va == vb)) {                                                                   \
            ++g_failed;                                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #a " == " #b         \
                      << "\n     got: " << va << "\n     expected: " << vb << "\n";          \
        }                                                                                    \
    } while (0)

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "cannot read " << path << "\n";
        ++g_failed;
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const char* verdictName(PathVerdict v) {
    switch (v) {
        case PathVerdict::Allowed: return "allowed";
        case PathVerdict::Warn: return "warn";
        case PathVerdict::Forbidden: return "forbidden";
    }
    return "?";
}

static const Package* find(const std::vector<Package>& list, const std::string& name) {
    for (const auto& p : list)
        if (p.name == name) return &p;
    return nullptr;
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// ---------------------------------------------------------------------------

static void testPathRules(const std::string& dir) {
    std::istringstream is(readFile(dir + "/path_rules.txt"));
    std::string line;
    int rows = 0;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string want = line.substr(0, tab);
        const std::string path = line.substr(tab + 1);
        const PathCheck c = checkInstallPath(path);
        ++rows;
        ++g_checks;
        if (want != verdictName(c.verdict)) {
            ++g_failed;
            std::cerr << "FAIL path rule: " << path << "\n     got: " << verdictName(c.verdict) << " ("
                      << c.reason << ")\n     expected: " << want << "\n";
        }
    }
    CHECK(rows >= 40);
}

static void testNormalize() {
    CHECK_EQ(normalizeRelPath("sdmc:/switch/a.nro"), std::string("switch/a.nro"));
    CHECK_EQ(normalizeRelPath("SDMC:/switch//a.nro"), std::string("switch/a.nro"));
    CHECK_EQ(normalizeRelPath("  /switch\\x\\a.nro \r"), std::string("switch/x/a.nro"));
    CHECK_EQ(normalizeRelPath("switch/../a"), std::string(""));
    CHECK_EQ(normalizeRelPath("./a"), std::string(""));
    CHECK_EQ(normalizeRelPath("c:/a"), std::string(""));
    CHECK_EQ(normalizeRelPath(""), std::string(""));
    CHECK_EQ(normalizeRelPath("/"), std::string(""));
}

static void testManifests(const std::string& dir) {
    std::vector<ManifestEntry> m;
    std::string err;

    // Checkpoint: one .nro, nothing else -> simple app.
    CHECK(parseManifest(readFile(dir + "/manifests/Checkpoint.install"), m, err));
    CHECK_EQ(m.size(), size_t(1));
    FileAnalysis a = analyzeFiles(m);
    CHECK_EQ(a.kinds, unsigned(KindApp));
    CHECK(a.simple);
    CHECK(a.worst == PathVerdict::Allowed);

    // sys-clk: app + overlay + sysmodule; README.md in the SD root -> warn.
    CHECK(parseManifest(readFile(dir + "/manifests/sys-clk.install"), m, err));
    CHECK_EQ(m.size(), size_t(6));
    a = analyzeFiles(m);
    CHECK_EQ(a.kinds, unsigned(KindApp | KindOverlay | KindSysmodule));
    CHECK_EQ(a.sysmodule_tids.size(), size_t(1));
    if (!a.sysmodule_tids.empty()) CHECK_EQ(a.sysmodule_tids[0], std::string("00FF0000636C6BFF"));
    CHECK(a.worst == PathVerdict::Warn);
    CHECK_EQ(a.worst_path, std::string("README.md"));
    CHECK(!a.simple);

    // Status Monitor: overlay + config template.
    CHECK(parseManifest(readFile(dir + "/manifests/Status-Monitor-Overlay.install"), m, err));
    a = analyzeFiles(m);
    CHECK_EQ(a.kinds, unsigned(KindOverlay));
    CHECK(!a.simple);
    CHECK(a.worst == PathVerdict::Allowed);

    // MissionControl: sysmodule + exefs patches -> warn.
    CHECK(parseManifest(readFile(dir + "/manifests/MissionControl.install"), m, err));
    CHECK(m.size() > 50);
    a = analyzeFiles(m);
    CHECK(a.kinds & KindSysmodule);
    CHECK(a.worst == PathVerdict::Warn);

    // Hekate: writes the bootloader -> forbidden.
    CHECK(parseManifest(readFile(dir + "/manifests/Hekate.install"), m, err));
    a = analyzeFiles(m);
    CHECK(a.worst == PathVerdict::Forbidden);

    // Unknown ops and blank lines are skipped, CRLF handled.
    CHECK(parseManifest("U: switch/a/a.nro\r\n\r\nX: whatever\r\nG: config/a/a.ini\r\nE: switch/a/data.bin\r\nL: switch/a/log.txt\r\n", m, err));
    CHECK_EQ(m.size(), size_t(4));
    if (m.size() == 4) {
        CHECK(m[0].op == ManifestOp::Update);
        CHECK(m[1].op == ManifestOp::Get);
        CHECK(m[2].op == ManifestOp::Extract);
        CHECK(m[3].op == ManifestOp::Local);
        CHECK_EQ(formatManifest(m), std::string("U: switch/a/a.nro\nG: config/a/a.ini\nE: switch/a/data.bin\nL: switch/a/log.txt\n"));
    }

    // Unsafe path fails the whole manifest; empty manifest fails.
    CHECK(!parseManifest("U: switch/a.nro\nU: ../x\n", m, err));
    CHECK(m.empty());
    CHECK(!parseManifest("\n\n", m, err));
}

static void testVersions() {
    CHECK(compareVersions("1.2.0", "1.2.0") == 0);
    CHECK(compareVersions("v1.2.0", "1.2") == 0);
    CHECK(compareVersions("1.10.0", "1.9.9") > 0);
    CHECK(compareVersions("1.9", "1.10") < 0);
    CHECK(compareVersions("2.1.0", "2.0.1-fix") > 0);
    CHECK(compareVersions("2.0.1-fix", "2.0.1") == 0);
    CHECK(compareVersions("912", "911") > 0);
    CHECK(compareVersions("nx_0.2.3", "0.2.4") < 0);
    // dd/mm/yyyy dates (JKSV): 11 March 2026 > 12 Feb 2025, and day/month not swapped.
    CHECK(compareVersions("11/03/2026", "12/02/2025") > 0);
    CHECK(compareVersions("03/12/2025", "12/02/2025") > 0);
    CHECK(compareVersions("2026-03-11", "11/03/2026") == 0);
    // No digits: only equality is known.
    CHECK(compareVersions("Untold", "Untold") == 0);
    CHECK(compareVersions("Untold", "Other") != 0);
}

static void testUrlsAndDates() {
    CHECK_EQ(githubRepoFromUrl("https://github.com/XorTroll/Goldleaf/releases"), std::string("xortroll/goldleaf"));
    CHECK_EQ(githubRepoFromUrl("https://github.com/retronx-team/sys-clk/"), std::string("retronx-team/sys-clk"));
    CHECK_EQ(githubRepoFromUrl("https://www.github.com/a/b.git"), std::string("a/b"));
    CHECK_EQ(githubRepoFromUrl("github.com/a/b?tab=readme"), std::string("a/b"));
    CHECK_EQ(githubRepoFromUrl("https://notgithub.com/a/b"), std::string(""));
    CHECK_EQ(githubRepoFromUrl("https://gitlab.com/a/b"), std::string(""));
    CHECK_EQ(githubRepoFromUrl("https://github.com/onlyowner"), std::string(""));

    CHECK_EQ(normalizeDate("03/12/2025"), std::string("2025-12-03"));
    CHECK_EQ(normalizeDate("2026-11-01T10:00:00Z"), std::string("2026-11-01"));
    CHECK_EQ(normalizeDate("yesterday"), std::string(""));
    CHECK_EQ(daysBetween("2026-11-01", "2026-11-09"), 8);
    CHECK_EQ(daysBetween("2025-12-31", "2026-01-01"), 1);
    CHECK_EQ(daysBetween("2024-02-28", "2024-03-01"), 2);
    CHECK_EQ(daysBetween("bad", "2026-01-01"), 0);
}

static void testSourcesAndMerge(const std::string& dir) {
    std::vector<Package> ft;
    std::string err;
    CHECK(parseFortheusersRepo(readFile(dir + "/fortheusers_repo.json"), ft, err));
    CHECK_EQ(ft.size(), size_t(8));

    const Package* smo = find(ft, "Status-Monitor-Overlay");
    CHECK(smo != nullptr);
    if (smo) {
        CHECK_EQ(smo->download_bytes, int64_t(209) * 1024);
        CHECK_EQ(smo->extracted_bytes, int64_t(457) * 1024);
        CHECK_EQ(smo->sha256, std::string("ffd2042a8ee08cd16ec3eb6e07a0c99a8b4598c70a0396db3bc45f8bef00681b"));
        CHECK_EQ(smo->download_url, std::string("https://switch.cdn.fortheusers.org/zips/Status-Monitor-Overlay.zip"));
        CHECK_EQ(smo->manifest_url, std::string("https://switch.cdn.fortheusers.org/packages/Status-Monitor-Overlay/manifest.install"));
        CHECK_EQ(smo->repo, std::string("masagrator/status-monitor-overlay"));
        CHECK(smo->source == Source::Fortheusers);
    }
    const Package* jksv = find(ft, "jksv");
    CHECK(jksv != nullptr);
    if (jksv) {
        CHECK_EQ(jksv->updated, std::string("2025-12-03"));
        CHECK_EQ(jksv->version, std::string("12/02/2025"));
    }
    for (const auto& p : ft) CHECK(p.changelog.find("\\n") == std::string::npos);

    CHECK(!parseFortheusersRepo("not json", ft, err));
    CHECK(!parseFortheusersRepo("{\"packages\": []}", ft, err));
    CHECK(parseFortheusersRepo(readFile(dir + "/fortheusers_repo.json"), ft, err));

    std::vector<Package> own;
    IndexExtras extras;
    CHECK(parseOwnIndex(readFile(dir + "/homebrew_own.json"), own, extras, err));
    // Dropped: BadPayload (bootloader), Traversal (".."), TwoFilesAsFile, NoVersion, second NewTool.
    CHECK_EQ(own.size(), size_t(10));
    CHECK(find(own, "BadPayload") == nullptr);
    CHECK(find(own, "Traversal") == nullptr);
    CHECK(find(own, "TwoFilesAsFile") == nullptr);
    CHECK(find(own, "NoVersion") == nullptr);
    CHECK(find(own, "BadZipRoot") == nullptr);
    if (const Package* p = find(own, "sys-clk")) CHECK_EQ(p->zip_root, std::string("SdOut/"));
    const Package* nt = find(own, "NewTool");
    CHECK(nt != nullptr);
    if (nt) {
        CHECK_EQ(nt->version, std::string("0.3"));
        CHECK(nt->download_type == DownloadType::File);
        CHECK(nt->auto_found);
        CHECK_EQ(nt->kinds, unsigned(KindApp));
    }
    CHECK_EQ(extras.hidden.size(), size_t(3));
    CHECK_EQ(extras.overlay_dependencies.size(), size_t(2));
    CHECK_EQ(extras.fortheusers_meta.size(), size_t(5));
    CHECK(!parseOwnIndex("[]", own, extras, err));
    CHECK(parseOwnIndex(readFile(dir + "/homebrew_own.json"), own, extras, err));

    const std::vector<Package> all = mergeSources(ft, own, extras);
    // 8 fortheusers - Hekate, RetroNX (hidden) + Fizeau, nx-ovlloader, Ultrahand, NewTool, OldAuto.
    CHECK_EQ(all.size(), size_t(11));
    CHECK(find(all, "Hekate") == nullptr);
    CHECK(find(all, "RetroNX") == nullptr);
    CHECK(find(all, "Daybreak") == nullptr);
    CHECK(find(all, "checkpoint-copy") == nullptr);
    CHECK(find(all, "JKSV-github") == nullptr);
    CHECK(find(all, "sys-clk") != nullptr);

    // Newer on GitHub: download switches, fortheusers name stays.
    if (const Package* p = find(all, "sys-clk")) {
        CHECK_EQ(p->version, std::string("2.1.0"));
        CHECK(p->source == Source::Github);
        CHECK_EQ(p->files.size(), size_t(5));
        CHECK(p->manifest_url.empty());
        CHECK_EQ(p->sha256, std::string("aabbcc"));
        CHECK_EQ(p->zip_root, std::string("SdOut/"));
        CHECK_EQ(p->updated, std::string("2026-11-01"));
        CHECK_EQ(p->kinds, unsigned(KindApp | KindOverlay | KindSysmodule));
        CHECK(has(p->depends, "nx-ovlloader") && has(p->depends, "Ultrahand"));
    }
    // Older on GitHub: fortheusers download stays, description added.
    if (const Package* p = find(all, "Goldleaf")) {
        CHECK_EQ(p->version, std::string("1.2.1"));
        CHECK(p->source == Source::Fortheusers);
        CHECK_EQ(p->description_i18n.count("ru"), size_t(1));
    }
    // Date versions.
    if (const Package* p = find(all, "jksv")) {
        CHECK_EQ(p->version, std::string("11/03/2026"));
        CHECK(p->source == Source::Github);
        CHECK(p->download_type == DownloadType::File);
    }
    // Matched by .nro path, same version: nothing changes; meta applied.
    if (const Package* p = find(all, "Checkpoint")) {
        CHECK(p->source == Source::Fortheusers);
        CHECK_EQ(p->kinds, unsigned(KindApp));
        CHECK(p->depends.empty());
        CHECK_EQ(p->description_i18n.at("ru"), std::string("Резервные копии сохранений"));
    }
    // Overlays get the loader + menu; the menu itself does not depend on itself.
    if (const Package* p = find(all, "Status-Monitor-Overlay")) CHECK_EQ(p->depends.size(), size_t(2));
    if (const Package* p = find(all, "Fizeau")) CHECK_EQ(p->depends.size(), size_t(2));
    if (const Package* p = find(all, "Ultrahand")) CHECK(p->depends.empty());
    if (const Package* p = find(all, "nx-ovlloader")) {
        CHECK(p->depends.empty());
        CHECK_EQ(p->kinds, unsigned(KindSysmodule));
    }
    if (const Package* p = find(all, "MissionControl")) CHECK_EQ(p->kinds, unsigned(KindSysmodule));

    // Sorted by title, case-insensitive.
    for (size_t i = 1; i < all.size(); ++i) {
        std::string a = all[i - 1].title, b = all[i].title;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        CHECK(a <= b);
    }

    // Name beats .nro path: a GitHub nx-ovlloader that also ships Checkpoint's
    // .nro must merge into "nx-ovlloader", not into Checkpoint.
    {
        Package loader;
        loader.name = "nx-ovlloader";
        loader.title = "nx-ovlloader";
        loader.version = "1.0.7";
        loader.repo = "werwolv/nx-ovlloader";
        std::vector<Package> ft2 = ft;
        ft2.push_back(loader);
        Package fork;
        fork.name = "nx-ovlloader";
        fork.version = "2.0.3";
        fork.repo = "ppkantorski/nx-ovlloader";
        fork.download_url = "https://example.org/fork.zip";
        fork.source = Source::Github;
        std::string e2;
        CHECK(parseManifest("U: atmosphere/contents/420000000007E51A/exefs.nsp\nU: switch/Checkpoint/Checkpoint.nro\n",
                            fork.files, e2));
        const std::vector<Package> m2 = mergeSources(ft2, {fork}, IndexExtras{});
        if (const Package* p = find(m2, "nx-ovlloader")) {
            CHECK_EQ(p->version, std::string("2.0.3"));
            CHECK(p->source == Source::Github);
        }
        if (const Package* p = find(m2, "Checkpoint")) CHECK(p->source == Source::Fortheusers);
        CHECK_EQ(m2.size(), ft2.size());
    }

    // "New" badge.
    if (const Package* p = find(all, "NewTool")) CHECK(isNew(*p, "2026-11-09"));
    if (const Package* p = find(all, "NewTool")) CHECK(!isNew(*p, "2026-11-20"));
    if (const Package* p = find(all, "OldAuto")) CHECK(!isNew(*p, "2026-11-09"));
    if (const Package* p = find(all, "Fizeau")) CHECK(!isNew(*p, "2026-11-09"));
}

static void testStatusAndRegistry() {
    Package p;
    p.name = "x";
    p.version = "1.2.0";
    Installed inst;
    CHECK(statusFor(p, inst) == Status::NotInstalled);
    inst.present = true;
    CHECK(statusFor(p, inst) == Status::UpdateAvailable);  // manifest without info.json
    inst.version = "1.2.0";
    CHECK(statusFor(p, inst) == Status::Installed);
    inst.version = "1.1.9";
    CHECK(statusFor(p, inst) == Status::UpdateAvailable);
    inst.version = "1.3.0";  // installed from GitHub, newer than the store
    CHECK(statusFor(p, inst) == Status::Installed);

    CHECK_EQ(parseInfoVersion("{\"version\":\"2.0.1-fix\"}"), std::string("2.0.1-fix"));
    CHECK_EQ(parseInfoVersion("{\"version\": 912}"), std::string("912"));
    CHECK_EQ(parseInfoVersion("garbage"), std::string(""));
    p.title = "X \"quoted\"";
    CHECK_EQ(parseInfoVersion(makeInfoJson(p)), std::string("1.2.0"));
}

static void testPlans() {
    std::vector<ManifestEntry> old_m, new_m;
    std::string err;
    CHECK(parseManifest("U: switch/a/a.nro\nU: switch/a/old.dat\nG: config/a/a.ini\nE: switch/a/assets.bin\n", old_m, err));
    CHECK(parseManifest("U: switch/a/a.nro\nU: switch/.overlays/a.ovl\nG: config/a/a.ini\nG: config/a/new.ini\nE: switch/a/assets.bin\nL: switch/a/log.txt\n", new_m, err));

    const std::set<std::string> on_card = {"config/a/a.ini"};
    const InstallPlan ip = planInstall(new_m, old_m, [&](const std::string& p) { return on_card.count(p) > 0; });
    std::vector<std::string> writes;
    for (const auto& e : ip.write) writes.push_back(e.path);
    CHECK_EQ(writes.size(), size_t(4));
    CHECK(has(writes, "switch/a/a.nro"));
    CHECK(has(writes, "switch/.overlays/a.ovl"));
    CHECK(has(writes, "config/a/new.ini"));
    CHECK(has(writes, "switch/a/assets.bin"));
    CHECK(!has(writes, "config/a/a.ini"));
    CHECK(!has(writes, "switch/a/log.txt"));
    CHECK_EQ(ip.keep_existing.size(), size_t(1));
    CHECK_EQ(ip.remove_stale.size(), size_t(1));
    if (!ip.remove_stale.empty()) CHECK_EQ(ip.remove_stale[0], std::string("switch/a/old.dat"));

    // Fresh install without old manifest.
    const InstallPlan fresh = planInstall(new_m, {}, nullptr);
    CHECK_EQ(fresh.write.size(), size_t(5));
    CHECK(fresh.remove_stale.empty());

    // Removal like libget: U, G, L go, E stays.
    std::vector<ManifestEntry> sc;
    CHECK(parseManifest("U: switch/sys-clk-manager.nro\nU: atmosphere/contents/00FF0000636C6BFF/exefs.nsp\n"
                        "U: atmosphere/contents/00FF0000636C6BFF/flags/boot2.flag\nG: config/sys-clk/config.ini\n"
                        "U: switch/.overlays/sys-clk-overlay.ovl\nE: switch/sys-clk/res/font.bin\n", sc, err));
    RemovalPlan rp = planRemoval(sc, false);
    CHECK_EQ(rp.remove_files.size(), size_t(5));
    CHECK(has(rp.kept, "switch/sys-clk/res/font.bin"));
    CHECK(has(rp.remove_files, "config/sys-clk/config.ini"));
    CHECK_EQ(rp.remove_dirs.size(), size_t(3));
    if (rp.remove_dirs.size() == 3) {
        CHECK_EQ(rp.remove_dirs[0], std::string("atmosphere/contents/00FF0000636C6BFF/flags"));
        CHECK_EQ(rp.remove_dirs[1], std::string("atmosphere/contents/00FF0000636C6BFF"));
        CHECK_EQ(rp.remove_dirs[2], std::string("config/sys-clk"));
    }
    CHECK(!has(rp.remove_dirs, "switch"));
    CHECK(!has(rp.remove_dirs, "switch/.overlays"));
    CHECK(!has(rp.remove_dirs, "atmosphere/contents"));

    // Keep settings: G and config/ stay.
    rp = planRemoval(sc, true);
    CHECK_EQ(rp.remove_files.size(), size_t(4));
    CHECK(has(rp.kept, "config/sys-clk/config.ini"));
    CHECK(!has(rp.remove_dirs, "config/sys-clk"));
}

// Smoke run over a full repo.json: everything parses, nothing throws.
static void smokeFullRepo(const std::string& path) {
    std::vector<Package> ft;
    std::string err;
    const bool ok = parseFortheusersRepo(readFile(path), ft, err);
    CHECK(ok);
    int with_repo = 0, with_sha = 0, dated = 0;
    for (const auto& p : ft) {
        with_repo += !p.repo.empty();
        with_sha += !p.sha256.empty();
        dated += !p.updated.empty();
        (void)compareVersions(p.version, "1.0.0");
    }
    std::cout << "full repo.json: " << ft.size() << " packages, " << with_repo << " with GitHub repo, "
              << with_sha << " with sha256, " << dated << " with a date\n";
    CHECK(ft.size() > 400);
    const std::vector<Package> merged = mergeSources(ft, {}, IndexExtras{});
    CHECK_EQ(merged.size(), ft.size());
}

// Smoke run: the real homebrew.json from the index builder merged with the full repo.json.
static void smokeRealIndex(const std::string& repo_path, const std::string& index_path) {
    std::vector<Package> ft, own;
    IndexExtras extras;
    std::string err;
    CHECK(parseFortheusersRepo(readFile(repo_path), ft, err));
    const bool ok = parseOwnIndex(readFile(index_path), own, extras, err);
    CHECK(ok);
    if (!ok) std::cerr << "     " << err << "\n";
    const std::vector<Package> all = mergeSources(ft, own, extras);

    int github = 0, overlays = 0, sysmodules = 0, apps = 0, unknown = 0, with_deps = 0;
    for (const auto& p : all) {
        github += p.source == Source::Github;
        overlays += (p.kinds & KindOverlay) != 0;
        sysmodules += (p.kinds & KindSysmodule) != 0;
        apps += (p.kinds & KindApp) != 0;
        unknown += p.kinds == 0;
        with_deps += !p.depends.empty();
        if (p.source == Source::Github) {
            CHECK(!p.files.empty());
            CHECK(analyzeFiles(p.files).worst != PathVerdict::Forbidden);
            std::cout << "  github: " << p.name << " " << p.version << " (" << p.files.size() << " files"
                      << (p.zip_root.empty() ? "" : ", root " + p.zip_root) << ")\n";
        }
        CHECK(!extras.hidden.count([&] { std::string l = p.name; std::transform(l.begin(), l.end(), l.begin(), ::tolower); return l; }()));
    }
    std::cout << "merged: " << all.size() << " packages (" << ft.size() << " fortheusers + " << own.size()
              << " own, " << extras.hidden.size() << " hidden), from GitHub " << github << ", apps " << apps
              << ", overlays " << overlays << ", sysmodules " << sysmodules << ", kinds unknown " << unknown
              << ", with dependencies " << with_deps << "\n";
    CHECK(own.size() >= 10);
    CHECK(all.size() > 450);
    CHECK(github >= 3);
    for (const char* n : {"Hekate", "Tesla-menu", "TegraExplorer", "argon-nx"}) CHECK(find(all, n) == nullptr);
    if (const Package* p = find(all, "UltrahandOverlay")) {
        CHECK(p->source == Source::Github);
        CHECK(p->depends.empty());
    }
    if (const Package* p = find(all, "Status-Monitor-Overlay")) {
        CHECK(has(p->depends, "nx-ovlloader"));
        CHECK(has(p->depends, "UltrahandOverlay"));
    }
    if (const Package* p = find(all, "nx-ovlloader")) {
        CHECK(p->source == Source::Github);
        CHECK(p->depends.empty());
        CHECK_EQ(p->author, std::string("ppkantorski"));  // the fork that is downloaded, not WerWolv
        CHECK_EQ(p->repo, std::string("ppkantorski/nx-ovlloader"));
    }
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "pctest/homebrew_fixtures";
    testPathRules(dir);
    testNormalize();
    testManifests(dir);
    testVersions();
    testUrlsAndDates();
    testSourcesAndMerge(dir);
    testStatusAndRegistry();
    testPlans();
    if (argc > 2) smokeFullRepo(argv[2]);
    if (argc > 3) smokeRealIndex(argv[2], argv[3]);

    std::cout << (g_failed ? "FAILED " : "OK ") << (g_checks - g_failed) << "/" << g_checks << " checks\n";
    return g_failed ? 1 : 0;
}
