#include "homebrew_package.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace homebrew {

using nlohmann::json;

namespace {

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool isHex16(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// String value of a field whatever its JSON type ("" when missing or null).
std::string str(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_float()) {
        std::ostringstream os;
        os << it->get<double>();
        return os.str();
    }
    if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
    return "";
}

int64_t num(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        char* end = nullptr;
        long long v = std::strtoll(s.c_str(), &end, 10);
        return (end && end != s.c_str()) ? v : 0;
    }
    return 0;
}

std::vector<std::string> strList(const json& obj, const char* key) {
    std::vector<std::string> out;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return out;
    for (const auto& v : *it)
        if (v.is_string() && !v.get<std::string>().empty()) out.push_back(v.get<std::string>());
    return out;
}

std::map<std::string, std::string> strMap(const json& obj, const char* key) {
    std::map<std::string, std::string> out;
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) return out;
    for (auto e = it->begin(); e != it->end(); ++e)
        if (e.value().is_string() && !e.value().get<std::string>().empty())
            out[e.key()] = e.value().get<std::string>();
    return out;
}

// fortheusers stores line breaks as the two characters '\' 'n'.
std::string unescapeNewlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string urlSegment(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

unsigned kindsFromNames(const std::vector<std::string>& names) {
    unsigned k = 0;
    for (const auto& n : names) {
        const std::string l = toLower(n);
        if (l == "app") k |= KindApp;
        else if (l == "overlay") k |= KindOverlay;
        else if (l == "sysmodule") k |= KindSysmodule;
    }
    return k;
}

// Days since 1970-01-01 (Howard Hinnant's days_from_civil).
long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

bool parseIsoDate(const std::string& s, int& y, unsigned& m, unsigned& d) {
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    y = std::stoi(s.substr(0, 4));
    m = static_cast<unsigned>(std::stoi(s.substr(5, 2)));
    d = static_cast<unsigned>(std::stoi(s.substr(8, 2)));
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

int rank(PathVerdict v) {
    switch (v) {
        case PathVerdict::Allowed: return 0;
        case PathVerdict::Warn: return 1;
        case PathVerdict::Forbidden: return 2;
    }
    return 0;
}

std::string dirName(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? "" : p.substr(0, s);
}

}  // namespace

// ---------------------------------------------------------------------------

std::string normalizeRelPath(const std::string& in) {
    std::string s = trim(in);
    std::replace(s.begin(), s.end(), '\\', '/');
    if (startsWith(toLower(s), "sdmc:")) s = s.substr(5);
    if (s.find(':') != std::string::npos) return "";

    std::string out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        const std::string seg = s.substr(i, j - i);
        if (seg == "." || seg == "..") return "";
        if (!seg.empty()) {
            if (!out.empty()) out += '/';
            out += seg;
        }
        i = j + 1;
    }
    return out;
}

bool parseManifest(const std::string& text, std::vector<ManifestEntry>& out, std::string& err) {
    out.clear();
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        line = trim(line);
        if (line.size() < 3 || line[1] != ':') continue;
        ManifestEntry e;
        switch (line[0]) {
            case 'U': e.op = ManifestOp::Update; break;
            case 'E': e.op = ManifestOp::Extract; break;
            case 'G': e.op = ManifestOp::Get; break;
            case 'L': e.op = ManifestOp::Local; break;
            default: continue;
        }
        e.path = normalizeRelPath(line.substr(2));
        if (e.path.empty()) {
            err = "unsafe path in manifest: " + line;
            out.clear();
            return false;
        }
        out.push_back(std::move(e));
    }
    if (out.empty()) {
        err = "manifest has no entries";
        return false;
    }
    return true;
}

std::string formatManifest(const std::vector<ManifestEntry>& entries) {
    std::string out;
    for (const auto& e : entries) {
        char op = 'U';
        switch (e.op) {
            case ManifestOp::Update: op = 'U'; break;
            case ManifestOp::Extract: op = 'E'; break;
            case ManifestOp::Get: op = 'G'; break;
            case ManifestOp::Local: op = 'L'; break;
        }
        out += op;
        out += ": ";
        out += e.path;
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------

PathCheck checkInstallPath(const std::string& rel_path) {
    const std::string p = toLower(normalizeRelPath(rel_path));
    if (p.empty()) return {PathVerdict::Forbidden, "unsafe path"};

    const size_t slash = p.find('/');
    if (slash == std::string::npos) {
        // A file in the SD root: payloads and boot files live here.
        static const char* boot_ext[] = {".bin", ".dat", ".ini", ".kip", ".nsp", ".bmp"};
        for (const char* ext : boot_ext)
            if (endsWith(p, ext)) return {PathVerdict::Forbidden, "boot file in SD root"};
        return {PathVerdict::Warn, "file in SD root"};
    }

    const std::string top = p.substr(0, slash);
    if (top == "bootloader") return {PathVerdict::Forbidden, "bootloader"};
    if (top == "emummc" || top == "nintendo") return {PathVerdict::Forbidden, "system data"};
    if (top == "payloads") return {PathVerdict::Forbidden, "payloads"};
    if (startsWith(p, "switch/torrentshopnx/")) return {PathVerdict::Forbidden, "TorrentShopNX files"};
    if (startsWith(p, "switch/appstore/.get/")) return {PathVerdict::Forbidden, "package registry"};

    if (top == "atmosphere") {
        static const char* core[] = {"atmosphere/package3", "atmosphere/stratosphere.romfs",
                                     "atmosphere/fusee", "atmosphere/hbl.nsp", "atmosphere/kips/",
                                     "atmosphere/reboot_payload.bin", "atmosphere/exosphere"};
        for (const char* c : core)
            if (startsWith(p, c)) return {PathVerdict::Forbidden, "Atmosphere core"};

        if (startsWith(p, "atmosphere/contents/")) {
            const std::string rest = p.substr(20);
            const size_t s2 = rest.find('/');
            const std::string tid = rest.substr(0, s2);
            if (s2 == std::string::npos || !isHex16(tid)) return {PathVerdict::Warn, "atmosphere/contents"};
            // 0100000000000000-01000000000010FF: system modules and applets.
            if (startsWith(tid, "01000000000") && tid.size() == 16) {
                const unsigned long long v = std::stoull(tid, nullptr, 16);
                if (v <= 0x01000000000010FFull) return {PathVerdict::Warn, "system title override"};
            }
            return {PathVerdict::Allowed, ""};
        }
        return {PathVerdict::Warn, "atmosphere"};
    }
    return {PathVerdict::Allowed, ""};
}

FileAnalysis analyzeFiles(const std::vector<ManifestEntry>& files) {
    FileAnalysis a;
    for (const auto& f : files) {
        const PathCheck c = checkInstallPath(f.path);
        if (rank(c.verdict) > rank(a.worst)) {
            a.worst = c.verdict;
            a.worst_path = f.path;
            a.worst_reason = c.reason;
        }
        const std::string l = toLower(f.path);
        if (startsWith(l, "switch/.overlays/") && endsWith(l, ".ovl")) {
            a.kinds |= KindOverlay;
        } else if (startsWith(l, "switch/") && endsWith(l, ".nro")) {
            a.kinds |= KindApp;
            a.nro_paths.push_back(f.path);
        } else if (startsWith(l, "atmosphere/contents/") && endsWith(l, "/exefs.nsp")) {
            const std::string tid = l.substr(20, 16);
            if (isHex16(tid) && l.size() == 20 + 16 + 10) {
                a.kinds |= KindSysmodule;
                a.sysmodule_tids.push_back(toUpper(tid));
            }
        }
    }
    a.simple = files.size() == 1 && a.worst == PathVerdict::Allowed &&
               (a.kinds == KindApp || a.kinds == KindOverlay);
    return a;
}

// ---------------------------------------------------------------------------

namespace {

std::vector<long long> versionParts(const std::string& raw) {
    std::string s = trim(raw);
    // dd/mm/yyyy
    int dd = 0, mm = 0, yy = 0;
    char tail = 0;
    if (std::sscanf(s.c_str(), "%d/%d/%d%c", &dd, &mm, &yy, &tail) == 3 && yy > 1900 && mm >= 1 &&
        mm <= 12 && dd >= 1 && dd <= 31)
        return {static_cast<long long>(yy) * 10000 + mm * 100 + dd};
    int y = 0;
    unsigned m = 0, d = 0;
    if (parseIsoDate(s, y, m, d) && (s.size() == 10)) return {static_cast<long long>(y) * 10000 + m * 100 + d};

    std::vector<long long> parts;
    long long cur = -1;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            cur = (cur < 0 ? 0 : cur) * 10 + (c - '0');
            if (cur > 1000000000000LL) cur = 1000000000000LL;
        } else if (cur >= 0) {
            parts.push_back(cur);
            cur = -1;
        }
    }
    if (cur >= 0) parts.push_back(cur);
    return parts;
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    const std::vector<long long> pa = versionParts(a), pb = versionParts(b);
    if (pa.empty() || pb.empty()) return trim(a) == trim(b) ? 0 : 1;
    const size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        const long long x = i < pa.size() ? pa[i] : 0;
        const long long y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::string githubRepoFromUrl(const std::string& url) {
    const std::string l = toLower(trim(url));
    const size_t at = l.find("github.com/");
    if (at == std::string::npos) return "";
    // Host must be exactly github.com or www.github.com (not notgithub.com).
    const bool host_ok = at == 0 || l[at - 1] == '/' || (at >= 4 && l.compare(at - 4, 4, "www.") == 0 &&
                                                          (at == 4 || l[at - 5] == '/'));
    if (!host_ok) return "";
    std::string rest = l.substr(at + 11);
    const size_t s1 = rest.find('/');
    if (s1 == std::string::npos || s1 == 0) return "";
    std::string owner = rest.substr(0, s1);
    std::string repo = rest.substr(s1 + 1);
    const size_t end = repo.find_first_of("/?#");
    if (end != std::string::npos) repo = repo.substr(0, end);
    if (endsWith(repo, ".git")) repo = repo.substr(0, repo.size() - 4);
    if (repo.empty()) return "";
    return owner + "/" + repo;
}

std::string normalizeDate(const std::string& date) {
    const std::string s = trim(date);
    int dd = 0, mm = 0, yy = 0;
    char tail = 0;
    if (std::sscanf(s.c_str(), "%d/%d/%d%c", &dd, &mm, &yy, &tail) == 3 && yy > 1900 && mm >= 1 &&
        mm <= 12 && dd >= 1 && dd <= 31) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", yy, mm, dd);
        return buf;
    }
    int y = 0;
    unsigned m = 0, d = 0;
    if (parseIsoDate(s, y, m, d)) return s.substr(0, 10);
    return "";
}

int daysBetween(const std::string& a, const std::string& b) {
    int ya = 0, yb = 0;
    unsigned ma = 0, da = 0, mb = 0, db = 0;
    if (!parseIsoDate(a, ya, ma, da) || !parseIsoDate(b, yb, mb, db)) return 0;
    return static_cast<int>(daysFromCivil(yb, mb, db) - daysFromCivil(ya, ma, da));
}

// ---------------------------------------------------------------------------

bool parseFortheusersRepo(const std::string& text, std::vector<Package>& out, std::string& err,
                          const std::string& cdn) {
    out.clear();
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "repo.json is not valid JSON";
        return false;
    }
    auto it = j.find("packages");
    if (it == j.end() || !it->is_array()) {
        err = "repo.json has no packages";
        return false;
    }
    for (const auto& p : *it) {
        if (!p.is_object()) continue;
        Package pkg;
        pkg.name = trim(str(p, "name"));
        if (pkg.name.empty()) continue;
        pkg.title = trim(str(p, "title"));
        if (pkg.title.empty()) pkg.title = pkg.name;
        pkg.author = str(p, "author");
        pkg.version = trim(str(p, "version"));
        pkg.category = str(p, "category");
        pkg.description = unescapeNewlines(str(p, "description"));
        pkg.details = unescapeNewlines(str(p, "details"));
        pkg.changelog = unescapeNewlines(str(p, "changelog"));
        pkg.license = str(p, "license");
        pkg.url = str(p, "url");
        pkg.repo = githubRepoFromUrl(pkg.url);
        pkg.binary = str(p, "binary");
        pkg.updated = normalizeDate(str(p, "updated"));
        pkg.download_bytes = num(p, "filesize") * 1024;
        pkg.extracted_bytes = num(p, "extracted") * 1024;
        pkg.downloads = num(p, "app_dls");
        pkg.md5 = toLower(str(p, "md5"));
        pkg.sha256 = toLower(str(p, "sha256"));
        pkg.source = Source::Fortheusers;
        pkg.download_type = DownloadType::Zip;
        const std::string seg = urlSegment(pkg.name);
        pkg.download_url = cdn + "/zips/" + seg + ".zip";
        pkg.icon_url = cdn + "/packages/" + seg + "/icon.png";
        pkg.screen_url = cdn + "/packages/" + seg + "/screen.png";
        pkg.manifest_url = cdn + "/packages/" + seg + "/manifest.install";
        out.push_back(std::move(pkg));
    }
    if (out.empty()) {
        err = "repo.json has no usable packages";
        return false;
    }
    return true;
}

bool parseOwnIndex(const std::string& text, std::vector<Package>& out, IndexExtras& extras,
                   std::string& err) {
    out.clear();
    extras = IndexExtras{};
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "homebrew.json is not valid JSON";
        return false;
    }

    for (const auto& h : strList(j, "hidden")) extras.hidden.insert(toLower(trim(h)));
    extras.overlay_dependencies = strList(j, "overlay_dependencies");

    auto ft = j.find("fortheusers");
    if (ft != j.end() && ft->is_object()) {
        for (auto e = ft->begin(); e != ft->end(); ++e) {
            if (!e.value().is_object()) continue;
            PackageMeta meta;
            if (e.value().contains("kinds")) {
                meta.kinds = kindsFromNames(strList(e.value(), "kinds"));
                meta.kinds_known = true;
            }
            meta.depends = strList(e.value(), "depends");
            meta.description_i18n = strMap(e.value(), "description_i18n");
            extras.fortheusers_meta[toLower(e.key())] = std::move(meta);
        }
    }

    auto it = j.find("packages");
    if (it == j.end() || !it->is_array()) {
        err = "homebrew.json has no packages";
        return false;
    }
    std::set<std::string> seen;
    for (const auto& p : *it) {
        if (!p.is_object()) continue;
        Package pkg;
        pkg.name = trim(str(p, "name"));
        pkg.version = trim(str(p, "version"));
        pkg.download_url = trim(str(p, "download_url"));
        if (pkg.name.empty() || pkg.version.empty() || pkg.download_url.empty()) continue;
        if (!seen.insert(toLower(pkg.name)).second) continue;

        std::string files_text;
        for (const auto& f : strList(p, "files")) files_text += f + "\n";
        std::string ferr;
        if (!parseManifest(files_text, pkg.files, ferr)) continue;
        const FileAnalysis fa = analyzeFiles(pkg.files);
        if (fa.worst == PathVerdict::Forbidden) continue;

        pkg.download_type = toLower(str(p, "download_type")) == "file" ? DownloadType::File : DownloadType::Zip;
        if (pkg.download_type == DownloadType::File && pkg.files.size() != 1) continue;
        const std::string root = str(p, "zip_root");
        if (!root.empty()) {
            pkg.zip_root = normalizeRelPath(root);
            if (pkg.zip_root.empty()) continue;  // "../" and the like
            pkg.zip_root += '/';
        }

        pkg.title = trim(str(p, "title"));
        if (pkg.title.empty()) pkg.title = pkg.name;
        pkg.author = str(p, "author");
        pkg.category = str(p, "category");
        pkg.description = str(p, "description");
        pkg.details = str(p, "details");
        pkg.changelog = str(p, "changelog");
        pkg.license = str(p, "license");
        pkg.url = str(p, "url");
        pkg.repo = toLower(trim(str(p, "repo")));
        if (pkg.repo.empty()) pkg.repo = githubRepoFromUrl(pkg.url);
        pkg.updated = normalizeDate(str(p, "updated"));
        pkg.first_seen = normalizeDate(str(p, "first_seen"));
        auto af = p.find("auto");
        pkg.auto_found = af != p.end() && af->is_boolean() && af->get<bool>();
        pkg.download_bytes = num(p, "filesize");
        pkg.extracted_bytes = num(p, "extracted");
        pkg.md5 = toLower(str(p, "md5"));
        pkg.sha256 = toLower(str(p, "sha256"));
        pkg.icon_url = str(p, "icon_url");
        pkg.screen_url = str(p, "screen_url");
        pkg.source = Source::Github;
        pkg.kinds = fa.kinds;
        pkg.depends = strList(p, "depends");
        pkg.description_i18n = strMap(p, "description_i18n");
        out.push_back(std::move(pkg));
    }
    return true;
}

std::vector<Package> mergeSources(const std::vector<Package>& fortheusers,
                                  const std::vector<Package>& own, const IndexExtras& extras) {
    std::vector<Package> result = fortheusers;

    std::unordered_map<std::string, size_t> by_repo, by_nro, by_name;
    for (size_t i = 0; i < result.size(); ++i) {
        Package& p = result[i];
        auto meta = extras.fortheusers_meta.find(toLower(p.name));
        if (meta != extras.fortheusers_meta.end()) {
            if (meta->second.kinds_known) p.kinds = meta->second.kinds;
            if (!meta->second.depends.empty()) p.depends = meta->second.depends;
            for (const auto& d : meta->second.description_i18n) p.description_i18n[d.first] = d.second;
        }
        if (!p.repo.empty()) by_repo.emplace(p.repo, i);
        const std::string bin = toLower(normalizeRelPath(p.binary));
        if (endsWith(bin, ".nro")) by_nro.emplace(bin, i);
        by_name.emplace(toLower(p.name), i);
    }

    // Same repo, then same name (the shared registry key), then the same main
    // .nro path. Name goes before .nro: a package may ship another project's
    // NRO (nx-ovlloader carries Ultrahand-Reload.nro).
    for (const Package& o : own) {
        size_t idx = result.size();
        if (!o.repo.empty()) {
            auto f = by_repo.find(o.repo);
            if (f != by_repo.end()) idx = f->second;
        }
        if (idx == result.size()) {
            auto f = by_name.find(toLower(o.name));
            if (f != by_name.end()) idx = f->second;
        }
        if (idx == result.size()) {
            for (const auto& f : o.files) {
                auto m = by_nro.find(toLower(f.path));
                if (m != by_nro.end()) {
                    idx = m->second;
                    break;
                }
            }
        }

        if (idx == result.size()) {
            result.push_back(o);
            by_name.emplace(toLower(o.name), result.size() - 1);
            continue;
        }

        Package& p = result[idx];
        if (p.repo.empty()) p.repo = o.repo;
        if (!o.depends.empty()) p.depends = o.depends;
        for (const auto& d : o.description_i18n) p.description_i18n[d.first] = d.second;
        if (p.icon_url.empty()) p.icon_url = o.icon_url;
        if (compareVersions(o.version, p.version) > 0 && versionParts(o.version).size() > 0) {
            p.version = o.version;
            if (!o.updated.empty()) p.updated = o.updated;
            if (!o.changelog.empty()) p.changelog = o.changelog;
            p.download_url = o.download_url;
            p.download_type = o.download_type;
            p.sha256 = o.sha256;
            p.md5 = o.md5;
            p.download_bytes = o.download_bytes;
            p.extracted_bytes = o.extracted_bytes;
            p.files = o.files;
            p.zip_root = o.zip_root;
            p.manifest_url.clear();
            if (o.kinds) p.kinds = o.kinds;
            p.source = Source::Github;
        }
    }

    // Drop hidden packages.
    result.erase(std::remove_if(result.begin(), result.end(),
                                [&](const Package& p) {
                                    return extras.hidden.count(toLower(p.name)) ||
                                           (!p.repo.empty() && extras.hidden.count(p.repo));
                                }),
                 result.end());

    std::set<std::string> names;
    for (const auto& p : result) names.insert(toLower(p.name));
    std::set<std::string> overlay_deps;
    for (const auto& d : extras.overlay_dependencies) overlay_deps.insert(toLower(d));

    for (Package& p : result) {
        if ((p.kinds & KindOverlay) && p.depends.empty() && !overlay_deps.count(toLower(p.name)))
            p.depends = extras.overlay_dependencies;
        // Only dependencies that exist in the store, never the package itself.
        p.depends.erase(std::remove_if(p.depends.begin(), p.depends.end(),
                                       [&](const std::string& d) {
                                           const std::string l = toLower(d);
                                           return !names.count(l) || l == toLower(p.name);
                                       }),
                        p.depends.end());
    }

    std::stable_sort(result.begin(), result.end(), [](const Package& a, const Package& b) {
        return toLower(a.title) < toLower(b.title);
    });
    return result;
}

bool isNew(const Package& pkg, const std::string& today, int days) {
    if (!pkg.auto_found || pkg.first_seen.empty()) return false;
    int y = 0;
    unsigned m = 0, d = 0;
    if (!parseIsoDate(today, y, m, d)) return false;
    const int age = daysBetween(pkg.first_seen, today);
    return age >= 0 && age < days;
}

// ---------------------------------------------------------------------------

Status statusFor(const Package& pkg, const Installed& inst) {
    if (!inst.present) return Status::NotInstalled;
    if (inst.version.empty()) return Status::UpdateAvailable;  // libget: manifest without info
    return compareVersions(pkg.version, inst.version) > 0 ? Status::UpdateAvailable : Status::Installed;
}

std::string parseInfoVersion(const std::string& text) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "";
    return trim(str(j, "version"));
}

std::string makeInfoJson(const Package& pkg) {
    json j;
    j["version"] = pkg.version;
    j["title"] = pkg.title;
    j["author"] = pkg.author;
    j["url"] = pkg.url;
    j["installed_by"] = "TorrentShopNX";
    j["source"] = pkg.source == Source::Github ? "github" : "fortheusers";
    return j.dump();
}

InstallPlan planInstall(const std::vector<ManifestEntry>& new_manifest,
                        const std::vector<ManifestEntry>& old_manifest,
                        const std::function<bool(const std::string&)>& exists) {
    InstallPlan plan;
    std::set<std::string> incoming;
    for (const auto& e : new_manifest) {
        incoming.insert(toLower(e.path));
        switch (e.op) {
            case ManifestOp::Update:
            case ManifestOp::Extract:
                plan.write.push_back(e);
                break;
            case ManifestOp::Get:
                if (exists && exists(e.path)) plan.keep_existing.push_back(e.path);
                else plan.write.push_back(e);
                break;
            case ManifestOp::Local:
                break;
        }
    }
    for (const auto& e : old_manifest)
        if (e.op == ManifestOp::Update && !incoming.count(toLower(e.path))) plan.remove_stale.push_back(e.path);
    return plan;
}

RemovalPlan planRemoval(const std::vector<ManifestEntry>& manifest, bool keep_settings) {
    RemovalPlan plan;
    std::set<std::string> dirs;
    static const std::set<std::string> protected_dirs = {
        "switch", "switch/.overlays", "config", "atmosphere", "atmosphere/contents",
        "atmosphere/exefs_patches", "atmosphere/kip_patches", "themes", "roms"};

    for (const auto& e : manifest) {
        const std::string l = toLower(e.path);
        bool keep = e.op == ManifestOp::Extract;
        if (keep_settings && (e.op == ManifestOp::Get || startsWith(l, "config/"))) keep = true;
        if (keep) {
            plan.kept.push_back(e.path);
            continue;
        }
        plan.remove_files.push_back(e.path);
        for (std::string d = dirName(e.path); !d.empty(); d = dirName(d)) {
            if (d.find('/') == std::string::npos) break;  // never a top-level folder
            if (protected_dirs.count(toLower(d))) break;
            dirs.insert(d);
        }
    }
    plan.remove_dirs.assign(dirs.begin(), dirs.end());
    std::stable_sort(plan.remove_dirs.begin(), plan.remove_dirs.end(), [](const std::string& a, const std::string& b) {
        const auto da = std::count(a.begin(), a.end(), '/');
        const auto db = std::count(b.begin(), b.end(), '/');
        return da != db ? da > db : a < b;
    });
    return plan;
}

}  // namespace homebrew
