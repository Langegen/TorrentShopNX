#include "retro_layout.h"
#include "retro_vita.h"

#include "../config/config.h"
#include "../net/http_client.h"
#include "../utils/app_paths.h"
#include "../utils/archive_utils.h"
#include "../utils/log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

// stb is compiled statically into this file as in net/image_downloader.cpp.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <borealis/extern/nanovg/stb_image.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

namespace fs = std::filesystem;

namespace catalog {
namespace retro {

namespace {

constexpr int kCoverSize = 512;

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segs;
    std::string cur;
    for (char ch : path) {
        if (ch == '/' || ch == '\\') {
            if (!cur.empty() && cur != "." && cur != "..") segs.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty() && cur != "." && cur != "..") segs.push_back(cur);
    return segs;
}

std::string extOf(const std::string& name) {
    std::string base = name;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return toLower(base.substr(dot));
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool isLaunchExt(const RetroConsoleInfo& c, const std::string& ext) {
    return !ext.empty() && contains(c.launch_extensions, ext);
}

// Cue/gdi sheets only make sense together with their track files.
bool isSheetExt(const std::string& ext) {
    return ext == ".cue" || ext == ".gdi";
}

// Track files listed by a cue/gdi sheet lying in the same folder.
bool isTrackExt(const std::string& ext) {
    return ext == ".bin" || ext == ".img" || ext == ".raw" || ext == ".sub";
}

// Files no emulator reads: release notes, scans, checksums, players, videos.
bool isJunkExt(const std::string& ext) {
    static const std::set<std::string> junk = {
        ".nfo", ".txt", ".url", ".diz", ".sfv", ".md5", ".sha1", ".sha", ".crc",
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp",
        ".pdf", ".htm", ".html", ".doc", ".docx", ".rtf",
        ".db", ".log", ".lnk", ".exe", ".bat", ".torrent",
        ".mp3", ".avi", ".mkv", ".mp4"
    };
    return junk.count(ext) > 0;
}

// Archive volumes: "x.7z.001" (byte split), "x.part2.rar", "x.r00" (old RAR),
// "x.z01" (spanned ZIP), or a plain archive.
enum class VolKind { None, Plain, Split, RarParts, RarOld, ZipSpan };

struct VolumeName {
    VolKind kind = VolKind::None;
    std::string base;   // lower-case name shared by all volumes
    int volume = 0;
};

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

VolumeName parseVolumeName(const std::string& fileName) {
    VolumeName v;
    std::string lf = toLower(fileName);
    size_t slash = lf.find_last_of("/\\");
    if (slash != std::string::npos) lf = lf.substr(slash + 1);
    const size_t n = lf.size();
    if (n > 4 && lf[n - 4] == '.' && allDigits(lf.substr(n - 3))) {
        std::string pre = lf.substr(0, n - 4);
        if (endsWith(pre, ".7z") || endsWith(pre, ".zip") || endsWith(pre, ".rar")) {
            v.kind = VolKind::Split;
            v.base = pre;
            v.volume = std::atoi(lf.substr(n - 3).c_str());
            return v;
        }
    }
    if (n > 4 && lf[n - 4] == '.' && allDigits(lf.substr(n - 2))) {
        char k = lf[n - 3];
        std::string pre = lf.substr(0, n - 4);
        if (k == 'r') {
            v.kind = VolKind::RarOld;
            v.base = pre + ".rar";
            v.volume = std::atoi(lf.substr(n - 2).c_str()) + 2;
            return v;
        }
        if (k == 'z') {
            v.kind = VolKind::ZipSpan;
            v.base = pre + ".zip";
            v.volume = std::atoi(lf.substr(n - 2).c_str());
            return v;
        }
    }
    if (endsWith(lf, ".rar")) {
        std::string pre = lf.substr(0, n - 4);
        size_t pp = pre.rfind(".part");
        if (pp != std::string::npos && allDigits(pre.substr(pp + 5))) {
            v.kind = VolKind::RarParts;
            v.base = pre.substr(0, pp) + ".rar";
            v.volume = std::atoi(pre.substr(pp + 5).c_str());
            return v;
        }
    }
    if (util::isArchiveFile(lf)) {
        v.kind = VolKind::Plain;
        v.base = lf;
        v.volume = endsWith(lf, ".zip") ? 1000 : 1; // a spanned zip ends with .zip
    }
    return v;
}

// Lower is better when one disc exists in several formats.
int formatPriority(const std::string& ext) {
    static const std::vector<std::string> order = {
        ".chd", ".cue", ".gdi", ".cdi", ".pbp", ".cso", ".iso", ".rvz", ".wbfs",
        ".gcz", ".ciso", ".wia", ".gcm", ".wad", ".ecm", ".bin"
    };
    auto it = std::find(order.begin(), order.end(), ext);
    return it == order.end() ? static_cast<int>(order.size()) : static_cast<int>(it - order.begin());
}

std::string joinSanitized(const std::vector<std::string>& segs, size_t from, const PathNaming& naming) {
    std::string out;
    for (size_t i = from; i < segs.size(); ++i) {
        std::string s = naming.component ? naming.component(segs[i]) : segs[i];
        if (s.empty()) continue;
        if (!out.empty()) out += '/';
        out += s;
    }
    return out;
}

std::vector<fs::path> listFilesRecursive(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2)) out.push_back(it->path());
    }
    return out;
}

bool isDirEmpty(const fs::path& dir) {
    std::error_code ec;
    return fs::is_empty(dir, ec) && !ec;
}

// "Final Fantasy VII (USA) (Disc 2)" -> {"Final Fantasy VII (USA)", 2}.
// Recognises Disc/Disk/CD followed by 1..10, in brackets or not.
std::pair<std::string, int> splitDiscTag(const std::string& stem) {
    std::string lower = toLower(stem);
    static const char* words[] = {"disc", "disk", "cd"};
    for (size_t pos = 0; pos < lower.size(); ++pos) {
        for (const char* w : words) {
            size_t wl = std::strlen(w);
            if (lower.compare(pos, wl, w) != 0) continue;
            if (pos > 0) {
                char prev = lower[pos - 1];
                if (std::isalnum(static_cast<unsigned char>(prev))) continue;
            }
            size_t j = pos + wl;
            while (j < lower.size() && (lower[j] == ' ' || lower[j] == '_' || lower[j] == '-')) ++j;
            size_t digits_start = j;
            while (j < lower.size() && std::isdigit(static_cast<unsigned char>(lower[j])) && j - digits_start < 2) ++j;
            if (j == digits_start) continue;
            if (j < lower.size() && std::isalnum(static_cast<unsigned char>(lower[j]))) continue;
            int num = std::atoi(lower.substr(digits_start, j - digits_start).c_str());
            if (num < 1 || num > 10) continue;

            // "Disc 1 of 3"
            size_t end = j;
            if (lower.compare(end, 4, " of ") == 0) {
                size_t k = end + 4;
                while (k < lower.size() && std::isdigit(static_cast<unsigned char>(lower[k]))) ++k;
                if (k > end + 4) end = k;
            }
            size_t start = pos;
            if (start > 0 && (lower[start - 1] == '(' || lower[start - 1] == '[') &&
                end < lower.size() && (lower[end] == ')' || lower[end] == ']')) {
                --start;
                ++end;
            }
            std::string key = stem.substr(0, start) + stem.substr(end);
            // Tidy separators left behind ("Game - _" -> "Game").
            std::string tidy;
            for (char ch : key) {
                if (ch == ' ' && !tidy.empty() && tidy.back() == ' ') continue;
                tidy.push_back(ch);
            }
            while (!tidy.empty() && (tidy.back() == ' ' || tidy.back() == '-' || tidy.back() == '_' || tidy.back() == '.')) tidy.pop_back();
            while (!tidy.empty() && (tidy.front() == ' ' || tidy.front() == '-' || tidy.front() == '_')) tidy.erase(tidy.begin());
            return {tidy, num};
        }
    }
    return {stem, 0};
}

bool isGenericName(const std::string& key) {
    std::string k = toLower(key);
    if (k.size() < 2) return true;
    if (std::all_of(k.begin(), k.end(), [](unsigned char c) { return std::isdigit(c) || c == ' ' || c == '_'; })) return true;
    static const std::set<std::string> generic = {
        "disc", "disk", "cd", "game", "image", "track", "track01", "track 01", "rom", "iso", "data"
    };
    return generic.count(k) > 0;
}

// A file name that is not taken yet in dir: "Name.ext", "Name (2).ext", ...
fs::path uniquePath(const fs::path& dir, const std::string& filename) {
    fs::path candidate = dir / filename;
    std::error_code ec;
    if (!fs::exists(candidate, ec)) return candidate;
    std::string stem = fs::path(filename).stem().string();
    std::string ext = fs::path(filename).extension().string();
    for (int n = 2; n < 1000; ++n) {
        candidate = dir / (stem + " (" + std::to_string(n) + ")" + ext);
        if (!fs::exists(candidate, ec)) return candidate;
    }
    return dir / filename;
}

bool moveFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::rename(from, to, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(from, ec);
    return true;
}

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeFile(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// ---------------------------------------------------------------- archives

bool extractWith(const fs::path& archive, const fs::path& dest_dir,
                 const util::PathTransformFn& transform,
                 const FinalizeCallbacks& cb,
                 const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::string name = archive.filename().string();
    if (cb.on_extract) cb.on_extract(name, 0);
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    std::string err;
    bool ok = util::extractArchive(archive.string(), dest_dir.string(),
        [&](const util::ArchiveProgress& p) {
            if (cb.on_extract) cb.on_extract(name, static_cast<int>(std::clamp(p.percentage, 0.0f, 100.0f)));
        },
        cancel, err, transform);
    if (ok) {
        util::logLine("retro: unpacked " + archive.string() + " -> " + dest_dir.string());
        fs::remove(archive, ec);
    } else {
        util::logLine("retro: unpack failed for " + archive.string() + ": " + err);
    }
    return ok;
}

// Files inside an archive on the SD card (folders left out).
bool listArchiveFiles(const fs::path& archive, std::vector<std::string>& out) {
    out.clear();
    return util::iterateArchiveEntries(archive.string(), [&](const std::string& name, bool isDir) -> bool {
        if (!isDir) out.push_back(name);
        return true;
    });
}

// One folder wrapping every file of the archive ("Game/Game.cue") is dropped.
size_t wrapperDepth(const std::vector<std::string>& entries) {
    if (entries.empty()) return 0;
    std::string top;
    for (const auto& e : entries) {
        std::vector<std::string> segs = splitPath(e);
        if (segs.size() < 2) return 0;
        std::string first = toLower(segs[0]);
        if (top.empty()) top = first;
        else if (first != top) return 0;
    }
    return 1;
}

// ------------------------------------------------------------- cue sheets

// Releases often rename track files (transliteration, case) without touching
// the .cue, which then points at files that do not exist.
void fixCueSheets(const fs::path& dir, const PathNaming& naming) {
    for (const auto& cue : listFilesRecursive(dir)) {
        if (extOf(cue.filename().string()) != ".cue") continue;
        std::string body;
        if (!readFile(cue, body)) continue;
        bool crlf = body.find("\r\n") != std::string::npos;

        std::vector<std::string> lines;
        {
            std::string cur;
            for (char ch : body) {
                if (ch == '\n') {
                    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                    lines.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(ch);
                }
            }
            if (!cur.empty()) lines.push_back(cur);
        }

        fs::path cueDir = cue.parent_path();
        std::vector<std::string> listing;
        std::vector<std::string> trackFiles;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(cueDir, ec)) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2)) continue;
            std::string fn = e.path().filename().string();
            listing.push_back(fn);
            std::string ext = extOf(fn);
            if (ext == ".bin" || ext == ".img") trackFiles.push_back(fn);
        }

        int fileLines = 0;
        for (const auto& l : lines) {
            size_t p = l.find_first_not_of(" \t");
            if (p != std::string::npos && toLower(l.substr(p, 5)) == "file ") ++fileLines;
        }

        bool changed = false;
        for (auto& line : lines) {
            size_t p = line.find_first_not_of(" \t");
            if (p == std::string::npos || toLower(line.substr(p, 5)) != "file ") continue;
            size_t q = line.find_first_not_of(' ', p + 5);
            if (q == std::string::npos) continue;
            std::string name, rest;
            if (line[q] == '"') {
                size_t close = line.find('"', q + 1);
                if (close == std::string::npos) continue;
                name = line.substr(q + 1, close - q - 1);
                rest = line.substr(close + 1);
            } else {
                size_t sp = line.find(' ', q);
                name = line.substr(q, sp == std::string::npos ? std::string::npos : sp - q);
                rest = sp == std::string::npos ? "" : line.substr(sp);
            }
            if (fs::exists(cueDir / name, ec)) continue;

            std::string found;
            std::string sanitized = naming.component ? naming.component(fs::path(name).filename().string()) : name;
            for (const auto& fn : listing) {
                std::string lfn = toLower(fn);
                if (lfn == toLower(name) || lfn == toLower(sanitized) ||
                    lfn == toLower(fs::path(name).filename().string())) {
                    found = fn;
                    break;
                }
            }
            if (found.empty() && fileLines == 1 && trackFiles.size() == 1) found = trackFiles.front();
            if (found.empty()) continue;

            line = line.substr(0, p) + "FILE \"" + found + "\"" + rest;
            changed = true;
        }

        if (changed) {
            std::string out;
            for (const auto& l : lines) {
                out += l;
                out += crlf ? "\r\n" : "\n";
            }
            if (writeFile(cue, out)) util::logLine("retro: fixed track names in " + cue.string());
        }
    }
}

// --------------------------------------------------------- disc images

std::string msf(long lba) {
    if (lba < 0) lba = 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", lba / 75 / 60, (lba / 75) % 60, lba % 75);
    return buf;
}

fs::path siblingWithExt(const fs::path& file, const std::string& ext) {
    std::error_code ec;
    std::string stem = toLower(file.stem().string());
    for (const auto& e : fs::directory_iterator(file.parent_path(), ec)) {
        if (toLower(e.path().stem().string()) == stem && extOf(e.path().filename().string()) == ext) return e.path();
    }
    return fs::path();
}

// CloneCD (.ccd + .img) -> .cue: DuckStation and Genesis Plus GX read cue sheets.
void convertCcd(const fs::path& ccd, const std::string& dataMode) {
    fs::path img = siblingWithExt(ccd, ".img");
    if (img.empty() || !siblingWithExt(ccd, ".cue").empty()) return;
    std::string body;
    if (!readFile(ccd, body)) return;

    struct Track {
        int mode = -1;
        long index0 = -1;
        long index1 = -1;
    };
    std::map<int, Track> tracks;
    int current = -1;
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::string l = toLower(line);
        if (l.rfind("[track ", 0) == 0) {
            current = std::atoi(l.substr(7).c_str());
            continue;
        }
        if (!l.empty() && l[0] == '[') {
            current = -1;
            continue;
        }
        if (current <= 0) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = l.substr(0, eq), val = l.substr(eq + 1);
        if (key == "mode") tracks[current].mode = std::atoi(val.c_str());
        else if (key == "index 0") tracks[current].index0 = std::atol(val.c_str());
        else if (key == "index 1") tracks[current].index1 = std::atol(val.c_str());
    }

    std::string cue = "FILE \"" + img.filename().string() + "\" BINARY\n";
    if (tracks.empty()) {
        cue += "  TRACK 01 " + dataMode + "\n    INDEX 01 00:00:00\n";
    } else {
        for (const auto& kv : tracks) {
            char head[32];
            std::snprintf(head, sizeof(head), "  TRACK %02d ", kv.first);
            std::string mode = kv.second.mode == 0 ? "AUDIO" : kv.second.mode == 1 ? "MODE1/2352" : "MODE2/2352";
            cue += head + mode + "\n";
            if (kv.second.index0 >= 0) cue += "    INDEX 00 " + msf(kv.second.index0) + "\n";
            cue += "    INDEX 01 " + msf(kv.second.index1 >= 0 ? kv.second.index1 : 0) + "\n";
        }
    }
    fs::path out = ccd;
    out.replace_extension(".cue");
    if (writeFile(out, cue)) util::logLine("retro: made " + out.string() + " from the CloneCD image");
}

// Alcohol (.mds + .mdf): a cue for the data track, enough for most single-track discs.
void convertMds(const fs::path& mds, const std::string& dataMode) {
    fs::path mdf = siblingWithExt(mds, ".mdf");
    if (mdf.empty() || !siblingWithExt(mds, ".cue").empty()) return;
    std::error_code ec;
    uintmax_t size = fs::file_size(mdf, ec);
    if (ec || size == 0) return;
    std::string mode;
    if (size % 2352 == 0) mode = dataMode;
    else if (size % 2048 == 0) mode = "MODE1/2048";
    else return;
    std::string cue = "FILE \"" + mdf.filename().string() + "\" BINARY\n  TRACK 01 " + mode + "\n    INDEX 01 00:00:00\n";
    fs::path out = mds;
    out.replace_extension(".cue");
    if (writeFile(out, cue)) util::logLine("retro: made " + out.string() + " from the Alcohol image");
}

void convertDiscImages(const RetroConsoleInfo& c, const fs::path& dir) {
    std::string dataMode;
    if (c.id == "ps1") dataMode = "MODE2/2352";
    else if (c.id == "sega_cd") dataMode = "MODE1/2352";
    else return;
    for (const auto& f : listFilesRecursive(dir)) {
        std::string ext = extOf(f.filename().string());
        if (ext == ".ccd") convertCcd(f, dataMode);
        else if (ext == ".mds") convertMds(f, dataMode);
    }
}

// ---------------------------------------------------------------- playlists

struct DiscFile {
    fs::path path;
    int number = 0;
    int priority = 0;
};

// One .m3u per game found in the release folder, written next to the folder.
std::vector<fs::path> writePlaylists(const RetroConsoleInfo& c, const fs::path& base,
                                     const fs::path& gameDir) {
    std::vector<fs::path> files = listFilesRecursive(gameDir);

    std::set<std::string> dirsWithSheets;
    for (const auto& f : files) {
        if (isSheetExt(extOf(f.filename().string()))) dirsWithSheets.insert(f.parent_path().string());
    }

    struct Group {
        std::string key;
        std::map<int, DiscFile> discs; // disc number -> best file
    };
    std::map<std::string, Group> groups;

    for (const auto& f : files) {
        std::string ext = extOf(f.filename().string());
        if (!isLaunchExt(c, ext)) continue;
        if (isTrackExt(ext) && dirsWithSheets.count(f.parent_path().string())) continue;

        auto tag = splitDiscTag(f.stem().string());
        int number = tag.second;
        std::string key = tag.first;
        if (number == 0 && f.parent_path() != gameDir) {
            auto dirTag = splitDiscTag(f.parent_path().filename().string());
            if (dirTag.second > 0) number = dirTag.second;
        }
        std::string groupId = toLower(key);
        Group& g = groups[groupId];
        if (g.key.empty()) g.key = key;

        DiscFile d{f, number, formatPriority(ext)};
        auto it = g.discs.find(number);
        if (it == g.discs.end() || d.priority < it->second.priority) g.discs[number] = d;
    }

    std::vector<fs::path> playlists;
    std::string folderName = gameDir.filename().string();
    for (const auto& kv : groups) {
        const Group& g = kv.second;
        if (g.discs.empty()) continue;

        std::string name;
        if (groups.size() == 1) name = folderName;
        else if (isGenericName(g.key)) name = folderName + " - " + std::to_string(playlists.size() + 1);
        else name = g.key;

        std::string content;
        for (const auto& disc : g.discs) {
            std::error_code ec;
            fs::path rel = fs::relative(disc.second.path, base, ec);
            if (ec) continue;
            content += rel.generic_string();
            content += "\n";
        }
        if (content.empty()) continue;

        fs::path m3u = base / (name + ".m3u");
        std::string existing;
        std::error_code ec;
        if (fs::exists(m3u, ec) && (!readFile(m3u, existing) || existing != content)) {
            m3u = uniquePath(base, name + ".m3u");
        }
        if (writeFile(m3u, content)) {
            util::logLine("retro: playlist " + m3u.string() + " (" + std::to_string(g.discs.size()) + " disc(s))");
            playlists.push_back(m3u);
        }
    }
    return playlists;
}

// ------------------------------------------------------------------ images

std::vector<uint8_t> resample(const uint8_t* src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 3);
    if (dw <= sw && dh <= sh) {
        // Area average when shrinking.
        float xr = static_cast<float>(sw) / dw, yr = static_cast<float>(sh) / dh;
        for (int y = 0; y < dh; ++y) {
            int y0 = static_cast<int>(y * yr), y1 = std::max(y0 + 1, std::min(sh, static_cast<int>((y + 1) * yr)));
            for (int x = 0; x < dw; ++x) {
                int x0 = static_cast<int>(x * xr), x1 = std::max(x0 + 1, std::min(sw, static_cast<int>((x + 1) * xr)));
                uint32_t acc[3] = {0, 0, 0};
                uint32_t n = 0;
                for (int sy = y0; sy < y1; ++sy) {
                    const uint8_t* row = src + (static_cast<size_t>(sy) * sw + x0) * 3;
                    for (int sx = x0; sx < x1; ++sx, row += 3) {
                        acc[0] += row[0]; acc[1] += row[1]; acc[2] += row[2];
                        ++n;
                    }
                }
                uint8_t* out = &dst[(static_cast<size_t>(y) * dw + x) * 3];
                for (int k = 0; k < 3; ++k) out[k] = static_cast<uint8_t>(acc[k] / std::max<uint32_t>(1, n));
            }
        }
        return dst;
    }
    // Bilinear when enlarging.
    for (int y = 0; y < dh; ++y) {
        float fy = (y + 0.5f) * sh / dh - 0.5f;
        int y0 = std::clamp(static_cast<int>(fy), 0, sh - 1), y1 = std::min(y0 + 1, sh - 1);
        float wy = std::clamp(fy - y0, 0.0f, 1.0f);
        for (int x = 0; x < dw; ++x) {
            float fx = (x + 0.5f) * sw / dw - 0.5f;
            int x0 = std::clamp(static_cast<int>(fx), 0, sw - 1), x1 = std::min(x0 + 1, sw - 1);
            float wx = std::clamp(fx - x0, 0.0f, 1.0f);
            uint8_t* out = &dst[(static_cast<size_t>(y) * dw + x) * 3];
            for (int k = 0; k < 3; ++k) {
                float a = src[(static_cast<size_t>(y0) * sw + x0) * 3 + k];
                float b = src[(static_cast<size_t>(y0) * sw + x1) * 3 + k];
                float cc = src[(static_cast<size_t>(y1) * sw + x0) * 3 + k];
                float d = src[(static_cast<size_t>(y1) * sw + x1) * 3 + k];
                float top = a + (b - a) * wx, bottom = cc + (d - cc) * wx;
                out[k] = static_cast<uint8_t>(std::clamp(top + (bottom - top) * wy, 0.0f, 255.0f));
            }
        }
    }
    return dst;
}

void appendToVector(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    v->insert(v->end(), p, p + size);
}

// Box art is rarely square: fit it into 512x512 and fill the sides with the
// average colour of the picture's edge so it does not look letterboxed.
bool encodeSquareCover(const std::string& imageData, const fs::path& outPath) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(imageData.data()),
                                            static_cast<int>(imageData.size()), &w, &h, &comp, 3);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }

    float scale = std::min(static_cast<float>(kCoverSize) / w, static_cast<float>(kCoverSize) / h);
    int dw = std::max(1, std::min(kCoverSize, static_cast<int>(w * scale + 0.5f)));
    int dh = std::max(1, std::min(kCoverSize, static_cast<int>(h * scale + 0.5f)));
    std::vector<uint8_t> scaled = resample(pixels, w, h, dw, dh);

    uint64_t acc[3] = {0, 0, 0};
    uint64_t n = 0;
    auto addPixel = [&](int x, int y) {
        const stbi_uc* p = pixels + (static_cast<size_t>(y) * w + x) * 3;
        acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
        ++n;
    };
    for (int x = 0; x < w; ++x) { addPixel(x, 0); addPixel(x, h - 1); }
    for (int y = 0; y < h; ++y) { addPixel(0, y); addPixel(w - 1, y); }
    stbi_image_free(pixels);
    uint8_t fill[3] = {
        static_cast<uint8_t>(acc[0] / std::max<uint64_t>(1, n)),
        static_cast<uint8_t>(acc[1] / std::max<uint64_t>(1, n)),
        static_cast<uint8_t>(acc[2] / std::max<uint64_t>(1, n))
    };

    std::vector<uint8_t> canvas(static_cast<size_t>(kCoverSize) * kCoverSize * 3);
    for (size_t i = 0; i < canvas.size(); i += 3) {
        canvas[i] = fill[0]; canvas[i + 1] = fill[1]; canvas[i + 2] = fill[2];
    }
    int ox = (kCoverSize - dw) / 2, oy = (kCoverSize - dh) / 2;
    for (int y = 0; y < dh; ++y) {
        std::memcpy(&canvas[(static_cast<size_t>(oy + y) * kCoverSize + ox) * 3],
                    &scaled[static_cast<size_t>(y) * dw * 3], static_cast<size_t>(dw) * 3);
    }

    std::vector<uint8_t> jpeg;
    stbi_write_jpg_to_func(appendToVector, &jpeg, kCoverSize, kCoverSize, 3, canvas.data(), 90);
    if (jpeg.empty()) return false;
    return writeFile(outPath, std::string(reinterpret_cast<const char*>(jpeg.data()), jpeg.size()));
}

// Moves everything from one folder into another without overwriting.
int moveDirContents(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(from, ec) || !fs::is_directory(from, ec)) return 0;
    fs::create_directories(to, ec);
    int moved = 0;
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(from, ec)) entries.push_back(e.path());
    for (const auto& src : entries) {
        fs::path dst = to / src.filename();
        if (fs::exists(dst, ec)) {
            util::logLine("retro: kept " + src.string() + " (already exists in " + to.string() + ")");
            continue;
        }
        ec.clear();
        fs::rename(src, dst, ec);
        if (ec) {
            util::logLine("retro: could not move " + src.string() + ": " + ec.message());
            continue;
        }
        ++moved;
    }
    if (isDirEmpty(from)) fs::remove(from, ec);
    return moved;
}

std::string markerPath(const char* name) {
    return std::string(TSNX_RETRO_DATA_DIR) + "/" + name;
}

bool hasMarker(const char* name) {
    std::error_code ec;
    return fs::exists(markerPath(name), ec);
}

void setMarker(const char* name) {
    tsnx_ensure_dir(TSNX_RETRO_DATA_DIR);
    writeFile(markerPath(name), "1\n");
}

std::vector<fs::path> legacyDirs(const char* subfolder) {
    std::vector<fs::path> dirs = {
        fs::path(TSNX_ROMS_BASE_DIR) / subfolder,
        fs::path(TSNX_DOWNLOADS_DIR) / "retro" / subfolder
    };
    const std::string& custom = config::ConfigManager::instance().getRetroCustomPath();
    if (!custom.empty()) dirs.push_back(fs::path(custom) / subfolder);
    return dirs;
}

int countEntries(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return 0;
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        (void)e;
        ++n;
    }
    return n;
}

} // namespace

// ================================================================== layout

LayoutPlan planLayout(const RetroConsoleInfo& c,
                      const std::string& release_title,
                      const std::vector<TorrentFileRef>& files,
                      const PathNaming& naming) {
    LayoutPlan plan;
    plan.base_dir = RetroCatalogManager::instance().getTargetDir(c.id);
    plan.release_folder = naming.release_folder ? naming.release_folder(release_title) : "game";
    if (plan.release_folder.empty()) plan.release_folder = "game";

    struct Item {
        const TorrentFileRef* ref;
        std::vector<std::string> segs;
        std::string ext;
        bool launch;
        bool archive;
    };
    auto makeItem = [&](const TorrentFileRef& f) {
        Item it{&f, splitPath(f.path), "", false, false};
        if (!it.segs.empty()) {
            it.ext = extOf(it.segs.back());
            it.launch = isLaunchExt(c, it.ext);
            it.archive = isArchiveName(c, it.segs.back());
        }
        return it;
    };

    // PS Vita folder installs carry icons (sce_sys/*.png), so nothing is dropped there.
    bool dropJunk = c.route != RetroRoute::Vita3K;
    std::vector<Item> items;
    for (const auto& f : files) {
        Item it = makeItem(f);
        if (it.segs.empty()) continue;
        if (dropJunk && isJunkExt(it.ext)) continue;
        items.push_back(std::move(it));
    }
    if (items.empty()) {
        // Nothing but notes and pictures: keep the release as it is.
        for (const auto& f : files) {
            Item it = makeItem(f);
            if (!it.segs.empty()) items.push_back(std::move(it));
        }
    }

    // A torrent usually wraps everything in one top folder: drop it.
    size_t common = 0;
    if (!items.empty()) {
        bool same = true;
        for (const auto& it : items) {
            if (it.segs.size() < 2 || toLower(it.segs[0]) != toLower(items[0].segs[0])) {
                same = false;
                break;
            }
        }
        if (same) common = 1;
    }

    auto placeInGameDir = [&](const std::vector<const Item*>& sel) {
        plan.game_dir = plan.release_folder;
        for (const Item* it : sel) {
            std::string rel = joinSanitized(it->segs, common, naming);
            if (rel.empty()) continue;
            plan.dest_rel[it->ref->path] = plan.game_dir + "/" + rel;
            plan.indices.push_back(it->ref->index);
        }
    };

    auto placeFlat = [&](const std::vector<const Item*>& sel) {
        std::set<std::string> used;
        for (const Item* it : sel) {
            std::string name = naming.component ? naming.component(it->segs.back()) : it->segs.back();
            if (name.empty()) continue;
            if (used.count(toLower(name))) {
                // Two files with one name (e.g. RUS/Game.nes and ENG/Game.nes).
                std::string stem = fs::path(name).stem().string();
                std::string ext = fs::path(name).extension().string();
                std::string parent = it->segs.size() >= 2
                    ? (naming.component ? naming.component(it->segs[it->segs.size() - 2]) : it->segs[it->segs.size() - 2])
                    : "";
                std::string candidate = parent.empty() ? name : (stem + " (" + parent + ")" + ext);
                for (int n = 2; used.count(toLower(candidate)); ++n) {
                    candidate = stem + " (" + std::to_string(n) + ")" + ext;
                }
                name = candidate;
            }
            used.insert(toLower(name));
            plan.dest_rel[it->ref->path] = name;
            plan.indices.push_back(it->ref->index);
        }
    };

    std::vector<const Item*> all;
    for (const auto& it : items) all.push_back(&it);

    if (c.route == RetroRoute::Vita3K || c.route == RetroRoute::Cemu) {
        if (all.size() == 1) placeFlat(all);
        else placeInGameDir(all);
    } else if (c.disc_based && c.supports_m3u) {
        if (all.size() == 1 && all[0]->launch && !isSheetExt(all[0]->ext)) placeFlat(all);
        else placeInGameDir(all);
    } else {
        // Tico does not look into subfolders: games and archives lie flat.
        std::vector<const Item*> sel;
        for (const Item* it : all) {
            if (it->launch || it->archive) sel.push_back(it);
        }
        if (sel.empty()) placeInGameDir(all);
        else placeFlat(sel);
    }

    util::logLine("retro: layout for " + c.id + " -> " + plan.base_dir +
                  (plan.game_dir.empty() ? std::string(" (flat)") : (" / " + plan.game_dir)) +
                  ", " + std::to_string(plan.indices.size()) + " of " + std::to_string(files.size()) + " files");
    return plan;
}

bool isArchiveName(const RetroConsoleInfo& c, const std::string& name) {
    if (isLaunchExt(c, extOf(name))) return false; // NetherSX2 reads .gz itself
    return parseVolumeName(name).kind != VolKind::None;
}

std::string archiveSetKey(const std::string& torrent_path) {
    std::string dir;
    size_t slash = torrent_path.find_last_of("/\\");
    if (slash != std::string::npos) dir = toLower(torrent_path.substr(0, slash));
    VolumeName v = parseVolumeName(torrent_path);
    std::string file = slash == std::string::npos ? torrent_path : torrent_path.substr(slash + 1);
    return dir + "/" + (v.base.empty() ? toLower(file) : v.base);
}

int archiveVolumeNumber(const std::string& name) {
    return parseVolumeName(name).volume;
}

bool isStreamableArchiveSet(const std::vector<std::string>& volume_names) {
    if (volume_names.empty()) return false;
    if (volume_names.size() == 1) {
        VolKind k = parseVolumeName(volume_names[0]).kind;
        return k == VolKind::Plain || k == VolKind::Split;
    }
    for (const auto& n : volume_names) {
        if (parseVolumeName(n).kind != VolKind::Split) return false;
    }
    return true;
}

ArchiveAction chooseArchiveAction(const RetroConsoleInfo& c, bool archive_is_flat,
                                  const std::vector<std::string>* entry_names) {
    if (c.route == RetroRoute::Vita3K) return ArchiveAction::Keep; // Vita3K installs archives itself
    if (c.keep_archives && archive_is_flat) {
        if (!entry_names) return ArchiveAction::Keep;
        int games = 0;
        bool atRoot = true;
        for (const auto& e : *entry_names) {
            if (!isLaunchExt(c, extOf(e))) continue;
            ++games;
            if (splitPath(e).size() > 1) atRoot = false;
        }
        if (games == 0 || (games == 1 && atRoot)) return ArchiveAction::Keep;
        return ArchiveAction::ExtractGamesFlat;
    }
    if (c.route == RetroRoute::Cemu || (c.disc_based && c.supports_m3u)) return ArchiveAction::ExtractToFolder;
    return ArchiveAction::ExtractGamesFlat;
}

ExtractTarget makeExtractTarget(const RetroConsoleInfo& c, ArchiveAction action,
                                const std::string& base_dir, const std::string& folder_dir,
                                const std::vector<std::string>& entry_names, const PathNaming& naming) {
    ExtractTarget t;
    t.placed_top = std::make_shared<std::vector<std::string>>();
    if (action == ArchiveAction::ExtractGamesFlat) {
        t.dest_dir = base_dir;
        auto taken = std::make_shared<std::set<std::string>>();
        auto placed = t.placed_top;
        RetroConsoleInfo cc = c;
        PathNaming nm = naming;
        std::string base = base_dir;
        t.transform = [cc, nm, base, taken, placed](const std::string& entry, bool isDir) -> std::string {
            if (isDir) return "";
            std::vector<std::string> segs = splitPath(entry);
            if (segs.empty() || !isLaunchExt(cc, extOf(segs.back()))) return "";
            std::string name = nm.component ? nm.component(segs.back()) : segs.back();
            if (name.empty()) return "";
            if (taken->count(toLower(name))) {
                // Same name twice in one archive (RUS/Game.nes and ENG/Game.nes).
                std::string stem = fs::path(name).stem().string();
                std::string ext = fs::path(name).extension().string();
                std::string candidate = name;
                for (int n = 2; taken->count(toLower(candidate)); ++n) {
                    candidate = stem + " (" + std::to_string(n) + ")" + ext;
                }
                name = candidate;
            }
            taken->insert(toLower(name));
            placed->push_back((fs::path(base) / name).string());
            return name;
        };
    } else if (action == ArchiveAction::ExtractToFolder) {
        t.dest_dir = folder_dir;
        size_t skip = wrapperDepth(entry_names);
        bool dropJunk = c.route != RetroRoute::Vita3K;
        PathNaming nm = naming;
        t.transform = [nm, skip, dropJunk](const std::string& entry, bool isDir) -> std::string {
            std::vector<std::string> segs = splitPath(entry);
            if (segs.size() <= skip) return "";
            if (!isDir && dropJunk && isJunkExt(extOf(segs.back()))) return "";
            return joinSanitized(segs, skip, nm);
        };
    }
    return t;
}

FinalizeResult finalizeRelease(const RetroConsoleInfo& c,
                               const LayoutPlan& plan,
                               const PathNaming& naming,
                               const FinalizeCallbacks& cb,
                               const std::shared_ptr<std::atomic<bool>>& cancel) {
    FinalizeResult res;
    const fs::path base(plan.base_dir);
    std::string gameDirName = plan.game_dir;
    std::vector<fs::path> top;
    auto cancelled = [&]() { return cancel && cancel->load(); };

    for (const auto& placed : plan.placed_top) {
        std::error_code e0;
        if (fs::exists(placed, e0)) top.push_back(placed);
    }

    // Archives still on the SD card (not unpacked while downloading), grouped by volume set.
    struct ArchiveSet {
        std::vector<std::pair<int, fs::path>> volumes;
        bool flat = true;
    };
    std::map<std::string, ArchiveSet> sets;
    std::error_code ec;
    for (const auto& kv : plan.dest_rel) {
        fs::path full = base / kv.second;
        if (!fs::exists(full, ec)) continue;
        bool flat = kv.second.find('/') == std::string::npos;
        if (c.route != RetroRoute::Vita3K && isArchiveName(c, kv.second)) {
            ArchiveSet& set = sets[archiveSetKey(kv.second)];
            set.volumes.push_back({archiveVolumeNumber(kv.second), full});
            set.flat = flat;
        } else if (flat) {
            top.push_back(full);
        }
    }

    std::set<std::string> failedArchives;

    // 1. Archives.
    for (auto& kv : sets) {
        if (cancelled()) break;
        ArchiveSet& set = kv.second;
        std::sort(set.volumes.begin(), set.volumes.end());
        const fs::path first = set.volumes.front().second;

        std::vector<std::string> entries;
        bool listed = listArchiveFiles(first, entries);
        ArchiveAction action = chooseArchiveAction(c, set.flat, listed ? &entries : nullptr);
        if (action == ArchiveAction::Keep) {
            if (set.flat) {
                for (const auto& v : set.volumes) top.push_back(v.second);
            }
            continue;
        }
        if (action == ArchiveAction::ExtractToFolder && set.flat) gameDirName = plan.release_folder;
        fs::path folder = set.flat ? base / plan.release_folder : first.parent_path();
        ExtractTarget target = makeExtractTarget(c, action, base.string(), folder.string(), entries, naming);
        if (extractWith(first, target.dest_dir, target.transform, cb, cancel)) {
            for (const auto& v : set.volumes) fs::remove(v.second, ec);
            for (const auto& placed : *target.placed_top) top.push_back(placed);
        } else {
            failedArchives.insert(first.string());
            if (set.flat) {
                for (const auto& v : set.volumes) top.push_back(v.second);
            }
        }
    }

    fs::path gameDir = gameDirName.empty() ? fs::path() : base / gameDirName;
    bool haveGameDir = !gameDir.empty() && fs::exists(gameDir, ec);

    // 2. Archives inside archives (one more level).
    if (haveGameDir && c.route != RetroRoute::Vita3K && !cancelled()) {
        for (const auto& f : listFilesRecursive(gameDir)) {
            if (cancelled()) break;
            if (isArchiveName(c, f.filename().string()) && !failedArchives.count(f.string())) {
                std::vector<std::string> inner;
                listArchiveFiles(f, inner);
                ExtractTarget target = makeExtractTarget(c, ArchiveAction::ExtractToFolder, base.string(),
                                                         f.parent_path().string(), inner, naming);
                extractWith(f, target.dest_dir, target.transform, cb, cancel);
            }
        }
    }

    if (haveGameDir && !cancelled()) {
        if (c.route != RetroRoute::Vita3K) {
            // Notes and scans go, unless the release has nothing else (then it is kept as is).
            std::vector<fs::path> files = listFilesRecursive(gameDir);
            bool hasContent = std::any_of(files.begin(), files.end(), [](const fs::path& f) {
                return !isJunkExt(extOf(f.filename().string()));
            });
            if (hasContent) {
                for (const auto& f : files) {
                    if (isJunkExt(extOf(f.filename().string()))) fs::remove(f, ec);
                }
            }
        }
        if (c.route == RetroRoute::Tico) {
            convertDiscImages(c, gameDir);
            fixCueSheets(gameDir, naming);
        }

        // A folder holding one self-contained file is not needed.
        if (c.route != RetroRoute::Vita3K) {
            std::vector<fs::path> files = listFilesRecursive(gameDir);
            if (files.size() == 1) {
                std::string ext = extOf(files[0].filename().string());
                if (isLaunchExt(c, ext) && !isSheetExt(ext)) {
                    fs::path dst = uniquePath(base, files[0].filename().string());
                    if (moveFile(files[0], dst)) {
                        fs::remove_all(gameDir, ec);
                        top.push_back(dst);
                        haveGameDir = false;
                    }
                }
            }
        }
    }

    if (haveGameDir && !cancelled() && c.route == RetroRoute::Tico) {
        if (c.supports_m3u) {
            if (cb.on_stage) cb.on_stage("app/retro/stage_m3u");
            std::vector<fs::path> playlists = writePlaylists(c, base, gameDir);
            top.insert(top.end(), playlists.begin(), playlists.end());
        } else {
            // No playlists here (PS2, 3DS): move the games up where Tico sees them.
            for (const auto& f : listFilesRecursive(gameDir)) {
                if (!isLaunchExt(c, extOf(f.filename().string()))) continue;
                fs::path dst = uniquePath(base, f.filename().string());
                if (moveFile(f, dst)) top.push_back(dst);
            }
            if (listFilesRecursive(gameDir).empty()) fs::remove_all(gameDir, ec);
        }
    }

    if (c.route == RetroRoute::Vita3K && !cancelled()) {
        std::vector<fs::path> candidates = top;
        if (haveGameDir) candidates.push_back(gameDir);
        VitaInstallSummary vita = installIntoVita3K(candidates, cancel);
        res.vita_installed = vita.installed;
        res.vita_staged = vita.staged;
        res.written.insert(res.written.end(), vita.installed_dirs.begin(), vita.installed_dirs.end());
        top.clear();
        for (const auto& left : vita.left_paths) top.push_back(left);
        haveGameDir = haveGameDir && fs::exists(gameDir, ec);
    }

    for (const auto& t : top) {
        res.launch_entries.push_back(t.string());
        res.written.push_back(t.string());
    }
    if (haveGameDir && fs::exists(gameDir, ec)) res.written.push_back(gameDir.string());

    if (c.route == RetroRoute::Tico) {
        bool playable = false;
        for (const auto& t : top) {
            std::string ext = extOf(t.filename().string());
            if (ext == ".m3u" || isLaunchExt(c, ext) || (c.keep_archives && util::isArchiveFile(t.string()))) {
                playable = true;
                break;
            }
        }
        res.unsupported_format = !playable;
        if (res.unsupported_format) {
            util::logLine("retro: no file of " + c.id + " format found in " + plan.base_dir + "/" + gameDirName);
        }
    }
    return res;
}

// =================================================================== covers

bool saveTicoCover(const std::string& tico_slug, const std::string& name, const std::string& cover_url) {
    if (tico_slug.empty() || name.empty() || cover_url.empty()) return false;
    fs::path out = fs::path(TSNX_TICO_COVERS_DIR) / tico_slug / (name + ".jpg");
    std::error_code ec;
    if (fs::exists(out, ec)) return true;

    net::HttpClient client;
    client.setTimeout(15);
    net::HttpResponse r = client.httpGet(cover_url);
    if (r.status_code != 200 || r.body.empty()) {
        util::logLine("retro: cover download failed (" + std::to_string(r.status_code) + ") " + cover_url);
        return false;
    }
    if (!encodeSquareCover(r.body, out)) {
        util::logLine("retro: cover could not be decoded: " + cover_url);
        return false;
    }
    util::logLine("retro: cover saved " + out.string());
    return true;
}

// ================================================================ migration

void migrateLegacyTicoLayout() {
    static const char* kMarker = "tico_layout_v2.done";
    if (hasMarker(kMarker)) return;

    // Older versions used these folder names; Tico reads the new ones.
    static const std::pair<const char*, const char*> renames[] = {
        {"megadrive", "genesis"},
        {"sms", "master-system"},
        {"gg", "game-gear"},
        {"segacd", "sega-cd"},
        {"32x", "sega-32x"},
    };
    int moved = 0;
    for (const auto& r : renames) {
        moved += moveDirContents(fs::path(TSNX_TICO_ROMS_DIR) / r.first, fs::path(TSNX_TICO_ROMS_DIR) / r.second);
        moved += moveDirContents(fs::path(TSNX_TICO_COVERS_DIR) / r.first, fs::path(TSNX_TICO_COVERS_DIR) / r.second);
    }

    // Tico reads .jpg covers only; older versions saved .png.
    int converted = 0;
    std::error_code ec;
    if (fs::exists(TSNX_TICO_COVERS_DIR, ec)) {
        for (const auto& f : listFilesRecursive(TSNX_TICO_COVERS_DIR)) {
            if (extOf(f.filename().string()) != ".png") continue;
            fs::path jpg = f;
            jpg.replace_extension(".jpg");
            std::string data;
            if (!fs::exists(jpg, ec) && readFile(f, data) && encodeSquareCover(data, jpg)) {
                fs::remove(f, ec);
                ++converted;
            }
        }
    }

    util::logLine("retro: layout migration moved " + std::to_string(moved) + " item(s), converted " +
                  std::to_string(converted) + " cover(s)");
    setMarker(kMarker);
}

LegacyStandaloneGames findLegacyStandaloneGames() {
    LegacyStandaloneGames g;
    for (const auto& d : legacyDirs("psvita")) g.vita += countEntries(d);
    for (const auto& d : legacyDirs("wiiu")) g.wiiu += countEntries(d);
    return g;
}

int moveLegacyStandaloneGames() {
    int moved = 0;
    for (const auto& d : legacyDirs("psvita")) moved += moveDirContents(d, TSNX_VITA3K_INSTALL_DIR);
    for (const auto& d : legacyDirs("wiiu")) moved += moveDirContents(d, TSNX_CEMU_GAMES_DIR);
    util::logLine("retro: moved " + std::to_string(moved) + " PS Vita / Wii U item(s) to emulator folders");
    return moved;
}

bool wasLegacyStandaloneOfferAnswered() {
    return hasMarker("legacy_standalone_offer.done");
}

void markLegacyStandaloneOfferAnswered() {
    setMarker("legacy_standalone_offer.done");
}

} // namespace retro
} // namespace catalog
