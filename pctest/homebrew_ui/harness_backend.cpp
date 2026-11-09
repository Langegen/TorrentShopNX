// Backends of the PC window: catalog from local files, installs of stand-in
// packages into ./hb_ui_sd/, icons from a local folder, no retro tab.

#include <borealis.hpp>
#include <borealis/extern/nlohmann/json.hpp>
#include <zlib.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include "catalog/homebrew_catalog.hpp"
#include "catalog/homebrew_installer.hpp"
#include "ui/homebrew/HomebrewGlue.hpp"

namespace fs = std::filesystem;

namespace {

std::string dataDir() {
    const char* d = std::getenv("HB_UI_DATA");
    return d ? d : "hb_ui_data";
}

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Stand-in zips do not match the real checksums: drop them from the data.
std::string withoutChecksums(const std::string& text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.contains("packages")) return text;
    for (auto& p : j["packages"]) {
        p.erase("md5");
        p.erase("sha256");
    }
    return j.dump();
}

void put16(std::string& s, uint32_t v) { s += char(v & 0xff); s += char((v >> 8) & 0xff); }
void put32(std::string& s, uint32_t v) { put16(s, v & 0xffff); put16(s, v >> 16); }

// Stored (uncompressed) zip with the given names.
std::string makeZip(const std::vector<std::string>& names) {
    std::string out, cd;
    for (const auto& name : names) {
        const std::string data = "stand-in for " + name + "\n";
        const uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
        const uint32_t off = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0x21);
        put32(out, crc); put32(out, uint32_t(data.size())); put32(out, uint32_t(data.size()));
        put16(out, uint32_t(name.size())); put16(out, 0);
        out += name + data;
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0x21);
        put32(cd, crc); put32(cd, uint32_t(data.size())); put32(cd, uint32_t(data.size()));
        put16(cd, uint32_t(name.size())); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0);
        put32(cd, off);
        cd += name;
    }
    const uint32_t cd_off = static_cast<uint32_t>(out.size());
    out += cd;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, uint32_t(names.size()));
    put16(out, uint32_t(names.size())); put32(out, uint32_t(cd.size())); put32(out, cd_off); put16(out, 0);
    return out;
}

homebrew::FetchResult localFetch(const std::string& url, const std::string&) {
    if (url == "local:repo.json" || url == "local:homebrew.json") {
        const std::string text = readAll(dataDir() + "/" + url.substr(6));
        if (text.empty()) return {404, "", ""};
        return {200, withoutChecksums(text), ""};
    }
    // fortheusers manifest: a saved copy, else one .nro named after the package.
    const size_t p = url.find("/packages/");
    if (p != std::string::npos) {
        std::string name = url.substr(p + 10);
        name = name.substr(0, name.find('/'));
        const std::string saved = readAll(dataDir() + "/manifests/" + name + ".install");
        if (!saved.empty()) return {200, saved, ""};
        return {200, "U: switch/" + name + "/" + name + ".nro\n", ""};
    }
    return {404, "", ""};
}

// Remembers the file list of the package being installed, for the stand-in zip.
std::vector<std::string> g_lastFiles;

}  // namespace

namespace homebrew {

Catalog& Catalog::instance() {
    static Catalog c(CatalogConfig{"hb_ui_cache", "hb_ui_sd/switch/appstore/.get/packages", "hb_ui_sd/",
                                   "local:repo.json", "local:homebrew.json"},
                     localFetch, [] { return static_cast<long long>(std::time(nullptr)); });
    return c;
}

Installer& Installer::instance() {
    static Installer inst(
        InstallerConfig{"hb_ui_sd/", "hb_ui_sd/switch/appstore/.get/packages", "hb_ui_sd/switch/TorrentShopNX/cache/tmp"},
        [](const std::string& url, const std::string& etag) {
            FetchResult r = localFetch(url, etag);
            g_lastFiles.clear();
            std::vector<ManifestEntry> m;
            std::string err;
            if (r.status == 200 && parseManifest(r.body, m, err))
                for (const auto& e : m) g_lastFiles.push_back(e.path);
            return r;
        },
        [](const std::string& url, const std::string& dest, const std::function<void(int64_t, int64_t)>& progress,
           const std::atomic<bool>* cancel, std::string& err) {
            if (url.find("fail") != std::string::npos) {
                err = "stand-in failure";
                return false;
            }
            // ~2 s of "downloading" so the progress bar can be seen.
            for (int i = 1; i <= 20; ++i) {
                if (cancel && cancel->load()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (progress) progress(i * 5, 100);
            }
            // Same layout as the real package: its file list (inline for GitHub
            // packages, the manifest for fortheusers ones), inside zip_root.
            const auto pkgs = Catalog::instance().snapshot();
            const Package* pkg = nullptr;
            for (const auto& p : *pkgs)
                if (p.download_url == url) pkg = &p;
            if (pkg && pkg->download_type == DownloadType::File) {
                std::ofstream(dest, std::ios::binary) << "NRO0 stand-in\n";
                return true;
            }
            std::vector<std::string> names;
            if (pkg && !pkg->files.empty())
                for (const auto& f : pkg->files) names.push_back(pkg->zip_root + f.path);
            else
                names = g_lastFiles;
            std::ofstream(dest, std::ios::binary) << makeZip(names);
            return true;
        },
        [](const std::string&) { return int64_t(-1); });
    return inst;
}

}  // namespace homebrew

// Borealis of this project reports double frees through the app's UI watchdog.
namespace uiwd {
void logCurrentStack(const char* tag) { brls::Logger::error("double free: {}", tag ? tag : ""); }
}  // namespace uiwd

namespace ui::hb {

void loadImage(brls::Image* img, const std::string& url, const std::shared_ptr<bool>&, bool, int, int) {
    if (!img) return;
    std::string safe = url;
    for (char& c : safe)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    for (const char* ext : {".jpg", ".png"}) {
        const std::string path = dataDir() + "/icons/" + safe + ext;
        if (!url.empty() && fs::exists(path)) {
            img->setImageFromFile(path);
            return;
        }
    }
    img->setImageFromFile(std::string(BRLS_RESOURCES) + "img/cover_blank.png");
}

void haptic(int) {}

static std::string g_section = "homebrew";
std::string currentSection() { return g_section; }
void rememberSection(const std::string& tab) { g_section = tab; }

void showRetroSection() { brls::Application::notify("Retro tab: not part of the PC window"); }

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void packagesChanged() {}

std::string sdRoot() { return "hb_ui_sd/"; }

// Pretend pm services: modules "run" once started, until stopped.
static std::set<std::string> g_running;
bool canControlSysmodules() { return true; }
bool sysmoduleRunning(const std::string& tid) { return g_running.count(tid) > 0; }
bool startSysmodule(const std::string& tid, std::string&) {
    g_running.insert(tid);
    return true;
}
bool stopSysmodule(const std::string& tid, std::string&) {
    g_running.erase(tid);
    return true;
}

}  // namespace ui::hb
