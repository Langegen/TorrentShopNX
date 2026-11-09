// Tests for homebrew::Catalog (source/catalog/homebrew_catalog.*): download
// schedule, ETag, cache, bad answers, the libget registry and hand-installed
// packages. Network and clock are fakes; files go to a temp folder.
//
// Build and run from the repo root:  powershell -File pctest\build_homebrew.ps1

#include "../source/catalog/homebrew_catalog.hpp"

#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace homebrew;
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

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void writeFile(const std::string& path, const std::string& data) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << data;
}

// Fake server: queued answers per URL; every request is recorded.
struct FakeNet {
    std::map<std::string, std::deque<FetchResult>> answers;
    std::vector<std::pair<std::string, std::string>> requests;  // url, etag sent
    FetchResult get(const std::string& url, const std::string& etag) {
        requests.emplace_back(url, etag);
        auto& q = answers[url];
        if (q.empty()) return FetchResult{0, "", ""};
        FetchResult r = q.front();
        q.pop_front();
        return r;
    }
};

static const char* kFtUrl = "https://cdn.test/repo.json";
static const char* kIdxUrl = "https://raw.test/homebrew.json";

struct Env {
    std::string root;
    CatalogConfig cfg;
    FakeNet net;
    long long now = 1'800'000'000;

    explicit Env(const std::string& base) {
        root = base;
        fs::remove_all(root);
        cfg.cache_dir = root + "/sd/switch/TorrentShopNX/cache/homebrew";  // created by the catalog
        cfg.registry_dir = root + "/sd/switch/appstore/.get/packages";
        cfg.sd_root = root + "/sd/";
        cfg.fortheusers_url = kFtUrl;
        cfg.index_url = kIdxUrl;
    }
    Catalog make() {
        return Catalog(cfg, [this](const std::string& u, const std::string& e) { return net.get(u, e); },
                       [this] { return now; });
    }
    void answer(const char* url, int status, const std::string& body = "", const std::string& etag = "") {
        net.answers[url].push_back(FetchResult{status, body, etag});
    }
};

static bool hasPackage(const Catalog& c, const std::string& name) {
    Package p;
    return c.find(name, p);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "pctest/homebrew_fixtures";
    const std::string ft = readFile(dir + "/fortheusers_repo.json");
    const std::string own = readFile(dir + "/homebrew_own.json");
    CHECK(!ft.empty() && !own.empty());
    const std::string tmp = (fs::temp_directory_path() / "tsnx_homebrew_catalog_test").string();

    // 1. Cold start: nothing cached, both sources downloaded and saved.
    {
        Env env(tmp);
        Catalog c = env.make();
        CHECK(!c.loadCached());
        CHECK(!c.loaded());
        env.answer(kFtUrl, 200, ft, "\"ft-1\"");
        env.answer(kIdxUrl, 200, own, "W/\"idx-1\"");
        const uint64_t gen0 = c.generation();
        RefreshResult r = c.refresh();
        CHECK(r.attempted);
        CHECK(r.ok);
        CHECK(r.changed);
        CHECK_EQ(r.error, std::string(""));
        CHECK(c.loaded());
        CHECK(c.generation() > gen0);
        CHECK_EQ(c.snapshot()->size(), size_t(11));
        CHECK_EQ(c.lastCheck(), env.now);
        CHECK_EQ(env.net.requests.size(), size_t(2));
        CHECK_EQ(env.net.requests[0].second, std::string(""));  // no ETag without a cached file
        CHECK(fs::exists(env.cfg.cache_dir + "/repo.json"));
        CHECK_EQ(readFile(env.cfg.cache_dir + "/homebrew.json.etag"), std::string("W/\"idx-1\""));
        CHECK(hasPackage(c, "sys-clk") && hasPackage(c, "NewTool") && !hasPackage(c, "Hekate"));

        // 2. Within 6 hours: nothing is downloaded.
        env.now += 3 * 3600;
        r = c.refresh();
        CHECK(!r.attempted);
        CHECK(r.ok);
        CHECK_EQ(env.net.requests.size(), size_t(2));

        // 3. Later: ETags go out, both answer 304, list kept, stamp moves.
        env.now += 4 * 3600;
        env.answer(kFtUrl, 304);
        env.answer(kIdxUrl, 304);
        const uint64_t gen1 = c.generation();
        r = c.refresh();
        CHECK(r.attempted && r.ok && !r.changed);
        CHECK_EQ(env.net.requests[2].second, std::string("\"ft-1\""));
        CHECK_EQ(env.net.requests[3].second, std::string("W/\"idx-1\""));
        CHECK_EQ(c.generation(), gen1);
        CHECK_EQ(c.lastCheck(), env.now);

        // 4. Forced, index changed (one package less): list rebuilt; no ETag in answer -> none kept.
        std::string own2 = own;
        const size_t at = own2.find("\"name\": \"OldAuto\"");
        CHECK(at != std::string::npos);
        own2.replace(at, 17, "\"name\": \"\"");  // nameless package is skipped by the parser
        env.answer(kFtUrl, 304);
        env.answer(kIdxUrl, 200, own2, "");
        r = c.refresh(true);
        CHECK(r.ok && r.changed);
        CHECK_EQ(c.snapshot()->size(), size_t(10));
        CHECK(!hasPackage(c, "OldAuto"));
        CHECK(!fs::exists(env.cfg.cache_dir + "/homebrew.json.etag"));

        // 5. Broken answer: rejected, old file and list kept, stamp not moved.
        const long long stamp = c.lastCheck();
        env.now += 7 * 3600;
        env.answer(kFtUrl, 304);
        env.answer(kIdxUrl, 200, "<html>rate limited</html>", "\"x\"");
        r = c.refresh();
        CHECK(r.attempted && !r.ok && !r.changed);
        CHECK(r.error.find("homebrew.json") != std::string::npos);
        CHECK_EQ(c.snapshot()->size(), size_t(10));
        CHECK_EQ(readFile(env.cfg.cache_dir + "/homebrew.json"), own2);
        CHECK_EQ(c.lastCheck(), stamp);
        CHECK_EQ(env.net.requests.back().second, std::string(""));  // no ETag file -> none sent

        // 6. Network down: same, and the next call tries again (stamp is old).
        r = c.refresh();
        CHECK(r.attempted && !r.ok);
        CHECK_EQ(r.fortheusers_status, 0);
        CHECK_EQ(c.snapshot()->size(), size_t(10));
        const size_t before = env.net.requests.size();
        r = c.refresh();
        CHECK(r.attempted);
        CHECK_EQ(env.net.requests.size(), before + 2);

        // 7. A new start reads the cache without the network.
        Catalog c2 = env.make();
        CHECK(c2.loadCached());
        CHECK_EQ(c2.snapshot()->size(), size_t(10));
        CHECK_EQ(env.net.requests.size(), before + 2);
    }

    // 8. Registry shared with the Homebrew App Store, and hand-installed packages.
    {
        Env env(tmp);
        const std::string reg = env.cfg.registry_dir;
        writeFile(reg + "/sys-clk/manifest.install", "U: switch/sys-clk-manager.nro\n");
        writeFile(reg + "/sys-clk/info.json", "{\"version\":\"2.0.1-fix\"}");   // store has 2.1.0
        writeFile(reg + "/Checkpoint/manifest.install", "U: switch/Checkpoint/Checkpoint.nro\n");
        writeFile(reg + "/Checkpoint/info.json", "{\"version\":\"5.3.0\"}");     // up to date
        writeFile(reg + "/goldleaf/manifest.install", "U: switch/Goldleaf/Goldleaf.nro\n");  // no info.json
        writeFile(reg + "/Junk/readme.txt", "no manifest -> not installed");
        writeFile(reg + "/NotInStore/manifest.install", "U: switch/x/x.nro\n");
        writeFile(env.cfg.sd_root + "switch/JKSV/JKSV.nro", "nro");               // jksv put there by hand
        writeFile(env.cfg.sd_root + "switch/.overlays/OldAuto.ovl", "ovl");       // overlays are not checked

        Catalog c = env.make();
        env.answer(kFtUrl, 200, ft, "a");
        env.answer(kIdxUrl, 200, own, "b");
        CHECK(c.refresh().ok);

        Package p;
        CHECK(c.find("SYS-CLK", p));  // names are case-insensitive
        CHECK(c.status(p) == Status::UpdateAvailable);
        CHECK_EQ(c.installed("sys-clk").version, std::string("2.0.1-fix"));
        CHECK_EQ(c.installed("sys-clk").manifest.size(), size_t(1));
        CHECK(c.find("Checkpoint", p) && c.status(p) == Status::Installed);
        CHECK(c.find("Goldleaf", p) && c.status(p) == Status::UpdateAvailable);
        CHECK(!c.installed("Junk").present);
        CHECK(c.installed("NotInStore").present);
        CHECK(c.find("jksv", p) && c.status(p) == Status::Installed);
        CHECK(c.installed("jksv").untracked);
        CHECK(c.find("OldAuto", p) && c.status(p) == Status::NotInstalled);
        CHECK(c.find("Fizeau", p) && c.status(p) == Status::NotInstalled);
        CHECK_EQ(c.updatesAvailable(), 2);  // sys-clk, Goldleaf; jksv is untracked

        // Installing from elsewhere (the official client) shows up after a rescan.
        writeFile(reg + "/Fizeau/manifest.install", "U: switch/Fizeau/Fizeau.nro\n");
        writeFile(reg + "/Fizeau/info.json", "{\"version\":\"2.7.0\"}");
        const uint64_t gen = c.generation();
        c.rescanInstalled();
        CHECK(c.generation() > gen);
        CHECK(c.find("Fizeau", p) && c.status(p) == Status::Installed);
        CHECK_EQ(c.installedSnapshot()->size(), size_t(6));
    }

    // 9. Cold start with fortheusers down: the index alone is shown, retried next time.
    {
        Env env(tmp);
        Catalog c = env.make();
        env.answer(kFtUrl, 503);
        env.answer(kIdxUrl, 200, own, "b");
        RefreshResult r = c.refresh();
        CHECK(!r.ok && r.changed);
        CHECK_EQ(r.fortheusers_status, 503);
        CHECK(c.loaded());
        CHECK(hasPackage(c, "Fizeau") && !hasPackage(c, "Checkpoint"));
        CHECK_EQ(c.lastCheck(), 0LL);
        env.answer(kFtUrl, 200, ft, "a");
        env.answer(kIdxUrl, 304);
        r = c.refresh();
        CHECK(r.attempted && r.ok && r.changed);
        CHECK(hasPackage(c, "Checkpoint"));
    }

    // 10. Both sources down on a cold start: nothing to show, no crash.
    {
        Env env(tmp);
        Catalog c = env.make();
        RefreshResult r = c.refresh();
        CHECK(!r.ok && !r.changed && !c.loaded());
        CHECK(c.snapshot()->empty());
        CHECK_EQ(c.updatesAvailable(), 0);
        c.rescanInstalled();  // no registry folder
        CHECK(c.installedSnapshot()->empty());
    }

    fs::remove_all(tmp);
    std::cout << (g_failed ? "FAILED " : "OK ") << (g_checks - g_failed) << "/" << g_checks << " catalog checks\n";
    return g_failed ? 1 : 0;
}
