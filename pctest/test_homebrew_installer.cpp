// Tests for homebrew::Installer, util::ZipReader and util hashes: install,
// update, settings kept, checksum, cancel, no space, forbidden paths, GitHub
// zips with a wrapping folder, single .nro, uninstall (also with a tampered
// registry), dependencies. Zips are built here with zlib; network is fake.
//
// Build and run from the repo root:  powershell -File pctest\build_homebrew.ps1
// Real package smoke:  test_homebrew_installer.exe <fixtures> <Status-Monitor-Overlay.zip>

#include "../source/catalog/homebrew_installer.hpp"
#include "../source/utils/hash_utils.h"
#include "../source/utils/zip_reader.h"

#include <zlib.h>

#include <cstring>
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

static std::string sha256Of(const std::string& s) {
    util::Sha256 h;
    h.update(s.data(), s.size());
    return h.hexDigest();
}

static std::string md5Of(const std::string& s) {
    util::Md5 h;
    h.update(s.data(), s.size());
    return h.hexDigest();
}

// --- tiny zip writer -------------------------------------------------------

struct ZipItem {
    std::string name;
    std::string data;
    bool deflate = true;
};

static void put16(std::string& s, uint32_t v) { s += char(v & 0xff); s += char((v >> 8) & 0xff); }
static void put32(std::string& s, uint32_t v) { put16(s, v & 0xffff); put16(s, v >> 16); }

static std::string rawDeflate(const std::string& in) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&zs, in.size()) + 16, '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

static std::string makeZip(const std::vector<ZipItem>& items) {
    std::string out, cd;
    for (const auto& it : items) {
        const uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(it.data.data()), static_cast<uInt>(it.data.size()));
        const std::string body = it.deflate ? rawDeflate(it.data) : it.data;
        const uint32_t off = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, it.deflate ? 8 : 0);
        put16(out, 0); put16(out, 0x21); put32(out, crc); put32(out, uint32_t(body.size()));
        put32(out, uint32_t(it.data.size())); put16(out, uint32_t(it.name.size())); put16(out, 0);
        out += it.name;
        out += body;
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, it.deflate ? 8 : 0);
        put16(cd, 0); put16(cd, 0x21); put32(cd, crc); put32(cd, uint32_t(body.size()));
        put32(cd, uint32_t(it.data.size())); put16(cd, uint32_t(it.name.size())); put16(cd, 0); put16(cd, 0);
        put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off);
        cd += it.name;
    }
    const uint32_t cd_off = static_cast<uint32_t>(out.size());
    out += cd;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, uint32_t(items.size()));
    put16(out, uint32_t(items.size())); put32(out, uint32_t(cd.size())); put32(out, cd_off); put16(out, 0);
    return out;
}

// --- fake environment ------------------------------------------------------

struct Env {
    std::string root, sd, reg, tmp, srv;
    std::map<std::string, std::string> served;     // url -> file body
    std::map<std::string, std::string> manifests;  // url -> manifest text
    int64_t free_bytes = -1;
    int downloads = 0;
    std::function<void()> on_download;  // runs inside the fake download

    explicit Env(const std::string& base) {
        root = base;
        fs::remove_all(root);
        sd = root + "/sd/";
        reg = sd + "switch/appstore/.get/packages";
        tmp = sd + "switch/TorrentShopNX/cache/tmp";
        fs::create_directories(sd);
    }
    Installer make() {
        return Installer(
            InstallerConfig{sd, reg, tmp},
            [this](const std::string& url, const std::string&) {
                auto it = manifests.find(url);
                return it == manifests.end() ? FetchResult{404, "", ""} : FetchResult{200, it->second, ""};
            },
            [this](const std::string& url, const std::string& dest, const std::function<void(int64_t, int64_t)>& prog,
                   const std::atomic<bool>* cancel, std::string& err) {
                ++downloads;
                if (on_download) on_download();
                if (cancel && cancel->load()) return false;
                auto it = served.find(url);
                if (it == served.end()) {
                    err = "404";
                    return false;
                }
                writeFile(dest, it->second);
                if (prog) prog(int64_t(it->second.size()), int64_t(it->second.size()));
                return true;
            },
            [this](const std::string&) { return free_bytes; });
    }
    std::string sdFile(const std::string& rel) const { return readFile(sd + rel); }
    bool sdHas(const std::string& rel) const { return fs::exists(sd + rel); }
};

static Installed registry(const Env& env, const std::string& name) {
    Installed in;
    const std::string dir = env.reg + "/" + name;
    std::string text = readFile(dir + "/manifest.install");
    if (text.empty()) return in;
    in.present = true;
    std::string err;
    parseManifest(text, in.manifest, err);
    in.version = parseInfoVersion(readFile(dir + "/info.json"));
    return in;
}

static bool noLeftovers(const std::string& dir) {
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        const std::string p = e.path().string();
        if (p.find(".hbnew") != std::string::npos || p.find(".hbold") != std::string::npos) return false;
    }
    return true;
}

static Package ftPackage(const std::string& name, const std::string& version, const std::string& zip,
                         const std::string& manifest, Env& env) {
    Package p;
    p.name = name;
    p.title = name;
    p.version = version;
    p.download_url = "https://cdn.test/zips/" + name + "-" + version + ".zip";
    p.manifest_url = "https://cdn.test/packages/" + name + "-" + version + "/manifest.install";
    p.sha256 = sha256Of(zip);
    p.download_bytes = int64_t(zip.size());
    p.extracted_bytes = 4096;
    env.served[p.download_url] = zip;
    env.manifests[p.manifest_url] = manifest;
    return p;
}

int main(int argc, char** argv) {
    const std::string tmp = (fs::temp_directory_path() / "tsnx_homebrew_installer_test").string();

    // Hashes: standard test vectors.
    CHECK_EQ(sha256Of(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256Of("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256Of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256Of(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    CHECK_EQ(md5Of(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(md5Of("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(md5Of("The quick brown fox jumps over the lazy dog"), std::string("9e107d9d372bb6826bd81d3542a419d6"));
    {
        util::Sha256 h;  // split updates give the same digest
        h.update("ab", 2);
        h.update("c", 1);
        CHECK_EQ(h.hexDigest(), sha256Of("abc"));
    }

    // Zip reader on its own: stored + deflate, case-insensitive lookup, CRC check.
    {
        Env env(tmp);
        const std::string big(200000, 'x');
        writeFile(env.root + "/a.zip", makeZip({{"dir/", "", false}, {"dir/Big.bin", big, true}, {"s.txt", "stored", false}}));
        util::ZipReader zr;
        std::string err;
        CHECK(zr.open(env.root + "/a.zip", err));
        CHECK_EQ(zr.entries().size(), size_t(3));
        const util::ZipEntry* e = zr.find("DIR/big.BIN");
        CHECK(e != nullptr);
        if (e) {
            uint64_t last = 0;
            CHECK(zr.extract(*e, env.root + "/big.out", nullptr, [&](uint64_t n) { last = n; }, err));
            CHECK_EQ(readFile(env.root + "/big.out"), big);
            CHECK_EQ(last, uint64_t(big.size()));
        }
        CHECK(zr.find("dir/") && zr.find("dir/")->is_dir);
        // Flip one byte of the stored entry's data: CRC must catch it.
        std::string z = makeZip({{"s.txt", "stored data", false}});
        z[30 + 5] ^= 0x20;
        writeFile(env.root + "/bad.zip", z);
        util::ZipReader zb;
        CHECK(zb.open(env.root + "/bad.zip", err));
        CHECK(!zb.extract(zb.entries()[0], env.root + "/bad.out", nullptr, nullptr, err));
        CHECK(err.find("CRC") != std::string::npos);
        CHECK(!fs::exists(env.root + "/bad.out"));
        writeFile(env.root + "/trunc.zip", "PK\x03\x04 not a zip");
        CHECK(!zb.open(env.root + "/trunc.zip", err));
    }

    // 1. Fresh install of a fortheusers package (list from manifest.install).
    {
        Env env(tmp);
        Installer inst = env.make();
        const std::string m1 =
            "U: switch/sys-clk-manager.nro\nU: atmosphere/contents/00FF0000636C6BFF/exefs.nsp\n"
            "U: atmosphere/contents/00FF0000636C6BFF/flags/boot2.flag\nG: config/sys-clk/config.ini\n"
            "U: switch/.overlays/sys-clk-overlay.ovl\nE: switch/sys-clk/res/font.bin\nL: config/sys-clk/log.txt\n"
            "U: README.md\n";
        const std::string z1 = makeZip({{"switch/sys-clk-manager.nro", "nro-v1"},
                                        {"atmosphere/contents/00FF0000636C6BFF/exefs.nsp", "nsp-v1"},
                                        {"atmosphere/contents/00FF0000636C6BFF/flags/boot2.flag", "", false},
                                        {"config/sys-clk/config.ini", "default=1"},
                                        {"switch/.overlays/sys-clk-overlay.ovl", "ovl-v1"},
                                        {"switch/sys-clk/res/font.bin", "font"},
                                        {"README.md", "readme"},
                                        {"manifest.install", m1},
                                        {"info.json", "{}"}});
        Package p = ftPackage("sys-clk", "2.0.0", z1, m1, env);
        std::vector<InstallProgress::Stage> stages;
        float last_fraction = -1.f;
        bool monotonic = true;
        InstallResult r = inst.install(p, Installed{}, [&](const InstallProgress& pr) {
            if (stages.empty() || stages.back() != pr.stage) stages.push_back(pr.stage);
            if (pr.fraction + 1e-6f < last_fraction) monotonic = false;
            last_fraction = pr.fraction;
        }, nullptr);
        CHECK(r.ok);
        CHECK_EQ(std::string(installErrorName(r.error)), std::string("none"));
        CHECK_EQ(r.written.size(), size_t(7));
        CHECK_EQ(env.sdFile("switch/sys-clk-manager.nro"), std::string("nro-v1"));
        CHECK(env.sdHas("atmosphere/contents/00FF0000636C6BFF/flags/boot2.flag"));
        CHECK_EQ(env.sdFile("config/sys-clk/config.ini"), std::string("default=1"));
        CHECK(!env.sdHas("manifest.install") && !env.sdHas("info.json"));  // only listed files
        CHECK(!env.sdHas("config/sys-clk/log.txt"));                        // L: never extracted
        const Installed reg = registry(env, "sys-clk");
        CHECK(reg.present);
        CHECK_EQ(reg.version, std::string("2.0.0"));
        CHECK_EQ(reg.manifest.size(), size_t(8));
        CHECK(noLeftovers(env.root));
        CHECK(fs::is_empty(env.tmp));  // download removed
        CHECK(monotonic);
        CHECK(stages.size() >= 4 && stages.front() == InstallProgress::Stage::Prepare &&
              stages.back() == InstallProgress::Stage::Finish);
        CHECK(last_fraction > 0.999f);

        // 2. Update: settings kept, a dropped file removed, E rewritten, registry updated.
        writeFile(env.sd + "config/sys-clk/config.ini", "user=42");
        const std::string m2 =
            "U: switch/sys-clk-manager.nro\nU: atmosphere/contents/00FF0000636C6BFF/exefs.nsp\n"
            "U: atmosphere/contents/00FF0000636C6BFF/flags/boot2.flag\nG: config/sys-clk/config.ini\n"
            "E: switch/sys-clk/res/font.bin\nU: README.md\n";  // overlay dropped
        const std::string z2 = makeZip({{"switch/sys-clk-manager.nro", "nro-v2"},
                                        {"atmosphere/contents/00FF0000636C6BFF/exefs.nsp", "nsp-v2"},
                                        {"atmosphere/contents/00FF0000636C6BFF/flags/boot2.flag", "", false},
                                        {"config/sys-clk/config.ini", "default=2"},
                                        {"switch/sys-clk/res/font.bin", "font2"},
                                        {"README.md", "readme2"}});
        Package p2 = ftPackage("sys-clk", "2.1.0", z2, m2, env);
        r = inst.install(p2, registry(env, "sys-clk"), nullptr, nullptr);
        CHECK(r.ok);
        CHECK_EQ(env.sdFile("switch/sys-clk-manager.nro"), std::string("nro-v2"));
        CHECK_EQ(env.sdFile("config/sys-clk/config.ini"), std::string("user=42"));
        CHECK_EQ(r.kept.size(), size_t(1));
        CHECK(!env.sdHas("switch/.overlays/sys-clk-overlay.ovl"));
        CHECK(env.sdHas("switch/.overlays"));  // shared folder stays
        CHECK_EQ(r.removed.size(), size_t(1));
        CHECK_EQ(env.sdFile("switch/sys-clk/res/font.bin"), std::string("font2"));
        CHECK_EQ(registry(env, "sys-clk").version, std::string("2.1.0"));
        CHECK(noLeftovers(env.root));

        // 3. Checksum mismatch: nothing on the card changes.
        Package p3 = ftPackage("sys-clk", "2.2.0", makeZip({{"switch/sys-clk-manager.nro", "nro-v3"}}),
                               "U: switch/sys-clk-manager.nro\n", env);
        p3.sha256 = std::string(64, '0');
        r = inst.install(p3, registry(env, "sys-clk"), nullptr, nullptr);
        CHECK(!r.ok);
        CHECK(r.error == InstallError::Checksum);
        CHECK_EQ(env.sdFile("switch/sys-clk-manager.nro"), std::string("nro-v2"));
        CHECK_EQ(registry(env, "sys-clk").version, std::string("2.1.0"));
        CHECK(fs::is_empty(env.tmp));

        // MD5 is used when there is no SHA-256 (most fortheusers packages).
        p3.sha256.clear();
        p3.md5 = md5Of(env.served[p3.download_url]);
        r = inst.install(p3, registry(env, "sys-clk"), nullptr, nullptr);
        CHECK(r.ok);
        CHECK_EQ(env.sdFile("switch/sys-clk-manager.nro"), std::string("nro-v3"));
        // ...and the files the 2.2.0 list no longer has are gone, except E and G ones.
        CHECK(!env.sdHas("atmosphere/contents/00FF0000636C6BFF/exefs.nsp"));
        CHECK(!env.sdHas("atmosphere/contents/00FF0000636C6BFF"));  // emptied folder removed
        CHECK(env.sdHas("switch/sys-clk/res/font.bin"));
        CHECK(env.sdHas("config/sys-clk/config.ini"));

        // 4. Cancel in the middle of unpacking: old files intact, no leftovers, new folders gone.
        const std::string m4 = "U: switch/sys-clk-manager.nro\nU: switch/newdir/a.bin\nU: switch/newdir/b.bin\n";
        Package p4 = ftPackage("sys-clk", "3.0.0",
                               makeZip({{"switch/sys-clk-manager.nro", "nro-v4"},
                                        {"switch/newdir/a.bin", std::string(300000, 'a')},
                                        {"switch/newdir/b.bin", std::string(300000, 'b')}}),
                               m4, env);
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        r = inst.install(p4, registry(env, "sys-clk"), [&](const InstallProgress& pr) {
            if (pr.stage == InstallProgress::Stage::Extract && pr.file == "switch/newdir/a.bin") cancel->store(true);
        }, cancel);
        CHECK(!r.ok);
        CHECK(r.error == InstallError::Cancelled);
        CHECK_EQ(env.sdFile("switch/sys-clk-manager.nro"), std::string("nro-v3"));
        CHECK(!env.sdHas("switch/newdir"));
        CHECK(noLeftovers(env.root));
        CHECK_EQ(registry(env, "sys-clk").version, std::string("2.2.0"));
        CHECK(fs::is_empty(env.tmp));

        // Cancel before the download even starts.
        cancel->store(true);
        const int before = env.downloads;
        r = inst.install(p4, registry(env, "sys-clk"), nullptr, cancel);
        CHECK(r.error == InstallError::Cancelled);
        CHECK_EQ(env.downloads, before);

        // 10. Uninstall removes what the current list (2.2.0: one .nro) owns, and the registry entry.
        r = inst.uninstall("SYS-CLK", false, nullptr);
        CHECK(r.ok);
        CHECK(!env.sdHas("switch/sys-clk-manager.nro"));
        CHECK(env.sdHas("config/sys-clk/config.ini"));     // left by older versions, not in the list
        CHECK(env.sdHas("switch/sys-clk/res/font.bin"));
        CHECK(!env.sdHas("switch/appstore/.get/packages/sys-clk"));
        CHECK(env.sdHas("switch"));
        r = inst.uninstall("sys-clk", false, nullptr);
        CHECK(!r.ok && r.error == InstallError::NotInstalled);
    }

    // Keep settings on uninstall; a tampered registry never deletes outside package paths.
    {
        Env env(tmp);
        Installer inst = env.make();
        const std::string m = "U: switch/x/x.nro\nG: config/x/x.ini\n";
        Package p = ftPackage("x", "1.0", makeZip({{"switch/x/x.nro", "n"}, {"config/x/x.ini", "c"}}), m, env);
        CHECK(inst.install(p, Installed{}, nullptr, nullptr).ok);
        InstallResult r = inst.uninstall("x", true, nullptr);
        CHECK(r.ok);
        CHECK(!env.sdHas("switch/x/x.nro"));
        CHECK(!env.sdHas("switch/x"));
        CHECK(env.sdHas("config/x/x.ini"));

        // Full removal like libget: U, G, L go, E stays, emptied folders go.
        const std::string my = "U: switch/y/y.nro\nG: config/y/y.ini\nL: config/y/log.txt\nE: switch/y/data.bin\n";
        Package py = ftPackage("y", "1.0",
                               makeZip({{"switch/y/y.nro", "n"}, {"config/y/y.ini", "c"}, {"switch/y/data.bin", "d"}}),
                               my, env);
        CHECK(inst.install(py, Installed{}, nullptr, nullptr).ok);
        writeFile(env.sd + "config/y/log.txt", "log");  // created by the app
        r = inst.uninstall("y", false, nullptr);
        CHECK(r.ok);
        CHECK_EQ(r.removed.size(), size_t(3));
        CHECK(!env.sdHas("config/y"));
        CHECK(env.sdHas("switch/y/data.bin"));
        CHECK(!env.sdHas("switch/appstore/.get/packages/y"));

        writeFile(env.sd + "bootloader/hekate_ipl.ini", "boot");
        writeFile(env.sd + "switch/TorrentShopNX/config.ini", "ours");
        writeFile(env.root + "/outside.txt", "outside");
        writeFile(env.reg + "/evil/manifest.install",
                  "U: bootloader/hekate_ipl.ini\nU: switch/TorrentShopNX/config.ini\nU: ../outside.txt\nU: switch/evil/e.nro\n");
        writeFile(env.sd + "switch/evil/e.nro", "e");
        r = inst.uninstall("evil", false, nullptr);
        CHECK(r.ok);
        CHECK(env.sdHas("bootloader/hekate_ipl.ini"));
        CHECK(env.sdHas("switch/TorrentShopNX/config.ini"));
        CHECK(fs::exists(env.root + "/outside.txt"));
        CHECK(!env.sdHas("switch/evil/e.nro"));
    }

    // 5./6./15. Refused before anything is downloaded; network failure.
    {
        Env env(tmp);
        Installer inst = env.make();
        Package bad = ftPackage("hekate", "6.5", makeZip({{"bootloader/update.bin", "b"}}),
                                "U: bootloader/update.bin\nU: switch/h/h.nro\n", env);
        InstallResult r = inst.install(bad, Installed{}, nullptr, nullptr);
        CHECK(r.error == InstallError::Forbidden);
        CHECK_EQ(env.downloads, 0);
        Package trav = ftPackage("trav", "1", makeZip({{"x", "x"}}), "U: switch/../../evil\n", env);
        CHECK(inst.install(trav, Installed{}, nullptr, nullptr).error == InstallError::Forbidden);

        Package big = ftPackage("big", "1", makeZip({{"switch/big/big.nro", "n"}}), "U: switch/big/big.nro\n", env);
        big.extracted_bytes = 50 * 1024 * 1024;
        env.free_bytes = 10 * 1024 * 1024;
        r = inst.install(big, Installed{}, nullptr, nullptr);
        CHECK(r.error == InstallError::NoSpace);
        CHECK_EQ(env.downloads, 0);
        env.free_bytes = -1;  // unknown: go on

        Package lost = ftPackage("lost", "1", makeZip({{"switch/l/l.nro", "n"}}), "U: switch/l/l.nro\n", env);
        env.served.erase(lost.download_url);
        r = inst.install(lost, Installed{}, nullptr, nullptr);
        CHECK(r.error == InstallError::Network);
        Package nomani = ftPackage("nomani", "1", makeZip({{"switch/n/n.nro", "n"}}), "U: switch/n/n.nro\n", env);
        env.manifests.erase(nomani.manifest_url);
        CHECK(inst.install(nomani, Installed{}, nullptr, nullptr).error == InstallError::Network);

        // 14. Not a zip / nothing from the list in it.
        Package junk = ftPackage("junk", "1", "this is not a zip at all, sorry", "U: switch/j/j.nro\n", env);
        CHECK(inst.install(junk, Installed{}, nullptr, nullptr).error == InstallError::BadArchive);
        Package empty = ftPackage("empty", "1", makeZip({{"other.txt", "o"}}), "U: switch/e/e.nro\n", env);
        CHECK(inst.install(empty, Installed{}, nullptr, nullptr).error == InstallError::BadArchive);
        CHECK(!env.sdHas("switch/e"));
        // 9. A listed file missing from the zip is skipped (libget does the same).
        Package part = ftPackage("part", "1", makeZip({{"switch/p/p.nro", "p"}}), "U: switch/p/p.nro\nU: switch/p/gone.dat\n", env);
        r = inst.install(part, Installed{}, nullptr, nullptr);
        CHECK(r.ok);
        CHECK_EQ(r.missing.size(), size_t(1));
        CHECK(env.sdHas("switch/p/p.nro"));
        CHECK(noLeftovers(env.root));
    }

    // 7./8./12. GitHub packages: zip with a wrapping folder, single .nro, registry folder case kept.
    {
        Env env(tmp);
        Installer inst = env.make();
        const std::string z = makeZip({{"SdOut/switch/.overlays/ovlmenu.ovl", "menu"},
                                       {"SdOut/config/ultrahand/config.ini", "cfg"},
                                       {"SdOut/README.md", "not listed"}});
        Package gh;
        gh.name = "UltrahandOverlay";
        gh.version = "2.5.4";
        gh.source = Source::Github;
        gh.download_type = DownloadType::Zip;
        gh.zip_root = "SdOut/";
        gh.download_url = "https://github.test/sdout.zip";
        gh.sha256 = sha256Of(z);
        std::string err;
        parseManifest("U: switch/.overlays/ovlmenu.ovl\nG: config/ultrahand/config.ini\n", gh.files, err);
        env.served[gh.download_url] = z;
        InstallResult r = inst.install(gh, Installed{}, nullptr, nullptr);
        CHECK(r.ok);
        CHECK_EQ(env.sdFile("switch/.overlays/ovlmenu.ovl"), std::string("menu"));
        CHECK_EQ(env.sdFile("config/ultrahand/config.ini"), std::string("cfg"));
        CHECK(!env.sdHas("README.md") && !env.sdHas("SdOut"));
        CHECK_EQ(readFile(env.reg + "/UltrahandOverlay/manifest.install"),
                 std::string("U: switch/.overlays/ovlmenu.ovl\nG: config/ultrahand/config.ini\n"));
        CHECK(readFile(env.reg + "/UltrahandOverlay/info.json").find("\"github\"") != std::string::npos);

        Package nro;
        nro.name = "Goldleaf";
        nro.version = "1.2.2";
        nro.download_type = DownloadType::File;
        nro.download_url = "https://github.test/Goldleaf.nro";
        parseManifest("U: switch/Goldleaf/Goldleaf.nro\n", nro.files, err);
        env.served[nro.download_url] = "NRO0-goldleaf";
        fs::create_directories(env.reg + "/goldleaf");  // made by the official client in lower case
        writeFile(env.reg + "/goldleaf/manifest.install", "U: switch/Goldleaf/Goldleaf.nro\n");
        r = inst.install(nro, registry(env, "goldleaf"), nullptr, nullptr);
        CHECK(r.ok);
        CHECK_EQ(env.sdFile("switch/Goldleaf/Goldleaf.nro"), std::string("NRO0-goldleaf"));
        CHECK_EQ(parseInfoVersion(readFile(env.reg + "/goldleaf/info.json")), std::string("1.2.2"));
        int goldleaf_dirs = 0;
        for (const auto& e : fs::directory_iterator(env.reg))
            if (fs::path(e.path()).filename().string().size() == 8 &&
                (e.path().filename() == "goldleaf" || e.path().filename() == "Goldleaf"))
                ++goldleaf_dirs;
        CHECK_EQ(goldleaf_dirs, 1);
        CHECK(fs::is_empty(env.tmp));
    }

    // 13. Dependencies through the catalog.
    {
        Env env(tmp);
        const std::string dir = argc > 1 ? argv[1] : "pctest/homebrew_fixtures";
        CatalogConfig cc;
        cc.cache_dir = env.root + "/cache";
        cc.registry_dir = env.reg;
        cc.sd_root = env.sd;
        cc.fortheusers_url = "ft";
        cc.index_url = "idx";
        const std::string ft = readFile(dir + "/fortheusers_repo.json"), own = readFile(dir + "/homebrew_own.json");
        Catalog cat(cc, [&](const std::string& u, const std::string&) {
            return FetchResult{200, u == "ft" ? ft : own, ""};
        }, [] { return 1LL; });
        CHECK(cat.refresh().ok);
        Package smo;
        CHECK(cat.find("Status-Monitor-Overlay", smo));
        std::vector<Package> deps = missingDependencies(smo, cat);
        CHECK_EQ(deps.size(), size_t(2));  // nx-ovlloader, Ultrahand
        writeFile(env.reg + "/nx-ovlloader/manifest.install", "U: atmosphere/contents/420000000007E51A/exefs.nsp\n");
        cat.rescanInstalled();
        deps = missingDependencies(smo, cat);
        CHECK_EQ(deps.size(), size_t(1));
        if (!deps.empty()) CHECK_EQ(deps[0].name, std::string("Ultrahand"));
        Package ultra;
        CHECK(cat.find("Ultrahand", ultra) && missingDependencies(ultra, cat).empty());
    }

    // Real fortheusers package, if given: our SHA-256 and unzip against their data.
    if (argc > 2) {
        Env env(tmp);
        Installer inst = env.make();
        const std::string dir = argv[1];
        std::vector<Package> ft;
        std::string err;
        CHECK(parseFortheusersRepo(readFile(dir + "/fortheusers_repo.json"), ft, err));
        Package smo;
        for (const auto& p : ft)
            if (p.name == "Status-Monitor-Overlay") smo = p;
        env.served[smo.download_url] = readFile(argv[2]);
        env.manifests[smo.manifest_url] = readFile(dir + "/manifests/Status-Monitor-Overlay.install");
        InstallResult r = inst.install(smo, Installed{}, nullptr, nullptr);
        CHECK(r.ok);
        if (!r.ok) std::cerr << "     " << installErrorName(r.error) << ": " << r.message << "\n";
        CHECK_EQ(fs::file_size(env.sd + "switch/.overlays/Status-Monitor-Overlay.ovl"), uintmax_t(467000));
        std::cout << "real package: Status-Monitor-Overlay " << smo.version << " installed, sha256 verified\n";
    }

    fs::remove_all(tmp);
    std::cout << (g_failed ? "FAILED " : "OK ") << (g_checks - g_failed) << "/" << g_checks << " installer checks\n";
    return g_failed ? 1 : 0;
}
