#include "retro_vita.h"

#include "../utils/app_paths.h"
#include "../utils/archive_utils.h"
#include "../utils/log.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace catalog {
namespace retro {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint32_t le32(const std::string& d, size_t at) {
    if (at + 4 > d.size()) return 0;
    return static_cast<uint32_t>(static_cast<unsigned char>(d[at])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d[at + 3])) << 24);
}

uint16_t le16(const std::string& d, size_t at) {
    if (at + 2 > d.size()) return 0;
    return static_cast<uint16_t>(static_cast<unsigned char>(d[at]) |
                                 (static_cast<unsigned char>(d[at + 1]) << 8));
}

std::string ux0AppDir() {
    return std::string(TSNX_VITA3K_DIR) + "/vita/ux0/app";
}

// Vita3K installs only these by copying; the rest it decrypts or merges.
bool isPlainApp(const SfoInfo& info) {
    return !info.title_id.empty() && info.category.find("gp") == std::string::npos && info.category != "ac";
}

bool safeTitleId(const std::string& id) {
    if (id.size() < 4 || id.size() > 16) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool hasAnyFile(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) return true;
    }
    return false;
}

// --- archives (.vpk is a zip of the app folder) ---

struct ArchiveApp {
    std::string prefix;       // path inside the archive up to sce_sys/
    bool encrypted = false;
    bool found = false;
};

ArchiveApp findAppInArchive(const fs::path& archive) {
    ArchiveApp app;
    std::vector<std::string> names;
    util::iterateArchiveEntries(archive.string(), [&](const std::string& name, bool) -> bool {
        names.push_back(name);
        return true;
    });
    for (const auto& n : names) {
        std::string l = lower(n);
        const std::string tail = "sce_sys/param.sfo";
        if (l.size() < tail.size() || l.compare(l.size() - tail.size(), tail.size(), tail) != 0) continue;
        std::string prefix = n.substr(0, n.size() - tail.size());
        if (!app.found || prefix.size() < app.prefix.size()) {
            app.prefix = prefix;
            app.found = true;
        }
    }
    if (!app.found) return app;
    std::string lp = lower(app.prefix);
    for (const auto& n : names) {
        std::string l = lower(n);
        if (l.rfind(lp + "sce_pfs/", 0) == 0 || l == lp + "sce_sys/package/work.bin") {
            app.encrypted = true;
            break;
        }
    }
    return app;
}

bool readSfoFromArchive(const fs::path& archive, const std::string& prefix, SfoInfo& info,
                        const std::shared_ptr<std::atomic<bool>>& cancel) {
    fs::path tmp = fs::path(TSNX_CACHE_TMP) / "vita_sfo";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::string target = lower(prefix + "sce_sys/param.sfo");
    std::string err;
    util::extractArchive(archive.string(), tmp.string(), nullptr, cancel, err,
        [&](const std::string& entry, bool isDir) -> std::string {
            return (!isDir && lower(entry) == target) ? "param.sfo" : "";
        });
    std::string data;
    bool ok = readFile(tmp / "param.sfo", data) && parseParamSfo(data, info);
    fs::remove_all(tmp, ec);
    return ok;
}

bool installArchive(const fs::path& archive, const std::shared_ptr<std::atomic<bool>>& cancel,
                    VitaInstallSummary& sum) {
    ArchiveApp app = findAppInArchive(archive);
    if (!app.found || app.encrypted) return false;
    SfoInfo info;
    if (!readSfoFromArchive(archive, app.prefix, info, cancel) || !isPlainApp(info) || !safeTitleId(info.title_id)) {
        return false;
    }
    fs::path dest = fs::path(ux0AppDir()) / info.title_id;
    std::error_code ec;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    std::string lp = lower(app.prefix);
    std::string err;
    bool ok = util::extractArchive(archive.string(), dest.string(), nullptr, cancel, err,
        [&](const std::string& entry, bool) -> std::string {
            if (lower(entry).rfind(lp, 0) != 0) return "";
            return entry.substr(app.prefix.size());
        });
    if (!ok) {
        util::logLine("retro: Vita3K install of " + archive.string() + " failed: " + err);
        fs::remove_all(dest, ec);
        return false;
    }
    fs::remove(archive, ec);
    util::logLine("retro: installed " + info.title + " (" + info.title_id + ") into Vita3K");
    sum.installed++;
    sum.installed_dirs.push_back(dest.string());
    return true;
}

// A zip of .vpk files (common for releases): unpack the .vpk files next to it.
std::vector<fs::path> unpackNestedVpks(const fs::path& archive, const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::vector<fs::path> out;
    std::string err;
    fs::path dir = archive.parent_path();
    util::extractArchive(archive.string(), dir.string(), nullptr, cancel, err,
        [&](const std::string& entry, bool isDir) -> std::string {
            if (isDir || lower(fs::path(entry).extension().string()) != ".vpk") return "";
            std::string name = fs::path(entry).filename().string();
            out.push_back(dir / name);
            return name;
        });
    return out;
}

// --- folders (a dump already unpacked: <TITLE_ID>/sce_sys/param.sfo) ---

int installFolder(const fs::path& dir, VitaInstallSummary& sum) {
    std::vector<fs::path> roots;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code e2;
        if (it->is_regular_file(e2) && lower(it->path().filename().string()) == "param.sfo" &&
            lower(it->path().parent_path().filename().string()) == "sce_sys") {
            roots.push_back(it->path().parent_path().parent_path());
        }
    }
    int done = 0;
    for (const auto& root : roots) {
        if (fs::exists(root / "sce_pfs", ec) || fs::exists(root / "sce_sys" / "package" / "work.bin", ec)) continue;
        std::string data;
        SfoInfo info;
        if (!readFile(root / "sce_sys" / "param.sfo", data) || !parseParamSfo(data, info)) continue;
        if (!isPlainApp(info) || !safeTitleId(info.title_id)) continue;
        fs::path dest = fs::path(ux0AppDir()) / info.title_id;
        fs::remove_all(dest, ec);
        fs::create_directories(dest.parent_path(), ec);
        ec.clear();
        fs::rename(root, dest, ec);
        if (ec) {
            ec.clear();
            fs::copy(root, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                util::logLine("retro: could not copy " + root.string() + " into Vita3K: " + ec.message());
                continue;
            }
            fs::remove_all(root, ec);
        }
        util::logLine("retro: installed " + info.title + " (" + info.title_id + ") into Vita3K");
        sum.installed++;
        sum.installed_dirs.push_back(dest.string());
        ++done;
    }
    return done;
}

} // namespace

bool parseParamSfo(const std::string& d, SfoInfo& out) {
    if (d.size() < 20 || d.compare(0, 4, std::string("\0PSF", 4)) != 0) return false;
    uint32_t keyTable = le32(d, 8);
    uint32_t dataTable = le32(d, 12);
    uint32_t count = le32(d, 16);
    if (count > 256) return false;
    for (uint32_t i = 0; i < count; ++i) {
        size_t e = 20 + static_cast<size_t>(i) * 16;
        if (e + 16 > d.size()) return false;
        uint16_t keyOff = le16(d, e);
        uint16_t fmt = le16(d, e + 2);
        uint32_t len = le32(d, e + 4);
        uint32_t off = le32(d, e + 12);
        size_t kp = static_cast<size_t>(keyTable) + keyOff;
        if (kp >= d.size()) continue;
        std::string key(d.c_str() + kp);
        if (fmt != 0x0204 && fmt != 0x0004) continue; // text values only
        size_t vp = static_cast<size_t>(dataTable) + off;
        if (vp + len > d.size()) continue;
        std::string value = d.substr(vp, len);
        size_t nul = value.find('\0');
        if (nul != std::string::npos) value.resize(nul);
        if (key == "TITLE_ID") out.title_id = value;
        else if (key == "TITLE") out.title = value;
        else if (key == "CATEGORY") out.category = value;
        else if (key == "CONTENT_ID") out.content_id = value;
    }
    return !out.title_id.empty();
}

VitaInstallSummary installIntoVita3K(const std::vector<fs::path>& candidates,
                                     const std::shared_ptr<std::atomic<bool>>& cancel) {
    VitaInstallSummary sum;
    std::vector<fs::path> queue = candidates;
    std::error_code ec;
    for (size_t i = 0; i < queue.size(); ++i) {
        if (cancel && cancel->load()) break;
        const fs::path item = queue[i];
        if (fs::is_directory(item, ec)) {
            installFolder(item, sum);
            // Archives saved inside the release folder.
            std::vector<fs::path> inner;
            for (auto it = fs::recursive_directory_iterator(item, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::error_code e2;
                std::string ext = lower(it->path().extension().string());
                if (it->is_regular_file(e2) && (ext == ".vpk" || ext == ".zip")) inner.push_back(it->path());
            }
            queue.insert(queue.end(), inner.begin(), inner.end());
            continue;
        }
        std::string ext = lower(item.extension().string());
        if (ext != ".vpk" && ext != ".zip") continue;
        if (installArchive(item, cancel, sum)) continue;
        if (ext == ".zip") {
            ArchiveApp app = findAppInArchive(item);
            if (!app.found) {
                std::vector<fs::path> vpks = unpackNestedVpks(item, cancel);
                if (!vpks.empty()) {
                    fs::remove(item, ec);
                    queue.insert(queue.end(), vpks.begin(), vpks.end());
                }
            }
        }
    }

    // Whatever is left goes to Vita3K's own installer (pkg, encrypted dumps, updates, DLC).
    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec)) {
            if (hasAnyFile(c)) sum.left_paths.push_back(c);
            else fs::remove_all(c, ec);
        } else if (fs::exists(c, ec)) {
            sum.left_paths.push_back(c);
        }
    }
    for (size_t i = candidates.size(); i < queue.size(); ++i) {
        if (fs::exists(queue[i], ec) && std::find(sum.left_paths.begin(), sum.left_paths.end(), queue[i]) == sum.left_paths.end()) {
            bool insideLeftDir = std::any_of(sum.left_paths.begin(), sum.left_paths.end(), [&](const fs::path& l) {
                return queue[i].string().rfind(l.string(), 0) == 0;
            });
            if (!insideLeftDir) sum.left_paths.push_back(queue[i]);
        }
    }
    sum.staged = static_cast<int>(sum.left_paths.size());
    return sum;
}

} // namespace retro
} // namespace catalog
