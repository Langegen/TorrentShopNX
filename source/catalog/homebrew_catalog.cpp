#include "homebrew_catalog.hpp"

#include "../utils/atomic_file.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

namespace homebrew {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    return s.substr(b);
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool readFile(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

int makeDir(const std::string& p) {
#if defined(_WIN32)
    return _mkdir(p.c_str());
#else
    return mkdir(p.c_str(), 0777);
#endif
}

// mkdir -p that also works with "sdmc:/" paths.
void ensureDir(const std::string& path) {
    std::string cur;
    size_t pos = 0;
    const size_t colon = path.find(":/");
    if (colon != std::string::npos) {
        cur = path.substr(0, colon + 2);
        pos = colon + 2;
    } else if (!path.empty() && path[0] == '/') {
        cur = "/";
        pos = 1;
    }
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        const std::string part = path.substr(pos, next - pos);
        if (!part.empty() && part != ".") {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += part;
            struct stat st;
            if (stat(cur.c_str(), &st) != 0) makeDir(cur);
        } else if (part == "." && cur.empty()) {
            cur = ".";
        }
        pos = next + 1;
    }
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

bool validRepo(const std::string& body) {
    std::vector<Package> p;
    std::string err;
    return parseFortheusersRepo(body, p, err);
}

bool validIndex(const std::string& body) {
    std::vector<Package> p;
    IndexExtras e;
    std::string err;
    return parseOwnIndex(body, p, e, err);
}

// Main .nro of a package, relative to SD root ("" if it has none).
std::string mainNro(const Package& p) {
    const std::string bin = normalizeRelPath(p.binary);
    if (bin.size() > 4 && lower(bin.substr(bin.size() - 4)) == ".nro") return bin;
    for (const auto& f : p.files) {
        const std::string l = lower(f.path);
        if (l.rfind("switch/", 0) == 0 && l.size() > 4 && l.substr(l.size() - 4) == ".nro" &&
            l.rfind("switch/.overlays/", 0) != 0)
            return f.path;
    }
    return "";
}

constexpr const char* kRepoFile = "repo.json";
constexpr const char* kIndexFile = "homebrew.json";
constexpr const char* kStampFile = "last_check";

}  // namespace

Catalog::Catalog(CatalogConfig cfg, Fetcher fetch, Clock now)
    : cfg_(std::move(cfg)), fetch_(std::move(fetch)), now_(std::move(now)),
      packages_(std::make_shared<PackageList>()), installed_(std::make_shared<InstalledMap>()) {}

bool Catalog::loadCached() {
    std::string err;
    const bool ok = rebuildFromFiles(err);
    if (ok) rescanInstalled();
    return ok;
}

bool Catalog::rebuildFromFiles(std::string& err) {
    std::vector<Package> ft, own;
    IndexExtras extras;
    std::string text, e1, e2;
    const bool have_ft = readFile(join(cfg_.cache_dir, kRepoFile), text) && parseFortheusersRepo(text, ft, e1);
    const bool have_own = readFile(join(cfg_.cache_dir, kIndexFile), text) && parseOwnIndex(text, own, extras, e2);
    if (!have_ft && !have_own) {
        err = "no cached catalog";
        return false;
    }
    auto merged = std::make_shared<const PackageList>(mergeSources(ft, own, extras));
    {
        std::lock_guard<std::mutex> lk(mutex_);
        packages_ = std::move(merged);
    }
    loaded_.store(true);
    generation_.fetch_add(1);
    return true;
}

bool Catalog::fetchOne(const std::string& url, const std::string& file, bool (*validate)(const std::string&),
                       int& status, bool& changed, std::string& err) {
    const std::string path = join(cfg_.cache_dir, file);
    const std::string etag_path = path + ".etag";
    std::string etag;
    if (fileExists(path) && readFile(etag_path, etag)) etag = trimmed(etag);

    const FetchResult r = fetch_ ? fetch_(url, etag) : FetchResult{};
    status = r.status;
    if (r.status == 304 && fileExists(path)) return true;
    if (r.status != 200 || r.body.empty()) {
        err += file + ": HTTP " + std::to_string(r.status) + "; ";
        return false;
    }
    if (!validate(r.body)) {
        err += file + ": invalid data; ";
        return false;
    }
    if (!util::atomicWriteFile(path, r.body)) {
        err += file + ": cannot write; ";
        return false;
    }
    if (r.etag.empty()) std::remove(etag_path.c_str());
    else util::atomicWriteFile(etag_path, r.etag);
    changed = true;
    return true;
}

RefreshResult Catalog::refresh(bool force) {
    std::lock_guard<std::mutex> guard(refresh_mutex_);
    RefreshResult res;
    const long long now = now_ ? now_() : 0;
    const long long last = lastCheck();
    if (!force && loaded_.load() && last > 0 && now >= last && now - last < cfg_.refresh_interval_sec) {
        res.ok = true;
        return res;
    }
    res.attempted = true;
    ensureDir(cfg_.cache_dir);

    bool changed = false;
    const bool ok_ft = fetchOne(cfg_.fortheusers_url, kRepoFile, validRepo, res.fortheusers_status, changed, res.error);
    const bool ok_own = fetchOne(cfg_.index_url, kIndexFile, validIndex, res.index_status, changed, res.error);
    res.ok = ok_ft && ok_own;

    if (changed || !loaded_.load()) {
        std::string err;
        if (rebuildFromFiles(err)) {
            res.changed = true;
            rescanInstalled();
        } else if (res.error.empty()) {
            res.error = err;
        }
    }
    if (res.ok) util::atomicWriteFile(join(cfg_.cache_dir, kStampFile), std::to_string(now));
    return res;
}

void Catalog::rescanInstalled() {
    auto map = std::make_shared<InstalledMap>();

    if (DIR* d = opendir(cfg_.registry_dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name == "." || name == "..") continue;
            const std::string dir = join(cfg_.registry_dir, name);
            std::string text;
            if (!readFile(join(dir, "manifest.install"), text)) continue;  // libget: not installed
            Installed in;
            in.present = true;
            std::string err;
            parseManifest(text, in.manifest, err);  // a broken manifest still means "installed"
            if (readFile(join(dir, "info.json"), text)) in.version = parseInfoVersion(text);
            (*map)[lower(name)] = std::move(in);
        }
        closedir(d);
    }

    // Packages put on the card by hand: no registry entry, but their .nro is there.
    const auto pkgs = snapshot();
    for (const Package& p : *pkgs) {
        const std::string key = lower(p.name);
        if (map->count(key)) continue;
        const std::string nro = mainNro(p);
        if (nro.empty() || !fileExists(cfg_.sd_root + nro)) continue;
        Installed in;
        in.present = true;
        in.untracked = true;
        (*map)[key] = std::move(in);
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        installed_ = std::move(map);
    }
    generation_.fetch_add(1);
}

std::shared_ptr<const PackageList> Catalog::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return packages_;
}

std::shared_ptr<const InstalledMap> Catalog::installedSnapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return installed_;
}

bool Catalog::find(const std::string& name, Package& out) const {
    const std::string key = lower(name);
    for (const Package& p : *snapshot()) {
        if (lower(p.name) == key) {
            out = p;
            return true;
        }
    }
    return false;
}

Installed Catalog::installed(const std::string& name) const {
    const auto map = installedSnapshot();
    auto it = map->find(lower(name));
    return it == map->end() ? Installed{} : it->second;
}

Status Catalog::status(const Package& pkg) const {
    return statusFor(pkg, installed(pkg.name));
}

int Catalog::updatesAvailable() const {
    const auto pkgs = snapshot();
    const auto map = installedSnapshot();
    int n = 0;
    for (const Package& p : *pkgs) {
        auto it = map->find(lower(p.name));
        if (it != map->end() && !it->second.untracked && statusFor(p, it->second) == Status::UpdateAvailable) ++n;
    }
    return n;
}

long long Catalog::lastCheck() const {
    std::string text;
    if (!readFile(join(cfg_.cache_dir, kStampFile), text)) return 0;
    text = trimmed(text);
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    return (end && end != text.c_str()) ? v : 0;
}

}  // namespace homebrew
