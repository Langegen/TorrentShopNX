#include "PortInstallPlanner.hpp"
#include "utils/log.h"
#include "utils/app_paths.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <borealis/extern/nlohmann/json.hpp>

namespace installer {

namespace {

// Forwarders are tiny (300 KB - 5 MB). A bigger NSP is a game, not a shortcut.
constexpr int64_t kMaxForwarderSize = 64LL * 1024 * 1024;

// ── Strings ─────────────────────────────────────────────────────────────────

// Lowercase ASCII and the basic Cyrillic block (А-Я, Ё) of a UTF-8 string.
std::string lowerName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>(std::tolower(c));
            continue;
        }
        if (c == 0xD0 && i + 1 < s.size()) {
            unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0x90 && d <= 0x9F) {        // А-П -> а-п
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d + 0x20);
                ++i;
                continue;
            }
            if (d >= 0xA0 && d <= 0xAF) {        // Р-Я -> р-я
                out += static_cast<char>(0xD1);
                out += static_cast<char>(d - 0x20);
                ++i;
                continue;
            }
            if (d == 0x81) {                     // Ё -> ё
                out += static_cast<char>(0xD1);
                out += static_cast<char>(0x91);
                ++i;
                continue;
            }
        }
        out += static_cast<char>(c);
    }
    return out;
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Words of a name: runs of ASCII letters/digits and non-ASCII bytes.
std::vector<std::string> tokens(const std::string& name) {
    std::string l = lowerName(name);
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : l) {
        if (c >= 0x80 || std::isalnum(c)) {
            cur += static_cast<char>(c);
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& segs, size_t from, size_t to) {
    std::string r;
    for (size_t i = from; i < to && i < segs.size(); ++i) {
        if (!r.empty()) r += '/';
        r += segs[i];
    }
    return r;
}

std::string extOf(const std::string& lowerFile) {
    size_t dot = lowerFile.rfind('.');
    return dot == std::string::npos ? std::string() : lowerFile.substr(dot + 1);
}

std::string fileNameOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ── Path parts ──────────────────────────────────────────────────────────────

struct SplitPath {
    std::vector<std::string> segs;
    bool unsafe = false;   // had ".." or a drive/device prefix
};

SplitPath splitPath(const std::string& raw) {
    SplitPath r;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return;
        }
        if (cur == ".." || cur.find(':') != std::string::npos) {
            r.unsafe = true;
        } else {
            r.segs.push_back(cur);
        }
        cur.clear();
    };
    for (char c : raw) {
        if (c == '/' || c == '\\') flush();
        else cur += c;
    }
    flush();
    return r;
}

bool isSwitchSeg(const std::string& seg) {
    return lowerName(seg) == "switch";
}

// A folder whose contents mirror the SD card root: "Homebrew", "Homebrew (12.10.2026)",
// "sdcard", "SD", "microSD", "SD card".
bool isSdRootMarker(const std::string& seg) {
    std::string l = lowerName(seg);
    if (l == "sd card" || l == "sd-card" || l == "sd_card" || l == "micro sd" ||
        l == "корень sd" || l == "корень карты") {
        return true;
    }
    auto t = tokens(seg);
    if (t.empty()) return false;
    return t[0] == "homebrew" || t[0] == "sdcard" || t[0] == "sd" || t[0] == "microsd";
}

// Folders that ports keep at the SD root.
bool isKnownRootDir(const std::string& seg) {
    std::string l = lowerName(seg);
    return l == "openmw" || l == "retroarch" || l == "atmosphere";
}

bool isJunkFile(const std::string& lowerFile) {
    static const std::set<std::string> kJunkExt = {
        "txt", "nfo", "url", "md", "jpg", "jpeg", "png", "gif", "bmp", "webp",
        "html", "htm", "pdf", "torrent", "lnk"
    };
    if (lowerFile == "thumbs.db" || lowerFile == ".ds_store" || lowerFile == "desktop.ini") return true;
    return kJunkExt.count(extOf(lowerFile)) > 0;
}

bool isNroFile(const std::string& lowerFile) { return endsWith(lowerFile, ".nro"); }
bool isNspFile(const std::string& lowerFile) { return endsWith(lowerFile, ".nsp"); }

// ── Archive names and volumes ───────────────────────────────────────────────

enum class VolKind { None, Plain, Split, RarParts, RarOld, ZipSpan };

struct ArchiveName {
    VolKind kind = VolKind::None;
    std::string base;   // lowercase name of the whole set, e.g. "data.7z"
    int volume = 0;
};

ArchiveName parseArchiveName(const std::string& fileName) {
    ArchiveName a;
    std::string lf = lowerName(fileName);
    const size_t n = lf.size();

    // data.7z.001 / data.zip.001
    if (n > 4 && lf[n - 4] == '.' && allDigits(lf.substr(n - 3))) {
        std::string pre = lf.substr(0, n - 4);
        if (endsWith(pre, ".7z") || endsWith(pre, ".zip")) {
            a.kind = VolKind::Split;
            a.base = pre;
            a.volume = std::stoi(lf.substr(n - 3));
            return a;
        }
    }
    // data.r00 (old RAR volumes) / data.z01 (spanned ZIP)
    if (n > 4 && lf[n - 4] == '.' && allDigits(lf.substr(n - 2))) {
        char k = lf[n - 3];
        std::string pre = lf.substr(0, n - 4);
        if (k == 'r') {
            a.kind = VolKind::RarOld;
            a.base = pre + ".rar";
            a.volume = std::stoi(lf.substr(n - 2)) + 1;
            return a;
        }
        if (k == 'z') {
            a.kind = VolKind::ZipSpan;
            a.base = pre + ".zip";
            a.volume = std::stoi(lf.substr(n - 2));
            return a;
        }
    }
    if (endsWith(lf, ".rar")) {
        std::string pre = lf.substr(0, n - 4);
        size_t p = pre.rfind(".part");
        if (p != std::string::npos && allDigits(pre.substr(p + 5))) {
            a.kind = VolKind::RarParts;
            a.base = pre.substr(0, p) + ".rar";
            a.volume = std::stoi(pre.substr(p + 5));
            return a;
        }
        a.kind = VolKind::Plain;
        a.base = lf;
        return a;
    }
    if (endsWith(lf, ".7z") || endsWith(lf, ".zip")) {
        a.kind = VolKind::Plain;
        a.base = lf;
        // A spanned zip ends with data.zip after data.z01..z99.
        if (endsWith(lf, ".zip")) a.volume = 1000;
        return a;
    }
    return a;
}

std::string stripArchiveExt(const std::string& lowerBase) {
    for (const char* ext : {".7z", ".zip", ".rar"}) {
        if (endsWith(lowerBase, ext)) return lowerBase.substr(0, lowerBase.size() - std::string(ext).size());
    }
    return lowerBase;
}

// ── Variant tags ────────────────────────────────────────────────────────────

enum class Tag { None, Ru, En, Mod, Vanilla };

bool isRuToken(const std::string& t) {
    return t == "ru" || t == "rus" || startsWith(t, "russ") || startsWith(t, "rusvo") ||
           startsWith(t, "rusmod") || startsWith(t, "русск") || startsWith(t, "русиф") || t == "рус";
}
bool isEnToken(const std::string& t) {
    return t == "en" || t == "eng" || startsWith(t, "english") || startsWith(t, "engvo") ||
           startsWith(t, "англ");
}
bool isModToken(const std::string& t) {
    return t == "mod" || t == "mods" || t == "modded" || t == "modpack" || t == "betterpack" ||
           t == "мод" || t == "моды" || t == "модами" || startsWith(t, "модиф") || t == "сборка";
}
bool isVanillaToken(const std::string& t) {
    return t == "vanilla" || t == "original" || t == "orig" || startsWith(t, "оригинал") || t == "ванилла";
}
bool isTagToken(const std::string& t) {
    return isRuToken(t) || isEnToken(t) || isModToken(t) || isVanillaToken(t) ||
           t == "voice" || t == "version" || t == "ver" || t == "озвучка" || t == "версия";
}

Tag classifyName(const std::string& name) {
    auto t = tokens(name);
    bool ru = false, en = false, mod = false, van = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (isRuToken(t[i])) ru = true;
        if (isEnToken(t[i])) en = true;
        if (isModToken(t[i])) mod = true;
        if (isVanillaToken(t[i])) van = true;
        if (t[i] == "better" && i + 1 < t.size() && t[i + 1] == "pack") mod = true;
    }
    if (ru && en) return Tag::None;        // "RUS-ENG": both languages inside
    if (ru) return Tag::Ru;
    if (en) return Tag::En;
    if (mod && van) return Tag::None;
    if (mod) return Tag::Mod;
    if (van) return Tag::Vanilla;
    return Tag::None;
}

// "switch_RUSSVOICE.7z" -> "switch"; used to pair a tagged archive with its untagged twin.
std::string untaggedStem(const std::string& lowerBase) {
    std::string r;
    for (const auto& t : tokens(stripArchiveExt(lowerBase))) {
        if (isTagToken(t)) continue;
        if (!r.empty()) r += ' ';
        r += t;
    }
    return r;
}

bool hasAnyToken(const std::string& name, std::initializer_list<const char*> prefixes) {
    for (const auto& t : tokens(name)) {
        for (const char* p : prefixes) {
            if (startsWith(t, p)) return true;
        }
    }
    return false;
}

// Folders with fixes on top of the main files, never a separate version.
bool isUpdateName(const std::string& name) {
    return hasAnyToken(name, {"update", "patch", "fix", "hotfix", "обнов", "патч", "фикс"});
}

bool isForwarderFolderName(const std::string& name) {
    return hasAnyToken(name, {"forwarder", "nsp", "ярлык", "форвардер"});
}

// ── Planner internals ───────────────────────────────────────────────────────

struct Entry {
    int idx = -1;
    int64_t size = 0;
    std::string raw;
    std::vector<std::string> segs;   // after the common root is removed
    bool unsafe = false;
    std::string unit;                // top-level unit key
    std::vector<std::string> rel;    // path used for mapping (variant folder removed)
    std::string lowerFile;
};

struct Unit {
    std::string key;
    std::string name;
    bool folder = false;
    bool marker = false;
    bool switchDir = false;
    bool knownRoot = false;
    Tag tag = Tag::None;
    bool hasForwarder = false;
    bool hasPayload = false;   // archive, .nro or other non-junk non-NSP file
    bool version = false;
    std::vector<size_t> entries;
};

enum class MapKind { Copy, Extract, Skip };

struct Mapping {
    MapKind kind = MapKind::Skip;
    std::string dest;
    std::string reason;
};

std::string sdRel(const std::string& sdmcPath) {
    return startsWith(sdmcPath, "sdmc:/") ? sdmcPath.substr(6) : sdmcPath;
}

Mapping mapRelPath(const std::vector<std::string>& rel, const std::string& game) {
    Mapping m;
    if (rel.empty()) {
        m.reason = "empty path";
        return m;
    }
    const std::string lowerFile = lowerName(rel.back());
    const bool archive = parseArchiveName(rel.back()).kind != VolKind::None;

    size_t start = 0;
    bool atSdRoot = false;
    if (rel.size() > 1 && isSdRootMarker(rel[0])) {
        start = 1;
        atSdRoot = true;
    }
    std::vector<std::string> rest(rel.begin() + static_cast<std::ptrdiff_t>(start), rel.end());

    // .../switch/<...> -> sdmc:/switch/<...>
    for (size_t i = 0; i + 1 < rest.size(); ++i) {
        if (isSwitchSeg(rest[i])) {
            m.kind = MapKind::Copy;
            m.dest = "sdmc:/switch/" + join(rest, i + 1, rest.size());
            return m;
        }
    }

    if (rest.size() == 1) {
        // A file at the release level (or at the SD root of a marker folder)
        if (archive) {
            m.kind = MapKind::Extract;
            m.dest = "sdmc:/";
            return m;
        }
        if (isJunkFile(lowerFile)) {
            m.reason = "readme/screenshot";
            return m;
        }
        m.kind = MapKind::Copy;
        m.dest = "sdmc:/switch/" + game + "/" + rest[0];
        return m;
    }

    if (atSdRoot || isKnownRootDir(rest[0])) {
        m.kind = MapKind::Copy;
        m.dest = "sdmc:/" + join(rest, 0, rest.size());
        return m;
    }

    // Unknown folder: keep its structure inside the game folder.
    m.kind = MapKind::Copy;
    if (lowerName(rest[0]) == lowerName(game)) {
        m.dest = "sdmc:/switch/" + join(rest, 0, rest.size());
    } else {
        m.dest = "sdmc:/switch/" + game + "/" + join(rest, 0, rest.size());
    }
    return m;
}

std::string sanitizeTitle(const std::string& release_title) {
    std::string clean = release_title;
    size_t br = clean.find('[');
    if (br != std::string::npos) clean = clean.substr(0, br);
    size_t paren = clean.find('(');
    if (paren != std::string::npos) clean = clean.substr(0, paren);
    std::string safe;
    for (char c : clean) {
        if (std::isalnum(static_cast<unsigned char>(c))) safe += c;
    }
    return safe.empty() ? "game" : lowerName(safe);
}

std::vector<Entry> prepareEntries(const std::vector<torrent::TorrentFileInfo>& files) {
    std::vector<Entry> entries;
    entries.reserve(files.size());
    for (const auto& f : files) {
        Entry e;
        e.idx = f.index;
        e.size = static_cast<int64_t>(f.size);
        e.raw = f.name;
        SplitPath sp = splitPath(f.name);
        e.segs = sp.segs;
        e.unsafe = sp.unsafe || sp.segs.empty();
        e.lowerFile = sp.segs.empty() ? std::string() : lowerName(sp.segs.back());
        entries.push_back(std::move(e));
    }
    // Drop the torrent's own root folder ("Max Payne [NRO]/...").
    bool common = true;
    std::string root;
    for (const auto& e : entries) {
        if (e.segs.size() < 2) { common = false; break; }
        if (root.empty()) root = e.segs[0];
        else if (e.segs[0] != root) { common = false; break; }
    }
    if (common && !root.empty() && !isSwitchSeg(root) && !isSdRootMarker(root) && !isKnownRootDir(root)) {
        for (auto& e : entries) e.segs.erase(e.segs.begin());
    }
    return entries;
}

int forwarderScore(const Entry& e) {
    int score = 0;
    if (hasAnyToken(fileNameOf(e.raw), {"port", "forwarder", "homebrew", "порт"})) score += 4;
    if (e.raw.find('[') != std::string::npos && e.raw.find(']') != std::string::npos) score += 2;
    if (e.size > 0 && e.size <= 16LL * 1024 * 1024) score += 1;
    return score;
}

} // namespace

// ── Protected SD paths ──────────────────────────────────────────────────────

bool isProtectedSdPath(const std::string& relative_to_sd_root) {
    SplitPath sp = splitPath(relative_to_sd_root);
    if (sp.unsafe) return true;
    if (sp.segs.empty()) return false;
    const std::string first = lowerName(sp.segs[0]);
    if (sp.segs.size() == 1) {
        return first == "hbmenu.nro" || first == "boot.dat" || first == "boot.ini" ||
               first == "payload.bin" || first == "atmosphere" || first == "bootloader" ||
               first == "nintendo" || first == "emummc";
    }
    if (first == "bootloader" || first == "nintendo" || first == "emummc" ||
        first == "emutendo" || first == "sept") {
        return true;
    }
    if (first == "atmosphere") {
        return lowerName(sp.segs[1]) != "contents";
    }
    return false;
}

// ── Archive contents ────────────────────────────────────────────────────────

void refineArchiveDestination(const PortInstallPlan& plan,
                              PortArchiveTask& task,
                              const std::vector<util::ArchiveEntryInfo>& entries) {
    uint64_t total = 0;
    for (const auto& e : entries) total += e.size;
    task.uncompressed_size = total;
    if (task.from_recipe) {
        task.dest_reason = "recipe";
        return;
    }

    // Folders the release already fills with loose files.
    std::set<std::string> rootDirs, switchDirs;
    for (const auto& kv : plan.file_destinations) {
        if (!startsWith(kv.second, "sdmc:/") || endsWith(lowerName(kv.second), ".nsp")) continue;
        SplitPath sp = splitPath(kv.second.substr(6));
        if (sp.segs.size() < 2) continue;
        if (isSwitchSeg(sp.segs[0])) {
            if (sp.segs.size() >= 3) switchDirs.insert(lowerName(sp.segs[1]));
        } else {
            rootDirs.insert(lowerName(sp.segs[0]));
        }
    }
    // ...and folders other archives of the release were already placed with.
    for (const auto& other : plan.archives_to_extract) {
        if (other.file_index == task.file_index || other.top_folder.empty()) continue;
        if (other.extract_dest_dir == "sdmc:/") rootDirs.insert(lowerName(other.top_folder));
        if (other.extract_dest_dir == "sdmc:/switch/") switchDirs.insert(lowerName(other.top_folder));
    }
    const std::string game = lowerName(plan.port_game_name);
    const std::string archiveStem = stripArchiveExt(parseArchiveName(fileNameOf(task.archive_torrent_path)).base);
    task.top_folder.clear();

    std::vector<std::vector<std::string>> paths;
    std::vector<bool> isDir;
    for (const auto& e : entries) {
        SplitPath sp = splitPath(e.path);
        if (sp.unsafe || sp.segs.empty()) continue;
        paths.push_back(sp.segs);
        isDir.push_back(e.is_dir);
    }

    auto decide = [&](const std::string& dest, const std::string& strip, const std::string& why) {
        task.extract_dest_dir = dest;
        task.strip_prefix = strip;
        task.dest_reason = why;
    };

    std::vector<std::string> prefix;   // folders unwrapped so far
    for (int depth = 0; depth < 4; ++depth) {
        // Top level under the prefix, ignoring readme/screenshots.
        std::set<std::string> topFolders;
        std::string topFolderName;
        bool nroAtTop = false, filesAtTop = false;
        bool rootMarker = false;
        for (size_t i = 0; i < paths.size(); ++i) {
            const auto& p = paths[i];
            if (p.size() <= prefix.size()) continue;
            bool under = true;
            for (size_t k = 0; k < prefix.size(); ++k) {
                if (p[k] != prefix[k]) { under = false; break; }
            }
            if (!under) continue;
            const std::string& seg = p[prefix.size()];
            const bool leaf = p.size() == prefix.size() + 1 && !isDir[i];
            if (leaf) {
                std::string lf = lowerName(seg);
                if (isJunkFile(lf)) continue;
                filesAtTop = true;
                if (isNroFile(lf)) nroAtTop = true;
                continue;
            }
            topFolders.insert(seg);
            topFolderName = seg;
            if (isSwitchSeg(seg) || isKnownRootDir(seg)) rootMarker = true;
        }
        const std::string strip = join(prefix, 0, prefix.size()) + (prefix.empty() ? "" : "/");

        if (rootMarker) {
            return decide("sdmc:/", strip, "switch/ or a root folder inside");
        }
        if (nroAtTop) {
            return decide("sdmc:/switch/" + plan.port_game_name + "/", strip, ".nro at the top");
        }
        if (topFolders.size() == 1 && !filesAtTop) {
            const std::string f = lowerName(topFolderName);
            task.top_folder = topFolderName;
            if (isSdRootMarker(topFolderName)) {
                task.top_folder.clear();
                return decide("sdmc:/", strip + topFolderName + "/", "SD root folder '" + topFolderName + "'");
            }
            if (rootDirs.count(f)) {
                return decide("sdmc:/", strip, "release puts '" + topFolderName + "' at the SD root");
            }
            if (switchDirs.count(f)) {
                return decide("sdmc:/switch/", strip, "release puts '" + topFolderName + "' in switch/");
            }
            // An .nro right inside the only folder -> it is the homebrew folder.
            bool nroInside = false, markerInside = false, filesInside = false;
            for (size_t i = 0; i < paths.size(); ++i) {
                const auto& p = paths[i];
                if (p.size() == prefix.size() + 2 && p[prefix.size()] == topFolderName) {
                    if (isNroFile(lowerName(p.back()))) nroInside = true;
                    if (!isDir[i] && !isJunkFile(lowerName(p.back()))) filesInside = true;
                }
                if (p.size() > prefix.size() + 2 && p[prefix.size()] == topFolderName) {
                    const std::string& c = p[prefix.size() + 1];
                    if (isSwitchSeg(c) || isKnownRootDir(c) || isSdRootMarker(c)) markerInside = true;
                }
            }
            if (nroInside) {
                return decide("sdmc:/switch/", strip, "'" + topFolderName + "' holds the .nro");
            }
            if (!game.empty() && f == game) {
                return decide("sdmc:/switch/", strip, "folder named like the game");
            }
            // Wrapper folder ("PCAE Switch.zip" -> "PCAE Switch/..."): look inside.
            if (markerInside || (depth == 0 && !filesInside && !archiveStem.empty() && f == archiveStem)) {
                prefix.push_back(topFolderName);
                task.top_folder.clear();
                continue;
            }
            return decide("sdmc:/", strip, "single folder, kept at the SD root");
        }
        if (topFolders.empty() && !filesAtTop) {
            return decide(task.extract_dest_dir.empty() ? "sdmc:/" : task.extract_dest_dir, strip, "empty listing");
        }
        // Several folders/files and nothing that belongs to the SD root: game data.
        return decide("sdmc:/switch/" + plan.port_game_name + "/", strip, "loose game files");
    }
    decide("sdmc:/", join(prefix, 0, prefix.size()) + "/", "nested wrappers");
}

std::string archiveEntryTarget(const PortArchiveTask& task, const std::string& entry_path) {
    std::string p = entry_path;
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    auto stripIfPrefix = [](std::string& s, const std::string& pre) -> bool {
        if (pre.empty()) return true;
        std::string pfx = pre;
        if (pfx.back() != '/') pfx += '/';
        if (lowerName(s.substr(0, pfx.size())) != lowerName(pfx)) {
            // the folder entry itself ("Wrapper") is skipped too
            return false;
        }
        s = s.substr(pfx.size());
        return true;
    };
    if (!task.only_subpath.empty()) {
        // Recipe: keep the folder itself ("maxpayne/..." stays "maxpayne/...").
        std::string sub = task.only_subpath;
        if (sub.back() != '/') sub += '/';
        if (lowerName(p.substr(0, sub.size())) != lowerName(sub)) return "";
        // only_subpath "Archive Root/maxpayne/" keeps "maxpayne/..." (the last folder)
        SplitPath sp = splitPath(sub);
        if (sp.segs.size() > 1) {
            p = p.substr(sub.size() - sp.segs.back().size() - 1);
        }
    }
    if (!stripIfPrefix(p, task.strip_prefix)) return "";
    if (p.empty()) return "";
    std::string root = startsWith(task.extract_dest_dir, "sdmc:/") ? task.extract_dest_dir.substr(6) : "";
    if (!root.empty() && root.back() != '/') root += '/';
    // "Changelog.txt" / readme / screenshots at the top of an archive must not
    // land in the SD root or in sdmc:/switch/ itself.
    if (p.find('/') == std::string::npos && isJunkFile(lowerName(p)) &&
        (root.empty() || lowerName(root) == "switch/")) {
        return "";
    }
    if (isProtectedSdPath(root + p)) return "";
    return p;
}

// ── Plan getters ────────────────────────────────────────────────────────────

PortInstallPlanner& PortInstallPlanner::instance() {
    static PortInstallPlanner inst;
    return inst;
}

std::vector<int> PortInstallPlan::forwardersForVariant(int variant_idx) const {
    if (variant_idx >= 0 && variant_idx < static_cast<int>(variants.size()) &&
        !variants[variant_idx].forwarder_nsp_indices.empty()) {
        return variants[variant_idx].forwarder_nsp_indices;
    }
    if (forwarder_nsp_index >= 0) return {forwarder_nsp_index};
    return {};
}

std::vector<int> PortInstallPlan::getFilesForVariant(int variant_idx) const {
    std::vector<int> res = common_file_indices;
    if (variant_idx >= 0 && variant_idx < static_cast<int>(variants.size())) {
        const auto& v = variants[variant_idx];
        res.insert(res.end(), v.file_indices.begin(), v.file_indices.end());
    } else if (variants.empty()) {
        for (const auto& pair : file_destinations) res.push_back(pair.first);
    }
    for (int f : forwardersForVariant(variant_idx)) res.push_back(f);
    std::sort(res.begin(), res.end());
    res.erase(std::unique(res.begin(), res.end()), res.end());
    return res;
}

std::vector<PortArchiveTask> PortInstallPlan::getArchivesForVariant(int variant_idx) const {
    std::unordered_set<int> common(common_file_indices.begin(), common_file_indices.end());
    std::unordered_set<int> own;
    if (variant_idx >= 0 && variant_idx < static_cast<int>(variants.size())) {
        own.insert(variants[variant_idx].file_indices.begin(), variants[variant_idx].file_indices.end());
    } else if (variants.empty()) {
        return archives_to_extract;
    }
    std::vector<PortArchiveTask> result;
    for (const auto& a : archives_to_extract) {
        if (common.count(a.file_index)) result.push_back(a);
    }
    for (const auto& a : archives_to_extract) {
        if (!common.count(a.file_index) && own.count(a.file_index)) result.push_back(a);
    }
    return result;
}

std::vector<int> PortInstallPlan::getLooseFilesForVariant(int variant_idx) const {
    std::unordered_set<int> excluded;
    for (const auto& a : archives_to_extract) {
        excluded.insert(a.volume_indices.begin(), a.volume_indices.end());
        excluded.insert(a.file_index);
    }
    if (forwarder_nsp_index >= 0) excluded.insert(forwarder_nsp_index);
    for (const auto& v : variants) {
        excluded.insert(v.forwarder_nsp_indices.begin(), v.forwarder_nsp_indices.end());
    }

    auto take = [&](std::vector<int> list, std::vector<int>& out) {
        std::sort(list.begin(), list.end());
        for (int idx : list) {
            if (excluded.count(idx) || skipped_files.count(idx)) continue;
            if (!file_destinations.count(idx)) continue;
            if (std::find(out.begin(), out.end(), idx) != out.end()) continue;
            out.push_back(idx);
        }
    };
    std::vector<int> out;
    take(common_file_indices, out);
    if (variant_idx >= 0 && variant_idx < static_cast<int>(variants.size())) {
        take(variants[variant_idx].file_indices, out);
    }
    return out;
}

int64_t PortInstallPlan::getDownloadSizeForVariant(int variant_idx,
                                                   const std::vector<torrent::TorrentFileInfo>& all_files) const {
    auto needed = getFilesForVariant(variant_idx);
    std::unordered_map<int, int64_t> sizeMap;
    for (const auto& f : all_files) {
        sizeMap[f.index] = static_cast<int64_t>(f.size);
    }
    int64_t total = 0;
    for (int idx : needed) {
        auto it = sizeMap.find(idx);
        if (it != sizeMap.end()) total += it->second;
    }
    return total;
}

std::string PortInstallPlan::describe(const std::vector<torrent::TorrentFileInfo>& all_files,
                                      size_t max_files) const {
    std::string out = "port plan: game='" + port_game_name + "' variants=" +
                      std::to_string(variants.size()) + " default=" + std::to_string(default_variant_index) +
                      " archives=" + std::to_string(archives_to_extract.size()) +
                      " skipped=" + std::to_string(skipped_files.size()) + "\n";
    for (size_t i = 0; i < variants.size(); ++i) {
        const auto& v = variants[i];
        out += "  variant[" + std::to_string(i) + "] id=" + v.id + " title='" + v.title +
               "' own_files=" + std::to_string(v.file_indices.size()) +
               " size=" + std::to_string(v.total_size) + " forwarders=";
        auto fw = forwardersForVariant(static_cast<int>(i));
        for (size_t k = 0; k < fw.size(); ++k) out += (k ? "," : "") + std::to_string(fw[k]);
        out += "\n";
    }
    for (const auto& a : archives_to_extract) {
        out += "  archive #" + std::to_string(a.file_index) + " volumes=" + std::to_string(a.volume_indices.size()) +
               " '" + a.archive_torrent_path + "' -> " + a.extract_dest_dir;
        if (!a.unsupported_reason.empty()) out += " UNSUPPORTED: " + a.unsupported_reason;
        out += "\n";
    }

    std::unordered_map<int, std::string> owner;
    for (int idx : common_file_indices) owner[idx] = "common";
    for (const auto& v : variants) {
        for (int idx : v.file_indices) {
            std::string& o = owner[idx];
            o += (o.empty() ? "" : ",") + v.id;
        }
    }
    if (forwarder_nsp_index >= 0) owner[forwarder_nsp_index] = "common";

    size_t shown = 0;
    for (const auto& f : all_files) {
        if (shown >= max_files) {
            out += "  ... " + std::to_string(all_files.size() - shown) + " more files\n";
            break;
        }
        ++shown;
        out += "  #" + std::to_string(f.index) + " " + f.name + " => ";
        auto sk = skipped_files.find(f.index);
        if (sk != skipped_files.end()) {
            out += "SKIP (" + sk->second + ")\n";
            continue;
        }
        auto d = file_destinations.find(f.index);
        out += (d != file_destinations.end() ? d->second : std::string("?"));
        auto o = owner.find(f.index);
        out += " [" + (o != owner.end() ? o->second : std::string("all")) + "]\n";
    }
    return out;
}

// ── Analysis ────────────────────────────────────────────────────────────────

PortInstallPlan PortInstallPlanner::analyze(const std::string& release_title,
                                           const std::vector<torrent::TorrentFileInfo>& files,
                                           const std::string& preferred_lang) {
    PortInstallPlan plan;
    if (files.empty()) return plan;

    // 1-2. Split paths, drop the torrent's own root folder.
    std::vector<Entry> entries = prepareEntries(files);

    // 3. Port detection.
    bool has_nro = false, has_switch = false, has_nsp = false, has_archive = false, has_marker = false;
    for (const auto& e : entries) {
        if (e.segs.empty()) continue;
        if (isNroFile(e.lowerFile)) has_nro = true;
        if (isNspFile(e.lowerFile)) has_nsp = true;
        if (parseArchiveName(e.segs.back()).kind != VolKind::None) has_archive = true;
        for (size_t i = 0; i + 1 < e.segs.size(); ++i) {
            if (isSwitchSeg(e.segs[i])) has_switch = true;
        }
        if (e.segs.size() > 1 && isSdRootMarker(e.segs[0])) has_marker = true;
    }
    std::string lt = lowerName(release_title);
    bool title_is_port = lt.find("порт") != std::string::npos || lt.find("хоумбрю") != std::string::npos ||
                         lt.find("homebrew") != std::string::npos || lt.find("recomp") != std::string::npos ||
                         lt.find("openmw") != std::string::npos || lt.find("xash3d") != std::string::npos ||
                         lt.find("port") != std::string::npos;
    if (!has_nro && !has_switch && !has_marker && !(has_nsp && title_is_port) && !has_archive) {
        return plan;
    }
    plan.is_port = true;

    // 4. Top-level units: a folder, an archive set, or a single file.
    std::map<std::string, Unit> units;
    std::vector<std::string> unitOrder;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if (e.segs.empty()) continue;
        std::string key, name;
        bool folder = e.segs.size() >= 2;
        if (folder) {
            name = e.segs[0];
            key = "d:" + name;
        } else {
            name = e.segs[0];
            ArchiveName an = parseArchiveName(name);
            key = an.kind != VolKind::None ? "a:" + an.base : "f:" + name;
        }
        e.unit = key;
        auto it = units.find(key);
        if (it == units.end()) {
            Unit u;
            u.key = key;
            u.name = name;
            u.folder = folder;
            if (folder) {
                u.marker = isSdRootMarker(name);
                u.switchDir = isSwitchSeg(name);
                u.knownRoot = isKnownRootDir(name);
                if (!u.marker && !u.switchDir && !u.knownRoot) u.tag = classifyName(name);
            } else if (parseArchiveName(name).kind != VolKind::None) {
                u.tag = classifyName(parseArchiveName(name).base);
            }
            it = units.emplace(key, u).first;
            unitOrder.push_back(key);
        }
        Unit& u = it->second;
        u.entries.push_back(i);
        if (!e.unsafe) {
            if (isNspFile(e.lowerFile)) {
                if (e.size <= kMaxForwarderSize) u.hasForwarder = true;
            } else if (!isJunkFile(e.lowerFile)) {
                u.hasPayload = true;
            }
        }
    }
    for (auto& kv : units) {
        Unit& u = kv.second;
        u.version = u.folder && !u.marker && !u.switchDir && !u.knownRoot && u.tag == Tag::None &&
                    u.hasForwarder && u.hasPayload && !isUpdateName(u.name) && !isForwarderFolderName(u.name);
    }

    // 5. Variants from units.
    struct Draft {
        std::string id;
        std::string title;
        std::set<std::string> units;
    };
    std::vector<Draft> drafts;
    std::vector<std::string> ruUnits, enUnits, modUnits, vanUnits, versionUnits;
    for (const auto& key : unitOrder) {
        const Unit& u = units[key];
        switch (u.tag) {
            case Tag::Ru: ruUnits.push_back(key); break;
            case Tag::En: enUnits.push_back(key); break;
            case Tag::Mod: modUnits.push_back(key); break;
            case Tag::Vanilla: vanUnits.push_back(key); break;
            default: break;
        }
        if (u.version) versionUnits.push_back(key);
    }
    // An untagged archive next to a Russian one with the same name is the English one
    // (switch.7z + switch_RUSSVOICE.7z).
    if (!ruUnits.empty() && enUnits.empty()) {
        std::set<std::string> ruStems;
        for (const auto& k : ruUnits) {
            if (startsWith(k, "a:")) ruStems.insert(untaggedStem(k.substr(2)));
        }
        for (const auto& key : unitOrder) {
            const Unit& u = units[key];
            if (u.tag == Tag::None && startsWith(key, "a:") && ruStems.count(untaggedStem(key.substr(2)))) {
                enUnits.push_back(key);
            }
        }
    }

    bool ruOverlay = false;
    bool combinable = false;
    if (!ruUnits.empty() && !enUnits.empty()) {
        drafts.push_back({"ru", "Русская версия (RUS)", {ruUnits.begin(), ruUnits.end()}});
        drafts.push_back({"en", "Английская версия (ENG)", {enUnits.begin(), enUnits.end()}});
    } else if (!modUnits.empty() && !vanUnits.empty()) {
        drafts.push_back({"modded", "Сборка с модами", {modUnits.begin(), modUnits.end()}});
        drafts.push_back({"vanilla", "Оригинальная версия (Vanilla)", {vanUnits.begin(), vanUnits.end()}});
    } else if (!ruUnits.empty()) {
        // Base game plus an optional Russian translation / voiceover on top of it.
        ruOverlay = true;
        drafts.push_back({"ru", "Русская версия (с озвучкой / переводом)", {ruUnits.begin(), ruUnits.end()}});
        drafts.push_back({"en", "Оригинальная английская версия (ENG)", {}});
    } else if (!versionUnits.empty()) {
        // Separate builds in their own folders, each with its own shortcut (Max Payne: NX + PCAE).
        bool mainHasPayload = false;
        for (const auto& key : unitOrder) {
            const Unit& u = units[key];
            if (u.version) continue;
            if (u.hasPayload && (u.marker || u.switchDir || u.knownRoot || startsWith(key, "a:") ||
                                 (startsWith(key, "f:") && isNroFile(lowerName(u.name))))) {
                mainHasPayload = true;
            }
        }
        if (mainHasPayload || versionUnits.size() >= 2) {
            std::set<std::string> versionSet(versionUnits.begin(), versionUnits.end());
            if (mainHasPayload) {
                std::set<std::string> mainUnits;
                for (const auto& key : unitOrder) {
                    if (!versionSet.count(key)) mainUnits.insert(key);
                }
                drafts.push_back({"main", "Основная версия", mainUnits});
            }
            int n = 1;
            for (const auto& key : versionUnits) {
                drafts.push_back({"alt" + std::to_string(n++), units[key].name, {key}});
            }
            combinable = true;
        }
    }

    std::map<std::string, int> unitVariant;   // unit -> draft index
    for (size_t d = 0; d < drafts.size(); ++d) {
        for (const auto& key : drafts[d].units) unitVariant[key] = static_cast<int>(d);
    }

    // 6. Mapping paths: variant/version folders are transparent.
    for (auto& e : entries) {
        if (e.segs.empty()) continue;
        const Unit& u = units[e.unit];
        bool strip = u.folder && (u.tag != Tag::None || u.version);
        e.rel = strip ? std::vector<std::string>(e.segs.begin() + 1, e.segs.end()) : e.segs;
    }

    // Game folder: sdmc:/switch/<folder>/, preferring the one with an .nro inside.
    {
        std::string withNro, any;
        for (const auto& e : entries) {
            const auto& r = e.rel;
            size_t start = (r.size() > 1 && isSdRootMarker(r[0])) ? 1 : 0;
            for (size_t i = start; i + 2 < r.size(); ++i) {
                if (!isSwitchSeg(r[i])) continue;
                if (any.empty()) any = r[i + 1];
                if (withNro.empty() && isNroFile(e.lowerFile)) withNro = r[i + 1];
                break;
            }
        }
        plan.port_game_name = !withNro.empty() ? withNro : (!any.empty() ? any : sanitizeTitle(release_title));
    }

    // 7. Forwarders: one per owner (a variant or common).
    std::map<int, std::vector<size_t>> nspByOwner;   // owner draft index, -1 = common
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.segs.empty() || !isNspFile(e.lowerFile)) continue;
        if (e.unsafe) {
            plan.skipped_files[e.idx] = "unsafe path";
            continue;
        }
        if (e.size > kMaxForwarderSize) {
            plan.skipped_files[e.idx] = "NSP too big for a forwarder";
            continue;
        }
        auto uv = unitVariant.find(e.unit);
        nspByOwner[uv != unitVariant.end() ? uv->second : -1].push_back(i);
    }
    std::map<int, int> forwarderOf;   // owner -> file index
    for (auto& kv : nspByOwner) {
        size_t best = kv.second.front();
        for (size_t i : kv.second) {
            if (forwarderScore(entries[i]) > forwarderScore(entries[best])) best = i;
        }
        for (size_t i : kv.second) {
            if (i != best) plan.skipped_files[entries[i].idx] = "extra NSP (one forwarder per version)";
        }
        const Entry& fe = entries[best];
        forwarderOf[kv.first] = fe.idx;
        plan.file_destinations[fe.idx] = std::string(TSNX_CACHE_TMP) + "/" + fileNameOf(fe.raw);
        if (kv.first < 0) {
            plan.forwarder_nsp_index = fe.idx;
            plan.forwarder_nsp_name = fileNameOf(fe.raw);
            plan.forwarder_nsp_size = fe.size;
        }
    }

    // 8. Map every other file.
    struct Placed {
        size_t entry;
        int owner;
        int priority;
    };
    std::map<std::string, Placed> byDest;   // lowercase dest -> winner (copied files only)
    std::map<std::string, std::vector<size_t>> archiveSets;   // dir|base -> entries
    std::vector<std::string> archiveSetOrder;
    std::unordered_map<int, int> ownerOf;  // file index -> owner draft (-1 common)
    std::unordered_set<int> extracted;     // archive volumes (extracted, not copied)

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.segs.empty() || isNspFile(e.lowerFile)) continue;
        if (e.unsafe) {
            plan.skipped_files[e.idx] = "unsafe path";
            continue;
        }
        auto uv = unitVariant.find(e.unit);
        const int owner = uv != unitVariant.end() ? uv->second : -1;
        ownerOf[e.idx] = owner;

        Mapping m = mapRelPath(e.rel, plan.port_game_name);
        if (m.kind == MapKind::Skip) {
            plan.skipped_files[e.idx] = m.reason;
            continue;
        }
        if (m.kind == MapKind::Extract) {
            ArchiveName an = parseArchiveName(e.rel.back());
            std::string setKey = lowerName(join(e.segs, 0, e.segs.size() - 1)) + "|" + an.base;
            if (!archiveSets.count(setKey)) archiveSetOrder.push_back(setKey);
            archiveSets[setKey].push_back(i);
            extracted.insert(e.idx);
            plan.file_destinations[e.idx] = m.dest;
            continue;
        }
        if (isProtectedSdPath(sdRel(m.dest))) {
            plan.skipped_files[e.idx] = "protected system path";
            continue;
        }
        // Two files for one destination: an update/marker folder wins, else the later file.
        const Unit& u = units[e.unit];
        const int priority = isUpdateName(u.name) ? 2 : (u.marker ? 1 : 0);
        const std::string key = std::to_string(owner) + "|" + lowerName(m.dest);
        auto it = byDest.find(key);
        if (it != byDest.end()) {
            const Placed& prev = it->second;
            if (priority >= prev.priority) {
                plan.skipped_files[entries[prev.entry].idx] = "replaced by " + e.raw;
                plan.file_destinations.erase(entries[prev.entry].idx);
                it->second = {i, owner, priority};
            } else {
                plan.skipped_files[e.idx] = "replaced by " + entries[prev.entry].raw;
                continue;
            }
        } else {
            byDest[key] = {i, owner, priority};
        }
        plan.file_destinations[e.idx] = m.dest;
    }

    // 9. Archive sets -> tasks (volumes in order).
    for (const auto& key : archiveSetOrder) {
        auto vols = archiveSets[key];
        bool rarParts = false, rarOld = false, zipSpan = false;
        for (size_t i : vols) {
            VolKind k = parseArchiveName(entries[i].rel.back()).kind;
            if (k == VolKind::RarParts) rarParts = true;
            if (k == VolKind::RarOld) rarOld = true;
            if (k == VolKind::ZipSpan) zipSpan = true;
        }
        std::stable_sort(vols.begin(), vols.end(), [&](size_t a, size_t b) {
            return parseArchiveName(entries[a].rel.back()).volume < parseArchiveName(entries[b].rel.back()).volume;
        });
        PortArchiveTask t;
        t.file_index = entries[vols.front()].idx;
        t.archive_torrent_path = join(entries[vols.front()].segs, 0, entries[vols.front()].segs.size());
        t.extract_dest_dir = "sdmc:/";
        for (size_t i : vols) {
            t.volume_indices.push_back(entries[i].idx);
            t.compressed_size += entries[i].size;
        }
        if (vols.size() > 1 && (rarParts || rarOld)) {
            t.unsupported_reason = "multi-volume RAR cannot be extracted while downloading; use \"Download full\"";
        } else if (vols.size() > 1 && zipSpan) {
            t.unsupported_reason = "spanned ZIP (.z01) cannot be extracted while downloading; use \"Download full\"";
        }
        plan.archives_to_extract.push_back(t);
    }

    // 10. Variant / common membership.
    std::vector<PortVariant> variants(drafts.size());
    for (size_t d = 0; d < drafts.size(); ++d) {
        variants[d].id = drafts[d].id;
        variants[d].title = drafts[d].title;
        auto fw = forwarderOf.find(static_cast<int>(d));
        if (fw != forwarderOf.end()) {
            variants[d].forwarder_nsp_indices.push_back(fw->second);
            variants[d].file_indices.push_back(fw->second);
        }
    }

    std::set<std::string> ruDests;
    if (ruOverlay) {
        for (const auto& e : entries) {
            auto o = ownerOf.find(e.idx);
            if (o == ownerOf.end() || o->second != 0) continue;
            auto d = plan.file_destinations.find(e.idx);
            if (d != plan.file_destinations.end() && !plan.skipped_files.count(e.idx) &&
                !extracted.count(e.idx)) {
                ruDests.insert(lowerName(d->second));
            }
        }
    }

    for (const auto& e : entries) {
        if (plan.skipped_files.count(e.idx) || !plan.file_destinations.count(e.idx)) continue;
        if (isNspFile(e.lowerFile)) continue;   // forwarders handled above
        auto o = ownerOf.find(e.idx);
        int owner = o != ownerOf.end() ? o->second : -1;
        if (owner >= 0) {
            variants[owner].file_indices.push_back(e.idx);
        } else if (ruOverlay && !extracted.count(e.idx) &&
                   ruDests.count(lowerName(plan.file_destinations[e.idx]))) {
            variants[1].file_indices.push_back(e.idx);   // replaced by the Russian files
        } else {
            plan.common_file_indices.push_back(e.idx);
        }
    }

    if (variants.empty()) {
        PortVariant vDef;
        vDef.id = "default";
        vDef.title = "Стандартная установка";
        variants.push_back(vDef);
    } else if (combinable && variants.size() >= 2) {
        // All versions at once, if their copied files do not overlap.
        std::set<std::string> seen;
        bool overlap = false;
        for (const auto& v : variants) {
            for (int idx : v.file_indices) {
                auto d = plan.file_destinations.find(idx);
                if (d == plan.file_destinations.end() || d->second == "sdmc:/") continue;
                if (!seen.insert(lowerName(d->second)).second) overlap = true;
            }
        }
        if (!overlap) {
            PortVariant all;
            all.id = "all";
            all.title = "Все версии";
            for (const auto& v : variants) {
                all.file_indices.insert(all.file_indices.end(), v.file_indices.begin(), v.file_indices.end());
                all.forwarder_nsp_indices.insert(all.forwarder_nsp_indices.end(),
                                                 v.forwarder_nsp_indices.begin(), v.forwarder_nsp_indices.end());
            }
            variants.push_back(all);
        }
    }
    plan.variants = std::move(variants);
    std::sort(plan.common_file_indices.begin(), plan.common_file_indices.end());

    // UI names (translated in the app; `title` stays as the fallback).
    for (auto& v : plan.variants) {
        if (v.id == "ru") v.title_key = ruOverlay ? "app/ports/variant_ru_overlay" : "app/ports/variant_ru";
        else if (v.id == "en") v.title_key = ruOverlay ? "app/ports/variant_en_original" : "app/ports/variant_en";
        else if (v.id == "modded" || v.id == "vanilla" || v.id == "main" || v.id == "all" || v.id == "default")
            v.title_key = "app/ports/variant_" + v.id;
        else if (startsWith(v.id, "alt")) {
            v.title_key = "app/ports/variant_folder";
            v.title_arg = v.title;
        }
    }

    // 11. Default variant: Russian only for a Russian UI.
    plan.default_variant_index = 0;
    const bool ruUi = startsWith(lowerName(preferred_lang), "ru");
    for (size_t i = 0; i < plan.variants.size(); ++i) {
        const std::string& id = plan.variants[i].id;
        if ((ruUi && id == "ru") || (!ruUi && id == "en")) {
            plan.default_variant_index = static_cast<int>(i);
            break;
        }
    }

    for (size_t i = 0; i < plan.variants.size(); ++i) {
        plan.variants[i].total_size = plan.getDownloadSizeForVariant(static_cast<int>(i), files);
    }

    util::logLine("PortInstallPlanner: analyzed '" + release_title + "' -> is_port=" +
                  (plan.is_port ? "true" : "false") + " game='" + plan.port_game_name +
                  "' forwarder=" + (plan.forwarder_nsp_index >= 0 ? plan.forwarder_nsp_name : "none") +
                  " variants=" + std::to_string(plan.variants.size()) +
                  " archives=" + std::to_string(plan.archives_to_extract.size()) +
                  " skipped=" + std::to_string(plan.skipped_files.size()));

    return plan;
}

// ── Recipes ─────────────────────────────────────────────────────────────────

bool parsePortRecipes(const std::string& json_text, std::vector<PortRecipe>& out, std::string& out_error) {
    out.clear();
    nlohmann::json j = nlohmann::json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        out_error = "recipes: invalid JSON";
        return false;
    }
    if (!j.contains("recipes") || !j["recipes"].is_array()) {
        out_error = "recipes: no \"recipes\" array";
        return false;
    }
    auto str = [](const nlohmann::json& o, const char* key) -> std::string {
        auto it = o.find(key);
        return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    for (const auto& rj : j["recipes"]) {
        if (!rj.is_object()) continue;
        PortRecipe r;
        r.topic_id = str(rj, "topic_id");
        r.title_match = str(rj, "title_match");
        r.game = str(rj, "game");
        r.default_variant = str(rj, "default");
        r.note = str(rj, "note");
        r.combinable = rj.value("combinable", false);
        if (r.topic_id.empty() && r.title_match.empty()) {
            out_error = "recipes: a recipe needs topic_id or title_match";
            return false;
        }
        if (!rj.contains("variants") || !rj["variants"].is_array() || rj["variants"].empty()) {
            out_error = "recipes: '" + (r.topic_id.empty() ? r.title_match : r.topic_id) + "' has no variants";
            return false;
        }
        for (const auto& vj : rj["variants"]) {
            PortRecipeVariant v;
            v.id = str(vj, "id");
            v.title = str(vj, "title");
            if (v.id.empty()) v.id = "v" + std::to_string(r.variants.size() + 1);
            if (vj.contains("check") && vj["check"].is_array()) {
                for (const auto& c : vj["check"]) {
                    if (c.is_string()) v.check.push_back(c.get<std::string>());
                }
            }
            if (vj.contains("steps") && vj["steps"].is_array()) {
                for (const auto& sj : vj["steps"]) {
                    PortRecipeStep s;
                    if (sj.contains("extract")) {
                        s.kind = PortRecipeStep::Kind::Extract;
                        s.from = str(sj, "extract");
                    } else if (sj.contains("copy")) {
                        s.kind = PortRecipeStep::Kind::Copy;
                        s.from = str(sj, "copy");
                    } else if (sj.contains("install_nsp")) {
                        s.kind = PortRecipeStep::Kind::InstallNsp;
                        s.from = str(sj, "install_nsp");
                    } else {
                        out_error = "recipes: unknown step in '" + v.id + "'";
                        return false;
                    }
                    s.to = str(sj, "to");
                    s.subpath = str(sj, "subpath");
                    const bool autoTarget = s.kind == PortRecipeStep::Kind::Extract && s.to == "auto";
                    if (s.from.empty() || (s.kind != PortRecipeStep::Kind::InstallNsp && !autoTarget && !startsWith(s.to, "sdmc:/"))) {
                        out_error = "recipes: step of '" + v.id + "' needs a path and an sdmc:/ target";
                        return false;
                    }
                    v.steps.push_back(s);
                }
            }
            r.variants.push_back(v);
        }
        out.push_back(std::move(r));
    }
    return true;
}

const PortRecipe* findPortRecipe(const std::vector<PortRecipe>& recipes,
                                 const std::string& topic_id,
                                 const std::string& release_title) {
    if (!topic_id.empty()) {
        for (const auto& r : recipes) {
            if (r.topic_id == topic_id) return &r;
        }
    }
    const std::string lt = lowerName(release_title);
    for (const auto& r : recipes) {
        if (!r.title_match.empty() && lt.find(lowerName(r.title_match)) != std::string::npos) return &r;
    }
    return nullptr;
}

PortRecipeStore& PortRecipeStore::instance() {
    static PortRecipeStore inst;
    return inst;
}

std::string onlinePortRecipesPath() {
    return std::string(TSNX_DATA_DIR) + "/port_recipes_online.json";
}

void PortRecipeStore::reload() {
    std::lock_guard<std::mutex> lk(mtx_);
    loaded_ = false;
    recipes_.clear();
}

bool PortRecipeStore::find(const std::string& topic_id, const std::string& release_title, PortRecipe& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!loaded_) {
        loaded_ = true;
#ifdef __SWITCH__
        const std::string builtin = "romfs:/ports/recipes.json";
#else
        const std::string builtin = "resources/ports/recipes.json";
#endif
        const std::string user = std::string(TSNX_DATA_DIR) + "/port_recipes.json";
        for (const std::string& path : {builtin, onlinePortRecipesPath(), user}) {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            std::stringstream ss;
            ss << in.rdbuf();
            std::vector<PortRecipe> list;
            std::string err;
            if (!parsePortRecipes(ss.str(), list, err)) {
                util::logLine("PortRecipeStore: " + path + ": " + err);
                continue;
            }
            const size_t count = list.size();
            // The user file overrides built-in recipes for the same release.
            for (auto& r : list) {
                auto same = std::find_if(recipes_.begin(), recipes_.end(), [&](const PortRecipe& o) {
                    return (!r.topic_id.empty() && o.topic_id == r.topic_id) ||
                           (r.topic_id.empty() && !r.title_match.empty() && o.title_match == r.title_match);
                });
                if (same != recipes_.end()) *same = std::move(r);
                else recipes_.push_back(std::move(r));
            }
            util::logLine("PortRecipeStore: loaded " + std::to_string(count) + " recipes from " + path);
        }
    }
    const PortRecipe* r = findPortRecipe(recipes_, topic_id, release_title);
    if (!r) return false;
    out = *r;
    return true;
}

bool PortInstallPlanner::analyzeWithRecipe(const PortRecipe& recipe,
                                           const std::string& release_title,
                                           const std::vector<torrent::TorrentFileInfo>& files,
                                           const std::string& preferred_lang,
                                           PortInstallPlan& out,
                                           std::string& out_error) {
    PortInstallPlan plan;
    plan.is_port = true;
    plan.recipe_id = !recipe.topic_id.empty() ? recipe.topic_id : recipe.title_match;
    plan.note = recipe.note;
    plan.port_game_name = !recipe.game.empty() ? recipe.game : sanitizeTitle(release_title);

    std::vector<Entry> entries = prepareEntries(files);
    std::vector<std::string> keys(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) keys[i] = lowerName(join(entries[i].segs, 0, entries[i].segs.size()));

    auto norm = [](const std::string& p) {
        SplitPath sp = splitPath(p);
        return lowerName(join(sp.segs, 0, sp.segs.size()));
    };
    auto findFile = [&](const std::string& from) -> int {
        const std::string k = norm(from);
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == k && !entries[i].unsafe) return static_cast<int>(i);
        }
        return -1;
    };
    auto setDest = [&](size_t pos, const std::string& dest) -> bool {
        const int idx = entries[pos].idx;
        auto it = plan.file_destinations.find(idx);
        if (it != plan.file_destinations.end() && it->second != dest) {
            out_error = "'" + entries[pos].raw + "' has two targets";
            return false;
        }
        plan.file_destinations[idx] = dest;
        return true;
    };

    std::map<size_t, std::set<int>> users;        // entry -> variants that need it
    std::set<size_t> nspEntries;
    std::map<std::string, size_t> taskBySet;      // dir|base -> task position

    for (size_t v = 0; v < recipe.variants.size(); ++v) {
        const auto& rv = recipe.variants[v];
        PortVariant pv;
        pv.id = rv.id;
        pv.title = !rv.title.empty() ? rv.title : rv.id;
        pv.check_paths = rv.check;
        for (const auto& step : rv.steps) {
            if (step.kind == PortRecipeStep::Kind::Extract) {
                int pos = findFile(step.from);
                if (pos < 0) { out_error = "archive not found: " + step.from; return false; }
                // "auto": the archive's own listing picks the folder at install time.
                const bool autoTarget = step.to == "auto";
                const std::string target = autoTarget ? std::string("sdmc:/") : step.to;
                if (isProtectedSdPath(sdRel(target))) { out_error = "protected target " + step.to; return false; }
                const Entry& e = entries[static_cast<size_t>(pos)];
                ArchiveName an = parseArchiveName(e.segs.back());
                if (an.kind == VolKind::None) { out_error = "not an archive: " + step.from; return false; }
                const std::string dir = lowerName(join(e.segs, 0, e.segs.size() - 1));
                const std::string setKey = dir + "|" + an.base;
                std::vector<size_t> vols;
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (entries[i].segs.empty() || entries[i].unsafe) continue;
                    if (lowerName(join(entries[i].segs, 0, entries[i].segs.size() - 1)) != dir) continue;
                    ArchiveName o = parseArchiveName(entries[i].segs.back());
                    if (o.kind != VolKind::None && o.base == an.base) vols.push_back(i);
                }
                std::stable_sort(vols.begin(), vols.end(), [&](size_t a, size_t b) {
                    return parseArchiveName(entries[a].segs.back()).volume < parseArchiveName(entries[b].segs.back()).volume;
                });
                auto tb = taskBySet.find(setKey);
                if (tb != taskBySet.end()) {
                    const auto& t = plan.archives_to_extract[tb->second];
                    if (t.extract_dest_dir != target || t.only_subpath != step.subpath || t.from_recipe == autoTarget) {
                        out_error = "archive used with two targets: " + step.from;
                        return false;
                    }
                } else {
                    PortArchiveTask t;
                    t.file_index = entries[vols.front()].idx;
                    t.archive_torrent_path = join(entries[vols.front()].segs, 0, entries[vols.front()].segs.size());
                    t.extract_dest_dir = target;
                    t.only_subpath = step.subpath;
                    t.from_recipe = !autoTarget;
                    t.dest_reason = autoTarget ? "recipe: auto" : "recipe";
                    for (size_t i : vols) {
                        t.volume_indices.push_back(entries[i].idx);
                        t.compressed_size += entries[i].size;
                    }
                    if (vols.size() > 1 && an.kind != VolKind::Split) {
                        t.unsupported_reason = "multi-volume RAR/ZIP cannot be extracted while downloading; use \"Download full\"";
                    }
                    taskBySet[setKey] = plan.archives_to_extract.size();
                    plan.archives_to_extract.push_back(t);
                }
                for (size_t i : vols) {
                    if (!setDest(i, target)) return false;
                    users[i].insert(static_cast<int>(v));
                }
            } else if (step.kind == PortRecipeStep::Kind::Copy) {
                const bool folder = step.from.back() == '/' || step.from.back() == '\\';
                std::string to = step.to;
                if (folder) {
                    const std::string prefix = norm(step.from) + "/";
                    const size_t prefixSegs = splitPath(step.from).segs.size();
                    if (to.back() != '/') to += '/';
                    bool any = false;
                    for (size_t i = 0; i < entries.size(); ++i) {
                        if (entries[i].unsafe || !startsWith(keys[i], prefix)) continue;
                        const std::string dest = to + join(entries[i].segs, prefixSegs, entries[i].segs.size());
                        if (isProtectedSdPath(sdRel(dest))) { out_error = "protected target " + dest; return false; }
                        if (!setDest(i, dest)) return false;
                        users[i].insert(static_cast<int>(v));
                        any = true;
                    }
                    if (!any) { out_error = "folder not found: " + step.from; return false; }
                } else {
                    int pos = findFile(step.from);
                    if (pos < 0) { out_error = "file not found: " + step.from; return false; }
                    const std::string dest = to.back() == '/' ? to + entries[static_cast<size_t>(pos)].segs.back() : to;
                    if (isProtectedSdPath(sdRel(dest))) { out_error = "protected target " + dest; return false; }
                    if (!setDest(static_cast<size_t>(pos), dest)) return false;
                    users[static_cast<size_t>(pos)].insert(static_cast<int>(v));
                }
            } else {
                int pos = findFile(step.from);
                if (pos < 0) { out_error = "NSP not found: " + step.from; return false; }
                const Entry& e = entries[static_cast<size_t>(pos)];
                if (!isNspFile(e.lowerFile)) { out_error = "not an NSP: " + step.from; return false; }
                if (!setDest(static_cast<size_t>(pos), std::string(TSNX_CACHE_TMP) + "/" + e.segs.back())) return false;
                pv.forwarder_nsp_indices.push_back(e.idx);
                nspEntries.insert(static_cast<size_t>(pos));
                users[static_cast<size_t>(pos)].insert(static_cast<int>(v));
            }
        }
        plan.variants.push_back(pv);
    }

    const size_t nv = plan.variants.size();
    for (const auto& kv : users) {
        const int idx = entries[kv.first].idx;
        if (!nspEntries.count(kv.first) && kv.second.size() == nv) {
            plan.common_file_indices.push_back(idx);
            continue;
        }
        for (int v : kv.second) plan.variants[static_cast<size_t>(v)].file_indices.push_back(idx);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!users.count(i)) plan.skipped_files[entries[i].idx] = "not in recipe";
    }
    std::sort(plan.common_file_indices.begin(), plan.common_file_indices.end());

    if (recipe.combinable && nv >= 2) {
        PortVariant all;
        all.id = "all";
        all.title = "Все версии";
        all.title_key = "app/ports/variant_all";
        for (const auto& v : plan.variants) {
            all.file_indices.insert(all.file_indices.end(), v.file_indices.begin(), v.file_indices.end());
            all.forwarder_nsp_indices.insert(all.forwarder_nsp_indices.end(),
                                             v.forwarder_nsp_indices.begin(), v.forwarder_nsp_indices.end());
            all.check_paths.insert(all.check_paths.end(), v.check_paths.begin(), v.check_paths.end());
        }
        std::sort(all.file_indices.begin(), all.file_indices.end());
        all.file_indices.erase(std::unique(all.file_indices.begin(), all.file_indices.end()), all.file_indices.end());
        plan.variants.push_back(all);
    }

    plan.default_variant_index = 0;
    const bool ruUi = startsWith(lowerName(preferred_lang), "ru");
    bool found = false;
    for (size_t i = 0; i < plan.variants.size() && !recipe.default_variant.empty(); ++i) {
        if (plan.variants[i].id == recipe.default_variant) {
            plan.default_variant_index = static_cast<int>(i);
            found = true;
            break;
        }
    }
    for (size_t i = 0; i < plan.variants.size() && !found; ++i) {
        const std::string& id = plan.variants[i].id;
        if ((ruUi && id == "ru") || (!ruUi && id == "en")) {
            plan.default_variant_index = static_cast<int>(i);
            break;
        }
    }
    for (size_t i = 0; i < plan.variants.size(); ++i) {
        plan.variants[i].total_size = plan.getDownloadSizeForVariant(static_cast<int>(i), files);
    }
    out = std::move(plan);
    return true;
}

PortInstallPlan PortInstallPlanner::plan(const std::string& topic_id,
                                         const std::string& release_title,
                                         const std::vector<torrent::TorrentFileInfo>& files,
                                         const std::string& preferred_lang) {
    PortRecipe recipe;
    if (PortRecipeStore::instance().find(topic_id, release_title, recipe)) {
        PortInstallPlan p;
        std::string err;
        if (analyzeWithRecipe(recipe, release_title, files, preferred_lang, p, err)) {
            util::logLine("PortInstallPlanner: recipe '" + p.recipe_id + "' used for '" + release_title + "'");
            return p;
        }
        util::logLine("PortInstallPlanner: recipe for '" + release_title + "' does not fit (" + err +
                      "), using heuristics");
    }
    return analyze(release_title, files, preferred_lang);
}

} // namespace installer

